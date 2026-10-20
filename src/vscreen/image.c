/*
 * image.c
 *
 *  Created on: Oct 15, 2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dgx_bits.h"
#include "dgx_image.h"
#include "drivers/vscreen.h"

/*
 * A row of the destination, n pixels from dst on, taken from a row of the
 * picture from column sx on, rightwards (_fwd) or leftwards (_back). A value
 * p that is not 0 becomes ink[p]: the color, the map or the value itself, put
 * into a table once a call, so that the loops only look it up. Columns are
 * counted by index, never by a pointer that could run before the row.
 *
 * The loops of 4 bits are functions of their own, not inlined: inlined into
 * the blit with the others, they left the compiler too few registers and kept
 * their values on the stack, half again as slow on an ESP32. The other loops
 * are better inlined: on their own the compiler of ESP-IDF 6.0.1 does not
 * give them the hardware loop of the Xtensa, and they are as much slower.
 */
#define IMAGE_LOOP static __attribute__((noinline)) void
static inline void image_row1_fwd(uint8_t *restrict dst, const uint8_t *restrict src, int sx, int n, uint8_t color)
{
    for (int i = 0; i < n; ++i, ++sx)
        if ((src[sx >> 3] << (sx & 7)) & 0x80u) dst[i] = color;
}

static inline void image_row1_back(uint8_t *restrict dst, const uint8_t *restrict src, int sx, int n, uint8_t color)
{
    for (int i = 0; i < n; ++i, --sx)
        if ((src[sx >> 3] << (sx & 7)) & 0x80u) dst[i] = color;
}

/* two pixels a byte: the high half first going right, the low half first going left */
IMAGE_LOOP image_row4_fwd(uint8_t *restrict dst, const uint8_t *restrict src, int sx, int n, const uint8_t *restrict ink)
{
    int i = 0, k = sx >> 1;
    if (sx & 1) {
        const unsigned p = src[k++] & 15u;
        if (p) dst[0] = ink[p];
        i = 1;
    }
    for (; i + 1 < n; i += 2) {
        const unsigned b = src[k++], hi = b >> 4, lo = b & 15u;
        if (hi) dst[i] = ink[hi];
        if (lo) dst[i + 1] = ink[lo];
    }
    if (i < n) {
        const unsigned p = src[k] >> 4;
        if (p) dst[i] = ink[p];
    }
}

/* mirrored: the same columns read rightwards from the leftmost one, put from the right end of the row leftwards */
IMAGE_LOOP image_row4_back(uint8_t *restrict dst, const uint8_t *restrict src, int sx, int n, const uint8_t *restrict ink)
{
    const int first = sx - n + 1;
    int       i = n - 1, k = first >> 1;
    if (first & 1) {
        const unsigned p = src[k++] & 15u;
        if (p) dst[i] = ink[p];
        --i;
    }
    for (; i >= 1; i -= 2) {
        const unsigned b = src[k++], hi = b >> 4, lo = b & 15u;
        if (hi) dst[i] = ink[hi];
        if (lo) dst[i - 1] = ink[lo];
    }
    if (i == 0) {
        const unsigned p = src[k] >> 4;
        if (p) dst[0] = ink[p];
    }
}

/* 8 bits: through the table, or the value itself when there is none */
static inline void image_row8_fwd(uint8_t *restrict dst, const uint8_t *restrict src, int n, const uint8_t *restrict ink)
{
    if (ink) {
        for (int i = 0; i < n; ++i) {
            const uint8_t p = src[i];
            if (p) dst[i] = ink[p];
        }
    } else {
        for (int i = 0; i < n; ++i) {
            const uint8_t p = src[i];
            if (p) dst[i] = p;
        }
    }
}

static inline void image_row8_back(uint8_t *restrict dst, const uint8_t *restrict src, int n, const uint8_t *restrict ink)
{
    if (ink) {
        for (int i = 0; i < n; ++i) {
            const uint8_t p = src[-i];
            if (p) dst[i] = ink[p];
        }
    } else {
        for (int i = 0; i < n; ++i) {
            const uint8_t p = src[-i];
            if (p) dst[i] = p;
        }
    }
}

/*
 * A row of the destination taken down or up a column of the picture: a turn
 * by 90 degrees or a transposition. src is the byte of the column in the
 * first row taken, `shift` and `mask` pick the pixel out of it, `stride`
 * goes to the next row of the picture, or back.
 */
static inline void image_column(uint8_t *restrict dst, const uint8_t *restrict src, ptrdiff_t stride, unsigned shift, unsigned mask, int n,
                         const uint8_t *restrict ink)
{
    for (int i = 0; i < n; ++i, src += stride) {
        const unsigned p = (*src >> shift) & mask;
        if (p) dst[i] = ink ? ink[p] : (uint8_t)p;
    }
}

esp_err_t dgx_image_blit(dgx_screen_t *dst, int x, int y, const dgx_image_t *image, const dgx_image_blit_t *how)
{
    static const dgx_image_blit_t as_is = {0};
    if (!how) how = &as_is;
    if (!dst || !image || !image->data) return ESP_ERR_INVALID_ARG;
    const uint8_t format = image->format;
    if (format != DGX_IMAGE_1BPP && format != DGX_IMAGE_4BPP && format != DGX_IMAGE_8BPP) return ESP_ERR_INVALID_ARG;
    if ((unsigned)how->orientation > (unsigned)DgxOutputTransverse) return ESP_ERR_INVALID_ARG;
    if (dst->color_bits != 8 || !dgx_vscreen_is_linear(dst)) return ESP_ERR_NOT_SUPPORTED;
    const int w = image->width, h = image->height;
    if (!w || !h) return ESP_OK;

    /*
     * The turn as three answers: does a row of the destination run down a
     * column of the picture, and are the columns and the rows of the picture
     * counted from their far end.
     */
    const dgx_output_orientation_t turn = how->orientation;
    const bool swapped = turn == DgxOutputTranspose || turn == DgxOutputRotate90CCW || turn == DgxOutputRotate90CW || turn == DgxOutputTransverse;
    const bool x_back = turn == DgxOutputMirrorX || turn == DgxOutputRotate180 || turn == DgxOutputRotate90CCW || turn == DgxOutputTransverse;
    const bool y_back = turn == DgxOutputMirrorY || turn == DgxOutputRotate180 || turn == DgxOutputRotate90CW || turn == DgxOutputTransverse;

    /* where the origin lies in the turned picture, and so where the turned picture lies on the destination */
    const int ox = x_back ? w - 1 - image->origin_x : image->origin_x, oy = y_back ? h - 1 - image->origin_y : image->origin_y;
    const int box_x = x - (swapped ? oy : ox), box_y = y - (swapped ? ox : oy);
    const int box_w = swapped ? h : w, box_h = swapped ? w : h;

    /* what of it may change: the destination, cut by the clip */
    int left = 0, top = 0, right = dst->width, bottom = dst->height;
    if (how->flags & DGX_IMAGE_CLIP) {
        left   = DGX_MAX(left, how->clip_x);
        top    = DGX_MAX(top, how->clip_y);
        right  = DGX_MIN(right, how->clip_x + how->clip_w);
        bottom = DGX_MIN(bottom, how->clip_y + how->clip_h);
    }
    left   = DGX_MAX(left, box_x);
    top    = DGX_MAX(top, box_y);
    right  = DGX_MIN(right, box_x + box_w);
    bottom = DGX_MIN(bottom, box_y + box_h);
    if (right <= left || bottom <= top) return ESP_OK;

    /* what a value p that is not 0 becomes: ink[p], or p itself at 8 bits without a map */
    uint8_t        table[256];
    const uint8_t *ink   = table;
    const int      top_p = (1 << format) - 1;
    if (format == DGX_IMAGE_1BPP || (how->flags & DGX_IMAGE_SILHOUETTE)) {
        memset(table + 1, how->color, (size_t)top_p);
    } else if (how->map) {
        memcpy(table + 1, how->map, (size_t)top_p);
    } else if (format == DGX_IMAGE_4BPP) {
        for (int p = 1; p <= 15; ++p) table[p] = (uint8_t)p;
    } else {
        ink = NULL;
    }

    dgx_screen_progress_up(dst);
    const size_t   stride = ((size_t)w * format + 7u) / 8u;
    const int      n      = right - left;
    const int      pitch  = dst->width;
    uint8_t       *row    = ((dgx_vscreen_t *)dst)->v_array + (size_t)top * (size_t)pitch + (size_t)left;
    /* the place in the turned picture of the first pixel of the first row drawn */
    const int u = left - box_x, v0 = top - box_y;
    if (!swapped) {
        const int sx = x_back ? w - 1 - u : u;
        for (int v = v0; v < v0 + bottom - top; ++v, row += pitch) {
            const uint8_t *src = image->data + (size_t)(y_back ? h - 1 - v : v) * stride;
            if (format == DGX_IMAGE_4BPP) {
                if (x_back) image_row4_back(row, src, sx, n, ink);
                else image_row4_fwd(row, src, sx, n, ink);
            } else if (format == DGX_IMAGE_8BPP) {
                if (x_back) image_row8_back(row, src + sx, n, ink);
                else image_row8_fwd(row, src + sx, n, ink);
            } else {
                if (x_back) image_row1_back(row, src, sx, n, how->color);
                else image_row1_fwd(row, src, sx, n, how->color);
            }
        }
    } else {
        /* a row of the destination runs along a column of the picture, a column of the destination along a row of it */
        const int       sy0  = y_back ? h - 1 - u : u;
        const ptrdiff_t step = y_back ? -(ptrdiff_t)stride : (ptrdiff_t)stride;
        const unsigned  mask = (unsigned)top_p;
        for (int v = v0; v < v0 + bottom - top; ++v, row += pitch) {
            const int      sx    = x_back ? w - 1 - v : v;
            const unsigned shift = 8u - format - (unsigned)(sx * format % 8);
            image_column(row, image->data + (size_t)sy0 * stride + (size_t)sx * format / 8u, step, shift, mask, n, ink);
        }
    }
    dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
    dgx_screen_progress_down(dst);
    return ESP_OK;
}
