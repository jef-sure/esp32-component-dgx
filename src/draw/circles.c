/*
 * circles.c
 *
 *  The outline of a circle and a solid circle, as every screen draws them
 *  by its pixels and rectangles.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>

#include "dgx_draw_priv.h"

/*
 * The points (xa..xb, yoff) of the upper right eighth of a circle and their
 * seven reflections: four rows and four columns. A run that starts on an
 * axis meets its reflection there, and the two go as one.
 *
 * The order matters to a display that takes a window: its columns and its
 * rows are set by two commands, and one that has not changed is not sent
 * again. So the runs follow one another with the columns or the rows kept:
 * of the eight only the first and the fifth set both.
 */
static void dgx_draw_circle_eighths(dgx_screen_t *scr, int x, int y, int yoff, int xa, int xb, uint32_t color)
{
    if (xa == 0) {
        dgx_draw_row_run(scr, x - xb, x + xb, y - yoff, color);
        dgx_draw_row_run(scr, x - xb, x + xb, y + yoff, color);    /* the same columns */
        dgx_draw_column_run(scr, x - yoff, y - xb, y + xb, color);
        dgx_draw_column_run(scr, x + yoff, y - xb, y + xb, color); /* the same rows */
        return;
    }
    dgx_draw_row_run(scr, x + xa, x + xb, y - yoff, color);
    dgx_draw_row_run(scr, x - xb, x - xa, y - yoff, color);    /* the same row */
    dgx_draw_row_run(scr, x - xb, x - xa, y + yoff, color);    /* the same columns */
    dgx_draw_row_run(scr, x + xa, x + xb, y + yoff, color);    /* the same row */
    dgx_draw_column_run(scr, x + yoff, y + xa, y + xb, color);
    dgx_draw_column_run(scr, x - yoff, y + xa, y + xb, color); /* the same rows */
    dgx_draw_column_run(scr, x - yoff, y - xb, y - xa, color); /* the same column */
    dgx_draw_column_run(scr, x + yoff, y - xb, y - xa, color); /* the same rows */
}

/*
 * A circle by the midpoint algorithm: one eighth is walked, from the top to
 * the diagonal, and every point of it is reflected into the other seven.
 * The points of the eighth that lie in one row go as one run.
 */
void dgx_draw_circle_runs(dgx_screen_t *scr, int x, int y, int r, uint32_t color)
{
    if (r < 0) return;
    if (y + r < 0 || y - r >= scr->height || x - r >= scr->width || x + r < 0) return;
    if (r == 0) {
        scr->set_pixel(scr, x, y, color);
        return;
    }
    int f   = 1 - r;
    int ddx = 1;
    int ddy = -2 * r;
    int xs  = 0;
    int ys  = r;
    int run = 0; /* the first point of the row the walk is in */
    dgx_screen_progress_up(scr);
    while (xs < ys) {
        int row = ys;
        int end = xs;
        if (f >= 0) {
            --ys;
            ddy += 2;
            f += ddy;
        }
        ++xs;
        ddx += 2;
        f += ddx;
        if (ys != row) {
            dgx_draw_circle_eighths(scr, x, y, row, run, end, color);
            run = xs;
        }
    }
    dgx_draw_circle_eighths(scr, x, y, ys, run, xs, color);
    dgx_screen_progress_down(scr);
}

/*
 * A solid circle by the same walk, filled by rows. A row at the height of a
 * point of the eighth reaches as far to the sides as that point lies from
 * the top; rows that reach equally far, as those near the middle do, go as
 * one rectangle. A row of the cap is done when the walk leaves it.
 *
 * Rows and not columns: where a screen is memory, a row is one piece of it
 * and a column is a pixel in every row it crosses. To a display that takes a
 * window it is all the same.
 */
void dgx_draw_solid_circle_rows(dgx_screen_t *scr, int x, int y, int r, uint32_t color)
{
    if (r < 0) return;
    if (y + r < 0 || y - r >= scr->height || x - r >= scr->width || x + r < 0) return;
    if (r == 0) {
        scr->set_pixel(scr, x, y, color);
        return;
    }
    int f    = 1 - r;
    int ddx  = 1;
    int ddy  = -2 * r;
    int xs   = 0;
    int ys   = r;
    int band = 0; /* the nearest to the middle of the rows that reach ys to each side */
    dgx_screen_progress_up(scr);
    while (1) {
        int  reach = ys;
        int  end   = xs;
        bool more  = xs < ys;
        if (more) {
            if (f >= 0) {
                --ys;
                ddy += 2;
                f += ddy;
            }
            ++xs;
            ddx += 2;
            f += ddx;
            if (ys == reach) continue;
        }
        /* the rows from band to end reach `reach` to each side: above and below the middle, or across it */
        if (band == 0) {
            scr->fill_rectangle(scr, x - reach, y - end, 2 * reach + 1, 2 * end + 1, color);
        } else {
            scr->fill_rectangle(scr, x - reach, y - end, 2 * reach + 1, end - band + 1, color);
            scr->fill_rectangle(scr, x - reach, y + band, 2 * reach + 1, end - band + 1, color);
        }
        /* the row of the cap the walk has left reaches as far as the walk came in it; the middle row is filled already */
        if (end > 0 && reach > 0) {
            scr->fill_rectangle(scr, x - end, y - reach, 2 * end + 1, 1, color);
            scr->fill_rectangle(scr, x - end, y + reach, 2 * end + 1, 1, color);
        }
        if (!more) break;
        band = xs;
    }
    dgx_screen_progress_down(scr);
}
