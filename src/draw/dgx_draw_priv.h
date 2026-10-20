#pragma once
/*
 * dgx_draw_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  The figures every screen draws by its own pixels and rectangles, a
 *  display behind a bus and a virtual screen alike: each through its own
 *  set_pixel() and fill_rectangle(), so a figure has the same pixels
 *  wherever it is drawn. A screen gets them as its draw_line, circle and
 *  solid_circle, see dgx_scr_init_slow_bus_optimized_funcs().
 */

#include <stdint.h>

#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/* a row of pixels from xa to xb, either may be the left one; one pixel is not worth a rectangle */
static inline void dgx_draw_row_run(dgx_screen_t *scr, int xa, int xb, int y, uint32_t color)
{
    if (xa == xb) {
        scr->set_pixel(scr, xa, y, color);
    } else if (xa < xb) {
        scr->fill_rectangle(scr, xa, y, xb - xa + 1, 1, color);
    } else {
        scr->fill_rectangle(scr, xb, y, xa - xb + 1, 1, color);
    }
}

/* a column of pixels from ya to yb */
static inline void dgx_draw_column_run(dgx_screen_t *scr, int x, int ya, int yb, uint32_t color)
{
    if (ya == yb) {
        scr->set_pixel(scr, x, ya, color);
    } else if (ya < yb) {
        scr->fill_rectangle(scr, x, ya, 1, yb - ya + 1, color);
    } else {
        scr->fill_rectangle(scr, x, yb, 1, ya - yb + 1, color);
    }
}

/* a line by Bresenham's algorithm, the pixels of a row or a column of it as one rectangle: see lines.c */
void dgx_draw_line_runs(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color);

/* the outline of a circle by the midpoint algorithm, by runs: see circles.c */
void dgx_draw_circle_runs(dgx_screen_t *scr, int x, int y, int r, uint32_t color);

/* a solid circle by the same walk, by rows: see circles.c */
void dgx_draw_solid_circle_rows(dgx_screen_t *scr, int x, int y, int r, uint32_t color);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
