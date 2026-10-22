/*
 * vscreen.c
 *
 *  A virtual screen: a picture in memory, drawn on as on any screen.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_arch.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "drivers/vscreen.h"
#include "dgx_vscreen_priv.h"

/* dgx_vscreen_t is defined publicly in drivers/vscreen.h; what moves pictures between screens is in transfer.c */

static const char TAG[] = "DGX V-Screen";

static void dgx_vscreen_set_area(dgx_screen_t *_scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    dgx_vscreen_t *scr = (dgx_vscreen_t *)_scr;
    if (left >= scr->base.width || top >= scr->base.height || right >= scr->base.width || bottom >= scr->base.height) {
        ESP_LOGE(TAG, "Set area failed");
        return;
    }
    scr->area.left     = left;
    scr->area.right    = right;
    scr->area.top      = top;
    scr->area.bottom   = bottom;
    scr->area.x_offset = left;
    scr->area.y_offset = top;
}

static inline void _dgx_move_to_next_area_pixel_v(dgx_screen_t *_scr)
{
    dgx_vscreen_t *scr = (dgx_vscreen_t *)_scr;
    if (scr->area.x_offset >= scr->area.right) {
        scr->area.x_offset = scr->area.left;
        if (scr->area.y_offset >= scr->area.bottom) {
            scr->area.y_offset = scr->area.top;
        } else {
            scr->area.y_offset++;
        }
    } else {
        scr->area.x_offset++;
    }
}

static void dgx_vscreen_write_area(dgx_screen_t *_scr, uint8_t *data, uint32_t lenbits)
{
    dgx_vscreen_t *scr = (dgx_vscreen_t *)_scr;
    int            idx = 0;
    /* an 18-bit pixel takes three bytes in a buffer, as it does on the wire */
    uint32_t       pixel_bits = _scr->color_bits == 18 ? 24u : _scr->color_bits;
    while (lenbits >= pixel_bits) {
        uint32_t offset = DGX_VSCR_OFFSET(scr, scr->area.x_offset, scr->area.y_offset);
        uint8_t *lp     = DGX_VSCR_PTR(scr, offset);
        uint32_t color  = dgx_read_buf_value(_scr->color_bits, &data, idx);
        ++idx;
        dgx_fill_buf_value(_scr->color_bits, lp, offset, color);
        _dgx_move_to_next_area_pixel_v(_scr);
        lenbits -= pixel_bits;
    }
}
static uint32_t dgx_vscreen_read_area(dgx_screen_t *_scr, uint8_t *data, uint32_t lenbits)
{
    uint32_t       ret = 0;
    dgx_vscreen_t *scr = (dgx_vscreen_t *)_scr;
    int            idx = 0;
    /* an 18-bit pixel takes three bytes in a buffer, as it does on the wire */
    uint32_t       pixel_bits = _scr->color_bits == 18 ? 24u : _scr->color_bits;
    while (lenbits >= pixel_bits) {
        uint32_t offset = DGX_VSCR_OFFSET(scr, scr->area.x_offset, scr->area.y_offset);
        uint8_t *lp     = DGX_VSCR_PTR(scr, offset);
        uint32_t color  = dgx_read_buf_value(_scr->color_bits, &lp, offset);
        data            = dgx_fill_buf_value(_scr->color_bits, data, idx, color);
        ++idx;
        _dgx_move_to_next_area_pixel_v(_scr);
        ret += pixel_bits;
        lenbits -= pixel_bits;
    }
    return ret;
}

static void dgx_vscreen_set_pixel(dgx_screen_t *_scr, int x, int y, uint32_t color)
{
    dgx_vscreen_t *scr = (dgx_vscreen_t *)_scr;
    if (x < 0 || y < 0 || x >= _scr->width || y >= _scr->height) return;
    uint32_t offset = DGX_VSCR_OFFSET(scr, x, y);
    uint8_t *lp     = DGX_VSCR_PTR(scr, offset);
    dgx_fill_buf_value(_scr->color_bits, lp, offset, color);
    dgx_screen_touch(_scr, x, x, y, y);
}

static uint32_t dgx_vscreen_get_pixel(dgx_screen_t *_scr, int x, int y)
{
    dgx_vscreen_t *scr = (dgx_vscreen_t *)_scr;
    if (x < 0 || y < 0 || x >= _scr->width || y >= _scr->height) return 0;
    uint32_t offset = DGX_VSCR_OFFSET(scr, x, y);
    uint8_t *lp     = DGX_VSCR_PTR(scr, offset);
    uint32_t color  = dgx_read_buf_value(_scr->color_bits, &lp, offset);
    return color;
}

bool dgx_vscreen_is_linear(const dgx_screen_t *scr)
{
    return scr && scr->get_pixel == dgx_vscreen_get_pixel;
}

static void dgx_vscreen_update_area(dgx_screen_t *_scr, int left, int right, int top, int bottom)
{
}
static void dgx_vscreen_wait_buffer_dummy(dgx_screen_t *_scr)
{
}
static void dgx_vscreen_screen_destroy(dgx_screen_t **_pscr)
{
    if (*_pscr) {
        dgx_vscreen_t *scr = (dgx_vscreen_t *)(*_pscr);
        free(scr->v_array);
        free(*_pscr);
        *_pscr = 0;
    }
}

static void dgx_vscreen_fill_rectangle(dgx_screen_t *_scr, int x, int y, int w, int h, uint32_t color)
{
    dgx_vscreen_t *scr = (dgx_vscreen_t *)_scr;
    /*
     * A row and a column first. Lines and circles are made of them, and for
     * one of a few pixels the checks a rectangle needs cost more than the
     * pixels; a column taken as rows of one pixel costs several times more.
     * Depths where pixels share bytes go the common way.
     */
    const uint8_t bits = _scr->color_bits;
    if ((h == 1 || w == 1) && bits >= 8 && bits != 12) {
        const size_t bytes = bits == 18 ? 3u : bits / 8u;
        const size_t pitch = (size_t)_scr->width * bytes;
        if (h == 1) {
            if (y < 0 || y >= _scr->height) return;
            if (x < 0) {
                w += x;
                x = 0;
            }
            if (x + w > _scr->width) w = _scr->width - x;
            if (w <= 0) return;
            uint8_t *lp = scr->v_array + (size_t)y * pitch + (size_t)x * bytes;
            if (bits == 8) {
                memset(lp, (int)color, (size_t)w);
            } else if (bits != 16) {
                dgx_fill_pixels(bits, lp, 0, (uint32_t)w, color);
            } else if (w < 8) {
                const uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)color;
                for (int k = 0; k < w; ++k, lp += 2) lp[0] = hi, lp[1] = lo;
            } else {
                dgx_fill_pixels_16(lp, (uint32_t)w, color);
            }
            dgx_screen_touch(_scr, x, x + w - 1, y, y);
            return;
        }
        if (x < 0 || x >= _scr->width) return;
        if (y < 0) {
            h += y;
            y = 0;
        }
        if (y + h > _scr->height) h = _scr->height - y;
        if (h <= 0) return;
        uint8_t *lp = scr->v_array + (size_t)y * pitch + (size_t)x * bytes;
        switch (bytes) {
        case 1:
            for (int k = 0; k < h; ++k, lp += pitch) lp[0] = (uint8_t)color;
            break;
        case 2:
            for (int k = 0; k < h; ++k, lp += pitch) lp[0] = (uint8_t)(color >> 8), lp[1] = (uint8_t)color;
            break;
        case 3:
            for (int k = 0; k < h; ++k, lp += pitch) lp[0] = (uint8_t)(color >> 16), lp[1] = (uint8_t)(color >> 8), lp[2] = (uint8_t)color;
            break;
        default:
            for (int k = 0; k < h; ++k, lp += pitch) dgx_fill_buf_value_32(lp, 0, color);
            break;
        }
        dgx_screen_touch(_scr, x, x, y, y + h - 1);
        return;
    }
    if (w < 0 || x + w < 0 || x >= _scr->width || h < 0 || y + h < 0 || y >= _scr->height) return;
    if (x < 0) {
        w = x + w;
        x = 0;
    }
    if (x + w > _scr->width) {
        w = _scr->width - x;
    }
    if (y < 0) {
        h = y + h;
        y = 0;
    }
    if (y + h > _scr->height) {
        h = _scr->height - y;
    }
    if (h <= 0 || w <= 0) return;
    if (x == 0 && w == _scr->width && y == 0 && h == _scr->height && color == 0) {
        uint32_t asize = dgx_color_points_to_bytes(_scr->color_bits, _scr->width * _scr->height);
        memset(scr->v_array, 0, asize);
    } else if (_scr->color_bits == 16 && w < 8) {
        /*
         * Short rows: the runs of a line, the caps of a circle, a column. Two bytes a pixel
         * set in place cost less than a fill by words set up for each row.
         */
        const uint8_t hi    = (uint8_t)(color >> 8), lo = (uint8_t)color;
        const size_t  pitch = (size_t)_scr->width * 2u;
        uint8_t      *row   = scr->v_array + (size_t)y * pitch + (size_t)x * 2u;
        for (int i = 0; i < h; ++i, row += pitch) {
            uint8_t *lp = row;
            for (int k = 0; k < w; ++k, lp += 2) lp[0] = hi, lp[1] = lo;
        }
    } else if (_scr->color_bits == 16) {
        /* longer rows by words, all the rows in one call */
        const size_t pitch = (size_t)_scr->width * 2u;
        dgx_fill_rows_16(scr->v_array + (size_t)y * pitch + (size_t)x * 2u, pitch, (uint32_t)w, (uint32_t)h, color);
    } else {
        for (int i = 0; i < h; ++i) {
            uint32_t offset = DGX_VSCR_OFFSET(scr, x, y + i);
            if (_scr->color_bits == 1) {
                dgx_fill_bits_msb(scr->v_array, offset, (size_t)w, color != 0);
                continue;
            }
            uint8_t *lp = DGX_VSCR_PTR(scr, offset);
            if (_scr->color_bits == 8) {
                memset(lp, (int)color, (size_t)w); // a byte a pixel: nothing to work out
                continue;
            }
            dgx_fill_pixels(_scr->color_bits, lp, offset, (uint32_t)w, color);
        }
    }
    dgx_screen_touch(_scr, x, x + w - 1, y, y + h - 1);
}
// Initialize the display
dgx_screen_t *dgx_vscreen_init(int width, int height, uint8_t color_bits, dgx_color_order_t cbo)
{
    // Attach the LCD to the SPI bus
    dgx_vscreen_t *scr = (dgx_vscreen_t *)calloc(1, sizeof(dgx_vscreen_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return 0;
    }
    scr->base.cg_row_shift    = 0;
    scr->base.cg_col_shift    = 0;
    scr->base.width           = width;
    scr->base.height          = height;
    scr->base.color_bits      = color_bits;
    scr->base.rgb_order       = cbo;
    scr->base.dir_x           = DgxScreenLeftRight;
    scr->base.dir_y           = DgxScreenTopBottom;
    scr->base.swap_xy         = false;
    scr->base.screen_name     = "V-SCREEN";
    scr->base.screen_submodel = 0;
    // must be set before dgx_scr_init_slow_bus_optimized_funcs(), which takes subtype 0 for a screen with a bus
    scr->base.screen_subtype  = DgxVirtualScreen;
    // lines and circles are those of every screen, made of the pixels and the rectangles of this one
    dgx_scr_init_slow_bus_optimized_funcs(&scr->base);
    scr->base.destroy         = dgx_vscreen_screen_destroy;
    scr->base.set_pixel       = dgx_vscreen_set_pixel;
    scr->base.fill_rectangle  = dgx_vscreen_fill_rectangle;
    scr->base.get_pixel       = dgx_vscreen_get_pixel;
    scr->base.set_area        = dgx_vscreen_set_area;
    scr->base.read_area       = dgx_vscreen_read_area;
    scr->base.write_area      = dgx_vscreen_write_area;
    scr->base.wait_buffer     = dgx_vscreen_wait_buffer_dummy;
    scr->base.update_screen   = dgx_vscreen_update_area;

    uint32_t asize = dgx_color_points_to_bytes(color_bits, width * height);
    scr->v_array   = calloc(asize, 1);
    if (!scr->v_array) {
        ESP_LOGE(TAG, "Impossible to allocation memory");
        free(scr);
        return 0;
    }
    return (dgx_screen_t *)scr;
}

bool dgx_vscreen_copy(dgx_screen_t *_scr_dst, dgx_screen_t *_scr_src)
{
    if (_scr_src->height != _scr_dst->height || _scr_src->width != _scr_dst->width || _scr_src->color_bits != _scr_dst->color_bits) return false;
    if (_scr_src->screen_subtype != DgxVirtualScreen && _scr_src->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_copy: source is not virtual");
        return false;
    }
    if (_scr_dst->screen_subtype != DgxVirtualScreen && _scr_dst->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_copy: destination is not virtual");
        return false;
    }
    if (!dgx_vscreen_is_linear(_scr_src) || !dgx_vscreen_is_linear(_scr_dst)) {
        ESP_LOGE(TAG, "dgx_vscreen_copy: screens with a non-linear buffer layout are not supported");
        return false;
    }
    dgx_vscreen_t *scr_dst = (dgx_vscreen_t *)_scr_dst;
    dgx_vscreen_t *scr_src = (dgx_vscreen_t *)_scr_src;
    if (!scr_dst->v_array || !scr_src->v_array) return false;
    uint32_t asize = dgx_color_points_to_bytes(_scr_src->color_bits, _scr_src->width * _scr_src->height);
    memcpy(scr_dst->v_array, scr_src->v_array, asize);
    dgx_screen_touch(_scr_dst, 0, _scr_dst->width - 1, 0, _scr_dst->height - 1);
    return true;
}

dgx_screen_t *dgx_vscreen_clone(dgx_screen_t *_scr_src)
{
    if (_scr_src->screen_subtype != DgxVirtualScreen && _scr_src->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_clone: source is not virtual");
        return 0;
    }
    if (!dgx_vscreen_is_linear(_scr_src)) {
        ESP_LOGE(TAG, "dgx_vscreen_clone: screens with a non-linear buffer layout are not supported");
        return 0;
    }
    dgx_screen_t *_scr = dgx_vscreen_init(_scr_src->width, _scr_src->height, _scr_src->color_bits, _scr_src->rgb_order);
    if (!_scr) return 0;
    dgx_vscreen_t *scr_src = (dgx_vscreen_t *)_scr_src;
    dgx_vscreen_t *scr     = (dgx_vscreen_t *)_scr;
    uint32_t       csize   = dgx_color_points_to_bytes(_scr_src->color_bits, _scr_src->width * _scr_src->height);
    memcpy(scr->v_array, scr_src->v_array, csize);
    return _scr;
}

/* a channel of n bits widened to 8, its high bits repeated below: the largest value becomes 0xff */
static inline uint32_t dgx_widen_channel(uint32_t c, unsigned n)
{
    return (c << (8 - n)) | (c >> (2 * n - 8));
}

static inline uint32_t dgx_argb_of_16(uint32_t v)
{
    return 0xff000000u | dgx_widen_channel((v >> 11) & 0x1f, 5) << 16 | dgx_widen_channel((v >> 5) & 0x3f, 6) << 8
           | dgx_widen_channel(v & 0x1f, 5);
}

static inline uint32_t dgx_argb_of(uint8_t color_bits, uint32_t v, const uint16_t *lut)
{
    switch (color_bits) {
    case 1:
        return v ? 0xffffffffu : 0xff000000u;
    case 4:
        v >>= 4; /* the index is read in the high bits */
        return lut ? dgx_argb_of_16(lut[v]) : 0xff000000u | (v * 0x11u) * 0x010101u;
    case 8:
        return lut ? dgx_argb_of_16(lut[v & 0xff]) : 0xff000000u | (v & 0xff) * 0x010101u;
    case 12:
        return 0xff000000u | ((v >> 12) & 0xf) * 0x110000u | ((v >> 8) & 0xf) * 0x1100u | ((v >> 4) & 0xf) * 0x11u;
    case 16:
        return dgx_argb_of_16(v);
    case 18:
        return 0xff000000u | dgx_widen_channel((v >> 18) & 0x3f, 6) << 16 | dgx_widen_channel((v >> 10) & 0x3f, 6) << 8
               | dgx_widen_channel((v >> 2) & 0x3f, 6);
    default:
        return 0xff000000u | (v & 0xffffffu);
    }
}

bool dgx_vscreen_to_argb8888(dgx_screen_t *_scr, int x, int y, int width, int height, const uint16_t *lut, uint32_t *dst,
                             int dst_stride)
{
    if (!_scr || !dst || width < 0 || height < 0 || dst_stride < width) return false;
    if (x < 0 || y < 0 || x + width > _scr->width || y + height > _scr->height) return false;
    const uint8_t bits = _scr->color_bits;
    if (dgx_vscreen_is_linear(_scr)) {
        dgx_vscreen_t *scr = (dgx_vscreen_t *)_scr;
        for (int j = 0; j < height; ++j, dst += dst_stride) {
            uint32_t offset = DGX_VSCR_OFFSET(scr, x, y + j);
            uint8_t *lp     = DGX_VSCR_PTR(scr, offset);
            if (bits == 16) {
                for (int i = 0; i < width; ++i, lp += 2) dst[i] = dgx_argb_of_16((uint32_t)lp[0] << 8 | lp[1]);
                continue;
            }
            for (int i = 0; i < width; ++i) dst[i] = dgx_argb_of(bits, dgx_read_buf_value(bits, &lp, (int)offset + i), lut);
        }
        return true;
    }
    if (!dgx_screen_is_readable(_scr)) return false;
    for (int j = 0; j < height; ++j, dst += dst_stride)
        for (int i = 0; i < width; ++i) dst[i] = dgx_argb_of(bits, dgx_get_pixel(_scr, x + i, y + j), lut);
    return true;
}
