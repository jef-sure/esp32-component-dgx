/*
 * ellipse.c
 *
 *  The outline of an ellipse, its arc and its sector.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "dgx_bits.h"
#include "dgx_draw.h"

/* the squares of larger radii do not fit the 64 bits they are compared in */
#define DGX_ELLIPSE_RADIUS INT16_MAX
/* an angle past this many degrees has no fraction of a degree left in a float */
#define DGX_ELLIPSE_DEGREES 1e6f

static const float DGX_QUARTER = 1.57079632679489661923f; /* of a turn, radians */
#ifndef DGX_ARC_WITHIN_SOFT
#define DGX_ARC_WITHIN_SOFT 0.125f
#endif

/* a row of pixels from xa to xb, xa <= xb; one pixel is not worth a rectangle */
static inline void dgx_ellipse_run(dgx_screen_t *scr, int xa, int xb, int y, uint32_t color)
{
    if (xa == xb) scr->set_pixel(scr, xa, y, color);
    else scr->fill_rectangle(scr, xa, y, xb - xa + 1, 1, color);
}

/* a column of pixels from ya to yb, ya <= yb */
static inline void dgx_ellipse_column(dgx_screen_t *scr, int x, int ya, int yb, uint32_t color)
{
    if (ya == yb) scr->set_pixel(scr, x, ya, color);
    else scr->fill_rectangle(scr, x, ya, 1, yb - ya + 1, color);
}

/*
 * The rows of an ellipse, from the top one to the middle one.
 *
 * A pixel is of the ellipse when the curve reaches the middle of that side
 * of it which faces the centre, the side across the row or the side across
 * the column. That is the choice of the midpoint algorithm, which rounds the
 * curve to a pixel along the columns where it runs flat and along the rows
 * where it runs steep; here both are asked of every pixel, and a pixel is
 * taken when either holds. So an ellipse of equal radii is the circle of
 * dgx_draw_circle(), and no pixel of the outline is further than half a
 * pixel from the curve, along its row or along its column.
 *
 * The walk of the textbooks goes a column to the side at every step of its
 * first part and a row down at every step of the second, whatever the curve
 * does within the step. It spoils the ends of a thin ellipse: a flat one
 * stops short of its end, a tall one is a column too wide at the top. And
 * where its two parts meet it may put a pixel further than half a pixel from
 * the curve; here the nearer one is taken, or one more pixel, a corner as
 * the circle of radius 4 has on its diagonals. Of the ellipses with radii
 * from 4 to 70 and no thinner than 8 to 1, one in 46 differs from the walk
 * so, by a pixel in each quarter; test/host/test_ellipse.c counts them.
 *
 * A row reaches so far to each side; further from the middle a row reaches
 * no further. The outline in a row is what the row above does not cover, and
 * the last pixel of the row at least.
 */
typedef struct {
    int64_t a, b, c; /* ry * ry, rx * rx, 4 * rx * rx * ry * ry */
    int     rx;
    int     row;     /* the row to come, counted from the middle; below 0: no more */
    int     reach;   /* of the row before it */
    bool    top;     /* no row was given yet */
} dgx_ellipse_t;

static void dgx_ellipse_begin(dgx_ellipse_t *e, int rx, int ry)
{
    e->a = (int64_t)ry * ry;
    e->b = (int64_t)rx * rx;
    e->c = 4 * e->a * e->b;
    e->rx = rx;
    e->row = ry;
    e->reach = 0;
    e->top = true;
}

/* the next row: it is `row` rows from the middle, reaches `reach` pixels to each side, its outline begins at `from` */
static bool dgx_ellipse_next(dgx_ellipse_t *e, int *row, int *from, int *reach)
{
    if (e->row < 0) return false;
    int x = e->reach;
    if (!e->c) {
        /* no width or no height: a line, all of it in the middle row or in the middle column */
        if (e->row == 0) x = e->rx;
    } else {
        const int64_t side = 2 * (int64_t)e->row - 1;
        const int64_t in_row = 4 * e->b * e->row * e->row, in_column = e->b * side * side;
        while (x < e->rx) {
            const int64_t next = x + 1, edge = 2 * next - 1;
            if (e->a * edge * edge + in_row >= e->c && 4 * e->a * next * next + in_column >= e->c) break;
            x = (int)next;
        }
    }
    *row = e->row;
    *reach = x;
    *from = e->top ? 0 : x > e->reach ? e->reach + 1 : x;
    e->reach = x;
    e->top = false;
    --e->row;
    return true;
}

/*
 * What lies between two rays from the centre. An angle is counted in degrees
 * from the ray that goes right, against the clock as the screen is seen. A
 * ray is kept as a whole vector of length DGX_WEDGE_ONE, so that a pixel on
 * it is told exactly: one that lies on a ray is of the wedge. The rays that
 * go along the axes and the diagonals are exact, and so are two rays a half
 * turn apart.
 */
#define DGX_WEDGE_ONE 16384
#define DGX_WEDGE_FAR (INT32_MAX / 2)

typedef struct {
    int32_t c1, s1, c2, s2;
    bool    all;  /* a whole turn */
    bool    wide; /* over a half turn: what lies on the side of either ray, not of both */
} dgx_wedge_t;

static void dgx_wedge_ray(float degrees, int32_t *c, int32_t *s)
{
    float rest = degrees - floorf(degrees / 360.0f) * 360.0f;
    if (!(rest >= 0 && rest < 360.0f)) rest = 0;
    int quarter = (int)(rest / 90.0f);
    if (quarter > 3) quarter = 3;
    rest -= quarter * 90.0f;
    /* an eighth of a turn is found, the other one is its mirror */
    const bool  over = rest > 45.0f;
    const float angle = (over ? 90.0f - rest : rest) * (DGX_QUARTER / 90.0f);
    int32_t     a = (int32_t)lroundf(cosf(angle) * DGX_WEDGE_ONE), b = (int32_t)lroundf(sinf(angle) * DGX_WEDGE_ONE);
    if (over) DGX_INT_SWAP(a, b);
    switch (quarter) {
    case 0: *c = a, *s = b; break;
    case 1: *c = -b, *s = a; break;
    case 2: *c = -a, *s = -b; break;
    default: *c = b, *s = -a; break;
    }
}

/* false: there is nothing between the rays */
static bool dgx_wedge_set(dgx_wedge_t *w, float from, float to)
{
    if (!(fabsf(from) < DGX_ELLIPSE_DEGREES) || !(fabsf(to) < DGX_ELLIPSE_DEGREES)) return false;
    if (to < from) {
        const float t = from;
        from = to, to = t;
    }
    const float sweep = to - from;
    if (!(sweep > 0)) return false;
    w->all = sweep >= 360.0f;
    w->wide = sweep > 180.0f;
    dgx_wedge_ray(from, &w->c1, &w->s1);
    dgx_wedge_ray(to, &w->c2, &w->s2);
    /* two rays that are one: an angle too small to tell them apart, and the two sides of one ray are its whole line */
    if (!w->wide && w->c1 == w->c2 && w->s1 == w->s2) return false;
    return true;
}

static inline int32_t dgx_wedge_floor(int32_t n, int32_t d)
{
    const int32_t q = n / d;
    return n % d < 0 ? q - 1 : q;
}

/* the columns x of a row with a * x + b >= 0 */
static inline void dgx_wedge_side(int32_t a, int32_t b, int *lo, int *hi)
{
    *lo = -DGX_WEDGE_FAR, *hi = DGX_WEDGE_FAR;
    if (a > 0) *lo = -dgx_wedge_floor(b, a);
    else if (a < 0) *hi = dgx_wedge_floor(b, -a);
    else if (b < 0) *lo = DGX_WEDGE_FAR, *hi = -DGX_WEDGE_FAR;
}

/*
 * What of the columns left..right of the row dy lies in the wedge, columns
 * and rows counted from the centre: the number of runs, none to two, and
 * their ends in out[].
 */
static int dgx_wedge_row(const dgx_wedge_t *w, int dy, int left, int right, int out[4])
{
    if (left > right) return 0;
    if (w->all) {
        out[0] = left, out[1] = right;
        return 1;
    }
    int lo1, hi1, lo2, hi2;
    /* against the clock of the first ray, and with the clock of the second one */
    dgx_wedge_side(-w->s1, -w->c1 * dy, &lo1, &hi1);
    dgx_wedge_side(w->s2, w->c2 * dy, &lo2, &hi2);
    if (!w->wide) {
        const int lo = DGX_MAX(lo1, lo2), hi = DGX_MIN(hi1, hi2);
        out[0] = DGX_MAX(left, lo), out[1] = DGX_MIN(right, hi);
        return out[0] <= out[1];
    }
    lo1 = DGX_MAX(left, lo1), hi1 = DGX_MIN(right, hi1);
    lo2 = DGX_MAX(left, lo2), hi2 = DGX_MIN(right, hi2);
    if (lo1 > hi1) {
        out[0] = lo2, out[1] = hi2;
        return lo2 <= hi2;
    }
    if (lo2 > hi2) {
        out[0] = lo1, out[1] = hi1;
        return 1;
    }
    if (lo1 <= hi2 + 1 && lo2 <= hi1 + 1) {
        out[0] = DGX_MIN(lo1, lo2), out[1] = DGX_MAX(hi1, hi2);
        return 1;
    }
    if (lo1 < lo2) {
        out[0] = lo1, out[1] = hi1, out[2] = lo2, out[3] = hi2;
    } else {
        out[0] = lo2, out[1] = hi2, out[2] = lo1, out[3] = hi1;
    }
    return 2;
}

static bool dgx_ellipse_fits(const dgx_screen_t *scr, int x, int y, int rx, int ry, int around)
{
    if (rx < 0 || ry < 0 || rx > DGX_ELLIPSE_RADIUS || ry > DGX_ELLIPSE_RADIUS) return false;
    if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX) return false;
    return y + ry + around >= 0 && y - ry - around < scr->height && x + rx + around >= 0 && x - rx - around < scr->width;
}

/* the single pixels of the rows top..bottom from the middle, `column` pixels to each side of it; top >= bottom */
static void dgx_ellipse_columns(dgx_screen_t *scr, int x, int y, int column, int top, int bottom, uint32_t color)
{
    if (column < 0) return;
    if (bottom == 0) {
        /* down to the middle row: what is above it and what is below it are one column */
        dgx_ellipse_column(scr, x + column, y - top, y + top, color);
        if (column) dgx_ellipse_column(scr, x - column, y - top, y + top, color);
        return;
    }
    dgx_ellipse_column(scr, x + column, y - top, y - bottom, color);
    dgx_ellipse_column(scr, x + column, y + bottom, y + top, color);
    if (column) {
        dgx_ellipse_column(scr, x - column, y - top, y - bottom, color);
        dgx_ellipse_column(scr, x - column, y + bottom, y + top, color);
    }
}

/*
 * The outline goes by rows where the curve is flat and by columns where it
 * is steep: the pixels a row has of it are one run, and single pixels of
 * rows one under another are one column. No pixel is sent twice.
 */
void dgx_draw_ellipse(dgx_screen_t *scr, int x, int y, int rx, int ry, uint32_t color)
{
    if (!dgx_ellipse_fits(scr, x, y, rx, ry, 0)) return;
    dgx_ellipse_t e;
    int           row, from, reach;
    int           column = -1, top = 0, bottom = 0; /* single pixels not sent yet */
    dgx_ellipse_begin(&e, rx, ry);
    dgx_screen_progress_up(scr);
    while (dgx_ellipse_next(&e, &row, &from, &reach)) {
        if (from == reach && column == reach) {
            bottom = row;
            continue;
        }
        dgx_ellipse_columns(scr, x, y, column, top, bottom, color);
        column = -1;
        if (from == reach) {
            column = reach, top = bottom = row;
            continue;
        }
        if (from == 0) {
            dgx_ellipse_run(scr, x - reach, x + reach, y - row, color);
            if (row) dgx_ellipse_run(scr, x - reach, x + reach, y + row, color);
            continue;
        }
        dgx_ellipse_run(scr, x + from, x + reach, y - row, color);
        dgx_ellipse_run(scr, x - reach, x - from, y - row, color);
        if (row) {
            dgx_ellipse_run(scr, x - reach, x - from, y + row, color);
            dgx_ellipse_run(scr, x + from, x + reach, y + row, color);
        }
    }
    dgx_ellipse_columns(scr, x, y, column, top, bottom, color);
    dgx_screen_progress_down(scr);
}

/* single pixels of an arc that lie one under another, not sent yet: a column from top to bottom; top > bottom: none */
typedef struct {
    int x, top, bottom;
} dgx_ellipse_stack_t;

static void dgx_ellipse_stack_send(dgx_ellipse_stack_t *stack, dgx_screen_t *scr, uint32_t color)
{
    if (stack->top <= stack->bottom) dgx_ellipse_column(scr, stack->x, stack->top, stack->bottom, color);
    stack->top = 1, stack->bottom = 0;
}

/*
 * What of the columns left..right of a row of the outline lies in the wedge.
 * Where the curve is steep a row has one pixel of it to a side, and the
 * pixels of rows one under another are a column: they are kept in `stack`
 * until the column ends.
 */
static void dgx_ellipse_arc_run(dgx_screen_t *scr, const dgx_wedge_t *w, int x, int y, int dy, int left, int right, uint32_t color,
                                dgx_ellipse_stack_t *stack)
{
    int       out[4];
    const int runs = dgx_wedge_row(w, dy, left, right, out);
    if (runs == 1 && out[0] == out[1]) {
        const int px = x + out[0], py = y + dy;
        if (stack->top <= stack->bottom && stack->x == px && (py == stack->bottom + 1 || py == stack->top - 1)) {
            if (py < stack->top) stack->top = py;
            else stack->bottom = py;
            return;
        }
        dgx_ellipse_stack_send(stack, scr, color);
        stack->x = px, stack->top = stack->bottom = py;
        return;
    }
    dgx_ellipse_stack_send(stack, scr, color);
    for (int i = 0; i < runs; ++i) dgx_ellipse_run(scr, x + out[2 * i], x + out[2 * i + 1], y + dy, color);
}

/* an arc a pixel wide: the pixels of the outline that lie between the rays */
static void dgx_ellipse_arc(dgx_screen_t *scr, int x, int y, int rx, int ry, const dgx_wedge_t *w, uint32_t color)
{
    dgx_ellipse_t       e;
    int                 row, from, reach;
    dgx_ellipse_stack_t stacks[2][2] = {{{0, 1, 0}, {0, 1, 0}}, {{0, 1, 0}, {0, 1, 0}}}; /* above the middle and below it, to the left and to the right */
    dgx_ellipse_begin(&e, rx, ry);
    dgx_screen_progress_up(scr);
    while (dgx_ellipse_next(&e, &row, &from, &reach)) {
        for (int half = 0; half < (row ? 2 : 1); ++half) {
            const int dy = half ? row : -row;
            if (y + dy < 0 || y + dy >= scr->height) continue;
            if (from == 0) {
                dgx_ellipse_arc_run(scr, w, x, y, dy, -reach, reach, color, &stacks[half][0]);
            } else {
                dgx_ellipse_arc_run(scr, w, x, y, dy, -reach, -from, color, &stacks[half][0]);
                dgx_ellipse_arc_run(scr, w, x, y, dy, from, reach, color, &stacks[half][1]);
            }
        }
    }
    for (int i = 0; i < 4; ++i) dgx_ellipse_stack_send(&stacks[i / 2][i % 2], scr, color);
    dgx_screen_progress_down(scr);
}

/* rows that are equal and lie one under another, not sent yet; h == 0: none */
typedef struct {
    int x, y, w, h;
} dgx_ellipse_band_t;

static void dgx_ellipse_band_send(dgx_ellipse_band_t *band, dgx_screen_t *scr, uint32_t color)
{
    if (!band->h) return;
    if (band->w == 1 && band->h == 1) scr->set_pixel(scr, band->x, band->y, color);
    else scr->fill_rectangle(scr, band->x, band->y, band->w, band->h, color);
    band->h = 0;
}

static void dgx_ellipse_band_row(dgx_ellipse_band_t *band, dgx_screen_t *scr, int left, int right, int y, uint32_t color)
{
    const int w = right - left + 1;
    if (band->h && band->x == left && band->w == w) {
        if (y == band->y + band->h) {
            ++band->h;
            return;
        }
        if (y == band->y - 1) {
            --band->y, ++band->h;
            return;
        }
    }
    dgx_ellipse_band_send(band, scr, color);
    band->x = left, band->y = y, band->w = w, band->h = 1;
}

/*
 * A sector goes by rows, and rows that are equal go as one rectangle. A row
 * has one run of it, or two when the sector is over a half turn and the row
 * crosses the part left out.
 */
void dgx_solid_sector(dgx_screen_t *scr, int x, int y, int rx, int ry, float from, float to, uint32_t color)
{
    dgx_wedge_t w;
    if (!dgx_ellipse_fits(scr, x, y, rx, ry, 0) || !dgx_wedge_set(&w, from, to)) return;
    dgx_ellipse_t      e;
    dgx_ellipse_band_t bands[2][2] = {{{0}}}; /* above the middle and below it; the first run of a row and the second one */
    int                row, start, reach, out[4];
    dgx_ellipse_begin(&e, rx, ry);
    dgx_screen_progress_up(scr);
    while (dgx_ellipse_next(&e, &row, &start, &reach)) {
        /* no further than the screen goes */
        const int left = DGX_MAX(-reach, -x), right = DGX_MIN(reach, scr->width - 1 - x);
        for (int half = 0; half < (row ? 2 : 1); ++half) {
            const int dy = half ? row : -row;
            if (y + dy < 0 || y + dy >= scr->height) continue;
            const int runs = dgx_wedge_row(&w, dy, left, right, out);
            for (int i = 0; i < runs; ++i) dgx_ellipse_band_row(&bands[half][i], scr, x + out[2 * i], x + out[2 * i + 1], y + dy, color);
        }
    }
    for (int i = 0; i < 2; ++i) {
        /* the rows right above the middle and right below it may be equal too */
        dgx_ellipse_band_t *above = &bands[0][i], *below = &bands[1][i];
        if (above->h && below->h && above->x == below->x && above->w == below->w && above->y + above->h == below->y) {
            above->h += below->h;
            below->h = 0;
        }
        dgx_ellipse_band_send(above, scr, color);
        dgx_ellipse_band_send(below, scr, color);
    }
    dgx_screen_progress_down(scr);
}

/*
 * An angle of the screen as the place on the ellipse where its ray meets
 * the curve, in quarters of a turn: the point there is the centre and
 * (rx * cos, -ry * sin) of that. It grows with the angle, a whole turn of
 * one is a whole turn of the other, and a ray along an axis is exact.
 */
static float dgx_arc_quarters(float degrees, int rx, int ry)
{
    const float turns = floorf(degrees / 360.0f);
    float       rest = degrees - turns * 360.0f;
    if (!(rest >= 0 && rest < 360.0f)) rest = 0;
    float quarters = rest / 90.0f;
    if (rx != ry && rx && ry && quarters != floorf(quarters)) {
        const float angle = rest * (DGX_QUARTER / 90.0f);
        quarters = atan2f(rx * sinf(angle), ry * cosf(angle)) / DGX_QUARTER;
        if (quarters < 0) quarters += 4;
    }
    return quarters + 4 * turns;
}

/* the cosine and the sine of so many quarters of a turn; those of a whole number of quarters are exact */
static void dgx_arc_ray(float quarters, float *c, float *s)
{
    const float whole = floorf(quarters);
    float       a = 1, b = 0;
    if (quarters != whole) {
        a = cosf((quarters - whole) * DGX_QUARTER);
        b = sinf((quarters - whole) * DGX_QUARTER);
    }
    switch ((int)(whole - 4 * floorf(whole / 4))) {
    case 0: *c = a, *s = b; break;
    case 1: *c = -b, *s = a; break;
    case 2: *c = -a, *s = -b; break;
    default: *c = b, *s = -a; break;
    }
}

/*
 * A part of an arc by a pen, no more than a quarter of a turn of it, as a
 * cubic curve: such a curve is within 0.03 % of the radius of the arc, and
 * is drawn in straight pieces that are within `within` pixels of it.
 *
 * The pen takes a curve by points that are whole pixels, and the two middle
 * points of this one are not. So the curve is begun by its points made whole
 * and given the coefficients of the true ones after: dgx_bezier4_begin()
 * finds from the middle points the coefficients and the number of pieces,
 * and keeps nothing else found from them. The ends of the arc are brought to
 * whole pixels, as the ends of every curve are, and the middle point next to
 * an end is moved with it. The ends between the quarters are whole as they
 * are.
 */
static void dgx_arc_part(dgx_screen_t *scr, int x, int y, int rx, int ry, float from, float to, bool first, bool last, int width,
                         uint32_t color, float within)
{
    float c0, s0, c1, s1;
    dgx_arc_ray(from, &c0, &s0);
    dgx_arc_ray(to, &c1, &s1);
    const float handle = (4.0f / 3.0f) * tanf((to - from) * DGX_QUARTER / 4);
    float       px[4], py[4];
    px[0] = x + rx * c0, py[0] = y - ry * s0;
    px[1] = px[0] - handle * rx * s0, py[1] = py[0] - handle * ry * c0;
    px[3] = x + rx * c1, py[3] = y - ry * s1;
    px[2] = px[3] + handle * rx * s1, py[2] = py[3] + handle * ry * c1;
    if (first) {
        const float wx = (float)lroundf(px[0]), wy = (float)lroundf(py[0]);
        px[1] += wx - px[0], py[1] += wy - py[0];
        px[0] = wx, py[0] = wy;
    }
    if (last) {
        const float wx = (float)lroundf(px[3]), wy = (float)lroundf(py[3]);
        px[2] += wx - px[3], py[2] += wy - py[3];
        px[3] = wx, py[3] = wy;
    }
    dgx_point_2d_t whole[4];
    for (int i = 0; i < 4; ++i) {
        whole[i].x = (int16_t)DGX_MAX(INT16_MIN, DGX_MIN(INT16_MAX, lroundf(px[i])));
        whole[i].y = (int16_t)DGX_MAX(INT16_MIN, DGX_MIN(INT16_MAX, lroundf(py[i])));
    }
    float kx[3], ky[3];
    kx[2] = 3 * (px[1] - px[0]);
    kx[1] = 3 * (px[2] - px[1]) - kx[2];
    kx[0] = px[3] - px[0] - kx[2] - kx[1];
    ky[2] = 3 * (py[1] - py[0]);
    ky[1] = 3 * (py[2] - py[1]) - ky[2];
    ky[0] = py[3] - py[0] - ky[2] - ky[1];
    /*
     * The number of pieces as dgx_bezier4_begin() counts it: a chord over 1/n of the parameter is not further
     * from the curve than an eighth of its second derivative over n squared, and that derivative is the largest
     * at one of the ends.
     */
    const float at_start = 2 * hypotf(kx[1], ky[1]), at_end = 2 * hypotf(3 * kx[0] + kx[1], 3 * ky[0] + ky[1]);
    int         pieces = (int)ceilf(sqrtf((at_start > at_end ? at_start : at_end) / (8 * within)));
    if (pieces < 1) pieces = 1;
    dgx_bezier_t curve;
    dgx_bezier4_begin(&curve, scr, whole, pieces, width, color);
    for (int i = 0; i < 3; ++i) curve.kx[i] = kx[i], curve.ky[i] = ky[i];
    dgx_bezier_draw_to(&curve, 1);
}

void dgx_draw_arc(dgx_screen_t *scr, int x, int y, int rx, int ry, float from, float to, int width, uint32_t color)
{
    if (width < 1 || (width & ~DGX_PEN_SOFT) < 1) return;
    const int   pen = width & ~DGX_PEN_SOFT;
    dgx_wedge_t w;
    if (!dgx_ellipse_fits(scr, x, y, rx, ry, pen > INT16_MAX ? INT16_MAX : pen) || !dgx_wedge_set(&w, from, to)) return;
    /* what the pen will be on this screen is told by the pen: one asked to be soft is hard where a pixel is not ink */
    dgx_bezier_t         pen_here;
    const dgx_point_2d_t dot[4] = {{(int16_t)x, (int16_t)y}, {(int16_t)x, (int16_t)y}, {(int16_t)x, (int16_t)y}, {(int16_t)x, (int16_t)y}};
    dgx_bezier4_begin(&pen_here, scr, dot, 1, width, color);
    if (!pen_here.soft && pen_here.width == 1) {
        dgx_ellipse_arc(scr, x, y, rx, ry, &w, color);
        return;
    }
    if (x - rx < INT16_MIN || x + rx > INT16_MAX || y - ry < INT16_MIN || y + ry > INT16_MAX) return;
    if (to < from) {
        const float t = from;
        from = to, to = t;
    }
    /*
     * How far the straight pieces may be from the arc, pixels. A hard line is made of whole pixels, and half a
     * pixel is what a curve given no number of pieces gets. A soft line shows where it runs to a fraction of a
     * pixel: with pieces half a pixel off, a small circle is a polygon to the eye.
     */
    const float within = pen_here.soft ? DGX_ARC_WITHIN_SOFT : 0.5f;
    const float start = dgx_arc_quarters(from, rx, ry);
    const float end = w.all ? start + 4 : dgx_arc_quarters(to, rx, ry);
    if (!(end > start)) return;
    dgx_screen_progress_up(scr);
    /* an arc of a quarter of a turn or less is one curve wherever it begins; a longer one is cut where the axes are, at whole pixels */
    const bool one = end - start <= 1.0f;
    for (float at = start; at < end;) {
        float next = one ? end : floorf(at) + 1;
        if (next > end) next = end;
        dgx_arc_part(scr, x, y, rx, ry, at, next, at == start, !(next < end), width, color, within);
        at = next;
    }
    dgx_screen_progress_down(scr);
}
