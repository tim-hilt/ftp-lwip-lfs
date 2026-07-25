/**
 * @file arch.h
 * @brief Mock lwIP architecture types for unit testing.
 */
#ifndef MOCK_LWIP_ARCH_H
#define MOCK_LWIP_ARCH_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8_t;
typedef int8_t   s8_t;
typedef uint16_t u16_t;
typedef int16_t  s16_t;
typedef uint32_t u32_t;
typedef int32_t  s32_t;

#ifndef LWIP_UNUSED_ARG
#define LWIP_UNUSED_ARG(x) (void)(x)
#endif

#endif /* MOCK_LWIP_ARCH_H */
