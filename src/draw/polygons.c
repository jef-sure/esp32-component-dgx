/*
 * polygons.c
 *
 *  A triangle and a quadrilateral filled by rows, and the outline of a
 *  triangle.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <stdint.h>

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "../dgx_hot_priv.h"

/* ------------------------------------------------------------------------- */
/*  dgx_fill_triangle                                                        */
/* ------------------------------------------------------------------------- */
#define DGX_SORT3PUD(x0, y0, x1, y1, x2, y2)  \
    do {                                       \
        if ((y0) > (y1)) {                     \
            DGX_INT_SWAP(y0, y1);              \
            DGX_INT_SWAP(x0, x1);              \
        }                                      \
        if ((y1) > (y2)) {                     \
            DGX_INT_SWAP(y2, y1);              \
            DGX_INT_SWAP(x2, x1);              \
        }                                      \
        if ((y0) > (y1)) {                     \
            DGX_INT_SWAP(y0, y1);              \
            DGX_INT_SWAP(x0, x1);              \
        }                                      \
    } while (0)

/*
 * A side of a triangle walked down its rows as dgx_draw_line() draws it: the
 * pixels of the line in each row, from the first to the last. A line is
 * walked from its upper end there, and so is a side here, by the same
 * arithmetic: the pixels are the same.
 */
typedef struct {
    int x;     /* where the side comes into the row it is in */
    int x2;    /* the x of its lower end */
    int adx, ady, sx, err;
    int steps; /* of a shallow side: the steps along x that are left */
} dgx_side_t;

/* a side from its upper end: y1 <= y2 */
static inline __attribute__((always_inline)) void dgx_side_begin(dgx_side_t *s, int x1, int y1, int x2, int y2)
{
    s->x = x1, s->x2 = x2;
    s->adx = DGX_ABS(x2 - x1), s->ady = y2 - y1;
    s->sx    = x1 < x2 ? 1 : -1;
    s->err   = s->adx - s->ady;
    s->steps = s->adx;
}

/*
 * The pixels of the side in the row it is in widen *a .. *b; the side goes on to its next row. Inline: called
 * for two or three sides in every row of a triangle, it is most of what a small triangle costs.
 */
static inline __attribute__((always_inline)) void dgx_side_row(dgx_side_t *s, int *a, int *b)
{
    const int from = s->x;
    int       to   = from;
    if (s->ady == 0) {
        /* level: the whole side lies in the row */
        to = s->x2;
    } else if (s->adx > s->ady) {
        /* shallow: the row ends where the line goes down, the last one at the end of the side */
        for (bool down = false; s->steps && !down; --s->steps) {
            const int e2 = 2 * s->err;
            s->err -= s->ady;
            if (e2 <= s->adx) {
                s->err += s->adx;
                down = true;
            } else {
                to += s->sx;
            }
            s->x += s->sx;
        }
    } else if (s->adx < s->ady) {
        /* steep: a pixel a row */
        const int e2 = 2 * s->err;
        s->err += s->adx;
        if (e2 >= -s->ady) {
            s->err -= s->ady;
            s->x += s->sx;
        }
    } else {
        s->x += s->sx; /* the diagonal */
    }
    if (from < *a) *a = from;
    if (to < *a) *a = to;
    if (from > *b) *b = from;
    if (to > *b) *b = to;
}

/*
 * A row of the triangle runs from the leftmost to the rightmost pixel its
 * sides have in that row, so the three lines of dgx_draw_triangle() are in
 * the triangle and are its border.
 */
DGX_HOT void dgx_fill_triangle(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color)
{
    DGX_SORT3PUD(x0, y0, x1, y1, x2, y2);
    dgx_side_t longest, upper, lower;
    dgx_side_begin(&longest, x0, y0, x2, y2);
    dgx_side_begin(&upper, x0, y0, x1, y1);
    dgx_side_begin(&lower, x1, y1, x2, y2);
    dgx_screen_batch_begin(scr);
    for (int y = y0; y <= y2; ++y) {
        int a = longest.x, b = a;
        dgx_side_row(&longest, &a, &b);
        if (y <= y1) dgx_side_row(&upper, &a, &b);
        if (y >= y1) dgx_side_row(&lower, &a, &b);
        scr->fill_rectangle(scr, a, y, b - a + 1, 1, color);
    }
    dgx_screen_batch_end(scr);
}

/* the outline: three lines, each the line of the screen, in one batch */
DGX_HOT void dgx_draw_triangle(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color)
{
    dgx_screen_batch_begin(scr);
    dgx_draw_line(scr, x0, y0, x1, y1, color);
    dgx_draw_line(scr, x1, y1, x2, y2, color);
    dgx_draw_line(scr, x2, y2, x0, y0, color);
    dgx_screen_batch_end(scr);
}

/* ------------------------------------------------------------------------- */
/*  dgx_fill_polygon4                                                        */
/* ------------------------------------------------------------------------- */
static int dgx_polygon4_intersection_x(int x0, int y0, int x1, int y1, int y)
{
    return x0 + (int)(((int64_t)(y - y0) * (x1 - x0)) / (y1 - y0));
}

/* which way the outline turns at vertex i: the sign of the cross product of the edge into it and the edge out of it */
static int dgx_polygon4_turn(const int vx[4], const int vy[4], int i)
{
    const int     p = (i + 3) & 3, n = (i + 1) & 3;
    const int64_t c = (int64_t)(vx[i] - vx[p]) * (vy[n] - vy[i]) - (int64_t)(vy[i] - vy[p]) * (vx[n] - vx[i]);
    return c > 0 ? 1 : c < 0 ? -1 : 0;
}

/* the vertex a concave quadrilateral turns the other way at, or -1 when the quadrilateral has none */
static int dgx_polygon4_reflex(const int vx[4], const int vy[4])
{
    int turns[4], positive = 0, negative = 0;
    for (int i = 0; i < 4; ++i) {
        turns[i] = dgx_polygon4_turn(vx, vy, i);
        positive += turns[i] > 0, negative += turns[i] < 0;
    }
    const int odd = positive >= 2 && negative == 1 ? -1 : negative >= 2 && positive == 1 ? 1 : 0;
    for (int i = 0; odd && i < 4; ++i) {
        if (turns[i] == odd) return i;
    }
    return -1;
}

void dgx_fill_polygon4(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, uint32_t color)
{
    int vx[4] = {x0, x1, x2, x3};
    int vy[4] = {y0, y1, y2, y3};
    int dirty_top = DGX_MIN(DGX_MIN(y0, y1), DGX_MIN(y2, y3));
    int dirty_bottom = DGX_MAX(DGX_MAX(y0, y1), DGX_MAX(y2, y3));

    /*
     * A concave quadrilateral is two triangles on either side of the diagonal
     * from the vertex it turns the other way at: that diagonal lies inside it.
     * The rows below take an outline with one bottom only, which a concave
     * one need not have: two tips side by side would be joined across the gap.
     */
    const int reflex = dgx_polygon4_reflex(vx, vy);
    if (reflex >= 0) {
        const int a = (reflex + 1) & 3, b = (reflex + 2) & 3, c = (reflex + 3) & 3;
        dgx_screen_batch_begin(scr);
        dgx_fill_triangle(scr, vx[reflex], vy[reflex], vx[a], vy[a], vx[b], vy[b], color);
        dgx_fill_triangle(scr, vx[reflex], vy[reflex], vx[b], vy[b], vx[c], vy[c], color);
        dgx_screen_batch_end(scr);
        return;
    }

    dgx_screen_batch_begin(scr);
    for (int y = dirty_top; y <= dirty_bottom; ++y) {
        int xs[4];
        int count = 0;

        for (int i = 0; i < 4; ++i) {
            int next = (i + 1) & 3;
            int ey0 = vy[i];
            int ey1 = vy[next];

            if (ey0 == ey1) {
                continue;
            }
            if (y >= DGX_MIN(ey0, ey1) && y < DGX_MAX(ey0, ey1)) {
                xs[count++] = dgx_polygon4_intersection_x(vx[i], ey0, vx[next], ey1, y);
            }
        }

        if (count == 0 && y == dirty_bottom) {
            int bottom_left = 0;
            int bottom_right = 0;
            bool has_bottom_vertex = false;

            for (int i = 0; i < 4; ++i) {
                if (vy[i] != y) {
                    continue;
                }
                if (!has_bottom_vertex) {
                    bottom_left = vx[i];
                    bottom_right = vx[i];
                    has_bottom_vertex = true;
                    continue;
                }
                if (vx[i] < bottom_left) bottom_left = vx[i];
                if (vx[i] > bottom_right) bottom_right = vx[i];
            }
            if (has_bottom_vertex) {
                scr->fill_rectangle(scr, bottom_left, y, bottom_right - bottom_left + 1, 1, color);
            }
            continue;
        }

        for (int i = 0; i < count; ++i) {
            for (int j = i + 1; j < count; ++j) {
                if (xs[j] < xs[i]) {
                    DGX_INT_SWAP(xs[i], xs[j]);
                }
            }
        }

        for (int i = 0; i + 1 < count; i += 2) {
            scr->fill_rectangle(scr, xs[i], y, xs[i + 1] - xs[i] + 1, 1, color);
        }
    }
    dgx_screen_batch_end(scr);
}
