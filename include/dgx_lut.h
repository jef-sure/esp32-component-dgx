#pragma once
/*
 * dgx_lut.h
 *
 *  Created on: Oct 15, 2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdint.h>

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/*
 * Tables of RGB565 colors for an 8-bit virtual screen, as
 * dgx_vscreen8_to_screen16(), dgx_vscreen8_region_to_screen16() and
 * dgx_overlay_set() take them: uint16_t lut[256]. Change the table and the
 * next transfer shows the whole frame in the new colors; the pixels in RAM
 * are not redrawn.
 *
 * The table holds RGB565, and the functions here take RGB888: a single color
 * as 0xRRGGBB, a palette as uint8_t [count][3]. A color in RGB565, such as
 * DGX_RED(dgx_rgb_to_16), compiles and gives wrong colors.
 *
 *     static uint16_t lut[256];
 *     dgx_lut_gradient(lut, 0, 256, 0x000000, dgx_rgb_to_24(150, 215, 240));
 *     dgx_vscreen8_to_screen16(lcd, 0, 0, band, lut, 0);
 *
 * Every function fills the entries first .. first + count - 1 of the table and
 * nothing else; entries outside 0 .. 255 are skipped, and count <= 0 does
 * nothing. A palette given as RGB888 has its own entries 0 .. count - 1: its
 * entry k goes to lut[first + k]. Palettes may be const, in flash. Nothing is
 * allocated.
 *
 * A single color is 0xRRGGBB, as dgx_rgb_to_24() and DGX_RED(dgx_rgb_to_24)
 * make it.
 *
 * Fades and flashes mix the RGB888 palette kept by the caller and convert the
 * result to RGB565 each time, so a fade done step by step does not gather the
 * rounding errors of RGB565. Mixing rounds to the nearest value: t = 0 gives a
 * exactly, t = 255 gives b exactly.
 */

/**
 * Entries first .. first + count - 1 from an RGB888 palette of count colors.
 *
 * @param lut   The table, uint16_t[256] of RGB565.
 * @param rgb   count colors as {r, g, b} bytes, RGB888; entry k goes to lut[first + k].
 */
void dgx_lut_from_rgb(uint16_t *lut, int first, int count, const uint8_t (*rgb)[3]);

/**
 * A smooth ramp over the entries first .. first + count - 1: the first one is
 * exactly rgb_from, the last one exactly rgb_to, the ones between rounded to
 * the nearest. The table of a soft pen, from the paper at 0 to the ink at 255,
 * is dgx_lut_gradient(lut, 0, 256, paper, ink).
 *
 * @param lut       The table, uint16_t[256] of RGB565.
 * @param rgb_from  The color of entry first, 0xRRGGBB.
 * @param rgb_to    The color of entry first + count - 1, 0xRRGGBB.
 */
void dgx_lut_gradient(uint16_t *lut, int first, int count, uint32_t rgb_from, uint32_t rgb_to);

/**
 * A mix of two RGB888 palettes of count colors: t = 0 gives a, t = 255 gives b. A change of palette.
 *
 * @param lut   The table, uint16_t[256] of RGB565.
 * @param a, b  count colors each as {r, g, b} bytes, RGB888.
 */
void dgx_lut_mix(uint16_t *lut, int first, int count, const uint8_t (*a)[3], const uint8_t (*b)[3], uint8_t t);

/**
 * A mix of an RGB888 palette of count colors with one color: a fade to black (0x000000) or a flash to white (0xffffff).
 *
 * @param lut   The table, uint16_t[256] of RGB565.
 * @param a     count colors as {r, g, b} bytes, RGB888.
 * @param rgb   The color mixed in, 0xRRGGBB.
 */
void dgx_lut_mix_color(uint16_t *lut, int first, int count, const uint8_t (*a)[3], uint32_t rgb, uint8_t t);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
