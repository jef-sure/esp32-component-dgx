#pragma once
/*
 * dgx_mix_priv.h: a color mixed over another by a of 255, rounded, as soft text and soft pictures mix their edges.
 *
 *  Copyright (c) 2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */
#include <stdbool.h>
#include <stdint.h>

/* whether colors of this many bits are mixed, channel by channel; 8 bits are an amount of ink */
static inline bool dgx_mix_bits(int bits)
{
    return bits == 4 || bits == 8 || bits == 12 || bits == 16 || bits == 18 || bits == 24;
}

static inline uint32_t dgx_mix_channel(uint32_t under, uint32_t color, unsigned a, unsigned shift, uint32_t mask)
{
    const uint32_t u = (under >> shift) & mask, c = (color >> shift) & mask;
    return ((u * (255u - a) + c * a + 127u) / 255u) << shift;
}

/*
 * `color` over `under` by a of 255, rounded, each channel in its own width: 4 bits a gray level in the high nibble, as
 * the SSD1322 has it; 8 bits one amount of ink; 12 bits nibbles at 15..12, 11..8 and 7..4 (DGX_RGB_12); 16 bits 5, 6
 * and 5; 18 bits the six high bits of each byte (DGX_RGB_18); 24 bits a byte each.
 */
static inline uint32_t dgx_mix_color(int bits, uint32_t under, uint32_t color, unsigned a)
{
    switch (bits) {
    case 4:
        return dgx_mix_channel(under, color, a, 4, 15);
    case 8:
        return dgx_mix_channel(under, color, a, 0, 0xff);
    case 12:
        return dgx_mix_channel(under, color, a, 12, 15) | dgx_mix_channel(under, color, a, 8, 15) | dgx_mix_channel(under, color, a, 4, 15);
    case 16:
        return dgx_mix_channel(under, color, a, 11, 31) | dgx_mix_channel(under, color, a, 5, 63) | dgx_mix_channel(under, color, a, 0, 31);
    case 18:
        return dgx_mix_channel(under, color, a, 18, 63) | dgx_mix_channel(under, color, a, 10, 63) | dgx_mix_channel(under, color, a, 2, 63);
    default:
        return dgx_mix_channel(under, color, a, 16, 255) | dgx_mix_channel(under, color, a, 8, 255) | dgx_mix_channel(under, color, a, 0, 255);
    }
}
