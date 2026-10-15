/*
 * lut.c
 *
 *  Created on: Oct 15, 2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include "dgx_lut.h"
#include "dgx_bits.h"

/* the part of k = 0 .. count - 1 whose entries first + k lie in the table */
static void dgx_lut_span(int first, int count, int *from, int *to)
{
    *from = first < 0 ? -first : 0;
    *to   = first + count > 256 ? 256 - first : count;
}

/* a of 255 - t and b of t, rounded to the nearest */
static inline uint8_t dgx_lut_mix_channel(unsigned a, unsigned b, unsigned t)
{
    return (uint8_t)((a * (255u - t) + b * t + 127u) / 255u);
}

void dgx_lut_from_rgb(uint16_t *lut, int first, int count, const uint8_t (*rgb)[3])
{
    int from, to;
    dgx_lut_span(first, count, &from, &to);
    for (int k = from; k < to; ++k) lut[first + k] = (uint16_t)dgx_rgb_to_16(rgb[k][0], rgb[k][1], rgb[k][2]);
}

void dgx_lut_gradient(uint16_t *lut, int first, int count, uint32_t rgb_from, uint32_t rgb_to)
{
    int from, to;
    dgx_lut_span(first, count, &from, &to);
    const unsigned last = count > 1 ? (unsigned)count - 1u : 1u;
    for (int k = from; k < to; ++k) {
        uint8_t c[3];
        for (int i = 0; i < 3; ++i) {
            const unsigned shift = 16u - 8u * (unsigned)i, a = (rgb_from >> shift) & 0xffu, b = (rgb_to >> shift) & 0xffu;
            c[i] = (uint8_t)((a * (last - (unsigned)k) + b * (unsigned)k + last / 2u) / last);
        }
        lut[first + k] = (uint16_t)dgx_rgb_to_16(c[0], c[1], c[2]);
    }
}

void dgx_lut_mix(uint16_t *lut, int first, int count, const uint8_t (*a)[3], const uint8_t (*b)[3], uint8_t t)
{
    int from, to;
    dgx_lut_span(first, count, &from, &to);
    for (int k = from; k < to; ++k) {
        lut[first + k] = (uint16_t)dgx_rgb_to_16(dgx_lut_mix_channel(a[k][0], b[k][0], t), dgx_lut_mix_channel(a[k][1], b[k][1], t),
                                                 dgx_lut_mix_channel(a[k][2], b[k][2], t));
    }
}

void dgx_lut_mix_color(uint16_t *lut, int first, int count, const uint8_t (*a)[3], uint32_t rgb, uint8_t t)
{
    int from, to;
    dgx_lut_span(first, count, &from, &to);
    const unsigned r = (rgb >> 16) & 0xffu, g = (rgb >> 8) & 0xffu, b = rgb & 0xffu;
    for (int k = from; k < to; ++k) {
        lut[first + k] = (uint16_t)dgx_rgb_to_16(dgx_lut_mix_channel(a[k][0], r, t), dgx_lut_mix_channel(a[k][1], g, t),
                                                 dgx_lut_mix_channel(a[k][2], b, t));
    }
}
