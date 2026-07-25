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

/** Session state machine. */
typedef enum {
    FTP_STATE_IDLE,          /**< waiting for commands */
    FTP_STATE_WAIT_USER,     /**< sent greeting, expect USER */
    FTP_STATE_WAIT_PASS,     /**< got USER, expect PASS */
    FTP_STATE_LOGGED_IN,     /**< authenticated, accept commands */
} ftp_auth_state_t;

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

    /* RETR: name and size reported in the "150" reply */
    lfs_soff_t            retr_size;

    uint16_t              cmd_len;
    uint16_t              port_port;

    /* Remaining bytes to flush for current chunk (RETR/LIST) */
    uint16_t              data_pending;
    uint16_t              data_offset;

    /* Idle timeout poll counter */
    uint16_t              idle_polls;

    uint8_t               in_use;

    /* Transfer type: 'A' (ASCII) or 'I' (binary) */
    char                  type;

    uint8_t               port_active; /**< 1 = use active mode */
    uint8_t               file_open;
    uint8_t               dir_open;

    /* STOR: flag that we are receiving data */
    uint8_t               stor_active;

    char                  cmd_buf[FTP_SERVER_CMD_BUF_SIZE];

    /* Working directory (always starts and ends with '/') */
    char                  cwd[FTP_SERVER_PATH_MAX];

    /* Pending RNFR path (set by RNFR, consumed by RNTO) */
    char                  rnfr[FTP_SERVER_PATH_MAX];

    char                  retr_name[FTP_SERVER_PATH_MAX];

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
        strncpy(out, s->cwd, FTP_SERVER_PATH_MAX - 1);
        out[FTP_SERVER_PATH_MAX - 1] = '\0';
        return 0;
    }

    char tmp[FTP_SERVER_PATH_MAX];
    if (arg[0] == '/') {
        /* Absolute path */
        strncpy(tmp, arg, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
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
            s->type   = 'A';
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
    if (tcp_sndbuf(s->ctrl_pcb) >= len) {
        tcp_write(s->ctrl_pcb, msg, len, TCP_WRITE_FLAG_COPY);
        tcp_output(s->ctrl_pcb);
    } else {
        /* Cannot deliver reply — tear down the session. */
        ftp_close_session(s);
    }
}

/* ------------------------------------------------------------------ */
/*  Close helpers                                                     */
/* ------------------------------------------------------------------ */

static void ftp_close_data(ftp_session_t *s)
{
    if (s->data_pcb) {
        tcp_arg(s->data_pcb, NULL);
        tcp_recv(s->data_pcb, NULL);
        tcp_sent(s->data_pcb, NULL);
        tcp_err(s->data_pcb, NULL);
        if (tcp_close(s->data_pcb) != ERR_OK) {
            tcp_abort(s->data_pcb);
        }
        s->data_pcb = NULL;
    }
    if (s->pasv_listen_pcb) {
        tcp_arg(s->pasv_listen_pcb, NULL);
        tcp_accept(s->pasv_listen_pcb, NULL);
        if (tcp_close(s->pasv_listen_pcb) != ERR_OK) {
            tcp_abort(s->pasv_listen_pcb);
        }
        s->pasv_listen_pcb = NULL;
    }
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
    s->stor_active  = 0;
    /* RFC 959: PORT/PASV should be re-issued per transfer. Resetting here
     * is intentionally strict; some clients assume the last PORT persists.
     * If relaxed, also clear port_active in the PASV handler (already done)
     * to avoid stale PORT state when switching between active/passive mode. */
    s->port_active  = 0;
}

static void ftp_close_session(ftp_session_t *s)
{
    ftp_close_data(s);
    if (s->ctrl_pcb) {
        tcp_arg(s->ctrl_pcb, NULL);
        tcp_recv(s->ctrl_pcb, NULL);
        tcp_sent(s->ctrl_pcb, NULL);
        tcp_err(s->ctrl_pcb, NULL);
        tcp_poll(s->ctrl_pcb, NULL, 0);
        if (tcp_close(s->ctrl_pcb) != ERR_OK) {
            tcp_abort(s->ctrl_pcb);
        }
        s->ctrl_pcb = NULL;
    }
    s->rnfr[0] = '\0';
    s->in_use = 0;
}

/* ------------------------------------------------------------------ */
/*  Data transfer: send file or listing data                          */
/* ------------------------------------------------------------------ */

/**
 * Attempt to flush all pending bytes in data_buf[data_offset..) to the
 * data PCB. Returns 1 if fully flushed (data_pending == 0), or 0 if
 * blocked on send buffer space / a tcp_write error — the caller must
 * return and wait for the next tcp_sent or poll callback.
 */
static int ftp_send_pending(ftp_session_t *s)
{
    while (s->data_pending > 0) {
        uint16_t space = tcp_sndbuf(s->data_pcb);
        if (space == 0) return 0; /* wait for sent callback */
        uint16_t to_send = (s->data_pending < space) ? s->data_pending : space;
        err_t err = tcp_write(s->data_pcb, s->data_buf + s->data_offset,
                              to_send, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) return 0; /* back-pressure */
        tcp_output(s->data_pcb);
        s->data_offset  += to_send;
        s->data_pending -= to_send;
    }
    return 1;
}

/**
 * Fill data_buf with the next chunk for the session's current transfer
 * mode (RETR file data, or LIST/NLST directory entries — "." and ".."
 * are skipped). Returns the number of bytes filled, or -1 when there is
 * no more data (EOF, end of directory, or a formatting error).
 */
static int ftp_fill_next_chunk(ftp_session_t *s)
{
    if (s->data_mode == FTP_DATA_RETR) {
        lfs_ssize_t r = lfs_file_read(s_lfs, &s->file,
                                       s->data_buf, FTP_SERVER_DATA_BUF_SIZE);
        return (r <= 0) ? -1 : (int)r;
    }

    if (s->data_mode != FTP_DATA_LIST && s->data_mode != FTP_DATA_NLST) {
        return -1;
    }

    for (;;) {
        struct lfs_info info;
        int r = lfs_dir_read(s_lfs, &s->dir, &info);
        if (r <= 0) return -1; /* end of directory or error */

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
        if (n < 0) return -1;
        if (n > (int)FTP_SERVER_DATA_BUF_SIZE) n = (int)FTP_SERVER_DATA_BUF_SIZE;
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

    /* If we have pending bytes from a previous fill, try to send them first. */
    if (!ftp_send_pending(s)) return;

    /* Fill and send subsequent chunks until the transfer is exhausted. */
    for (;;) {
        int n = ftp_fill_next_chunk(s);
        if (n < 0) break;

        s->data_offset  = 0;
        s->data_pending = (uint16_t)n;
        if (!ftp_send_pending(s)) return;
    }

    /* Transfer complete — close data connection and inform client. */
    ftp_close_data(s);
    ftp_send_reply(s, "226 Transfer complete.\r\n");
}

/* ------------------------------------------------------------------ */
/*  Data connection: initiate active-mode connect                     */
/* ------------------------------------------------------------------ */

static err_t ftp_data_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    if (err != ERR_OK || !s) {
        if (pcb) tcp_abort(pcb);
        if (s) {
            s->data_pcb = NULL;
            ftp_send_reply(s, "425 Can't open data connection.\r\n");
        }
        return ERR_OK;
    }
    s->data_pcb = pcb;
    ftp_start_transfer(s);
    return ERR_OK;
}

/**
 * Called once the data connection is ready (passive accept or active connect).
 * Begins the actual data transfer.
 */
static void ftp_start_transfer(ftp_session_t *s)
{
    if (!s->data_pcb) return;

    if (s->data_mode == FTP_DATA_STOR) {
        s->stor_active = 1;
        ftp_send_reply(s, "150 Ok to send data.\r\n");
    } else if (s->data_mode == FTP_DATA_RETR) {
        char reply[FTP_SERVER_PATH_MAX + 64];
        snprintf(reply, sizeof(reply),
                 "150 Opening %s mode data connection for %s (%ld bytes).\r\n",
                 (s->type == 'I') ? "BINARY" : "ASCII",
                 s->retr_name, (long)s->retr_size);
        ftp_send_reply(s, reply);
        ftp_send_next_data(s);
    } else {
        ftp_send_reply(s, "150 Here comes the data.\r\n");
        ftp_send_next_data(s);
    }
}

/**
 * Open a data connection: either accept on PASV listener already set up,
 * or actively connect to the client's PORT address.
 * Returns 1 if async (will call ftp_start_transfer later), 0 if already ready.
 */
static int ftp_open_data_connection(ftp_session_t *s)
{
    if (s->data_pcb) {
        /* Already connected (PASV client connected early). */
        return 0;
    }

    if (s->port_active) {
        /* Active mode: connect to client. */
        struct tcp_pcb *pcb = tcp_new();
        if (!pcb) {
            ftp_send_reply(s, "425 Can't open data connection.\r\n");
            return -1;
        }
        tcp_arg(pcb, s);
        tcp_err(pcb, ftp_data_err);
        tcp_recv(pcb, ftp_data_recv);
        tcp_sent(pcb, ftp_data_sent);

        err_t err = tcp_connect(pcb, &s->port_addr, s->port_port,
                                ftp_data_connected);
        if (err != ERR_OK) {
            tcp_abort(pcb);
            ftp_send_reply(s, "425 Can't open data connection.\r\n");
            return -1;
        }
        /* Connection in progress; ftp_data_connected will call start_transfer. */
        return 1;
    }

    if (s->pasv_listen_pcb) {
        /* Passive mode listener exists but client hasn't connected yet.
         * The data_accept callback will trigger start_transfer. */
        return 1;
    }

    ftp_send_reply(s, "425 Use PASV or PORT first.\r\n");
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Data connection callbacks                                         */
/* ------------------------------------------------------------------ */

static err_t ftp_data_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    if (err != ERR_OK || !s) {
        if (pcb) tcp_abort(pcb);
        return ERR_OK;
    }

    s->data_pcb = pcb;
    tcp_arg(pcb, s);
    tcp_recv(pcb, ftp_data_recv);
    tcp_sent(pcb, ftp_data_sent);
    tcp_err(pcb, ftp_data_err);

    /* Close the listener — only one data connection per transfer. */
    if (s->pasv_listen_pcb) {
        tcp_arg(s->pasv_listen_pcb, NULL);
        tcp_accept(s->pasv_listen_pcb, NULL);
        tcp_close(s->pasv_listen_pcb);
        s->pasv_listen_pcb = NULL;
    }

    /* If a transfer is pending (command was already issued), start it. */
    if (s->data_mode != FTP_DATA_IDLE) {
        ftp_start_transfer(s);
    }

    return ERR_OK;
}

static err_t ftp_data_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)err;
    ftp_session_t *s = (ftp_session_t *)arg;
    if (!s) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }

    if (!p) {
        /* Client closed data connection — end of upload. */
        if (s->data_mode == FTP_DATA_STOR && s->file_open) {
            lfs_file_close(s_lfs, &s->file);
            s->file_open = 0;
        }
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_close(pcb);
        s->data_pcb    = NULL;
        s->stor_active = 0;
        s->data_mode   = FTP_DATA_IDLE;
        ftp_send_reply(s, "226 Transfer complete.\r\n");
        return ERR_OK;
    }

    if (s->data_mode == FTP_DATA_STOR && s->file_open) {
        /* Write received data to LittleFS file. */
        struct pbuf *q;
        for (q = p; q != NULL; q = q->next) {
            lfs_ssize_t w = lfs_file_write(s_lfs, &s->file,
                                           q->payload, q->len);
            if (w < 0) {
                tcp_recved(pcb, p->tot_len);
                pbuf_free(p);
                ftp_close_data(s);
                ftp_send_reply(s, "452 Insufficient storage space.\r\n");
                return ERR_OK;
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

    /* Previous chunk ACK'd — send more data. */
    if (s->data_mode == FTP_DATA_RETR ||
        s->data_mode == FTP_DATA_LIST ||
        s->data_mode == FTP_DATA_NLST) {
        ftp_send_next_data(s);
    }

    return ERR_OK;
}

static void ftp_data_err(void *arg, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    (void)err;
    if (!s) return;

    /* PCB is already freed by lwIP before the error callback fires. */
    s->data_pcb = NULL;
    ftp_close_data(s);   /* handles pasv_listen, file, dir, all flags */
}

/* ------------------------------------------------------------------ */
/*  Command processing                                                */
/* ------------------------------------------------------------------ */

/** Signature for a post-auth command handler. */
typedef void (*ftp_cmd_fn)(ftp_session_t *s, const char *arg);

/** One entry in the post-auth command dispatch table. */
struct ftp_cmd {
    const char *name;
    ftp_cmd_fn  handler;
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
        strncpy(upper, arg, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
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
        s->type = 'I';
        ftp_send_reply(s, "200 Switching to Binary mode.\r\n");
    } else {
        ftp_send_reply(s, "504 ASCII transfer mode not supported, use binary.\r\n");
    }
}

static void cmd_pwd(ftp_session_t *s, const char *arg)
{
    (void)arg;
    char reply[FTP_SERVER_PATH_MAX + 64];
    snprintf(reply, sizeof(reply), "257 \"%s\" is the current directory.\r\n",
             s->cwd);
    ftp_send_reply(s, reply);
}

static void cmd_cwd(ftp_session_t *s, const char *arg)
{
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    struct lfs_info info;
    if (lfs_stat(s_lfs, path, &info) < 0 || info.type != LFS_TYPE_DIR) {
        ftp_send_reply(s, "550 Failed to change directory.\r\n");
        return;
    }
    strncpy(s->cwd, path, FTP_SERVER_PATH_MAX - 1);
    s->cwd[FTP_SERVER_PATH_MAX - 1] = '\0';
    ftp_send_reply(s, "250 Directory successfully changed.\r\n");
}

static void cmd_cdup(ftp_session_t *s, const char *arg)
{
    (void)arg;
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, "..", path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    strncpy(s->cwd, path, FTP_SERVER_PATH_MAX - 1);
    s->cwd[FTP_SERVER_PATH_MAX - 1] = '\0';
    ftp_send_reply(s, "250 Directory successfully changed.\r\n");
}

static void cmd_pasv(ftp_session_t *s, const char *arg)
{
    (void)arg;
    /* Close any previous data channel. */
    ftp_close_data(s);
    s->port_active = 0;

    struct tcp_pcb *lpcb = tcp_new();
    if (!lpcb) {
        ftp_send_reply(s, "421 Cannot create data socket.\r\n");
        return;
    }

    uint16_t port = 0;
    uint16_t range = FTP_SERVER_PASV_PORT_MAX - FTP_SERVER_PASV_PORT_MIN + 1;
    err_t err = ERR_USE;
    for (uint16_t tries = 0; tries < range; tries++) {
        port = ftp_next_pasv_port();
        err = tcp_bind(lpcb, IP_ADDR_ANY, port);
        if (err == ERR_OK) break;
    }
    if (err != ERR_OK) {
        tcp_close(lpcb);
        ftp_send_reply(s, "421 Cannot bind data socket.\r\n");
        return;
    }

    lpcb = tcp_listen(lpcb);
    if (!lpcb) {
        ftp_send_reply(s, "421 Cannot listen on data socket.\r\n");
        return;
    }

    s->pasv_listen_pcb = lpcb;
    tcp_arg(lpcb, s);
    tcp_accept(lpcb, ftp_data_accept);

    /* Build reply with the server's IP from the control connection. */
    ip_addr_t local_ip;
    u16_t local_port;
    tcp_tcp_get_tcp_addrinfo(s->ctrl_pcb, 1, &local_ip, &local_port);

    uint32_t ip4 = ip_addr_get_ip4_u32(&local_ip);
    uint8_t *ip  = (uint8_t *)&ip4;

    char reply[FTP_SERVER_PATH_MAX + 64];
    snprintf(reply, sizeof(reply),
             "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).\r\n",
             ip[0], ip[1], ip[2], ip[3],
             (unsigned)(port >> 8), (unsigned)(port & 0xff));
    ftp_send_reply(s, reply);
}

static void cmd_port(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Syntax error in parameters.\r\n");
        return;
    }
    ftp_close_data(s);

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
    unsigned int h1 = (unsigned int)fields[0], h2 = (unsigned int)fields[1];
    unsigned int h3 = (unsigned int)fields[2], h4 = (unsigned int)fields[3];
    unsigned int p1 = (unsigned int)fields[4], p2 = (unsigned int)fields[5];

    IP_ADDR4(&s->port_addr, h1, h2, h3, h4);
    s->port_port   = (uint16_t)((p1 << 8) | p2);
    s->port_active = 1;

    ftp_send_reply(s, "200 PORT command successful.\r\n");
}

static void ftp_do_list(ftp_session_t *s, const char *arg, ftp_data_mode_t mode)
{
    char path[FTP_SERVER_PATH_MAX];

    /* Skip common options like -la, -a, etc. */
    const char *list_arg = arg;
    if (list_arg && list_arg[0] == '-') {
        /* Skip past the option */
        while (*list_arg && *list_arg != ' ') list_arg++;
        while (*list_arg == ' ') list_arg++;
        if (*list_arg == '\0') list_arg = NULL;
    }

    if (ftp_resolve_path(s, list_arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }

    int rc = lfs_dir_open(s_lfs, &s->dir, path);
    if (rc < 0) {
        ftp_send_reply(s, "550 Failed to open directory.\r\n");
        return;
    }
    s->dir_open  = 1;
    s->data_mode = mode;

    int r = ftp_open_data_connection(s);
    if (r < 0) {
        /* Error already replied. */
        ftp_close_data(s);
        return;
    }
    if (r == 0) {
        /* Data connection already ready. */
        ftp_start_transfer(s);
    }
    /* else: async — callback will start transfer. */
}

static void cmd_list(ftp_session_t *s, const char *arg)
{
    ftp_do_list(s, arg, FTP_DATA_LIST);
}

static void cmd_nlst(ftp_session_t *s, const char *arg)
{
    ftp_do_list(s, arg, FTP_DATA_NLST);
}

static void cmd_retr(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Specify file name.\r\n");
        return;
    }
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    memset(&s->file_cfg, 0, sizeof(s->file_cfg));
    s->file_cfg.buffer = s->file_cache;
    int rc = lfs_file_opencfg(s_lfs, &s->file, path,
                              LFS_O_RDONLY, &s->file_cfg);
    if (rc < 0) {
        ftp_send_reply(s, "550 Failed to open file.\r\n");
        return;
    }
    s->file_open = 1;
    s->data_mode = FTP_DATA_RETR;
    s->retr_size = lfs_file_size(s_lfs, &s->file);
    strncpy(s->retr_name, arg, FTP_SERVER_PATH_MAX - 1);
    s->retr_name[FTP_SERVER_PATH_MAX - 1] = '\0';

    int r = ftp_open_data_connection(s);
    if (r < 0) {
        ftp_close_data(s);
        return;
    }
    if (r == 0) {
        ftp_start_transfer(s);
    }
}

static void cmd_stor(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Specify file name.\r\n");
        return;
    }
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    memset(&s->file_cfg, 0, sizeof(s->file_cfg));
    s->file_cfg.buffer = s->file_cache;
    int rc = lfs_file_opencfg(s_lfs, &s->file, path,
                              LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                              &s->file_cfg);
    if (rc < 0) {
        ftp_send_reply(s, "550 Failed to create file.\r\n");
        return;
    }
    s->file_open = 1;
    s->data_mode = FTP_DATA_STOR;

    int r = ftp_open_data_connection(s);
    if (r < 0) {
        ftp_close_data(s);
        return;
    }
    if (r == 0) {
        ftp_start_transfer(s);
    }
}

static void cmd_dele(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Specify file name.\r\n");
        return;
    }
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    if (lfs_remove(s_lfs, path) < 0) {
        ftp_send_reply(s, "550 Delete operation failed.\r\n");
    } else {
        ftp_send_reply(s, "250 Delete operation successful.\r\n");
    }
}

static void cmd_mkd(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Specify directory name.\r\n");
        return;
    }
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    if (lfs_mkdir(s_lfs, path) < 0) {
        ftp_send_reply(s, "550 Create directory failed.\r\n");
    } else {
        char reply[FTP_SERVER_PATH_MAX + 64];
        snprintf(reply, sizeof(reply),
                 "257 \"%s\" created.\r\n", path);
        ftp_send_reply(s, reply);
    }
}

static void cmd_rmd(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Specify directory name.\r\n");
        return;
    }
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    if (lfs_remove(s_lfs, path) < 0) {
        ftp_send_reply(s, "550 Remove directory failed.\r\n");
    } else {
        ftp_send_reply(s, "250 Remove directory successful.\r\n");
    }
}

static void cmd_rnfr(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Specify file name.\r\n");
        return;
    }
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    /* Verify the source exists. */
    struct lfs_info info;
    if (lfs_stat(s_lfs, path, &info) < 0) {
        ftp_send_reply(s, "550 File not found.\r\n");
        return;
    }
    strncpy(s->rnfr, path, FTP_SERVER_PATH_MAX - 1);
    s->rnfr[FTP_SERVER_PATH_MAX - 1] = '\0';
    ftp_send_reply(s, "350 Ready for RNTO.\r\n");
}

static void cmd_rnto(ftp_session_t *s, const char *arg)
{
    if (!arg) {
        ftp_send_reply(s, "501 Specify file name.\r\n");
        return;
    }
    if (s->rnfr[0] == '\0') {
        ftp_send_reply(s, "503 RNFR required first.\r\n");
        return;
    }
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
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
    if (!arg) {
        ftp_send_reply(s, "501 Specify file name.\r\n");
        return;
    }
    char path[FTP_SERVER_PATH_MAX];
    if (ftp_resolve_path(s, arg, path) < 0) {
        ftp_send_reply(s, "550 Path too long.\r\n");
        return;
    }
    struct lfs_info info;
    if (lfs_stat(s_lfs, path, &info) < 0 || info.type != LFS_TYPE_REG) {
        ftp_send_reply(s, "550 Could not get file size.\r\n");
        return;
    }
    char reply[FTP_SERVER_PATH_MAX + 64];
    snprintf(reply, sizeof(reply), "213 %lu\r\n",
             (unsigned long)info.size);
    ftp_send_reply(s, reply);
}

static void cmd_abor(ftp_session_t *s, const char *arg)
{
    (void)arg;
    ftp_close_data(s);
    ftp_send_reply(s, "226 ABOR command successful.\r\n");
}

/** Post-auth command dispatch table. */
static const struct ftp_cmd ftp_commands[] = {
    {"SYST", cmd_syst},
    {"FEAT", cmd_feat},
    {"OPTS", cmd_opts},
    {"NOOP", cmd_noop},
    {"TYPE", cmd_type},
    {"PWD",  cmd_pwd},
    {"XPWD", cmd_pwd},
    {"CWD",  cmd_cwd},
    {"XCWD", cmd_cwd},
    {"CDUP", cmd_cdup},
    {"XCUP", cmd_cdup},
    {"PASV", cmd_pasv},
    {"PORT", cmd_port},
    {"LIST", cmd_list},
    {"NLST", cmd_nlst},
    {"RETR", cmd_retr},
    {"STOR", cmd_stor},
    {"DELE", cmd_dele},
    {"MKD",  cmd_mkd},
    {"XMKD", cmd_mkd},
    {"RMD",  cmd_rmd},
    {"XRMD", cmd_rmd},
    {"RNFR", cmd_rnfr},
    {"RNTO", cmd_rnto},
    {"SIZE", cmd_size},
    {"ABOR", cmd_abor},
    {NULL,   NULL},
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

    /* -------------------------------------------------------------- */
    /*  Commands allowed before authentication                        */
    /* -------------------------------------------------------------- */

    if (strcmp(cmd, "USER") == 0) {
        const char *need_user = FTP_SERVER_USER;
        if (need_user == NULL) {
            /* No auth required. */
            s->auth = FTP_STATE_LOGGED_IN;
            ftp_send_reply(s, "230 Login successful.\r\n");
        } else {
            if (arg && strcmp(arg, need_user) == 0) {
                const char *need_pass = FTP_SERVER_PASS;
                if (need_pass == NULL) {
                    s->auth = FTP_STATE_LOGGED_IN;
                    ftp_send_reply(s, "230 Login successful.\r\n");
                } else {
                    s->auth = FTP_STATE_WAIT_PASS;
                    ftp_send_reply(s, "331 Please specify the password.\r\n");
                }
            } else {
                ftp_send_reply(s, "530 Login incorrect.\r\n");
            }
        }
        return;
    }

    if (strcmp(cmd, "PASS") == 0) {
        if (s->auth != FTP_STATE_WAIT_PASS) {
            ftp_send_reply(s, "503 Login with USER first.\r\n");
            return;
        }
        const char *need_pass = FTP_SERVER_PASS;
        if (need_pass == NULL || (arg && strcmp(arg, need_pass) == 0)) {
            s->auth = FTP_STATE_LOGGED_IN;
            ftp_send_reply(s, "230 Login successful.\r\n");
        } else {
            s->auth = FTP_STATE_WAIT_USER;
            ftp_send_reply(s, "530 Login incorrect.\r\n");
        }
        return;
    }

    if (strcmp(cmd, "QUIT") == 0) {
        ftp_send_reply(s, "221 Goodbye.\r\n");
        ftp_close_session(s);
        return;
    }

    /* All other commands require authentication. */
    if (s->auth != FTP_STATE_LOGGED_IN) {
        ftp_send_reply(s, "530 Please login with USER and PASS.\r\n");
        return;
    }

    /* -------------------------------------------------------------- */
    /*  Post-auth commands — dispatch table                           */
    /* -------------------------------------------------------------- */

    for (const struct ftp_cmd *c = ftp_commands; c->name; c++) {
        if (strcmp(cmd, c->name) == 0) {
            c->handler(s, arg);
            return;
        }
    }

    /* -------------------------------------------------------------- */
    /*  Unknown command                                               */
    /* -------------------------------------------------------------- */

    ftp_send_reply(s, "502 Command not implemented.\r\n");
}

/* ------------------------------------------------------------------ */
/*  Control connection callbacks                                      */
/* ------------------------------------------------------------------ */

static err_t ftp_ctrl_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    ftp_session_t *s = (ftp_session_t *)arg;
    (void)err;

    if (!s) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }

    if (!p) {
        /* Client disconnected. */
        ftp_close_session(s);
        return ERR_OK;
    }

    /* Reset idle timeout on any received data. */
    s->idle_polls = 0;

    /* Append the entire pbuf payload into the command buffer. */
    uint16_t total = p->tot_len;
    uint16_t room  = (uint16_t)(FTP_SERVER_CMD_BUF_SIZE - 1 - s->cmd_len);
    uint16_t copy  = (total < room) ? total : room;

    if (copy > 0) {
        pbuf_copy_partial(p, s->cmd_buf + s->cmd_len, copy, 0);
        s->cmd_len += copy;
    }

    tcp_recved(pcb, total);
    pbuf_free(p);

    /* Process all complete lines in the buffer. A complete line ends with
     * '\n'.  We may have multiple commands in one TCP segment. */
    while (s->cmd_len > 0) {
        char *nl = (char *)memchr(s->cmd_buf, '\n', s->cmd_len);
        if (!nl) break; /* partial line — wait for more data */

        /* Length of this line including the '\n'. */
        uint16_t line_len = (uint16_t)(nl - s->cmd_buf + 1);

        /* Save total buffered length, then tell process_command the line
         * length so it can NUL-terminate correctly. */
        uint16_t saved_total = s->cmd_len;
        s->cmd_len = line_len;

        ftp_process_command(s);

        /* Session may have been freed by QUIT. */
        if (!s->in_use) return ERR_OK;

        /* Shift remaining bytes to the front of the buffer. */
        uint16_t remaining = saved_total - line_len;
        if (remaining > 0) {
            memmove(s->cmd_buf, s->cmd_buf + line_len, remaining);
        }
        s->cmd_len = remaining;
    }

    /* If buffer is full without a complete line, discard it. */
    if (s->cmd_len >= FTP_SERVER_CMD_BUF_SIZE - 1) {
        s->cmd_len = 0;
        ftp_send_reply(s, "500 Command line too long.\r\n");
    }

    return ERR_OK;
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

    s->idle_polls++;
    if (s->idle_polls >= FTP_SERVER_IDLE_TIMEOUT_POLLS) {
        ftp_close_session(s);
        return ERR_OK;
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
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

err_t ftp_server_init(lfs_t *lfs)
{
    if (!lfs) return ERR_ARG;
    if (lfs->cfg->cache_size > FTP_SERVER_FILE_CACHE_SIZE) return ERR_ARG;
    s_lfs = lfs;

    memset(s_sessions, 0, sizeof(s_sessions));
    s_next_pasv_port = FTP_SERVER_PASV_PORT_MIN;

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return ERR_MEM;

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, FTP_SERVER_PORT);
    if (err != ERR_OK) {
        tcp_close(pcb);
        return err;
    }

    pcb = tcp_listen(pcb);
    if (!pcb) return ERR_MEM;

    s_listen_pcb = pcb;
    tcp_accept(pcb, ftp_ctrl_accept);

    return ERR_OK;
}

void ftp_server_deinit(void)
{
    for (int i = 0; i < FTP_SERVER_MAX_CLIENTS; i++) {
        if (s_sessions[i].in_use) {
            ftp_close_session(&s_sessions[i]);
        }
    }

    if (s_listen_pcb) {
        tcp_close(s_listen_pcb);
        s_listen_pcb = NULL;
    }

    s_lfs = NULL;
}
