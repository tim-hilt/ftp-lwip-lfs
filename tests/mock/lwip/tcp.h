/**
 * @file tcp.h
 * @brief Mock lwIP TCP API for unit testing.
 *
 * Provides minimal struct definitions and function declarations that shadow
 * the real lwip/tcp.h.  ftp_server.c never reaches into tcp_pcb internals
 * except via tcp_sndbuf() — which the real lwIP defines as a macro reading
 * pcb->snd_buf.  The mock keeps that field so the macro works.
 */
#ifndef MOCK_LWIP_TCP_H
#define MOCK_LWIP_TCP_H

#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — the callback typedefs reference this. */
struct tcp_pcb;

/* ------------------------------------------------------------------ */
/*  Callback typedefs (match real lwIP signatures)                    */
/* ------------------------------------------------------------------ */

typedef err_t (*tcp_accept_fn)(void *arg, struct tcp_pcb *newpcb, err_t err);
typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *tpcb,
                             struct pbuf *p, err_t err);
typedef err_t (*tcp_sent_fn)(void *arg, struct tcp_pcb *tpcb, u16_t len);
typedef err_t (*tcp_poll_fn)(void *arg, struct tcp_pcb *tpcb);
typedef void  (*tcp_err_fn)(void *arg, err_t err);
typedef err_t (*tcp_connected_fn)(void *arg, struct tcp_pcb *tpcb, err_t err);

/* ------------------------------------------------------------------ */
/*  Minimal tcp_pcb — only the fields ftp_server.c touches via macros */
/* ------------------------------------------------------------------ */

struct tcp_pcb {
    u16_t snd_buf;       /* tcp_sndbuf reads this */
    u16_t snd_queuelen;
};

/* In the real lwIP tcp_sndbuf is a macro — replicate that. */
#define TCPWND16(x) (x)
#define tcp_sndbuf(pcb)  (TCPWND16((pcb)->snd_buf))

#define TCP_WRITE_FLAG_COPY 0x01
#define TCP_WRITE_FLAG_MORE 0x02

#define TCP_DEFAULT_LISTEN_BACKLOG 0xff

/* ------------------------------------------------------------------ */
/*  Function declarations (implemented in mock_lwip.c)                */
/* ------------------------------------------------------------------ */

struct tcp_pcb *tcp_new(void);

void  tcp_arg    (struct tcp_pcb *pcb, void *arg);
void  tcp_recv   (struct tcp_pcb *pcb, tcp_recv_fn recv);
void  tcp_sent   (struct tcp_pcb *pcb, tcp_sent_fn sent);
void  tcp_err    (struct tcp_pcb *pcb, tcp_err_fn err);
void  tcp_accept (struct tcp_pcb *pcb, tcp_accept_fn accept);
void  tcp_poll   (struct tcp_pcb *pcb, tcp_poll_fn poll, u8_t interval);

err_t tcp_bind   (struct tcp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port);

struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog);
#define tcp_listen(pcb) tcp_listen_with_backlog((pcb), TCP_DEFAULT_LISTEN_BACKLOG)

/**
 * Real lwIP defines this as a macro over the *listening* PCB that gives the
 * backlog slot back. The mock makes it a function so tests can observe that
 * every accept callback honours the contract.
 */
void tcp_accepted(struct tcp_pcb *lpcb);

err_t tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *ipaddr,
                  u16_t port, tcp_connected_fn connected);

void  tcp_recved (struct tcp_pcb *pcb, u16_t len);
err_t tcp_write  (struct tcp_pcb *pcb, const void *dataptr,
                  u16_t len, u8_t apiflags);
err_t tcp_output (struct tcp_pcb *pcb);
err_t tcp_close  (struct tcp_pcb *pcb);
void  tcp_abort  (struct tcp_pcb *pcb);

err_t tcp_tcp_get_tcp_addrinfo(struct tcp_pcb *pcb, int local,
                               ip_addr_t *addr, u16_t *port);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LWIP_TCP_H */
