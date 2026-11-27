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
#include "esp_heap_caps.h"
#include "../dgx_clip_priv.h"
#include "../dgx_mix_priv.h"
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

/* the value of a pixel of a row of the picture, 0 for none */
static inline unsigned image_value(const uint8_t *row, unsigned format, int sx)
{
    switch (format) {
    case DGX_IMAGE_1BPP:
        return (row[sx >> 3] >> (7 - (sx & 7))) & 1u;
    case DGX_IMAGE_4BPP:
        return (sx & 1 ? row[sx >> 1] : row[sx >> 1] >> 4) & 15u;
    default:
        return row[sx];
    }
}

/*
 * A picture at a scale is walked by its own pixels: the rows of the destination go in bands that show one row v of
 * the turned picture, at most `scale` rows, and along a band each pixel u of that row is a piece of the destination
 * of at most `scale` pixels, the clip cutting the first and the last. The row of the picture under (u, v), and in it
 * the column sx.
 */
static inline const uint8_t *image_cell(const image_walk_t *wk, int u, int v, int *sx)
{
    const int w = wk->image->width, h = wk->image->height;
    *sx          = wk->x_back ? w - 1 - (wk->swapped ? v : u) : (wk->swapped ? v : u);
    const int sy = wk->y_back ? h - 1 - (wk->swapped ? u : v) : (wk->swapped ? u : v);
    return wk->image->data + (size_t)sy * wk->stride;
}

/* the band of rows from y that shows row v of the turned picture: its v and how many rows it has */
static inline int image_band(const image_walk_t *wk, int scale, int y, int bottom, int *v)
{
    *v = (y - wk->box_y) / scale;
    return DGX_MIN(bottom, wk->box_y + (*v + 1) * scale) - y;
}

/* image_runs() at a scale: a run of pieces of one value is one rectangle as high as its band */
static void image_runs_scaled(dgx_screen_t *dst, const image_walk_t *wk, int scale, uint32_t color, bool opaque, uint32_t bg, int left, int top,
                              int right, int bottom)
{
    const unsigned format = wk->image->format;
    for (int y = top, v, band; y < bottom; y += band) {
        band      = image_band(wk, scale, y, bottom, &v);
        int  run  = left;
        bool ink  = false;
        for (int u = (left - wk->box_x) / scale, x = left; x < right; ++u) {
            const int      end = DGX_MIN(right, wk->box_x + (u + 1) * scale);
            int            sx;
            const uint8_t *row = image_cell(wk, u, v, &sx);
            const bool     now = image_inked(row, format, sx);
            if (x > left && now != ink) {
                if (ink || opaque) dgx_fill_rectangle(dst, run, y, x - run, band, ink ? color : bg);
                run = x;
            }
            ink = now, x = end;
        }
        if (ink || opaque) dgx_fill_rectangle(dst, run, y, right - run, band, ink ? color : bg);
    }
}

/*
 * image_window() at a scale: one window over the box of the scaled picture, a row put into the buffer of the bus
 * once a band and sent as many times as the band has rows; the buffer is not written over in between, so every pixel
 * goes once and the same bytes go again.
 */
static bool image_window_scaled(dgx_screen_t *dst, const image_walk_t *wk, int scale, uint32_t color, uint32_t bg, int left, int top, int right,
                                int bottom)
{
    const int n = right - left;
    if (dst->screen_subtype != DgxPhysicalScreenWithBus || !dst->draw_buffer || (uint32_t)n * dst->color_bits % 8u) return false;
    const uint32_t bytes = dgx_color_points_to_bytes(dst->color_bits, (uint32_t)n);
    if (bytes > dst->draw_buffer_len) return false;
    const unsigned format = wk->image->format;
    if (dst->wait) dst->wait(dst);
    dst->set_area(dst, (uint16_t)left, (uint16_t)(right - 1), (uint16_t)top, (uint16_t)(bottom - 1));
    for (int y = top, v, band; y < bottom; y += band) {
        band = image_band(wk, scale, y, bottom, &v);
        /* the rows before may still be on their way from the buffer */
        if (dst->wait) dst->wait(dst);
        uint8_t *lp = dst->draw_buffer;
        int      i  = 0;
        for (int u = (left - wk->box_x) / scale, x = left; x < right; ++u) {
            const int      end = DGX_MIN(right, wk->box_x + (u + 1) * scale);
            int            sx;
            const uint8_t *row = image_cell(wk, u, v, &sx);
            const uint32_t c   = image_inked(row, format, sx) ? color : bg;
            for (; x < end; ++x, ++i) lp = dgx_fill_buf_value(dst->color_bits, lp, (uint32_t)i, c);
        }
        for (int k = 0; k < band; ++k) dst->write_area(dst, dst->draw_buffer, bytes * 8u);
    }
    dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
    return true;
}

/*
 * ---- A picture by its colors: 16 bits, or values through `colors`, or the coverage of one color, soft or not ----
 *
 * Every pixel of the destination is looked up in the picture: its color, in the format of the screen, and its
 * opacity. The pixels a row writes go into a row of memory and out by runs, each through a window of the screen, as
 * the texture quad sends its rows: a display behind a bus gets one window a run, a virtual screen and a DSI display
 * take the same calls.
 */
typedef struct {
    const dgx_image_t *image;
    unsigned           format;
    uint32_t           color;    /* the coverage of one color: what covers */
    bool               coverage; /* a value of 4 or 8 bits is how much of color covers the pixel */
    const uint32_t    *colors;   /* the color of each value */
    const uint8_t     *alpha;    /* the opacity of each value; NULL for 0 transparent, the rest opaque */
    const dgx_screen_t *dst;
} image_colors_t;

/* the color, in the format of the screen, and the opacity of the pixel at column sx of row sy of the picture */
static inline uint32_t image_color(const image_colors_t *c, const uint8_t *row, int sx, int sy, unsigned *a)
{
    if (c->format == DGX_IMAGE_16BPP) {
        const uint32_t v = (uint32_t)row[2 * sx] << 8 | row[2 * sx + 1];
        *a               = c->image->alpha ? c->image->alpha[(size_t)sy * c->image->width + (size_t)sx] : 255u;
        if (c->dst->color_bits == 16) return v;
        return dgx_screen_rgb(c->dst, (uint8_t)((v >> 8) & 0xf8u), (uint8_t)((v >> 3) & 0xfcu), (uint8_t)((v << 3) & 0xf8u));
    }
    const unsigned p = image_value(row, c->format, sx);
    if (c->coverage) {
        *a = c->format == DGX_IMAGE_4BPP ? p * 17u : p;
        return c->color;
    }
    *a = c->alpha ? c->alpha[p] : p ? 255u : 0u;
    return c->colors[p];
}

/* how the pixels of a picture by its colors are put: what partly covered ones are mixed with */
typedef struct {
    bool     opaque; /* DGX_IMAGE_OPAQUE: every pixel of the box, the uncovered ones bg */
    bool     mix;    /* DGX_IMAGE_SOFT on a screen whose colors are mixed */
    bool     paper;  /* DGX_IMAGE_BG: the edges mixed with bg, the screen not read */
    bool     under;  /* the edges mixed with what lies on the screen */
    uint32_t bg;
    uint8_t  bits;
} image_put_t;

/* the pixel x, y of the destination: false when it is left as it is, else its value in *v */
static inline bool image_pixel(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int x, int y,
                               uint32_t *v)
{
    const int      u = (x - wk->box_x) / scale, w = (y - wk->box_y) / scale;
    const int      W = wk->image->width, H = wk->image->height;
    const int      sx = wk->x_back ? W - 1 - (wk->swapped ? w : u) : (wk->swapped ? w : u);
    const int      sy = wk->y_back ? H - 1 - (wk->swapped ? u : w) : (wk->swapped ? u : w);
    unsigned       a;
    const uint32_t color = image_color(c, wk->image->data + (size_t)sy * wk->stride, sx, sy, &a);
    if (a == 255u) return *v = color, true;
    if (k->opaque) return *v = a == 0u ? k->bg : k->mix ? dgx_mix_color(k->bits, k->bg, color, a) : a >= 128u ? color : k->bg, true;
    if (!a) return false;
    if (k->paper) return *v = dgx_mix_color(k->bits, k->bg, color, a), true;
    if (k->under) return *v = dgx_mix_color(k->bits, dgx_get_pixel(dst, x, y), color, a), true;
    if (a >= 128u) return *v = color, true;
    return false;
}

static esp_err_t image_by_colors(dgx_screen_t *dst, const image_walk_t *wk, int scale, const dgx_image_blit_t *how, const image_colors_t *c,
                                 int left, int top, int right, int bottom)
{
    const uint8_t bits = dst->color_bits;
    image_put_t   k    = {.opaque = (how->flags & DGX_IMAGE_OPAQUE) != 0,
                          .mix    = (how->flags & DGX_IMAGE_SOFT) && dgx_mix_bits(bits),
                          .bg     = how->bg,
                          .bits   = bits};
    k.paper            = k.mix && !k.opaque && (how->flags & DGX_IMAGE_BG);
    k.under            = k.mix && !k.opaque && !k.paper && dgx_screen_is_readable(dst);
    const int      n     = right - left;
    const uint32_t bytes = dgx_color_points_to_bytes(bits, (uint32_t)n);
    uint32_t       v;
    dgx_screen_batch_begin(dst);
#if DGX_IMAGE_INDEXED
    if (dgx_vscreen_is_linear(dst)) {
        /* a virtual screen in memory: every pixel written in its place, no row of memory needed */
        uint8_t *const array  = ((dgx_vscreen_t *)dst)->v_array;
        const unsigned stored = bits == 18 ? 24u : bits;
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
                if (image_pixel(dst, wk, scale, c, &k, x, y, &v)) {
                    const uint32_t i = (uint32_t)y * dst->width + (uint32_t)x;
                    dgx_fill_buf_value(bits, array + (size_t)i * stored / 8u, (int)i, v);
                }
        dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
        dgx_screen_batch_end(dst);
        return ESP_OK;
    }
#endif
    if (k.opaque && dst->screen_subtype == DgxPhysicalScreenWithBus && dst->draw_buffer && (uint32_t)n * bits % 8u == 0 && bytes <= dst->draw_buffer_len) {
        /* every pixel of the box, on a display behind a bus: one window, a row at a time through the buffer of the bus; at a
           scale the rows of a band are one row of the picture, made once and sent as many times */
        if (dst->wait) dst->wait(dst);
        dst->set_area(dst, (uint16_t)left, (uint16_t)(right - 1), (uint16_t)top, (uint16_t)(bottom - 1));
        int made = -1; /* the row of the picture the buffer holds */
        for (int y = top; y < bottom; ++y) {
            const int row = (y - wk->box_y) / scale;
            if (row != made) {
                if (dst->wait) dst->wait(dst); /* the rows before may still be on their way from the buffer */
                uint8_t *lp = dst->draw_buffer;
                for (int x = left; x < right; ++x) {
                    image_pixel(dst, wk, scale, c, &k, x, y, &v);
                    lp = dgx_fill_buf_value(bits, lp, x - left, v);
                }
                made = row;
            }
            dst->write_area(dst, dst->draw_buffer, bytes * 8u);
        }
        dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
        dgx_screen_batch_end(dst);
        return ESP_OK;
    }
    const bool window = dst->set_area && dst->write_area;
    /* a display behind a bus has its buffer; another screen with windows a row of its own; one without, a pixel at a time */
    const bool shared = window && dst->draw_buffer && dst->draw_buffer_len >= bytes;
    uint8_t   *own    = NULL;
    if (window && !shared) {
        own = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
        if (!own) {
            dgx_screen_batch_end(dst);
            return ESP_ERR_NO_MEM;
        }
    }
    uint8_t *const buffer = shared ? dst->draw_buffer : own;
    for (int y = top; y < bottom; ++y) {
        int run = -1; /* where the run being made starts, -1 for none */
        if (shared && dst->wait) dst->wait(dst); /* the row before may still be on its way from the buffer */
        uint8_t *lp = buffer;
        for (int x = left; x <= right; ++x) {
            const bool put = x < right && image_pixel(dst, wk, scale, c, &k, x, y, &v);
            if (put) {
                if (!window) {
                    dgx_set_pixel(dst, x, y, v);
                    continue;
                }
                if (run < 0) run = x, lp = buffer;
                lp = dgx_fill_buf_value(bits, lp, x - run, v);
            } else if (run >= 0) {
                /* the run ends: out through a window of its own */
                dst->set_area(dst, (uint16_t)run, (uint16_t)(x - 1), (uint16_t)y, (uint16_t)y);
                dst->write_area(dst, buffer, dgx_color_points_to_bytes(bits, (uint32_t)(x - run)) * 8u);
                if (shared && dst->wait && x < right) dst->wait(dst);
                run = -1;
            }
        }
    }
    if (window) dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
    dgx_screen_batch_end(dst);
    if (own) heap_caps_free(own);
    return ESP_OK;
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
    if (format != DGX_IMAGE_1BPP && format != DGX_IMAGE_4BPP && format != DGX_IMAGE_8BPP && format != DGX_IMAGE_16BPP) return ESP_ERR_INVALID_ARG;
    if ((unsigned)how->orientation > (unsigned)DgxOutputTransverse) return ESP_ERR_INVALID_ARG;
    /* one color (a 1-bit picture or a silhouette) goes to any screen; values and maps are indices of an 8-bit virtual screen */
    const bool one_color = format == DGX_IMAGE_1BPP || (how->flags & DGX_IMAGE_SILHOUETTE);
    /* by colors, to any screen: 16 bits, values through `colors`, or the coverage of one color with soft edges */
    const bool by_colors = format == DGX_IMAGE_16BPP || how->colors ||
                           ((how->flags & DGX_IMAGE_SOFT) && format != DGX_IMAGE_1BPP && (how->flags & DGX_IMAGE_SILHOUETTE));
    const bool indexed   = image_indexed_screen(dst);
    if (!by_colors && !indexed && !one_color) return ESP_ERR_NOT_SUPPORTED;
    const int w = image->width, h = image->height;
    if (!w || !h) return ESP_OK;
    const int scale = how->scale > 1 ? how->scale : 1;

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
    const int box_x = x - (swapped ? oy : ox) * scale, box_y = y - (swapped ? ox : oy) * scale;
    const int box_w = (swapped ? h : w) * scale, box_h = (swapped ? w : h) * scale;

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

    const bool         opaque = (how->flags & DGX_IMAGE_OPAQUE) != 0;
    const image_walk_t wk     = {image, ((size_t)w * format + 7u) / 8u, swapped, x_back, y_back, box_x, box_y};
    if (by_colors) {
        const image_colors_t c = {
            .image    = image,
            .format   = format,
            .color    = how->color,
            .coverage = format != DGX_IMAGE_16BPP && !how->colors,
            .colors   = how->colors,
            .alpha    = how->alpha,
            .dst      = dst,
        };
        return image_by_colors(dst, &wk, scale, how, &c, left, top, right, bottom);
    }
    if (!indexed) {
        /* the screen draws the runs itself, in one batch; an opaque box on a display behind a bus goes in one window */
        dgx_screen_batch_begin(dst);
        if (scale > 1) {
            if (!opaque || !image_window_scaled(dst, &wk, scale, how->color, how->bg, left, top, right, bottom))
                image_runs_scaled(dst, &wk, scale, how->color, opaque, how->bg, left, top, right, bottom);
        } else if (!opaque || !image_window(dst, &wk, how->color, how->bg, left, top, right, bottom)) {
            image_runs(dst, &wk, how->color, opaque, how->bg, left, top, right, bottom);
        }
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
    if (scale > 1) {
        /* at a scale, by the pixels of the picture: every row of a band takes the same pieces */
        uint8_t *const array = ((dgx_vscreen_t *)dst)->v_array;
        for (int y = top, v, band; y < bottom; y += band) {
            band = image_band(&wk, scale, y, bottom, &v);
            for (int k = 0; k < band; ++k) {
                uint8_t *const line = array + (size_t)(y + k) * (size_t)pitch;
                for (int u = (left - box_x) / scale, x = left; x < right; ++u) {
                    const int      end = DGX_MIN(right, box_x + (u + 1) * scale);
                    int            sx;
                    const uint8_t *src = image_cell(&wk, u, v, &sx);
                    const unsigned p   = image_value(src, format, sx);
                    if (p) memset(line + x, ink ? ink[p] : (int)p, (size_t)(end - x));
                    else if (opaque) memset(line + x, (int)(uint8_t)how->bg, (size_t)(end - x));
                    x = end;
                }
            }
        }
        dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
        dgx_screen_batch_end(dst);
        return ESP_OK;
    }
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
