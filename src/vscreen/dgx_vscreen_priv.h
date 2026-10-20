#pragma once
/*
 * dgx_vscreen_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Where a pixel of a virtual screen lies in its memory: rows one after
 *  another, the pixels of a row one after another, packed into bytes when
 *  they are smaller than a byte. The screen has to be linear,
 *  dgx_vscreen_is_linear().
 */

#include <stdint.h>

#include "dgx_bits.h"
#include "drivers/vscreen.h"

/* the number of the pixel (x, y) counted from (0, 0) row by row */
#define DGX_VSCR_OFFSET(vscr, x, y) ((x) + (y) * (vscr)->base.width)

/* the byte the pixel `offset` starts in */
#define DGX_VSCR_PTR(vscr, offset) \
    ((vscr)->v_array + ((vscr)->base.color_bits < 8u     ? (offset) / (8u / (vscr)->base.color_bits) \
                        : (vscr)->base.color_bits == 12u ? (offset) * 3u / 2u                        \
                                                         : (offset) * (((vscr)->base.color_bits + 7u) / 8u)))

/* bytes a pixel takes in memory when it takes whole bytes: 8, 16, 18, 24 and 32 bits; 0 for the others */
static inline uint8_t dgx_vscreen_pixel_bytes(uint8_t color_bits)
{
    switch (color_bits) {
    case 8:
        return 1;
    case 16:
        return 2;
    case 18:
    case 24:
        return 3;
    case 32:
        return 4;
    default:
        return 0;
    }
}

/*
 * The value of a pixel of whole bytes, the high byte first as a virtual
 * screen keeps it, and the other way. A loop over the bytes of every pixel
 * costs as much again as the pixel.
 */
static inline uint32_t dgx_vscreen_value(const uint8_t *at, uint8_t bytes)
{
    switch (bytes) {
    case 1:
        return at[0];
    case 2:
        return (uint32_t)at[0] << 8 | at[1];
    case 3:
        return (uint32_t)at[0] << 16 | (uint32_t)at[1] << 8 | at[2];
    default:
        return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
    }
}

static inline void dgx_vscreen_put_value(uint8_t *at, uint8_t bytes, uint32_t value)
{
    switch (bytes) {
    case 1:
        at[0] = (uint8_t)value;
        return;
    case 2:
        at[0] = (uint8_t)(value >> 8), at[1] = (uint8_t)value;
        return;
    case 3:
        at[0] = (uint8_t)(value >> 16), at[1] = (uint8_t)(value >> 8), at[2] = (uint8_t)value;
        return;
    default:
        at[0] = (uint8_t)(value >> 24), at[1] = (uint8_t)(value >> 16), at[2] = (uint8_t)(value >> 8), at[3] = (uint8_t)value;
        return;
    }
}
