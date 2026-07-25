/**
 * @file mock_lwip.c
 * @brief Mock implementations of the lwIP functions used by ftp_server.c.
 *
 * Every function delegates to a hook pointer when set; otherwise a sane
 * default runs.  All state lives in static arrays — no heap.
 */
#include "mock_lwip.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  ip_addr_any — referenced via IP_ADDR_ANY / IP4_ADDR_ANY           */
/* ------------------------------------------------------------------ */

const ip_addr_t ip_addr_any = { 0 };

/* ------------------------------------------------------------------ */
/*  Hook pointers (all default to NULL)                               */
/* ------------------------------------------------------------------ */

struct tcp_pcb *(*mock_tcp_new_fn)(void)                                       = NULL;
void  (*mock_tcp_arg_fn)(struct tcp_pcb *, void *)                             = NULL;
void  (*mock_tcp_recv_fn)(struct tcp_pcb *, tcp_recv_fn)                       = NULL;
void  (*mock_tcp_sent_fn)(struct tcp_pcb *, tcp_sent_fn)                       = NULL;
void  (*mock_tcp_err_fn)(struct tcp_pcb *, tcp_err_fn)                         = NULL;
void  (*mock_tcp_accept_fn)(struct tcp_pcb *, tcp_accept_fn)                   = NULL;
void  (*mock_tcp_poll_fn)(struct tcp_pcb *, tcp_poll_fn, u8_t)                 = NULL;
err_t (*mock_tcp_bind_fn)(struct tcp_pcb *, const ip_addr_t *, u16_t)          = NULL;
struct tcp_pcb *(*mock_tcp_listen_with_backlog_fn)(struct tcp_pcb *, u8_t)     = NULL;
err_t (*mock_tcp_connect_fn)(struct tcp_pcb *, const ip_addr_t *, u16_t,
                             tcp_connected_fn)                                 = NULL;
void  (*mock_tcp_recved_fn)(struct tcp_pcb *, u16_t)                           = NULL;
err_t (*mock_tcp_write_fn)(struct tcp_pcb *, const void *, u16_t, u8_t)        = NULL;
err_t (*mock_tcp_output_fn)(struct tcp_pcb *)                                  = NULL;
err_t (*mock_tcp_close_fn)(struct tcp_pcb *)                                   = NULL;
void  (*mock_tcp_abort_fn)(struct tcp_pcb *)                                   = NULL;
err_t (*mock_tcp_tcp_get_tcp_addrinfo_fn)(struct tcp_pcb *, int,
                                          ip_addr_t *, u16_t *)               = NULL;
u8_t  (*mock_pbuf_free_fn)(struct pbuf *)                                      = NULL;
u16_t (*mock_pbuf_copy_partial_fn)(const struct pbuf *, void *, u16_t, u16_t)  = NULL;

/* ------------------------------------------------------------------ */
/*  PCB pool                                                          */
/* ------------------------------------------------------------------ */

static struct tcp_pcb s_pcbs[MOCK_TCP_MAX_PCBS];
static int            s_pcb_next;

/* ------------------------------------------------------------------ */
/*  Capture buffer                                                    */
/* ------------------------------------------------------------------ */

char   mock_tcp_write_buf[MOCK_TCP_WRITE_CAPTURE_SIZE];
u16_t  mock_tcp_write_len;

/* ------------------------------------------------------------------ */
/*  Callback capture arrays                                           */
/* ------------------------------------------------------------------ */

void          *mock_tcp_cb_arg[MOCK_TCP_MAX_PCBS];
tcp_accept_fn  mock_tcp_cb_accept[MOCK_TCP_MAX_PCBS];
tcp_recv_fn    mock_tcp_cb_recv[MOCK_TCP_MAX_PCBS];
tcp_sent_fn    mock_tcp_cb_sent[MOCK_TCP_MAX_PCBS];
tcp_err_fn     mock_tcp_cb_err[MOCK_TCP_MAX_PCBS];
tcp_poll_fn    mock_tcp_cb_poll[MOCK_TCP_MAX_PCBS];

/* ------------------------------------------------------------------ */
/*  Helper                                                            */
/* ------------------------------------------------------------------ */

int mock_tcp_pcb_index(const struct tcp_pcb *pcb)
{
    if (!pcb) return -1;
    ptrdiff_t idx = pcb - s_pcbs;
    if (idx < 0 || idx >= MOCK_TCP_MAX_PCBS) return -1;
    return (int)idx;
}

/* ------------------------------------------------------------------ */
/*  Reset                                                             */
/* ------------------------------------------------------------------ */

void mock_lwip_reset(void)
{
    /* Hooks */
    mock_tcp_new_fn                    = NULL;
    mock_tcp_arg_fn                    = NULL;
    mock_tcp_recv_fn                   = NULL;
    mock_tcp_sent_fn                   = NULL;
    mock_tcp_err_fn                    = NULL;
    mock_tcp_accept_fn                 = NULL;
    mock_tcp_poll_fn                   = NULL;
    mock_tcp_bind_fn                   = NULL;
    mock_tcp_listen_with_backlog_fn    = NULL;
    mock_tcp_connect_fn                = NULL;
    mock_tcp_recved_fn                 = NULL;
    mock_tcp_write_fn                  = NULL;
    mock_tcp_output_fn                 = NULL;
    mock_tcp_close_fn                  = NULL;
    mock_tcp_abort_fn                  = NULL;
    mock_tcp_tcp_get_tcp_addrinfo_fn   = NULL;
    mock_pbuf_free_fn                  = NULL;
    mock_pbuf_copy_partial_fn          = NULL;

    /* PCB pool */
    memset(s_pcbs, 0, sizeof(s_pcbs));
    s_pcb_next = 0;

    /* Default send buffer — large enough that ftp_send_reply never fails. */
    for (int i = 0; i < MOCK_TCP_MAX_PCBS; i++) {
        s_pcbs[i].snd_buf = 4096;
    }

    /* Capture buffer */
    memset(mock_tcp_write_buf, 0, sizeof(mock_tcp_write_buf));
    mock_tcp_write_len = 0;

    /* Callback arrays */
    memset(mock_tcp_cb_arg,    0, sizeof(mock_tcp_cb_arg));
    memset(mock_tcp_cb_accept, 0, sizeof(mock_tcp_cb_accept));
    memset(mock_tcp_cb_recv,   0, sizeof(mock_tcp_cb_recv));
    memset(mock_tcp_cb_sent,   0, sizeof(mock_tcp_cb_sent));
    memset(mock_tcp_cb_err,    0, sizeof(mock_tcp_cb_err));
    memset(mock_tcp_cb_poll,   0, sizeof(mock_tcp_cb_poll));
}

/* ------------------------------------------------------------------ */
/*  tcp_new                                                           */
/* ------------------------------------------------------------------ */

struct tcp_pcb *tcp_new(void)
{
    if (mock_tcp_new_fn) return mock_tcp_new_fn();
    if (s_pcb_next >= MOCK_TCP_MAX_PCBS) return NULL;
    struct tcp_pcb *p = &s_pcbs[s_pcb_next++];
    return p;
}

/* ------------------------------------------------------------------ */
/*  Callback registration                                             */
/* ------------------------------------------------------------------ */

void tcp_arg(struct tcp_pcb *pcb, void *arg)
{
    if (mock_tcp_arg_fn) { mock_tcp_arg_fn(pcb, arg); return; }
    int i = mock_tcp_pcb_index(pcb);
    if (i >= 0) mock_tcp_cb_arg[i] = arg;
}

void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv)
{
    if (mock_tcp_recv_fn) { mock_tcp_recv_fn(pcb, recv); return; }
    int i = mock_tcp_pcb_index(pcb);
    if (i >= 0) mock_tcp_cb_recv[i] = recv;
}

void tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent)
{
    if (mock_tcp_sent_fn) { mock_tcp_sent_fn(pcb, sent); return; }
    int i = mock_tcp_pcb_index(pcb);
    if (i >= 0) mock_tcp_cb_sent[i] = sent;
}

void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err)
{
    if (mock_tcp_err_fn) { mock_tcp_err_fn(pcb, err); return; }
    int i = mock_tcp_pcb_index(pcb);
    if (i >= 0) mock_tcp_cb_err[i] = err;
}

void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept)
{
    if (mock_tcp_accept_fn) { mock_tcp_accept_fn(pcb, accept); return; }
    int i = mock_tcp_pcb_index(pcb);
    if (i >= 0) mock_tcp_cb_accept[i] = accept;
}

void tcp_poll(struct tcp_pcb *pcb, tcp_poll_fn poll, u8_t interval)
{
    if (mock_tcp_poll_fn) { mock_tcp_poll_fn(pcb, poll, interval); return; }
    (void)interval;
    int i = mock_tcp_pcb_index(pcb);
    if (i >= 0) mock_tcp_cb_poll[i] = poll;
}

/* ------------------------------------------------------------------ */
/*  Connection setup                                                  */
/* ------------------------------------------------------------------ */

err_t tcp_bind(struct tcp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port)
{
    if (mock_tcp_bind_fn) return mock_tcp_bind_fn(pcb, ipaddr, port);
    (void)pcb; (void)ipaddr; (void)port;
    return ERR_OK;
}

struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog)
{
    if (mock_tcp_listen_with_backlog_fn)
        return mock_tcp_listen_with_backlog_fn(pcb, backlog);
    (void)backlog;
    return pcb; /* return same PCB — good enough for the mock */
}

err_t tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *ipaddr,
                  u16_t port, tcp_connected_fn connected)
{
    if (mock_tcp_connect_fn)
        return mock_tcp_connect_fn(pcb, ipaddr, port, connected);
    (void)pcb; (void)ipaddr; (void)port; (void)connected;
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  Data transfer                                                     */
/* ------------------------------------------------------------------ */

void tcp_recved(struct tcp_pcb *pcb, u16_t len)
{
    if (mock_tcp_recved_fn) { mock_tcp_recved_fn(pcb, len); return; }
    (void)pcb; (void)len;
}

err_t tcp_write(struct tcp_pcb *pcb, const void *dataptr,
                u16_t len, u8_t apiflags)
{
    if (mock_tcp_write_fn)
        return mock_tcp_write_fn(pcb, dataptr, len, apiflags);

    /* Default: append to capture buffer. */
    (void)pcb; (void)apiflags;
    u16_t room = (u16_t)(MOCK_TCP_WRITE_CAPTURE_SIZE - mock_tcp_write_len);
    u16_t copy = (len < room) ? len : room;
    if (copy > 0) {
        memcpy(mock_tcp_write_buf + mock_tcp_write_len, dataptr, copy);
        mock_tcp_write_len += copy;
    }
    return ERR_OK;
}

err_t tcp_output(struct tcp_pcb *pcb)
{
    if (mock_tcp_output_fn) return mock_tcp_output_fn(pcb);
    (void)pcb;
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  Close / abort                                                     */
/* ------------------------------------------------------------------ */

err_t tcp_close(struct tcp_pcb *pcb)
{
    if (mock_tcp_close_fn) return mock_tcp_close_fn(pcb);
    (void)pcb;
    return ERR_OK;
}

void tcp_abort(struct tcp_pcb *pcb)
{
    if (mock_tcp_abort_fn) { mock_tcp_abort_fn(pcb); return; }
    (void)pcb;
}

/* ------------------------------------------------------------------ */
/*  Address info                                                      */
/* ------------------------------------------------------------------ */

err_t tcp_tcp_get_tcp_addrinfo(struct tcp_pcb *pcb, int local,
                               ip_addr_t *addr, u16_t *port)
{
    if (mock_tcp_tcp_get_tcp_addrinfo_fn)
        return mock_tcp_tcp_get_tcp_addrinfo_fn(pcb, local, addr, port);
    /* Default: return 192.168.1.1:21. */
    (void)pcb; (void)local;
    if (addr) IP4_ADDR(addr, 192,168,1,1);
    if (port) *port = 21;
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  pbuf helpers                                                      */
/* ------------------------------------------------------------------ */

u8_t pbuf_free(struct pbuf *p)
{
    if (mock_pbuf_free_fn) return mock_pbuf_free_fn(p);
    (void)p;
    return 1; /* pretend we freed one pbuf */
}

u16_t pbuf_copy_partial(const struct pbuf *p, void *dataptr,
                        u16_t len, u16_t offset)
{
    if (mock_pbuf_copy_partial_fn)
        return mock_pbuf_copy_partial_fn(p, dataptr, len, offset);

    /* Default: walk the chain and copy bytes. */
    u16_t copied = 0;
    const struct pbuf *q = p;
    while (q && len > 0) {
        if (offset >= q->len) {
            offset -= q->len;
            q = q->next;
            continue;
        }
        u16_t avail = (u16_t)(q->len - offset);
        u16_t chunk = (avail < len) ? avail : len;
        memcpy((char *)dataptr + copied, (const char *)q->payload + offset, chunk);
        copied += chunk;
        len    -= chunk;
        offset  = 0;
        q = q->next;
    }
    return copied;
}
