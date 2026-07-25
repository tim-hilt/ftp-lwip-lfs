/**
 * @file ip4_addr.h
 * @brief Mock lwIP IPv4 address types for unit testing.
 */
#ifndef MOCK_LWIP_IP4_ADDR_H
#define MOCK_LWIP_IP4_ADDR_H

#include "lwip/arch.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ip4_addr {
    u32_t addr;
};

typedef struct ip4_addr ip4_addr_t;

/* Construct a u32 from four octets (network byte order on LE hosts). */
#define LWIP_MAKEU32(a,b,c,d) \
    (((u32_t)((a) & 0xff) << 24) | \
     ((u32_t)((b) & 0xff) << 16) | \
     ((u32_t)((c) & 0xff) <<  8) | \
      (u32_t)((d) & 0xff))

/*
 * On the real target this would be PP_HTONL, which byte-swaps on LE hosts.
 * For the mock we store addresses in the same layout ftp_server.c expects:
 * the four octets packed into a u32 so that casting to uint8_t* yields
 * a[0]..a[3] in network order.  On little-endian test hosts this means
 * storing them WITHOUT swapping — i.e. PP_HTONL is the identity here
 * is incorrect; we must swap so that (uint8_t*)&addr gives the octets
 * in order.  We simply store them in host byte order and let the mock
 * for tcp_tcp_get_tcp_addrinfo hand them back.  The real PP_HTONL macro
 * is endian-dependent; for consistency we replicate the LE variant.
 */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PP_HTONL(x) ((u32_t)(x))
#else
#define PP_HTONL(x) \
    ((u32_t)((((x) & 0x000000ffUL) << 24) | \
             (((x) & 0x0000ff00UL) <<  8) | \
             (((x) & 0x00ff0000UL) >>  8) | \
             (((x) & 0xff000000UL) >> 24)))
#endif

#define IP4_ADDR(ipaddr, a,b,c,d) \
    (ipaddr)->addr = PP_HTONL(LWIP_MAKEU32(a,b,c,d))

#define ip4_addr_get_u32(src) ((src)->addr)

#define IPADDR_NONE    ((u32_t)0xffffffffUL)
#define IPADDR_ANY     ((u32_t)0x00000000UL)

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LWIP_IP4_ADDR_H */
