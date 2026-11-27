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
#include "dgx_halves_priv.h"
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
 * A row of the destination is walked along a row of the turned picture. Its pixels are read a piece at a time into
 * their colors, in the format of the screen, and their opacities, by a loop for each format that steps from pixel to
 * pixel of the picture; each is then put on as many pixels of the row as the scale makes it, with no division a
 * pixel. A virtual screen takes them into its memory. With DGX_IMAGE_OPAQUE a display behind a bus takes the box in
 * one window, through the halves of its buffer: a row is made while the one before is on the wire. Elsewhere a row
 * goes by runs, each through a window of the screen, as the texture quad sends its rows.
 */
typedef struct {
    const dgx_image_t  *image;
    unsigned            format;
    uint32_t            color;    /* the coverage of one color: what covers */
    bool                coverage; /* a value of 4 or 8 bits is how much of color covers the pixel */
    const uint32_t     *colors;   /* the color of each value */
    const uint8_t      *alpha;    /* the opacity of each value; NULL for 0 transparent, the rest opaque */
    const dgx_screen_t *dst;
} image_colors_t;

/* how the pixels of a picture by its colors are put: what partly covered ones are mixed with */
typedef struct {
    bool     opaque; /* DGX_IMAGE_OPAQUE: every pixel of the box, the uncovered ones bg */
    bool     mix;    /* DGX_IMAGE_SOFT on a screen whose colors are mixed */
    bool     paper;  /* DGX_IMAGE_BG: the edges mixed with bg, the screen not read */
    bool     under;  /* the edges mixed with what lies on the screen */
    uint32_t bg;
    uint8_t  bits;
    /* every pixel of the box written on a screen of 16 bits: a row is made by image_row16() */
    bool     fast;
    /*
     * A picture of 1 or 4 bits, a sprite above all, whose values put the same wherever they lie: what lies under a
     * pixel is not taken, or no value is partly opaque. What each value puts, and whether it is put, a bit a value,
     * found once for the picture; its rows are made by image_row_table().
     */
    bool     table;
    uint32_t value[16];
    uint16_t puts;
} image_put_t;

/* pixels of the picture read at a time: their colors and opacities lie on the stack */
#define IMAGE_PIECE 32

/* the colors and opacities of n pixels of row v of the turned picture, from its pixel u on */
static void image_read(const image_colors_t *c, const image_walk_t *wk, int u, int v, int n, uint32_t *col, uint8_t *al)
{
    const dgx_image_t *im = c->image;
    const int          W = im->width, H = im->height;
    int                sx = wk->x_back ? W - 1 - (wk->swapped ? v : u) : (wk->swapped ? v : u);
    const int          sy = wk->y_back ? H - 1 - (wk->swapped ? u : v) : (wk->swapped ? u : v);
    const uint8_t     *row = im->data + (size_t)sy * wk->stride;
    /* along u the picture is walked along its row, or down or up its column when turned */
    const int       dx   = wk->swapped ? 0 : wk->x_back ? -1 : 1;
    const ptrdiff_t drow = !wk->swapped ? 0 : wk->y_back ? -(ptrdiff_t)wk->stride : (ptrdiff_t)wk->stride;
    switch (c->format) {
    case DGX_IMAGE_16BPP: {
        const uint8_t  *p  = row + 2 * sx;
        const ptrdiff_t dp = wk->swapped ? drow : 2 * dx;
        if (c->dst->color_bits == 16)
            for (int i = 0; i < n; ++i, p += dp) col[i] = (uint32_t)p[0] << 8 | p[1];
        else
            for (int i = 0; i < n; ++i, p += dp) {
                /* each channel widened to 8 bits by its high bits again, as DGX widens 16 bits elsewhere: white stays white */
                const unsigned r = p[0] >> 3, g = (unsigned)(p[0] & 7u) << 3 | p[1] >> 5, b = p[1] & 31u;
                col[i] = dgx_screen_rgb(c->dst, (uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 2 | g >> 4), (uint8_t)(b << 3 | b >> 2));
            }
        if (im->alpha) {
            const uint8_t  *a  = im->alpha + (size_t)sy * (size_t)W + (size_t)sx;
            const ptrdiff_t da = !wk->swapped ? dx : wk->y_back ? -(ptrdiff_t)W : (ptrdiff_t)W;
            for (int i = 0; i < n; ++i, a += da) al[i] = *a;
        } else {
            memset(al, 255, (size_t)n);
        }
        return;
    }
    case DGX_IMAGE_8BPP: {
        const uint8_t  *p  = row + sx;
        const ptrdiff_t dp = wk->swapped ? drow : dx;
        if (c->coverage)
            for (int i = 0; i < n; ++i, p += dp) col[i] = c->color, al[i] = *p;
        else
            for (int i = 0; i < n; ++i, p += dp) col[i] = c->colors[*p], al[i] = c->alpha ? c->alpha[*p] : *p ? 255u : 0u;
        return;
    }
    default: /* pixels that share bytes: the column moves along the row, or the row along the column */
        for (int i = 0; i < n; ++i, sx += dx, row += drow) {
            const unsigned q = image_value(row, c->format, sx);
            if (c->coverage) col[i] = c->color, al[i] = (uint8_t)(c->format == DGX_IMAGE_4BPP ? q * 17u : q * 255u);
            else col[i] = c->colors[q], al[i] = c->alpha ? c->alpha[q] : q ? 255u : 0u;
        }
        return;
    }
}

/* what a pixel of the picture puts on the pixel x, y of the destination: false when it is left as it is */
static inline bool image_put(dgx_screen_t *dst, const image_put_t *k, uint32_t color, unsigned a, int x, int y, uint32_t *v)
{
    if (a == 255u) return *v = color, true;
    if (k->opaque) return *v = a == 0u ? k->bg : k->mix ? dgx_mix_color(k->bits, k->bg, color, a) : a >= 128u ? color : k->bg, true;
    if (!a) return false;
    if (k->paper) return *v = dgx_mix_color(k->bits, k->bg, color, a), true;
    if (k->under) return *v = dgx_mix_color(k->bits, dgx_get_pixel(dst, x, y), color, a), true;
    if (a >= 128u) return *v = color, true;
    return false;
}

/* a value into a row of pixels of `bits` from `base`, the i-th of it: 16 bits in place, the others by a call */
static void image_store_other(uint8_t bits, uint8_t *base, uint32_t i, uint32_t v)
{
    dgx_fill_buf_value(bits, base + (size_t)i * (bits == 18 ? 24u : bits) / 8u, (int)i, v);
}

static inline void image_store(uint8_t bits, uint8_t *base, uint32_t i, uint32_t v)
{
    if (bits != 16) {
        image_store_other(bits, base, i, v);
        return;
    }
    uint8_t *p = base + 2u * i;
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

/* where the pixels of a row go */
enum { IMAGE_TO_ROW, IMAGE_TO_MEMORY, IMAGE_TO_RUNS };

typedef struct {
    dgx_screen_t *dst;
    uint8_t      *base;   /* the row in a buffer; the memory of a virtual screen; where the run being made goes */
    uint32_t      first;  /* the index the pixel x = 0 of the row has from base */
    bool          window; /* the runs go through windows, else a pixel at a time */
    bool          shared; /* the buffer of the runs may still be on the wire: it is waited for before it is filled again */
    int           run;    /* where the run being made starts, -1 for none */
    int           right;  /* where the row ends */
    /*
     * A buffer of the bus of two rows and more: the runs go one after another into a half of it and on into the other
     * when the half has no room for the longest run that may come, so a run is made while the one before is on the
     * wire. A run is sent only once the runs before it have gone, and those of the other half are all before the
     * last one: the half filled again has gone.
     */
    bool     ring;
    uint8_t *halves;
    uint32_t half, at; /* bytes of a half; where in the half being filled the next run goes */
    int      which;
} image_sink_t;

/* a run of a row out through a window of its own, as many bits as its pixels take */
static void image_run_out(image_sink_t *s, uint8_t bits, int x, int y, bool more)
{
    /* the bits the pixels take in the row: a pixel of 18 bits three bytes, and those of 12 bits with the rest of their
       last byte, as a display behind a bus takes whole bytes; but pixels of 1 or 4 bits as many bits as they are, as a
       screen of pixels that share bytes would take the rest of the last byte as pixels from the start of the window again */
    const uint32_t n = (uint32_t)(x - s->run);
    s->dst->set_area(s->dst, (uint16_t)s->run, (uint16_t)(x - 1), (uint16_t)y, (uint16_t)y);
    s->dst->write_area(s->dst, s->base, bits < 8 ? n * bits : dgx_color_points_to_bytes(bits, n) * 8u);
    if (s->ring) s->at += dgx_color_points_to_bytes(bits, n);
    else if (s->shared && more && s->dst->wait) s->dst->wait(s->dst);
    s->run = -1;
}

/* a run starts at x: where it goes */
static inline void image_run_start(image_sink_t *s, uint8_t bits, int x)
{
    s->run = x;
    if (!s->ring) return;
    if (s->at + dgx_color_points_to_bytes(bits, (uint32_t)(s->right - x)) > s->half) s->which ^= 1, s->at = 0;
    s->base = s->halves + (size_t)s->which * s->half + s->at;
}

/* the pixel x, y: put to the sink, or left as it is */
static inline __attribute__((always_inline)) void image_sink(image_sink_t *s, int to, uint8_t bits, int x, int y, bool put, uint32_t v)
{
    if (to == IMAGE_TO_ROW) {
        image_store(bits, s->base, (uint32_t)x - s->first, v);
    } else if (to == IMAGE_TO_MEMORY) {
        if (put) image_store(bits, s->base, s->first + (uint32_t)x, v);
    } else if (put) {
        if (!s->window) {
            dgx_set_pixel(s->dst, x, y, v);
            return;
        }
        if (s->run < 0) image_run_start(s, bits, x);
        image_store(bits, s->base, (uint32_t)(x - s->run), v);
    } else if (s->run >= 0) {
        image_run_out(s, bits, x, y, true);
    }
}

/* the pixels left .. right - 1 of the row y of the destination, to the sink */
static inline __attribute__((always_inline)) void image_row(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c,
                                                            const image_put_t *k, int left, int right, int y, image_sink_t *s, int to)
{
    const int v    = (y - wk->box_y) / scale;
    const int last = (right - 1 - wk->box_x) / scale; /* the last pixel u of the picture the row shows */
    uint32_t  col[IMAGE_PIECE];
    uint8_t   al[IMAGE_PIECE];
    int       x = left;
    for (int u = (left - wk->box_x) / scale; u <= last;) {
        const int n = DGX_MIN(IMAGE_PIECE, last - u + 1);
        image_read(c, wk, u, v, n, col, al);
        for (int i = 0; i < n; ++i, ++u) {
            const int end   = scale == 1 ? x + 1 : DGX_MIN(right, wk->box_x + (u + 1) * scale);
            uint32_t  value = 0;
            bool      put   = false;
            /* the pixels of a piece take one value, unless each is mixed with what lies under it */
            for (const int from = x; x < end; ++x) {
                if (x == from || k->under) put = image_put(dst, k, col[i], al[i], x, y, &value);
                image_sink(s, to, k->bits, x, y, put, value);
            }
        }
    }
    if (to == IMAGE_TO_RUNS && s->run >= 0) image_run_out(s, k->bits, right, y, false);
}

/* image_row() made once for each place the pixels go */
static __attribute__((noinline)) void image_row_to_row(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left,
                             int right, int y, image_sink_t *s)
{
    image_row(dst, wk, scale, c, k, left, right, y, s, IMAGE_TO_ROW);
}

#if DGX_IMAGE_INDEXED
static void image_row_to_memory(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left,
                                int right, int y, image_sink_t *s)
{
    image_row(dst, wk, scale, c, k, left, right, y, s, IMAGE_TO_MEMORY);
}
#endif

static void image_row_to_runs(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left,
                              int right, int y, image_sink_t *s)
{
    image_row(dst, wk, scale, c, k, left, right, y, s, IMAGE_TO_RUNS);
}

/* a pixel to the runs, out of line: the runs of a sprite go through the window of the screen, which costs more */
static __attribute__((noinline)) void image_run_pixel(image_sink_t *s, uint8_t bits, int x, int y, bool put, uint32_t v)
{
    image_sink(s, IMAGE_TO_RUNS, bits, x, y, put, v);
}

/* what a value puts, out of line, for the table of a picture */
static __attribute__((noinline)) bool image_put_value(dgx_screen_t *dst, const image_put_t *k, uint32_t color, unsigned a, uint32_t *v)
{
    return image_put(dst, k, color, a, 0, 0, v);
}

/*
 * The pixels left .. right - 1 of the row y of a picture of 1 or 4 bits by the values of its table, to the memory of a
 * virtual screen or by runs: one function for both, which costs a test a pixel and spares a copy of the loop.
 */
static __attribute__((noinline)) void image_row_table(const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left, int right, int y,
                            image_sink_t *s, int to)
{
    const dgx_image_t *im = c->image;
    const int          u = (left - wk->box_x) / scale, v = (y - wk->box_y) / scale;
    int                sx   = wk->x_back ? im->width - 1 - (wk->swapped ? v : u) : (wk->swapped ? v : u);
    const int          sy   = wk->y_back ? im->height - 1 - (wk->swapped ? u : v) : (wk->swapped ? u : v);
    const uint8_t     *row  = im->data + (size_t)sy * wk->stride;
    const int          dx   = wk->swapped ? 0 : wk->x_back ? -1 : 1;
    const ptrdiff_t    drow = !wk->swapped ? 0 : wk->y_back ? -(ptrdiff_t)wk->stride : (ptrdiff_t)wk->stride;
    const bool         four = c->format == DGX_IMAGE_4BPP;
    if (to == IMAGE_TO_RUNS) s->right = right;
    for (int x = left, next = wk->box_x + (u + 1) * scale;;) {
        const unsigned q     = four ? (sx & 1 ? row[sx >> 1] : row[sx >> 1] >> 4) & 15u : (row[sx >> 3] >> (7 - (sx & 7))) & 1u;
        const bool     put   = (k->puts >> q) & 1u;
        const uint32_t value = k->value[q];
        const int      stop  = scale == 1 ? x + 1 : DGX_MIN(right, next);
        if (to == IMAGE_TO_MEMORY) {
            for (; x < stop; ++x) image_sink(s, IMAGE_TO_MEMORY, k->bits, x, y, put, value);
        } else {
            for (; x < stop; ++x) image_run_pixel(s, k->bits, x, y, put, value);
        }
        if (x >= right) break;
        next += scale, sx += dx, row += drow;
    }
    if (to == IMAGE_TO_RUNS && s->run >= 0) image_run_out(s, k->bits, right, y, false);
}


/*
 * A row with every pixel of the box written, on a screen of 16 bits: every pixel of the picture read, its value found
 * and written by one loop for its format, the row straight from the first pixel to the last; a picture of 16 bits of
 * no plane, neither turned nor mirrored, copied as it lies, its bytes in the order of the screen.
 */

/* what a pixel not opaque puts with DGX_IMAGE_OPAQUE: bg, mixed with it, or its color from half on */
static __attribute__((noinline)) uint32_t image_opaque16(const image_put_t *k, uint32_t color, unsigned a)
{
    return a == 0u ? k->bg : k->mix ? dgx_mix_color(16, k->bg, color, a) : a >= 128u ? color : k->bg;
}

#define IMAGE_ROW16(VALUE, STEP)                                                \
    do {                                                                        \
        for (;;) {                                                              \
            const uint32_t value = (VALUE);                                     \
            const uint8_t  high = (uint8_t)(value >> 8), low = (uint8_t)value;  \
            const int      stop = scale == 1 ? x + 1 : DGX_MIN(right, next);    \
            for (; x < stop; ++x) out[0] = high, out[1] = low, out += 2;        \
            if (x >= right) break;                                              \
            next += scale;                                                      \
            STEP;                                                               \
        }                                                                       \
    } while (0)

#define IMAGE_OPAQUE16(COLOR, ALPHA) ((ALPHA) == 255u ? (COLOR) : image_opaque16(k, (COLOR), (ALPHA)))

static __attribute__((noinline)) void image_row16(const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left,
                                                  int right, int y, uint8_t *out)
{
    const dgx_image_t *im = c->image;
    const int          W = im->width, H = im->height;
    const int          u = (left - wk->box_x) / scale, v = (y - wk->box_y) / scale;
    int                sx   = wk->x_back ? W - 1 - (wk->swapped ? v : u) : (wk->swapped ? v : u);
    const int          sy   = wk->y_back ? H - 1 - (wk->swapped ? u : v) : (wk->swapped ? u : v);
    const uint8_t     *row  = im->data + (size_t)sy * wk->stride;
    const int          dx   = wk->swapped ? 0 : wk->x_back ? -1 : 1;
    const ptrdiff_t    drow = !wk->swapped ? 0 : wk->y_back ? -(ptrdiff_t)wk->stride : (ptrdiff_t)wk->stride;
    int                x = left, next = wk->box_x + (u + 1) * scale; /* where the piece of the next pixel of the picture starts */
    switch (c->format) {
    case DGX_IMAGE_16BPP: {
        const uint8_t  *p  = row + 2 * sx;
        const ptrdiff_t dp = wk->swapped ? drow : 2 * dx;
        if (!im->alpha) {
            if (scale == 1 && dp == 2) {
                memcpy(out, p, 2u * (uint32_t)(right - left));
                return;
            }
            IMAGE_ROW16((uint32_t)p[0] << 8 | p[1], p += dp);
        } else {
            const uint8_t  *a  = im->alpha + (size_t)sy * (size_t)W + (size_t)sx;
            const ptrdiff_t da = !wk->swapped ? dx : wk->y_back ? -(ptrdiff_t)W : (ptrdiff_t)W;
            IMAGE_ROW16(IMAGE_OPAQUE16((uint32_t)p[0] << 8 | p[1], *a), (p += dp, a += da));
        }
        return;
    }
    case DGX_IMAGE_8BPP: {
        const uint8_t  *p  = row + sx;
        const ptrdiff_t dp = wk->swapped ? drow : dx;
        if (c->coverage) IMAGE_ROW16(IMAGE_OPAQUE16(c->color, *p), p += dp);
        else if (c->alpha) IMAGE_ROW16(IMAGE_OPAQUE16(c->colors[*p], c->alpha[*p]), p += dp);
        else IMAGE_ROW16(*p ? c->colors[*p] : k->bg, p += dp);
        return;
    }
    case DGX_IMAGE_4BPP:
        IMAGE_ROW16(k->value[(sx & 1 ? row[sx >> 1] : row[sx >> 1] >> 4) & 15u], (sx += dx, row += drow));
        return;
    default:
        IMAGE_ROW16(k->value[(row[sx >> 3] >> (7 - (sx & 7))) & 1u], (sx += dx, row += drow));
        return;
    }
}

/* a row of every pixel of the box made into a buffer: by image_row16() where it serves, else by image_row() */
static inline void image_make_row(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left,
                                  int right, int y, image_sink_t *s)
{
    if (k->fast) image_row16(wk, scale, c, k, left, right, y, s->base + 2u * ((uint32_t)left - s->first));
    else image_row_to_row(dst, wk, scale, c, k, left, right, y, s);
}

/*
 * Every pixel of the box on a display behind a bus whose pixels are of whole bytes and whose buffer holds two rows:
 * one window, the rows made into the halves of the buffer, one while the other is on the wire. At a scale the rows
 * of a band are one row of the picture, made once and copied, from a half that is not written over before the copy.
 */
static bool image_halves(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left, int top,
                         int right, int bottom)
{
    const uint8_t bytes = dgx_vscreen_pixel_bytes(dst->color_bits);
    if (!bytes || dst->screen_subtype != DgxPhysicalScreenWithBus || !dst->set_area || !dst->write_area || !dst->wait) return false;
    const dgx_bus_protocols_t *bus       = ((dgx_screen_with_bus_t *)dst)->bus;
    const uint32_t             row_bytes = (uint32_t)(right - left) * bytes;
    const uint32_t             all_bytes = row_bytes * (uint32_t)(bottom - top);
    if (!bus->buffer || (all_bytes > bus->buffer_len && row_bytes > bus->buffer_len / 2u / bytes * bytes)) return false;
    dgx_vscreen_halves_t h;
    dgx_vscreen_halves_window(&h, dst, left, top, right - left, bottom - top, bytes);
    const uint8_t *made     = NULL;
    int            made_row = -1; /* the row of the picture made last */
    for (int y = top; y < bottom; ++y) {
        const int row = (y - wk->box_y) / scale;
        if (row == made_row) {
            memmove(h.at, made, row_bytes);
        } else {
            image_sink_t s = {.dst = dst, .base = h.at, .first = (uint32_t)left};
            image_make_row(dst, wk, scale, c, k, left, right, y, &s);
            made = h.at, made_row = row;
        }
        h.at += row_bytes;
        if (dgx_vscreen_halves_room(&h) == 0) dgx_vscreen_halves_send(&h);
    }
    dgx_vscreen_halves_end(&h);
    return true;
}

#if DGX_IMAGE_INDEXED
/* a virtual screen in memory: every pixel written in its place, no row of memory needed; with every pixel of the box
   put, its rows are rows of memory */
static void image_to_memory(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left, int top,
                            int right, int bottom)
{
    image_sink_t s = {.dst = dst, .base = ((dgx_vscreen_t *)dst)->v_array};
    for (int y = top; y < bottom; ++y) {
        s.first = (uint32_t)y * (uint32_t)dst->width;
        if (k->fast) image_row16(wk, scale, c, k, left, right, y, s.base + 2u * (s.first + (uint32_t)left));
        else if (k->table) image_row_table(wk, scale, c, k, left, right, y, &s, IMAGE_TO_MEMORY);
        else image_row_to_memory(dst, wk, scale, c, k, left, right, y, &s);
    }
}
#endif

/*
 * Every pixel of the box on a display behind a bus where image_halves() does not serve: the buffer does not hold two
 * rows, or a pixel is not of whole bytes. One window, a row at a time through the buffer, a band of rows at a scale
 * sent from one row made once.
 */
static bool image_one_buffer(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left, int top,
                             int right, int bottom)
{
    const uint32_t n = (uint32_t)(right - left), bytes = dgx_color_points_to_bytes(dst->color_bits, n);
    if (dst->screen_subtype != DgxPhysicalScreenWithBus || !dst->draw_buffer || n * dst->color_bits % 8u || bytes > dst->draw_buffer_len) return false;
    if (dst->wait) dst->wait(dst);
    dst->set_area(dst, (uint16_t)left, (uint16_t)(right - 1), (uint16_t)top, (uint16_t)(bottom - 1));
    int made = -1; /* the row of the picture the buffer holds */
    for (int y = top; y < bottom; ++y) {
        const int row = (y - wk->box_y) / scale;
        if (row != made) {
            if (dst->wait) dst->wait(dst); /* the rows before may still be on their way from the buffer */
            image_sink_t s = {.dst = dst, .base = dst->draw_buffer, .first = (uint32_t)left};
            image_make_row(dst, wk, scale, c, k, left, right, y, &s);
            made = row;
        }
        dst->write_area(dst, dst->draw_buffer, bytes * 8u);
    }
    return true;
}

/*
 * The pixels put by runs, each through a window of its own: a display behind a bus through its buffer, a row longer
 * than it, or another screen with windows, through a row of its own; a screen without windows a pixel at a time.
 * Either buffer may still be on the wire when write_area() returns.
 */
static esp_err_t image_by_runs(dgx_screen_t *dst, const image_walk_t *wk, int scale, const image_colors_t *c, const image_put_t *k, int left, int top,
                               int right, int bottom, bool *window)
{
    const uint32_t bytes = dgx_color_points_to_bytes(dst->color_bits, (uint32_t)(right - left));
    *window              = dst->set_area && dst->write_area;
    const bool bus_buffer = *window && dst->draw_buffer && dst->draw_buffer_len >= bytes;
    uint8_t   *own        = NULL;
    if (*window && !bus_buffer) {
        own = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
        if (!own) {
            *window = false;
            return ESP_ERR_NO_MEM;
        }
    }
    image_sink_t s = {.dst = dst, .base = bus_buffer ? dst->draw_buffer : own, .window = *window, .shared = *window, .run = -1, .right = right};
    if (bus_buffer && dst->screen_subtype == DgxPhysicalScreenWithBus && bytes <= dst->draw_buffer_len / 2u) {
        s.ring = true, s.halves = dst->draw_buffer, s.half = dst->draw_buffer_len / 2u;
        if (dst->wait) dst->wait(dst); /* the buffer free of what was sent before */
    }
    for (int y = top; y < bottom; ++y) {
        if (s.shared && !s.ring && dst->wait) dst->wait(dst); /* the row before may still be on its way from the buffer */
        if (k->table) image_row_table(wk, scale, c, k, left, right, y, &s, IMAGE_TO_RUNS);
        else image_row_to_runs(dst, wk, scale, c, k, left, right, y, &s);
    }
    if (own) {
        if (dst->wait) dst->wait(dst); /* the last run off the row before it is freed */
        heap_caps_free(own);
    }
    return ESP_OK;
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
    k.fast             = bits == 16 && k.opaque;
    if (c->format == DGX_IMAGE_1BPP || c->format == DGX_IMAGE_4BPP) {
        /* what each value of 1 or 4 bits puts; the table serves unless a value partly opaque is mixed with what lies under it */
        const unsigned values = c->format == DGX_IMAGE_1BPP ? 2u : 16u;
        k.table               = true;
        for (unsigned q = 0; q < values; ++q) {
            const uint32_t color = c->coverage ? c->color : c->colors[q];
            const unsigned a     = c->coverage ? (c->format == DGX_IMAGE_4BPP ? q * 17u : q * 255u) : c->alpha ? c->alpha[q] : q ? 255u : 0u;
            if (k.under && a != 0u && a != 255u) k.table = false;
            if (image_put_value(dst, &k, color, a, &k.value[q])) k.puts |= (uint16_t)(1u << q);
        }
    }
    dgx_screen_batch_begin(dst);
    esp_err_t e       = ESP_OK;
    bool      touched = true;
#if DGX_IMAGE_INDEXED
    if (dgx_vscreen_is_linear(dst)) image_to_memory(dst, wk, scale, c, &k, left, top, right, bottom);
    else
#endif
        if (k.opaque && image_halves(dst, wk, scale, c, &k, left, top, right, bottom)) {
    } else if (k.opaque && image_one_buffer(dst, wk, scale, c, &k, left, top, right, bottom)) {
    } else {
        e = image_by_runs(dst, wk, scale, c, &k, left, top, right, bottom, &touched);
    }
    if (touched) dgx_screen_touch(dst, left, right - 1, top, bottom - 1);
    dgx_screen_batch_end(dst);
    return e;
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
