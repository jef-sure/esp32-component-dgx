/*
 * rectangles.c
 *
 *  The outline of a rectangle, and a rectangle with round corners, its
 *  outline and the solid one, as every screen draws them by its pixels and
 *  rectangles.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>

#include "dgx_draw.h"
#include "dgx_draw_priv.h"

/* a negative width or height takes the rectangle the other way, as dgx_fill_rectangle() does; false when nothing is left */
static bool dgx_rectangle_upright(int *x, int *y, int *w, int *h)
{
    if (*w < 0) *x += *w, *w = -*w;
    if (*h < 0) *y += *h, *h = -*h;
    return *w > 0 && *h > 0;
}

/*
 * The top and the bottom rows go whole, the sides between them: no pixel is
 * sent twice. The bottom row has the columns of the top one and the right
 * side the rows of the left one, so a display that takes a window is not
 * sent the same columns or rows again.
 */
void dgx_draw_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    if (!dgx_rectangle_upright(&x, &y, &w, &h)) return;
    const int right = x + w - 1, bottom = y + h - 1;
    dgx_screen_progress_up(scr);
    dgx_draw_row_run(scr, x, right, y, color);
    if (h > 1) dgx_draw_row_run(scr, x, right, bottom, color);
    if (h > 2) {
        dgx_draw_column_run(scr, x, y + 1, bottom - 1, color);
        if (w > 1) dgx_draw_column_run(scr, right, y + 1, bottom - 1, color);
    }
    dgx_screen_progress_down(scr);
}

/*
 * The radius a rectangle of w by h can have: the centres of the corners
 * meet, but do not pass one another.
 */
static int dgx_round_radius(int w, int h, int r)
{
    const int most = ((w < h ? w : h) - 1) / 2;
    return r < 0 ? 0 : r > most ? most : r;
}

/*
 * The points (xa..xb, yoff) of an eighth of a circle and their seven
 * reflections, as dgx_draw_circle() puts them about its centre; here each
 * corner has a centre of its own, (left, top) to (right, bottom). A run
 * that starts on an axis goes on along the side to the next corner, and
 * so the sides are drawn.
 */
static void dgx_round_eighths(dgx_screen_t *scr, int left, int top, int right, int bottom, int yoff, int xa, int xb, uint32_t color)
{
    if (xa == 0) {
        dgx_draw_row_run(scr, left - xb, right + xb, top - yoff, color);
        dgx_draw_row_run(scr, left - xb, right + xb, bottom + yoff, color);    /* the same columns */
        dgx_draw_column_run(scr, left - yoff, top - xb, bottom + xb, color);
        dgx_draw_column_run(scr, right + yoff, top - xb, bottom + xb, color); /* the same rows */
        return;
    }
    dgx_draw_row_run(scr, right + xa, right + xb, top - yoff, color);
    dgx_draw_row_run(scr, left - xb, left - xa, top - yoff, color);            /* the same row */
    dgx_draw_row_run(scr, left - xb, left - xa, bottom + yoff, color);         /* the same columns */
    dgx_draw_row_run(scr, right + xa, right + xb, bottom + yoff, color);       /* the same row */
    dgx_draw_column_run(scr, right + yoff, bottom + xa, bottom + xb, color);
    dgx_draw_column_run(scr, left - yoff, bottom + xa, bottom + xb, color);    /* the same rows */
    dgx_draw_column_run(scr, left - yoff, top - xb, top - xa, color);          /* the same column */
    dgx_draw_column_run(scr, right + yoff, top - xb, top - xa, color);         /* the same rows */
}

/*
 * The corners are the quarters of the circle of dgx_draw_circle() of radius
 * r, walked as it walks them. A square of 2r + 1 is that circle, pixel for
 * pixel; with a radius of 0 it is the outline of dgx_draw_rectangle().
 */
void dgx_draw_round_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, int r, uint32_t color)
{
    if (!dgx_rectangle_upright(&x, &y, &w, &h)) return;
    r = dgx_round_radius(w, h, r);
    if (r == 0) {
        dgx_draw_rectangle(scr, x, y, w, h, color);
        return;
    }
    if (y + h <= 0 || y >= scr->height || x + w <= 0 || x >= scr->width) return;
    const int left = x + r, top = y + r, right = x + w - 1 - r, bottom = y + h - 1 - r;
    int       f   = 1 - r;
    int       ddx = 1;
    int       ddy = -2 * r;
    int       xs  = 0;
    int       ys  = r;
    int       run = 0; /* the first point of the row the walk is in */
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
            dgx_round_eighths(scr, left, top, right, bottom, row, run, end, color);
            run = xs;
        }
    }
    dgx_round_eighths(scr, left, top, right, bottom, ys, run, xs, color);
    dgx_screen_progress_down(scr);
}

/* the column at which the walk of a circle of radius r reaches the diagonal and stops */
static int dgx_round_walk_end(int r)
{
    int f = 1 - r, ddx = 1, ddy = -2 * r, xs = 0, ys = r;
    while (xs < ys) {
        if (f >= 0) {
            --ys;
            ddy += 2;
            f += ddy;
        }
        ++xs;
        ddx += 2;
        f += ddx;
    }
    return xs;
}

/*
 * The walk of dgx_solid_circle(), by rows, with the corners apart: the rows
 * of a corner reach from its centre as far as those of the circle do, and
 * the rows between the corners reach r from them. Rows that reach equally
 * far go as one rectangle, as the rows between the corners do. A square of
 * 2r + 1 is the solid circle, pixel for pixel; with a radius of 0 it is the
 * rectangle of dgx_fill_rectangle().
 *
 * A row of a cap that the rows near the middle reach as well is not sent:
 * the solid circle sends the few at the diagonal twice, but here a row is
 * as long as the rectangle is wide. Where the walk stops, and so how high
 * the rows near the middle go, is found by a walk first.
 */
void dgx_fill_round_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, int r, uint32_t color)
{
    if (!dgx_rectangle_upright(&x, &y, &w, &h)) return;
    r = dgx_round_radius(w, h, r);
    if (y + h <= 0 || y >= scr->height || x + w <= 0 || x >= scr->width) return;
    const int left = x + r, top = y + r, right = x + w - 1 - r, bottom = y + h - 1 - r;
    const int across = right - left + 1, down = bottom - top + 1; /* between the centres of the corners, both ends in */
    const int middle = dgx_round_walk_end(r);                       /* the rows near the middle go this high */
    int       f    = 1 - r;
    int       ddx  = 1;
    int       ddy  = -2 * r;
    int       xs   = 0;
    int       ys   = r;
    int       band = 0; /* the nearest to the middle of the rows that reach ys to each side */
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
        /* the rows from band to end above the top corners and below the bottom ones reach `reach` to each side; or across them */
        if (band == 0) {
            scr->fill_rectangle(scr, left - reach, top - end, across + 2 * reach, down + 2 * end, color);
        } else {
            scr->fill_rectangle(scr, left - reach, top - end, across + 2 * reach, end - band + 1, color);
            scr->fill_rectangle(scr, left - reach, bottom + band, across + 2 * reach, end - band + 1, color);
        }
        /* the row of the cap the walk has left reaches as far as the walk came in it, unless the rows near the middle have it */
        if (end > 0 && reach > middle) {
            scr->fill_rectangle(scr, left - end, top - reach, across + 2 * end, 1, color);
            scr->fill_rectangle(scr, left - end, bottom + reach, across + 2 * end, 1, color);
        }
        if (!more) break;
        band = xs;
    }
    dgx_screen_progress_down(scr);
}
