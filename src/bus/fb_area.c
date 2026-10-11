/*
 * fb_area.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stddef.h>
#include <string.h>

#include "dgx_fb_area_priv.h"

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

void dgx_fb_area_write(dgx_fb_area_t *area, const uint8_t *data, uint32_t pixels)
{
    while (pixels) {
        uint8_t  *to;
        ptrdiff_t step;
        uint32_t  count = dgx_fb_area_run(area, pixels, &to, &step);
        dgx_fb_area_touch(area, to, step, count);
        pixels -= count;
        if (area->pixel_bytes == 2) {
            if (!area->swap_rb) {
                /* the buffer has the high byte first, the frame buffer the low one */
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
    const uint8_t *above = NULL; /* a row of the rectangle already filled that lies as one piece of memory */
    for (uint32_t row = y; row < (uint32_t)y + height; ++row) {
        ptrdiff_t step;
        uint8_t  *to = dgx_fb_area_at(area, x, row, &step);
        dgx_fb_area_touch(area, to, step, width);
        if (step == bytes && above) {
            memcpy(to, above, (size_t)width * bytes);
            continue;
        }
        uint8_t *lp = to;
        if (bytes == 2) {
            for (uint32_t i = 0; i < width; ++i, lp += step) {
                lp[0] = stored[0];
                lp[1] = stored[1];
            }
        } else {
            for (uint32_t i = 0; i < width; ++i, lp += step) {
                lp[0] = stored[0];
                lp[1] = stored[1];
                lp[2] = stored[2];
            }
        }
        if (step == bytes) above = to;
    }
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
