/*
 * fb_area.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stddef.h>
#include <string.h>

#include "dgx_fb_area_priv.h"
#include "../vscreen/dgx_overlay_priv.h"

#define DGX_FB_AREA_CACHE_LINE 64u

void dgx_fb_area_init(dgx_fb_area_t *area, uint8_t *fb, uint16_t fb_width, uint16_t fb_height, uint8_t pixel_bytes, bool swap_rb)
{
    area->fb          = fb;
    area->fb_width    = fb_width;
    area->fb_height   = fb_height;
    area->pixel_bytes = pixel_bytes;
    area->swap_rb     = swap_rb;
    area->dirty_from = area->dirty_to = 0;
    area->dirty_weight               = 0;
    area->command                    = 0;
    dgx_fb_area_orientation(area, false, false, false);
}

void dgx_fb_area_orientation(dgx_fb_area_t *area, bool mirror_x, bool mirror_y, bool swap_xy)
{
    area->mirror_x = mirror_x;
    area->mirror_y = mirror_y;
    area->swap_xy  = swap_xy;
    area->left = area->top = 0;
    area->right  = dgx_fb_area_width(area) - 1;
    area->bottom = dgx_fb_area_height(area) - 1;
    dgx_fb_area_rewind(area);
}

static void dgx_fb_area_cut(uint16_t *from, uint16_t *to, uint16_t size)
{
    if (*from > *to) {
        uint16_t t = *from;
        *from      = *to;
        *to        = t;
    }
    if (*to >= size) *to = size - 1;
    if (*from >= size) *from = size - 1;
}

void dgx_fb_area_set_columns(dgx_fb_area_t *area, uint16_t from, uint16_t to)
{
    dgx_fb_area_cut(&from, &to, dgx_fb_area_width(area));
    area->left  = from;
    area->right = to;
    dgx_fb_area_rewind(area);
}

void dgx_fb_area_set_rows(dgx_fb_area_t *area, uint16_t from, uint16_t to)
{
    dgx_fb_area_cut(&from, &to, dgx_fb_area_height(area));
    area->top    = from;
    area->bottom = to;
    dgx_fb_area_rewind(area);
}

/* Where the pixel (x, y) of the screen lies in the frame buffer and how many bytes further the next one of its row */
static uint8_t *dgx_fb_area_at(const dgx_fb_area_t *area, uint32_t x, uint32_t y, ptrdiff_t *step)
{
    uint32_t  px = area->swap_xy ? y : x;
    uint32_t  py = area->swap_xy ? x : y;
    ptrdiff_t along;
    if (area->swap_xy) {
        along = (ptrdiff_t)area->fb_width * area->pixel_bytes;
        if (area->mirror_y) along = -along;
    } else {
        along = area->mirror_x ? -(ptrdiff_t)area->pixel_bytes : (ptrdiff_t)area->pixel_bytes;
    }
    if (area->mirror_x) px = area->fb_width - 1u - px;
    if (area->mirror_y) py = area->fb_height - 1u - py;
    *step = along;
    return area->fb + (py * area->fb_width + px) * area->pixel_bytes;
}

/*
 * The pixels of a row of the window from the next one to the end of the row,
 * or `pixels` of them when that is less: where the first lies in the frame
 * buffer and how many bytes further each following one. The next pixel moves
 * past them.
 */
static uint32_t dgx_fb_area_run(dgx_fb_area_t *area, uint32_t pixels, uint8_t **first, ptrdiff_t *step)
{
    uint32_t count = (uint32_t)(area->right - area->x) + 1u;
    if (count > pixels) count = pixels;
    *first = dgx_fb_area_at(area, area->x, area->y, step);
    area->x += count;
    if (area->x > area->right) {
        area->x = area->left;
        area->y = area->y >= area->bottom ? area->top : area->y + 1;
    }
    return count;
}

static void dgx_fb_area_touch(dgx_fb_area_t *area, const uint8_t *first, ptrdiff_t step, uint32_t count)
{
    const uint8_t *last = first + step * (ptrdiff_t)(count - 1u);
    uint32_t       from = (uint32_t)((step < 0 ? last : first) - area->fb);
    uint32_t       to   = (uint32_t)((step < 0 ? first : last) - area->fb) + area->pixel_bytes;
    if (area->dirty_to <= area->dirty_from) {
        area->dirty_from = from;
        area->dirty_to   = to;
    } else {
        if (from < area->dirty_from) area->dirty_from = from;
        if (to > area->dirty_to) area->dirty_to = to;
    }
    /* pixels that follow one another share lines of a cache, pixels a row apart take a line each */
    uint32_t apart = (uint32_t)(step < 0 ? -step : step);
    area->dirty_weight += apart >= DGX_FB_AREA_CACHE_LINE ? count * DGX_FB_AREA_CACHE_LINE : DGX_FB_AREA_CACHE_LINE + count * apart;
}

/* RGB565 with red and blue exchanged */
static inline uint16_t dgx_fb_area_swap_565(uint16_t color)
{
    return (uint16_t)((color & 0x07e0u) | (color >> 11) | ((color & 0x1fu) << 11));
}

/* a word of a pixel buffer: the buffers are bytes, and are moved by words where that is faster */
typedef uint32_t __attribute__((may_alias)) dgx_fb_word_t;

/* From this many pixels on a run of 16-bit pixels is moved by words: measured on an ESP32-P4 */
#define DGX_FB_AREA_WORDS_FROM 16u

/* the two bytes of each of the two pixels in a word exchanged */
static inline uint32_t dgx_fb_area_swap_word(uint32_t word)
{
    return ((word & 0x00ff00ffu) << 8) | ((word >> 8) & 0x00ff00ffu);
}

/*
 * 16-bit pixels of a buffer, the high byte first, into pixels of the frame
 * buffer that follow one another, the low byte first. Two pixels are one
 * word, and their bytes are exchanged in it at once: a third faster than
 * byte by byte on a long run. Words are read and written at their own
 * boundaries only, so when the buffer and the frame buffer start at
 * different halves of a word, a pixel is carried from one word to the next.
 */
static void dgx_fb_area_write_words(uint8_t *to, const uint8_t *data, uint32_t count)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (count >= DGX_FB_AREA_WORDS_FROM && !(((uintptr_t)to | (uintptr_t)data) & 1u)) {
        if ((uintptr_t)to & 2u) {
            to[0] = data[1], to[1] = data[0];
            to += 2, data += 2, --count;
        }
        dgx_fb_word_t *wp = (dgx_fb_word_t *)(void *)to;
        if (!((uintptr_t)data & 2u)) {
            const dgx_fb_word_t *rp = (const dgx_fb_word_t *)(const void *)data;
            for (uint32_t pairs = count >> 1; pairs; --pairs) *wp++ = dgx_fb_area_swap_word(*rp++);
            data = (const uint8_t *)rp;
            count &= 1u;
        } else {
            /* the first pixel waits for the one that shares its word in the frame buffer */
            uint32_t             carry = (uint32_t)data[1] | (uint32_t)data[0] << 8;
            const dgx_fb_word_t *rp    = (const dgx_fb_word_t *)(const void *)(data + 2);
            --count;
            for (uint32_t pairs = count >> 1; pairs; --pairs) {
                uint32_t word = dgx_fb_area_swap_word(*rp++);
                *wp++         = carry | word << 16;
                carry         = word >> 16;
            }
            data = (const uint8_t *)rp;
            to   = (uint8_t *)wp;
            to[0] = (uint8_t)carry, to[1] = (uint8_t)(carry >> 8);
            wp    = (dgx_fb_word_t *)(void *)(to + 2); /* only as a place: the pixel left over goes by bytes */
            count &= 1u;
        }
        to = (uint8_t *)wp;
    }
#endif
    for (; count; --count, data += 2, to += 2) {
        to[0] = data[1];
        to[1] = data[0];
    }
}

void dgx_fb_area_write(dgx_fb_area_t *area, const uint8_t *data, uint32_t pixels)
{
    while (pixels) {
        uint8_t  *to;
        ptrdiff_t step;
        uint32_t  count = dgx_fb_area_run(area, pixels, &to, &step);
        dgx_fb_area_touch(area, to, step, count);
        pixels -= count;
        if (area->pixel_bytes == 2) {
            if (!area->swap_rb && step == 2) {
                /* the buffer has the high byte first, the frame buffer the low one */
                dgx_fb_area_write_words(to, data, count);
                data += (size_t)count * 2u;
            } else if (!area->swap_rb) {
                /* mirrored or turned: a pixel at a time */
                for (; count; --count, data += 2, to += step) {
                    to[0] = data[1];
                    to[1] = data[0];
                }
            } else {
                for (; count; --count, data += 2, to += step) {
                    uint16_t color = dgx_fb_area_swap_565((uint16_t)(data[0] << 8 | data[1]));
                    to[0]          = (uint8_t)color;
                    to[1]          = (uint8_t)(color >> 8);
                }
            }
        } else {
            /* the buffer has red, green, blue; the frame buffer blue, green, red */
            uint8_t r = area->swap_rb ? 0 : 2, b = 2 - r;
            for (; count; --count, data += 3, to += step) {
                to[r] = data[0];
                to[1] = data[1];
                to[b] = data[2];
            }
        }
    }
}

void dgx_fb_area_read(dgx_fb_area_t *area, uint8_t *data, uint32_t pixels)
{
    while (pixels) {
        uint8_t  *from;
        ptrdiff_t step;
        uint32_t  count = dgx_fb_area_run(area, pixels, &from, &step);
        pixels -= count;
        if (area->pixel_bytes == 2) {
            for (; count; --count, data += 2, from += step) {
                uint16_t color = (uint16_t)(from[1] << 8 | from[0]);
                if (area->swap_rb) color = dgx_fb_area_swap_565(color);
                data[0] = (uint8_t)(color >> 8);
                data[1] = (uint8_t)color;
            }
        } else {
            uint8_t r = area->swap_rb ? 0 : 2, b = 2 - r;
            for (; count; --count, data += 3, from += step) {
                data[0] = from[r];
                data[1] = from[1];
                data[2] = from[b];
            }
        }
    }
}

/* a color as the frame buffer holds it */
static void dgx_fb_area_stored(const dgx_fb_area_t *area, uint32_t color, uint8_t *stored)
{
    if (area->pixel_bytes == 2) {
        if (area->swap_rb) color = dgx_fb_area_swap_565((uint16_t)color);
        stored[0] = (uint8_t)color;
        stored[1] = (uint8_t)(color >> 8);
    } else {
        uint8_t r = area->swap_rb ? 0 : 2;
        stored[r]     = (uint8_t)(color >> 16);
        stored[1]     = (uint8_t)(color >> 8);
        stored[2 - r] = (uint8_t)color;
    }
}

void dgx_fb_area_set_pixel(dgx_fb_area_t *area, uint16_t x, uint16_t y, uint32_t color)
{
    ptrdiff_t step;
    uint8_t  *to = dgx_fb_area_at(area, x, y, &step);
    dgx_fb_area_stored(area, color, to);
    dgx_fb_area_touch(area, to, step, 1);
}

uint32_t dgx_fb_area_get_pixel(const dgx_fb_area_t *area, uint16_t x, uint16_t y)
{
    ptrdiff_t      step;
    const uint8_t *from = dgx_fb_area_at(area, x, y, &step);
    if (area->pixel_bytes == 2) {
        uint16_t color = (uint16_t)(from[1] << 8 | from[0]);
        return area->swap_rb ? dgx_fb_area_swap_565(color) : color;
    }
    uint8_t r = area->swap_rb ? 0 : 2;
    return (uint32_t)from[r] << 16 | (uint32_t)from[1] << 8 | from[2 - r];
}

void dgx_fb_area_fill(dgx_fb_area_t *area, uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint32_t color)
{
    uint8_t stored[3];
    uint8_t bytes = area->pixel_bytes;
    dgx_fb_area_stored(area, color, stored);
    /* the bytes in locals: every store into the frame buffer would have the compiler read `stored` again */
    const uint8_t  s0 = stored[0], s1 = stored[1], s2 = stored[2];
    const uint16_t pixel = (uint16_t)(s0 | s1 << 8);
    const uint8_t *above = NULL; /* a row of the rectangle already filled that lies as one piece of memory, from its lowest address */
    for (uint32_t row = y; row < (uint32_t)y + height; ++row) {
        ptrdiff_t step;
        uint8_t  *to = dgx_fb_area_at(area, x, row, &step);
        dgx_fb_area_touch(area, to, step, width);
        /* a row of the frame buffer, mirrored or not, is one piece of memory; its lowest address */
        const bool along = step == bytes || step == -(ptrdiff_t)bytes;
        uint8_t   *first = step > 0 ? to : to - (size_t)(width - 1u) * bytes;
        if (along && above) {
            memcpy(first, above, (size_t)width * bytes);
            continue;
        }
        if (along && bytes == 2) {
            /* a halfword a pixel, two pixels a word from a boundary of a word on */
            uint16_t *p = (uint16_t *)first;
            uint32_t  n = width;
            if (((uintptr_t)p & 2u) && n) *p++ = pixel, --n;
            uint32_t      *w    = (uint32_t *)p;
            const uint32_t both = pixel | (uint32_t)pixel << 16;
            for (; n >= 2; n -= 2) *w++ = both;
            if (n) *(uint16_t *)w = pixel;
        } else if (bytes == 2) {
            uint8_t *lp = to;
            for (uint32_t i = 0; i < width; ++i, lp += step) lp[0] = s0, lp[1] = s1;
        } else {
            uint8_t *lp = to;
            for (uint32_t i = 0; i < width; ++i, lp += step) lp[0] = s0, lp[1] = s1, lp[2] = s2;
        }
        if (along) above = first;
    }
}

void dgx_fb_area_to_panel(const dgx_fb_area_t *area, int left, int right, int top, int bottom, uint16_t *x0, uint16_t *y0, uint16_t *x1,
                          uint16_t *y1)
{
    /* two opposite corners are enough: mirroring and swapping keep a rectangle a rectangle */
    uint32_t ax = area->swap_xy ? (uint32_t)top : (uint32_t)left, ay = area->swap_xy ? (uint32_t)left : (uint32_t)top;
    uint32_t bx = area->swap_xy ? (uint32_t)bottom : (uint32_t)right, by = area->swap_xy ? (uint32_t)right : (uint32_t)bottom;
    if (area->mirror_x) ax = area->fb_width - 1u - ax, bx = area->fb_width - 1u - bx;
    if (area->mirror_y) ay = area->fb_height - 1u - ay, by = area->fb_height - 1u - by;
    *x0 = (uint16_t)(ax < bx ? ax : bx), *x1 = (uint16_t)(ax < bx ? bx : ax);
    *y0 = (uint16_t)(ay < by ? ay : by), *y1 = (uint16_t)(ay < by ? by : ay);
}

void dgx_fb_area_move(dgx_fb_area_t *area, uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t to_x, uint16_t to_y)
{
    if (!width || !height || (x == to_x && y == to_y)) return;
    uint16_t from_x0, from_y0, to_x0, to_y0, x1, y1;
    dgx_fb_area_to_panel(area, x, x + width - 1, y, y + height - 1, &from_x0, &from_y0, &x1, &y1);
    const uint32_t columns = (uint32_t)x1 - from_x0 + 1u, rows = (uint32_t)y1 - from_y0 + 1u;
    dgx_fb_area_to_panel(area, to_x, to_x + width - 1, to_y, to_y + height - 1, &to_x0, &to_y0, &x1, &y1);
    const size_t   pitch = (size_t)area->fb_width * area->pixel_bytes, bytes = (size_t)columns * area->pixel_bytes;
    const uint8_t *from = area->fb + from_y0 * pitch + (size_t)from_x0 * area->pixel_bytes;
    uint8_t       *to   = area->fb + to_y0 * pitch + (size_t)to_x0 * area->pixel_bytes;
    /* a picture that goes down the panel is moved from its last row: no row is written over before it is read */
    const bool     down = to_y0 > from_y0;
    for (uint32_t i = 0; i < rows; ++i) {
        const uint32_t row = down ? rows - 1u - i : i;
        memmove(to + row * pitch, from + row * pitch, bytes);
        dgx_fb_area_touch(area, to + row * pitch, area->pixel_bytes, columns);
    }
}

/*
 * How a row of the panel runs along the picture made `scale` times as large,
 * see dgx_fb_area_indexed_rows(). The picture is turned on the screen and
 * the screen on the panel; whatever the two turns, a row of the panel is a
 * row or a column of the larger picture, read forward or back, and the next
 * row of the panel is the line next to it, one way or the other.
 */
typedef struct {
    uint16_t x0, y0, x1, y1; /* the rectangle on the panel */
    bool     along_y;        /* a row of the panel runs along a column of the larger picture */
    int      dir;            /* along it forward, or back */
    int32_t  runs_at;        /* where it is in the first column of the rectangle */
    int      across;         /* the next row of the panel is the line after this one, or the one before it */
    int32_t  line_at;        /* the line the first row of the rectangle on the panel is */
} dgx_fb_area_runs_t;

/* the point of the larger picture, as it lies before it is turned, the pixel (px, py) of the panel shows */
static void dgx_fb_area_picture_point(const dgx_fb_area_t *area, uint16_t left, uint16_t top, const dgx_bus_indexed_t *picture, int32_t px,
                                      int32_t py, int32_t *u, int32_t *v)
{
    /* the pixel of the screen, as dgx_fb_area_point_to_panel() tells the other way */
    const int32_t ax = area->mirror_x ? area->fb_width - 1 - px : px, ay = area->mirror_y ? area->fb_height - 1 - py : py;
    const int32_t sx = area->swap_xy ? ay : ax, sy = area->swap_xy ? ax : ay;
    /* the point of the turned picture, and of the picture before the turn */
    const int32_t tx = (int32_t)picture->x + sx - left, ty = (int32_t)picture->y + sy - top;
    const int32_t wide = (int32_t)picture->width * picture->scale, high = (int32_t)picture->height * picture->scale;
    const unsigned turn = picture->orientation;
    if (!(turn & 4u)) {
        *u = turn & 1u ? wide - 1 - tx : tx;
        *v = turn & 2u ? high - 1 - ty : ty;
    } else {
        *u = turn & 1u ? wide - 1 - ty : ty;
        *v = turn & 2u ? high - 1 - tx : tx;
    }
}

static void dgx_fb_area_runs(const dgx_fb_area_t *area, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const dgx_bus_indexed_t *picture,
                             dgx_fb_area_runs_t *runs)
{
    dgx_fb_area_to_panel(area, left, left + width - 1, top, top + height - 1, &runs->x0, &runs->y0, &runs->x1, &runs->y1);
    /* the first pixel of the rectangle on the panel, the one right of it and the one below it: one point of the picture changes by one each time */
    int32_t u, v, u_right, v_right, u_below, v_below;
    dgx_fb_area_picture_point(area, left, top, picture, runs->x0, runs->y0, &u, &v);
    dgx_fb_area_picture_point(area, left, top, picture, runs->x0 + 1, runs->y0, &u_right, &v_right);
    dgx_fb_area_picture_point(area, left, top, picture, runs->x0, runs->y0 + 1, &u_below, &v_below);
    runs->along_y = v_right != v;
    runs->dir     = runs->along_y ? (int)(v_right - v) : (int)(u_right - u);
    runs->runs_at = runs->along_y ? v : u;
    runs->across  = runs->along_y ? (int)(u_below - u) : (int)(v_below - v);
    runs->line_at = runs->along_y ? u : v;
}

void dgx_fb_area_indexed_lines(const dgx_fb_area_t *area, dgx_scale_lines_t *lines, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                               const dgx_bus_indexed_t *picture, uint8_t *scratch)
{
    dgx_fb_area_runs_t runs;
    dgx_fb_area_runs(area, left, top, width, height, picture, &runs);
    const uint32_t columns = (uint32_t)runs.x1 - runs.x0 + 1u;
    /* the points of a line the row takes, from the first one */
    const int32_t from = runs.dir > 0 ? runs.runs_at : runs.runs_at - (int32_t)columns + 1;
    dgx_scale_lines_begin(lines, picture, runs.along_y, from, columns, scratch);
}

/*
 * A smoothed picture: every row of the panel is a line of the larger
 * picture, a row of it or a column, made by dgx_scale_colors() in the
 * direction the panel runs. Rows of the panel next to each other that are
 * made from the same line of the smaller picture are made together.
 */
static void dgx_fb_area_smooth_rows(uint16_t *row, uint32_t columns, const dgx_fb_area_runs_t *runs, uint32_t first, uint32_t rows,
                                    dgx_scale_lines_t *lines)
{
    /* a row made backwards starts at its end */
    const uint32_t start = runs->dir > 0 ? 0u : columns - 1u;
    for (uint32_t r = 0; r < rows;) {
        const int32_t at   = runs->line_at + (int32_t)(first + r) * runs->across;
        uint32_t      some = dgx_scale_lines_left(lines, at, runs->across);
        if (some > rows - r) some = rows - r;
        dgx_scale_colors(lines, at, runs->across, some, row + start, (ptrdiff_t)columns, runs->dir);
        row += (size_t)some * columns;
        r += some;
    }
}

void dgx_fb_area_indexed_rows(const dgx_fb_area_t *area, uint8_t *to, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                              const dgx_bus_indexed_t *picture, uint16_t first, uint16_t rows, dgx_scale_lines_t *lines)
{
    dgx_fb_area_runs_t runs;
    dgx_fb_area_runs(area, left, top, width, height, picture, &runs);
    const uint32_t columns = (uint32_t)runs.x1 - runs.x0 + 1u;
    const uint32_t scale   = picture->scale;
    const int      dir     = runs.dir;
    /* both the ESP32-P4 and the hosts the tests run on keep the low byte of a pixel first, as the frame buffer does */
    uint16_t *row = (uint16_t *)to;
    if (picture->smooth && (scale == 2 || scale == 3 || scale == 4)) {
        dgx_fb_area_smooth_rows(row, columns, &runs, first, rows, lines);
        return;
    }
    /* the first index of a row, how many pixels it has there, and the step to the next index */
    const uint32_t  runs_index = (uint32_t)runs.runs_at / scale;
    const uint32_t  first_run  = dir > 0 ? scale - (uint32_t)runs.runs_at % scale : (uint32_t)runs.runs_at % scale + 1u;
    const uint32_t  pitch      = picture->pitch;
    const ptrdiff_t step       = runs.along_y ? dir * (ptrdiff_t)pitch : dir;
    const uint16_t *lut        = picture->lut;
    uint32_t        line_last  = UINT32_MAX;
    for (uint32_t r = 0; r < rows; ++r, row += columns) {
        const uint32_t line_index = (uint32_t)(runs.line_at + (int32_t)(first + r) * runs.across) / scale;
        if (line_index == line_last) {
            /* the indices of the row above: the same line of the picture again */
            memcpy(row, row - columns, columns * 2u);
            continue;
        }
        line_last            = line_index;
        const uint8_t *index = picture->pixels + (runs.along_y ? line_index + (size_t)runs_index * pitch : (size_t)line_index * pitch + runs_index);
        if (scale == 1 && step == 1) {
            /*
             * Every index a pixel, the indices side by side: four at a time, so
             * that the loads of one pixel do not wait for those of another. Down
             * a column of the picture, a line of the cache apart each, that only
             * costs more.
             */
            uint32_t i = 0;
            for (; i + 4u <= columns; i += 4u, index += 4) {
                const uint16_t c0 = lut[index[0]], c1 = lut[index[1]], c2 = lut[index[2]], c3 = lut[index[3]];
                row[i] = c0, row[i + 1] = c1, row[i + 2] = c2, row[i + 3] = c3;
            }
            for (; i < columns; ++i) row[i] = lut[*index++];
            continue;
        }
        if (scale == 1) {
            /* every index a pixel: nothing to repeat */
            for (uint32_t i = 0; i < columns; ++i, index += step) row[i] = lut[*index];
            continue;
        }
        uint16_t *out     = row;
        uint32_t  left_px = columns, run = first_run;
        while (left_px) {
            const uint32_t n     = run < left_px ? run : left_px;
            const uint16_t color = lut[*index];
            for (uint32_t i = 0; i < n; ++i) out[i] = color;
            out += n;
            left_px -= n;
            index += step;
            run = scale;
        }
    }
}

/* a pixel of the band mixed with the layer's index */
static inline void dgx_fb_area_overlay_mix(uint16_t *out, const uint32_t *pre, uint32_t under, uint8_t index)
{
    *out = under ? dgx_bus_overlay_mix(*out, pre[index], under) : (uint16_t)((pre[index] >> 5) | (pre[index] >> 21));
}

void dgx_fb_area_overlay_rows(const dgx_fb_area_t *area, uint8_t *to, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                              const dgx_bus_overlay_t *overlay, uint16_t first, uint16_t rows)
{
    /* the part of the rectangle the layer has something in, and where both lie on the panel */
    const int l = left > overlay->left ? left : overlay->left, r = left + width - 1 < overlay->right ? left + width - 1 : overlay->right;
    const int t = top > overlay->top ? top : overlay->top, b = top + height - 1 < overlay->bottom ? top + height - 1 : overlay->bottom;
    if (l > r || t > b) return;
    uint16_t x0, y0, x1, y1, px0, py0, px1, py1;
    dgx_fb_area_to_panel(area, left, left + width - 1, top, top + height - 1, &x0, &y0, &x1, &y1);
    dgx_fb_area_to_panel(area, l, r, t, b, &px0, &py0, &px1, &py1);
    /* the rows of the band under the layer */
    const uint32_t band_first = (uint32_t)y0 + first, band_last = band_first + rows - 1u;
    const uint32_t from_py = band_first > py0 ? band_first : py0, to_py = band_last < py1 ? band_last : py1;
    if (from_py > to_py) return;
    const uint32_t  columns = (uint32_t)x1 - x0 + 1u, count = (uint32_t)px1 - px0 + 1u;
    const uint32_t *pre = overlay->pre, under = overlay->under;
    uint16_t       *band = (uint16_t *)to;
    /* where a point of the panel lies on the screen, across the rows and along them */
#define DGX_ACROSS(py) ((int32_t)(area->mirror_y ? area->fb_height - 1u - (py) : (py)))
#define DGX_ALONG(px) ((int32_t)(area->mirror_x ? area->fb_width - 1u - (px) : (px)))
    if (!area->swap_xy) {
        /* a row of the panel is a row of the screen and of the layer: the layer is read along it */
        for (uint32_t py = from_py; py <= to_py; ++py) {
            const int32_t  sy = DGX_ACROSS(py), sx = area->mirror_x ? DGX_ALONG(px1) : DGX_ALONG(px0);
            const uint8_t *index = overlay->pixels + (size_t)(sy - overlay->y) * overlay->pitch + (size_t)(sx - overlay->x);
            uint16_t      *row   = band + (size_t)(py - band_first) * columns;
            /* the layer's points from the left of the screen on; on the panel from px0 on, or from px1 back when mirrored */
            /* the pixel of the band the layer's point j goes to, and the step to the next one */
            uint16_t *const base = row + ((area->mirror_x ? px1 : px0) - x0);
            const ptrdiff_t step = area->mirror_x ? -1 : 1;
            const int32_t lx = sx - overlay->x, ly = sy - overlay->y;
            for (uint32_t j = dgx_overlay_next_in(overlay, index, lx, ly, 0, count); j < count; j = dgx_overlay_next_in(overlay, index, lx, ly, j, count)) {
                uint16_t *out = base + (ptrdiff_t)j * step;
                for (; j < count && index[j]; ++j, out += step) dgx_fb_area_overlay_mix(out, pre, under, index[j]);
            }
        }
    } else {
        /*
         * A row of the panel is a column of the screen. The layer is read
         * along its own rows all the same: for every column of the panel,
         * the row of the layer at that y, over the x of the band's rows.
         */
        const int32_t  sx_a = DGX_ACROSS(from_py), sx_b = DGX_ACROSS(to_py);
        const int32_t  sx_lo = sx_a < sx_b ? sx_a : sx_b;
        const uint32_t n     = to_py - from_py + 1u;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t px    = px0 + k;
            const int32_t  sy    = DGX_ALONG(px);
            const uint8_t *index = overlay->pixels + (size_t)(sy - overlay->y) * overlay->pitch + (size_t)(sx_lo - overlay->x);
            /* the screen's x = sx_lo + j is across the panel row from_py + j, or to_py - j when mirrored: a column of the band */
            uint16_t *const base = band + (size_t)((area->mirror_y ? to_py : from_py) - band_first) * columns + (px - x0);
            const ptrdiff_t step = area->mirror_y ? -(ptrdiff_t)columns : (ptrdiff_t)columns;
            const int32_t lx = sx_lo - overlay->x, ly = sy - overlay->y;
            for (uint32_t j = dgx_overlay_next_in(overlay, index, lx, ly, 0, n); j < n; j = dgx_overlay_next_in(overlay, index, lx, ly, j, n)) {
                uint16_t *out = base + (ptrdiff_t)j * step;
                for (; j < n && index[j]; ++j, out += step) dgx_fb_area_overlay_mix(out, pre, under, index[j]);
            }
        }
    }
#undef DGX_ACROSS
#undef DGX_ALONG
}

bool dgx_fb_area_command(dgx_fb_area_t *area, uint8_t command)
{
    switch (command) {
    case DGX_FB_AREA_WRITE:
    case DGX_FB_AREA_READ:
        dgx_fb_area_rewind(area);
        break;
    case DGX_FB_AREA_SET_COLUMNS:
    case DGX_FB_AREA_SET_ROWS:
    case DGX_FB_AREA_WRITE_CONTINUE:
    case DGX_FB_AREA_READ_CONTINUE:
        break;
    default:
        area->command = 0;
        return false;
    }
    area->command = command;
    return true;
}

bool dgx_fb_area_data(dgx_fb_area_t *area, const uint8_t *data, uint32_t bytes)
{
    switch (area->command) {
    case DGX_FB_AREA_SET_COLUMNS:
        if (bytes >= 4) dgx_fb_area_set_columns(area, (uint16_t)(data[0] << 8 | data[1]), (uint16_t)(data[2] << 8 | data[3]));
        return true;
    case DGX_FB_AREA_SET_ROWS:
        if (bytes >= 4) dgx_fb_area_set_rows(area, (uint16_t)(data[0] << 8 | data[1]), (uint16_t)(data[2] << 8 | data[3]));
        return true;
    case DGX_FB_AREA_WRITE:
    case DGX_FB_AREA_WRITE_CONTINUE:
        dgx_fb_area_write(area, data, bytes / area->pixel_bytes);
        return true;
    case DGX_FB_AREA_READ:
    case DGX_FB_AREA_READ_CONTINUE:
        return true; /* nothing is written while the memory is read */
    default:
        return false;
    }
}

bool dgx_fb_area_take_dirty(dgx_fb_area_t *area, uint32_t *from, uint32_t *length)
{
    bool dirty = area->dirty_to > area->dirty_from;
    *from      = dirty ? area->dirty_from : 0;
    *length    = dirty ? area->dirty_to - area->dirty_from : 0;
    area->dirty_from = area->dirty_to = 0;
    area->dirty_weight               = 0;
    return dirty;
}
