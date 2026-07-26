/**
 * @file ftp_server.c
 * @brief FTP server implementation using LwIP raw TCP API and LittleFS.
 *
 * Supports: USER, PASS, SYST, FEAT, TYPE, MODE, STRU, PWD, CWD, CDUP, PASV,
 *           PORT, LIST, NLST, RETR, STOR, DELE, MKD, RMD, RNFR, RNTO, SIZE,
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

/* Deliberately no <stdio.h>/<stdlib.h>: snprintf() and strtoul() were the only
 * users, and on a typical newlib target snprintf drags several KB of vfprintf
 * into an image whose whole point is to be small. The handful of replies and
 * the two listing formats that need formatting go through ftp_put_str()/
 * ftp_put_uint() instead, and ftp_parse_port_arg() reads its own digits. */
#include <string.h>
#include <ctype.h>

/* The listing formatter reserves the last two bytes of the transfer buffer for
 * the CRLF that terminates an entry, and a reply buffer has to hold at least
 * the fixed text of the longest reply. Both are configurable, so state the
 * floor rather than let a small value corrupt the arithmetic. */
#if FTP_SERVER_DATA_BUF_SIZE < 64
#error "FTP_SERVER_DATA_BUF_SIZE must be at least 64"
#endif
#if FTP_SERVER_PATH_MAX < 16
#error "FTP_SERVER_PATH_MAX must be at least 16"
#endif

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
    FTP_STATE_WAIT_PASS,     /**< got a known USER, expect PASS */
    FTP_STATE_WAIT_PASS_BAD, /**< got an unknown USER; PASS will be refused */
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

/* Scratch for the resolved path a command operates on, shared for the same
 * reason as s_reply: command handlers never nest, so one buffer serves them
 * all and no handler needs a FTP_SERVER_PATH_MAX frame of its own. Only valid
 * until the handler that called ftp_arg_path() returns. */
static char               s_path[FTP_SERVER_PATH_MAX];

/* Replies reused by several handlers — one copy in .rodata each. */
static const char ftp_reply_need_file[] = "501 Specify file name.\r\n";
static const char ftp_reply_need_dir[]  = "501 Specify directory name.\r\n";
static const char ftp_reply_login_ok[]  = "230 Login successful.\r\n";
static const char ftp_reply_login_bad[] = "530 Login incorrect.\r\n";
static const char ftp_reply_complete[]  = "226 Transfer complete.\r\n";
static const char ftp_reply_aborted[]   =
    "426 Connection closed; transfer aborted.\r\n";
static const char ftp_reply_no_data[]   = "425 Can't open data connection.\r\n";
static const char ftp_reply_local_err[] =
    "451 Requested action aborted: local error in processing.\r\n";

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
static err_t  ftp_data_poll(void *arg, struct tcp_pcb *pcb);
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
    /* Not strncpy(): that NUL-pads the whole destination, so every CWD would
     * scribble over all of FTP_SERVER_PATH_MAX to store a short path. */
    size_t n = 0;
    while (n + 1 < size && src[n] != '\0') {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Helper: bounded text building                                     */
/*                                                                    */
/*  Both take the write cursor and a one-past-the-last-writable-byte   */
/*  limit, clamp to it, and return the new cursor, so a sequence of    */
/*  appends can be written without a bounds check between each one.    */
/*  Nothing is NUL-terminated: the callers either terminate once at    */
/*  the end (replies) or use the returned length (listing entries).    */
/* ------------------------------------------------------------------ */

static char *ftp_put_str(char *dst, const char *const end, const char *src)
{
    while (*src != '\0' && dst < end) *dst++ = *src++;
    return dst;
}

/**
 * Append @p val in decimal, right-aligned in at least @p width columns by
 * padding with leading spaces (@p width 0 for no padding) — the LIST size
 * column is the only caller that needs it.
 */
static char *ftp_put_uint(char *dst, const char *const end, unsigned long val,
                          unsigned width)
{
    char     digits[20];   /* 2^64 is 20 decimal digits */
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + (val % 10));
        val /= 10;
    } while (val != 0);

    while (width > n && dst < end) {
        *dst++ = ' ';
        width--;
    }
    while (n > 0 && dst < end) *dst++ = digits[--n];
    return dst;
}

/** Case-insensitive strcmp — strcasecmp is POSIX, not C99. */
static int ftp_strcasecmp(const char *a, const char *b)
{
    while (*a && toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
        a++;
        b++;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
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

/**
 * tcp_new() + tcp_bind() + tcp_listen() on @p port, cleaning up after itself
 * at whichever step fails.
 *
 * @param err Receives the failing step's error: ERR_MEM when a PCB could not
 *            be allocated, otherwise tcp_bind()'s error (ERR_USE for a port
 *            that is already taken). Untouched on success.
 * @return the listening PCB, or NULL with nothing left allocated.
 */
static struct tcp_pcb *ftp_listen_on(uint16_t port, err_t *err)
{
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        *err = ERR_MEM;
        return NULL;
    }

    *err = tcp_bind(pcb, IP_ADDR_ANY, port);
    if (*err != ERR_OK) {
        tcp_close(pcb);
        return NULL;
    }

    /* tcp_listen() consumes `pcb` and returns a fresh listening PCB on
     * success; on failure `pcb` is untouched and still ours to free. */
    struct tcp_pcb *lpcb = tcp_listen(pcb);
    if (!lpcb) {
        tcp_close(pcb);
        *err = ERR_MEM;
    }
    return lpcb;
}

/**
 * Fetch the remote address of @p pcb.
 *
 * Hands back the whole ip_addr_t rather than an IPv4 u32 so the callers can
 * compare with ip_addr_cmp(); flattening first would defeat the RFC 2577
 * checks entirely (DESIGN.md, "Security").
 *
 * @return 0 on success, -1 if the peer cannot be determined.
 */
static int ftp_peer_addr(struct tcp_pcb *pcb, ip_addr_t *addr)
{
    u16_t port;
    if (!pcb || tcp_tcp_get_tcp_addrinfo(pcb, 0, addr, &port) != ERR_OK)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helper: path resolution                                           */
/* ------------------------------------------------------------------ */

/**
 * Append the components of @p src to the normalised absolute path being built
 * in @p out, resolving "." and "..", collapsing runs of '/', and leaving a
 * trailing '/' after every component for the caller to strip.
 *
 * @param pdst In/out write cursor; @p out[0] is already the leading '/'.
 * @return 0 on success, -1 if the result would not fit in FTP_SERVER_PATH_MAX.
 */
static int ftp_append_path(const char *src, char *out, char **pdst)
{
    char *dst = *pdst;
    char *end = out + FTP_SERVER_PATH_MAX - 1;

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

        /* copy component. Overflowing would silently address a *different*
         * existing path (a prefix of the requested one), so reject instead. */
        if (dst + len + 1 > end) {
            *pdst = dst;
            return -1;
        }
        memcpy(dst, comp, len);
        dst += len;
        *dst++ = '/';
    }

    *pdst = dst;
    return 0;
}

/**
 * Build an absolute path from the session CWD and a user-supplied argument.
 * Result is written to @p out (size FTP_SERVER_PATH_MAX).
 * Returns 0 on success, -1 on overflow.
 *
 * The CWD and the argument are normalised straight into @p out rather than
 * concatenated first, so no FTP_SERVER_PATH_MAX-sized frame lands on the
 * (scarce) MCU stack — the same reason s_path and s_reply are static.
 */
static int ftp_resolve_path(const ftp_session_t *s, const char *arg, char *out)
{
    char *dst = out;
    *dst++ = '/';

    /* An absent argument means "the CWD itself"; a relative one starts from
     * the CWD; an absolute one ignores it. */
    if (!arg || arg[0] == '\0') {
        arg = s->cwd;
    } else if (arg[0] != '/' && ftp_append_path(s->cwd, out, &dst) < 0) {
        return -1;
    }

    if (ftp_append_path(arg, out, &dst) < 0) return -1;

    /* remove trailing '/' unless root */
    if (dst > out + 1 && *(dst - 1) == '/') dst--;
    *dst = '\0';

    return 0;
}

/**
 * Resolve a command argument into the shared s_path buffer, replying on
 * failure so the caller can simply return.
 *
 * @param missing Reply sent when @p arg is absent, or NULL to treat an absent
 *                argument as "the current directory".
 * @return s_path on success, NULL after a reply has been sent.
 */
static const char *ftp_arg_path(ftp_session_t *s, const char *arg,
                                const char *missing)
{
    /* A trailing space ("RMD ") parses to an empty argument rather than no
     * argument at all. Resolving it would yield the CWD, so `RMD ` would
     * cheerfully delete the directory the client is sitting in. */
    if (arg && arg[0] == '\0') arg = NULL;

    if (!arg && missing) {
        ftp_send_reply(s, missing);
        return NULL;
    }
    if (ftp_resolve_path(s, arg, s_path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return NULL;
    }
    return s_path;
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
            /* The memset above already supplied the terminator. */
            s->cwd[0] = '/';
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

/**
 * Send a 257 reply naming @p path, followed by @p tail.
 *
 * RFC 959 Appendix II: the pathname is quoted and every embedded double quote
 * is doubled, so the client can tell which quote closes the name. A path whose
 * quoted form would not fit in s_reply is truncated rather than given a second
 * FTP_SERVER_PATH_MAX of static RAM — that needs a path near the length limit
 * made mostly of quote characters.
 *
 * A doubled quote is emitted all-or-nothing. Truncating between the two halves
 * would leave the escape unbalanced, and the client would then read the closing
 * quote as the second half of an escaped one and never find the end of the
 * name — a mangled reply rather than a merely shortened one.
 */
static void ftp_send_257(ftp_session_t *s, const char *path, const char *tail)
{
    char       *dst = ftp_put_str(s_reply, s_reply + sizeof(s_reply), "257 \"");
    /* Reserve room for the closing quote, the tail and the terminator. */
    char *const end = s_reply + sizeof(s_reply) - strlen(tail) - 2;

    for (; *path != '\0'; path++) {
        int need = (*path == '"') ? 2 : 1;
        if (end - dst < need) break;
        *dst++ = *path;
        if (*path == '"') *dst++ = '"';
    }
    *dst++ = '"';
    dst = ftp_put_str(dst, s_reply + sizeof(s_reply) - 1, tail);
    *dst = '\0';

    ftp_send_reply(s, s_reply);
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
 * Tear the data channel down on behalf of something *other* than the transfer
 * itself — ABOR, or a PASV/PORT that supersedes it.
 *
 * A transfer that already got its "150" owes the client a completion reply;
 * without the 426 the client blocks waiting for a 226 that will never come.
 * A transfer that is merely armed (command issued, data connection not up
 * yet) never announced itself, so it is dropped silently.
 */
static void ftp_abort_transfer(ftp_session_t *s)
{
    int announced = (s->data_mode != FTP_DATA_IDLE &&
                     s->data_pcb != NULL && !s->data_connecting);
    ftp_close_data(s);
    if (announced) ftp_send_reply(s, ftp_reply_aborted);
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

/** True for the modes ftp_send_next_data() knows how to feed. */
static int ftp_mode_is_send(ftp_data_mode_t mode)
{
    return mode == FTP_DATA_RETR || mode == FTP_DATA_LIST ||
           mode == FTP_DATA_NLST;
}

/**
 * Attempt to flush all pending bytes in data_buf[data_offset..) to the
 * data PCB.
 *
 * Queues only — the caller owns the tcp_output(), so a whole directory listing
 * is handed to lwIP before anything is pushed onto the wire.
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
        if (space == 0) break; /* wait for sent callback */
        uint16_t to_send = (s->data_pending < space) ? s->data_pending : space;
        err_t err = tcp_write(s->data_pcb, s->data_buf + s->data_offset,
                              to_send, TCP_WRITE_FLAG_COPY);
        /* ERR_MEM is back-pressure, retried from tcp_sent or ftp_data_poll;
         * anything else (ERR_CONN, ERR_ARG, ...) never resolves on its own, and
         * treating it as back-pressure would hang the transfer until the idle
         * timer fires. */
        if (err == ERR_MEM) break;
        if (err != ERR_OK)  return -1;
        s->data_offset  += to_send;
        s->data_pending -= to_send;
    }

    return s->data_pending == 0;
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

        /* The entry is returned as a length, never as a C string, so the whole
         * buffer is available and no slot goes to a NUL terminator. */
        char *const buf = (char *)s->data_buf;
        char *const end = buf + FTP_SERVER_DATA_BUF_SIZE - 2; /* CRLF reserved */
        char       *dst = buf;

        if (s->data_mode == FTP_DATA_LIST) {
            /* A Unix-style listing: 45 bytes of fixed columns, the name, and
             * the CRLF. littlefs stores no timestamp, so the date is a
             * constant that clients parse and ignore. */
            dst = ftp_put_str(dst, end, (info.type == LFS_TYPE_DIR)
                                            ? "drwxr-xr-x" : "-rw-r--r--");
            dst = ftp_put_str(dst, end, " 1 ftp ftp ");
            dst = ftp_put_uint(dst, end, (unsigned long)info.size, 10);
            dst = ftp_put_str(dst, end, " Jan 01  2000 ");
        }
        dst = ftp_put_str(dst, end, info.name);

        /* The CRLF is what truncation drops first, and without it a clamped
         * name runs into the following entry and the client parses one mangled
         * line instead of two. Reserving the two bytes up front means it is
         * always there, so there is no clamped-length case to repair. */
        *dst++ = '\r';
        *dst++ = '\n';
        return (int)(dst - buf);
    }
}

/**
 * Fill data_buf with the next chunk and write it to the data PCB.
 * Called after the data connection is established and after each
 * tcp_sent callback confirms the previous chunk was ACK'd.
 */
static void ftp_send_next_data(ftp_session_t *s)
{
    /* data_connecting means the PCB exists but its handshake is outstanding, so
     * it is not a usable data connection yet. lwIP polls a SYN_SENT PCB and
     * tcp_write() accepts one, so without this guard ftp_data_poll() would run
     * the whole transfer into a connection that never opened. See DESIGN.md,
     * "Data transfers". */
    if (!s->data_pcb || s->data_connecting) return;

    for (;;) {
        /* Flush what is already buffered — leftovers from a previous fill on
         * the first pass, the chunk just filled on later ones. */
        int flushed = ftp_send_pending(s);
        if (flushed == 0) {
            /* Push once here rather than once per chunk: a listing is many
             * small entries, and one tcp_output each would stop lwIP from
             * coalescing them into full segments. The paths that leave this
             * loop instead go through ftp_close_data(), and tcp_close()
             * flushes the queue itself. */
            tcp_output(s->data_pcb);
            return;   /* wait for tcp_sent */
        }
        if (flushed < 0) {
            ftp_close_data(s);
            ftp_send_reply(s, ftp_reply_aborted);
            return;
        }

        int n = ftp_fill_next_chunk(s);
        if (n < 0) {
            ftp_close_data(s);
            ftp_send_reply(s, (n == FTP_CHUNK_ERR) ? ftp_reply_local_err
                                                   : ftp_reply_complete);
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

    /* lwIP has exactly one call site for this callback (tcp_in.c, on the ACK
     * that completes the handshake) and it always passes a live PCB and
     * ERR_OK; a *failed* connect is reported through tcp_err instead, which
     * is where ftp_data_err() turns it into a 425. */
    (void)err;

    /* The session may have moved on while the connect was in flight — closed
     * by QUIT, the idle timer or an error, its slot possibly already handed to
     * a different client, or the transfer cancelled by ABOR/PASV. Only a
     * session still waiting on this very connect may be touched; anything else
     * gets the stray connection closed and nothing else. */
    if (!s || !s->in_use || !s->data_connecting || s->data_pcb != pcb) {
        struct tcp_pcb *stale = pcb;
        return ftp_close_pcb(&stale, 0) ? ERR_ABRT : ERR_OK;
    }

    s->data_connecting = 0;
    s->data_aborted    = 0;
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
         * so the mode is a constant. Clients that care parse the byte count.
         * cmd_retr() has already rejected a negative size. */
        char *const end = s_reply + sizeof(s_reply) - 1;
        char       *dst = ftp_put_str(s_reply, end,
                    "150 Opening BINARY mode data connection (");
        dst = ftp_put_uint(dst, end, (unsigned long)s->retr_size, 0);
        dst = ftp_put_str(dst, end, " bytes).\r\n");
        *dst = '\0';
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
            tcp_poll(pcb, ftp_data_poll, 4); /* ~2 s; see ftp_data_poll() */

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
            /* Fall through to ftp_close_data(), which detaches ftp_data_err()
             * before closing. A bare tcp_abort() here would re-enter it and
             * emit a spurious 426 ahead of the 425 below; see DESIGN.md,
             * "Data transfers". */
        }
        ftp_close_data(s);
        ftp_send_reply(s, ftp_reply_no_data);
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
    /* lwIP passes a NULL pcb when it could not allocate one for the incoming
     * connection; there is nothing to abort in that case. */
    if (!pcb) return ERR_VAL;

    ftp_session_t *s = (ftp_session_t *)arg;
    if (err != ERR_OK || !s) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    /* RFC 2577: the data connection must come from the same host as the control
     * connection — the passive counterpart of the anti-bounce check in
     * cmd_port(), since the listener binds IP_ADDR_ANY (DESIGN.md,
     * "Security"). The listener stays open so the genuine client can still
     * connect. */
    ip_addr_t ctrl_addr;
    ip_addr_t data_addr;
    if (ftp_peer_addr(s->ctrl_pcb, &ctrl_addr) < 0 ||
        ftp_peer_addr(pcb, &data_addr) < 0 ||
        !ip_addr_cmp(&ctrl_addr, &data_addr)) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    /* No tcp_accepted() here on purpose: lwIP calls tcp_backlog_accepted()
     * itself immediately before invoking this callback (tcp_in.c), and
     * tcp_accepted() is now only a no-op compatibility macro (tcp.h). */
    s->data_aborted = 0;
    s->data_pcb = pcb;
    tcp_arg(pcb, s);
    tcp_recv(pcb, ftp_data_recv);
    tcp_sent(pcb, ftp_data_sent);
    tcp_err(pcb, ftp_data_err);
    tcp_poll(pcb, ftp_data_poll, 4); /* ~2 s; see ftp_data_poll() */

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

    /* Data arriving before the command that consumes it has been processed.
     * Returning a non-OK error *without* consuming the pbuf hands it back to
     * lwIP, which parks it in pcb->refused_data (tcp_in.c) and re-offers it
     * from tcp_fasttmr every 250 ms (tcp.c) until it is taken. Deliberately
     * ahead of the idle_polls reset below, so being re-offered forever cannot
     * keep a session alive. See DESIGN.md, "Data transfers". */
    if (p && s->data_mode == FTP_DATA_IDLE) return ERR_MEM;

    s->data_aborted = 0;
    /* Data-channel progress keeps the session alive; see ftp_ctrl_poll(). */
    s->idle_polls = 0;

    if (!p) {
        /* Client closed the data connection. For an upload that is how the
         * transfer ends; for a download it means the client gave up partway,
         * and answering 226 would report a truncated file as complete. */
        ftp_data_mode_t mode = s->data_mode;

        /* littlefs buffers writes and only flushes the tail of the file from
         * lfs_file_close(), so a full volume or a failing block device on the
         * last chunk surfaces *here* and nowhere else. Closing through
         * ftp_close_data(), which discards the result, would announce a
         * truncated upload as complete. */
        int flush_failed = 0;
        if (mode == FTP_DATA_STOR && s->file_open) {
            flush_failed = (lfs_file_close(s_lfs, &s->file) < 0);
            s->file_open = 0;
        }

        ftp_close_data(s);
        if (mode == FTP_DATA_STOR) {
            ftp_send_reply(s, flush_failed ? ftp_reply_local_err
                                           : ftp_reply_complete);
        } else if (mode != FTP_DATA_IDLE) {
            ftp_send_reply(s, ftp_reply_aborted);
        }
        return s->data_aborted ? ERR_ABRT : ERR_OK;
    }

    if (s->data_mode == FTP_DATA_STOR && s->file_open) {
        /* Write received data to LittleFS file. */
        struct pbuf *q;
        for (q = p; q != NULL; q = q->next) {
            lfs_ssize_t w = lfs_file_write(s_lfs, &s->file,
                                           q->payload, q->len);
            /* littlefs returns either the full count or a negative error —
             * the latter when a cache flush mid-pbuf hits a full volume.
             * Accepting it would silently truncate the uploaded file. */
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

/**
 * Shared body of the two data-channel callbacks that resume a send.
 *
 * @param progress Whether reaching here proves the transfer moved. Only the
 *                 tcp_sent path does; see ftp_data_poll().
 */
static err_t ftp_data_pump(void *arg, int progress)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    if (!s) return ERR_OK;
    s->data_aborted = 0;
    if (progress) s->idle_polls = 0;

    if (ftp_mode_is_send(s->data_mode)) ftp_send_next_data(s);

    return s->data_aborted ? ERR_ABRT : ERR_OK;
}

/** Previous chunk ACK'd — send more data. */
static err_t ftp_data_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    return ftp_data_pump(arg, 1);
}

/**
 * Retry a send that tcp_write() could not even queue.
 *
 * tcp_write() returns ERR_MEM both for a full send window — where the peer's
 * ACK brings ftp_data_sent() along to resume the transfer — and for an empty
 * pbuf/segment pool, where it queues nothing at all (tcp_out.c tolerates
 * snd_queuelen == 0 on its error path). In the second case no ACK is coming
 * and no tcp_sent will ever fire, so without this the transfer would sit
 * untouched until the idle timer killed the whole session.
 *
 * Deliberately does not count as progress: a transfer that only ever gets here
 * is making none and should still time out. Note that lwIP also polls a PCB
 * whose connect is still outstanding; ftp_send_next_data() is what refuses to
 * serve that.
 */
static err_t ftp_data_poll(void *arg, struct tcp_pcb *pcb)
{
    (void)pcb;
    return ftp_data_pump(arg, 0);
}

static void ftp_data_err(void *arg, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    (void)err;
    if (!s) return;

    /* PCB is already freed by lwIP before the error callback fires. */
    s->data_pcb = NULL;

    /* lwIP reports a *failed* active-mode connect through this callback rather
     * than through ftp_data_connected(), and a connection that never opened is
     * a 425, not a transfer that died in flight. */
    int connecting       = s->data_connecting;
    int was_transferring = (s->data_mode != FTP_DATA_IDLE);
    ftp_close_data(s);   /* handles pasv_listen, file, dir, all flags */

    /* Without this the client waits for a 226 that will never arrive. */
    if (connecting) {
        ftp_send_reply(s, ftp_reply_no_data);
    } else if (was_transferring) {
        ftp_send_reply(s, ftp_reply_aborted);
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
    if (arg && ftp_strcasecmp(arg, "UTF8 ON") == 0) {
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

/**
 * Match a transfer-parameter argument against a single Telnet character code,
 * case-insensitively. TYPE, MODE and STRU all take exactly one such letter.
 */
static int ftp_arg_is_code(const char *arg, char code)
{
    return arg && toupper((unsigned char)arg[0]) == code && arg[1] == '\0';
}

/**
 * RFC 959 section 3.1.1.1 makes ASCII the default representation type and one
 * that "must be accepted by all FTP implementations"; rejecting it stops the
 * clients that set TYPE A before a listing. This server is byte-transparent —
 * it performs no CRLF translation in either direction, which is what a client
 * moving files onto a flash filesystem wants — so both types behave alike and
 * the reply says so rather than claiming a conversion that does not happen.
 */
static void cmd_type(ftp_session_t *s, const char *arg)
{
    char code = arg ? (char)toupper((unsigned char)arg[0]) : '\0';
    /* ASCII and EBCDIC take an optional format parameter; only the default,
     * Non-print, is meaningful here (section 3.1.1.5.1). */
    int ok = (code == 'A' || code == 'I') &&
             (arg[1] == '\0' ||
              (arg[1] == ' ' && ftp_arg_is_code(arg + 2, 'N')));

    if (!ok) {
        ftp_send_reply(s, "504 Only types A and I are supported.\r\n");
    } else if (code == 'I') {
        ftp_send_reply(s, "200 Switching to Binary mode.\r\n");
    } else {
        ftp_send_reply(s, "200 Type set to A (no conversion performed).\r\n");
    }
}

/* RFC 959 section 5.1 puts MODE and STRU in the minimum implementation every
 * server must accept "for the default values" — Stream and File. Clients that
 * send them defensively get a 200 instead of a 502 for a no-op. */

static void cmd_mode(ftp_session_t *s, const char *arg)
{
    if (ftp_arg_is_code(arg, 'S')) {
        ftp_send_reply(s, "200 Mode set to S.\r\n");
    } else {
        ftp_send_reply(s, "504 Only stream mode is supported.\r\n");
    }
}

static void cmd_stru(ftp_session_t *s, const char *arg)
{
    if (ftp_arg_is_code(arg, 'F')) {
        ftp_send_reply(s, "200 Structure set to F.\r\n");
    } else {
        ftp_send_reply(s, "504 Only file structure is supported.\r\n");
    }
}

static void cmd_pwd(ftp_session_t *s, const char *arg)
{
    (void)arg;
    ftp_send_257(s, s->cwd, " is the current directory.\r\n");
}

/** Adopt @p path as the session CWD and confirm it. */
static void ftp_set_cwd(ftp_session_t *s, const char *path)
{
    ftp_strlcpy(s->cwd, path, sizeof(s->cwd));
    ftp_send_reply(s, "250 Directory successfully changed.\r\n");
}

static void cmd_cwd(ftp_session_t *s, const char *arg)
{
    const char *path = ftp_arg_path(s, arg, NULL);
    if (!path) return;

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
    const char *path = ftp_arg_path(s, "..", NULL);
    if (!path) return;
    ftp_set_cwd(s, path);
}

static void cmd_pasv(ftp_session_t *s, const char *arg)
{
    (void)arg;
    /* Close any previous data channel, telling the client if that cut a
     * transfer short. An undeliverable 426 tears the session down. */
    ftp_abort_transfer(s);
    if (!s->ctrl_pcb) return;

    uint16_t port  = 0;
    uint16_t range = FTP_SERVER_PASV_PORT_MAX - FTP_SERVER_PASV_PORT_MIN + 1;
    struct tcp_pcb *lpcb = NULL;
    err_t err = ERR_MEM;
    for (uint16_t tries = 0; tries < range; tries++) {
        port = ftp_next_pasv_port();
        lpcb = ftp_listen_on(port, &err);
        /* Only a port collision is worth walking the range for; an exhausted
         * PCB pool will not refill inside this loop. */
        if (lpcb || err != ERR_USE) break;
    }
    if (!lpcb) {
        ftp_send_reply(s, "421 Cannot open data socket.\r\n");
        return;
    }

    s->pasv_listen_pcb = lpcb;
    tcp_arg(lpcb, s);
    tcp_accept(lpcb, ftp_data_accept);

    /* Build reply with the server's IP from the control connection. */
    ip_addr_t local_ip;
    u16_t local_port;
    if (tcp_tcp_get_tcp_addrinfo(s->ctrl_pcb, 1, &local_ip, &local_port) != ERR_OK ||
        !IP_IS_V4(&local_ip)) {
        /* local_ip is untouched on failure — advertising it would send the
         * client at a garbage address. A 227 carries four IPv4 octets and
         * nothing else, so a control connection that is not IPv4 has to be
         * refused rather than answered with a bogus address; see DESIGN.md,
         * "Security". IP_IS_V4 is a constant 1 in an IPv4-only lwIP. */
        ftp_close_data(s);
        ftp_send_reply(s, "421 Cannot determine server address.\r\n");
        return;
    }

    uint32_t ip4 = ip_addr_get_ip4_u32(&local_ip);
    uint8_t *ip  = (uint8_t *)&ip4;

    char *const end = s_reply + sizeof(s_reply) - 1;
    char       *dst = ftp_put_str(s_reply, end, "227 Entering Passive Mode (");
    for (int i = 0; i < 4; i++) {
        dst = ftp_put_uint(dst, end, ip[i], 0);
        dst = ftp_put_str(dst, end, ",");
    }
    dst = ftp_put_uint(dst, end, (unsigned long)(port >> 8), 0);
    dst = ftp_put_str(dst, end, ",");
    dst = ftp_put_uint(dst, end, (unsigned long)(port & 0xff), 0);
    dst = ftp_put_str(dst, end, ").\r\n");
    *dst = '\0';
    ftp_send_reply(s, s_reply);
}

/**
 * Parse a PORT argument, "h1,h2,h3,h4,p1,p2", into six 0..255 fields.
 *
 * Hand-rolled rather than strtoul(): that pulls in <stdlib.h> for one call, and
 * it also accepts leading whitespace and a sign, neither of which the RFC 959
 * HOST-PORT syntax permits. Only decimal digits are taken here.
 *
 * @return 0 on success, -1 on any syntax error.
 */
static int ftp_parse_port_arg(const char *arg, uint8_t fields[6])
{
    const char *p = arg;
    for (int i = 0; i < 6; i++) {
        unsigned val    = 0;
        int      digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = (val * 10u) + (unsigned)(*p++ - '0');
            /* Bail on the fourth digit rather than let a long run of them
             * wrap around into a value that passes the 255 test. */
            if (++digits > 3 || val > 255) return -1;
        }
        if (digits == 0) return -1;
        if (*p != ((i < 5) ? ',' : '\0')) return -1;
        if (i < 5) p++;
        fields[i] = (uint8_t)val;
    }
    return 0;
}

static void cmd_port(ftp_session_t *s, const char *arg)
{
    uint8_t fields[6];
    if (!arg || ftp_parse_port_arg(arg, fields) < 0) {
        ftp_send_reply(s, "501 Syntax error in parameters.\r\n");
        return;
    }

    ip_addr_t addr;
    IP_ADDR4(&addr, fields[0], fields[1], fields[2], fields[3]);

    /* RFC 2577: refuse to open a data connection to anywhere other than the
     * client itself, so the server cannot be used as an "FTP bounce" proxy.
     * ip_addr_cmp() takes the address type into account, so a control
     * connection that is not IPv4 can never match the v4 address parsed above
     * and PORT is refused outright — which is correct, since the PORT syntax
     * cannot name a non-IPv4 host in the first place. */
    ip_addr_t peer;
    if (ftp_peer_addr(s->ctrl_pcb, &peer) < 0 || !ip_addr_cmp(&peer, &addr)) {
        ftp_send_reply(s, "501 PORT address must match the control connection.\r\n");
        return;
    }

    /* Supersedes any previous data channel; a transfer already under way is
     * told so rather than left hanging. */
    ftp_abort_transfer(s);
    if (!s->ctrl_pcb) return;

    s->port_addr   = addr;
    s->port_port   = (uint16_t)(((unsigned)fields[4] << 8) | fields[5]);
    s->port_active = 1;

    ftp_send_reply(s, "200 PORT command successful.\r\n");
}

static void ftp_do_list(ftp_session_t *s, const char *arg, ftp_data_mode_t mode)
{
    if (ftp_transfer_busy(s)) return;

    /* Skip ls-style options ("-la", "-l -a", ...); whatever follows is a path. */
    const char *list_arg = arg;
    while (list_arg && list_arg[0] == '-') {
        while (*list_arg && *list_arg != ' ') list_arg++;
        while (*list_arg == ' ') list_arg++;
        if (*list_arg == '\0') list_arg = NULL;
    }

    const char *path = ftp_arg_path(s, list_arg, NULL);
    if (!path) return;

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

    const char *path = ftp_arg_path(s, arg, ftp_reply_need_file);
    if (!path) return -1;

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

    /* An unchecked error here would be announced to the client as a negative
     * byte count in the "150" reply. */
    s->retr_size = lfs_file_size(s_lfs, &s->file);
    if (s->retr_size < 0) {
        ftp_close_data(s);   /* closes the file just opened, disarms the mode */
        ftp_send_reply(s, ftp_reply_local_err);
        return;
    }
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
    const char *path = ftp_arg_path(s, arg, (type == LFS_TYPE_DIR)
                                            ? ftp_reply_need_dir
                                            : ftp_reply_need_file);
    if (!path) return;

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
    const char *path = ftp_arg_path(s, arg, ftp_reply_need_dir);
    if (!path) return;

    if (lfs_mkdir(s_lfs, path) < 0) {
        ftp_send_reply(s, "550 Create directory failed.\r\n");
        return;
    }
    ftp_send_257(s, path, " created.\r\n");
}

static void cmd_rnfr(ftp_session_t *s, const char *arg)
{
    const char *path = ftp_arg_path(s, arg, ftp_reply_need_file);
    if (!path) return;

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
    /* Checked before the argument is resolved: a bare RNTO with no rename
     * pending is a sequencing error (503), not a missing-filename one. */
    if (s->rnfr[0] == '\0') {
        ftp_send_reply(s, "503 RNFR required first.\r\n");
        return;
    }

    const char *path = ftp_arg_path(s, arg, ftp_reply_need_file);
    if (!path) return;

    if (lfs_rename(s_lfs, s->rnfr, path) < 0) {
        ftp_send_reply(s, "550 Rename failed.\r\n");
    } else {
        ftp_send_reply(s, "250 Rename successful.\r\n");
    }
    s->rnfr[0] = '\0';
}

static void cmd_size(ftp_session_t *s, const char *arg)
{
    const char *path = ftp_arg_path(s, arg, ftp_reply_need_file);
    if (!path) return;

    struct lfs_info info;
    if (lfs_stat(s_lfs, path, &info) < 0 || info.type != LFS_TYPE_REG) {
        ftp_send_reply(s, "550 Could not get file size.\r\n");
        return;
    }
    char *const end = s_reply + sizeof(s_reply) - 1;
    char       *dst = ftp_put_str(s_reply, end, "213 ");
    dst = ftp_put_uint(dst, end, (unsigned long)info.size, 0);
    dst = ftp_put_str(dst, end, "\r\n");
    *dst = '\0';
    ftp_send_reply(s, s_reply);
}

static void cmd_abor(ftp_session_t *s, const char *arg)
{
    (void)arg;
    ftp_abort_transfer(s);
    ftp_send_reply(s, "226 ABOR command successful.\r\n");
}

/**
 * Every path below reassigns s->auth, which is what stops a rejected USER from
 * leaving an earlier successful login standing: the client asked to become
 * somebody else and must not keep what it already had.
 */
static void cmd_user(ftp_session_t *s, const char *arg)
{
    const char *need_user = FTP_SERVER_USER;
    const char *need_pass = FTP_SERVER_PASS;

    if (need_user == NULL) {   /* authentication disabled */
        s->auth = FTP_STATE_LOGGED_IN;
        ftp_send_reply(s, ftp_reply_login_ok);
        return;
    }

    int user_ok = (arg != NULL && strcmp(arg, need_user) == 0);

    if (need_pass != NULL) {
        /* RFC 2577 section 3: answer every USER identically, so that a wrong
         * name cannot be distinguished from the configured one without also
         * knowing the password. Rejecting here instead would let an attacker
         * recover the single valid user name without guessing at it. The
         * verdict rides along in the state for cmd_pass() to apply. */
        s->auth = user_ok ? FTP_STATE_WAIT_PASS : FTP_STATE_WAIT_PASS_BAD;
        ftp_send_reply(s, "331 Please specify the password.\r\n");
        return;
    }

    /* No password configured, so there is no later step to defer the answer
     * to and the reply necessarily reveals whether the name was right. */
    s->auth = user_ok ? FTP_STATE_LOGGED_IN : FTP_STATE_WAIT_USER;
    ftp_send_reply(s, user_ok ? ftp_reply_login_ok : ftp_reply_login_bad);
}

static void cmd_pass(ftp_session_t *s, const char *arg)
{
    if (s->auth != FTP_STATE_WAIT_PASS && s->auth != FTP_STATE_WAIT_PASS_BAD) {
        ftp_send_reply(s, "503 Login with USER first.\r\n");
        return;
    }
    /* Neither WAIT_PASS state is reachable unless FTP_SERVER_PASS is non-NULL,
     * so a NULL here can only mean a corrupt state. */
    const char *need_pass = FTP_SERVER_PASS;
    if (s->auth == FTP_STATE_WAIT_PASS && need_pass != NULL &&
        arg && strcmp(arg, need_pass) == 0) {
        s->auth = FTP_STATE_LOGGED_IN;
        ftp_send_reply(s, ftp_reply_login_ok);
    } else {
        s->auth = FTP_STATE_WAIT_USER;
        ftp_send_reply(s, ftp_reply_login_bad);
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
    /* RFC 2389 section 3: FEAT may be issued before login, and clients use it
     * to decide what to send during the login sequence itself. OPTS is the
     * other half of that exchange — the usual opening is FEAT, then
     * "OPTS UTF8 ON", then USER — so it has to be reachable at the same
     * point or the client is answered 530 for acting on what FEAT told it. */
    {"FEAT",     1,   0,   cmd_feat},
    {"OPTS",     1,   0,   cmd_opts},
    {"NOOP",     0,   0,   cmd_noop},
    {"TYPE",     0,   0,   cmd_type},
    {"MODE",     0,   0,   cmd_mode},
    {"STRU",     0,   0,   cmd_stru},
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

/** Telnet IAC and the WILL/WONT/DO/DONT range that carries an option byte. */
#define FTP_TELNET_IAC  0xFFU
#define FTP_TELNET_WILL 0xFBU
#define FTP_TELNET_DONT 0xFEU

/**
 * Remove Telnet command sequences from the first @p len bytes of @p line,
 * in place, and NUL-terminate what is left.
 *
 * RFC 959 section 4.1 has clients precede a command sent during a transfer
 * (ABOR above all, also STAT and QUIT) with the Telnet "Interrupt Process"
 * and "Synch" signals. lwIP does not interpret the urgent pointer, so those
 * IAC bytes arrive inline: without this the server reads "\xFF\xF4\xFF\xF2ABOR"
 * and answers 502 to every abort a conforming client sends.
 *
 * Works on a length rather than a C string because an option byte may itself
 * be NUL — IAC DO BINARY is 0xFF 0xFD 0x00 — which would otherwise swallow the
 * command that follows it. Stray NULs are dropped for the same reason: FTP
 * commands are Telnet strings of printable characters, so a NUL is never data
 * here, and letting one truncate the line just loses the rest of it.
 *
 * @return the new end of the line, so the caller need not re-measure it.
 */
static char *ftp_strip_telnet(char *line, uint16_t len)
{
    char *dst = line;

    /* Compaction never outruns the read cursor, so this is safe in place. */
    for (uint16_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)line[i];
        if (ch == '\0') continue;
        if (ch != FTP_TELNET_IAC) {
            *dst++ = (char)ch;
            continue;
        }

        if (i + 1 >= len) break;   /* truncated sequence — drop the lone IAC */
        unsigned char cmd = (unsigned char)line[i + 1];
        if (cmd == FTP_TELNET_IAC) {   /* IAC IAC — an escaped 0xFF data byte */
            *dst++ = (char)FTP_TELNET_IAC;
            i++;
            continue;
        }
        /* Negotiation verbs are followed by an option byte; every other
         * command (IP, DM, ...) is the whole two-byte sequence. */
        i += (cmd >= FTP_TELNET_WILL && cmd <= FTP_TELNET_DONT) ? 2 : 1;
    }

    *dst = '\0';
    return dst;
}

/**
 * Parse one command from the command buffer and dispatch it.
 */
static void ftp_process_command(ftp_session_t *s)
{
    /* Drop Telnet control sequences (this NUL-terminates the line, at or
     * before cmd_buf[cmd_len]), then strip trailing CR/LF. */
    char *line = s->cmd_buf;
    char *end  = ftp_strip_telnet(line, s->cmd_len);
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

    const struct ftp_cmd *c = NULL;
    for (size_t i = 0; i < sizeof(ftp_commands) / sizeof(ftp_commands[0]); i++) {
        if (ftp_strcasecmp(cmd, ftp_commands[i].name) == 0) {
            c = &ftp_commands[i];
            break;
        }
    }

    /* Cancel a pending rename before dispatching, not after the auth gate:
     * USER, PASS and QUIT are handled pre-auth and would otherwise let an
     * RNFR survive a re-login and be completed by a later RNTO. */
    if (!c || !c->keeps_rnfr) {
        s->rnfr[0] = '\0';
    }

    if (c && c->pre_auth) {
        c->handler(s, arg);
        return;
    }
    if (s->auth != FTP_STATE_LOGGED_IN) {
        ftp_send_reply(s, "530 Please login with USER and PASS.\r\n");
        return;
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
            /* ftp_process_command() terminates the line at or before
             * cmd_buf[line_len] (before it, if Telnet sequences were
             * stripped); when more data follows in the buffer (pipelined
             * commands in one TCP segment) that byte belongs to the next
             * queued line, so it is preserved and restored either way rather
             * than risk being clobbered. */
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

    /* See ftp_data_accept() for why tcp_accepted() is not called here. */
    ftp_session_t *s = ftp_alloc_session();
    if (!s) {
        /* Say why before hanging up: an aborted connection just looks like a
         * network fault to the client. tcp_abort() would discard the reply
         * along with everything else queued, so close instead. */
        static const char busy[] = "421 Too many users, try again later.\r\n";
        if (tcp_sndbuf(pcb) >= sizeof(busy) - 1) {
            (void)tcp_write(pcb, busy, sizeof(busy) - 1, TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
        }
        return ftp_close_pcb(&pcb, 0) ? ERR_ABRT : ERR_OK;
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
    /* cfg is checked too: it is dereferenced right below, and a caller that
     * can hand over an unmounted lfs_t can hand over one with no config. */
    if (!lfs || !lfs->cfg) return ERR_ARG;
    if (lfs->cfg->cache_size > FTP_SERVER_FILE_CACHE_SIZE) return ERR_ARG;

    /* Make re-initialisation safe: drop any listener/sessions still around. */
    ftp_server_deinit();
    s_lfs = lfs;

    s_next_pasv_port = FTP_SERVER_PASV_PORT_MIN;

    err_t err = ERR_OK;
    s_listen_pcb = ftp_listen_on(FTP_SERVER_PORT, &err);
    if (!s_listen_pcb) return err;

    tcp_accept(s_listen_pcb, ftp_ctrl_accept);

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
