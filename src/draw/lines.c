/*
 * lines.c
 *
 *  Lines: the one every screen draws by its pixels and rectangles, a thick
 *  one and a dotted one.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <math.h>

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_draw_priv.h"

/*
 * A line by Bresenham's algorithm. The pixels that lie in one row of a
 * shallow line, or in one column of a steep one, go as one rectangle. The
 * line is always walked from its upper end, so it has the same pixels
 * whichever end it is given from.
 */
void dgx_draw_line_runs(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color)
{
    int area_left   = DGX_MIN(x1, x2);
    int area_right  = DGX_MAX(x1, x2);
    int area_top    = DGX_MIN(y1, y2);
    int area_bottom = DGX_MAX(y1, y2);
    if (area_bottom < 0 || area_top >= scr->height || area_left >= scr->width || area_right < 0) return;
    if (x1 == x2 && y1 == y2) {
        scr->set_pixel(scr, x1, y1, color);
        return;
    }
    if (x1 == x2 || y1 == y2) {
        scr->fill_rectangle(scr, area_left, area_top, area_right - area_left + 1, area_bottom - area_top + 1, color);
        return;
    }
    if (y1 > y2) {
        DGX_INT_SWAP(x1, x2);
        DGX_INT_SWAP(y1, y2);
    }
    int adx = DGX_ABS(x2 - x1);
    int ady = y2 - y1;
    int sx  = x1 < x2 ? 1 : -1;
    int err = adx - ady;
    dgx_screen_progress_up(scr);
    if (adx == ady) {
        /* the diagonal: a pixel a row, nothing to join */
        for (; y1 <= y2; ++y1, x1 += sx) scr->set_pixel(scr, x1, y1, color);
    } else if (adx > ady) {
        /* shallow: every step goes along x, and a row ends where the line goes down */
        int run = x1;
        for (int steps = adx; steps; --steps) {
            int e2 = 2 * err;
            err -= ady;
            if (e2 <= adx) {
                dgx_draw_row_run(scr, run, x1, y1, color);
                err += adx;
                ++y1;
                run = x1 + sx;
            }
            x1 += sx;
        }
        dgx_draw_row_run(scr, run, x1, y1, color);
    } else {
        /* steep: every step goes down, and a column ends where the line goes to the side */
        int run = y1;
        for (int steps = ady; steps; --steps) {
            int e2 = 2 * err;
            err += adx;
            if (e2 >= -ady) {
                dgx_draw_column_run(scr, x1, run, y1, color);
                err -= ady;
                x1 += sx;
                run = y1 + 1;
            }
            ++y1;
        }
        dgx_draw_column_run(scr, x1, run, y1, color);
    }
    dgx_screen_progress_down(scr);
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
        dgx_screen_progress_down(scr);
        return;
    }
    if (dx == 0) {
        /* the wider side of an even width is on the right of the way, as for slanted lines */
        if (dy < 0) {
            scr->fill_rectangle(scr, x - mw, y2, width, -dy + 1, color);
        } else {
            scr->fill_rectangle(scr, x - pw, y, width, dy + 1, color);
        }
        dgx_screen_progress_down(scr);
        return;
    }
    if (dy == 0) {
        if (dx < 0) {
            scr->fill_rectangle(scr, x2, y - pw, -dx + 1, width, color);
        } else {
            scr->fill_rectangle(scr, x, y - mw, dx + 1, width, color);
        }
        dgx_screen_progress_down(scr);
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
            dgx_screen_progress_down(scr);
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
            dgx_screen_progress_down(scr);
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
    dgx_screen_progress_down(scr);
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
        dgx_screen_progress_down(scr);
        return mask;
    }
    if (adx == 0) {
        for (int y = y1;; y += sy) {
            scr->set_pixel(scr, x1, y, (mask & 1u) ? color : bg);
            mask = dgx_ror_nbits(mask, mask_bits, 1);
            if (y == y2) break;
        }
        dgx_screen_progress_down(scr);
        return mask;
    }
    if (ady == 0) {
        for (int x = x1;; x += sx) {
            scr->set_pixel(scr, x, y1, (mask & 1u) ? color : bg);
            mask = dgx_ror_nbits(mask, mask_bits, 1);
            if (x == x2) break;
        }
        dgx_screen_progress_down(scr);
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
    dgx_screen_progress_down(scr);
    return mask;
}
