#pragma once
/*
 * dgx_overlay.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  An overlay layer of a screen: a picture of 8-bit indices lying over a
 *  part of the screen, mixed with what goes to the screen under it. Buttons
 *  drawn over a picture of a game, which shows through them.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Lay a layer over a part of a 16-bit screen, or move it, or give it other colors.
 *
 * The layer is an 8-bit virtual screen of dgx_vscreen_init(); whoever owns
 * it draws into it as into any screen. Index 0 of it is empty; another
 * index is the color `lut` gives it, mixed with the pixel under it by
 * `alpha`: 255 the color alone, 128 half and half, 0 nothing of it. The
 * mixing goes in 32 steps.
 *
 * What is mixed is what goes to the screen from virtual screens:
 * dgx_vscreen8_to_screen16() and dgx_vscreen8_region_to_screen16() with all
 * their flags, dgx_vscreen_to_screen() and dgx_vscreen_region_to_screen().
 * What is drawn straight on the screen, a line, a text, a filled rectangle,
 * is not: it goes under the layer as if there were none, and the layer
 * shows over it only where something from a virtual screen is put again.
 * A transfer with DGX_LUT_TRANSPARENT mixes the pixels it puts.
 *
 * The screen cannot draw again by itself what lies under a part of the
 * layer that has changed: a display behind SPI keeps its pixels where they
 * cannot be read fast, a frame buffer has been drawn over. So the places
 * that change are kept, see dgx_overlay_take_dirty(), and whoever puts the
 * picture on the screen puts them again from where it has it. They are
 * changed by drawing into the layer, by this call and by
 * dgx_overlay_clear(). Drawing into the layer is seen through its
 * update_screen, which this call takes over until the layer is taken
 * away: pixels written into its memory past the drawing functions, and
 * dgx_screen_take_dirty() of the layer inside a batch, are not seen.
 *
 * Nothing here waits for anything: the layer is drawn into and the screen
 * is drawn on by the same task, or the program sees to it that they are
 * not at the same time.
 *
 * @param display The screen, 16 bits a pixel.
 * @param layer   8-bit virtual screen, of any size; one screen has one layer, a layer lies over one screen.
 * @param x,y     Where its pixel (0, 0) lies on the screen; a part of it may lie outside.
 * @param lut     Its 256 colors, RGB565; taken when called, the table is not kept.
 * @param alpha   How much of its colors over what lies under them, 0 to 255.
 * @return false when the screen is not 16-bit, the layer not an 8-bit virtual screen or lies over another screen, or there is no memory.
 */
bool dgx_overlay_set(dgx_screen_t *display, dgx_screen_t *layer, int x, int y, const uint16_t *lut, uint8_t alpha);

/** @brief Take the layer away from the screen; its place becomes changed. */
void dgx_overlay_clear(dgx_screen_t *display);

/**
 * @brief Take the part of the screen the layer has changed since the last call, and forget it.
 *
 * One rectangle that holds all the changes. Whoever puts the picture on
 * the screen puts this part of it again, and the layer is mixed in on the
 * way.
 *
 * @return false when nothing has changed; the rectangle is all zeros then.
 */
bool dgx_overlay_take_dirty(dgx_screen_t *display, int *x, int *y, int *width, int *height);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
