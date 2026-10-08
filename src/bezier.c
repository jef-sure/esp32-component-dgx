#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "dgx_draw.h"

/* the thick polyline of a curve, drawn one piece at a time */
typedef dgx_bezier_t dgx_stroke_t;

#define DGX_STROKE_FAR  1e6f
#define DGX_STROKE_SLOP 0.01f

static inline int dgx_stroke_floor(float v)
{
    int i = (int)v;
    return i - (v < i);
}

static void dgx_stroke_flush(dgx_stroke_t *s)
{
    if (s->rh) s->scr->fill_rectangle(s->scr, s->rx, s->ry, s->rw, s->rh, s->color);
    s->rh = 0;
}

static inline void dgx_stroke_row(dgx_stroke_t *s, int y, int left, int right)
{
    int w = right - left + 1;
    if (s->rh && s->rx == left && s->rw == w && s->ry + s->rh == y) {
        ++s->rh;
        return;
    }
    dgx_stroke_flush(s);
    s->rx = left, s->ry = y, s->rw = w, s->rh = 1;
}

/*
 * A piece of a curve runs from the pen straight to (bx, by) and is len long.
 * It is one convex figure: a rectangle with a flat start and a disc of the
 * line width around its end, every row of it one interval. The disc of the
 * previous piece lies around the start, so it closes the joint whatever the
 * turn is.
 *
 * A piece may be drawn part by part. This draws the rectangle from f0 to f1
 * of its length, if f1 > f0, and a disc of diameter d around (cx, cy), if
 * d > 0: the full disc of the end, or a smaller one where the pen has stopped
 * on its way, small enough to stay inside the finished line.
 *
 * Pixel centres are whole numbers; the axis runs (ex, ey) half pixels from the
 * points.
 */
static void dgx_stroke_figure(dgx_stroke_t *s, int bx, int by, float len, float f0, float f1, int cx, int cy, int d)
{
    int   dx = bx - s->x;
    int   dy = by - s->y;
    int   ctop = 1, cbottom = 0; /* rows of the disc */
    int   rtop = 1, rbottom = 0; /* rows of the rectangle */
    /* its ends are x = elo + y * ke and x = ehi + y * ke, its sides x = side + y * ks -+ half */
    float ke = 0, elo = -DGX_STROKE_FAR, ehi = DGX_STROKE_FAR, ks = 0, side = 0, half = DGX_STROKE_FAR;
    if (d > 0) {
        ctop = cy + (s->ey - (d - 1)) / 2;
        cbottom = ctop + d - 1;
    }
    if (f1 > f0) {
        float r = s->width * 0.5f;
        float ax = s->x + s->ex * 0.5f, ay = s->y + s->ey * 0.5f;
        /* the ends of this part of the piece; a whole piece has them in its own points */
        float p0x = ax, p0y = ay, p1x = bx + s->ex * 0.5f, p1y = by + s->ey * 0.5f;
        if (f0 > 0) p0x = ax + dx * f0 / len, p0y = ay + dy * f0 / len;
        if (f1 < len) p1x = ax + dx * f1 / len, p1y = ay + dy * f1 / len;
        float reach; /* of the ends up and down from the axis */
        if (dx == 0) {
            side = ax, half = r, reach = 0;
        } else if (dy == 0) {
            elo = p0x < p1x ? p0x : p1x, ehi = p0x < p1x ? p1x : p0x, reach = r;
        } else {
            ke = -(float)dy / dx;
            ks = (float)dx / dy;
            elo = (dx > 0 ? p0x : p1x) - (dx > 0 ? p0y : p1y) * ke;
            ehi = (dx > 0 ? p1x : p0x) - (dx > 0 ? p1y : p0y) * ke;
            side = ax - ay * ks;
            half = r * len / (dy < 0 ? -dy : dy);
            reach = r * (dx < 0 ? -dx : dx) / len;
        }
        rtop = -dgx_stroke_floor(-((p0y < p1y ? p0y : p1y) - reach - DGX_STROKE_SLOP));
        rbottom = dgx_stroke_floor((p0y < p1y ? p1y : p0y) + reach + DGX_STROKE_SLOP);
    }
    int top = ctop, bottom = cbottom;
    if (rtop <= rbottom) {
        if (top > bottom || rtop < top) top = rtop;
        if (top > bottom || rbottom > bottom) bottom = rbottom;
    }
    int p = (d & 1) ^ 1; /* the chord of the disc in half pixels */
    for (int row = top; row <= bottom; ++row) {
        int left = 1, right = 0;
        if (row >= rtop && row <= rbottom) {
            float lo = elo + row * ke;
            float hi = ehi + row * ke;
            float c = side + row * ks;
            if (c - half > lo) lo = c - half;
            if (c + half < hi) hi = c + half;
            left = -dgx_stroke_floor(-(lo - DGX_STROKE_SLOP));
            right = dgx_stroke_floor(hi + DGX_STROKE_SLOP);
        }
        if (row >= ctop && row <= cbottom) {
            int q = 2 * (row - cy) - s->ey;
            int room = d * d - q * q;
            while ((p + 2) * (p + 2) <= room) p += 2;
            while (p * p > room) p -= 2;
            int cl = cx + (s->ex - p) / 2;
            int cr = cx + (s->ex + p) / 2;
            if (left > right) {
                left = cl, right = cr;
            } else {
                if (cl < left) left = cl;
                if (cr > right) right = cr;
            }
        }
        if (left <= right) dgx_stroke_row(s, row, left, right);
    }
    dgx_stroke_flush(s);
}

/*
 * A line one pixel wide: the pixels of a piece from f0 to f1 of its length.
 * Which pixels a piece has does not depend on the parts it is drawn by.
 */
static void dgx_stroke_thin(dgx_stroke_t *s, int bx, int by, float len, float f0, float f1)
{
    int dx = bx - s->x;
    int dy = by - s->y;
    int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    int from = (int)(f0 * n / len);
    int to = f1 >= len ? n : (int)(f1 * n / len);
    for (int k = from; k <= to; ++k) {
        dgx_set_pixel(s->scr, s->x + (int)lroundf((float)k * dx / n), s->y + (int)lroundf((float)k * dy / n), s->color);
    }
}

/* how far the polyline may be from the curve when the pieces are counted here, pixels */
#define DGX_BEZIER_TOLERANCE 0.5f

/*
 * Without a number of pieces it is found from the points. A chord over 1/n of
 * the parameter is not further from the curve than a eighth of its second
 * derivative over n squared; bend is the largest value of that derivative.
 */
static int dgx_bezier_pieces(int pieces, float bend)
{
    if (pieces < 1) pieces = (int)ceilf(sqrtf(bend / (8 * DGX_BEZIER_TOLERANCE)));
    if (pieces < 1) return 1;
    return pieces > INT16_MAX ? INT16_MAX : pieces;
}

static void dgx_bezier_begin(dgx_bezier_t *b, dgx_screen_t *scr, dgx_point_2d_t end, int pieces, int width, uint32_t color)
{
    b->scr = scr;
    b->color = color;
    b->width = width;
    b->x = (int)lroundf(b->kx[3]);
    b->y = (int)lroundf(b->ky[3]);
    b->ex = b->ey = 0;
    b->started = false;
    b->rh = 0;
    b->end = end;
    b->pieces = width < 1 ? 0 : pieces;
    b->done = 0;
    b->length = -1;
    b->drawn = 0;
    b->in_piece = false;
}

void dgx_bezier3_begin(dgx_bezier_t *b, dgx_screen_t *scr, const dgx_point_2d_t points[3], int pieces, int width, uint32_t color)
{
    b->kx[0] = 0;
    b->kx[1] = points[0].x - 2 * points[1].x + points[2].x;
    b->kx[2] = 2 * (points[1].x - points[0].x);
    b->kx[3] = points[0].x;
    b->ky[0] = 0;
    b->ky[1] = points[0].y - 2 * points[1].y + points[2].y;
    b->ky[2] = 2 * (points[1].y - points[0].y);
    b->ky[3] = points[0].y;
    /* the second derivative is 2 * k[1] all along */
    dgx_bezier_begin(b, scr, points[2], dgx_bezier_pieces(pieces, 2 * hypotf(b->kx[1], b->ky[1])), width, color);
}

void dgx_bezier4_begin(dgx_bezier_t *b, dgx_screen_t *scr, const dgx_point_2d_t points[4], int pieces, int width, uint32_t color)
{
    b->kx[3] = points[0].x;
    b->kx[2] = 3 * (points[1].x - points[0].x);
    b->kx[1] = 3 * (points[2].x - points[1].x) - b->kx[2];
    b->kx[0] = points[3].x - points[0].x - b->kx[2] - b->kx[1];
    b->ky[3] = points[0].y;
    b->ky[2] = 3 * (points[1].y - points[0].y);
    b->ky[1] = 3 * (points[2].y - points[1].y) - b->ky[2];
    b->ky[0] = points[3].y - points[0].y - b->ky[2] - b->ky[1];
    /* the second derivative is 6 * k[0] * t + 2 * k[1], the largest at one of the ends */
    float at_start = 2 * hypotf(b->kx[1], b->ky[1]);
    float at_end = 2 * hypotf(3 * b->kx[0] + b->kx[1], 3 * b->ky[0] + b->ky[1]);
    dgx_bezier_begin(b, scr, points[3], dgx_bezier_pieces(pieces, at_start > at_end ? at_start : at_end), width, color);
}

/* the end of a piece, pieces counted from 1: a point is ((k[0] * t + k[1]) * t + k[2]) * t + k[3] */
static void dgx_bezier_point(const dgx_bezier_t *b, int piece, int *x, int *y)
{
    if (piece >= b->pieces) {
        *x = b->end.x, *y = b->end.y;
        return;
    }
    float t = (float)piece / b->pieces;
    *x = (int)lroundf(((b->kx[0] * t + b->kx[1]) * t + b->kx[2]) * t + b->kx[3]);
    *y = (int)lroundf(((b->ky[0] * t + b->ky[1]) * t + b->ky[2]) * t + b->ky[3]);
}

float dgx_bezier_length(dgx_bezier_t *b)
{
    if (b->length < 0) {
        int x0 = (int)lroundf(b->kx[3]), y0 = (int)lroundf(b->ky[3]);
        b->length = 0;
        for (int i = 1; i <= b->pieces; ++i) {
            int x, y;
            dgx_bezier_point(b, i, &x, &y);
            b->length += hypotf((float)(x - x0), (float)(y - y0));
            x0 = x, y0 = y;
        }
    }
    return b->length;
}

/* the round start of the line, where the pen is */
static void dgx_bezier_start(dgx_bezier_t *b)
{
    b->started = true;
    if (b->width == 1) {
        dgx_set_pixel(b->scr, b->x, b->y, b->color);
        return;
    }
    if (!(b->width & 1)) {
        if (!b->ex) b->ex = 1;
        if (!b->ey) b->ey = 1;
    }
    dgx_stroke_figure(b, b->x, b->y, 0, 0, 0, b->x, b->y, b->width);
}

/*
 * Two levels: the pieces of the curve one after another, and how far the
 * piece the pen is on has got. t is turned into a way along the polyline; the
 * pieces which fit into it are drawn whole, the last one as far as it goes.
 */
bool dgx_bezier_draw_to(dgx_bezier_t *b, float t)
{
    if (b->done >= b->pieces) return true;
    bool  whole = t >= 1;
    float target = whole ? 0 : t * dgx_bezier_length(b);
    if (!whole && !(target > b->drawn)) return false;
    dgx_screen_progress_up(b->scr);
    while (b->done < b->pieces) {
        if (!b->in_piece) {
            int x, y;
            dgx_bezier_point(b, b->done + 1, &x, &y);
            int dx = x - b->x, dy = y - b->y;
            if (dx == 0 && dy == 0) {
                ++b->done;
                continue;
            }
            b->bx = x, b->by = y;
            /* the ends may be the whole range of a point apart: not to be squared in whole numbers */
            b->piece_length = sqrtf((float)dx * dx + (float)dy * dy);
            b->piece_drawn = 0;
            b->in_piece = true;
            /*
             * An even width has no middle pixel: like dgx_draw_line_thick() gives the
             * spare pixel to the right of the way, the axis runs half a pixel to the
             * right of it. The half pixel along the way only keeps the disc on whole
             * pixels.
             */
            if (!(b->width & 1)) {
                if (dx) b->ey = dx > 0 ? 1 : -1;
                if (dy) b->ex = dy > 0 ? -1 : 1;
            }
        }
        if (!b->started) dgx_bezier_start(b);
        float from = b->piece_drawn;
        float rest = b->piece_length - from;
        if (whole || target - b->drawn >= rest) {
            if (b->width == 1) dgx_stroke_thin(b, b->bx, b->by, b->piece_length, from, b->piece_length);
            else dgx_stroke_figure(b, b->bx, b->by, b->piece_length, from, b->piece_length, b->bx, b->by, b->width);
            b->drawn += rest;
            b->x = b->bx, b->y = b->by;
            b->in_piece = false;
            ++b->done;
            continue;
        }
        float to = from + target - b->drawn;
        if (b->width == 1) {
            dgx_stroke_thin(b, b->bx, b->by, b->piece_length, from, to);
        } else {
            /* where the pen has stopped the line ends with a disc two pixels smaller, it stays within the piece */
            int cx = b->x + (int)lroundf((b->bx - b->x) * to / b->piece_length);
            int cy = b->y + (int)lroundf((b->by - b->y) * to / b->piece_length);
            dgx_stroke_figure(b, b->bx, b->by, b->piece_length, from, to, cx, cy, b->width - 2);
        }
        b->piece_drawn = to;
        b->drawn = target;
        break;
    }
    /* all the points of the curve are one: a dot */
    if (b->done >= b->pieces && !b->started) dgx_bezier_start(b);
    dgx_screen_progress_down(b->scr);
    return b->done >= b->pieces;
}

void dgx_draw_bezier3(dgx_screen_t *scr, const dgx_point_2d_t points[3], int pieces, int width, uint32_t color)
{
    dgx_bezier_t b;
    dgx_bezier3_begin(&b, scr, points, pieces, width, color);
    dgx_bezier_draw_to(&b, 1);
}

void dgx_draw_bezier4(dgx_screen_t *scr, const dgx_point_2d_t points[4], int pieces, int width, uint32_t color)
{
    dgx_bezier_t b;
    dgx_bezier4_begin(&b, scr, points, pieces, width, color);
    dgx_bezier_draw_to(&b, 1);
}
