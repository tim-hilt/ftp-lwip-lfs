/**
 * @file ftp_server.c
 * @brief FTP server implementation using LwIP raw TCP API and LittleFS.
 *
 * Supports: USER, PASS, SYST, FEAT, TYPE, PWD, CWD, CDUP, PASV, PORT,
 *           LIST, NLST, RETR, STOR, DELE, MKD, RMD, RNFR, RNTO, SIZE,
 *           NOOP, QUIT, ABOR.
 *
 * Data transfers use passive mode (PASV) or active mode (PORT).
 * Only one data connection per session at a time.
 *
 * lwIP callback contract
 * ----------------------
 * If a raw-API callback aborts its own PCB (directly, or indirectly via a
 * failed tcp_close()), it must return ERR_ABRT so lwIP stops touching the
 * freed PCB. Every close path here records whether an abort happened in
 * ftp_session_t::ctrl_aborted / ::data_aborted, and each callback turns that
 * into its return value.
 */

#include "ftp_server.h"

#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"

#include "lfs.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  Internal types                                                    */
/* ------------------------------------------------------------------ */

/** Transfer direction for the current data connection. */
typedef enum {
    FTP_DATA_IDLE,
    FTP_DATA_LIST,
    FTP_DATA_NLST,
    FTP_DATA_RETR,
    FTP_DATA_STOR,
} ftp_data_mode_t;

/** Session state machine. WAIT_USER is 0, so a zeroed session is logged out. */
typedef enum {
    FTP_STATE_WAIT_USER,     /**< sent greeting, expect USER */
    FTP_STATE_WAIT_PASS,     /**< got USER, expect PASS */
    FTP_STATE_LOGGED_IN,     /**< authenticated, accept commands */
} ftp_auth_state_t;

/** Negative return codes from ftp_fill_next_chunk(). */
enum {
    FTP_CHUNK_EOF = -1,      /**< no more data — transfer completed normally */
    FTP_CHUNK_ERR = -2,      /**< filesystem or formatting error */
};

/** Per-client session. */
typedef struct ftp_session {
    /* Control connection */
    struct tcp_pcb      *ctrl_pcb;

    /* Passive mode listener */
    struct tcp_pcb      *pasv_listen_pcb;

    /* Connected data socket (accepted from PASV or connected via PORT) */
    struct tcp_pcb      *data_pcb;

    /* File cache for lfs_file_opencfg (avoids heap allocation) */
    struct lfs_file_config file_cfg;

    /* File / directory being transferred */
    lfs_dir_t             dir;
    lfs_file_t            file;

    /* Authentication */
    ftp_auth_state_t      auth;

    /* Data connection ------------------------------------------------ */
    ftp_data_mode_t       data_mode;

    /* Active mode target */
    ip_addr_t             port_addr;

    /* RETR: size reported in the "150" reply */
    lfs_soff_t            retr_size;

    uint16_t              cmd_len;
    uint16_t              port_port;

    /* Remaining bytes to flush for current chunk (RETR/LIST) */
    uint16_t              data_pending;
    uint16_t              data_offset;

    /* Idle timeout poll counter */
    uint16_t              idle_polls;

    uint8_t               in_use;

    uint8_t               port_active; /**< 1 = use active mode */
    uint8_t               file_open;
    uint8_t               dir_open;

    /* Active mode: data_pcb is set but tcp_connect() has not completed yet,
     * so it must not be treated as a usable data connection. */
    uint8_t               data_connecting;

    /* Resynchronising after an over-long command line: everything up to
     * and including the next '\n' belongs to the discarded line. */
    uint8_t               discard_line;

    /* Set when a close path had to abort the PCB; the lwIP callback that
     * is running must then return ERR_ABRT instead of ERR_OK. */
    uint8_t               ctrl_aborted;
    uint8_t               data_aborted;

    char                  cmd_buf[FTP_SERVER_CMD_BUF_SIZE];

    /* Working directory: absolute and normalised, with no trailing '/'
     * except at the root, which is "/". */
    char                  cwd[FTP_SERVER_PATH_MAX];

    /* Pending RNFR path (set by RNFR, consumed by RNTO) */
    char                  rnfr[FTP_SERVER_PATH_MAX];

    uint8_t               file_cache[FTP_SERVER_FILE_CACHE_SIZE];

    /* Transfer buffer */
    uint8_t               data_buf[FTP_SERVER_DATA_BUF_SIZE];
} ftp_session_t;

/* ------------------------------------------------------------------ */
/*  Module state                                                      */
/* ------------------------------------------------------------------ */

static lfs_t             *s_lfs;
static struct tcp_pcb    *s_listen_pcb;
static ftp_session_t      s_sessions[FTP_SERVER_MAX_CLIENTS];
static uint16_t           s_next_pasv_port = FTP_SERVER_PASV_PORT_MIN;

/* Scratch for the handful of replies that have to be formatted. lwIP's raw
 * API runs every callback on one thread, so a single shared buffer is safe
 * and keeps FTP_SERVER_PATH_MAX-sized frames off the (scarce) MCU stack.
 * Only valid until the ftp_send_reply() that consumes it returns. */
static char               s_reply[FTP_SERVER_PATH_MAX + 64];

/* Replies reused by several handlers — one copy in .rodata each. */
static const char ftp_reply_need_file[] = "501 Specify file name.\r\n";
static const char ftp_reply_need_dir[]  = "501 Specify directory name.\r\n";

/* ------------------------------------------------------------------ */
/*  Forward declarations                                              */
/* ------------------------------------------------------------------ */

static err_t  ftp_ctrl_accept(void *arg, struct tcp_pcb *pcb, err_t err);
static err_t  ftp_ctrl_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static void   ftp_ctrl_err(void *arg, err_t err);
static err_t  ftp_ctrl_poll(void *arg, struct tcp_pcb *pcb);

static err_t  ftp_data_accept(void *arg, struct tcp_pcb *pcb, err_t err);
static err_t  ftp_data_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static err_t  ftp_data_sent(void *arg, struct tcp_pcb *pcb, u16_t len);
static void   ftp_data_err(void *arg, err_t err);

static void   ftp_process_command(ftp_session_t *s);
static void   ftp_send_reply(ftp_session_t *s, const char *msg);
static void   ftp_close_data(ftp_session_t *s);
static void   ftp_close_session(ftp_session_t *s);
static void   ftp_start_transfer(ftp_session_t *s);
static void   ftp_send_next_data(ftp_session_t *s);

/* ------------------------------------------------------------------ */
/*  Small helpers                                                     */
/* ------------------------------------------------------------------ */

/** Bounded copy that always NUL-terminates. @p size must be non-zero. */
static void ftp_strlcpy(char *dst, const char *src, size_t size)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/**
 * Detach every callback from *ppcb, close it, and NULL the caller's pointer.
 *
 * @p listener selects which callback setters are legal: lwIP asserts on
 * tcp_recv/tcp_sent/tcp_err/tcp_poll for a PCB in the LISTEN state, and
 * tcp_accept is a no-op for anything else.
 *
 * @return 1 if tcp_close() failed and the PCB had to be aborted.
 */
static int ftp_close_pcb(struct tcp_pcb **ppcb, int listener)
{
    struct tcp_pcb *pcb = *ppcb;
    if (!pcb) return 0;
    *ppcb = NULL;

    tcp_arg(pcb, NULL);
    if (listener) {
        tcp_accept(pcb, NULL);
    } else {
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
    }

    if (tcp_close(pcb) == ERR_OK) return 0;
    /* tcp_close() cannot fail on a LISTEN pcb (it just unlinks and frees it),
     * and tcp_abort() asserts "don't call tcp_abort for listen-pcbs" — so the
     * fallback below is for connected PCBs only. */
    if (listener) return 0;
    tcp_abort(pcb);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Helper: path resolution                                           */
/* ------------------------------------------------------------------ */

/**
 * Build an absolute path from the session CWD and a user-supplied argument.
 * Result is written to @p out (size FTP_SERVER_PATH_MAX).
 * Returns 0 on success, -1 on overflow.
 */
static int ftp_resolve_path(const ftp_session_t *s, const char *arg, char *out)
{
    if (!arg || arg[0] == '\0') {
        ftp_strlcpy(out, s->cwd, FTP_SERVER_PATH_MAX);
        return 0;
    }

    char tmp[FTP_SERVER_PATH_MAX];
    if (arg[0] == '/') {
        /* Absolute path. Truncating here would silently address a *different*
         * existing path (a prefix of the requested one), so reject instead. */
        if (strlen(arg) >= sizeof(tmp)) return -1;
        ftp_strlcpy(tmp, arg, sizeof(tmp));
    } else {
        /* Relative to cwd */
        int n = snprintf(tmp, sizeof(tmp), "%s%s%s",
                         s->cwd,
                         (s->cwd[strlen(s->cwd) - 1] == '/') ? "" : "/",
                         arg);
        if (n < 0 || (size_t)n >= sizeof(tmp))
            return -1;
    }

    /* Normalise: resolve "." and ".." components, collapse "//" */
    const char *src = tmp;
    char *dst = out;
    char *end = out + FTP_SERVER_PATH_MAX - 1;

    *dst++ = '/';
    while (*src) {
        /* skip slashes */
        while (*src == '/') src++;
        if (*src == '\0') break;

        /* find component end */
        const char *comp = src;
        while (*src && *src != '/') src++;
        size_t len = (size_t)(src - comp);

        if (len == 1 && comp[0] == '.') {
            /* skip "." */
            continue;
        }
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            /* go up: remove last component */
            if (dst > out + 1) {
                dst--;           /* back over trailing '/' */
                while (dst > out + 1 && *(dst - 1) != '/') dst--;
            }
            continue;
        }

        /* copy component */
        if (dst + len + 1 > end) return -1;
        memcpy(dst, comp, len);
        dst += len;
        *dst++ = '/';
    }

    /* remove trailing '/' unless root */
    if (dst > out + 1 && *(dst - 1) == '/') dst--;
    *dst = '\0';

    return 0;
}

/**
 * Resolve a command argument into @p out (size FTP_SERVER_PATH_MAX),
 * replying on failure so the caller can simply return.
 *
 * @param missing Reply sent when @p arg is NULL, or NULL to treat a missing
 *                argument as "the current directory".
 * @return 0 on success, -1 after a reply has been sent.
 */
static int ftp_arg_path(ftp_session_t *s, const char *arg, char *out,
                        const char *missing)
{
    if (!arg && missing) {
        ftp_send_reply(s, missing);
        return -1;
    }
    if (ftp_resolve_path(s, arg, out) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Allocate / find session                                           */
/* ------------------------------------------------------------------ */

static ftp_session_t *ftp_alloc_session(void)
{
    for (int i = 0; i < FTP_SERVER_MAX_CLIENTS; i++) {
        if (!s_sessions[i].in_use) {
            ftp_session_t *s = &s_sessions[i];
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            /* The rest of file_cfg stays zeroed for the session's lifetime;
             * only the no-heap cache buffer has to be pointed at. */
            s->file_cfg.buffer = s->file_cache;
            strcpy(s->cwd, "/");
            return s;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Next PASV port (round-robin)                                      */
/* ------------------------------------------------------------------ */

static uint16_t ftp_next_pasv_port(void)
{
    uint16_t p = s_next_pasv_port;
    s_next_pasv_port++;
    if (s_next_pasv_port > FTP_SERVER_PASV_PORT_MAX)
        s_next_pasv_port = FTP_SERVER_PASV_PORT_MIN;
    return p;
}

/* ------------------------------------------------------------------ */
/*  Send a reply on the control connection                            */
/* ------------------------------------------------------------------ */

static void ftp_send_reply(ftp_session_t *s, const char *msg)
{
    if (!s->ctrl_pcb) return;
    uint16_t len = (uint16_t)strlen(msg);
    if (tcp_sndbuf(s->ctrl_pcb) < len ||
        tcp_write(s->ctrl_pcb, msg, len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        /* Cannot deliver reply — tear down the session. */
        ftp_close_session(s);
        return;
    }
    tcp_output(s->ctrl_pcb);
}

/* ------------------------------------------------------------------ */
/*  Close helpers                                                     */
/* ------------------------------------------------------------------ */

static void ftp_close_data(ftp_session_t *s)
{
    if (ftp_close_pcb(&s->data_pcb, 0)) s->data_aborted = 1;
    (void)ftp_close_pcb(&s->pasv_listen_pcb, 1);
    s->data_connecting = 0;

    if (s->file_open) {
        lfs_file_close(s_lfs, &s->file);
        s->file_open = 0;
    }
    if (s->dir_open) {
        lfs_dir_close(s_lfs, &s->dir);
        s->dir_open = 0;
    }
    s->data_mode    = FTP_DATA_IDLE;
    s->data_pending = 0;
    s->data_offset  = 0;
    /* RFC 959: PORT/PASV should be re-issued per transfer. Resetting here
     * is intentionally strict; some clients assume the last PORT persists.
     * If relaxed, also clear port_active in the PASV handler (already done)
     * to avoid stale PORT state when switching between active/passive mode. */
    s->port_active  = 0;
}

static void ftp_close_session(ftp_session_t *s)
{
    ftp_close_data(s);
    if (ftp_close_pcb(&s->ctrl_pcb, 0)) s->ctrl_aborted = 1;
    s->rnfr[0] = '\0';
    s->in_use = 0;
}

/**
 * Reject a transfer command while another transfer is still set up.
 *
 * Re-opening s->file / s->dir without closing them first would add the same
 * node to littlefs's intrusive list of open handles twice, making it point
 * at itself and corrupting every later traversal. PASV and ABOR reset the
 * state, so a client always has a way out.
 *
 * @return 1 if busy (a reply has been sent), 0 if the caller may proceed.
 */
static int ftp_transfer_busy(ftp_session_t *s)
{
    if (s->data_mode == FTP_DATA_IDLE && !s->file_open && !s->dir_open)
        return 0;
    ftp_send_reply(s, "450 Transfer already in progress.\r\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Data transfer: send file or listing data                          */
/* ------------------------------------------------------------------ */

/**
 * Attempt to flush all pending bytes in data_buf[data_offset..) to the
 * data PCB.
 *
 * @return 1 when fully flushed (data_pending == 0);
 *         0 when blocked on send buffer space — the caller must return and
 *           wait for the next tcp_sent or poll callback;
 *        -1 when the connection cannot carry the data at all.
 */
static int ftp_send_pending(ftp_session_t *s)
{
    while (s->data_pending > 0) {
        uint16_t space = tcp_sndbuf(s->data_pcb);
        if (space == 0) return 0; /* wait for sent callback */
        uint16_t to_send = (s->data_pending < space) ? s->data_pending : space;
        err_t err = tcp_write(s->data_pcb, s->data_buf + s->data_offset,
                              to_send, TCP_WRITE_FLAG_COPY);
        /* ERR_MEM is transient back-pressure and the sent callback will retry;
         * anything else (ERR_CONN, ERR_ARG, ...) never resolves on its own, and
         * treating it as back-pressure would hang the transfer until the idle
         * timer fires. */
        if (err == ERR_MEM) return 0;
        if (err != ERR_OK)  return -1;
        tcp_output(s->data_pcb);
        s->data_offset  += to_send;
        s->data_pending -= to_send;
    }
    return 1;
}

/**
 * Fill data_buf with the next chunk for the session's current transfer
 * mode (RETR file data, or LIST/NLST directory entries — "." and ".."
 * are skipped). Returns the number of bytes filled, FTP_CHUNK_EOF when the
 * transfer is exhausted, or FTP_CHUNK_ERR on a filesystem error.
 */
static int ftp_fill_next_chunk(ftp_session_t *s)
{
    if (s->data_mode == FTP_DATA_RETR) {
        lfs_ssize_t r = lfs_file_read(s_lfs, &s->file,
                                       s->data_buf, FTP_SERVER_DATA_BUF_SIZE);
        if (r < 0) return FTP_CHUNK_ERR;
        return (r == 0) ? FTP_CHUNK_EOF : (int)r;
    }

    /* Defensive: ftp_send_next_data() only runs for RETR/LIST/NLST, so this
     * is unreachable unless a future mode forgets to wire up its own filler. */
    if (s->data_mode != FTP_DATA_LIST && s->data_mode != FTP_DATA_NLST) {
        return FTP_CHUNK_ERR; /* GCOVR_EXCL_LINE */
    }

    for (;;) {
        struct lfs_info info;
        int r = lfs_dir_read(s_lfs, &s->dir, &info);
        if (r < 0) return FTP_CHUNK_ERR;
        if (r == 0) return FTP_CHUNK_EOF;

        /* Skip . and .. */
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        int n;
        if (s->data_mode == FTP_DATA_NLST) {
            n = snprintf((char *)s->data_buf, FTP_SERVER_DATA_BUF_SIZE,
                         "%s\r\n", info.name);
        } else {
            /* LIST: emit a Unix-style listing */
            const char *type_str = (info.type == LFS_TYPE_DIR)
                ? "drwxr-xr-x" : "-rw-r--r--";
            n = snprintf((char *)s->data_buf, FTP_SERVER_DATA_BUF_SIZE,
                         "%s 1 ftp ftp %10lu Jan 01  2000 %s\r\n",
                         type_str,
                         (unsigned long)info.size,
                         info.name);
        }
        if (n < 0) return FTP_CHUNK_ERR;
        /* snprintf returns the length it *would* have written; on truncation
         * only FTP_SERVER_DATA_BUF_SIZE - 1 bytes are real, the last slot
         * holding the NUL terminator. */
        if (n >= (int)FTP_SERVER_DATA_BUF_SIZE) n = (int)FTP_SERVER_DATA_BUF_SIZE - 1;
        return n;
    }
}

/**
 * Fill data_buf with the next chunk and write it to the data PCB.
 * Called after the data connection is established and after each
 * tcp_sent callback confirms the previous chunk was ACK'd.
 */
static void ftp_send_next_data(ftp_session_t *s)
{
    if (!s->data_pcb) return;

    for (;;) {
        /* Flush what is already buffered — leftovers from a previous fill on
         * the first pass, the chunk just filled on later ones. */
        int flushed = ftp_send_pending(s);
        if (flushed == 0) return;   /* wait for tcp_sent */
        if (flushed < 0) {
            ftp_close_data(s);
            ftp_send_reply(s, "426 Connection closed; transfer aborted.\r\n");
            return;
        }

        int n = ftp_fill_next_chunk(s);
        if (n < 0) {
            ftp_close_data(s);
            ftp_send_reply(s, (n == FTP_CHUNK_ERR)
                ? "451 Requested action aborted: local error in processing.\r\n"
                : "226 Transfer complete.\r\n");
            return;
        }

        s->data_offset  = 0;
        s->data_pending = (uint16_t)n;
    }
}

/* ------------------------------------------------------------------ */
/*  Data connection: initiate active-mode connect                     */
/* ------------------------------------------------------------------ */

static err_t ftp_data_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;

    /* The session may have moved on while the connect was in flight — closed
     * by QUIT, the idle timer or an error, its slot possibly already handed to
     * a different client, or the transfer cancelled by ABOR/PASV. Only a
     * session still waiting on this connect may be touched; anything else gets
     * the stray connection closed and nothing else. (lwIP reports a failed
     * connect with the PCB already freed, so it may pass NULL here — then
     * data_connecting is all there is to match on.) */
    int mine = s && s->in_use && s->data_connecting &&
               (pcb == NULL || s->data_pcb == pcb);
    if (!mine) {
        /* lwIP only invokes this callback on a successful connect; a non-OK
         * err means the PCB has already been freed and must not be touched. */
        if (err != ERR_OK || !pcb) return ERR_OK;
        struct tcp_pcb *stale = pcb;
        return ftp_close_pcb(&stale, 0) ? ERR_ABRT : ERR_OK;
    }

    s->data_connecting = 0;

    if (err != ERR_OK) {
        s->data_pcb = NULL; /* already freed by lwIP */
        ftp_send_reply(s, "425 Can't open data connection.\r\n");
        ftp_close_data(s);
        return ERR_OK;
    }

    s->data_aborted = 0;
    ftp_start_transfer(s);
    return s->data_aborted ? ERR_ABRT : ERR_OK;
}

/**
 * Called once the data connection is ready (passive accept or active connect).
 * Begins the actual data transfer.
 */
static void ftp_start_transfer(ftp_session_t *s)
{
    if (!s->data_pcb) return;

    if (s->data_mode == FTP_DATA_STOR) {
        ftp_send_reply(s, "150 Ok to send data.\r\n");
    } else if (s->data_mode == FTP_DATA_RETR) {
        /* Only binary transfers are implemented (TYPE rejects anything else),
         * so the mode is a constant. Clients that care parse the byte count. */
        (void)snprintf(s_reply, sizeof(s_reply),
                 "150 Opening BINARY mode data connection (%ld bytes).\r\n",
                 (long)s->retr_size);
        ftp_send_reply(s, s_reply);
        ftp_send_next_data(s);
    } else {
        ftp_send_reply(s, "150 Here comes the data.\r\n");
        ftp_send_next_data(s);
    }
}

/**
 * Open the data connection for the transfer the caller has just set up, and
 * start it. Three cases:
 *   - a PASV socket the client already connected to  -> start immediately;
 *   - an active-mode PORT target -> connect, ftp_data_connected starts it;
 *   - a PASV listener still waiting -> ftp_data_accept starts it.
 * On failure the error reply is sent and the pending transfer is torn down.
 */
static void ftp_begin_transfer(ftp_session_t *s)
{
    if (s->data_pcb && !s->data_connecting) {
        /* Already connected (PASV client connected early). */
        ftp_start_transfer(s);
        return;
    }

    if (s->port_active) {
        /* Active mode: connect to client. */
        struct tcp_pcb *pcb = tcp_new();
        if (pcb) {
            tcp_arg(pcb, s);
            tcp_err(pcb, ftp_data_err);
            tcp_recv(pcb, ftp_data_recv);
            tcp_sent(pcb, ftp_data_sent);

            /* Publish the PCB *before* connecting: until the connect completes
             * it is still ours to close, and a teardown in between must not
             * leave lwIP holding callbacks into a recycled session. */
            s->data_pcb        = pcb;
            s->data_connecting = 1;
            if (tcp_connect(pcb, &s->port_addr, s->port_port,
                            ftp_data_connected) == ERR_OK) {
                /* ftp_data_connected() will call ftp_start_transfer(). */
                return;
            }
            s->data_pcb        = NULL;
            s->data_connecting = 0;
            tcp_abort(pcb);
        }
        ftp_send_reply(s, "425 Can't open data connection.\r\n");
        ftp_close_data(s);
        return;
    }

    if (s->pasv_listen_pcb) {
        /* Passive mode listener exists but client hasn't connected yet.
         * The data_accept callback will trigger start_transfer. */
        return;
    }

    ftp_send_reply(s, "425 Use PASV or PORT first.\r\n");
    ftp_close_data(s);
}

/* ------------------------------------------------------------------ */
/*  Data connection callbacks                                         */
/* ------------------------------------------------------------------ */

static err_t ftp_data_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    if (err != ERR_OK || !s) {
        if (!pcb) return ERR_VAL;
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    /* Tell lwIP the backlog slot is free again; without this a build with
     * TCP_LISTEN_BACKLOG enabled stops accepting after `backlog` connections. */
    tcp_accepted(s->pasv_listen_pcb);

    s->data_aborted = 0;
    s->data_pcb = pcb;
    tcp_arg(pcb, s);
    tcp_recv(pcb, ftp_data_recv);
    tcp_sent(pcb, ftp_data_sent);
    tcp_err(pcb, ftp_data_err);

    /* Close the listener — only one data connection per transfer. */
    (void)ftp_close_pcb(&s->pasv_listen_pcb, 1);

    /* If a transfer is pending (command was already issued), start it. */
    if (s->data_mode != FTP_DATA_IDLE) {
        ftp_start_transfer(s);
    }

    return s->data_aborted ? ERR_ABRT : ERR_OK;
}

static err_t ftp_data_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)err;
    ftp_session_t *s = (ftp_session_t *)arg;
    if (!s) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    s->data_aborted = 0;
    /* Data-channel progress keeps the session alive; see ftp_ctrl_poll(). */
    s->idle_polls = 0;

    if (!p) {
        /* Client closed the data connection. For an upload that is how the
         * transfer ends; for a download it means the client gave up partway,
         * and answering 226 would report a truncated file as complete. */
        ftp_data_mode_t mode = s->data_mode;
        ftp_close_data(s);
        if (mode == FTP_DATA_STOR) {
            ftp_send_reply(s, "226 Transfer complete.\r\n");
        } else if (mode != FTP_DATA_IDLE) {
            ftp_send_reply(s, "426 Connection closed; transfer aborted.\r\n");
        }
        return s->data_aborted ? ERR_ABRT : ERR_OK;
    }

    if (s->data_mode == FTP_DATA_STOR && s->file_open) {
        /* Write received data to LittleFS file. */
        struct pbuf *q;
        for (q = p; q != NULL; q = q->next) {
            lfs_ssize_t w = lfs_file_write(s_lfs, &s->file,
                                           q->payload, q->len);
            /* A short write means the volume filled up mid-pbuf; accepting it
             * would silently truncate the uploaded file. */
            if (w != (lfs_ssize_t)q->len) {
                tcp_recved(pcb, p->tot_len);
                pbuf_free(p);
                ftp_close_data(s);
                ftp_send_reply(s, "452 Insufficient storage space.\r\n");
                return s->data_aborted ? ERR_ABRT : ERR_OK;
            }
        }
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t ftp_data_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    (void)pcb;
    (void)len;
    if (!s) return ERR_OK;
    s->data_aborted = 0;
    s->idle_polls   = 0; /* transfer is making progress */

    /* Previous chunk ACK'd — send more data. */
    if (s->data_mode == FTP_DATA_RETR ||
        s->data_mode == FTP_DATA_LIST ||
        s->data_mode == FTP_DATA_NLST) {
        ftp_send_next_data(s);
    }

    return s->data_aborted ? ERR_ABRT : ERR_OK;
}

static void ftp_data_err(void *arg, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    (void)err;
    if (!s) return;

    /* PCB is already freed by lwIP before the error callback fires. */
    s->data_pcb = NULL;
    int was_transferring = (s->data_mode != FTP_DATA_IDLE);
    ftp_close_data(s);   /* handles pasv_listen, file, dir, all flags */

    /* Without this the client waits for a 226 that will never arrive. */
    if (was_transferring) {
        ftp_send_reply(s, "426 Connection closed; transfer aborted.\r\n");
    }
}

/* ------------------------------------------------------------------ */
/*  Command processing                                                */
/* ------------------------------------------------------------------ */

/** Signature for a command handler. */
typedef void (*ftp_cmd_fn)(ftp_session_t *s, const char *arg);

/** One entry in the command dispatch table. */
struct ftp_cmd {
    /* Inline rather than a pointer: every FTP verb fits, and it saves a
     * pointer plus a relocation per entry in the image. */
    char       name[5];
    uint8_t    pre_auth   : 1;  /**< accepted before login */
    uint8_t    keeps_rnfr : 1;  /**< does not cancel a pending RNFR */
    ftp_cmd_fn handler;
};

static void cmd_syst(ftp_session_t *s, const char *arg)
{
    (void)arg;
    ftp_send_reply(s, "215 UNIX Type: L8\r\n");
}

static void cmd_feat(ftp_session_t *s, const char *arg)
{
    (void)arg;
    ftp_send_reply(s, "211-Features:\r\n"
                      " PASV\r\n"
                      " SIZE\r\n"
                      " UTF8\r\n"
                      "211 End\r\n");
}

static void cmd_opts(ftp_session_t *s, const char *arg)
{
    char upper[FTP_SERVER_CMD_BUF_SIZE];
    if (arg) {
        ftp_strlcpy(upper, arg, sizeof(upper));
        for (char *p = upper; *p; p++) *p = (char)toupper((unsigned char)*p);
        arg = upper;
    }
    if (arg && strcmp(arg, "UTF8 ON") == 0) {
        ftp_send_reply(s, "200 UTF8 set to on.\r\n");
    } else {
        ftp_send_reply(s, "501 Option not understood.\r\n");
    }
}

static void cmd_noop(ftp_session_t *s, const char *arg)
{
    (void)arg;
    ftp_send_reply(s, "200 NOOP ok.\r\n");
}

static void cmd_type(ftp_session_t *s, const char *arg)
{
    if (arg && (arg[0] == 'I' || arg[0] == 'i')) {
        ftp_send_reply(s, "200 Switching to Binary mode.\r\n");
    } else {
        ftp_send_reply(s, "504 ASCII transfer mode not supported, use binary.\r\n");
    }
}

static void cmd_pwd(ftp_session_t *s, const char *arg)
{
    (void)arg;
    (void)snprintf(s_reply, sizeof(s_reply),
             "257 \"%s\" is the current directory.\r\n", s->cwd);
    ftp_send_reply(s, s_reply);
}

/** Adopt @p path as the session CWD and confirm it. */
static void ftp_set_cwd(ftp_session_t *s, const char *path)
{
    ftp_strlcpy(s->cwd, path, sizeof(s->cwd));
    ftp_send_reply(s, "250 Directory successfully changed.\r\n");
}

static void cmd_cwd(ftp_session_t *s, const char *arg)
{
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_arg_path(s, arg, path, NULL) < 0) return;

    struct lfs_info info;
    if (lfs_stat(s_lfs, path, &info) < 0 || info.type != LFS_TYPE_DIR) {
        ftp_send_reply(s, "550 Failed to change directory.\r\n");
        return;
    }
    ftp_set_cwd(s, path);
}

static void cmd_cdup(ftp_session_t *s, const char *arg)
{
    (void)arg;
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_arg_path(s, "..", path, NULL) < 0) return;
    ftp_set_cwd(s, path);
}

static void cmd_pasv(ftp_session_t *s, const char *arg)
{
    (void)arg;
    /* Close any previous data channel. */
    ftp_close_data(s);

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        ftp_send_reply(s, "421 Cannot create data socket.\r\n");
        return;
    }

    uint16_t port = 0;
    uint16_t range = FTP_SERVER_PASV_PORT_MAX - FTP_SERVER_PASV_PORT_MIN + 1;
    err_t err = ERR_USE;
    for (uint16_t tries = 0; tries < range; tries++) {
        port = ftp_next_pasv_port();
        err = tcp_bind(pcb, IP_ADDR_ANY, port);
        if (err == ERR_OK) break;
    }
    if (err != ERR_OK) {
        tcp_close(pcb);
        ftp_send_reply(s, "421 Cannot bind data socket.\r\n");
        return;
    }

    /* tcp_listen() consumes `pcb` and returns a fresh listening PCB on
     * success; on failure `pcb` is untouched and still ours to free. */
    struct tcp_pcb *lpcb = tcp_listen(pcb);
    if (!lpcb) {
        tcp_close(pcb);
        ftp_send_reply(s, "421 Cannot listen on data socket.\r\n");
        return;
    }

    s->pasv_listen_pcb = lpcb;
    tcp_arg(lpcb, s);
    tcp_accept(lpcb, ftp_data_accept);

    /* Build reply with the server's IP from the control connection. */
    ip_addr_t local_ip;
    u16_t local_port;
    if (tcp_tcp_get_tcp_addrinfo(s->ctrl_pcb, 1, &local_ip, &local_port) != ERR_OK) {
        /* local_ip is untouched on failure — advertising it would send the
         * client at a garbage address. */
        ftp_close_data(s);
        ftp_send_reply(s, "421 Cannot determine server address.\r\n");
        return;
    }

    uint32_t ip4 = ip_addr_get_ip4_u32(&local_ip);
    uint8_t *ip  = (uint8_t *)&ip4;

    (void)snprintf(s_reply, sizeof(s_reply),
             "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).\r\n",
             ip[0], ip[1], ip[2], ip[3],
             (unsigned)(port >> 8), (unsigned)(port & 0xff));
    ftp_send_reply(s, s_reply);
}

static void cmd_port(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Syntax error in parameters.\r\n");
        return;
    }

    unsigned long fields[6];
    const char *p = arg;
    for (int i = 0; i < 6; i++) {
        char *end = NULL;
        fields[i] = strtoul(p, &end, 10);
        if (end == p || fields[i] > 255) {
            ftp_send_reply(s, "501 Syntax error in parameters.\r\n");
            return;
        }
        char expect = (i < 5) ? ',' : '\0';
        if (*end != expect) {
            ftp_send_reply(s, "501 Syntax error in parameters.\r\n");
            return;
        }
        p = end + 1;
    }

    ip_addr_t addr;
    IP_ADDR4(&addr, (unsigned int)fields[0], (unsigned int)fields[1],
                    (unsigned int)fields[2], (unsigned int)fields[3]);

    /* RFC 2577: refuse to open a data connection to anywhere other than the
     * client itself, so the server cannot be used as an "FTP bounce" proxy. */
    ip_addr_t peer;
    u16_t peer_port;
    if (tcp_tcp_get_tcp_addrinfo(s->ctrl_pcb, 0, &peer, &peer_port) != ERR_OK ||
        ip_addr_get_ip4_u32(&peer) != ip_addr_get_ip4_u32(&addr)) {
        ftp_send_reply(s, "501 PORT address must match the control connection.\r\n");
        return;
    }

    ftp_close_data(s);
    s->port_addr   = addr;
    s->port_port   = (uint16_t)((fields[4] << 8) | fields[5]);
    s->port_active = 1;

    ftp_send_reply(s, "200 PORT command successful.\r\n");
}

static void ftp_do_list(ftp_session_t *s, const char *arg, ftp_data_mode_t mode)
{
    if (ftp_transfer_busy(s)) return;

    char path[FTP_SERVER_PATH_MAX];

    /* Skip ls-style options ("-la", "-l -a", ...); whatever follows is a path. */
    const char *list_arg = arg;
    while (list_arg && list_arg[0] == '-') {
        while (*list_arg && *list_arg != ' ') list_arg++;
        while (*list_arg == ' ') list_arg++;
        if (*list_arg == '\0') list_arg = NULL;
    }

    if (ftp_arg_path(s, list_arg, path, NULL) < 0) return;

    if (lfs_dir_open(s_lfs, &s->dir, path) < 0) {
        ftp_send_reply(s, "550 Failed to open directory.\r\n");
        return;
    }
    s->dir_open  = 1;
    s->data_mode = mode;

    ftp_begin_transfer(s);
}

static void cmd_list(ftp_session_t *s, const char *arg)
{
    ftp_do_list(s, arg, FTP_DATA_LIST);
}

static void cmd_nlst(ftp_session_t *s, const char *arg)
{
    ftp_do_list(s, arg, FTP_DATA_NLST);
}

/**
 * Shared RETR / STOR setup: resolve @p arg, open s->file with the session's
 * static cache and arm @p mode. Replies on failure so the caller can return.
 *
 * @return 0 on success, -1 after a reply has been sent.
 */
static int ftp_open_transfer(ftp_session_t *s, const char *arg,
                             ftp_data_mode_t mode, int flags,
                             const char *fail_msg)
{
    if (ftp_transfer_busy(s)) return -1;

    char path[FTP_SERVER_PATH_MAX];
    if (ftp_arg_path(s, arg, path, ftp_reply_need_file) < 0) return -1;

    if (lfs_file_opencfg(s_lfs, &s->file, path, flags, &s->file_cfg) < 0) {
        ftp_send_reply(s, fail_msg);
        return -1;
    }
    s->file_open = 1;
    s->data_mode = mode;
    return 0;
}

static void cmd_retr(ftp_session_t *s, const char *arg)
{
    if (ftp_open_transfer(s, arg, FTP_DATA_RETR, LFS_O_RDONLY,
                          "550 Failed to open file.\r\n") < 0) return;

    s->retr_size = lfs_file_size(s_lfs, &s->file);
    ftp_begin_transfer(s);
}

static void cmd_stor(ftp_session_t *s, const char *arg)
{
    if (ftp_open_transfer(s, arg, FTP_DATA_STOR,
                          LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                          "550 Failed to create file.\r\n") < 0) return;

    ftp_begin_transfer(s);
}

/**
 * Shared DELE / RMD implementation. littlefs uses lfs_remove() for both, so
 * the type check is what keeps DELE off directories and RMD off files.
 */
static void ftp_do_remove(ftp_session_t *s, const char *arg, uint8_t type,
                          const char *ok_msg, const char *fail_msg)
{
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_arg_path(s, arg, path,
                     (type == LFS_TYPE_DIR) ? ftp_reply_need_dir
                                            : ftp_reply_need_file) < 0) return;

    struct lfs_info info;
    if (lfs_stat(s_lfs, path, &info) < 0 || info.type != type ||
        lfs_remove(s_lfs, path) < 0) {
        ftp_send_reply(s, fail_msg);
        return;
    }
    ftp_send_reply(s, ok_msg);
}

static void cmd_dele(ftp_session_t *s, const char *arg)
{
    ftp_do_remove(s, arg, LFS_TYPE_REG,
                  "250 Delete operation successful.\r\n",
                  "550 Delete operation failed.\r\n");
}

static void cmd_rmd(ftp_session_t *s, const char *arg)
{
    ftp_do_remove(s, arg, LFS_TYPE_DIR,
                  "250 Remove directory successful.\r\n",
                  "550 Remove directory failed.\r\n");
}

static void cmd_mkd(ftp_session_t *s, const char *arg)
{
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_arg_path(s, arg, path, ftp_reply_need_dir) < 0) return;

    if (lfs_mkdir(s_lfs, path) < 0) {
        ftp_send_reply(s, "550 Create directory failed.\r\n");
        return;
    }
    (void)snprintf(s_reply, sizeof(s_reply), "257 \"%s\" created.\r\n", path);
    ftp_send_reply(s, s_reply);
}

static void cmd_rnfr(ftp_session_t *s, const char *arg)
{
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_arg_path(s, arg, path, ftp_reply_need_file) < 0) return;

    /* Verify the source exists. */
    struct lfs_info info;
    if (lfs_stat(s_lfs, path, &info) < 0) {
        ftp_send_reply(s, "550 File not found.\r\n");
        return;
    }
    ftp_strlcpy(s->rnfr, path, sizeof(s->rnfr));
    ftp_send_reply(s, "350 Ready for RNTO.\r\n");
}

static void cmd_rnto(ftp_session_t *s, const char *arg)
{
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_arg_path(s, arg, path, ftp_reply_need_file) < 0) return;

    if (s->rnfr[0] == '\0') {
        ftp_send_reply(s, "503 RNFR required first.\r\n");
        return;
    }
    if (lfs_rename(s_lfs, s->rnfr, path) < 0) {
        ftp_send_reply(s, "550 Rename failed.\r\n");
    } else {
        ftp_send_reply(s, "250 Rename successful.\r\n");
    }
    s->rnfr[0] = '\0';
}

static void cmd_size(ftp_session_t *s, const char *arg)
{
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_arg_path(s, arg, path, ftp_reply_need_file) < 0) return;

    struct lfs_info info;
    if (lfs_stat(s_lfs, path, &info) < 0 || info.type != LFS_TYPE_REG) {
        ftp_send_reply(s, "550 Could not get file size.\r\n");
        return;
    }
    (void)snprintf(s_reply, sizeof(s_reply), "213 %lu\r\n",
             (unsigned long)info.size);
    ftp_send_reply(s, s_reply);
}

static void cmd_abor(ftp_session_t *s, const char *arg)
{
    (void)arg;
    ftp_close_data(s);
    ftp_send_reply(s, "226 ABOR command successful.\r\n");
}

static void cmd_user(ftp_session_t *s, const char *arg)
{
    const char *need_user = FTP_SERVER_USER;
    const char *need_pass = FTP_SERVER_PASS;

    if (need_user != NULL && (!arg || strcmp(arg, need_user) != 0)) {
        ftp_send_reply(s, "530 Login incorrect.\r\n");
        return;
    }
    if (need_user != NULL && need_pass != NULL) {
        s->auth = FTP_STATE_WAIT_PASS;
        ftp_send_reply(s, "331 Please specify the password.\r\n");
        return;
    }
    s->auth = FTP_STATE_LOGGED_IN;
    ftp_send_reply(s, "230 Login successful.\r\n");
}

static void cmd_pass(ftp_session_t *s, const char *arg)
{
    if (s->auth != FTP_STATE_WAIT_PASS) {
        ftp_send_reply(s, "503 Login with USER first.\r\n");
        return;
    }
    /* FTP_STATE_WAIT_PASS is only ever entered when FTP_SERVER_PASS is
     * non-NULL, so a NULL here can only mean a corrupt state. */
    const char *need_pass = FTP_SERVER_PASS;
    if (need_pass != NULL && arg && strcmp(arg, need_pass) == 0) {
        s->auth = FTP_STATE_LOGGED_IN;
        ftp_send_reply(s, "230 Login successful.\r\n");
    } else {
        s->auth = FTP_STATE_WAIT_USER;
        ftp_send_reply(s, "530 Login incorrect.\r\n");
    }
}

static void cmd_quit(ftp_session_t *s, const char *arg)
{
    (void)arg;
    ftp_send_reply(s, "221 Goodbye.\r\n");
    ftp_close_session(s);
}

/**
 * Command dispatch table.
 *
 * `pre_auth` marks the three commands accepted before login; `keeps_rnfr`
 * marks the pair that may leave a pending rename intact (RFC 959: RNTO must
 * directly follow RNFR, anything else cancels it).
 */
static const struct ftp_cmd ftp_commands[] = {
    /*  name    pre  rnfr  handler   */
    {"USER",     1,   0,   cmd_user},
    {"PASS",     1,   0,   cmd_pass},
    {"QUIT",     1,   0,   cmd_quit},
    {"SYST",     0,   0,   cmd_syst},
    {"FEAT",     0,   0,   cmd_feat},
    {"OPTS",     0,   0,   cmd_opts},
    {"NOOP",     0,   0,   cmd_noop},
    {"TYPE",     0,   0,   cmd_type},
    {"PWD",      0,   0,   cmd_pwd},
    {"XPWD",     0,   0,   cmd_pwd},
    {"CWD",      0,   0,   cmd_cwd},
    {"XCWD",     0,   0,   cmd_cwd},
    {"CDUP",     0,   0,   cmd_cdup},
    {"XCUP",     0,   0,   cmd_cdup},
    {"PASV",     0,   0,   cmd_pasv},
    {"PORT",     0,   0,   cmd_port},
    {"LIST",     0,   0,   cmd_list},
    {"NLST",     0,   0,   cmd_nlst},
    {"RETR",     0,   0,   cmd_retr},
    {"STOR",     0,   0,   cmd_stor},
    {"DELE",     0,   0,   cmd_dele},
    {"MKD",      0,   0,   cmd_mkd},
    {"XMKD",     0,   0,   cmd_mkd},
    {"RMD",      0,   0,   cmd_rmd},
    {"XRMD",     0,   0,   cmd_rmd},
    {"RNFR",     0,   1,   cmd_rnfr},
    {"RNTO",     0,   1,   cmd_rnto},
    {"SIZE",     0,   0,   cmd_size},
    {"ABOR",     0,   0,   cmd_abor},
};

/**
 * Parse one command from the command buffer and dispatch it.
 */
static void ftp_process_command(ftp_session_t *s)
{
    /* NUL-terminate and strip trailing CR/LF. */
    s->cmd_buf[s->cmd_len] = '\0';
    char *line = s->cmd_buf;
    char *end  = line + s->cmd_len;
    while (end > line && (*(end - 1) == '\r' || *(end - 1) == '\n'))
        *(--end) = '\0';

    /* Split into command and argument. */
    char *cmd = line;
    char *arg = NULL;
    char *sp  = strchr(line, ' ');
    if (sp) {
        *sp  = '\0';
        arg  = sp + 1;
        /* skip leading whitespace in argument */
        while (*arg == ' ') arg++;
    }

    /* Uppercase the command for case-insensitive matching. */
    for (char *p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);

    const struct ftp_cmd *c = NULL;
    for (size_t i = 0; i < sizeof(ftp_commands) / sizeof(ftp_commands[0]); i++) {
        if (strcmp(cmd, ftp_commands[i].name) == 0) {
            c = &ftp_commands[i];
            break;
        }
    }

    if (c && c->pre_auth) {
        c->handler(s, arg);
        return;
    }
    if (s->auth != FTP_STATE_LOGGED_IN) {
        ftp_send_reply(s, "530 Please login with USER and PASS.\r\n");
        return;
    }
    if (!c || !c->keeps_rnfr) {
        s->rnfr[0] = '\0';
    }
    if (!c) {
        ftp_send_reply(s, "502 Command not implemented.\r\n");
        return;
    }
    c->handler(s, arg);
}

/* ------------------------------------------------------------------ */
/*  Control connection callbacks                                      */
/* ------------------------------------------------------------------ */

/**
 * Dispatch every complete line currently in cmd_buf, leaving any trailing
 * partial line at the front of the buffer.
 *
 * @return 0 if the session was torn down (the caller must stop touching it
 *         and hand ERR_ABRT/ERR_OK back to lwIP), 1 otherwise.
 */
static int ftp_consume_lines(ftp_session_t *s)
{
    while (s->cmd_len > 0) {
        char *nl = (char *)memchr(s->cmd_buf, '\n', s->cmd_len);
        if (!nl) {
            /* Partial line: normally wait for more data, but while
             * resynchronising it is all tail of the discarded line. */
            if (s->discard_line) s->cmd_len = 0;
            return 1;
        }

        uint16_t line_len  = (uint16_t)(nl - s->cmd_buf + 1);
        uint16_t total_len = s->cmd_len;

        if (s->discard_line) {
            /* Tail of an over-long command line — drop through to the shift
             * below, which resynchronises on the next line. */
            s->discard_line = 0;
        } else {
            /* ftp_process_command() writes a NUL at cmd_buf[line_len]; when
             * more data follows in the buffer (pipelined commands in one TCP
             * segment) that byte belongs to the next queued line, so it must
             * be preserved and restored instead of permanently clobbered. */
            uint8_t has_next  = (line_len < total_len);
            char    next_byte = (char)(has_next ? s->cmd_buf[line_len] : '\0');

            s->cmd_len = line_len;
            ftp_process_command(s);

            /* Session may have been freed by QUIT or an undeliverable reply. */
            if (!s->in_use) return 0;

            if (has_next) s->cmd_buf[line_len] = next_byte;
        }

        /* Shift remaining bytes to the front of the buffer. */
        uint16_t remaining = (uint16_t)(total_len - line_len);
        if (remaining > 0) {
            memmove(s->cmd_buf, s->cmd_buf + line_len, remaining);
        }
        s->cmd_len = remaining;
    }
    return 1;
}

static err_t ftp_ctrl_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    (void)err;

    if (!s) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    s->ctrl_aborted = 0;

    if (!p) {
        /* Client disconnected. */
        ftp_close_session(s);
        return s->ctrl_aborted ? ERR_ABRT : ERR_OK;
    }

    /* Reset idle timeout on any received data. */
    s->idle_polls = 0;

    uint16_t total = p->tot_len;
    tcp_recved(pcb, total);

    /* Feed the segment through the command buffer in buffer-sized bites.
     * Copying only what currently fits and dropping the rest would throw away
     * the '\n' that ends an over-long line — and with it whatever pipelined
     * commands followed it — so the segment is consumed in full instead. */
    for (uint16_t consumed = 0; consumed < total; ) {
        uint16_t room  = (uint16_t)(FTP_SERVER_CMD_BUF_SIZE - 1 - s->cmd_len);
        uint16_t chunk = (uint16_t)(total - consumed);
        if (chunk > room) chunk = room;

        pbuf_copy_partial(p, s->cmd_buf + s->cmd_len, chunk, consumed);
        s->cmd_len = (uint16_t)(s->cmd_len + chunk);
        consumed   = (uint16_t)(consumed + chunk);

        /* A complete line ends with '\n'; one segment may hold several. */
        if (!ftp_consume_lines(s)) break;

        /* Buffer full without a complete line: discard it and skip everything
         * up to the next '\n' — which may still be in this very segment — so
         * the tail of the over-long line is not mistaken for a fresh command. */
        if (s->cmd_len >= FTP_SERVER_CMD_BUF_SIZE - 1) {
            s->cmd_len      = 0;
            s->discard_line = 1;
            ftp_send_reply(s, "500 Command line too long.\r\n");
            if (!s->in_use) break;
        }
    }

    pbuf_free(p);
    return s->ctrl_aborted ? ERR_ABRT : ERR_OK;
}

static void ftp_ctrl_err(void *arg, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    (void)err;
    if (!s) return;

    /* PCB already freed by lwIP. */
    s->ctrl_pcb = NULL;
    ftp_close_data(s);
    s->in_use = 0;
}

static err_t ftp_ctrl_poll(void *arg, struct tcp_pcb *pcb)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    (void)pcb;
    if (!s) return ERR_OK;

    /* Counts silence on *both* channels: ftp_ctrl_recv() and the data-channel
     * callbacks reset it, so a long transfer survives while a genuinely stuck
     * one still times out. */
    s->idle_polls++;
    if (s->idle_polls >= FTP_SERVER_IDLE_TIMEOUT_POLLS) {
        s->ctrl_aborted = 0;
        ftp_close_session(s);
        return s->ctrl_aborted ? ERR_ABRT : ERR_OK;
    }
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  Accept a new control connection                                   */
/* ------------------------------------------------------------------ */

static err_t ftp_ctrl_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;

    /* Release the listener's backlog slot; a build with TCP_LISTEN_BACKLOG
     * enabled otherwise stops accepting after `backlog` connections. */
    tcp_accepted(s_listen_pcb);

    ftp_session_t *s = ftp_alloc_session();
    if (!s) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    s->ctrl_pcb = pcb;
    /* FTP_SERVER_USER defaults to NULL but is project-configurable (ftp_server.h). */
    // NOLINTNEXTLINE(misc-redundant-expression)
    s->auth     = (FTP_SERVER_USER == NULL)
                      ? FTP_STATE_LOGGED_IN
                      : FTP_STATE_WAIT_USER;

    tcp_arg(pcb, s);
    tcp_recv(pcb, ftp_ctrl_recv);
    tcp_err(pcb, ftp_ctrl_err);
    tcp_poll(pcb, ftp_ctrl_poll, 10); /* ~5 s at default TCP_SLOW_INTERVAL */

    ftp_send_reply(s, "220 LwIP-LFS FTP server ready.\r\n");
    return s->ctrl_aborted ? ERR_ABRT : ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

err_t ftp_server_init(lfs_t *lfs)
{
    if (!lfs) return ERR_ARG;
    if (lfs->cfg->cache_size > FTP_SERVER_FILE_CACHE_SIZE) return ERR_ARG;

    /* Make re-initialisation safe: drop any listener/sessions still around. */
    ftp_server_deinit();
    s_lfs = lfs;

    s_next_pasv_port = FTP_SERVER_PASV_PORT_MIN;

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return ERR_MEM;

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, FTP_SERVER_PORT);
    if (err != ERR_OK) {
        tcp_close(pcb);
        return err;
    }

    /* On failure tcp_listen() leaves `pcb` allocated and ownership with us. */
    struct tcp_pcb *lpcb = tcp_listen(pcb);
    if (!lpcb) {
        tcp_close(pcb);
        return ERR_MEM;
    }

    s_listen_pcb = lpcb;
    tcp_accept(lpcb, ftp_ctrl_accept);

    return ERR_OK;
}

void ftp_server_deinit(void)
{
    for (int i = 0; i < FTP_SERVER_MAX_CLIENTS; i++) {
        if (s_sessions[i].in_use) {
            ftp_close_session(&s_sessions[i]);
        }
    }
    memset(s_sessions, 0, sizeof(s_sessions));

    (void)ftp_close_pcb(&s_listen_pcb, 1);

    s_lfs = NULL;
}
