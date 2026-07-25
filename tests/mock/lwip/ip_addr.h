/**
 * @file ip_addr.h
 * @brief Mock lwIP IP address layer for unit testing (IPv4-only).
 *
 * The production build has LWIP_IPV6 == 0, so ip_addr_t == ip4_addr_t.
 * This mock mirrors that simplification.
 */
#ifndef MOCK_LWIP_IP_ADDR_H
#define MOCK_LWIP_IP_ADDR_H

#include "lwip/ip4_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IPv4-only: ip_addr_t is just ip4_addr_t. */
typedef ip4_addr_t ip_addr_t;

/* Identity macros — no dual-stack indirection needed. */
#define ip_2_ip4(ipaddr)                (ipaddr)
#define IP_ADDR4(ipaddr, a,b,c,d)       IP4_ADDR(ipaddr, a,b,c,d)
#define ip_addr_get_ip4_u32(ipaddr)     ip4_addr_get_u32(ip_2_ip4(ipaddr))

#define IP_SET_TYPE_VAL(ipaddr, iptype)
#define IP_SET_TYPE(ipaddr, iptype)

/* Global "any" address (defined in mock_lwip.c). */
extern const ip_addr_t ip_addr_any;
#define IP4_ADDR_ANY   (&ip_addr_any)
#define IP_ADDR_ANY    IP4_ADDR_ANY

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LWIP_IP_ADDR_H */
