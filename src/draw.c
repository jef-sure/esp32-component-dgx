/*
 * draw.c
 *
 *  Created on: Apr 2, 2023
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include "dgx_draw.h"
#include "dgx_bits.h"
#include "esp_log.h"
#include <math.h>

static const char TAG[] = "DGX DRAW";

/*
 * Draws a rectangle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the top-left corner of the rectangle.
 * @param y The y coordinate of the top-left corner of the rectangle.
 * @param w The width of the rectangle.
 * @param h The height of the rectangle.
 * @param color The color of the rectangle.
*/
void dgx_fill_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color) {
    if (!scr->fill_rectangle) {
        ESP_LOGE(TAG, "fill_rectangle is not implemented");
        return;
    }
    scr->fill_rectangle(scr, x, y, w, h, color);
}

/*
 * Draws a circle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the center of the circle.
 * @param y The y coordinate of the center of the circle.
 * @param r The radius of the circle
 * @param color The color of the circle.
 */
void dgx_draw_circle(dgx_screen_t *scr, int x, int y, int r, uint32_t color) {
    if (!scr->circle) {
        ESP_LOGE(TAG, "circle is not implemented");
        return;
    }
    scr->circle(scr, x, y, r, color);
}

/*
 * Draws a line on the screen.
 *
 * @param scr The screen to draw on.
 * @param x1 The x coordinate of the first point of the line.
 * @param y1 The y coordinate of the first point of the line.
 * @param x2 The x coordinate of the second point of the line.
 * @param y2 The y coordinate of the second point of the line.
 * @param color The color of the line.
 */
void dgx_draw_line(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color) {
    if (!scr->draw_line) {
        ESP_LOGE(TAG, "draw_line is not implemented");
        return;
    }
    scr->draw_line(scr, x1, y1, x2, y2, color);
}

/*
 * Draws a circle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the center of the circle.
 * @param y The y coordinate of the center of the circle.
 * @param r The radius of the circle.
 * @param color The color of the circle.
*/
void dgx_solid_circle(dgx_screen_t *scr, int x, int y, int r, uint32_t color) {
    if (!scr->solid_circle) {
        ESP_LOGE(TAG, "solid_circle is not implemented");
        return;
    }
    scr->solid_circle(scr, x, y, r, color);
}

/*
 * Set a pixel on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the pixel.
 * @param y The y coordinate of the pixel.
 * @param color The color of the pixel.
*/
void dgx_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color) {
    if (!scr->set_pixel) {
        ESP_LOGE(TAG, "set_pixel is not implemented");
        return;
    }
    scr->set_pixel(scr, x, y, color);
}

/*
 * Get a pixel on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the pixel.
 * @param y The y coordinate of the pixel.
 * @return The color of the pixel.
 * @note If the screen does not have a get_pixel function, this function will
 * return 0.
*/
uint32_t dgx_get_pixel(dgx_screen_t *scr, int x, int y) {
    if (!scr->get_pixel) {
        ESP_LOGE(TAG, "set_pixel is not implemented");
        return 0;
    }
    return scr->get_pixel(scr, x, y);
}

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
        if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, a, b, y0, y0);
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
    a = b = x0;
    if (x1 < a) a = x1;
    else if (x1 > b) b = x1;
    if (x2 < a) a = x2;
    else if (x2 > b) b = x2;
    if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, a, b, y0, y2);
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
    int dirty_left = DGX_MIN(DGX_MIN(x0, x1), DGX_MIN(x2, x3));
    int dirty_right = DGX_MAX(DGX_MAX(x0, x1), DGX_MAX(x2, x3));
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
    if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, dirty_left, dirty_right, dirty_top, dirty_bottom);
}

/* ------------------------------------------------------------------------- */
/*  dgx_draw_line_thick                                                      */
/* ------------------------------------------------------------------------- */
void dgx_draw_line_thick(dgx_screen_t *scr, int x, int y, int x2, int y2, int width, uint32_t color)
{
    if (width < 1) return;
    int dx = x2 - x;
    int dy = y2 - y;
    int pw = width / 2;
    int mw = width - pw - 1;
    dgx_screen_progress_up(scr);
    if (dx == 0 && dy == 0) {
        dgx_solid_circle(scr, x, y, mw ? mw : pw, color);
        if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, x - pw, x + pw, y - pw, y + pw);
        return;
    }
    if (dx == 0) {
        if (dy < 0) {
            y = y2;
            dy = -dy;
        }
        scr->fill_rectangle(scr, x - mw, y, width, dy + 1, color);
        if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, x - mw, x + pw, y, y + dy);
        return;
    }
    if (dy == 0) {
        if (dx < 0) {
            x = x2;
            dx = -dx;
        }
        scr->fill_rectangle(scr, x, y - mw, dx + 1, width, color);
        if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, x, x + dx, y - mw, y + pw);
        return;
    }
    float s = sqrtf((float)((int32_t)dx * dx + (int32_t)dy * dy));
    int mx = (int)lroundf(mw * (int32_t)dy / s);
    int px = (int)lroundf(pw * (int32_t)-dy / s);
    int my = (int)lroundf(mw * (int32_t)-dx / s);
    int py = (int)lroundf(pw * (int32_t)dx / s);
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int err;
    int dirty_left, dirty_right, dirty_top, dirty_bottom;
    if (adx >= ady) {
        if (x > x2) {
            DGX_INT_SWAP(x, x2);
            DGX_INT_SWAP(y, y2);
            DGX_INT_SWAP(mx, px);
            DGX_INT_SWAP(my, py);
        }
        int ltx0 = x + mx, lty0 = y + my;
        int ltx1 = x + px, lty1 = y + py;
        int rtx0 = x2 + mx, rty0 = y2 + my;
        int rtx1 = x2 + px, rty1 = y2 + py;
        dirty_left   = DGX_MIN(DGX_MIN(ltx0, ltx1), DGX_MIN(rtx0, rtx1));
        dirty_right  = DGX_MAX(DGX_MAX(ltx0, ltx1), DGX_MAX(rtx0, rtx1));
        dirty_top    = DGX_MIN(DGX_MIN(lty0, lty1), DGX_MIN(rty0, rty1));
        dirty_bottom = DGX_MAX(DGX_MAX(lty0, lty1), DGX_MAX(rty0, rty1));
        int lty2, ltx2, rtx2, rty2, usy, dsy;
        if (ltx1 > ltx0) {
            ltx2 = ltx1;
            rtx2 = rtx0;
            lty2 = lty0 - (ady * (int32_t)(ltx1 - ltx0)) / adx;
            rty2 = rty1 + (ady * (int32_t)(rtx1 - rtx0)) / adx;
            int yt = y - (ady * (int32_t)(ltx1 - x)) / adx;
            usy = yt - lty2;
            dsy = lty1 - lty2 + 1;
        } else if (ltx0 > ltx1) {
            ltx2 = ltx0;
            lty2 = lty1 + (ady * (int32_t)(ltx0 - ltx1)) / adx;
            rtx2 = rtx1;
            rty2 = rty0 - (ady * (int32_t)(rtx0 - rtx1)) / adx;
            int yt = y + (ady * (int32_t)(ltx0 - x)) / adx;
            usy = yt - lty0;
            dsy = lty2 - lty0 + 1;
        } else {
            ltx2 = ltx0;
            rtx2 = rtx0;
            lty2 = lty0;
            rty2 = rty0;
            usy = y - lty0;
            dsy = lty1 - lty0 + 1;
        }
        if (ltx1 >= rtx0 || ltx0 >= rtx1) {
            dgx_draw_triangle_solid(scr, ltx0, lty0, ltx1, lty1, rtx1, rty1, color);
            dgx_draw_triangle_solid(scr, rtx0, rty0, rtx1, rty1, ltx0, lty0, color);
            if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, dirty_left, dirty_right, dirty_top, dirty_bottom);
            return;
        }
        dgx_draw_triangle_solid(scr, ltx0, lty0, ltx1, lty1, ltx2, lty2, color);
        dgx_draw_triangle_solid(scr, rtx0, rty0, rtx1, rty1, rtx2, rty2, color);
        int lx = DGX_MAX(ltx1, ltx0) + 1;
        int rx = DGX_MIN(rtx1, rtx0) - 1;
        int x1 = x;
        int y1 = y;
        int sp = x;
        int sy = y2 > y ? 1 : -1;
        err = adx / 2;
        do {
            err += ady;
            if (err >= adx || x1 == x2) {
                int lx0 = DGX_MAX(lx, sp);
                int rx0 = DGX_MIN(rx, x1);
                if (rx0 >= lx0) {
                    scr->fill_rectangle(scr, lx0, y1 - usy, rx0 - lx0 + 1, dsy, color);
                }
                sp = x1 + 1;
                y1 += sy;
                err -= adx;
            }
        } while (x1++ != x2);
    } else {
        if (y > y2) {
            DGX_INT_SWAP(x, x2);
            DGX_INT_SWAP(y, y2);
            DGX_INT_SWAP(px, mx);
            DGX_INT_SWAP(py, my);
        }
        int ltx0 = x + mx, lty0 = y + my;
        int ltx1 = x + px, lty1 = y + py;
        int rtx0 = x2 + mx, rty0 = y2 + my;
        int rtx1 = x2 + px, rty1 = y2 + py;
        dirty_left   = DGX_MIN(DGX_MIN(ltx0, ltx1), DGX_MIN(rtx0, rtx1));
        dirty_right  = DGX_MAX(DGX_MAX(ltx0, ltx1), DGX_MAX(rtx0, rtx1));
        dirty_top    = DGX_MIN(DGX_MIN(lty0, lty1), DGX_MIN(rty0, rty1));
        dirty_bottom = DGX_MAX(DGX_MAX(lty0, lty1), DGX_MAX(rty0, rty1));
        int lty2, ltx2, rtx2, rty2, ls, ds;
        if (lty1 > lty0) {
            lty2 = lty1;
            rty2 = rty0;
            ltx2 = ltx0 + (adx * (int32_t)(lty1 - lty0)) / ady;
            rtx2 = rtx1 - (adx * (int32_t)(rty1 - rty0)) / ady;
            int xt = x + (adx * (int32_t)(lty1 - y)) / ady;
            ls = xt - ltx1;
            ds = ltx2 - ltx1 + 1;
        } else if (lty0 > lty1) {
            lty2 = lty0;
            rty2 = rty1;
            ltx2 = ltx1 - (adx * (int32_t)(lty0 - lty1)) / ady;
            rtx2 = rtx0 + (adx * (int32_t)(rty0 - rty1)) / ady;
            int xt = x - (adx * (int32_t)(y - lty1)) / ady;
            ls = xt - ltx2;
            ds = ltx0 - ltx2 + 1;
        } else {
            ltx2 = ltx0;
            rtx2 = rtx0;
            lty2 = lty0;
            rty2 = rty0;
            ls = x - ltx1;
            ds = ltx0 - ltx1 + 1;
        }
        if (lty1 >= rty0 || lty0 >= rty1) {
            dgx_draw_triangle_solid(scr, ltx0, lty0, ltx1, lty1, rtx1, rty1, color);
            dgx_draw_triangle_solid(scr, rtx0, rty0, rtx1, rty1, ltx0, lty0, color);
            if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, dirty_left, dirty_right, dirty_top, dirty_bottom);
            return;
        }
        dgx_draw_triangle_solid(scr, ltx0, lty0, ltx1, lty1, ltx2, lty2, color);
        dgx_draw_triangle_solid(scr, rtx0, rty0, rtx1, rty1, rtx2, rty2, color);
        int ly = DGX_MAX(lty1, lty0) + 1;
        int ryl = DGX_MIN(rty1, rty0) - 1;
        int sx = x2 > x ? 1 : -1;
        int x1 = x;
        int y1 = y;
        int sp = y;
        err = ady / 2;
        do {
            err += adx;
            if (err >= ady || y1 == y2) {
                int ly0 = DGX_MAX(ly, sp);
                int ry0 = DGX_MIN(ryl, y1);
                if (ry0 >= ly0) {
                    scr->fill_rectangle(scr, x1 - ls, ly0, ds, ry0 - ly0 + 1, color);
                }
                sp = y1 + 1;
                x1 += sx;
                err -= ady;
            }
        } while (y1++ != y2);
    }
    if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, dirty_left, dirty_right, dirty_top, dirty_bottom);
}

/* ------------------------------------------------------------------------- */
/*  dgx_draw_line_mask  (dotted/dashed line via rotating bit pattern)        */
/* ------------------------------------------------------------------------- */
uint32_t dgx_draw_line_mask(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color, uint32_t bg, uint32_t mask,
                            uint8_t mask_bits)
{
    if (mask_bits == 0 || mask_bits > 32) return mask;
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    dgx_screen_progress_up(scr);
    int xa = x1, ya = y1;
    int xb = x2, yb = y2;
    if (xa > xb) DGX_INT_SWAP(xa, xb);
    if (ya > yb) DGX_INT_SWAP(ya, yb);
    if (adx == 0 && ady == 0) {
        scr->set_pixel(scr, x1, y1, (mask & 1u) ? color : bg);
        mask = dgx_ror_nbits(mask, mask_bits, 1);
        if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, x1, x1, y1, y1);
        return mask;
    }
    if (adx == 0) {
        for (int y = y1;; y += sy) {
            scr->set_pixel(scr, x1, y, (mask & 1u) ? color : bg);
            mask = dgx_ror_nbits(mask, mask_bits, 1);
            if (y == y2) break;
        }
        if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, x1, x1, ya, yb);
        return mask;
    }
    if (ady == 0) {
        for (int x = x1;; x += sx) {
            scr->set_pixel(scr, x, y1, (mask & 1u) ? color : bg);
            mask = dgx_ror_nbits(mask, mask_bits, 1);
            if (x == x2) break;
        }
        if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, xa, xb, y1, y1);
        return mask;
    }
    /* Diagonal: per-pixel Bresenham (no fill primitives can carry the pattern). */
    if (adx >= ady) {
        int err = adx / 2;
        int x = x1, y = y1;
        while (1) {
            scr->set_pixel(scr, x, y, (mask & 1u) ? color : bg);
            mask = dgx_ror_nbits(mask, mask_bits, 1);
            if (x == x2) break;
            x += sx;
            err -= ady;
            if (err < 0) {
                y += sy;
                err += adx;
            }
        }
    } else {
        int err = ady / 2;
        int x = x1, y = y1;
        while (1) {
            scr->set_pixel(scr, x, y, (mask & 1u) ? color : bg);
            mask = dgx_ror_nbits(mask, mask_bits, 1);
            if (y == y2) break;
            y += sy;
            err -= adx;
            if (err < 0) {
                x += sx;
                err += ady;
            }
        }
    }
    if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, xa, xb, ya, yb);
    return mask;
}
