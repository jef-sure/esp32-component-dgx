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
 * dgx_vscreen8_to_screen16() and dgx_vscreen8_region_to_screen16() take them:
 * uint16_t lut[256]. Change the table and the next transfer shows the whole
 * frame in the new colors; the pixels in RAM are not redrawn.
 *
 * Every function fills the entries first .. first + count - 1 of the table and
 * nothing else; entries outside 0 .. 255 are skipped, and count <= 0 does
 * nothing. A palette given as RGB888 has its own entries 0 .. count - 1: its
 * entry k goes to lut[first + k]. Palettes may be const, in flash. Nothing is
 * allocated.
 *
 * A single color is 0xRRGGBB, as dgx_rgb_to_24() makes it.
 *
 * Fades and flashes mix the RGB888 palette kept by the caller and convert the
 * result to RGB565 each time, so a fade done step by step does not gather the
 * rounding errors of RGB565. Mixing rounds to the nearest value: t = 0 gives a
 * exactly, t = 255 gives b exactly.
 */

/** Entries first .. first + count - 1 from an RGB888 palette of count colors. */
void dgx_lut_from_rgb(uint16_t *lut, int first, int count, const uint8_t (*rgb)[3]);

/**
 * A smooth ramp over the entries first .. first + count - 1: the first one is
 * exactly rgb_from, the last one exactly rgb_to, the ones between rounded to
 * the nearest. The table of a soft pen, from the paper at 0 to the ink at 255,
 * is dgx_lut_gradient(lut, 0, 256, paper, ink).
 */
void dgx_lut_gradient(uint16_t *lut, int first, int count, uint32_t rgb_from, uint32_t rgb_to);

/** A mix of two RGB888 palettes of count colors: t = 0 gives a, t = 255 gives b. A change of palette. */
void dgx_lut_mix(uint16_t *lut, int first, int count, const uint8_t (*a)[3], const uint8_t (*b)[3], uint8_t t);

/** A mix of an RGB888 palette of count colors with one color: a fade to black (0x000000) or a flash to white (0xffffff). */
void dgx_lut_mix_color(uint16_t *lut, int first, int count, const uint8_t (*a)[3], uint32_t rgb, uint8_t t);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
