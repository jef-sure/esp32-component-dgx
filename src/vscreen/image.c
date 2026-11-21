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
#include "dgx_draw.h"
#include "dgx_image.h"
#include "dgx_vscreen.h"
#include "../dgx_clip_priv.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/*
 * Drawing by values or through a map needs a linear 8-bit virtual screen,
 * and recognizing one needs vscreen.c, which is optional in the component
 * build. A picture in one color does not need it.
 */
#if !defined(ESP_PLATFORM) || defined(CONFIG_DGX_ENABLE_VSCREEN)
#define DGX_IMAGE_INDEXED 1
#else
#define DGX_IMAGE_INDEXED 0
#endif

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

/* whether the pixel in column sx of a picture row is nonzero */
static inline bool image_inked(const uint8_t *row, unsigned format, int sx)
{
    switch (format) {
    case DGX_IMAGE_1BPP:
        return ((row[sx >> 3] << (sx & 7)) & 0x80u) != 0;
    case DGX_IMAGE_4BPP:
        return ((sx & 1 ? row[sx >> 1] : row[sx >> 1] >> 4) & 15u) != 0;
    default:
        return row[sx] != 0;
    }
}

/* the turned picture walked along a row of the destination: where its first pixel is in the picture, and the step to the next */
typedef struct {
    const dgx_image_t *image;
    size_t             stride;
    bool               swapped, x_back, y_back;
    int                box_x, box_y;
} image_walk_t;

static inline void image_row_start(const image_walk_t *wk, int left, int y, int *sx, int *sy, int *dx, int *dy)
{
    const int u = left - wk->box_x, v = y - wk->box_y, w = wk->image->width, h = wk->image->height;
    *sx = wk->x_back ? w - 1 - (wk->swapped ? v : u) : (wk->swapped ? v : u);
    *sy = wk->y_back ? h - 1 - (wk->swapped ? u : v) : (wk->swapped ? u : v);
    *dx = wk->swapped ? 0 : wk->x_back ? -1 : 1, *dy = !wk->swapped ? 0 : wk->y_back ? -1 : 1;
}

/*
 * A picture in one color on any screen. Covers rows top .. bottom - 1 and
 * columns left .. right - 1 of the destination. The pixels of a row are
 * gathered into runs of one value, and each run is drawn as a rectangle one
 * pixel high by the screen's own fill, the way glyph rows are drawn: a
 * display behind a bus gets one window per run instead of one per pixel.
 * Zero pixels are left alone, or take bg when `opaque`: every pixel once.
 */
static void image_runs(dgx_screen_t *dst, const image_walk_t *wk, uint32_t color, bool opaque, uint32_t bg, int left, int top, int right,
                       int bottom)
{
    const int      n      = right - left;
    const unsigned format = wk->image->format;
    for (int y = top; y < bottom; ++y) {
        int sx, sy, dx, dy;
        image_row_start(wk, left, y, &sx, &sy, &dx, &dy);
        int  run   = 0; /* where the current run starts */
        bool inked = false;
        for (int i = 0; i < n; ++i, sx += dx, sy += dy) {
            const bool now = image_inked(wk->image->data + (size_t)sy * wk->stride, format, sx);
            if (i && now != inked) {
                if (inked || opaque) dgx_fill_rectangle(dst, left + run, y, i - run, 1, inked ? color : bg);
                run = i;
            }
            inked = now;
        }
        if (inked || opaque) dgx_fill_rectangle(dst, left + run, y, n - run, 1, inked ? color : bg);
    }
}

/*
 * The same picture in one color, opaque, on a display behind a bus: one
 * window over the whole box, every row put into the buffer of the bus as
 * pixels of the screen and sent. False, with nothing drawn, when a row does
 * not fit in the buffer or does not end on a whole byte.
 */
static bool image_window(dgx_screen_t *dst, const image_walk_t *wk, uint32_t color, uint32_t bg, int left, int top, int right, int bottom)
{
    const int n = right - left;
    if (dst->screen_subtype != DgxPhysicalScreenWithBus || !dst->draw_buffer || (uint32_t)n * dst->color_bits % 8u) return false;
    const uint32_t bytes = dgx_color_points_to_bytes(dst->color_bits, (uint32_t)n);
    if (bytes > dst->draw_buffer_len) return false;
    const unsigned format = wk->image->format;
    /* the buffer free first: waiting after the window would send its command alone, ahead of the pixels */
    if (dst->wait) dst->wait(dst);
    dst->set_area(dst, (uint16_t)left, (uint16_t)(right - 1), (uint16_t)top, (uint16_t)(bottom - 1));
    for (int y = top; y < bottom; ++y) {
        int sx, sy, dx, dy;
        image_row_start(wk, left, y, &sx, &sy, &dx, &dy);
        /* the row before may still be on its way from the buffer */
        if (dst->wait) dst->wait(dst);
        uint8_t *lp = dst->draw_buffer;
        for (int i = 0; i < n; ++i, sx += dx, sy += dy)
            lp = dgx_fill_buf_value(dst->color_bits, lp, (uint32_t)i, image_inked(wk->image->data + (size_t)sy * wk->stride, format, sx) ? color : bg);
        dst->write_area(dst, dst->draw_buffer, bytes * 8u);
    }
    dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
    return true;
}

/* a linear 8-bit virtual screen: the only destination for values and maps */
static inline bool image_indexed_screen(const dgx_screen_t *dst)
{
#if DGX_IMAGE_INDEXED
    return dst->color_bits == 8 && dgx_vscreen_is_linear(dst);
#else
    (void)dst;
    return false;
#endif
}

esp_err_t dgx_image_blit(dgx_screen_t *dst, int x, int y, const dgx_image_t *image, const dgx_image_blit_t *how)
{
    static const dgx_image_blit_t as_is = {0};
    if (!how) how = &as_is;
    if (!dst || !image || !image->data) return ESP_ERR_INVALID_ARG;
    const uint8_t format = image->format;
    if (format != DGX_IMAGE_1BPP && format != DGX_IMAGE_4BPP && format != DGX_IMAGE_8BPP) return ESP_ERR_INVALID_ARG;
    if ((unsigned)how->orientation > (unsigned)DgxOutputTransverse) return ESP_ERR_INVALID_ARG;
    /* one color (a 1-bit picture or a silhouette) goes to any screen; values and maps are indices of an 8-bit virtual screen */
    const bool one_color = format == DGX_IMAGE_1BPP || (how->flags & DGX_IMAGE_SILHOUETTE);
    const bool indexed   = image_indexed_screen(dst);
    if (!indexed && !one_color) return ESP_ERR_NOT_SUPPORTED;
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

    /* what of it may change: the clip of the destination, cut by the clip of the call */
    int left = dgx_clip_left(dst), top = dgx_clip_top(dst), right = dgx_clip_right(dst), bottom = dgx_clip_bottom(dst);
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

    const bool opaque = (how->flags & DGX_IMAGE_OPAQUE) != 0;
    if (!indexed) {
        /* the screen draws the runs itself, in one batch; an opaque box on a display behind a bus goes in one window */
        const image_walk_t wk = {image, ((size_t)w * format + 7u) / 8u, swapped, x_back, y_back, box_x, box_y};
        dgx_screen_batch_begin(dst);
        if (!opaque || !image_window(dst, &wk, how->color, how->bg, left, top, right, bottom))
            image_runs(dst, &wk, how->color, opaque, how->bg, left, top, right, bottom);
        dgx_screen_batch_end(dst);
        return ESP_OK;
    }

    /* what a value p that is not 0 becomes: ink[p], or p itself at 8 bits without a map */
    uint8_t        table[256];
    const uint8_t *ink   = table;
    const int      top_p = (1 << format) - 1;
    const uint8_t  index = (uint8_t)how->color; /* of an 8-bit screen */
    if (one_color) {
        memset(table + 1, index, (size_t)top_p);
    } else if (how->map) {
        memcpy(table + 1, how->map, (size_t)top_p);
    } else if (format == DGX_IMAGE_4BPP) {
        for (int p = 1; p <= 15; ++p) table[p] = (uint8_t)p;
    } else {
        ink = NULL;
    }

    dgx_screen_batch_begin(dst);
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
            /* in memory, nothing is seen in between: the row takes the paper, then the pixels that are not 0 */
            if (opaque) memset(row, (int)(uint8_t)how->bg, (size_t)n);
            if (format == DGX_IMAGE_4BPP) {
                if (x_back) image_row4_back(row, src, sx, n, ink);
                else image_row4_fwd(row, src, sx, n, ink);
            } else if (format == DGX_IMAGE_8BPP) {
                if (x_back) image_row8_back(row, src + sx, n, ink);
                else image_row8_fwd(row, src + sx, n, ink);
            } else {
                if (x_back) image_row1_back(row, src, sx, n, index);
                else image_row1_fwd(row, src, sx, n, index);
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
            if (opaque) memset(row, (int)(uint8_t)how->bg, (size_t)n);
            image_column(row, image->data + (size_t)sy0 * stride + (size_t)sx * format / 8u, step, shift, mask, n, ink);
        }
    }
    dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
    dgx_screen_batch_end(dst);
    return ESP_OK;
}
