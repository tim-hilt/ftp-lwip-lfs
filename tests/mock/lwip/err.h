/**
 * @file err.h
 * @brief Mock lwIP error codes for unit testing.
 */
#ifndef MOCK_LWIP_ERR_H
#define MOCK_LWIP_ERR_H

#include "lwip/arch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ERR_OK         =   0,
    ERR_MEM        =  -1,
    ERR_BUF        =  -2,
    ERR_TIMEOUT    =  -3,
    ERR_RTE        =  -4,
    ERR_INPROGRESS =  -5,
    ERR_VAL        =  -6,
    ERR_WOULDBLOCK =  -7,
    ERR_USE        =  -8,
    ERR_ALREADY    =  -9,
    ERR_ISCONN     = -10,
    ERR_CONN       = -11,
    ERR_IF         = -12,
    ERR_ABRT       = -13,
    ERR_RST        = -14,
    ERR_CLSD       = -15,
    ERR_ARG        = -16
} err_enum_t;

typedef s8_t err_t;

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LWIP_ERR_H */
