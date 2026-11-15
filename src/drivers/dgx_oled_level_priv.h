#pragma once
/*
 * dgx_oled_level_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  The level of dgx_screen_brightness() as the contrast of a page OLED
 *  controller: SSD1306, SSD1309, SH1106.
 */

#include <stdint.h>

/*
 * The current of a pixel is in proportion to the contrast, and the eye tells
 * ratios of light apart, not differences: every 32 levels double the
 * contrast, 1 at level 0, 16 at 128, 128 at 224, and 255 at the top.
 *
 * Level 0 is contrast 1, the dimmest that is lit: at contrast 0 the 0.42"
 * glass of 72x40 lights nothing. On that glass the eye finds the full
 * brightness from a contrast of 24 up and a clearly dimmer picture at 1,
 * with little to tell between: the range of an OLED of this kind is short.
 */
static inline uint8_t dgx_oled_level_to_contrast(uint8_t level)
{
    if (level == 255) return 255;
    const unsigned base = 1u << (level >> 5);
    return (uint8_t)(base + base * (level & 31u) / 32u);
}
