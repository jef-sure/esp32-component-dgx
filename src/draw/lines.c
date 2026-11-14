/*
 * lines.c
 *
 *  Lines: the one every screen draws by its pixels and rectangles, a thick
 *  one, a dotted one, and a line through a list of points.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <math.h>

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_draw_priv.h"
#include "../dgx_hot_priv.h"

/*
 * A line by Bresenham's algorithm. The pixels that lie in one row of a
 * shallow line, or in one column of a steep one, go as one rectangle. The
 * line is always walked from its upper end, so it has the same pixels
 * whichever end it is given from.
 */
DGX_HOT void dgx_draw_line_runs(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color)
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
    dgx_screen_batch_begin(scr);
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
    dgx_screen_batch_end(scr);
}

/* ------------------------------------------------------------------------- */
/*  dgx_draw_thick_line                                                      */
/* ------------------------------------------------------------------------- */

/* a pixel is of a slanted thick line when its centre is no further than this past the edge of the line */
#define DGX_LINE_SLOP 0.01f

static inline int dgx_line_floor(float v)
{
    int i = (int)v;
    return i - (v < i);
}

/* a rectangle of runs: rows of the screen, or, across, columns of it with x and y exchanged */
static inline void dgx_line_thick_fill(dgx_screen_t *scr, bool across, int x, int y, int w, int h, uint32_t color)
{
    if (across) scr->fill_rectangle(scr, y, x, h, w, color);
    else scr->fill_rectangle(scr, x, y, w, h, color);
}

/*
 * The runs of a slanted thick line, row by row. Across, the line is given with x and y exchanged, and its rows
 * are columns of the screen: a steep line goes by columns, as few as a gentle one has rows, and equal columns
 * side by side go as one rectangle.
 */
static void dgx_line_thick_runs(dgx_screen_t *scr, int x, int y, int x2, int y2, int pw, int mw, bool across, uint32_t color)
{
    int dx = x2 - x;
    int dy = y2 - y;
    /*
     * A slanted line is the rectangle around its way, pw + 0.5 pixels to the right of the way and mw + 0.5 to
     * the left of it, as a level line has its rows, cut square at the end points. A pixel is of the line when
     * its centre lies in the rectangle, so the line is as wide at every angle. A row of it is one run, between
     * the larger of the bounds the two sides and the two ends give and the smaller one, each a straight
     * function of the row. The line is counted from its upper end, so that an odd width gives the same pixels
     * whichever end it is given from; the sides then change places.
     */
    if (dy < 0) {
        DGX_INT_SWAP(x, x2);
        DGX_INT_SWAP(y, y2);
        DGX_INT_SWAP(pw, mw);
        dx = -dx;
        dy = -dy;
    }
    /* in floats: the square of the length of a long line is more than an int holds */
    const float square = (float)dx * dx + (float)dy * dy, len = sqrtf(square);
    const float right = pw + 0.5f, left = mw + 0.5f, adx = dx < 0 ? -(float)dx : (float)dx;
    /* a row r below the upper end, columns from x: the sides lie at r * dx / dy less right * len / dy and more left * len / dy */
    const float side = (float)dx / dy, side_lo = -right * len / dy, side_hi = left * len / dy;
    /* the ends at -r * dy / dx and square / dx further */
    const float end = -(float)dy / dx, far = square / dx, end_lo = far < 0 ? far : 0, end_hi = far < 0 ? 0 : far;
    /* the rows from the corner at the upper end that lies highest to the one at the lower end that lies lowest */
    const float rise = (dx > 0 ? left : right) * adx / len, fall = (dx > 0 ? right : left) * adx / len;
    int         top = y - dgx_line_floor(rise + DGX_LINE_SLOP), bottom = y2 + dgx_line_floor(fall + DGX_LINE_SLOP);
    if (top < 0) top = 0;
    if (bottom >= (across ? scr->width : scr->height)) bottom = (across ? scr->width : scr->height) - 1;
    /* no further than the screen goes: a row of a line far off may have bounds no int holds */
    const float first = -1.0f - x, last = (float)(across ? scr->height : scr->width) - x;
    int         run_x = 0, run_y = 0, run_w = 0, run_h = 0; /* equal rows one under another not sent yet; run_h == 0: none */
    for (int row = top; row <= bottom; ++row) {
        const float r = (float)(row - y), e = r * end;
        float       lo = r * side + side_lo, hi = r * side + side_hi;
        if (e + end_lo > lo) lo = e + end_lo;
        if (e + end_hi < hi) hi = e + end_hi;
        if (lo < first) lo = first;
        if (hi > last) hi = last;
        const int from = x - dgx_line_floor(-(lo - DGX_LINE_SLOP)), to = x + dgx_line_floor(hi + DGX_LINE_SLOP);
        if (from > to) continue;
        if (run_h && run_x == from && run_w == to - from + 1 && run_y + run_h == row) {
            ++run_h;
            continue;
        }
        if (run_h) dgx_line_thick_fill(scr, across, run_x, run_y, run_w, run_h, color);
        run_x = from, run_y = row, run_w = to - from + 1, run_h = 1;
    }
    if (run_h) dgx_line_thick_fill(scr, across, run_x, run_y, run_w, run_h, color);
}

void dgx_draw_thick_line(dgx_screen_t *scr, int x1, int y1, int x2, int y2, int width, uint32_t color)
{
    if (width < 1) return;
    int dx = x2 - x1;
    int dy = y2 - y1;
    int pw = width / 2;
    int mw = width - pw - 1;
    dgx_screen_batch_begin(scr);
    if (dx == 0 && dy == 0) {
        /* a dot as wide as the line: a disc of an odd width, the nearest to one of an even width, a square of round corners */
        dgx_fill_round_rectangle(scr, x1 - pw, y1 - pw, width, width, (width - 1) / 2, color);
        dgx_screen_batch_end(scr);
        return;
    }
    if (dx == 0) {
        /* the wider side of an even width is on the right of the way, as for slanted lines */
        if (dy < 0) {
            scr->fill_rectangle(scr, x1 - mw, y2, width, -dy + 1, color);
        } else {
            scr->fill_rectangle(scr, x1 - pw, y1, width, dy + 1, color);
        }
        dgx_screen_batch_end(scr);
        return;
    }
    if (dy == 0) {
        if (dx < 0) {
            scr->fill_rectangle(scr, x2, y1 - pw, -dx + 1, width, color);
        } else {
            scr->fill_rectangle(scr, x1, y1 - mw, dx + 1, width, color);
        }
        dgx_screen_batch_end(scr);
        return;
    }
    /* a rectangle a pixel wide would have two pixels in some rows: a slanted line of one pixel is the line of dgx_draw_line() */
    if (width == 1) {
        dgx_draw_line(scr, x1, y1, x2, y2, color);
        dgx_screen_batch_end(scr);
        return;
    }
    /*
     * A steep line is drawn by columns: the same rectangle with x and y exchanged. The exchange mirrors the line
     * over the diagonal, which puts its right side on the left, so pw and mw change places.
     */
    const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (ady > adx) dgx_line_thick_runs(scr, y1, x1, y2, x2, mw, pw, true, color);
    else dgx_line_thick_runs(scr, x1, y1, x2, y2, pw, mw, false, color);
    dgx_screen_batch_end(scr);
}

/* ------------------------------------------------------------------------- */
/*  dgx_draw_polyline                                                        */
/* ------------------------------------------------------------------------- */

/*
 * A line through points: the segments of the pen between neighbors, each with a disc at its end that
 * closes the joint with the next one, as the pieces of a curve go. The first segment has a disc at its
 * start and the last one a disc at its end only when the line asks for them. A line whose last point is
 * its first is closed: the joint there is one more joint, closed by the disc at the end of the last
 * segment. A point that repeats the one before it is skipped: a segment of no length is a dot or
 * nothing, not a piece of a line.
 *
 * It lies here and not with the segments: segment.c may be kept in IRAM whole, and this is no code to
 * pay for there.
 */
void dgx_draw_polyline(dgx_screen_t *scr, const dgx_point_2d_t *points, int count, int width, uint32_t color)
{
    if (!points || count < 1 || width < 1) return;
    /* the last point that is not the one before it; 0: there is one point, however many times */
    int end = count - 1;
    while (end > 0 && points[end].x == points[end - 1].x && points[end].y == points[end - 1].y) --end;
    if (end == 0) {
        dgx_draw_thick_segment(scr, points[0].x, points[0].y, points[0].x, points[0].y, width, color);
        return;
    }
    const int pen = width & ~(DGX_PEN_START_DISC | DGX_PEN_END_DISC);
    /* a pen of one pixel not asked to be soft: the lines of the screen, which a display takes by rows and columns */
    const bool thin = pen == 1;
    const bool closed = points[0].x == points[end].x && points[0].y == points[end].y;
    /*
     * A hard pen of an even width has no middle pixel, and a segment gives the spare one to the right of
     * its own way: its axis runs half a pixel to the left of its points when it goes down and half a pixel
     * above them when it goes left, half a pixel to the right of them and below them otherwise. Two
     * segments that meet would have their axes a pixel apart then, and the disc of the joint would stick
     * out of one of them. A line has one axis for all its segments, half a pixel to the right of the
     * points and below them, where a soft pen and a dot have theirs: a segment that would run on the
     * other side is given its ends a pixel further.
     */
    const bool even_hard = !thin && !((pen & ~(DGX_PEN_SOFT | DGX_PEN_MAX)) & 1) && !dgx_pen_is_soft(scr, pen);
    int        discs = width & DGX_PEN_START_DISC; /* of the segment to come: the first one starts as the line does */
    dgx_screen_batch_begin(scr);
    for (int from = 0, to = 1; to <= end; ++to) {
        const dgx_point_2d_t a = points[from], b = points[to];
        if (a.x == b.x && a.y == b.y) continue;
        if (thin) {
            dgx_draw_line(scr, a.x, a.y, b.x, b.y, color);
        } else {
            if (to < end || closed || (width & DGX_PEN_END_DISC)) discs |= DGX_PEN_END_DISC;
            const int right = even_hard && b.y > a.y, down = even_hard && b.x < a.x;
            dgx_draw_thick_segment(scr, a.x + right, a.y + down, b.x + right, b.y + down, pen | discs, color);
        }
        discs = 0;
        from = to;
    }
    dgx_screen_batch_end(scr);
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
    dgx_screen_batch_begin(scr);
    if (adx == 0 && ady == 0) {
        scr->set_pixel(scr, x1, y1, (mask & 1u) ? color : bg);
        mask = dgx_ror_nbits(mask, mask_bits, 1);
        dgx_screen_batch_end(scr);
        return mask;
    }
    if (adx == 0) {
        for (int y = y1;; y += sy) {
            scr->set_pixel(scr, x1, y, (mask & 1u) ? color : bg);
            mask = dgx_ror_nbits(mask, mask_bits, 1);
            if (y == y2) break;
        }
        dgx_screen_batch_end(scr);
        return mask;
    }
    if (ady == 0) {
        for (int x = x1;; x += sx) {
            scr->set_pixel(scr, x, y1, (mask & 1u) ? color : bg);
            mask = dgx_ror_nbits(mask, mask_bits, 1);
            if (x == x2) break;
        }
        dgx_screen_batch_end(scr);
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
    dgx_screen_batch_end(scr);
    return mask;
}
