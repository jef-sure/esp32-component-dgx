/*
 * polygons.c
 *
 *  A solid triangle and a solid quadrilateral, filled by rows.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <stdint.h>

#include "dgx_bits.h"
#include "dgx_draw.h"

/* ------------------------------------------------------------------------- */
/*  dgx_draw_triangle_solid                                                  */
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

void dgx_draw_triangle_solid(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color)
{
    int a, b, y, last;
    DGX_SORT3PUD(x0, y0, x1, y1, x2, y2);
    dgx_screen_progress_up(scr);
    if (y0 == y2) {
        a = b = x0;
        if (x1 < a) a = x1;
        else if (x1 > b) b = x1;
        if (x2 < a) a = x2;
        else if (x2 > b) b = x2;
        scr->fill_rectangle(scr, a, y0, b - a + 1, 1, color);
        dgx_screen_progress_down(scr);
        return;
    }
    int dx01 = x1 - x0;
    int dy01 = y1 - y0;
    int dx02 = x2 - x0;
    int dy02 = y2 - y0;
    int dx12 = x2 - x1;
    int dy12 = y2 - y1;
    int32_t sa = 0;
    int32_t sb = 0;
    last = (y1 == y2) ? y1 : y1 - 1;
    for (y = y0; y <= last; y++) {
        a = x0 + sa / dy01;
        b = x0 + sb / dy02;
        sa += dx01;
        sb += dx02;
        if (a > b) DGX_INT_SWAP(a, b);
        scr->fill_rectangle(scr, a, y, b - a + 1, 1, color);
    }
    sa = (int32_t)dx12 * (y - y1);
    sb = (int32_t)dx02 * (y - y0);
    for (; y <= y2; y++) {
        a = x1 + sa / dy12;
        b = x0 + sb / dy02;
        sa += dx12;
        sb += dx02;
        if (a > b) DGX_INT_SWAP(a, b);
        scr->fill_rectangle(scr, a, y, b - a + 1, 1, color);
    }
    dgx_screen_progress_down(scr);
}

/* ------------------------------------------------------------------------- */
/*  dgx_draw_polygon4_solid                                                  */
/* ------------------------------------------------------------------------- */
static int dgx_polygon4_intersection_x(int x0, int y0, int x1, int y1, int y)
{
    return x0 + (int)(((int64_t)(y - y0) * (x1 - x0)) / (y1 - y0));
}

void dgx_draw_polygon4_solid(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3,
                             uint32_t color)
{
    int vx[4] = {x0, x1, x2, x3};
    int vy[4] = {y0, y1, y2, y3};
    int dirty_top = DGX_MIN(DGX_MIN(y0, y1), DGX_MIN(y2, y3));
    int dirty_bottom = DGX_MAX(DGX_MAX(y0, y1), DGX_MAX(y2, y3));

    dgx_screen_progress_up(scr);
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
    dgx_screen_progress_down(scr);
}
