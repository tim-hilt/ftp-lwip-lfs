/**
 * @file mock_lwip.h
 * @brief Test-side control surface for the lwIP mock.
 *
 * Include this from your Catch2 test files (inside `extern "C" {}` if
 * compiling as C++).  It exposes:
 *
 *  - Per-function hook pointers: set one to override a single lwIP call's
 *    behaviour for the duration of a test.  NULL → default behaviour.
 *
 *  - mock_lwip_reset(): clears all hooks and internal state.  Call from a
 *    Catch2 test fixture or a global `beforeEach` equivalent.
 *
 *  - A capture buffer for tcp_write payloads so tests can inspect what
 *    ftp_server.c tried to send on the control/data connection.
 */
#ifndef MOCK_LWIP_H
#define MOCK_LWIP_H

#include "lwip/tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  PCB pool                                                          */
/* ------------------------------------------------------------------ */

/** Maximum number of fake PCBs the mock can hand out (no heap). */
#define MOCK_TCP_MAX_PCBS 16

/* ------------------------------------------------------------------ */
/*  Per-function hooks                                                */
/*                                                                    */
/*  Each pointer defaults to NULL → the mock uses a built-in default. */
/*  Set the pointer in your test to inject custom behaviour.          */
/* ------------------------------------------------------------------ */

extern struct tcp_pcb *(*mock_tcp_new_fn)(void);
extern void  (*mock_tcp_arg_fn)(struct tcp_pcb *pcb, void *arg);
extern void  (*mock_tcp_recv_fn)(struct tcp_pcb *pcb, tcp_recv_fn recv);
extern void  (*mock_tcp_sent_fn)(struct tcp_pcb *pcb, tcp_sent_fn sent);
extern void  (*mock_tcp_err_fn)(struct tcp_pcb *pcb, tcp_err_fn err);
extern void  (*mock_tcp_accept_fn)(struct tcp_pcb *pcb, tcp_accept_fn accept);
extern void  (*mock_tcp_poll_fn)(struct tcp_pcb *pcb, tcp_poll_fn poll,
                                 u8_t interval);
extern err_t (*mock_tcp_bind_fn)(struct tcp_pcb *pcb, const ip_addr_t *ipaddr,
                                 u16_t port);
extern struct tcp_pcb *(*mock_tcp_listen_with_backlog_fn)(struct tcp_pcb *pcb,
                                                          u8_t backlog);
extern err_t (*mock_tcp_connect_fn)(struct tcp_pcb *pcb,
                                    const ip_addr_t *ipaddr, u16_t port,
                                    tcp_connected_fn connected);
extern void  (*mock_tcp_recved_fn)(struct tcp_pcb *pcb, u16_t len);
extern err_t (*mock_tcp_write_fn)(struct tcp_pcb *pcb, const void *dataptr,
                                  u16_t len, u8_t apiflags);
extern err_t (*mock_tcp_output_fn)(struct tcp_pcb *pcb);
extern err_t (*mock_tcp_close_fn)(struct tcp_pcb *pcb);
extern void  (*mock_tcp_abort_fn)(struct tcp_pcb *pcb);
extern err_t (*mock_tcp_tcp_get_tcp_addrinfo_fn)(struct tcp_pcb *pcb,
                                                  int local,
                                                  ip_addr_t *addr,
                                                  u16_t *port);

extern u8_t  (*mock_pbuf_free_fn)(struct pbuf *p);
extern u16_t (*mock_pbuf_copy_partial_fn)(const struct pbuf *p, void *dataptr,
                                          u16_t len, u16_t offset);

/* ------------------------------------------------------------------ */
/*  Capture buffer for tcp_write payloads                             */
/* ------------------------------------------------------------------ */

/** Size of the capture ring (bytes).  Increase if needed. */
#define MOCK_TCP_WRITE_CAPTURE_SIZE 4096

/** Raw capture buffer.  tcp_write's default implementation appends here. */
extern char   mock_tcp_write_buf[MOCK_TCP_WRITE_CAPTURE_SIZE];
/** Current write position in mock_tcp_write_buf. */
extern u16_t  mock_tcp_write_len;

/* ------------------------------------------------------------------ */
/*  Callback capture                                                  */
/*                                                                    */
/*  The default tcp_arg / tcp_recv / … implementations store the last */
/*  values so tests can retrieve (and invoke) the callbacks that      */
/*  ftp_server.c registered.                                          */
/* ------------------------------------------------------------------ */

/** Last callback arg set via tcp_arg for each PCB slot. */
extern void          *mock_tcp_cb_arg[MOCK_TCP_MAX_PCBS];
extern tcp_accept_fn  mock_tcp_cb_accept[MOCK_TCP_MAX_PCBS];
extern tcp_recv_fn    mock_tcp_cb_recv[MOCK_TCP_MAX_PCBS];
extern tcp_sent_fn    mock_tcp_cb_sent[MOCK_TCP_MAX_PCBS];
extern tcp_err_fn     mock_tcp_cb_err[MOCK_TCP_MAX_PCBS];
extern tcp_poll_fn    mock_tcp_cb_poll[MOCK_TCP_MAX_PCBS];

/** Return the pool-relative index of a PCB, or -1 if not from the pool. */
int mock_tcp_pcb_index(const struct tcp_pcb *pcb);

/* ------------------------------------------------------------------ */
/*  Reset                                                             */
/* ------------------------------------------------------------------ */

/**
 * Reset all mock state: hooks, PCB pool, capture buffer, callback arrays.
 * Call at the start of every test.
 */
void mock_lwip_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LWIP_H */
