/**
 * @file pbuf.h
 * @brief Mock lwIP pbuf for unit testing.
 */
#ifndef MOCK_LWIP_PBU_H
#define MOCK_LWIP_PBU_H

#include "lwip/arch.h"
#include "lwip/err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pbuf {
    struct pbuf *next;
    void        *payload;
    u16_t        tot_len;
    u16_t        len;
    u8_t         type_internal;
    u8_t         flags;
    u8_t         ref;
    u8_t         if_idx;
};

u8_t  pbuf_free(struct pbuf *p);
u16_t pbuf_copy_partial(const struct pbuf *p, void *dataptr,
                        u16_t len, u16_t offset);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LWIP_PBU_H */
