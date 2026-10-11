/*
 * pixels.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <string.h>

#include "dgx_bits.h"

/* a word of a pixel buffer: the buffer is bytes, and is written by words where pixels repeat */
typedef uint32_t __attribute__((may_alias)) dgx_pixel_word_t;

/* A run shorter than this is faster byte by byte than with the words set up: measured on an ESP32 */
#define DGX_FILL_WORDS_FROM_24 16u

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define DGX_FILL_BY_WORDS 1
#else
#define DGX_FILL_BY_WORDS 0
#endif

void dgx_fill_rows_16(uint8_t *row, size_t pitch, uint32_t width, uint32_t height, uint32_t color)
{
    for (; height; --height, row += pitch) dgx_fill_pixels_16(row, width, color);
}

void dgx_fill_pixels(uint8_t color_bits, uint8_t *lp, uint32_t idx, uint32_t count, uint32_t color)
{
    switch (color_bits) {
    case 1:
        dgx_fill_bits_msb(lp, idx & 7u, count, color != 0);
        return;
    case 4:
        for (uint32_t i = 0; i < count; ++i) lp = dgx_fill_buf_value_4(lp, (int)(idx + i), color);
        return;
    case 8:
        memset(lp, (int)color, count);
        return;
    case 12:
        for (uint32_t i = 0; i < count; ++i) lp = dgx_fill_buf_value_12(lp, (int)(idx + i), color);
        return;
    case 16:
        dgx_fill_pixels_16(lp, count, color);
        return;
    case 18:
    case 24: {
        const uint8_t c[3] = {(uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color};
        uint32_t      n = count * 3u, phase = 0;
        if (DGX_FILL_BY_WORDS && count >= DGX_FILL_WORDS_FROM_24) {
            for (; (uintptr_t)lp & 3u; --n) {
                *lp++ = c[phase];
                phase = phase == 2 ? 0 : phase + 1;
            }
            /* four pixels are three words, and after them the bytes of a pixel come round to where they were */
            uint32_t          c0 = c[phase], c1 = c[phase == 2 ? 0 : phase + 1], c2 = c[phase == 0 ? 2 : phase - 1];
            uint32_t          w0 = c0 | c1 << 8 | c2 << 16 | c0 << 24;
            uint32_t          w1 = c1 | c2 << 8 | c0 << 16 | c1 << 24;
            uint32_t          w2 = c2 | c0 << 8 | c1 << 16 | c2 << 24;
            dgx_pixel_word_t *wp = (dgx_pixel_word_t *)(void *)lp;
            for (; n >= 12; n -= 12, wp += 3) wp[0] = w0, wp[1] = w1, wp[2] = w2;
            lp = (uint8_t *)wp;
        }
        for (; n; --n) {
            *lp++ = c[phase];
            phase = phase == 2 ? 0 : phase + 1;
        }
        return;
    }
    case 32:
        for (uint32_t i = 0; i < count; ++i) lp = dgx_fill_buf_value_32(lp, (int)i, color);
        return;
    default:
        return;
    }
}
