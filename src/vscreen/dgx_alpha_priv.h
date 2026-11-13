#pragma once
/*
 * dgx_alpha_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  A transfer with DGX_VSCREEN_ALPHA(a): every pixel it puts mixed with
 *  what lies under it, in 32 steps as the overlay layer is. Under it is
 *  what the destination holds where it can be read: its memory, or its
 *  pixels through the screen. A display behind a bus that cannot be read
 *  has black under it, so a picture there fades to black and from it.
 *  Each channel of a pixel is (color * w + under * (32 - w)) >> 5.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef struct {
    uint32_t       weight; /* of the picture, 0 to 31; 32, the picture alone, is no mixing, and a transfer then has no alpha */
    bool           read;   /* what lies under is read from the destination; else it is black */
    /*
     * A mask, DGX_VSCREEN_MASK: the colors the transfer puts are the indices
     * of its picture, and each puts `ink` over what lies under it, unders[index]
     * of 32 left of that; `weight` is not used then. NULL for none.
     */
    const uint8_t *unders;
    uint16_t       ink;
} dgx_alpha_t;

/* the weight of the picture a transfer asks for: 32 without DGX_VSCREEN_ALPHA, or with it of 255 */
static inline uint32_t dgx_alpha_weight(uint32_t flags)
{
    const uint32_t alpha = 255u - (flags >> 24);
    return (alpha * 32u + 127u) / 255u;
}

/* a channel of `mask` at `shift` mixed */
static inline uint32_t dgx_alpha_channel(uint32_t under, uint32_t color, uint32_t w, unsigned shift, uint32_t mask)
{
    const uint32_t u = (under >> shift) & mask, c = (color >> shift) & mask;
    return ((c * w + u * (32u - w)) >> 5) << shift;
}

/* an RGB565 color over another, the three channels in one multiplication each, as the overlay layer mixes */
static inline uint16_t dgx_alpha_mix16(uint16_t under, uint16_t color, uint32_t w)
{
    const uint32_t pre = ((color | (uint32_t)color << 16) & 0x07E0F81Fu) * w;
    return dgx_bus_overlay_mix(under, pre, 32u - w);
}

/*
 * A value of a pixel of `bits` mixed with the one under it: 1 bit the picture
 * from half on; 4 bits a gray level in the high nibble; 8 bits an amount of
 * ink; 12 bits nibbles at 15..12, 11..8 and 7..4; 16 bits RGB565; 18 bits the
 * six high bits of each byte; 24 bits a byte each; 32 bits four bytes.
 */
static inline uint32_t dgx_alpha_mix(uint8_t bits, uint32_t under, uint32_t color, uint32_t w)
{
    switch (bits) {
    case 1:
        return w >= 16u ? color : under;
    case 4:
        return dgx_alpha_channel(under, color, w, 4, 15);
    case 8:
        return dgx_alpha_channel(under, color, w, 0, 255);
    case 12:
        return dgx_alpha_channel(under, color, w, 12, 15) | dgx_alpha_channel(under, color, w, 8, 15) | dgx_alpha_channel(under, color, w, 4, 15);
    case 16:
        return dgx_alpha_mix16((uint16_t)under, (uint16_t)color, w);
    case 18:
        return dgx_alpha_channel(under, color, w, 18, 63) | dgx_alpha_channel(under, color, w, 10, 63) | dgx_alpha_channel(under, color, w, 2, 63);
    case 24:
        return dgx_alpha_channel(under, color, w, 16, 255) | dgx_alpha_channel(under, color, w, 8, 255) | dgx_alpha_channel(under, color, w, 0, 255);
    default:
        return dgx_alpha_channel(under, color, w, 24, 255) | dgx_alpha_channel(under, color, w, 16, 255) | dgx_alpha_channel(under, color, w, 8, 255) |
               dgx_alpha_channel(under, color, w, 0, 255);
    }
}

/* `color` put over `under`: by the weight of the transfer, or as an index of a mask */
static inline uint32_t dgx_alpha_apply(const dgx_alpha_t *alpha, uint8_t bits, uint32_t under, uint32_t color)
{
    if (alpha->unders) return dgx_alpha_mix16((uint16_t)under, alpha->ink, 32u - alpha->unders[color & 0xffu]);
    return dgx_alpha_mix(bits, under, color, alpha->weight);
}

/* a pixel the transfer puts at (x, y) of the destination, with what lies under it */
static inline uint32_t dgx_alpha_pixel(const dgx_alpha_t *alpha, dgx_screen_t *dst, int x, int y, uint32_t color)
{
    return dgx_alpha_apply(alpha, dst->color_bits, alpha->read ? dst->get_pixel(dst, x, y) : 0u, color);
}

/*
 * n pixels of `bytes` each, kept the high byte first as a virtual screen
 * keeps them and a display takes them, mixed with those of `under` kept so
 * too; with black when `under` is NULL. `to` may be `under`.
 */
void dgx_alpha_row(uint8_t *to, const uint8_t *under, const uint8_t *from, uint32_t n, uint8_t bits, uint32_t w);

/*
 * A row of a display about to be sent, n pixels from x on of its row y,
 * mixed with what lies there: read pixel by pixel, or black.
 */
void dgx_alpha_row_on(const dgx_alpha_t *alpha, dgx_screen_t *dst, uint8_t *row, int x, int y, uint32_t n, uint8_t bytes);

/* a table of 256 RGB565 colors over black: the colors of an 8-bit picture put on a display that cannot be read */
void dgx_alpha_lut(uint16_t *to, const uint16_t *lut, uint32_t w);

/* the table that gives every index itself: a mask goes through it, and its indices come out where colors would */
const uint16_t *dgx_alpha_indices(void);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
