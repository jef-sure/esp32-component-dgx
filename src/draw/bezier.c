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

/* the thick polyline of a curve, drawn one piece at a time */
typedef dgx_bezier_t dgx_stroke_t;

#define DGX_STROKE_FAR  1e6f
#define DGX_STROKE_SLOP 0.01f
/* a row of fewer pixels is filled in place: to call memset() for it costs more than to fill it */
#define DGX_STROKE_SHORT 8

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

/* a part of a piece begins this far behind the place the pen stopped at: the soft end of the part before is covered whole */
#define DGX_STROKE_LAP    2.0f
/* a joint that is not made round may give a pixel this much more of its ink than a round one would */
#define DGX_STROKE_CORNER (1.0f / 16)

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

/* along a piece and across it pixels are counted in whole numbers, so much to a pixel */
#define DGX_STROKE_Q     13
#define DGX_STROKE_ONE   (1 << DGX_STROKE_Q)
/* the rows of a bar are looked at this much wider than they are found to be */
#define DGX_STROKE_GUARD (1.0f / 16)
/* to find where a row of a bar begins and ends pays when it saves looking at so many pixels beside it */
#define DGX_STROKE_BESIDE 5.0f

static inline int dgx_stroke_fixed(float v)
{
    return (int)(v * DGX_STROKE_ONE + (v < 0 ? -0.5f : 0.5f));
}

/* a pixel this near to the end of a piece is given both to the piece and to the disc at that end: the two are not counted the same way */
#define DGX_STROKE_SEAM  (1.0f / 64)

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
 * shares, and so the parts together give what the piece gives at once. Where
 * the rows of a long bar begin and end is found roughly, row after row; it
 * only says which pixels are looked at.
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
    const int   ink_all = erase ? 255 : (int)(s->color & 0xff), half = DGX_STROKE_ONE / 2;
    const uint32_t span = (uint32_t)((int)(reach * DGX_STROKE_ONE) - from);
    /*
     * A row of the bar is so many pixels long: as long as the bar is thick along a row, or as long as
     * it is itself. When the columns the whole bar lies between are few more than that, every row is
     * looked at from the first of them to the last: to find where a row begins and ends would cost
     * more than to look at the pixels beside it. A bar along a row or a column is always so.
     */
    const float box = most_hi - most_lo;
    const bool  rows = (box - DGX_STROKE_BESIDE) * wy > 2 * edge && (box - DGX_STROKE_BESIDE) * wx > reach - start;
    /* between which columns a row of the bar lies: by its ends and by its sides, and how they move with a row */
    float       end_lo = 0, end_hi = 0, end_step = 0, side_lo = 0, side_hi = 0, side_step = 0;
    if (rows) {
        /* 1 / ux and 1 / uy by one division */
        const float both = dgx_stroke_one_over(ux * uy), ix = uy * both, iy = ux * both, down = top - ay;
        const float e0 = ax + (start - down * uy) * ix, e1 = ax + (reach - down * uy) * ix;
        const float f0 = ax + (down * ux - edge) * iy, f1 = ax + (down * ux + edge) * iy;
        end_lo = e0 < e1 ? e0 : e1, end_hi = e0 < e1 ? e1 : e0;
        end_step = -uy * ix;
        side_lo = f0 < f1 ? f0 : f1, side_hi = f0 < f1 ? f1 : f0;
        side_step = ux * iy;
    }
    int      touched_left = most_right, touched_right = most_left;
    uint8_t *line = ((dgx_vscreen_t *)s->scr)->v_array + (size_t)top * (size_t)width;
    /* the count for the first of the columns in the row `top`; a row down, the counts move by uyi and by -uxi */
    int      along_row = (most_left - ox) * uxi + (top - oy) * uyi - along0, across_row = (most_left - ox) * uyi - (top - oy) * uxi - across0;
    for (int row = top; row <= bottom; ++row, line += width, along_row += uyi, across_row -= uxi) {
        int left = most_left, right = most_right;
        if (rows) {
            /* the first pixel not left of lo and the last one not right of hi, a little more of both */
            const float lo = (end_lo > side_lo ? end_lo : side_lo) + (1 - DGX_STROKE_GUARD);
            const float hi = (end_hi < side_hi ? end_hi : side_hi) + DGX_STROKE_GUARD;
            end_lo += end_step, end_hi += end_step, side_lo += side_step, side_hi += side_step;
            if (hi < most_left) continue;
            if (lo > most_left + 1) left = (int)lo;
            if (hi < most_right) right = (int)hi;
            if (left > right) continue;
            if (left < touched_left) touched_left = left;
            if (right > touched_right) touched_right = right;
        }
        int along = along_row + (left - most_left) * uxi, across = across_row + (left - most_left) * uyi;
        for (uint8_t *p = line + left, *stop = line + right; p <= stop; ++p, along += uxi, across += uyi) {
            if ((uint32_t)(along - from) > span) continue;
            int share = soft - (across < 0 ? -across : across);
            if (stopped && half + (to - along) < share) share = half + (to - along);
            if (share <= 0) continue;
            dgx_stroke_put(p, share >= DGX_STROKE_ONE ? ink_all : (share * ink_all + half) >> DGX_STROKE_Q, erase);
        }
    }
    if (!rows) touched_left = most_left, touched_right = most_right;
    if (touched_left <= touched_right) dgx_screen_touch(s->scr, touched_left, touched_right, top, bottom);
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

/* how far the polyline may be from the curve when the pieces are counted here, pixels */
#define DGX_BEZIER_TOLERANCE 0.5f

/*
 * Without a number of pieces it is found from the points. A chord over 1/n of
 * the parameter is not further from the curve than a eighth of its second
 * derivative over n squared; bend is the largest value of that derivative.
 */
static int dgx_bezier_pieces(float bend)
{
    int pieces = (int)ceilf(sqrtf(bend / (8 * DGX_BEZIER_TOLERANCE)));
    return pieces < 1 ? 1 : pieces;
}

static void dgx_bezier_begin(dgx_bezier_t *b, dgx_screen_t *scr, dgx_point_2d_t end, int pieces, int width, uint32_t color)
{
    /* the kind of the pen comes with its width; a width below 1 is no pen at all, whatever bits it has */
    const bool asked = width > 0 && (width & DGX_PEN_SOFT);
    if (width > 0) width &= ~DGX_PEN_SOFT;
    b->scr = scr;
    b->color = color;
    b->width = width > INT16_MAX ? INT16_MAX : width;
    b->started = false;
    b->soft = false;
    b->direct = 0;
#if DGX_STROKE_MEMORY
    if ((scr->color_bits == 8 || scr->color_bits == 16) && dgx_vscreen_is_linear(scr)) b->direct = scr->color_bits / 8;
#endif
    /*
     * A soft edge is a share of the ink in a pixel, and where lines meet the larger share stays. Only
     * a pixel that is an amount of ink can do it, and that is a byte of a virtual screen. A pixel of
     * colors has nothing to take the larger of, and a display behind a bus cannot be read: there the
     * pen is hard whatever is asked for.
     */
    b->soft = DGX_STROKE_SOFT && asked && width >= 1 && scr->color_bits == 8 && dgx_vscreen_is_linear(scr);
    if (b->soft) {
        const float edge = b->width * 0.5f + 0.5f;
        b->fx = b->kx[3] + dgx_stroke_half(b->width), b->fy = b->ky[3] + dgx_stroke_half(b->width);
        /*
         * In a corner a pixel may have edge * tan(half the turn) squared more of it covered than in a round
         * joint: the least cosine of a turn that keeps it within DGX_STROKE_CORNER.
         */
        b->little = (edge - DGX_STROKE_CORNER) * dgx_stroke_one_over(edge + DGX_STROKE_CORNER);
        b->back = 0;
        b->next_length = 0;
    }
    if (!b->soft) {
        b->x = dgx_stroke_round(b->kx[3]);
        b->y = dgx_stroke_round(b->ky[3]);
        b->ex = b->ey = 0;
        b->rh = 0;
    }
    b->end = end;
    b->pieces = width < 1 ? 0 : pieces > INT16_MAX ? INT16_MAX : pieces;
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
    if (pieces < 1) pieces = dgx_bezier_pieces(2 * hypotf(b->kx[1], b->ky[1]));
    dgx_bezier_begin(b, scr, points[2], pieces, width, color);
}

/*
 * dgx_draw_arc() of ellipse.c begins its curves here by points made whole and gives them kx[0..2] and
 * ky[0..2] of its own after: nothing else found from the two middle points may be kept in the curve.
 */
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
    if (pieces < 1) {
        float at_start = 2 * hypotf(b->kx[1], b->ky[1]);
        float at_end = 2 * hypotf(3 * b->kx[0] + b->kx[1], 3 * b->ky[0] + b->ky[1]);
        pieces = dgx_bezier_pieces(at_start > at_end ? at_start : at_end);
    }
    dgx_bezier_begin(b, scr, points[3], pieces, width, color);
}

/* the end of a piece, pieces counted from 1: a point is ((k[0] * t + k[1]) * t + k[2]) * t + k[3] */
static void dgx_bezier_point(const dgx_bezier_t *b, int piece, int *x, int *y)
{
    if (piece >= b->pieces) {
        *x = b->end.x, *y = b->end.y;
        return;
    }
    float t = (float)piece / b->pieces;
    *x = dgx_stroke_round(((b->kx[0] * t + b->kx[1]) * t + b->kx[2]) * t + b->kx[3]);
    *y = dgx_stroke_round(((b->ky[0] * t + b->ky[1]) * t + b->ky[2]) * t + b->ky[3]);
}

/* the same for a soft line, pieces counted from 0, the start of the curve: the point as it is */
static void dgx_bezier_soft_point(const dgx_bezier_t *b, int piece, float *x, float *y)
{
    const float half = dgx_stroke_half(b->width);
    if (piece >= b->pieces) {
        *x = b->end.x + half, *y = b->end.y + half;
        return;
    }
    /* no division: on an ESP32 it is a function of the library */
    float t = (float)piece * dgx_stroke_one_over((float)b->pieces);
    *x = ((b->kx[0] * t + b->kx[1]) * t + b->kx[2]) * t + b->kx[3] + half;
    *y = ((b->ky[0] * t + b->ky[1]) * t + b->ky[2]) * t + b->ky[3] + half;
}

float dgx_bezier_length(dgx_bezier_t *b)
{
    if (DGX_STROKE_SOFT && b->length < 0 && b->soft) {
        float x0, y0;
        dgx_bezier_soft_point(b, 0, &x0, &y0);
        b->length = 0;
        for (int i = 1; i <= b->pieces; ++i) {
            float x, y;
            dgx_bezier_soft_point(b, i, &x, &y);
            /* as the pieces are measured when they are drawn */
            const float square = (x - x0) * (x - x0) + (y - y0) * (y - y0);
            if (square > DGX_STROKE_SLOP * DGX_STROKE_SLOP) b->length += square * dgx_stroke_rsqrt(square);
            x0 = x, y0 = y;
        }
    }
    if (b->length < 0) {
        int x0 = dgx_stroke_round(b->kx[3]), y0 = dgx_stroke_round(b->ky[3]);
        b->length = 0;
        for (int i = 1; i <= b->pieces; ++i) {
            int x, y;
            dgx_bezier_point(b, i, &x, &y);
            b->length += dgx_stroke_long(x - x0, y - y0);
            x0 = x, y0 = y;
        }
    }
    return b->length;
}

/* the round start of the line, where the pen is */
static void dgx_bezier_start(dgx_bezier_t *b)
{
    b->started = true;
    if (DGX_STROKE_SOFT && b->soft) {
        /* of a line, what lies behind the start of its first piece; of a dot, all of it */
        const float ahead[2] = {b->ux, b->uy};
        dgx_stroke_disc(b, b->fx, b->fy, b->width, b->in_piece ? ahead : NULL, DGX_STROKE_SEAM, NULL);
        return;
    }
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
 * What dgx_bezier_draw_to() does for a soft line.
 *
 * A piece is one bar. Where the line turns a little at a joint, the two
 * pieces are made longer by tan(half the turn) of half their width each, the
 * soft edge counted in, and so meet in a corner. No pixel gets more than
 * DGX_STROKE_CORNER of its ink over what a round joint would give it: the
 * size of a turn that is still little is found from that. A sharper joint is
 * round: the piece before it ends with the part of a disc that lies between
 * the two pieces, and so does the last piece of a curve, with the half of a
 * disc past its end. So to draw a piece the one after it is looked at first;
 * it is not found a second time.
 */
static void dgx_bezier_soft_to(dgx_bezier_t *b, bool whole, float target)
{
    /* the soft edge of the line is this far from its middle, and the corner is where the edges of the two pieces meet */
    const float edge = b->width * 0.5f + 0.5f;
    while (b->done < b->pieces) {
        if (!b->in_piece) {
            float x, y;
            if (b->next_length > 0) {
                x = b->nx, y = b->ny;
                b->ux = b->next[0], b->uy = b->next[1];
                b->piece_length = b->next_length;
            } else {
                dgx_bezier_soft_point(b, b->done + 1, &x, &y);
                const float dx = x - b->fx, dy = y - b->fy, square = dx * dx + dy * dy;
                /* too short to have a direction: the next one starts where this one does */
                if (!(square > DGX_STROKE_SLOP * DGX_STROKE_SLOP)) {
                    ++b->done;
                    continue;
                }
                const float by = dgx_stroke_rsqrt(square);
                b->piece_length = square * by;
                b->ux = dx * by, b->uy = dy * by;
            }
            b->fbx = x, b->fby = y;
            b->fore = -1;
            b->next_length = 0;
            if (b->done + 2 <= b->pieces) {
                dgx_bezier_soft_point(b, b->done + 2, &b->nx, &b->ny);
                const float dx = b->nx - x, dy = b->ny - y, square = dx * dx + dy * dy;
                if (square > DGX_STROKE_SLOP * DGX_STROKE_SLOP) {
                    const float by = dgx_stroke_rsqrt(square);
                    b->next_length = square * by;
                    b->next[0] = dx * by, b->next[1] = dy * by;
                    /* the cosine and the sine of the turn; tan(half of it) = sine / (1 + cosine) */
                    const float turn = b->ux * b->next[0] + b->uy * b->next[1], side = b->ux * b->next[1] - b->uy * b->next[0];
                    if (turn >= b->little) {
                        /* 1 / (1 + turn) = 1 / (2 - less), by the first terms of its series: the turn is little */
                        const float less = 1 - turn;
                        b->fore = edge * (side < 0 ? -side : side) * (0.5f + less * (0.25f + less * (0.125f + less * 0.0625f)));
                        /* nearly straight, no disc is drawn: the bars still go over the seam, or a pixel on it is left out */
                        if (b->fore < DGX_STROKE_SEAM) b->fore = DGX_STROKE_SEAM;
                    }
                }
            }
            b->piece_drawn = 0;
            b->in_piece = true;
        }
        if (!b->started) dgx_bezier_start(b);
        const float from = b->piece_drawn, rest = b->piece_length - from;
        const float end = b->piece_length + (b->fore > 0 ? b->fore : 0);
        /* a part begins behind the place the pen stopped at: the soft end of the part before is covered whole */
        const float start = from > DGX_STROKE_LAP ? from - DGX_STROKE_LAP : -b->back;
        if (whole || target - b->drawn >= rest) {
            dgx_stroke_bar(b, b->fx, b->fy, b->ux, b->uy, b->piece_length, start, b->piece_length, end);
            if (!(b->fore > 0)) {
                /*
                 * A round end: what of the disc lies past the end of this piece and, at a joint, before the
                 * start of the next one. The longer the piece, the further apart the two counts of its end.
                 */
                const float back[2] = {-b->ux, -b->uy};
                dgx_stroke_disc(b, b->fbx, b->fby, b->width, back, DGX_STROKE_SEAM + b->piece_length * (2.0f / DGX_STROKE_ONE),
                                b->next_length > 0 ? b->next : NULL);
            }
            b->drawn += rest;
            b->fx = b->fbx, b->fy = b->fby;
            b->back = b->fore > 0 ? b->fore : 0;
            b->in_piece = false;
            ++b->done;
            continue;
        }
        const float to = from + target - b->drawn;
        /* where the pen has stopped the line ends with a disc two pixels smaller, it stays within the line */
        dgx_stroke_bar(b, b->fx, b->fy, b->ux, b->uy, b->piece_length, start, to, end);
        dgx_stroke_disc(b, b->fx + b->ux * to, b->fy + b->uy * to, b->width - 2, NULL, 0, NULL);
        b->piece_drawn = to;
        b->drawn = target;
        break;
    }
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
    if (DGX_STROKE_SOFT && b->soft) dgx_bezier_soft_to(b, whole, target);
    while (!b->soft && b->done < b->pieces) {
        if (!b->in_piece) {
            int x, y;
            dgx_bezier_point(b, b->done + 1, &x, &y);
            int dx = x - b->x, dy = y - b->y;
            if (dx == 0 && dy == 0) {
                ++b->done;
                continue;
            }
            b->bx = x, b->by = y;
            b->piece_length = dgx_stroke_long(dx, dy);
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
            int cx = b->x + dgx_stroke_round((b->bx - b->x) * to / b->piece_length);
            int cy = b->y + dgx_stroke_round((b->by - b->y) * to / b->piece_length);
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
