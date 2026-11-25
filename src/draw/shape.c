/*
 * shape.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Shapes whose outlines lie between pixels: rings, wedges, ellipses,
 *  rectangles with round corners, polygons and lines through points, painted
 *  with a color or a gradient, opaque or translucent, with hard or soft
 *  edges.
 *
 *  Every shape becomes an outline of straight edges in 1/256 of a pixel,
 *  counted from a whole pixel of its own, curves cut into chords within 1/64
 *  of a pixel of them. A row takes from each edge that crosses it the share
 *  of each pixel the edge leaves on its side, in whole numbers, as the
 *  rasterizers of fonts do: a cell of a pixel gets the height the edge goes
 *  down it and the area it leaves to its left, and the sum of the heights of
 *  the cells from the left gives the share of every pixel past them. A point
 *  is inside where the outline goes round it more times one way than the
 *  other, and a row where it may go round twice is cut into bands first, see
 *  dgx_sh_rows(). The share of a pixel is exact, and so the same whatever
 *  part of the shape a clip or a strip of rows leaves, and the shape moved by
 *  whole pixels is the same numbers.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_draw.h"
#include "dgx_vscreen.h"
#include "../dgx_clip_priv.h"
#include "../vscreen/dgx_alpha_priv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Writing straight into the memory of a virtual screen needs vscreen.c, which is optional in the component build. */
#if !defined(ESP_PLATFORM) || defined(CONFIG_DGX_ENABLE_VSCREEN)
#define DGX_SHAPE_MEMORY 1
#endif

static const char TAG[] = "DGX SHAPE";

/* a pixel in the units of an outline */
#define DGX_SH_ONE 256
/* how far a chord of a curve may lie from the curve, pixels: half of it either way */
#define DGX_SH_TOLERANCE (1.0f / 32)
/* the most chords a curve is cut into */
#define DGX_SH_MAX_CHORDS 4096
/* how far from its own pixel a shape may reach, pixels */
#define DGX_SH_REACH 32767.0f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * A square root within a few millionths: the one of the library is slow on a
 * chip without its instruction. The bits of the number go through a union,
 * which the compiler turns into a move between registers; memcpy() there is
 * a call on an ESP32-S3.
 */
static inline float dgx_sh_sqrt(float x)
{
    if (!(x > 0)) return 0;
    union {
        float    f;
        uint32_t i;
    } u = {.f = x};
    u.i     = 0x5f3759dfu - (u.i >> 1);
    float y = u.f;
    y       = y * (1.5f - 0.5f * x * y * y);
    y       = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}

/* ---------------------------------------------------------------- outline */

/* an edge that goes down from (x0, y0) to (x1, y1), or up when dir is -1 */
typedef struct {
    int32_t x0, y0, x1, y1;
    int32_t dir;
    /*
     * Where the edge is at the top of the row being drawn, and the remainder
     * of the division that gave it; the step to the next row, 256 * (x1 - x0)
     * as a quotient and a remainder of y1 - y0: the place is found again by
     * adding, exactly as a division of its own would give it.
     */
    int32_t x, rem, step, step_rem;
} dgx_sh_edge_t;

typedef struct {
    dgx_sh_edge_t *edges;
    int            n, cap;
    int            ox, oy;         /* the whole pixel the outline is counted from */
    int32_t        sx, sy, px, py; /* the start of the contour and its last point */
    bool           open;           /* a contour is begun */
    bool           failed;         /* no memory */
    bool           too_far;        /* a point past DGX_SH_REACH */
    int32_t        xmin, xmax, ymin, ymax;
    float          chord_r, chord_step; /* the radius asked last of dgx_sh_chords(), and the largest angle of a chord of it */
    bool           simple;              /* the outline goes round no point twice, by the way it is made */
    bool           corners;             /* its rows are cut at its corners too: a polygon, of a few long sides */
} dgx_sh_path_t;

static void dgx_sh_path_init(dgx_sh_path_t *p, int ox, int oy)
{
    memset(p, 0, sizeof(*p));
    p->ox = ox, p->oy = oy;
    p->xmin = p->ymin = INT32_MAX;
    p->xmax = p->ymax = INT32_MIN;
}

static void dgx_sh_path_free(dgx_sh_path_t *p)
{
    free(p->edges);
    p->edges = NULL;
}

static void dgx_sh_edge(dgx_sh_path_t *p, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (y0 == y1 || p->failed) return; /* along a row an edge covers nothing */
    if (p->n == p->cap) {
        const int      cap   = p->cap ? p->cap * 2 : 64;
        dgx_sh_edge_t *edges = realloc(p->edges, (size_t)cap * sizeof(*edges));
        if (!edges) {
            p->failed = true;
            return;
        }
        p->edges = edges, p->cap = cap;
    }
    dgx_sh_edge_t *e = &p->edges[p->n++];
    if (y0 < y1) *e = (dgx_sh_edge_t){.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .dir = 1};
    else *e = (dgx_sh_edge_t){.x0 = x1, .y0 = y1, .x1 = x0, .y1 = y0, .dir = -1};
}

/* the next point of the contour, in 1/256 of a pixel from the pixel of the outline */
static void dgx_sh_point_i(dgx_sh_path_t *p, int32_t x, int32_t y)
{
    if (x < p->xmin) p->xmin = x;
    if (x > p->xmax) p->xmax = x;
    if (y < p->ymin) p->ymin = y;
    if (y > p->ymax) p->ymax = y;
    if (!p->open) {
        p->open = true;
        p->sx = p->px = x, p->sy = p->py = y;
        return;
    }
    dgx_sh_edge(p, p->px, p->py, x, y);
    p->px = x, p->py = y;
}

/* the next point, in pixels from the pixel of the outline */
static void dgx_sh_point(dgx_sh_path_t *p, float x, float y)
{
    if (!(x > -DGX_SH_REACH && x < DGX_SH_REACH && y > -DGX_SH_REACH && y < DGX_SH_REACH)) {
        p->too_far = true; /* and what is not a number */
        return;
    }
    /* rounded half away from 0, as lroundf() does, without its call */
    const float X = x * DGX_SH_ONE, Y = y * DGX_SH_ONE;
    dgx_sh_point_i(p, (int32_t)(X + (X >= 0 ? 0.5f : -0.5f)), (int32_t)(Y + (Y >= 0 ? 0.5f : -0.5f)));
}

/* the contour closed back to its start */
static void dgx_sh_close(dgx_sh_path_t *p)
{
    if (!p->open) return;
    dgx_sh_edge(p, p->px, p->py, p->sx, p->sy);
    p->open = false;
}

/*
 * How many chords an arc of `r` pixels over `angle` radians is cut into, and
 * the radius their inner ends are put on: half the depth of a chord off the
 * arc, so that the chords lie as much outside it as inside. The largest angle
 * of a chord is kept for the radius, which the arcs of a shape mostly share.
 */
static int dgx_sh_chords(dgx_sh_path_t *p, float r, float angle, float *rr)
{
    angle = fabsf(angle);
    *rr   = r;
    if (r <= DGX_SH_TOLERANCE || angle <= 0) return 1;
    if (r != p->chord_r) p->chord_r = r, p->chord_step = 2.0f * acosf(1.0f - DGX_SH_TOLERANCE / r);
    const float step = p->chord_step;
    int         k    = step > 0 ? (int)ceilf(angle / step) : DGX_SH_MAX_CHORDS;
    if (k < 1) k = 1;
    if (k > DGX_SH_MAX_CHORDS) k = DGX_SH_MAX_CHORDS;
    /* 1 - cos h by its series: h is under a quarter of a turn */
    const float h = angle / (float)k * 0.5f, h2 = h * h;
    *rr           = r + r * h2 * (0.5f - h2 * (1.0f / 24 - h2 * (1.0f / 720))) * 0.5f;
    return k;
}

/*
 * An arc around (cx, cy) of the radius r from the direction (ux, uy) to
 * (wx, wy), both of length 1, turning by `sweep` radians as the angles of the
 * screen go, y down: by +pi/2 from right to down. Its ends lie on the radius
 * where they are given; the points between are turned one from the other,
 * set again from the angle every 16 so that they do not drift. `first` puts
 * its first point too.
 */
static void dgx_sh_arc_v(dgx_sh_path_t *p, float cx, float cy, float r, float ux, float uy, float wx, float wy, float sweep, bool first)
{
    float     rr;
    const int k = dgx_sh_chords(p, r, sweep, &rr);
    if (first) dgx_sh_point(p, cx + r * ux, cy + r * uy);
    if (k > 1) {
        const float d = sweep / (float)k, c = cosf(d), sn = sinf(d), a0 = k > 16 ? atan2f(uy, ux) : 0;
        float       vx = ux, vy = uy;
        for (int i = 1; i < k; ++i) {
            if (!(i & 15)) {
                vx = cosf(a0 + d * (float)i), vy = sinf(a0 + d * (float)i);
            } else {
                const float t = vx * c - vy * sn;
                vy = vx * sn + vy * c, vx = t;
            }
            dgx_sh_point(p, cx + rr * vx, cy + rr * vy);
        }
    }
    dgx_sh_point(p, cx + r * wx, cy + r * wy);
}

/* a whole circle, a contour of its own, going the way `way` says, 1 or -1 */
static void dgx_sh_circle(dgx_sh_path_t *p, float cx, float cy, float r, float way)
{
    dgx_sh_arc_v(p, cx, cy, r, 1, 0, 1, 0, way * (float)(2 * M_PI), true);
    dgx_sh_close(p);
}

static inline int dgx_sh_floor_div(int v, int d)
{
    return v >= 0 ? v / d : -((-v + d - 1) / d);
}

static inline int32_t dgx_sh_floor256(int32_t v)
{
    return v >= 0 ? v >> 8 : -((-v + 255) >> 8);
}

/* ------------------------------------------------------------------ paint */

enum { DGX_SH_MEM16, DGX_SH_MEM8, DGX_SH_ROW16, DGX_SH_RUNS };

typedef struct {
    dgx_screen_t   *scr;
    uint8_t         bits;
    int             mode;
    uint8_t        *memory;   /* the memory of a linear virtual screen */
    int             pitch;
    uint16_t       *row;      /* the run of a 16-bit screen not in memory, written by set_area and write_area */
    int             run_x, run_n;
    uint32_t        g_color;  /* a run of one color, for dgx_fill_rectangle() */
    int             g_x, g_n;
    int             y;
    bool            soft, read, paper_on, opacity_on;
    uint32_t        paper, opacity, color;
    const uint32_t *lut;      /* the 257 colors of a gradient, or NULL */
    bool            radial;
    float           kx, ky, kr, t_row; /* the gradient: t of a pixel from its distance, and of the row */
    int             gx, gy;
    float           y2;
} dgx_sh_out_t;

static inline uint16_t dgx_sh_swap(uint32_t c)
{
    return (uint16_t)((c >> 8 & 0xffu) | (c & 0xffu) << 8);
}

static inline uint32_t dgx_sh_channel(uint32_t a, uint32_t b, uint32_t t, unsigned shift, uint32_t mask)
{
    const uint32_t u = (a >> shift) & mask, v = (b >> shift) & mask;
    return ((u * (256u - t) + v * t + 128u) >> 8) << shift;
}

/* a value of a pixel of `bits` between a and b, t of 256 of the way: the channels of dgx_alpha_mix() */
static uint32_t dgx_sh_mix256(uint8_t bits, uint32_t a, uint32_t b, uint32_t t)
{
    switch (bits) {
    case 1:
        return t >= 128u ? b : a;
    case 4:
        return dgx_sh_channel(a, b, t, 4, 15);
    case 8:
        return dgx_sh_channel(a, b, t, 0, 255);
    case 12:
        return dgx_sh_channel(a, b, t, 12, 15) | dgx_sh_channel(a, b, t, 8, 15) | dgx_sh_channel(a, b, t, 4, 15);
    case 16:
        return dgx_sh_channel(a, b, t, 11, 31) | dgx_sh_channel(a, b, t, 5, 63) | dgx_sh_channel(a, b, t, 0, 31);
    case 18:
        return dgx_sh_channel(a, b, t, 18, 63) | dgx_sh_channel(a, b, t, 10, 63) | dgx_sh_channel(a, b, t, 2, 63);
    case 24:
        return dgx_sh_channel(a, b, t, 16, 255) | dgx_sh_channel(a, b, t, 8, 255) | dgx_sh_channel(a, b, t, 0, 255);
    default:
        return dgx_sh_channel(a, b, t, 24, 255) | dgx_sh_channel(a, b, t, 16, 255) | dgx_sh_channel(a, b, t, 8, 255) |
               dgx_sh_channel(a, b, t, 0, 255);
    }
}

/* n pixels of one value, 16 bits each: two at a time where they are aligned */
static inline void dgx_sh_fill16(uint16_t *at, int n, uint16_t v)
{
    if (n > 0 && ((uintptr_t)at & 2u)) *at++ = v, --n;
    uint32_t      *w  = (uint32_t *)at;
    const uint32_t vv = (uint32_t)v | (uint32_t)v << 16;
    for (int i = 0; i < n / 2; ++i) w[i] = vv;
    if (n & 1) at[n - 1] = v;
}

/* the run of a 16-bit screen sent */
static void dgx_sh_flush_row(dgx_sh_out_t *o)
{
    if (!o->run_n) return;
    dgx_screen_t *scr = o->scr;
    scr->set_area(scr, (uint16_t)o->run_x, (uint16_t)(o->run_x + o->run_n - 1), (uint16_t)o->y, (uint16_t)o->y);
    scr->write_area(scr, (uint8_t *)o->row, (uint32_t)o->run_n * 16u);
    o->run_n = 0;
}

/* the run of one color drawn */
static void dgx_sh_flush_runs(dgx_sh_out_t *o)
{
    if (!o->g_n) return;
    dgx_fill_rectangle(o->scr, o->g_x, o->y, o->g_n, 1, o->g_color);
    o->g_n = 0;
}

static void dgx_sh_flush(dgx_sh_out_t *o)
{
    if (o->mode == DGX_SH_ROW16) dgx_sh_flush_row(o);
    else if (o->mode == DGX_SH_RUNS) dgx_sh_flush_runs(o);
}

/* where the next pixel of the run of a 16-bit screen goes: x goes on from the run, or a new one begins */
static inline uint16_t *dgx_sh_row_at(dgx_sh_out_t *o, int x)
{
    if (o->run_n && o->run_x + o->run_n != x) dgx_sh_flush_row(o);
    /* the run before may still be on its way to the panel from this buffer */
    if (!o->run_n) {
        if (o->scr->wait) o->scr->wait(o->scr);
        o->run_x = x;
    }
    return o->row + o->run_n;
}

static inline void dgx_sh_write(dgx_sh_out_t *o, int x, uint32_t color)
{
    switch (o->mode) {
    case DGX_SH_MEM16:
        ((uint16_t *)(o->memory + o->y * o->pitch))[x] = dgx_sh_swap(color);
        break;
    case DGX_SH_MEM8:
        (o->memory + o->y * o->pitch)[x] = (uint8_t)color;
        break;
    case DGX_SH_ROW16:
        *dgx_sh_row_at(o, x) = dgx_sh_swap(color);
        ++o->run_n;
        break;
    default:
        if (o->g_n && (o->g_x + o->g_n != x || o->g_color != color)) dgx_sh_flush_runs(o);
        if (!o->g_n) o->g_x = x, o->g_color = color;
        ++o->g_n;
        break;
    }
}

/* the color of the paint at the pixel x of the row */
static inline uint32_t dgx_sh_color(const dgx_sh_out_t *o, int x)
{
    if (!o->lut) return o->color;
    const float X = (float)(x * 16 + 8 - o->gx);
    float       t = o->radial ? dgx_sh_sqrt(X * X + o->y2) * o->kr : X * o->kx + o->t_row;
    if (!(t > 0)) return o->lut[0];
    if (t >= 256.0f) return o->lut[256];
    return o->lut[(int)(t + 0.5f)];
}

/* `color` at x by the weight w of 32: mixed with what lies under it, the paper or nothing */
static void dgx_sh_put(dgx_sh_out_t *o, int x, uint32_t color, uint32_t w)
{
    if (w < 32u) {
        uint32_t under;
        if (o->paper_on) {
            under = o->paper;
        } else if (o->mode == DGX_SH_MEM16) {
            under = dgx_sh_swap(((uint16_t *)(o->memory + o->y * o->pitch))[x]);
        } else if (o->mode == DGX_SH_MEM8) {
            under = (o->memory + o->y * o->pitch)[x];
        } else if (o->read) {
            under = o->scr->get_pixel(o->scr, x, o->y);
        } else {
            /* nothing to mix with: whole from half on */
            if (w < 16u) {
                dgx_sh_flush(o);
                return;
            }
            under = color, w = 32u;
        }
        if (w < 32u) color = dgx_alpha_mix(o->bits, under, color, w);
    }
    dgx_sh_write(o, x, color);
}

/* the weight of 32 a pixel covered `c` of 256 takes */
static inline uint32_t dgx_sh_weight(const dgx_sh_out_t *o, int32_t c)
{
    uint32_t a = o->soft ? (uint32_t)c : c >= 128 ? 256u : 0u;
    if (o->opacity_on) a = (a * o->opacity + 127u) / 255u;
    return (a * 32u + 128u) >> 8;
}

/*
 * n pixels of 16 bits from `at` on, as the wire takes them, of the weight w:
 * in memory, or in a run of a display with nothing under it to read. The
 * colors of a gradient are found as dgx_sh_color() finds them, pixel for
 * pixel the same.
 */
static void dgx_sh_run16(const dgx_sh_out_t *o, uint16_t *at, int x, int n, uint32_t w)
{
    if (!o->lut) {
        if (w == 32u) {
            dgx_sh_fill16(at, n, dgx_sh_swap(o->color));
            return;
        }
        /* one color by one weight: over the paper one color, over memory each pixel mixed by one multiplication a channel */
        const uint32_t pre = ((o->color | o->color << 16) & 0x07E0F81Fu) * w;
        if (o->paper_on) {
            dgx_sh_fill16(at, n, dgx_sh_swap(dgx_bus_overlay_mix((uint16_t)o->paper, pre, 32u - w)));
            return;
        }
        for (int i = 0; i < n; ++i) at[i] = dgx_sh_swap(dgx_bus_overlay_mix(dgx_sh_swap(at[i]), pre, 32u - w));
        return;
    }
    const uint32_t *lut = o->lut;
    const float     kx = o->kx, kr = o->kr, t_row = o->t_row, y2 = o->y2;
    const bool      radial = o->radial, paper_on = o->paper_on;
    const uint16_t  paper  = (uint16_t)o->paper;
    int             X      = x * 16 + 8 - o->gx;
    for (int i = 0; i < n; ++i, X += 16) {
        const float    fx = (float)X;
        const float    t  = radial ? dgx_sh_sqrt(fx * fx + y2) * kr : fx * kx + t_row;
        const uint16_t c  = (uint16_t)lut[t > 0 ? t < 256.0f ? (int)(t + 0.5f) : 256 : 0];
        at[i]             = dgx_sh_swap(w == 32u ? c : dgx_alpha_mix16(paper_on ? paper : dgx_sh_swap(at[i]), c, w));
    }
}

/* n pixels from x on, each covered `c` of 256 */
static void dgx_sh_span(dgx_sh_out_t *o, int x, int n, int32_t c)
{
    const uint32_t w = dgx_sh_weight(o, c);
    if (!w) {
        dgx_sh_flush(o);
        return;
    }
    if (o->mode == DGX_SH_MEM16) {
        dgx_sh_run16(o, (uint16_t *)(o->memory + o->y * o->pitch) + x, x, n, w);
        return;
    }
    if (o->mode == DGX_SH_ROW16 && (w == 32u || o->paper_on)) {
        dgx_sh_run16(o, dgx_sh_row_at(o, x), x, n, w);
        o->run_n += n;
        return;
    }
    if (w == 32u && !o->lut && o->mode != DGX_SH_ROW16) {
        if (o->mode == DGX_SH_MEM8) {
            memset(o->memory + o->y * o->pitch + x, (int)(o->color & 0xffu), (size_t)n);
        } else {
            /* the run of one color goes on by n */
            dgx_sh_write(o, x, o->color);
            o->g_n += n - 1;
        }
        return;
    }
    for (int i = 0; i < n; ++i) dgx_sh_put(o, x + i, dgx_sh_color(o, x + i), w);
}

/* -------------------------------------------------------------- the cells */

/* the most bands a row is cut into, where its edges cross and where its corners lie */
#define DGX_SH_MAX_BANDS 48
/* the most edges a row of an outline that may go round a point twice has, for it to be cut into bands */
#define DGX_SH_CUT_EDGES 12

/* the cell of a pixel of a band of a row: the height edges go down it, and twice the area they leave to their left */
typedef struct {
    int32_t x;
    int32_t cover, area;
    int32_t band;
} dgx_sh_cell_t;

typedef struct {
    dgx_sh_cell_t *cells;
    int            n, cap;
    bool           failed;
    int32_t        band;        /* the band the edges go into now */
    int32_t        left, right; /* the pixels drawn, counted from the pixel of the outline */
    dgx_sh_cell_t *other;       /* for sorting many cells by counting: a second list, and a count for each pixel */
    int            other_cap;
    int           *counts;
} dgx_sh_row_t;

static void dgx_sh_cell(dgx_sh_row_t *r, int32_t ex, int32_t cover, int32_t area)
{
    if (ex > r->right || (!cover && !area)) return; /* right of the pixels drawn nothing reaches them */
    if (ex < r->left) ex = r->left - 1, area = 0;    /* left of them only the height does */
    if (r->n && r->cells[r->n - 1].x == ex && r->cells[r->n - 1].band == r->band) {
        r->cells[r->n - 1].cover += cover, r->cells[r->n - 1].area += area;
        return;
    }
    if (r->n == r->cap) {
        const int      cap   = r->cap ? r->cap * 2 : 256;
        dgx_sh_cell_t *cells = realloc(r->cells, (size_t)cap * sizeof(*cells));
        if (!cells) {
            r->failed = true;
            return;
        }
        r->cells = cells, r->cap = cap;
    }
    r->cells[r->n++] = (dgx_sh_cell_t){ex, cover, area, r->band};
}

/*
 * An edge within a band of a row, from (x1, y1) to (x2, y2), y of 0 to 256
 * in the row: the cells of the pixels it goes through get the height it goes
 * down each and twice the area it leaves to its left there, by steps of whole
 * numbers that carry their remainders, so the parts of a pixel add up exactly.
 */
static void dgx_sh_hline(dgx_sh_row_t *r, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if (y1 == y2) return;
    const int32_t ex1 = dgx_sh_floor256(x1), ex2 = dgx_sh_floor256(x2);
    const int32_t fx1 = x1 - ex1 * DGX_SH_ONE, fx2 = x2 - ex2 * DGX_SH_ONE;
    if (ex1 == ex2) {
        const int32_t d = y2 - y1;
        dgx_sh_cell(r, ex1, d, (fx1 + fx2) * d);
        return;
    }
    int32_t p, first, incr, dx = x2 - x1;
    if (dx > 0) {
        p = (DGX_SH_ONE - fx1) * (y2 - y1), first = DGX_SH_ONE, incr = 1;
    } else {
        p = fx1 * (y2 - y1), first = 0, incr = -1, dx = -dx;
    }
    int32_t delta = p / dx, mod = p % dx;
    if (mod < 0) --delta, mod += dx;
    dgx_sh_cell(r, ex1, delta, (fx1 + first) * delta);
    int32_t ex = ex1 + incr, y = y1 + delta;
    if (ex != ex2) {
        p            = DGX_SH_ONE * (y2 - y + delta);
        int32_t lift = p / dx, rem = p % dx;
        if (rem < 0) --lift, rem += dx;
        mod -= dx;
        while (ex != ex2) {
            /* past the right end of the pixels drawn nothing reaches them; past the left end only the heights do */
            if (incr > 0 && ex > r->right) return;
            if (incr < 0 && ex < r->left) {
                dgx_sh_cell(r, ex, y2 - y, 0);
                return;
            }
            delta = lift, mod += rem;
            if (mod >= 0) mod -= dx, ++delta;
            dgx_sh_cell(r, ex, delta, DGX_SH_ONE * delta);
            y += delta, ex += incr;
        }
    }
    delta = y2 - y;
    dgx_sh_cell(r, ex2, delta, (fx2 + DGX_SH_ONE - first) * delta);
}

/* num / den rounded down, and what is left, 0 .. den - 1; den > 0 */
static inline int64_t dgx_sh_div(int64_t num, int64_t den, int64_t *left)
{
    int64_t q = num / den, r = num % den;
    if (r < 0) --q, r += den;
    *left = r;
    return q;
}

/* where the edge is at y, between its ends: rounded down, the same for the band above and the band below */
static inline int32_t dgx_sh_x_at(const dgx_sh_edge_t *e, int32_t y)
{
    if (y <= e->y0) return e->x0;
    if (y >= e->y1) return e->x1;
    int64_t r;
    return e->x0 + (int32_t)dgx_sh_div((int64_t)(y - e->y0) * (e->x1 - e->x0), e->y1 - e->y0, &r);
}

/* the edge at the top of a row, y, below its start: by a division the first time, then a row at a time by adding */
static inline void dgx_sh_edge_at(dgx_sh_edge_t *e, int32_t y, bool first)
{
    const int64_t dy = e->y1 - e->y0;
    if (first) {
        int64_t r;
        e->x        = e->x0 + (int32_t)dgx_sh_div((int64_t)(y - e->y0) * (e->x1 - e->x0), dy, &r);
        e->rem      = (int32_t)r;
        e->step     = (int32_t)dgx_sh_div((int64_t)DGX_SH_ONE * (e->x1 - e->x0), dy, &r);
        e->step_rem = (int32_t)r;
        return;
    }
    e->x += e->step, e->rem += e->step_rem;
    if (e->rem >= dy) e->rem -= (int32_t)dy, ++e->x;
}

/* the share of 256 of a pixel from the heights to its left and its own area, within the height h of its band */
static inline int32_t dgx_sh_cover(int32_t v, int32_t h)
{
    if (v < 0) v = -v;
    v >>= 9;
    return v > h ? h : v;
}



/*
 * The cells of a row by x: a few by insertion, nearly in order as they mostly
 * are; many by counting them for each pixel, from left - 1 to right, where
 * dgx_sh_cell() keeps them. False when the second list gets no memory.
 */
static bool dgx_sh_sort_cells(dgx_sh_row_t *r)
{
    dgx_sh_cell_t *c = r->cells;
    const int      n = r->n;
    if (n <= 32) {
        for (int i = 1; i < n; ++i) {
            const dgx_sh_cell_t v = c[i];
            int                 j = i - 1;
            while (j >= 0 && c[j].x > v.x) c[j + 1] = c[j], --j;
            c[j + 1] = v;
        }
        return true;
    }
    if (!r->counts) {
        r->counts = malloc((size_t)(r->right - r->left + 3) * sizeof(int));
        if (!r->counts) return false;
    }
    if (r->other_cap < r->cap) {
        dgx_sh_cell_t *other = realloc(r->other, (size_t)r->cap * sizeof(*other));
        if (!other) return false;
        r->other = other, r->other_cap = r->cap;
    }
    int    *counts = r->counts;
    int32_t base = c[0].x, last = c[0].x;
    for (int i = 1; i < n; ++i) {
        if (c[i].x < base) base = c[i].x;
        if (c[i].x > last) last = c[i].x;
    }
    const int width = last - base + 1;
    memset(counts, 0, (size_t)(width + 1) * sizeof(int));
    for (int i = 0; i < n; ++i) ++counts[c[i].x - base + 1];
    for (int i = 1; i <= width; ++i) counts[i] += counts[i - 1];
    for (int i = 0; i < n; ++i) r->other[counts[c[i].x - base]++] = c[i];
    dgx_sh_cell_t *t = r->cells;
    const int      k = r->cap;
    r->cells = r->other, r->cap = r->other_cap;
    r->other = t, r->other_cap = k;
    return true;
}

/* an edge of a row: where it is at the top and the bottom of its part in the row, y of 0 to 256 */
typedef struct {
    const dgx_sh_edge_t *e;
    int32_t              ya, yb, xa, xb, lo, hi;
} dgx_sh_part_t;

/* an edge within a band, and where it lies across it: the sum of the x of its ends */
typedef struct {
    int32_t xa, ya, xb, yb, dir;
    int64_t mid;
} dgx_sh_piece_t;

static int dgx_sh_by_y(const void *a, const void *b)
{
    const int32_t ya = *(const int32_t *)a, yb = *(const int32_t *)b;
    return (ya > yb) - (ya < yb);
}

/*
 * The rows of the outline painted. A ring, an ellipse, a rectangle and a
 * wedge go round no point twice, and a row of theirs is counted whole. A row
 * of a polygon is cut into bands where two of its edges cross and where a
 * corner lies, and a row of a line with no corner in it where its edges
 * cross: within such a band every edge goes across it whole in the order it
 * keeps, the windings between edges are whole numbers, and a pixel the
 * outline goes round twice is not counted twice where it meets one it does
 * not go round at all. Other rows of a line, with its many short edges, are
 * counted by the ways of their edges, and a pixel on the edge where the line
 * lies over itself may come out darker. The bands are found from the edges
 * alone, so they are the same wherever the row is drawn.
 */
static esp_err_t dgx_sh_rows(dgx_sh_path_t *path, dgx_sh_out_t *o, int x0, int x1, int y0, int y1)
{
    dgx_sh_part_t  *parts  = malloc((size_t)path->n * sizeof(*parts));
    dgx_sh_piece_t *pieces = malloc((size_t)path->n * sizeof(*pieces));
    int            *active = malloc((size_t)path->n * sizeof(int));
    dgx_sh_row_t    row    = {.left = x0 - path->ox, .right = x1 - path->ox};
    if (!parts || !pieces || !active) {
        free(parts), free(pieces), free(active);
        return ESP_ERR_NO_MEM;
    }
    /* the edges in the order of the rows they begin in, counted into place */
    const int32_t first_row = dgx_sh_floor256(path->ymin), rows = dgx_sh_floor256(path->ymax - 1) - first_row + 1;
    int          *order     = malloc((size_t)path->n * sizeof(int)), *starts = calloc((size_t)rows + 1, sizeof(int));
    if (!order || !starts) {
        free(order), free(starts), free(parts), free(pieces), free(active);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < path->n; ++i) ++starts[dgx_sh_floor256(path->edges[i].y0) - first_row + 1];
    for (int r = 1; r <= rows; ++r) starts[r] += starts[r - 1];
    for (int i = 0; i < path->n; ++i) order[starts[dgx_sh_floor256(path->edges[i].y0) - first_row]++] = i;
    free(starts);
    int next = 0, na = 0;
    for (int y = y0; y <= y1; ++y) {
        const int32_t top = (int32_t)(y - path->oy) * DGX_SH_ONE, bottom = top + DGX_SH_ONE;
        while (next < path->n && path->edges[order[next]].y0 < bottom) active[na++] = order[next++];
        /* the edges of the row, in the order they came in, whatever row the drawing began at */
        int  np = 0, kept = 0;
        bool corner = false; /* a corner of the outline lies within the row */
        for (int i = 0; i < na; ++i) {
            dgx_sh_edge_t *e = &path->edges[active[i]];
            if (e->y1 <= top) continue;
            active[kept++] = active[i];
            dgx_sh_part_t *p = &parts[np++];
            p->e = e, p->ya = (e->y0 > top ? e->y0 : top) - top, p->yb = (e->y1 < bottom ? e->y1 : bottom) - top;
            /* at the top, where it starts or where the row before left it; at the bottom, where it ends or a row on */
            if (e->y0 < top) {
                if (y == y0) dgx_sh_edge_at(e, top, true);
                p->xa = e->x;
            } else {
                p->xa = e->x0;
            }
            if (e->y1 > bottom) {
                dgx_sh_edge_at(e, bottom, e->y0 >= top);
                p->xb = e->x;
            } else {
                p->xb = e->x1;
            }
            p->lo = p->xa < p->xb ? p->xa : p->xb, p->hi = p->xa < p->xb ? p->xb : p->xa;
            corner = corner || p->ya > 0 || p->yb < DGX_SH_ONE;
        }
        na = kept;
        /* where two edges cross within the row: the bands begin there */
        int32_t splits[DGX_SH_MAX_BANDS + 1];
        int     ns = 0;
        splits[ns++] = 0;
        /*
         * An outline that may go round a point twice, with a few edges in the
         * row: the bands begin where two edges cross, and in a polygon where a
         * corner lies, so that every edge goes across a band whole. A row of a
         * line with a corner in it, or of more edges, is counted by the ways of
         * its edges.
         */
        const bool cut = !path->simple && np <= DGX_SH_CUT_EDGES && (path->corners || !corner);
        if (cut && corner)
            for (int i = 0; i < np; ++i) {
                if (parts[i].ya > 0) splits[ns++] = parts[i].ya;
                if (parts[i].yb < DGX_SH_ONE) splits[ns++] = parts[i].yb;
            }
        for (int i = 0; i < np && ns < DGX_SH_MAX_BANDS && cut; ++i)
            for (int j = i + 1; j < np && ns < DGX_SH_MAX_BANDS; ++j) {
                const dgx_sh_part_t *a = &parts[i], *b = &parts[j];
                if (a->hi < b->lo || b->hi < a->lo) continue;
                const int32_t lo = a->ya > b->ya ? a->ya : b->ya, hi = a->yb < b->yb ? a->yb : b->yb;
                if (hi - lo < 2) continue;
                const int64_t dl = (int64_t)(lo == a->ya ? a->xa : dgx_sh_x_at(a->e, top + lo)) - (lo == b->ya ? b->xa : dgx_sh_x_at(b->e, top + lo));
                const int64_t dh = (int64_t)(hi == a->yb ? a->xb : dgx_sh_x_at(a->e, top + hi)) - (hi == b->yb ? b->xb : dgx_sh_x_at(b->e, top + hi));
                if (!((dl < 0 && dh > 0) || (dl > 0 && dh < 0))) continue;
                const int32_t yc = lo + (int32_t)((hi - lo) * dl / (dl - dh));
                if (yc > lo && yc < hi) splits[ns++] = yc;
            }
        if (ns > 2) qsort(splits + 1, (size_t)(ns - 1), sizeof(int32_t), dgx_sh_by_y);
        splits[ns] = DGX_SH_ONE;
        /* the cells of every band */
        int32_t height[DGX_SH_MAX_BANDS];
        int     nb = 0;
        row.n      = 0;
        for (int s = 0; s < ns; ++s) {
            const int32_t from = splits[s], to = splits[s + 1];
            if (to <= from) continue;
            row.band = nb, height[nb++] = to - from;
            /* an outline that may go round a point twice, where every edge goes across the whole band */
            bool span = !path->simple;
            for (int i = 0; i < np && span; ++i) {
                const dgx_sh_part_t *p = &parts[i];
                if (p->ya < to && p->yb > from && (p->ya > from || p->yb < to)) span = false;
            }
            if (!span) {
                for (int i = 0; i < np; ++i) {
                    const dgx_sh_part_t *p  = &parts[i];
                    const int32_t        ya = p->ya > from ? p->ya : from, yb = p->yb < to ? p->yb : to;
                    if (yb <= ya) continue;
                    const int32_t xa = ya == p->ya ? p->xa : dgx_sh_x_at(p->e, top + ya), xb = yb == p->yb ? p->xb : dgx_sh_x_at(p->e, top + yb);
                    if (p->e->dir > 0) dgx_sh_hline(&row, xa, ya, xb, yb);
                    else dgx_sh_hline(&row, xb, yb, xa, ya);
                }
                continue;
            }
            /* the edges of the band in their order across it, which they keep within it */
            int nq = 0;
            for (int i = 0; i < np; ++i) {
                const dgx_sh_part_t *p  = &parts[i];
                const int32_t        ya = p->ya > from ? p->ya : from, yb = p->yb < to ? p->yb : to;
                if (yb <= ya) continue;
                dgx_sh_piece_t q = {.xa = ya == p->ya ? p->xa : dgx_sh_x_at(p->e, top + ya), .ya = ya, .yb = yb, .dir = p->e->dir};
                q.xb             = yb == p->yb ? p->xb : dgx_sh_x_at(p->e, top + yb);
                q.mid            = (int64_t)q.xa + q.xb;
                int j            = nq++;
                while (j > 0 && pieces[j - 1].mid > q.mid) pieces[j] = pieces[j - 1], --j;
                pieces[j] = q;
            }
            /*
             * Where every edge goes across the whole band, the winding between
             * two of them is a whole number. An edge with the outline round both
             * its sides is inside the shape and leaves nothing; the others go
             * into it or out of it, whichever way and however many times the
             * outline goes round, so a pixel the shape covers is counted once.
             * A band where a corner of the outline lies is counted by the ways
             * of the edges themselves.
             */
            int w = 0;
            for (int i = 0; i < nq; ++i) {
                const dgx_sh_piece_t *q  = &pieces[i];
                const int             wl = w;
                w += q->dir;
                if (wl && w) continue;
                const int first = row.n;
                if (!wl) dgx_sh_hline(&row, q->xa, q->ya, q->xb, q->yb);
                else dgx_sh_hline(&row, q->xb, q->yb, q->xa, q->ya);
                /* the cells of an edge that goes left turned round: the edges go left to right, and so do nearly all the cells */
                for (int a = first, b = row.n - 1; a < b; ++a, --b) {
                    const dgx_sh_cell_t c = row.cells[a];
                    row.cells[a] = row.cells[b], row.cells[b] = c;
                }
            }
        }
        if (row.failed || !dgx_sh_sort_cells(&row)) {
            row.failed = true;
            break;
        }
        /* the pixels from left to right: each band's share by the heights so far, a cell's own area at its pixel */
        int32_t acc[DGX_SH_MAX_BANDS] = {0};
        int32_t run                   = 0; /* the share of the pixels between cells */
        int32_t at                    = row.left;
        o->y                          = y;
        if (o->lut) {
            const float Y = (float)(y * 16 + 8 - o->gy);
            o->t_row = Y * o->ky, o->y2 = Y * Y;
        }
        for (int i = 0; i < row.n;) {
            const int32_t x = row.cells[i].x;
            int           j = i;
            while (j < row.n && row.cells[j].x == x) ++j;
            if (x >= row.left && x > at) dgx_sh_span(o, path->ox + at, x - at, run);
            int32_t own = 0;
            if (nb == 1) {
                int32_t cover = 0, area = 0;
                for (int k = i; k < j; ++k) cover += row.cells[k].cover, area += row.cells[k].area;
                acc[0] += cover;
                own = dgx_sh_cover(acc[0] * 512 - area, DGX_SH_ONE);
                run = dgx_sh_cover(acc[0] * 512, DGX_SH_ONE);
            } else {
                int32_t area[DGX_SH_MAX_BANDS] = {0};
                for (int k = i; k < j; ++k) acc[row.cells[k].band] += row.cells[k].cover, area[row.cells[k].band] += row.cells[k].area;
                run = 0;
                for (int b = 0; b < nb; ++b) own += dgx_sh_cover(acc[b] * 512 - area[b], height[b]), run += dgx_sh_cover(acc[b] * 512, height[b]);
            }
            if (x >= row.left) dgx_sh_span(o, path->ox + x, 1, own), at = x + 1;
            i = j;
        }
        if (at <= row.right) dgx_sh_span(o, path->ox + at, row.right - at + 1, run);
        dgx_sh_flush(o);
    }
    free(parts), free(pieces), free(active), free(order);
    const bool failed = row.failed;
    free(row.cells), free(row.other), free(row.counts);
    return failed ? ESP_ERR_NO_MEM : ESP_OK;
}

/* the outline painted on the screen */
static esp_err_t dgx_sh_fill(dgx_screen_t *scr, dgx_sh_path_t *path, const dgx_paint_t *paint)
{
    dgx_sh_close(path);
    if (path->failed) {
        ESP_LOGE(TAG, "failed to allocate the outline");
        return ESP_ERR_NO_MEM;
    }
    if (path->too_far) {
        ESP_LOGE(TAG, "shape reaches past %d pixels", (int)DGX_SH_REACH);
        return ESP_ERR_INVALID_ARG;
    }
    if (!path->n) return ESP_OK;
    if ((paint->flags & DGX_PAINT_LINEAR) && (paint->flags & DGX_PAINT_RADIAL)) {
        ESP_LOGE(TAG, "a gradient is linear or radial, not both");
        return ESP_ERR_INVALID_ARG;
    }
    /* the pixels the outline may touch */
    const int px0 = path->ox + dgx_sh_floor256(path->xmin), px1 = path->ox + dgx_sh_floor256(path->xmax);
    const int py0 = path->oy + dgx_sh_floor256(path->ymin), py1 = path->oy + dgx_sh_floor256(path->ymax - 1);
    if (dgx_clip_misses(scr, px0, px1, py0, py1)) return ESP_OK;
    const int cl = dgx_clip_left(scr), cr = dgx_clip_right(scr), ct = dgx_clip_top(scr), cb = dgx_clip_bottom(scr);
    const int x0 = px0 > cl ? px0 : cl, x1 = px1 < cr - 1 ? px1 : cr - 1;
    const int y0 = py0 > ct ? py0 : ct, y1 = py1 < cb - 1 ? py1 : cb - 1;
    if (x0 > x1 || y0 > y1) return ESP_OK;

    dgx_sh_out_t o = {
        .scr        = scr,
        .bits       = scr->color_bits,
        .mode       = DGX_SH_RUNS,
        .soft       = (paint->flags & DGX_PAINT_SOFT) != 0,
        .read       = dgx_screen_is_readable(scr),
        .paper_on   = (paint->flags & DGX_PAINT_PAPER) != 0,
        .opacity_on = (paint->flags & DGX_PAINT_OPACITY) != 0,
        .paper      = paint->paper,
        .opacity    = paint->opacity,
        .color      = paint->color,
    };
#ifdef DGX_SHAPE_MEMORY
    if ((o.bits == 16 || o.bits == 8) && dgx_vscreen_is_linear(scr)) {
        o.mode   = o.bits == 16 ? DGX_SH_MEM16 : DGX_SH_MEM8;
        o.memory = ((dgx_vscreen_t *)scr)->v_array;
        o.pitch  = scr->width * (o.bits / 8);
    }
#endif
    uint16_t *own_row = NULL;
    if (o.mode == DGX_SH_RUNS && o.bits == 16 && scr->set_area && scr->write_area) {
        const uint32_t row_bytes = (uint32_t)scr->width * 2u;
        o.mode                   = DGX_SH_ROW16;
        o.row = scr->draw_buffer && scr->draw_buffer_len >= row_bytes && !((uintptr_t)scr->draw_buffer & 1u) ? (uint16_t *)scr->draw_buffer : NULL;
        if (!o.row) {
            o.row = own_row = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
            if (!own_row) {
                ESP_LOGE(TAG, "failed to allocate a %u-byte row", (unsigned)row_bytes);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    uint32_t *lut = NULL;
    if (paint->flags & (DGX_PAINT_LINEAR | DGX_PAINT_RADIAL)) {
        lut = malloc(257 * sizeof(uint32_t));
        if (!lut) {
            free(own_row);
            ESP_LOGE(TAG, "failed to allocate a gradient");
            return ESP_ERR_NO_MEM;
        }
        for (uint32_t t = 0; t <= 256; ++t) lut[t] = dgx_sh_mix256(o.bits, paint->color, paint->color2, t);
        const float vx = (float)(paint->gx[1] - paint->gx[0]), vy = (float)(paint->gy[1] - paint->gy[0]);
        const float len2 = vx * vx + vy * vy;
        o.lut = lut, o.gx = paint->gx[0], o.gy = paint->gy[0];
        o.radial = (paint->flags & DGX_PAINT_RADIAL) != 0;
        /* a gradient of no length is its end color everywhere */
        if (len2 > 0) o.kx = vx * 256.0f / len2, o.ky = vy * 256.0f / len2, o.kr = 256.0f / sqrtf(len2);
        else o.kx = o.ky = 0, o.kr = 1e30f;
    }
    dgx_screen_batch_begin(scr);
    const esp_err_t err = dgx_sh_rows(path, &o, x0, x1, y0, y1);
    if (err != ESP_OK) ESP_LOGE(TAG, "failed to allocate the cells of a row");
    free(lut);
    if (own_row) {
        if (scr->wait) scr->wait(scr);
        free(own_row);
    }
    if (o.mode == DGX_SH_MEM16 || o.mode == DGX_SH_MEM8 || o.mode == DGX_SH_ROW16) dgx_screen_touch(scr, x0, x1, y0, y1);
    dgx_screen_batch_end(scr);
    return err;
}

/* ----------------------------------------------------------------- shapes */

/* a place in sixteenths: its whole pixel, and what is left of it in pixels */
static inline int dgx_sh_pixel_of(int v)
{
    return dgx_sh_floor_div(v, DGX_SUBPIXEL);
}

static inline float dgx_sh_from(int v, int o)
{
    return (float)(v - o * DGX_SUBPIXEL) / DGX_SUBPIXEL;
}

esp_err_t dgx_fill_ring_with(dgx_screen_t *scr, const dgx_ring_t *ring)
{
    if (!scr || !ring) return ESP_ERR_INVALID_ARG;
    if (ring->r_outer <= 0 || ring->r_inner < 0 || ring->r_inner >= ring->r_outer) return ESP_OK;
    float a = ring->from, b = ring->to;
    if (!(a == a) || !(b == b) || a == b) return ESP_OK;
    if (a > b) {
        const float t = a;
        a = b, b = t;
    }
    const int ox = dgx_sh_pixel_of(ring->x), oy = dgx_sh_pixel_of(ring->y);
    const float cx = dgx_sh_from(ring->x, ox), cy = dgx_sh_from(ring->y, oy);
    const float ro = (float)ring->r_outer / DGX_SUBPIXEL, ri = (float)ring->r_inner / DGX_SUBPIXEL;
    dgx_sh_path_t p;
    dgx_sh_path_init(&p, ox, oy);
    /* round ends that come near each other across the gap of a ring may overlap */
    p.simple = !ring->round || (360.0f - (b - a)) * (float)(M_PI / 180.0) * (ro + ri) * 0.5f > ro - ri + 1.0f;
    if (b - a >= 360.0f) {
        dgx_sh_circle(&p, cx, cy, ro, 1.0f);
        if (ri > 0) dgx_sh_circle(&p, cx, cy, ri, -1.0f);
    } else {
        /* the angles of the screen go clockwise, y down: those of a ring the other way */
        const float A = -a * (float)(M_PI / 180.0), B = -b * (float)(M_PI / 180.0), PI = (float)M_PI;
        const float ax = cosf(A), ay = sinf(A), bx = cosf(B), by = sinf(B);
        dgx_sh_arc_v(&p, cx, cy, ro, ax, ay, bx, by, B - A, true);
        if (ring->round) {
            const float hw = (ro - ri) * 0.5f, rm = (ro + ri) * 0.5f;
            dgx_sh_arc_v(&p, cx + rm * bx, cy + rm * by, hw, bx, by, -bx, -by, -PI, false);
            if (ri > 0) dgx_sh_arc_v(&p, cx, cy, ri, bx, by, ax, ay, A - B, false);
            else dgx_sh_point(&p, cx, cy);
            dgx_sh_arc_v(&p, cx + rm * ax, cy + rm * ay, hw, -ax, -ay, ax, ay, -PI, false);
        } else if (ri > 0) {
            dgx_sh_arc_v(&p, cx, cy, ri, bx, by, ax, ay, A - B, true);
        } else {
            dgx_sh_point(&p, cx, cy);
        }
        dgx_sh_close(&p);
    }
    const esp_err_t err = dgx_sh_fill(scr, &p, &ring->paint);
    dgx_sh_path_free(&p);
    return err;
}

esp_err_t dgx_draw_wedge_with(dgx_screen_t *scr, const dgx_wedge_t *wedge)
{
    if (!scr || !wedge) return ESP_ERR_INVALID_ARG;
    if (wedge->width[0] < 0 || wedge->width[1] < 0 || (wedge->width[0] == 0 && wedge->width[1] == 0)) return ESP_OK;
    const int ox = dgx_sh_pixel_of(wedge->x[0]), oy = dgx_sh_pixel_of(wedge->y[0]);
    const float x1 = dgx_sh_from(wedge->x[0], ox), y1 = dgx_sh_from(wedge->y[0], oy);
    const float x2 = dgx_sh_from(wedge->x[1], ox), y2 = dgx_sh_from(wedge->y[1], oy);
    const float r1 = (float)wedge->width[0] / (2 * DGX_SUBPIXEL), r2 = (float)wedge->width[1] / (2 * DGX_SUBPIXEL);
    const float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
    dgx_sh_path_t p;
    dgx_sh_path_init(&p, ox, oy);
    p.simple = true;
    if (wedge->round) {
        if (len <= fabsf(r1 - r2) || len < 1e-6f) {
            /* one end within the other: the larger disc */
            if (r1 >= r2) dgx_sh_circle(&p, x1, y1, r1, 1.0f);
            else dgx_sh_circle(&p, x2, y2, r2, 1.0f);
        } else {
            /* the lines that touch both discs, at the angle al either side of the way */
            const float ux = dx / len, uy = dy / len, c = (r1 - r2) / len, sn = sqrtf(1 - c * c), al = acosf(c), PI = (float)M_PI;
            const float px = ux * c - uy * sn, py = ux * sn + uy * c, mx = ux * c + uy * sn, my = -ux * sn + uy * c;
            dgx_sh_arc_v(&p, x2, y2, r2, px, py, mx, my, -2 * al, true);
            dgx_sh_arc_v(&p, x1, y1, r1, mx, my, px, py, 2 * al - 2 * PI, true);
            dgx_sh_close(&p);
        }
    } else if (len >= 1e-6f) {
        const float nx = -dy / len, ny = dx / len;
        dgx_sh_point(&p, x1 + nx * r1, y1 + ny * r1);
        dgx_sh_point(&p, x2 + nx * r2, y2 + ny * r2);
        dgx_sh_point(&p, x2 - nx * r2, y2 - ny * r2);
        dgx_sh_point(&p, x1 - nx * r1, y1 - ny * r1);
        dgx_sh_close(&p);
    }
    const esp_err_t err = dgx_sh_fill(scr, &p, &wedge->paint);
    dgx_sh_path_free(&p);
    return err;
}

esp_err_t dgx_fill_ellipse_with(dgx_screen_t *scr, const dgx_ellipse_t *e)
{
    if (!scr || !e) return ESP_ERR_INVALID_ARG;
    if (e->rx <= 0 || e->ry <= 0) return ESP_OK;
    const int ox = dgx_sh_pixel_of(e->x), oy = dgx_sh_pixel_of(e->y);
    const float cx = dgx_sh_from(e->x, ox), cy = dgx_sh_from(e->y, oy);
    const float rx = (float)e->rx / DGX_SUBPIXEL, ry = (float)e->ry / DGX_SUBPIXEL;
    dgx_sh_path_t p;
    dgx_sh_path_init(&p, ox, oy);
    p.simple = true;
    /* the points of a circle of the larger radius, its axes scaled */
    float       rr;
    const float r = rx > ry ? rx : ry;
    const int   k = dgx_sh_chords(&p, r, (float)(2 * M_PI), &rr);
    const float sx = rx * rr / r, sy = ry * rr / r, d = (float)(2 * M_PI) / (float)k, c = cosf(d), sn = sinf(d);
    float       vx = 1, vy = 0;
    for (int i = 0; i < k; ++i) {
        if (i && !(i & 15)) {
            vx = cosf(d * (float)i), vy = sinf(d * (float)i);
        } else if (i) {
            const float t = vx * c - vy * sn;
            vy = vx * sn + vy * c, vx = t;
        }
        dgx_sh_point(&p, cx + sx * vx, cy + sy * vy);
    }
    const esp_err_t err = dgx_sh_fill(scr, &p, &e->paint);
    dgx_sh_path_free(&p);
    return err;
}

esp_err_t dgx_fill_round_rectangle_with(dgx_screen_t *scr, const dgx_round_rectangle_t *rect)
{
    if (!scr || !rect) return ESP_ERR_INVALID_ARG;
    if (rect->w <= 0 || rect->h <= 0) return ESP_OK;
    const int ox = dgx_sh_pixel_of(rect->x), oy = dgx_sh_pixel_of(rect->y);
    dgx_sh_path_t p;
    dgx_sh_path_init(&p, ox, oy);
    p.simple = true;
    int r = rect->r < 0 ? 0 : rect->r;
    if (r > rect->w / 2) r = rect->w / 2;
    if (r > rect->h / 2) r = rect->h / 2;
    if (!r) {
        /* whole sixteenths make whole numbers of the outline */
        const int32_t x = (rect->x - ox * DGX_SUBPIXEL) * (DGX_SH_ONE / DGX_SUBPIXEL), y = (rect->y - oy * DGX_SUBPIXEL) * (DGX_SH_ONE / DGX_SUBPIXEL);
        const int64_t w = (int64_t)rect->w * (DGX_SH_ONE / DGX_SUBPIXEL), h = (int64_t)rect->h * (DGX_SH_ONE / DGX_SUBPIXEL);
        if (w > (int64_t)(DGX_SH_REACH * DGX_SH_ONE) || h > (int64_t)(DGX_SH_REACH * DGX_SH_ONE)) {
            p.too_far = true;
        } else {
            dgx_sh_point_i(&p, x, y);
            dgx_sh_point_i(&p, x + (int32_t)w, y);
            dgx_sh_point_i(&p, x + (int32_t)w, y + (int32_t)h);
            dgx_sh_point_i(&p, x, y + (int32_t)h);
        }
    } else {
        const float x = dgx_sh_from(rect->x, ox), y = dgx_sh_from(rect->y, oy);
        const float w = (float)rect->w / DGX_SUBPIXEL, h = (float)rect->h / DGX_SUBPIXEL, R = (float)r / DGX_SUBPIXEL;
        const float Q = (float)(M_PI / 2);
        dgx_sh_arc_v(&p, x + w - R, y + R, R, 0, -1, 1, 0, Q, true);
        dgx_sh_arc_v(&p, x + w - R, y + h - R, R, 1, 0, 0, 1, Q, true);
        dgx_sh_arc_v(&p, x + R, y + h - R, R, 0, 1, -1, 0, Q, true);
        dgx_sh_arc_v(&p, x + R, y + R, R, -1, 0, 0, -1, Q, true);
    }
    const esp_err_t err = dgx_sh_fill(scr, &p, &rect->paint);
    dgx_sh_path_free(&p);
    return err;
}

esp_err_t dgx_fill_polygon_with(dgx_screen_t *scr, const dgx_polygon_t *polygon)
{
    if (!scr || !polygon) return ESP_ERR_INVALID_ARG;
    if (!polygon->points || polygon->count < 1) {
        ESP_LOGE(TAG, "polygon has no points");
        return ESP_ERR_INVALID_ARG;
    }
    const dgx_point_2d_t *pt = polygon->points;
    const int             ox = dgx_sh_pixel_of(pt[0].x), oy = dgx_sh_pixel_of(pt[0].y);
    dgx_sh_path_t         p;
    dgx_sh_path_init(&p, ox, oy);
    p.corners = true;
    for (int i = 0; i < polygon->count; ++i)
        dgx_sh_point_i(&p, (pt[i].x - ox * DGX_SUBPIXEL) * (DGX_SH_ONE / DGX_SUBPIXEL), (pt[i].y - oy * DGX_SUBPIXEL) * (DGX_SH_ONE / DGX_SUBPIXEL));
    const esp_err_t err = dgx_sh_fill(scr, &p, &polygon->paint);
    dgx_sh_path_free(&p);
    return err;
}

/* ------------------------------------------------------------ the polyline */

typedef struct {
    float x, y;
} dgx_sh_pt_t;

typedef struct {
    dgx_sh_pt_t *p;
    int          n, cap;
    bool         failed;
} dgx_sh_pts_t;

/* the next point of the way, unless it is where the one before is */
static void dgx_sh_pts_add(dgx_sh_pts_t *s, float x, float y)
{
    if (s->failed) return;
    if (s->n && fabsf(s->p[s->n - 1].x - x) < 1.0f / 512 && fabsf(s->p[s->n - 1].y - y) < 1.0f / 512) return;
    if (s->n == s->cap) {
        const int    cap = s->cap ? s->cap * 2 : 64;
        dgx_sh_pt_t *p   = realloc(s->p, (size_t)cap * sizeof(*p));
        if (!p) {
            s->failed = true;
            return;
        }
        s->p = p, s->cap = cap;
    }
    s->p[s->n++] = (dgx_sh_pt_t){x, y};
}

/* a cubic Bezier curve from b0, already on the way, cut into chords within the tolerance */
static void dgx_sh_cubic(dgx_sh_pts_t *s, dgx_sh_pt_t b0, dgx_sh_pt_t b1, dgx_sh_pt_t b2, dgx_sh_pt_t b3)
{
    const float ax = b0.x - 2 * b1.x + b2.x, ay = b0.y - 2 * b1.y + b2.y;
    const float bx = b1.x - 2 * b2.x + b3.x, by = b1.y - 2 * b2.y + b3.y;
    const float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by), l = la > lb ? la : lb;
    /* a chord of the n ones is off the curve by at most 3 l / (4 n^2) */
    int n = (int)ceilf(sqrtf(0.75f * l / DGX_SH_TOLERANCE));
    if (n < 1) n = 1;
    if (n > 1024) n = 1024;
    for (int i = 1; i <= n; ++i) {
        const float t = (float)i / (float)n, u = 1 - t;
        const float k0 = u * u * u, k1 = 3 * u * u * t, k2 = 3 * u * t * t, k3 = t * t * t;
        dgx_sh_pts_add(s, k0 * b0.x + k1 * b1.x + k2 * b2.x + k3 * b3.x, k0 * b0.y + k1 * b1.y + k2 * b2.y + k3 * b3.y);
    }
}

/* the control point of a centripetal Catmull-Rom piece from p1 toward p2, with p0 before p1 */
static dgx_sh_pt_t dgx_sh_catmull(dgx_sh_pt_t p0, dgx_sh_pt_t p1, dgx_sh_pt_t p2)
{
    const float d1 = sqrtf(sqrtf((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y)));
    const float d2 = sqrtf(sqrtf((p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y)));
    if (d1 < 1e-6f || d2 < 1e-6f) return (dgx_sh_pt_t){p1.x + (p2.x - p1.x) / 3, p1.y + (p2.y - p1.y) / 3};
    const float a = d1 * d1, b = d2 * d2, c = 2 * a + 3 * d1 * d2 + b, den = 3 * d1 * (d1 + d2);
    return (dgx_sh_pt_t){(a * p2.x - b * p0.x + c * p1.x) / den, (a * p2.y - b * p0.y + c * p1.y) / den};
}

static float dgx_sh_sign(float v)
{
    return (float)((v > 0) - (v < 0));
}

/* the way of a polyline as points: its own, or a curve through them cut into chords */
static esp_err_t dgx_sh_way(const dgx_polyline_t *l, int ox, int oy, dgx_sh_pts_t *s)
{
    const dgx_point_2d_t *pt = l->points;
    const int             m  = l->count;
    if (l->way == DGX_POLYLINE_MONOTONE) {
        if (l->closed) {
            ESP_LOGE(TAG, "monotone curve cannot be closed");
            return ESP_ERR_INVALID_ARG;
        }
        for (int i = 1; i < m; ++i)
            if (pt[i].x <= pt[i - 1].x) {
                ESP_LOGE(TAG, "monotone curve takes points whose x grows");
                return ESP_ERR_INVALID_ARG;
            }
    }
    if (l->way == DGX_POLYLINE_STRAIGHT || m < 3) {
        for (int i = 0; i < m; ++i) dgx_sh_pts_add(s, dgx_sh_from(pt[i].x, ox), dgx_sh_from(pt[i].y, oy));
        return ESP_OK;
    }
    dgx_sh_pt_t *q = malloc((size_t)m * sizeof(*q));
    if (!q) return ESP_ERR_NO_MEM;
    for (int i = 0; i < m; ++i) q[i] = (dgx_sh_pt_t){dgx_sh_from(pt[i].x, ox), dgx_sh_from(pt[i].y, oy)};
    dgx_sh_pts_add(s, q[0].x, q[0].y);
    if (l->way == DGX_POLYLINE_MONOTONE) {
        /* Steffen's slopes: between two points the curve keeps within them */
        float *tan = malloc((size_t)m * sizeof(float));
        if (!tan) {
            free(q);
            return ESP_ERR_NO_MEM;
        }
        for (int i = 1; i < m - 1; ++i) {
            const float h0 = q[i].x - q[i - 1].x, h1 = q[i + 1].x - q[i].x;
            const float s0 = (q[i].y - q[i - 1].y) / h0, s1 = (q[i + 1].y - q[i].y) / h1;
            const float pp = (s0 * h1 + s1 * h0) / (h0 + h1);
            float       lo = fabsf(s0) < fabsf(s1) ? fabsf(s0) : fabsf(s1);
            if (0.5f * fabsf(pp) < lo) lo = 0.5f * fabsf(pp);
            tan[i] = (dgx_sh_sign(s0) + dgx_sh_sign(s1)) * lo;
        }
        for (int e = 0; e < 2; ++e) {
            /* an end: the slope of a parabola through it, kept on the side of the slope of its piece */
            const int   i = e ? m - 1 : 0, j = e ? m - 2 : 1;
            const float sl = (q[e ? i : j].y - q[e ? j : i].y) / (q[e ? i : j].x - q[e ? j : i].x);
            float       t  = (3 * sl - tan[j]) * 0.5f;
            if (dgx_sh_sign(t) != dgx_sh_sign(sl)) t = 0;
            else if (fabsf(t) > 3 * fabsf(sl)) t = 3 * sl;
            tan[i] = t;
        }
        for (int i = 0; i < m - 1; ++i) {
            const float h = (q[i + 1].x - q[i].x) / 3;
            dgx_sh_cubic(s, q[i], (dgx_sh_pt_t){q[i].x + h, q[i].y + tan[i] * h}, (dgx_sh_pt_t){q[i + 1].x - h, q[i + 1].y - tan[i + 1] * h},
                         q[i + 1]);
        }
        free(tan);
    } else {
        const int pieces = l->closed ? m : m - 1;
        for (int i = 0; i < pieces; ++i) {
            const dgx_sh_pt_t p1 = q[i], p2 = q[(i + 1) % m];
            /* past the ends of an open line, the point across the end from its neighbour */
            const dgx_sh_pt_t p0 = l->closed || i > 0 ? q[(i + m - 1) % m] : (dgx_sh_pt_t){2 * p1.x - p2.x, 2 * p1.y - p2.y};
            const dgx_sh_pt_t p3 = l->closed || i + 2 < m ? q[(i + 2) % m] : (dgx_sh_pt_t){2 * p2.x - p1.x, 2 * p2.y - p1.y};
            dgx_sh_cubic(s, p1, dgx_sh_catmull(p0, p1, p2), dgx_sh_catmull(p3, p2, p1), p2);
        }
    }
    free(q);
    return ESP_OK;
}

/* a piece of the way: its direction, of length 1, and its length */
typedef struct {
    float dx, dy, len;
} dgx_sh_dir_t;

/*
 * One side of a line of the half width hw along the points of the way, the
 * way round when `back`: the line moved left of the way, with round joints
 * where the side is outside a turn and cut at the meeting of the two sides
 * where it is inside, or through the point of the way where the pieces are
 * too short for that. Closed, the joint at the first point too, and the
 * contour closed; open, the round end after the last point. The pieces of
 * the way are given once for both sides; a joint that turns by no more than
 * a chord takes no angle.
 */
static void dgx_sh_side(dgx_sh_path_t *p, const dgx_sh_pt_t *pts, const dgx_sh_dir_t *dirs, int m, bool back, bool closed, float hw, float cos_step)
{
    const int segments = closed ? m : m - 1;
#define Q(i) pts[back ? m - 1 - (i) : (i)]
    /* the piece k of the way as this side goes: the piece m - 2 - k turned round, going back */
#define D(k, f) (back ? -dirs[(2 * m - 2 - (k)) % m].f : dirs[(k)].f)
#define L(k) (back ? dirs[(2 * m - 2 - (k)) % m].len : dirs[(k)].len)
    for (int k = closed ? 0 : 1; k <= (closed ? m - 1 : m - 2); ++k) {
        const dgx_sh_pt_t v  = Q(k);
        const int         k1 = (k + segments - 1) % segments;
        const float       d1x = D(k1, dx), d1y = D(k1, dy), d2x = D(k, dx), d2y = D(k, dy);
        const float       n1x = -d1y, n1y = d1x, n2x = -d2y, n2y = d2x;
        if (!closed && k == 1) dgx_sh_point(p, Q(0).x + n1x * hw, Q(0).y + n1y * hw);
        const float cross = d1x * d2y - d1y * d2x, dot = d1x * d2x + d1y * d2y;
        if (cross <= 0) {
            /* outside the turn: round, from one side to the other, the way the line turns; a turn of a chord or less, straight */
            if (dot >= cos_step) {
                dgx_sh_point(p, v.x + n1x * hw, v.y + n1y * hw);
                dgx_sh_point(p, v.x + n2x * hw, v.y + n2y * hw);
            } else {
                const float turn = cross == 0 ? -(float)M_PI : atan2f(cross, dot);
                dgx_sh_arc_v(p, v.x, v.y, hw, n1x, n1y, n2x, n2y, turn, true);
            }
        } else {
            /* inside: the sides meet hw tan(turn / 2) before the point, if both pieces are twice as long */
            const float trim = 1 + dot > 1e-3f ? hw * cross / (1 + dot) : 1e30f;
            if (trim <= 0.5f * L(k1) && trim <= 0.5f * L(k)) {
                const float k2 = hw / (1 + dot);
                dgx_sh_point(p, v.x + (n1x + n2x) * k2, v.y + (n1y + n2y) * k2);
            } else {
                dgx_sh_point(p, v.x + n1x * hw, v.y + n1y * hw);
                dgx_sh_point(p, v.x, v.y);
                dgx_sh_point(p, v.x + n2x * hw, v.y + n2y * hw);
            }
        }
    }
    if (closed) {
        dgx_sh_close(p);
    } else {
        const dgx_sh_pt_t a = Q(segments - 1), b = Q(segments);
        const float       dx = D(segments - 1, dx), dy = D(segments - 1, dy);
        if (segments == 1) dgx_sh_point(p, a.x - dy * hw, a.y + dx * hw);
        dgx_sh_point(p, b.x - dy * hw, b.y + dx * hw);
        /* the round end: from this side to the other round the end of the way */
        dgx_sh_arc_v(p, b.x, b.y, hw, -dy, dx, dy, -dx, -(float)M_PI, false);
    }
#undef Q
#undef D
#undef L
}

esp_err_t dgx_draw_polyline_with(dgx_screen_t *scr, const dgx_polyline_t *l)
{
    if (!scr || !l) return ESP_ERR_INVALID_ARG;
    if (!l->points || l->count < 1 || l->width < 1) {
        ESP_LOGE(TAG, "polyline takes points and a width");
        return ESP_ERR_INVALID_ARG;
    }
    const int    ox = dgx_sh_pixel_of(l->points[0].x), oy = dgx_sh_pixel_of(l->points[0].y);
    dgx_sh_pts_t s  = {0};
    esp_err_t    err = dgx_sh_way(l, ox, oy, &s);
    if (err == ESP_OK && s.failed) err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        if (err == ESP_ERR_NO_MEM) ESP_LOGE(TAG, "failed to allocate the way of a polyline");
        free(s.p);
        return err;
    }
    bool closed = l->closed;
    /* a closed way that ends where it began: the last point is the first */
    if (closed && s.n > 1 && fabsf(s.p[s.n - 1].x - s.p[0].x) < 1.0f / 512 && fabsf(s.p[s.n - 1].y - s.p[0].y) < 1.0f / 512) --s.n;
    if (s.n < 3) closed = false;
    const float   hw = (float)l->width / (2 * DGX_SUBPIXEL);
    dgx_sh_path_t p;
    dgx_sh_path_init(&p, ox, oy);
    if (s.n == 1) {
        dgx_sh_circle(&p, s.p[0].x, s.p[0].y, hw, 1.0f);
    } else {
        /* the pieces of the way, and the cosine of the largest turn a joint of one chord takes */
        const int     segments = closed ? s.n : s.n - 1;
        dgx_sh_dir_t *dirs     = malloc((size_t)segments * sizeof(*dirs));
        if (!dirs) {
            free(s.p);
            ESP_LOGE(TAG, "failed to allocate the way of a polyline");
            return ESP_ERR_NO_MEM;
        }
        for (int i = 0; i < segments; ++i) {
            const dgx_sh_pt_t a = s.p[i], b = s.p[(i + 1) % s.n];
            const float       dx = b.x - a.x, dy = b.y - a.y, len = dgx_sh_sqrt(dx * dx + dy * dy), k = 1.0f / len;
            dirs[i]                = (dgx_sh_dir_t){dx * k, dy * k, len};
        }
        float rr;
        dgx_sh_chords(&p, hw, 1, &rr);
        const float cos_step = hw <= DGX_SH_TOLERANCE ? -1.0f : cosf(p.chord_step);
        if (closed) {
            dgx_sh_side(&p, s.p, dirs, s.n, false, true, hw, cos_step);
            dgx_sh_side(&p, s.p, dirs, s.n, true, true, hw, cos_step);
        } else {
            /* one contour: along the left side, round the end, back along the other side, round the start */
            dgx_sh_side(&p, s.p, dirs, s.n, false, false, hw, cos_step);
            dgx_sh_side(&p, s.p, dirs, s.n, true, false, hw, cos_step);
            dgx_sh_close(&p);
        }
        free(dirs);
    }
    free(s.p);
    err = dgx_sh_fill(scr, &p, &l->paint);
    dgx_sh_path_free(&p);
    return err;
}
