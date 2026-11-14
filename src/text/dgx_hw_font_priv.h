#pragma once
/*
 * dgx_hw_font_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  What the writing of a handwritten text, hw_font.c, and its morphs,
 *  hw_morph.c, both take strokes by: every element is drawn as a cubic
 *  Bezier curve.
 */

#include <math.h>

#include "dgx_hw_font.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/* any element as a cubic curve drawing the same; x and y have room for its 4 points */
static inline void dgx_hw_cubic(int type, float *x, float *y)
{
    if (type == DGX_HW_CURVE3P) {
        x[3] = x[2], y[3] = y[2];
        x[2] = x[3] + (x[1] - x[3]) * 2 / 3, y[2] = y[3] + (y[1] - y[3]) * 2 / 3;
        x[1] = x[0] + (x[1] - x[0]) * 2 / 3, y[1] = y[0] + (y[1] - y[0]) * 2 / 3;
    } else if (type == DGX_HW_LINE) {
        x[3] = x[1], y[3] = y[1];
        x[1] = x[0] + (x[3] - x[0]) / 3, y[1] = y[0] + (y[3] - y[0]) / 3;
        x[2] = x[0] + (x[3] - x[0]) * 2 / 3, y[2] = y[0] + (y[3] - y[0]) * 2 / 3;
    } else if (type == DGX_HW_DOT) {
        x[1] = x[2] = x[3] = x[0], y[1] = y[2] = y[3] = y[0];
    }
}

/* the bits a pen width carries besides the thickness */
#define DGX_HW_PEN_BITS (DGX_PEN_SOFT | DGX_PEN_START_DISC | DGX_PEN_END_DISC | DGX_PEN_MAX)

/* the scales of a context: 0 across is a pixel a cell, 0 down is the scale across */
static inline void dgx_hw_context_scales(const dgx_hw_context_t *ctx, float *scale_x, float *scale_y)
{
    *scale_x = ctx->scale_x == 0 ? 1 : ctx->scale_x;
    *scale_y = ctx->scale_y == 0 ? *scale_x : ctx->scale_y;
}

/* the pen of a context: a thickness of 0 is 1, and the bits stay */
static inline int dgx_hw_context_pen(const dgx_hw_context_t *ctx)
{
    return (ctx->width & ~DGX_HW_PEN_BITS) ? ctx->width : ctx->width | 1;
}

/*
 * The pixels a pen paints, found without drawing: dgx_hw_bounds() and
 * dgx_hw_morph_bounds().
 *
 * dgx_hw_pixel() rounds a point after the place of the text is added, and it
 * rounds a half away from zero: the same point lands on one pixel or on its
 * neighbor depending on where the text is put. A box must hold both, so its
 * low end takes the lower pixel and its high end the higher one. A value
 * within DGX_HW_HALF_SLACK of a half is treated the same way: added to a
 * large coordinate as a float, it may cross the half.
 */
#define DGX_HW_HALF_SLACK (1.0f / 128)

static inline float dgx_hw_pixel_low(float v)
{
    if (!(v > -32767.0f)) return -32767.0f;
    if (v > 32767.0f) return 32767.0f;
    return ceilf(v - 0.5f - DGX_HW_HALF_SLACK);
}

static inline float dgx_hw_pixel_high(float v)
{
    if (!(v > -32767.0f)) return -32767.0f;
    if (v > 32767.0f) return 32767.0f;
    return floorf(v + 0.5f + DGX_HW_HALF_SLACK);
}

/*
 * The least and the greatest value of a Bezier curve of n control values: 2
 * for a line, 3 or 4 for a curve, 1 for a point. Where the curve turns back
 * between its ends it is taken to reach `swell` further than it does.
 */
static inline void dgx_hw_span(const float *c, int n, float swell, float *least, float *greatest)
{
    float lo = c[0] < c[n - 1] ? c[0] : c[n - 1], hi = c[0] < c[n - 1] ? c[n - 1] : c[0];
    float t[2];
    int   roots = 0;
    if (n == 3) {
        /* the derivative is 0 at one t */
        const float d = c[0] - 2 * c[1] + c[2];
        if (d != 0) t[roots++] = (c[0] - c[1]) / d;
    } else if (n == 4) {
        /* a t^2 + b t + d is a third of the derivative */
        const float a = c[3] - c[0] + 3 * (c[1] - c[2]), b = 2 * (c[0] - 2 * c[1] + c[2]), d = c[1] - c[0];
        if (a != 0) {
            const float disc = b * b - 4 * a * d;
            if (disc >= 0) {
                const float root = sqrtf(disc);
                t[roots++] = (-b - root) / (2 * a);
                t[roots++] = (-b + root) / (2 * a);
            }
        } else if (b != 0) {
            t[roots++] = -d / b;
        }
    }
    for (int i = 0; i < roots; ++i) {
        if (!(t[i] > 0 && t[i] < 1)) continue;
        const float u = 1 - t[i];
        const float v = n == 3 ? u * u * c[0] + 2 * u * t[i] * c[1] + t[i] * t[i] * c[2]
                               : u * u * u * c[0] + 3 * u * u * t[i] * c[1] + 3 * u * t[i] * t[i] * c[2] + t[i] * t[i] * t[i] * c[3];
        if (v - swell < lo) lo = v - swell;
        if (v + swell > hi) hi = v + swell;
    }
    *least = lo, *greatest = hi;
}

/*
 * Where a soft line turns a little at a joint, the joint is a corner, not a
 * round one, and the corner reaches further than a round joint would, by
 * less than this: DGX_STROKE_CORNER of bezier.c, the most ink a pixel may
 * gain by it. That ink is faint, but it is ink. It is counted where a curve
 * turns back; the ends of a curve are round.
 */
#define DGX_HW_CORNER (1.0f / 16)

/*
 * The first and the last column, or row, that a pen may paint along a
 * stroke. v holds the n control values of the stroke along that axis, in
 * pixels from the place of the text, not rounded.
 *
 * The stroke is drawn as straight pieces whose ends lie on the curve through
 * the rounded points, so the middle of the pen stays between the least and
 * the greatest value of that curve.
 *
 * A hard pen rounds the ends of the pieces to whole pixels and paints at
 * most width / 2 pixels either way of them. An even width has no middle
 * pixel: the spare one lies to the right of the direction of the piece, so
 * it may fall on either side of the box.
 *
 * A soft pen does not round them and inks every pixel it covers a part of:
 * (width - 1) / 2 pixels before the pixel its middle is in and width / 2
 * after it, since an even width runs half a pixel to the right and down.
 * Whether a pen asked to be soft is soft depends on the screen, so the box
 * of such a pen holds what it paints either way.
 */
static inline void dgx_hw_extent(const float *v, int n, int width, int *first, int *last)
{
    float     low[4], high[4], least, greatest, unused;
    const int pen = width & ~DGX_HW_PEN_BITS;
    for (int i = 0; i < n; ++i) low[i] = dgx_hw_pixel_low(v[i]), high[i] = dgx_hw_pixel_high(v[i]);
    dgx_hw_span(low, n, 0, &least, &unused);
    dgx_hw_span(high, n, 0, &unused, &greatest);
    *first = (int)ceilf(least - 0.5f - DGX_HW_HALF_SLACK) - pen / 2;
    *last = (int)floorf(greatest + 0.5f + DGX_HW_HALF_SLACK) + pen / 2;
    if (width & DGX_PEN_SOFT) {
        dgx_hw_span(low, n, DGX_HW_CORNER, &least, &unused);
        dgx_hw_span(high, n, DGX_HW_CORNER, &unused, &greatest);
        const int soft_first = (int)floorf(least) - (pen - 1) / 2, soft_last = (int)ceilf(greatest) + pen / 2;
        if (soft_first < *first) *first = soft_first;
        if (soft_last > *last) *last = soft_last;
    }
}

/* a box of pixels gathered stroke by stroke */
typedef struct {
    int  left, top, right, bottom;
    bool any;
} dgx_hw_box_t;

static inline void dgx_hw_box_add(dgx_hw_box_t *box, int left, int right, int top, int bottom)
{
    if (!box->any || left < box->left) box->left = left;
    if (!box->any || right > box->right) box->right = right;
    if (!box->any || top < box->top) box->top = top;
    if (!box->any || bottom > box->bottom) box->bottom = bottom;
    box->any = true;
}

/* the box as the calls return it: its corner and its size, zeros for an empty one; any output may be NULL */
static inline bool dgx_hw_box_give(const dgx_hw_box_t *box, int *left, int *top, int *width, int *height)
{
    if (left) *left = box->any ? box->left : 0;
    if (top) *top = box->any ? box->top : 0;
    if (width) *width = box->any ? box->right - box->left + 1 : 0;
    if (height) *height = box->any ? box->bottom - box->top + 1 : 0;
    return box->any;
}

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
