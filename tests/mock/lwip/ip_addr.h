/**
 * @file ip_addr.h
 * @brief Mock lwIP IP address layer for unit testing.
 *
 * Models lwIP's *dual-stack* ip_addr_t — a tagged union — rather than the
 * IPv4-only simplification where ip_addr_t is just ip4_addr_t.
 *
 * ftp_server.c is a library and cannot assume LWIP_IPV6 == 0 in the projects
 * that build it, and the difference is not cosmetic: in a dual-stack lwIP,
 * ip_addr_get_ip4_u32() flattens *every* IPv6 address to 0, so comparing peers
 * through it makes any two IPv6 hosts look like the same host. With a v4-only
 * ip_addr_t that class of bug is unrepresentable and the RFC 2577 peer checks
 * in ftp_data_accept()/cmd_port() would pass no matter how they were written.
 *
 * Everything still defaults to IPADDR_TYPE_V4, so IPv4 tests are unaffected.
 */
#ifndef MOCK_LWIP_IP_ADDR_H
#define MOCK_LWIP_IP_ADDR_H

#include "lwip/ip4_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPADDR_TYPE_V4 0
#define IPADDR_TYPE_V6 6

/** Minimal stand-in for lwIP's ip6_addr_t (no zone support needed here). */
struct ip6_addr {
    u32_t addr[4];
};

typedef struct ip6_addr ip6_addr_t;

/* Laid out like the real dual-stack ip_addr_t: union first, then the tag. */
typedef struct ip_addr {
    union {
        ip6_addr_t ip6;
        ip4_addr_t ip4;
    } u_addr;
    u8_t type;
} ip_addr_t;

#define ip_2_ip4(ipaddr)  (&((ipaddr)->u_addr.ip4))
#define ip_2_ip6(ipaddr)  (&((ipaddr)->u_addr.ip6))

#define IP_GET_TYPE(ipaddr)              ((ipaddr)->type)
#define IP_SET_TYPE_VAL(ipaddr, iptype)  do { (ipaddr).type = (u8_t)(iptype); } while (0)
#define IP_SET_TYPE(ipaddr, iptype) \
    do { if (ipaddr) { (ipaddr)->type = (u8_t)(iptype); } } while (0)

#define IP_IS_V4_VAL(ipaddr)  ((ipaddr).type == IPADDR_TYPE_V4)
#define IP_IS_V6_VAL(ipaddr)  ((ipaddr).type == IPADDR_TYPE_V6)
#define IP_IS_V4(ipaddr)      (((ipaddr) == NULL) || IP_IS_V4_VAL(*(ipaddr)))
#define IP_IS_V6(ipaddr)      (((ipaddr) != NULL) && IP_IS_V6_VAL(*(ipaddr)))

#define IP_ADDR4(ipaddr, a, b, c, d)                 \
    do {                                             \
        IP4_ADDR(ip_2_ip4(ipaddr), a, b, c, d);      \
        IP_SET_TYPE(ipaddr, IPADDR_TYPE_V4);         \
    } while (0)

/** Set @p ipaddr to an IPv6 address built from four 32-bit words. */
#define IP_ADDR6(ipaddr, w0, w1, w2, w3)             \
    do {                                             \
        ip_2_ip6(ipaddr)->addr[0] = (u32_t)(w0);     \
        ip_2_ip6(ipaddr)->addr[1] = (u32_t)(w1);     \
        ip_2_ip6(ipaddr)->addr[2] = (u32_t)(w2);     \
        ip_2_ip6(ipaddr)->addr[3] = (u32_t)(w3);     \
        IP_SET_TYPE(ipaddr, IPADDR_TYPE_V6);         \
    } while (0)

/* Mirrors lwIP exactly, IPv6-flattens-to-zero behaviour included: this is the
 * trap ftp_server.c must not fall into, so the mock must not paper over it. */
#define ip_addr_get_ip4_u32(ipaddr) \
    (((ipaddr) && IP_IS_V4(ipaddr)) ? ip4_addr_get_u32(ip_2_ip4(ipaddr)) : 0)

#define ip4_addr_eq(a1, a2) ((a1)->addr == (a2)->addr)
#define ip6_addr_eq(a1, a2) ((a1)->addr[0] == (a2)->addr[0] && \
                             (a1)->addr[1] == (a2)->addr[1] && \
                             (a1)->addr[2] == (a2)->addr[2] && \
                             (a1)->addr[3] == (a2)->addr[3])

/* Type-aware, like lwIP's: addresses of different families never compare
 * equal. ip_addr_cmp is the spelling that exists across lwIP versions
 * (ip_addr_eq was introduced in 2.2.0 and ip_addr_cmp kept as its alias). */
#define ip_addr_eq(a1, a2)                                        \
    ((IP_GET_TYPE(a1) != IP_GET_TYPE(a2)) ? 0                     \
     : (IP_IS_V6_VAL(*(a1)) ? ip6_addr_eq(ip_2_ip6(a1), ip_2_ip6(a2)) \
                            : ip4_addr_eq(ip_2_ip4(a1), ip_2_ip4(a2))))
#define ip_addr_cmp(a1, a2) ip_addr_eq((a1), (a2))

/* Global "any" address (defined in mock_lwip.c). */
extern const ip_addr_t ip_addr_any;
#define IP4_ADDR_ANY   (&ip_addr_any)
#define IP_ADDR_ANY    IP4_ADDR_ANY

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LWIP_IP_ADDR_H */
