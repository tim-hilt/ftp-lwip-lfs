/**
 * @file lwipopts.h
 * @brief lwIP configuration for building the ftp_server static library.
 *
 * Only the raw TCP API (used by ftp_server.c) plus IPv4 are required, so
 * this project builds lwIP with NO_SYS=1 (no OS/thread layer) and leaves
 * every other option at its documented default (see lwip/src/include/
 * lwip/opt.h). A real application embedding this library alongside its own
 * lwIP port is expected to supply its own lwipopts.h/arch headers instead.
 */
#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* No OS: the application drives lwIP via tcp_poll()/sys_check_timeouts()
 * from its own main loop, exactly as ftp_server.c's raw API usage expects. */
#define NO_SYS 1

/* This build only compiles the core + raw API sources (see CMakeLists.txt);
 * the sequential (netconn) and BSD socket APIs require NO_SYS=0 and are not
 * part of this library. */
#define LWIP_NETCONN 0
#define LWIP_SOCKET  0

/* macOS's <sys/_endian.h> (pulled in transitively via system headers) also
 * defines htons/ntohs/htonl/ntohl, conflicting with lwIP's own macros of
 * the same name in def.h. Keep lwIP's lwip_htons()/etc. functions but skip
 * the htons()/etc. macro aliases so both header sets coexist under -Werror. */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1

#endif /* LWIP_LWIPOPTS_H */
