#pragma once
/*
 * dgx_soft_turn_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Orientation in software, for drivers that keep the picture in a virtual
 *  screen and send it to the panel: those whose controller cannot turn it
 *  (ST7920, SSD1322), and the page controllers (SSD1306, SH1106, SSD1309),
 *  whose mirroring by segments and rows depends on how a module wires its
 *  glass. The screen is drawn as it lies and turned while it is copied to
 *  the panel. As with the MX, MY and MV bits of a color controller: dir_x
 *  counts the panel columns from the right, dir_y its rows from the bottom,
 *  and swap_xy runs x of the screen down the panel columns.
 */

#include <stdbool.h>
#include <string.h>

#include "drivers/vscreen.h"

/* true when the screen is not the panel as it is */
static inline bool dgx_soft_turned(const dgx_screen_t *scr)
{
    return scr->swap_xy || scr->dir_x == DgxScreenRightLeft || scr->dir_y == DgxScreenBottomTop;
}

/* the panel a screen lies on, pw x ph: the screen with its axes swapped back */
static inline void dgx_soft_turn_panel(const dgx_screen_t *scr, int *pw, int *ph)
{
    *pw = scr->swap_xy ? scr->height : scr->width;
    *ph = scr->swap_xy ? scr->width : scr->height;
}

/* the screen pixel shown at (px, py) of a panel pw x ph */
static inline void dgx_soft_turn_pixel(const dgx_screen_t *scr, int pw, int ph, int px, int py, int *sx, int *sy)
{
    const int ax = scr->dir_x == DgxScreenRightLeft ? pw - 1 - px : px;
    const int ay = scr->dir_y == DgxScreenBottomTop ? ph - 1 - py : py;
    *sx          = scr->swap_xy ? ay : ax;
    *sy          = scr->swap_xy ? ax : ay;
}

/*
 * A rectangle of the screen, both ends included, clipped to the screen and
 * turned into the same rectangle of a panel pw x ph. false when none of it
 * is on the screen.
 */
static inline bool dgx_soft_turn_rect(const dgx_screen_t *scr, int pw, int ph, int *left, int *right, int *top, int *bottom)
{
    if (*left < 0) *left = 0;
    if (*top < 0) *top = 0;
    if (*right >= scr->width) *right = scr->width - 1;
    if (*bottom >= scr->height) *bottom = scr->height - 1;
    if (*left > *right || *top > *bottom) return false;
    const int a0 = scr->swap_xy ? *top : *left, a1 = scr->swap_xy ? *bottom : *right;
    const int b0 = scr->swap_xy ? *left : *top, b1 = scr->swap_xy ? *right : *bottom;
    *left        = scr->dir_x == DgxScreenRightLeft ? pw - 1 - a1 : a0;
    *right       = scr->dir_x == DgxScreenRightLeft ? pw - 1 - a0 : a1;
    *top         = scr->dir_y == DgxScreenBottomTop ? ph - 1 - b1 : b0;
    *bottom      = scr->dir_y == DgxScreenBottomTop ? ph - 1 - b0 : b1;
    return true;
}

/*
 * The bytes of a page of a page controller, of a panel pw x ph: a byte
 * holds 8 rows of a panel column, the top one in bit 0, and the page rows
 * page * 8 to page * 8 + 7. They are gathered for the columns left to right
 * from a screen in the page layout of dgx_bw_init(), whose memory is pages
 * as wide as the screen, turned on the panel.
 */
static inline void dgx_soft_turn_page(const dgx_screen_t *scr, const uint8_t *pages, int pw, int ph, int page, int left, int right,
                                      uint8_t *bytes)
{
    for (int px = left; px <= right; ++px) {
        uint8_t byte = 0;
        for (int bit = 0; bit < 8 && page * 8 + bit < ph; ++bit) {
            int sx, sy;
            dgx_soft_turn_pixel(scr, pw, ph, px, page * 8 + bit, &sx, &sy);
            if (pages[(size_t)(sy >> 3) * (size_t)scr->width + (size_t)sx] >> (sy & 7) & 1u) byte |= (uint8_t)(1u << bit);
        }
        *bytes++ = byte;
    }
}

/*
 * Sets the orientation of a screen over a panel pw x ph, its picture in
 * memory; the driver then sends the whole screen. With the axes swapped the
 * screen is ph x pw: its memory stays the same size, but the rows change
 * length, so the picture is cleared. Whatever was waiting to be sent is
 * dropped.
 */
static inline void dgx_soft_turn_set(dgx_screen_t *scr, uint8_t *memory, int pw, int ph, dgx_orientation_t dir_x, dgx_orientation_t dir_y,
                                     bool swap_xy)
{
    const int width = swap_xy ? ph : pw, height = swap_xy ? pw : ph;
    scr->dir_x   = dir_x;
    scr->dir_y   = dir_y;
    scr->swap_xy = swap_xy;
    if (scr->width != width) {
        memset(memory, 0, (size_t)pw * (size_t)ph * scr->color_bits / 8u);
        scr->width  = width;
        scr->height = height;
    }
    scr->dirty_left = scr->dirty_right = scr->dirty_top = scr->dirty_bottom = 0;
}
