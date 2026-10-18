#pragma once
/*
 * Scale2x as the engine of games on DGX (dgx-ge, dgx_ge_frame.c) makes it,
 * for the tests to hold the smoothing of DGX against: a picture made 2 or 4
 * times as large whole, row by row, as that code does it. Past the edges of
 * the picture its last pixel goes on; at 4 the first pass is made a point
 * beyond on every side, each such point with its own neighbours.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* a point P with A above, B right, C left and D below made into four */
static inline void ref_scale2x_point(uint8_t P, uint8_t A, uint8_t B, uint8_t C, uint8_t D, uint8_t *top, uint8_t *bottom)
{
    top[0]    = C == A && C != D && A != B ? A : P;
    top[1]    = A == B && A != C && B != D ? B : P;
    bottom[0] = D == C && D != B && C != A ? C : P;
    bottom[1] = B == D && B != A && D != C ? D : P;
}

/* the points x0 .. x0 + w - 1 of a row `width` long, ends going on, into two rows 2 * w long */
static void ref_scale2x_row(const uint8_t *up, const uint8_t *row, const uint8_t *down, int width, int x0, int w, uint8_t *top, uint8_t *bottom)
{
    const int last = width - 1;
    for (int i = 0; i < w; ++i) {
        const int x = x0 + i, at = x < 0 ? 0 : x > last ? last : x;
        const int l = x - 1 < 0 ? 0 : x - 1 > last ? last : x - 1, r = x + 1 < 0 ? 0 : x + 1 > last ? last : x + 1;
        ref_scale2x_point(row[at], up[at], row[r], row[l], down[at], top + 2 * i, bottom + 2 * i);
    }
}

static const uint8_t *ref_row_of(const uint8_t *src, int w, int h, int y)
{
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    return src + (size_t)y * w;
}

/* the picture w x h made n (2 or 4) times as large into out, n * w by n * h */
static void ref_smooth(const uint8_t *src, int w, int h, int n, uint8_t *out)
{
    const int ow = n * w;
    const int bw = 2 * (w + 2);
    uint8_t  *b  = malloc((size_t)6 * bw);
    for (int y = 0; y < h; ++y) {
        if (n == 2) {
            ref_scale2x_row(ref_row_of(src, w, h, y - 1), ref_row_of(src, w, h, y), ref_row_of(src, w, h, y + 1), w, 0, w, out + (size_t)(2 * y) * ow,
                            out + (size_t)(2 * y + 1) * ow);
            continue;
        }
        for (int r = 0; r < 3; ++r)
            ref_scale2x_row(ref_row_of(src, w, h, y - 2 + r), ref_row_of(src, w, h, y - 1 + r), ref_row_of(src, w, h, y + r), w, -1, w + 2,
                            b + (size_t)(2 * r) * bw, b + (size_t)(2 * r + 1) * bw);
        ref_scale2x_row(b + (size_t)1 * bw, b + (size_t)2 * bw, b + (size_t)3 * bw, bw, 2, 2 * w, out + (size_t)(4 * y) * ow,
                        out + (size_t)(4 * y + 1) * ow);
        ref_scale2x_row(b + (size_t)2 * bw, b + (size_t)3 * bw, b + (size_t)4 * bw, bw, 2, 2 * w, out + (size_t)(4 * y + 2) * ow,
                        out + (size_t)(4 * y + 3) * ow);
    }
    free(b);
}
