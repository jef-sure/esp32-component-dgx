/*
 * indexed.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <string.h>

#include "dgx_indexed_priv.h"

/* half of a point, rounded down also left of 0: the point of the smaller picture a point of the larger one lies in */
static inline int32_t dgx_indexed_half(int32_t v)
{
    return v >= 0 ? v / 2 : -((1 - v) / 2);
}

static inline int32_t dgx_indexed_clamp(int32_t v, int32_t size)
{
    return v < 0 ? 0 : v >= size ? size - 1 : v;
}

/*
 * Scale2x of a line: the points of `mid`, with the lines `up` and `down` on
 * either side of it, made into the half `j` (0 the one towards `up`) of the
 * line twice as long, its points `from` to `from + count - 1`. A smaller
 * line holds its point p at [p - base] and has the points on either side of
 * those used.
 *
 * A point E with B towards `up`, H towards `down`, D before it and F after
 * it gives two halves of two points each. Where B and H differ and D and F
 * differ, a point whose two neighbours are equal takes their index: D == B
 * for the first point of half 0, B == F for its second, D == H and H == F
 * for half 1. Everywhere else it stays E. The rule is the same whichever way
 * the picture is turned or mirrored, so a column of it is made the same way
 * as a row.
 */
static void dgx_indexed_scale2x(uint8_t *out, const uint8_t *up, const uint8_t *mid, const uint8_t *down, int32_t base, int32_t from, uint32_t count,
                                unsigned j)
{
    if (!count) return;
    const uint8_t *side = j ? down : up;
    /* the smaller point the first one lies in, and its neighbours before and after it along the line */
    uint32_t k = (uint32_t)(dgx_indexed_half(from) - base);
    uint8_t  d = mid[k - 1], e = mid[k], f = mid[k + 1];
    uint32_t left = count;
    if (from & 1) {
        /* the line starts with the second point of a pair */
        const uint8_t b = up[k], h = down[k], s = side[k];
        *out++          = b != h && d != f && s == f ? f : e;
        --left;
        ++k;
        d = e, e = f, f = mid[k + 1];
    }
    for (; left >= 2; left -= 2, ++k, d = e, e = f, f = mid[k + 1]) {
        const uint8_t b = up[k], h = down[k];
        if (b == h || d == f) {
            /* no edge here: most of a picture */
            out[0] = out[1] = e;
        } else {
            const uint8_t s = side[k];
            out[0]          = d == s ? d : e;
            out[1]          = s == f ? f : e;
        }
        out += 2;
    }
    if (left) {
        /* the line ends with the first point of a pair */
        const uint8_t b = up[k], h = down[k], s = side[k];
        *out            = b != h && d != f && d == s ? d : e;
    }
}

/*
 * The same as dgx_indexed_scale2x() for both halves at once, straight into
 * colors: half 0 into `top`, half 1 into `bottom`, a point `step` pixels
 * from the one before it (-1 for a line made backwards). The two halves
 * share every index they look at.
 */
static void dgx_indexed_scale2x_colors(uint16_t *top, uint16_t *bottom, ptrdiff_t step, const uint8_t *up, const uint8_t *mid, const uint8_t *down,
                                       int32_t base, int32_t from, uint32_t count, const uint16_t *lut)
{
    if (!count) return;
    uint32_t k = (uint32_t)(dgx_indexed_half(from) - base);
    uint8_t  d = mid[k - 1], e = mid[k], f = mid[k + 1];
    uint32_t left = count;
    if (from & 1) {
        /* the line starts with the second point of a pair */
        const uint8_t b = up[k], h = down[k];
        const bool    edge = b != h && d != f;
        *top               = lut[edge && b == f ? f : e];
        *bottom            = lut[edge && h == f ? f : e];
        top += step, bottom += step;
        --left;
        ++k;
        d = e, e = f, f = mid[k + 1];
    }
    for (; left >= 2; left -= 2, ++k, d = e, e = f, f = mid[k + 1]) {
        const uint8_t b = up[k], h = down[k];
        if (b == h || d == f) {
            /* no edge here: most of a picture */
            const uint16_t color = lut[e];
            top[0] = top[step] = bottom[0] = bottom[step] = color;
        } else {
            top[0]       = lut[d == b ? d : e];
            top[step]    = lut[b == f ? f : e];
            bottom[0]    = lut[d == h ? d : e];
            bottom[step] = lut[h == f ? f : e];
        }
        top += 2 * step, bottom += 2 * step;
    }
    if (left) {
        /* the line ends with the first point of a pair */
        const uint8_t b = up[k], h = down[k];
        const bool    edge = b != h && d != f;
        *top               = lut[edge && d == b ? d : e];
        *bottom            = lut[edge && d == h ? d : e];
    }
}

void dgx_indexed_lines_begin(dgx_indexed_lines_t *lines, const dgx_bus_indexed_t *picture, bool along_y, int32_t from, uint32_t count,
                             uint8_t *scratch)
{
    lines->picture = picture;
    lines->along_y = along_y;
    lines->from    = from;
    lines->count   = count;
    /*
     * The points of the smaller pictures the line needs, with one more on
     * either side for their neighbours, and one more at the end that
     * dgx_indexed_scale2x() looks at after its last point.
     */
    const int32_t last = from + (int32_t)count - 1;
    if (picture->scale == 4) {
        lines->base2 = dgx_indexed_half(from) - 1;
        lines->len2  = (uint32_t)(dgx_indexed_half(last) + 1 - lines->base2 + 2);
        lines->base1 = dgx_indexed_half(lines->base2) - 1;
        lines->len1  = (uint32_t)(dgx_indexed_half(lines->base2 + (int32_t)lines->len2 - 1) + 1 - lines->base1 + 2);
    } else {
        lines->base2 = 0;
        lines->len2  = 0;
        lines->base1 = dgx_indexed_half(from) - 1;
        lines->len1  = (uint32_t)(dgx_indexed_half(last) + 1 - lines->base1 + 2);
    }
    /* a row of colors nobody wants, for the half of a pair that is not asked for; first, where scratch is aligned */
    lines->spare = (uint16_t *)scratch;
    scratch += 2u * (size_t)count;
    for (int i = 0; i < 4; ++i) {
        lines->tag1[i]  = INT32_MIN;
        lines->tag2[i]  = INT32_MIN;
        lines->line1[i] = scratch + (size_t)i * lines->len1;
        lines->line2[i] = scratch + 4u * lines->len1 + (size_t)i * lines->len2;
    }
}

/* the line t of the picture itself; past its edges its last pixel goes on */
static const uint8_t *dgx_indexed_line1(dgx_indexed_lines_t *lines, int32_t t)
{
    uint8_t *line = lines->line1[t & 3];
    if (lines->tag1[t & 3] == t) return line;
    lines->tag1[t & 3]               = t;
    const dgx_bus_indexed_t *picture = lines->picture;
    const int32_t            run     = lines->along_y ? picture->height : picture->width;
    const int32_t            across  = lines->along_y ? picture->width : picture->height;
    const uint32_t           step    = lines->along_y ? picture->pitch : 1u;
    const uint8_t           *first   = picture->pixels + (size_t)dgx_indexed_clamp(t, across) * (lines->along_y ? 1u : picture->pitch);
    for (uint32_t i = 0; i < lines->len1; ++i) line[i] = first[(size_t)dgx_indexed_clamp(lines->base1 + (int32_t)i, run) * step];
    return line;
}

/* the line t of the picture twice as large */
static const uint8_t *dgx_indexed_line2(dgx_indexed_lines_t *lines, int32_t t)
{
    uint8_t *line = lines->line2[t & 3];
    if (lines->tag2[t & 3] == t) return line;
    lines->tag2[t & 3] = t;
    const int32_t  half = dgx_indexed_half(t);
    const uint8_t *up = dgx_indexed_line1(lines, half - 1), *mid = dgx_indexed_line1(lines, half), *down = dgx_indexed_line1(lines, half + 1);
    dgx_indexed_scale2x(line, up, mid, down, lines->base1, lines->base2, lines->len2, (unsigned)(t & 1));
    return line;
}

void dgx_indexed_line(dgx_indexed_lines_t *lines, int32_t at, uint8_t *out)
{
    const dgx_bus_indexed_t *picture = lines->picture;
    const int32_t            scale   = picture->scale;
    if (picture->smooth && scale == 2) {
        const int32_t  half = at / 2;
        const uint8_t *up = dgx_indexed_line1(lines, half - 1), *mid = dgx_indexed_line1(lines, half), *down = dgx_indexed_line1(lines, half + 1);
        dgx_indexed_scale2x(out, up, mid, down, lines->base1, lines->from, lines->count, (unsigned)(at & 1));
        return;
    }
    if (picture->smooth && scale == 4) {
        const int32_t  half = at / 2;
        const uint8_t *up = dgx_indexed_line2(lines, half - 1), *mid = dgx_indexed_line2(lines, half), *down = dgx_indexed_line2(lines, half + 1);
        dgx_indexed_scale2x(out, up, mid, down, lines->base2, lines->from, lines->count, (unsigned)(at & 1));
        return;
    }
    /* every index a square: the line of the picture it lies in, each point repeated */
    const uint32_t step  = lines->along_y ? picture->pitch : 1u;
    const uint8_t *first = picture->pixels + (size_t)(at / scale) * (lines->along_y ? 1u : picture->pitch);
    for (uint32_t i = 0; i < lines->count; ++i) out[i] = first[(size_t)((lines->from + (int32_t)i) / scale) * step];
}

void dgx_indexed_colors(dgx_indexed_lines_t *lines, int32_t at, uint16_t *row, uint16_t *pair, ptrdiff_t step)
{
    const dgx_bus_indexed_t *picture = lines->picture;
    const int32_t            half    = at / 2;
    uint16_t                *halves[2];
    halves[at & 1]       = row;
    halves[(at & 1) ^ 1] = pair ? pair : lines->spare + (step < 0 ? lines->count - 1u : 0u);
    const uint8_t *up, *mid, *down;
    int32_t        base;
    if (picture->scale == 4) {
        up = dgx_indexed_line2(lines, half - 1), mid = dgx_indexed_line2(lines, half), down = dgx_indexed_line2(lines, half + 1);
        base = lines->base2;
    } else {
        up = dgx_indexed_line1(lines, half - 1), mid = dgx_indexed_line1(lines, half), down = dgx_indexed_line1(lines, half + 1);
        base = lines->base1;
    }
    dgx_indexed_scale2x_colors(halves[0], halves[1], step, up, mid, down, base, lines->from, lines->count, picture->lut);
}
