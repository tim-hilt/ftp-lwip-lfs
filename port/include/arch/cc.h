/**
 * @file cc.h
 * @brief Minimal lwIP compiler/platform abstraction for building
 *        ftp_server.c as a standalone static library (NO_SYS=1, raw API).
 *
 * A real deployment will normally supply its own arch/cc.h tailored to the
 * target MCU; this one only needs to satisfy the host toolchain used to
 * compile the library sources themselves.
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard integer/size types and format specifiers are available on the
 * host toolchain, so the stdint.h/inttypes.h defaults in lwip/arch.h apply
 * unchanged. */

#define LWIP_RAND() ((u32_t)rand())

#define LWIP_ERRNO_STDINCLUDE 1

typedef unsigned int sys_prot_t;

#ifdef __cplusplus
}
#endif

#endif /* LWIP_ARCH_CC_H */
