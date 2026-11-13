/*
 * alpha.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  The rows of a transfer with DGX_VSCREEN_ALPHA(a) mixed with what lies
 *  under them: dgx_alpha_priv.h.
 */

#include "dgx_alpha_priv.h"
#include "dgx_vscreen_priv.h"

void dgx_alpha_row(uint8_t *to, const uint8_t *under, const uint8_t *from, uint32_t n, uint8_t bits, uint32_t w)
{
    if (bits == 16) {
        /* the crossfade of two 16-bit pictures: a loop of its own, with no test in it */
        if (under) {
            for (uint32_t i = 0; i < n; ++i, to += 2, under += 2, from += 2) {
                const uint16_t mixed = dgx_alpha_mix16((uint16_t)(under[0] << 8 | under[1]), (uint16_t)(from[0] << 8 | from[1]), w);
                to[0]                = (uint8_t)(mixed >> 8);
                to[1]                = (uint8_t)mixed;
            }
        } else {
            for (uint32_t i = 0; i < n; ++i, to += 2, from += 2) {
                const uint16_t mixed = dgx_alpha_mix16(0, (uint16_t)(from[0] << 8 | from[1]), w);
                to[0]                = (uint8_t)(mixed >> 8);
                to[1]                = (uint8_t)mixed;
            }
        }
        return;
    }
    const uint8_t bytes = dgx_vscreen_pixel_bytes(bits);
    for (uint32_t i = 0; i < n; ++i, to += bytes, from += bytes) {
        const uint32_t below = under ? dgx_vscreen_value(under + (size_t)i * bytes, bytes) : 0u;
        dgx_vscreen_put_value(to, bytes, dgx_alpha_mix(bits, below, dgx_vscreen_value(from, bytes), w));
    }
}

void dgx_alpha_row_on(const dgx_alpha_t *alpha, dgx_screen_t *dst, uint8_t *row, int x, int y, uint32_t n, uint8_t bytes)
{
    if (!alpha->read) {
        dgx_alpha_row(row, NULL, row, n, dst->color_bits, alpha->weight);
        return;
    }
    for (uint32_t i = 0; i < n; ++i, row += bytes) {
        const uint32_t under = dst->get_pixel(dst, x + (int)i, y);
        dgx_vscreen_put_value(row, bytes, dgx_alpha_apply(alpha, dst->color_bits, under, dgx_vscreen_value(row, bytes)));
    }
}

void dgx_alpha_lut(uint16_t *to, const uint16_t *lut, uint32_t w)
{
    for (int i = 0; i < 256; ++i) to[i] = dgx_alpha_mix16(0, lut[i], w);
}

const uint16_t *dgx_alpha_indices(void)
{
    /* made at the first mask; the values are the same however often it is made */
    static uint16_t indices[256];
    if (!indices[255])
        for (int i = 0; i < 256; ++i) indices[i] = (uint16_t)i;
    return indices;
}
