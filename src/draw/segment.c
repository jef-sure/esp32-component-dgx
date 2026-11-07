/*
 * segment.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  A straight segment by a pen of a width: the figures of the pen of the
 *  curves, the hard one and the soft one, on one straight piece. They are a
 *  copy of those of bezier.c, which the curves keep: drawn by these they came
 *  out slower on an ESP32 from flash. A fix of a figure is a fix of both. How
 *  the figures are made and what they cost is in docs/pen-en.md.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dgx_draw.h"
#include "drivers/vscreen.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/*
 * What a pen does only on a virtual screen; without virtual screens both are 0. The soft pen has
 * functions of its own, and a plain if before their calls is enough for the compiler to leave them
 * out. The pen into the memory lies inside the loops of the hard pen: an if there, even one that is
 * never true, makes the compiler arrange the hard pen differently, so it is cut out by #if.
 */
#if !defined(ESP_PLATFORM) || defined(CONFIG_DGX_ENABLE_VSCREEN)
#define DGX_STROKE_SOFT   1 /* a pen with a soft edge */
#define DGX_STROKE_MEMORY 1 /* a pen that writes into the memory of a virtual screen itself */
#else
#define DGX_STROKE_SOFT   0
#define DGX_STROKE_MEMORY 0
#endif

/* what may come in the width of a pen besides the width */
#define DGX_PEN_BITS (DGX_PEN_SOFT | DGX_PEN_START_DISC | DGX_PEN_END_DISC)

/* the state of a pen: that of a curve, whose pen this is; a segment fills what its figures use */
typedef dgx_bezier_t dgx_stroke_t;

/* a pixel is of a figure of the hard pen when its centre is no further than this past its edge */
#define DGX_STROKE_SLOP 0.01f
/* along a piece and across it pixels are counted in whole numbers, so much to a pixel */
#define DGX_STROKE_Q    13
#define DGX_STROKE_ONE  (1 << DGX_STROKE_Q)
/* a pixel this near to the end of a piece is given both to the piece and to the disc at that end: the two are not counted the same way */
#define DGX_STROKE_SEAM (1.0f / 64)

static inline int dgx_stroke_floor(float v)
{
    int i = (int)v;
    return i - (v < i);
}

/*
 * An even pen has no middle pixel, and around the centre of a pixel it would
 * spread over three. Its line runs half a pixel to the right and down: the
 * whole points, the ends of a curve and a dot among them, lie between pixels
 * then, where the dot of a hard pen lies.
 */
static inline float dgx_stroke_half(int width)
{
    return width & 1 ? 0 : 0.5f;
}

/*
 * The pieces of a soft line are many and small, and on the ESP32 a call out
 * of the code that draws them may cost more than all its own work: the
 * functions of the C library lie elsewhere in flash, and a miss of its cache
 * takes the time of hundreds of instructions. So nothing is called from
 * here. The one over the square root of a number, by the well-known guess
 * from its bits and two steps of Newton, within 5 parts of a million: it
 * gives the root, and the one over a number as well.
 */
static inline float dgx_stroke_rsqrt(float v)
{
    union {
        float    f;
        uint32_t i;
    } u = {v};
    u.i = 0x5f375a86u - (u.i >> 1);
    float y = u.f;
    y *= 1.5f - 0.5f * v * y * y;
    y *= 1.5f - 0.5f * v * y * y;
    return y;
}

static inline float dgx_stroke_one_over(float v)
{
    float y = dgx_stroke_rsqrt(v * v);
    return v < 0 ? -y : y;
}

/*
 * What lroundf() gives for a number that fits an int, found in place: a half goes away from 0. The
 * functions of the C library lie elsewhere in flash, and a pen that calls one for every point of
 * every piece may wait for it to be read; see docs/pen-en.md.
 */
static inline int dgx_stroke_round(float v)
{
    const int   whole = (int)v;
    const float rest = v - (float)whole;
    return rest >= 0.5f ? whole + 1 : rest <= -0.5f ? whole - 1 : whole;
}

/*
 * How long a piece of (dx, dy) is. The ends may be the whole range of a point apart: not to be squared
 * in whole numbers. The root is that of the library: one found in place may differ from it in the last
 * bit, and a pixel that lies on the very edge of a line would then come and go.
 */
static inline float dgx_stroke_long(int dx, int dy)
{
    return sqrtf((float)dx * dx + (float)dy * dy);
}

/*
 * The pen of a width, its bits among them, and a color on a screen: which of the two pens it is there,
 * and whether it writes into the memory of the screen itself. Fills scr, color, width, soft and direct.
 * A width below 1 is no pen at all, whatever bits it has: false.
 */
static inline bool dgx_stroke_set(dgx_stroke_t *s, dgx_screen_t *scr, int width, uint32_t color)
{
    /* the kind of the pen comes with its width; a width below 1 is no pen at all, whatever bits it has */
    const bool asked = width > 0 && (width & DGX_PEN_SOFT);
    if (width > 0) width &= ~DGX_PEN_BITS;
    s->scr = scr;
    s->color = color;
    s->width = width > INT16_MAX ? INT16_MAX : width;
    s->soft = false;
    s->direct = 0;
#if DGX_STROKE_MEMORY
    if ((scr->color_bits == 8 || scr->color_bits == 16) && dgx_vscreen_is_linear(scr)) s->direct = scr->color_bits / 8;
#endif
    /*
     * A soft edge is a share of the ink in a pixel, and where lines meet the larger share stays. Only
     * a pixel that is an amount of ink can do it, and that is a byte of a virtual screen. A pixel of
     * colors has nothing to take the larger of, and a display behind a bus cannot be read: there the
     * pen is hard whatever is asked for.
     */
    s->soft = DGX_STROKE_SOFT && asked && width >= 1 && scr->color_bits == 8 && dgx_vscreen_is_linear(scr);
    return width >= 1;
}

#define DGX_STROKE_FAR 1e6f
/* a row of fewer pixels is filled in place: to call memset() for it costs more than to fill it */
#define DGX_STROKE_SHORT 8

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
    /* the disc of a pen stopped on its way is cut by the start of its piece, x = tip_x + y * ke: the side that stays, or the rows */
    bool  tip = false;
    int   tip_side = 0, tip_first = INT32_MIN, tip_last = INT32_MAX;
    float tip_x = 0;
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
        /*
         * What the disc of a stopped pen has behind the start of its piece lies within the disc of the
         * piece before when the two pieces have their axes on the same pixels, and may be a pixel out of
         * it when they have not: a pen of an even width has its axis half a pixel to the right of the
         * way, and the way has turned. Cut by the start, the disc leaves nothing that the whole curve
         * does not have.
         */
        if (d > 0 && f1 < len) {
            tip = true;
            if (dx) {
                tip_side = dx > 0 ? 1 : -1;
                tip_x = ax - ay * ke;
            } else if (dy > 0) {
                tip_first = -dgx_stroke_floor(-(ay - DGX_STROKE_SLOP));
            } else {
                tip_last = dgx_stroke_floor(ay + DGX_STROKE_SLOP);
            }
        }
    }
    int top = ctop, bottom = cbottom;
    if (rtop <= rbottom) {
        if (top > bottom || rtop < top) top = rtop;
        if (top > bottom || rbottom > bottom) bottom = rbottom;
    }
    /* a pen of any width may be asked for: only the rows of the screen are gone through */
    if (top < 0) top = 0;
    if (bottom >= s->scr->height) bottom = s->scr->height - 1;
    int        chord = (d & 1) ^ 1; /* of the disc, in half pixels */
    int64_t    chord_wide = chord;  /* the same for a disc whose square is too large for 32 bits */
    const bool wide = d > 23000;
#if DGX_STROKE_MEMORY
    /*
     * A virtual screen of whole bytes a pixel has its rows put here, not asked for one by one: a figure
     * of a small text is a few rows of a few pixels, and to ask for a row costs more than to fill it.
     */
    const int bytes = s->direct, width = s->scr->width;
    uint8_t  *line = bytes ? ((dgx_vscreen_t *)s->scr)->v_array + (size_t)top * (size_t)width * (size_t)bytes : NULL;
    int       touched_left = width, touched_right = -1;
#endif
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
            if (!wide) {
                const int q = 2 * (row - cy) - s->ey, room = d * d - q * q;
                /* the first row of a disc cut by the screen: the chord is found at once */
                if (row == top && row > ctop) chord = ((int)sqrt((double)room) & ~1) | ((d & 1) ^ 1);
                while ((chord + 2) * (chord + 2) <= room) chord += 2;
                while (chord > 0 && chord * chord > room) chord -= 2;
            } else {
                const int64_t q = 2 * (int64_t)(row - cy) - s->ey, room = (int64_t)d * d - q * q;
                if (row == top && row > ctop) chord_wide = ((int64_t)sqrt((double)room) & ~(int64_t)1) | ((d & 1) ^ 1);
                while ((chord_wide + 2) * (chord_wide + 2) <= room) chord_wide += 2;
                while (chord_wide > 0 && chord_wide * chord_wide > room) chord_wide -= 2;
                chord = (int)chord_wide;
            }
            int cl = cx + (s->ex - chord) / 2;
            int cr = cx + (s->ex + chord) / 2;
            if (tip) {
                if (tip_side > 0) {
                    const int edge = -dgx_stroke_floor(-(tip_x + row * ke - DGX_STROKE_SLOP));
                    if (cl < edge) cl = edge;
                } else if (tip_side < 0) {
                    const int edge = dgx_stroke_floor(tip_x + row * ke + DGX_STROKE_SLOP);
                    if (cr > edge) cr = edge;
                } else if (row < tip_first || row > tip_last) {
                    cl = 1, cr = 0;
                }
            }
            if (cl > cr) {
                /* nothing of the disc is left in this row */
            } else if (left > right) {
                left = cl, right = cr;
            } else {
                if (cl < left) left = cl;
                if (cr > right) right = cr;
            }
        }
#if DGX_STROKE_MEMORY
        if (bytes) {
            if (left < 0) left = 0;
            if (right >= width) right = width - 1;
            if (left <= right) {
                if (bytes == 1) {
                    const uint8_t ink = (uint8_t)s->color;
                    if (right - left < DGX_STROKE_SHORT) {
                        /* the empty statement keeps the compiler from making a call of memset() of this loop */
                        for (uint8_t *q = line + left, *stop = line + right; q <= stop; ++q) {
                            *q = ink;
                            __asm__ volatile("");
                        }
                    } else {
                        memset(line + left, ink, (size_t)(right - left + 1));
                    }
                } else {
                    const uint8_t hi = (uint8_t)(s->color >> 8), lo = (uint8_t)s->color;
                    for (uint8_t *q = line + 2 * left, *stop = line + 2 * right; q <= stop; q += 2) q[0] = hi, q[1] = lo;
                }
                if (left < touched_left) touched_left = left;
                if (right > touched_right) touched_right = right;
            }
            line += (size_t)width * (size_t)bytes;
            continue;
        }
#endif
        if (left <= right) dgx_stroke_row(s, row, left, right);
    }
#if DGX_STROKE_MEMORY
    if (bytes) {
        if (touched_left <= touched_right) dgx_screen_touch(s->scr, touched_left, touched_right, top, bottom);
        return;
    }
#endif
    dgx_stroke_flush(s);
}

/* to find where a row of a bar begins and ends pays when it saves looking at so many pixels beside it */
#ifndef DGX_STROKE_BESIDE
#define DGX_STROKE_BESIDE 5.0f
#endif

static inline int dgx_stroke_fixed(float v)
{
    return (int)(v * DGX_STROKE_ONE + (v < 0 ? -0.5f : 0.5f));
}

/*
 * A pen with a soft edge, DGX_PEN_SOFT, on a virtual screen of a byte a
 * pixel: a byte is an amount of ink there. A pixel a hard pen takes or leaves
 * by its centre gets here the share of it the figure covers, taken as 0.5 +
 * how deep its centre is inside the figure, within 0 .. 1: half a pixel
 * either way from the edge. A pixel keeps the larger of the ink it has and
 * the ink it gets, so what is drawn twice is the same as drawn once, and a
 * line that meets or crosses another gets no darker there. With color 0 the
 * ink is taken off every pixel the pen with all the ink would leave any on.
 *
 * Nothing here needs whole pixels, and the points of the polyline are not
 * brought to them. Rounded points are what makes a thin line heavy and
 * uneven, more than its hard edge does.
 */
static inline void dgx_stroke_put(uint8_t *p, int ink, bool erase)
{
    if (erase) {
        if (ink) *p = 0;
    } else if (ink > *p) {
        *p = (uint8_t)ink;
    }
}

/* the pixels of a row of a bar from the column `left` to `right`, the counts of the first of them given */
static inline __attribute__((always_inline)) void dgx_stroke_bar_run(uint8_t *line, int left, int right, int along, int across, int uxi, int uyi, int from,
                                                                     uint32_t span, int soft, bool stopped, int to, int ink_all, bool erase)
{
    const int half = DGX_STROKE_ONE / 2;
    for (uint8_t *p = line + left, *stop = line + right; p <= stop; ++p, along += uxi, across += uyi) {
        if ((uint32_t)(along - from) > span) continue;
        int share = soft - (across < 0 ? -across : across);
        if (stopped && half + (to - along) < share) share = half + (to - along);
        if (share <= 0) continue;
        dgx_stroke_put(p, share >= DGX_STROKE_ONE ? ink_all : (share * ink_all + half) >> DGX_STROKE_Q, erase);
    }
}

/*
 * The least or the most column of a row where a count stays within a limit: a whole number of steps of
 * the count, the numerator over the step kept as its whole part and what is left, 0 <= r < d. A row down
 * the numerator moves by the same amount, so the bound is moved by additions, exactly.
 */
typedef struct {
    int q, r;   /* the whole part and what is left */
    int d;      /* the step of the count along the row, made positive */
    int dq, dr; /* how the two move a row down */
} dgx_stroke_bound_t;

static void dgx_stroke_bound_set(dgx_stroke_bound_t *b, int n, int d, int move)
{
    b->d = d;
    b->q = n / d, b->r = n % d;
    if (b->r < 0) b->r += d, --b->q;
    b->dq = move / d, b->dr = move % d;
    if (b->dr < 0) b->dr += d, --b->dq;
}

static inline void dgx_stroke_bound_next(dgx_stroke_bound_t *b)
{
    b->q += b->dq, b->r += b->dr;
    if (b->r >= b->d) b->r -= b->d, ++b->q;
}

/*
 * The columns k of a row where lo <= count + k * step <= hi, step not 0, the count moving by `move` a row
 * down: from the least, the numerator over the step brought up, to the most, brought down.
 */
static void dgx_stroke_bounds(dgx_stroke_bound_t *least, dgx_stroke_bound_t *most, int count, int move, int step, int lo, int hi)
{
    if (step > 0) {
        dgx_stroke_bound_set(least, lo - count, step, -move);
        dgx_stroke_bound_set(most, hi - count, step, -move);
    } else {
        dgx_stroke_bound_set(least, count - hi, -step, move);
        dgx_stroke_bound_set(most, count - lo, -step, move);
    }
}

/* what the rows of a long bar are found and drawn with, see dgx_stroke_bar() */
typedef struct {
    dgx_screen_t *scr;
    uint8_t      *line;                         /* the row `top` of the memory of the screen */
    int           width, top, bottom, most_left, most_right;
    int           along_row, across_row;        /* the counts of the column most_left in the row `top` */
    int           uxi, uyi, from, to, soft, ink_all;
    uint32_t      span;
    bool          stopped, erase;
} dgx_stroke_rows_t;

/*
 * The rows of a long bar. A pixel has ink when along lies from `from` to the end of the bar, and before the
 * place the pen has stopped at, and across within soft either way. Both counts are straight functions of
 * the column, so each of the four limits gives the least or the most column of a row. A function of its own:
 * in the function of the bar what finds the rows takes registers from the loop of the whole box, which is
 * what nearly all pieces of a small text are drawn by.
 */
static __attribute__((noinline)) void dgx_stroke_bar_rows(const dgx_stroke_rows_t *r)
{
    const int      uxi = r->uxi, uyi = r->uyi, from = r->from, to = r->to, soft = r->soft, ink_all = r->ink_all, width = r->width;
    const int      most_left = r->most_left, most_right = r->most_right, half = DGX_STROKE_ONE / 2;
    const uint32_t span = r->span;
    const bool     stopped = r->stopped, erase = r->erase;
    const int      stop = half + to, along_last = stopped && stop - 1 < from + (int)span ? stop - 1 : from + (int)span;
    const int      columns = most_right - most_left;
    uint8_t       *line = r->line;
    int            along_row = r->along_row, across_row = r->across_row;
    /* along_row moves by uyi a row and across_row by -uxi; a count that does not change along a row is within or not for all of it */
    dgx_stroke_bound_t along_least, along_most, across_least, across_most;
    if (uxi) dgx_stroke_bounds(&along_least, &along_most, along_row, uyi, uxi, from, along_last);
    if (uyi) dgx_stroke_bounds(&across_least, &across_most, across_row, -uxi, uyi, 1 - soft, soft - 1);
    int touched_left = most_right, touched_right = most_left;
    for (int row = r->top; row <= r->bottom; ++row, line += width, along_row += uyi, across_row -= uxi) {
        int k_lo = 0, k_hi = columns;
        if (uxi) {
            const int least = along_least.q + (along_least.r != 0), most = along_most.q;
            if (least > k_lo) k_lo = least;
            if (most < k_hi) k_hi = most;
            dgx_stroke_bound_next(&along_least), dgx_stroke_bound_next(&along_most);
        } else if (along_row < from || along_row > along_last) {
            k_hi = -1;
        }
        if (uyi) {
            const int least = across_least.q + (across_least.r != 0), most = across_most.q;
            if (least > k_lo) k_lo = least;
            if (most < k_hi) k_hi = most;
            dgx_stroke_bound_next(&across_least), dgx_stroke_bound_next(&across_most);
        } else if (across_row <= -soft || across_row >= soft) {
            k_hi = -1;
        }
        if (k_lo > k_hi) continue;
        const int left = most_left + k_lo, right = most_left + k_hi;
        if (left < touched_left) touched_left = left;
        if (right > touched_right) touched_right = right;
        dgx_stroke_bar_run(line, left, right, along_row + k_lo * uxi, across_row + k_lo * uyi, uxi, uyi, from, span, soft, stopped, to, ink_all, erase);
    }
    if (touched_left <= touched_right) dgx_screen_touch(r->scr, touched_left, touched_right, r->top, r->bottom);
}

/*
 * A piece: a bar as wide as the pen from (ax, ay) along (ux, uy), a vector
 * of length 1. It runs from `start` to `pen`, or to `end` when the pen has
 * passed the piece, pen >= len. Only its sides are soft; its ends are cut
 * straight, what lies past them is another figure's to give. Where the pen
 * has stopped on its way the bar ends softly.
 *
 * How far a pixel is along the piece and across it is counted in whole
 * numbers from the pixel the piece starts in: a step to the next pixel is an
 * addition, and the number does not depend on where the count began. So the
 * share of a pixel depends on the piece and the pixel alone, not on the part
 * being drawn, but for the soft end of a part. A part begins behind the
 * place the part before it has stopped at, where that part has put its whole
 * shares, and so the parts together give what the piece gives at once.
 *
 * Of a long bar not the whole box it lies in is looked at but its rows, and
 * where a row begins and ends is found from the counts themselves, not from
 * the bar as the floats have it: the counts are what gives a pixel its ink,
 * and over a long way they drift a little from it. A count is a straight
 * function of the column, and each of its limits gives a bound of the row: a
 * numerator over the step of the count, brought up or down to a whole column.
 * It is kept as a whole number and what is left and moved from row to row by
 * additions. So a row has the pixels the whole box would give it, no more and
 * no fewer, and no row costs a float.
 */
static void dgx_stroke_bar(dgx_stroke_t *s, float ax, float ay, float ux, float uy, float len, float start, float pen, float end)
{
    const float edge = 0.5f + s->width * 0.5f, wx = ux < 0 ? -ux : ux, wy = uy < 0 ? -uy : uy;
    const bool  erase = (s->color & 0xff) == 0, stopped = pen < len;
    const int   width = s->scr->width, last = width - 1;
    /* how far along the piece ink is put: the soft end of a part does not go past the end of the piece */
    const float reach = stopped && pen + 0.5f < end ? pen + 0.5f : end;
    const float x0 = ax + start * ux, x1 = ax + reach * ux, y0 = ay + start * uy, y1 = ay + reach * uy;
    const float out_x = edge * wy, out_y = edge * wx;
    int         top = -dgx_stroke_floor(-((y0 < y1 ? y0 : y1) - out_y)), bottom = dgx_stroke_floor((y0 < y1 ? y1 : y0) + out_y);
    /* a pen of any width at any place may be asked for: only the pixels of the screen are gone through */
    if (top < 0) top = 0;
    if (bottom >= s->scr->height) bottom = s->scr->height - 1;
    if (top > bottom) return;
    /* the columns the bar lies between */
    float       most_lo = (x0 < x1 ? x0 : x1) - out_x, most_hi = (x0 < x1 ? x1 : x0) + out_x;
    if (most_lo < 0) most_lo = 0;
    if (most_hi > last) most_hi = last;
    if (!(most_lo <= most_hi)) return;
    const int most_left = -dgx_stroke_floor(-most_lo), most_right = (int)most_hi;
    if (most_left > most_right) return;

    /* the pixel the count starts from, and the count for a pixel (x, y): along = (x - ox) * uxi + (y - oy) * uyi - along0 */
    const int   ox = dgx_stroke_floor(ax), oy = dgx_stroke_floor(ay);
    const float bx = ax - ox, by = ay - oy;
    const int   uxi = dgx_stroke_fixed(ux), uyi = dgx_stroke_fixed(uy);
    const int   along0 = (int)((bx * ux + by * uy) * DGX_STROKE_ONE), across0 = (int)((bx * uy - by * ux) * DGX_STROKE_ONE);
    const int   from = (int)(start * DGX_STROKE_ONE), to = (int)(pen * DGX_STROKE_ONE), soft = (int)(edge * DGX_STROKE_ONE);
    const int   ink_all = erase ? 255 : (int)(s->color & 0xff);
    const uint32_t span = (uint32_t)((int)(reach * DGX_STROKE_ONE) - from);
    /*
     * A row of the bar is so many pixels long: as long as the bar is thick along a row, or as long as
     * it is itself, whichever is shorter. When the columns the whole bar lies between are few more than
     * that, every row is looked at from the first of them to the last: to find where a row begins and
     * ends would cost more than to look at the pixels beside it. A bar along a row or a column is
     * always so.
     */
    const float box = most_hi - most_lo;
    const bool  rows = (box - DGX_STROKE_BESIDE) * wy > 2 * edge || (box - DGX_STROKE_BESIDE) * wx > reach - start;
    uint8_t *line = ((dgx_vscreen_t *)s->scr)->v_array + (size_t)top * (size_t)width;
    /* the count for the first of the columns in the row `top`; a row down, the counts move by uyi and by -uxi */
    int      along_row = (most_left - ox) * uxi + (top - oy) * uyi - along0, across_row = (most_left - ox) * uyi - (top - oy) * uxi - across0;
    if (rows) {
        const dgx_stroke_rows_t r = {s->scr, line, width, top, bottom, most_left, most_right, along_row, across_row, uxi, uyi, from,
                                     to, soft, ink_all, span, stopped, erase};
        dgx_stroke_bar_rows(&r);
        return;
    }
    for (int row = top; row <= bottom; ++row, line += width, along_row += uyi, across_row -= uxi)
        dgx_stroke_bar_run(line, most_left, most_right, along_row, across_row, uxi, uyi, from, span, soft, stopped, to, ink_all, erase);
    dgx_screen_touch(s->scr, most_left, most_right, top, bottom);
}

/*
 * A disc of diameter d around (cx, cy): the round end of a line, the round
 * outside of a joint, the pen where it has stopped, a dot. Its edge is
 * soft. Of a disc at an end of a piece only what lies past that end is
 * looked at, and of the disc of a joint only what lies between the two
 * pieces: a pixel is looked at when it is not further than `first_limit`
 * along the vector `first` from the centre, and not further than
 * DGX_STROKE_SEAM along `second`; a vector that is not given says nothing.
 *
 * It is counted in whole numbers too, 64 to a pixel, or fewer when the disc
 * is too large for the square of its radius to fit. Only the pixels of the
 * soft edge take a square root, and those of a disc of 5 pixels and more
 * take two terms of a series in its place.
 */
static void dgx_stroke_disc(dgx_stroke_t *s, float cx, float cy, int d, const float *first, float first_limit, const float *second)
{
    const float dr = d * 0.5f, reach = dr + 0.5f;
    const bool  erase = (s->color & 0xff) == 0;
    const int   ink_all = erase ? 255 : (int)(s->color & 0xff);
    const int   last = s->scr->width - 1;
    /* how far from the centre the pixels to look at lie: left, right, up and down */
    float       west = reach, east = reach, north = reach, south = reach;
    if (d < 1) return;
    /*
     * What is cut off the disc by a vector is not looked at. Along the vector the disc ends at the limit;
     * across it, where its chord at the limit ends, and that is no further than the limit and the radius
     * by the other part of the vector, its length being 1.
     */
    for (int i = 0; i < 2; ++i) {
        const float *v = i ? second : first;
        if (!v) continue;
        const float limit = i ? DGX_STROKE_SEAM : first_limit;
        const float by_x = reach * (v[1] < 0 ? -v[1] : v[1]) + limit, by_y = reach * (v[0] < 0 ? -v[0] : v[0]) + limit;
        if (v[0] > 0 && by_x < east) east = by_x;
        if (v[0] < 0 && by_x < west) west = by_x;
        if (v[1] > 0 && by_y < south) south = by_y;
        if (v[1] < 0 && by_y < north) north = by_y;
    }
    float lo = cx - west, hi = cx + east;
    /* a pen of any width at any place may be asked for: only the pixels of the screen are gone through */
    if (lo < 0) lo = 0;
    if (hi > last) hi = last;
    if (!(lo <= hi)) return;
    const int left = -dgx_stroke_floor(-lo), right = (int)hi;
    int       top = -dgx_stroke_floor(-(cy - north)), bottom = dgx_stroke_floor(cy + south);
    if (top < 0) top = 0;
    if (bottom >= s->scr->height) bottom = s->scr->height - 1;
    if (left > right || top > bottom) return;
    /* so many to a pixel that the square of the way from the centre to the edge fits a whole number */
    int   q = 6;
    while (q > 0 && reach * (float)(1 << q) > 46000.0f) --q;
    const int   one = 1 << q;
    const float unit = (float)one, back = (float)(1 << (6 - q)) * (1.0f / 64), full = (float)ink_all;
    const int   cxi = dgx_stroke_floor(cx * unit + 0.5f), cyi = dgx_stroke_floor(cy * unit + 0.5f);
    const int   inner = dr > 0.5f ? (int)((dr - 0.5f) * unit) : 0, outer = (int)(reach * unit);
    const int   in2 = inner * inner, out2 = outer * outer;
    /*
     * The soft edge of a disc of 5 pixels and more is thin beside its radius, and the way of a pixel
     * from the centre is found without a root, from how much its square differs from the square of the
     * radius, mid2: way = radius + more / (2 * radius) - more * more / (8 * radius cubed), within a
     * hundredth of a pixel. The ink is what half a pixel less that difference of ways gives.
     */
    const bool  wide = dr >= 2.5f && q == 6;
    const float mid = dr * unit, over_mid = wide ? dgx_stroke_one_over(mid) : 0;
    const int   mid2 = (int)(mid * mid);
    const float half_ink = 0.5f * full + 0.5f, ring1 = 0.5f * over_mid * back * full, ring2 = 0.125f * over_mid * over_mid * over_mid * back * full;
    /*
     * How far the first pixel of the row `top` is along each vector past what is allowed, in the measure
     * of the bars and from the centre as it is, not as it is counted here: this is where a disc meets a bar.
     */
    int         first_x = 0, first_y = 0, first_row = 0, second_x = 0, second_y = 0, second_row = 0;
    if (first) {
        first_x = dgx_stroke_fixed(first[0]), first_y = dgx_stroke_fixed(first[1]);
        first_row = dgx_stroke_fixed((left - cx) * first[0] + (top - cy) * first[1] - first_limit);
    }
    if (second) {
        second_x = dgx_stroke_fixed(second[0]), second_y = dgx_stroke_fixed(second[1]);
        second_row = dgx_stroke_fixed((left - cx) * second[0] + (top - cy) * second[1] - DGX_STROKE_SEAM);
    }
    uint8_t *line = ((dgx_vscreen_t *)s->scr)->v_array + (size_t)top * (size_t)s->scr->width;
    const int off0 = left * one - cxi;
    for (int row = top; row <= bottom; ++row, line += s->scr->width, first_row += first_y, second_row += second_y) {
        const int down = row * one - cyi, dy2 = down * down;
        if (dy2 >= out2) continue;
        int off = off0, on_first = first_row, on_second = second_row;
        for (uint8_t *p = line + left, *stop = line + right; p <= stop; ++p, off += one, on_first += first_x, on_second += second_x) {
            if (on_first > 0 || on_second > 0) continue;
            /* towards the corners of the box the sum of the squares may not fit an int; past the edge it is not needed */
            const uint32_t far = (uint32_t)(off * off) + (uint32_t)dy2;
            if (far >= (uint32_t)out2) continue;
            const int d2  = (int)far;
            int       ink = ink_all;
            if (d2 > in2) {
                if (wide) {
                    const float more = (float)(d2 - mid2);
                    ink = (int)(half_ink - more * (ring1 - more * ring2));
                } else {
                    const float square = (float)d2;
                    ink = (int)((reach - square * dgx_stroke_rsqrt(square) * back) * full + 0.5f);
                }
                if (ink <= 0) continue;
            }
            dgx_stroke_put(p, ink, erase);
        }
    }
    dgx_screen_touch(s->scr, left, right, top, bottom);
}

/*
 * A line one pixel wide: the pixels of a piece from f0 to f1 of its length.
 * Which pixels a piece has does not depend on the parts it is drawn by: the
 * pixel number k of n is k * dx / n and k * dy / n from the start, each
 * brought to the nearest whole number, a half away from 0. It is counted in
 * whole numbers, a step an addition: `over` is twice what the pixel has not
 * come up to yet, in n-ths of a pixel.
 */
static void dgx_stroke_thin(dgx_stroke_t *s, int bx, int by, float len, float f0, float f1)
{
    const int dx = bx - s->x, dy = by - s->y;
    const int wx = dx < 0 ? -dx : dx, wy = dy < 0 ? -dy : dy, n = wx > wy ? wx : wy;
    const int from = (int)(f0 * n / len), to = f1 >= len ? n : (int)(f1 * n / len);
    /* where the pixel number `from` is */
    const int gone_x = (int)((2 * (int64_t)from * wx + n) / (2 * (int64_t)n)), gone_y = (int)((2 * (int64_t)from * wy + n) / (2 * (int64_t)n));
    int       over_x = (int)(2 * ((int64_t)from * wx - (int64_t)gone_x * n)), over_y = (int)(2 * ((int64_t)from * wy - (int64_t)gone_y * n));
    int       x = s->x + (dx < 0 ? -gone_x : gone_x), y = s->y + (dy < 0 ? -gone_y : gone_y);
#if DGX_STROKE_MEMORY
    if (s->direct) {
        const int      width = s->scr->width, height = s->scr->height, bytes = s->direct, step_x = dx < 0 ? -1 : 1, step_y = dy < 0 ? -1 : 1;
        uint8_t *const memory = ((dgx_vscreen_t *)s->scr)->v_array;
        const uint8_t  hi = (uint8_t)(s->color >> 8), lo = (uint8_t)s->color;
        int            left = width, right = -1, top = height, bottom = -1;
        for (int k = from; k <= to; ++k) {
            if ((unsigned)x < (unsigned)width && (unsigned)y < (unsigned)height) {
                uint8_t *p = memory + ((size_t)y * (size_t)width + (size_t)x) * (size_t)bytes;
                if (bytes == 1) p[0] = lo;
                else p[0] = hi, p[1] = lo;
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
            over_x += 2 * wx, over_y += 2 * wy;
            if (over_x >= n) x += step_x, over_x -= 2 * n;
            if (over_y >= n) y += step_y, over_y -= 2 * n;
        }
        if (left <= right) dgx_screen_touch(s->scr, left, right, top, bottom);
        return;
    }
#endif
    for (int k = from; k <= to; ++k) {
        dgx_set_pixel(s->scr, x, y, s->color);
        over_x += 2 * wx, over_y += 2 * wy;
        if (over_x >= n) x += dx < 0 ? -1 : 1, over_x -= 2 * n;
        if (over_y >= n) y += dy < 0 ? -1 : 1, over_y -= 2 * n;
    }
}

/*
 * What a call draws of a segment: the disc at the start, the way the pen goes, and the disc where it
 * stops, of the pen at the end and two pixels smaller on the way. Shares of the way: `to` 1 is the end.
 */
typedef struct {
    float from, to; /* the pen goes from `from` to `to`, when it moves */
    bool  moves;
    bool  start;    /* the disc at the start, not drawn before */
    bool  tip;      /* a disc where the pen stops */
    bool  bare;     /* there is no disc at the start, from before or now: what lies behind the start is no part of the segment */
    bool  round;    /* the call asks for a disc at the end: the bar of the soft pen ends at the end, the disc lies past it */
} dgx_segment_step_t;

/*
 * A segment by the hard pen is a curve of one piece between its ends brought to whole pixels: the
 * rectangle of the piece, and the disc of the pen at an end. A disc where the pen stands and does not
 * move is drawn with the last pixel of the way before it, which is the line's anyway: so the disc is cut
 * by the straight line the segment starts with, as the disc of a pen on its way is.
 */
static void dgx_segment_hard(dgx_stroke_t *s, float x1, float y1, float x2, float y2, const dgx_segment_step_t *w)
{
    s->x = dgx_stroke_round(x1), s->y = dgx_stroke_round(y1);
    const int bx = dgx_stroke_round(x2), by = dgx_stroke_round(y2), dx = bx - s->x, dy = by - s->y;
    s->rh = 0;
    /*
     * An even width has no middle pixel: like dgx_draw_line_thick() gives the spare pixel to the right of the
     * way, the axis runs half a pixel to the right of it. A way along a row or a column leaves the other half
     * pixel free, and it goes right or down, as at the start of a curve; a dot has both so.
     */
    s->ex = s->ey = 0;
    if (!(s->width & 1)) {
        s->ey = dx < 0 ? -1 : 1;
        s->ex = dy > 0 ? -1 : 1;
    }
    const bool whole = w->to >= 1;
    if (dx == 0 && dy == 0) {
        /* the two ends are one point: a dot, by a pen of one pixel when the pen first goes, by a wider one with a disc at either end */
        if (s->width == 1 ? w->moves && w->from == 0 : w->start || w->tip) {
            if (s->width == 1) dgx_set_pixel(s->scr, s->x, s->y, s->color);
            else dgx_stroke_figure(s, s->x, s->y, 0, 0, 0, s->x, s->y, s->width);
        }
        return;
    }
    const float len = dgx_stroke_long(dx, dy), f0 = w->from * len, f1 = whole ? len : w->to * len;
    if (s->width == 1) {
        /* the pixels of the way, both ends among them: a disc of one pixel is the pixel at the end */
        if (w->start) dgx_set_pixel(s->scr, s->x, s->y, s->color);
        if (w->moves) dgx_stroke_thin(s, bx, by, len, f0, f1);
        else if (w->tip && whole) dgx_set_pixel(s->scr, bx, by, s->color);
        return;
    }
    if (w->start) dgx_stroke_figure(s, s->x, s->y, 0, 0, 0, s->x, s->y, s->width);
    if (!w->moves && !w->tip) return;
    const float from = w->moves ? f0 : f1 > 1 ? f1 - 1 : 0;
    if (whole) {
        dgx_stroke_figure(s, bx, by, len, from, len, bx, by, w->tip ? s->width : 0);
    } else if (w->tip) {
        /* where the pen has stopped the line ends with a disc two pixels smaller, it stays within the segment */
        const int cx = s->x + dgx_stroke_round(dx * f1 / len), cy = s->y + dgx_stroke_round(dy * f1 / len);
        dgx_stroke_figure(s, bx, by, len, from, f1, cx, cy, s->width - 2);
    } else {
        dgx_stroke_figure(s, bx, by, len, from, f1, 0, 0, 0);
    }
}

/* a part of a soft segment begins this far behind the place the pen stopped at: the soft end of the part before is covered whole */
#define DGX_SEGMENT_LAP 2.0f

/*
 * A segment by the soft pen: a bar, and at an end with a disc the half of the disc of the pen that lies
 * past it. The ends are taken as they are, not brought to whole pixels. An end without a disc is cut
 * sharply across the way, a sixty-fourth of a pixel past the end: the two counts of a pixel on the seam
 * with the next segment may differ by that much, and the pixel is not left out. A part of the way is
 * drawn as a curve draws the piece its pen is on: it begins a little behind where the part before has
 * stopped, where that one has put its whole shares, and a pixel keeps the larger of the two.
 */
static void dgx_segment_soft(dgx_stroke_t *s, float x1, float y1, float x2, float y2, const dgx_segment_step_t *w)
{
    const float half = dgx_stroke_half(s->width);
    const float ax = x1 + half, ay = y1 + half, bx = x2 + half, by = y2 + half;
    const float dx = bx - ax, dy = by - ay, square = dx * dx + dy * dy;
    const bool  whole = w->to >= 1;
    /* too short to have a direction: a dot, with a disc at either end */
    if (!(square > DGX_STROKE_SLOP * DGX_STROKE_SLOP)) {
        if (w->start || w->tip) dgx_stroke_disc(s, ax, ay, s->width, NULL, 0, NULL);
        return;
    }
    const float unit = dgx_stroke_rsqrt(square), len = square * unit;
    const float ahead[2] = {dx * unit, dy * unit}, back[2] = {-ahead[0], -ahead[1]};
    const float f0 = w->from * len, pen = whole ? len : w->to * len;
    if (w->start) dgx_stroke_disc(s, ax, ay, s->width, ahead, DGX_STROKE_SEAM, NULL);
    if (w->moves) {
        const float begin = f0 > DGX_SEGMENT_LAP ? f0 - DGX_SEGMENT_LAP : w->bare ? -DGX_STROKE_SEAM : 0;
        dgx_stroke_bar(s, ax, ay, ahead[0], ahead[1], len, begin, pen, w->round ? len : len + DGX_STROKE_SEAM);
    }
    if (!w->tip) return;
    if (whole) {
        /* the longer the piece, the further apart the two counts of its end */
        dgx_stroke_disc(s, bx, by, s->width, back, DGX_STROKE_SEAM + len * (2.0f / DGX_STROKE_ONE), NULL);
    } else {
        /*
         * Where the pen has stopped the line ends with a disc two pixels smaller, it stays within the
         * segment. Without a disc at the start it is cut where the segment starts: what lies behind that
         * is no part of the segment.
         */
        dgx_stroke_disc(s, ax + ahead[0] * pen, ay + ahead[1] * pen, s->width - 2, w->bare ? back : NULL, pen - DGX_STROKE_SEAM, NULL);
    }
}

/* the ends of a segment are taken where the points of a curve may lie */
#define DGX_SEGMENT_FAR 32767.0f

bool dgx_draw_thick_segment_part(dgx_screen_t *scr, float x1, float y1, float x2, float y2, int width, uint32_t color, dgx_segment_part_t *part)
{
    /* the discs a call asks for, whatever the calls before asked for: each is drawn once it is asked for */
    const int  asked = width > 0 ? width & (DGX_PEN_START_DISC | DGX_PEN_END_DISC) : 0;
    const bool round = asked & DGX_PEN_END_DISC;
    const int  drawn_discs = part->discs;
    /* a share that is not a number fails every comparison: the pen does not move, and a segment is not drawn back */
    const bool moves = part->t > part->drawn && part->drawn < 1;
    dgx_segment_step_t w = {
        .from = part->drawn,
        .to = moves ? (part->t < 1 ? part->t : 1) : part->drawn,
        .moves = moves,
        .start = (asked & DGX_PEN_START_DISC) && !(drawn_discs & DGX_PEN_START_DISC),
        .round = round,
    };
    /* a disc where the pen stands: on its way, or once where it stands still; none before the pen has gone at all */
    w.tip = round && w.to > 0 && (moves || !(drawn_discs & DGX_PEN_END_DISC));
    w.bare = !(drawn_discs & DGX_PEN_START_DISC) && !w.start;
    if (!w.moves && !w.start && !w.tip) return part->drawn >= 1;
    dgx_stroke_t s;
    /* a segment that cannot be drawn is done with: its ends too far or not numbers, a width that is no pen */
    const bool near = x1 >= -DGX_SEGMENT_FAR && x1 <= DGX_SEGMENT_FAR && y1 >= -DGX_SEGMENT_FAR && y1 <= DGX_SEGMENT_FAR && x2 >= -DGX_SEGMENT_FAR &&
                      x2 <= DGX_SEGMENT_FAR && y2 >= -DGX_SEGMENT_FAR && y2 <= DGX_SEGMENT_FAR;
    if (!near || !dgx_stroke_set(&s, scr, width, color)) {
        part->drawn = 1;
        part->discs = DGX_PEN_START_DISC | DGX_PEN_END_DISC;
        return true;
    }
    dgx_screen_progress_up(scr);
    if (DGX_STROKE_SOFT && s.soft) dgx_segment_soft(&s, x1, y1, x2, y2, &w);
    else dgx_segment_hard(&s, x1, y1, x2, y2, &w);
    dgx_screen_progress_down(scr);
    /* the disc where the pen stands is there when it was drawn now, or when it was before and the pen has not moved */
    int discs = drawn_discs | (w.start ? DGX_PEN_START_DISC : 0);
    if (w.tip) discs |= DGX_PEN_END_DISC;
    else if (moves) discs &= ~DGX_PEN_END_DISC;
    part->discs = discs;
    part->drawn = w.to;
    return w.to >= 1;
}

void dgx_draw_thick_segment(dgx_screen_t *scr, float x1, float y1, float x2, float y2, int width, uint32_t color)
{
    dgx_segment_part_t part = {.t = 1};
    dgx_draw_thick_segment_part(scr, x1, y1, x2, y2, width, color, &part);
}
