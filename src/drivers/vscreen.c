#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_arch.h"
#include "dgx_bits.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/vscreen.h"
#include "../dgx_indexed_priv.h"
#include "../dgx_overlay_priv.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* dgx_vscreen_t is defined publicly in drivers/vscreen.h */

/* Forward declarations for orientation helpers defined later in this file. */
typedef void (*dgx_vscreen_oriented_blit_func_t)(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src,
                                                 int x_src, int y_src, int width, int height, bool has_transparency);
static inline bool dgx_vscreen_output_is_canonical(dgx_output_orientation_t orientation);
static inline int  dgx_vscreen_output_width(int width, int height, dgx_output_orientation_t orientation);
static inline int  dgx_vscreen_output_height(int width, int height, dgx_output_orientation_t orientation);
static dgx_vscreen_oriented_blit_func_t dgx_vscreen_output_blit_func(dgx_output_orientation_t orientation);

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

#define DGX_VSCR_OFFSET(vscr, x, y) ((x) + (y) * (vscr)->base.width)
#define DGX_VSCR_PTR(vscr, offset) \
    ((vscr)->v_array + ((vscr)->base.color_bits < 8u     ? (offset) / (8u / (vscr)->base.color_bits) \
                        : (vscr)->base.color_bits == 12u ? (offset) * 3u / 2u                        \
                                                         : (offset) * (((vscr)->base.color_bits + 7u) / 8u)))

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

/* Interscreen operatiopns */

// big endian
#define _DGX_READ_COLOR(bits, ptr)                                                                                   \
    (((bits) == 8    ? (uint32_t) * (ptr)                                                                            \
      : (bits) == 16 ? ((uint32_t)(ptr)[1] | ((uint32_t)(ptr)[0] << 8))                                              \
                     : ((bits) == 24 ? ((uint32_t)(ptr)[2] | ((uint32_t)(ptr)[1] << 8) | ((uint32_t)(ptr)[0] << 16)) \
                                     : ((uint32_t)(ptr)[3] | ((uint32_t)(ptr)[2] << 8) | ((uint32_t)(ptr)[1] << 16) | ((uint32_t)(ptr)[0] << 24)))))

// big endian
#define _DGX_WRITE_COLOR(bits, ptr, color)        \
    do {                                          \
        switch (bits) {                           \
        case 8:                                   \
            (ptr)[0] = (uint8_t)(color);          \
            break;                                \
        case 16:                                  \
            (ptr)[1] = (uint8_t)(color);          \
            (ptr)[0] = (uint8_t)((color) >> 8u);  \
            break;                                \
        case 24:                                  \
            (ptr)[2] = (uint8_t)(color);          \
            (ptr)[1] = (uint8_t)((color) >> 8u);  \
            (ptr)[0] = (uint8_t)((color) >> 16u); \
            break;                                \
        default:                                  \
            (ptr)[3] = (uint8_t)(color);          \
            (ptr)[2] = (uint8_t)((color) >> 8u);  \
            (ptr)[1] = (uint8_t)((color) >> 16u); \
            (ptr)[0] = (uint8_t)((color) >> 24u); \
            break;                                \
        }                                         \
    } while (0)

/*
 * A display behind a bus that has a faster way than its window takes a
 * rectangle of a virtual screen as it lies in memory: whole bytes a pixel,
 * the same depth on both sides, the rectangle inside both screens. Returns
 * false when the pixels have to go through the window.
 */
static bool dgx_vscreen_rows_to_bus(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_vscreen_t *scr_src, int x_src, int y_src, int width, int height)
{
    if (_scr_dst->screen_subtype != DgxPhysicalScreenWithBus || _scr_dst->color_bits % 8u != 0) return false;
    dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)_scr_dst)->bus;
    if (!bus->write_rows) return false;
    const uint32_t bip = dgx_color_points_to_bytes(scr_src->base.color_bits, 1);
    return bus->write_rows(bus, (uint16_t)x_dst, (uint16_t)y_dst, (uint16_t)width, (uint16_t)height,
                           DGX_VSCR_PTR(scr_src, DGX_VSCR_OFFSET(scr_src, x_src, y_src)), (uint32_t)scr_src->base.width * bip);
}

/*
 * The transfer buffer in two halves: while one goes to the display, the
 * pixels of the next are put into the other. Sending a piece returns once
 * the pieces before it have gone, so a half is never written while it is
 * being sent; a bus that queues the piece before it waits, as SPI does, goes
 * on to the next half with no pause. A half is of whole pixels, and of
 * whole rows where a row fits in it: a row is then not cut, and a display
 * whose window goes with its pixels is not told a window for the rest of a
 * row. A rectangle the whole buffer takes goes in one piece: a second one
 * would cost a transfer of its own. A buffer too small for two halves of a
 * pixel each is one half, and every piece of it is waited for.
 */
typedef struct {
    dgx_screen_t *scr;
    uint8_t      *half[2];
    uint8_t      *at;    /* where the next byte goes */
    uint32_t      len;   /* bytes of a half */
    int           which; /* the half being filled */
    bool          alone; /* one half only */
} dgx_vscreen_halves_t;

static inline void dgx_vscreen_halves_begin(dgx_vscreen_halves_t *h, dgx_screen_t *scr, uint8_t *buffer, size_t buffer_len, uint32_t pixel_bytes,
                                            uint32_t row_bytes, uint32_t all_bytes)
{
    uint32_t len = (uint32_t)(buffer_len / 2u) / pixel_bytes * pixel_bytes;
    h->alone     = false;
    if (all_bytes <= buffer_len) {
        len = all_bytes;
    } else {
        if (len == 0) len = (uint32_t)buffer_len / pixel_bytes * pixel_bytes, h->alone = true;
        if (len >= row_bytes) len = len / row_bytes * row_bytes;
    }
    h->scr     = scr;
    h->half[0] = h->at = buffer;
    h->half[1]         = h->alone ? buffer : buffer + len;
    h->len             = len;
    h->which           = 0;
}

/* bytes left in the half being filled */
static inline uint32_t dgx_vscreen_halves_room(const dgx_vscreen_halves_t *h)
{
    return h->len - (uint32_t)(h->at - h->half[h->which]);
}

/* the half filled goes to the display, the other one is filled next */
static inline void dgx_vscreen_halves_send(dgx_vscreen_halves_t *h)
{
    h->scr->write_area(h->scr, h->half[h->which], 8u * (uint32_t)(h->at - h->half[h->which]));
    if (h->alone) h->scr->wait_buffer(h->scr);
    h->which ^= 1;
    h->at = h->half[h->which];
}

/* what is left goes, and all of it has gone when this returns */
static inline void dgx_vscreen_halves_end(dgx_vscreen_halves_t *h)
{
    if (h->at != h->half[h->which]) dgx_vscreen_halves_send(h);
    h->scr->wait_buffer(h->scr);
}

/*
 * A run of whole pixels put into the halves. A virtual screen keeps its
 * pixels in the byte order of the wire, so this is a plain copy.
 */
static void dgx_vscreen_send_row(dgx_vscreen_halves_t *h, const uint8_t *src, uint32_t bytes)
{
    while (bytes) {
        const uint32_t room  = dgx_vscreen_halves_room(h);
        const uint32_t chunk = bytes < room ? bytes : room;
        memcpy(h->at, src, chunk);
        h->at += chunk;
        src += chunk;
        bytes -= chunk;
        if (chunk == room) dgx_vscreen_halves_send(h);
    }
}

/*
 * A row of 16-bit pixels into the halves, the overlay layer of the screen
 * mixed in on the way: the pixels from x on of the row y of the screen.
 */
static void dgx_vscreen_send_row_over(dgx_vscreen_halves_t *h, const uint8_t *src, uint32_t bytes, const dgx_bus_overlay_t *overlay, int x, int y)
{
    while (bytes) {
        const uint32_t room  = dgx_vscreen_halves_room(h);
        const uint32_t chunk = bytes < room ? bytes : room;
        memcpy(h->at, src, chunk);
        dgx_overlay_row(overlay, h->at, x, y, (int)(chunk / 2u));
        x += (int)(chunk / 2u);
        h->at += chunk;
        src += chunk;
        bytes -= chunk;
        if (chunk == room) dgx_vscreen_halves_send(h);
    }
}

void dgx_vscreen_to_vscreen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, bool has_transparency)
{
    if (_scr_src->screen_subtype != DgxVirtualScreen && _scr_src->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_to_vscreen: source is not virtual");
        return;
    }
    if (_scr_dst->screen_subtype != DgxVirtualScreen && _scr_dst->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_to_vscreen: destination is not virtual");
        return;
    }
    dgx_vscreen_t *scr_dst = (dgx_vscreen_t *)_scr_dst;
    dgx_vscreen_t *scr_src = (dgx_vscreen_t *)_scr_src;
    if (_scr_dst->color_bits != _scr_src->color_bits) {
        ESP_LOGE(TAG, "vscreen_to_vscreen: color bits for source and destination arn't equal");
        return;
    }
    int x1     = x_dst;
    int y1     = y_dst;
    int bw     = _scr_src->width;
    int bh     = _scr_src->height;
    int skip_x = 0;
    int skip_y = 0;
    if (x1 < 0) {
        skip_x = -x1;
        bw += x1;
        x1 = 0;
    }
    if (y1 < 0) {
        skip_y = -y1;
        bh += y1;
        y1 = 0;
    }
    if (x1 + bw <= 0 || y1 + bh <= 0) return;
    if (x1 + bw > _scr_dst->width) {
        bw = _scr_dst->width - x1;
    }
    if (y1 + bh > _scr_dst->height) {
        bh = _scr_dst->height - y1;
    }
    if (bh <= 0 || bw <= 0) return;
    dgx_screen_progress_up(_scr_dst);
    uint32_t csize = dgx_color_points_to_bytes(_scr_dst->color_bits, bw);
    for (int br = 0; br < bh; ++br) {
        if (_scr_dst->color_bits % 8u == 0) {
            uint32_t offset_src = DGX_VSCR_OFFSET(scr_src, skip_x, skip_y + br);
            uint8_t *lp_src     = DGX_VSCR_PTR(scr_src, offset_src);
            uint32_t offset_dst = DGX_VSCR_OFFSET(scr_dst, x1, y1 + br);
            uint8_t *lp_dst     = DGX_VSCR_PTR(scr_dst, offset_dst);
            if (!has_transparency) {
                memcpy(lp_dst, lp_src, csize);
            } else {
                uint32_t psize = dgx_color_points_to_bytes(_scr_dst->color_bits, 1);
                for (int bc = 0; bc < bw; ++bc) {
                    uint8_t *src_ptr = lp_src + bc * psize;
                    uint32_t color   = _DGX_READ_COLOR(_scr_dst->color_bits, src_ptr);
                    if (color || !has_transparency) {
                        uint8_t *dst_ptr = lp_dst + bc * psize;
                        _DGX_WRITE_COLOR(_scr_dst->color_bits, dst_ptr, color);
                    }
                }
            }
        } else {
            for (int bc = 0; bc < bw; ++bc) {
                uint32_t color = _scr_src->get_pixel(_scr_src, skip_x + bc, skip_y + br);
                if (color || !has_transparency) _scr_dst->set_pixel(_scr_dst, x1 + bc, y1 + br, color);
            }
        }
    }
    dgx_screen_touch(_scr_dst, x1, x1 + bw - 1, y1, y1 + bh - 1);
    dgx_screen_progress_down(_scr_dst);
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

void dgx_vscreen_to_vscreen_oriented(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, bool has_transparency,
                                     dgx_output_orientation_t orientation)
{
    if (dgx_vscreen_output_is_canonical(orientation)) {
        dgx_vscreen_to_vscreen(_scr_dst, x_dst, y_dst, _scr_src, has_transparency);
        return;
    }
    if (_scr_src->screen_subtype != DgxVirtualScreen && _scr_src->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_to_vscreen: source is not virtual");
        return;
    }
    if (_scr_dst->screen_subtype != DgxVirtualScreen && _scr_dst->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_to_vscreen: destination is not virtual");
        return;
    }
    if (_scr_dst->color_bits != _scr_src->color_bits) {
        ESP_LOGE(TAG, "vscreen_to_vscreen: color bits for source and destination arn't equal");
        return;
    }

    dgx_vscreen_oriented_blit_func_t blit_func = dgx_vscreen_output_blit_func(orientation);
    if (!blit_func) {
        ESP_LOGE(TAG, "dgx_vscreen_to_vscreen: unknown output orientation %d", (int)orientation);
        return;
    }

    int out_width  = dgx_vscreen_output_width(_scr_src->width, _scr_src->height, orientation);
    int out_height = dgx_vscreen_output_height(_scr_src->width, _scr_src->height, orientation);
    int left       = DGX_MAX(0, x_dst);
    int top        = DGX_MAX(0, y_dst);
    int right      = DGX_MIN(_scr_dst->width - 1, x_dst + out_width - 1);
    int bottom     = DGX_MIN(_scr_dst->height - 1, y_dst + out_height - 1);
    if (right < left || bottom < top) return;

    dgx_screen_progress_up(_scr_dst);
    blit_func(_scr_dst, x_dst, y_dst, _scr_src, 0, 0, _scr_src->width, _scr_src->height, has_transparency);
    dgx_screen_touch(_scr_dst, left, right, top, bottom);
    dgx_screen_progress_down(_scr_dst);
}

#define DGX_INTERSECT_RECTANGLES(xo, yo, wo, ho, xb, yb, wb, hb) \
    do {                                                         \
        int xc = DGX_MAX(xo, xb);                                \
        int yc = DGX_MAX(yo, yb);                                \
        wo += xo - xc;                                           \
        ho += yo - yc;                                           \
        int rx = DGX_MIN(xc + wo, xb + wb);                      \
        int ry = DGX_MIN(yc + ho, yb + hb);                      \
        wo     = rx - xc;                                        \
        ho     = ry - yc;                                        \
        xo     = xc;                                             \
        yo     = yc;                                             \
    } while (0)

static inline bool dgx_vscreen_output_is_canonical(dgx_output_orientation_t orientation)
{
    return orientation == DgxOutputNormal;
}

static inline bool dgx_vscreen_output_is_swapped(dgx_output_orientation_t orientation)
{
    return orientation == DgxOutputTranspose || orientation == DgxOutputRotate90CCW || orientation == DgxOutputRotate90CW ||
           orientation == DgxOutputTransverse;
}

static inline int dgx_vscreen_output_width(int width, int height, dgx_output_orientation_t orientation)
{
    return dgx_vscreen_output_is_swapped(orientation) ? height : width;
}

static inline int dgx_vscreen_output_height(int width, int height, dgx_output_orientation_t orientation)
{
    return dgx_vscreen_output_is_swapped(orientation) ? width : height;
}

#define DGX_VSCREEN_DEFINE_ORIENTED_BLIT(name, dx_expr, dy_expr)                                                                                \
    static void name(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int src_width, int src_height, \
                     bool has_transparency)                                                                                                     \
    {                                                                                                                                           \
        for (int sy = 0; sy < src_height; ++sy) {                                                                                               \
            for (int sx = 0; sx < src_width; ++sx) {                                                                                            \
                int dx = x_dst + (dx_expr);                                                                                                     \
                int dy = y_dst + (dy_expr);                                                                                                     \
                if (dx < 0 || dy < 0 || dx >= _scr_dst->width || dy >= _scr_dst->height) continue;                                              \
                uint32_t color = _scr_src->get_pixel(_scr_src, x_src + sx, y_src + sy);                                                         \
                if (has_transparency && !color) continue;                                                                                       \
                _scr_dst->set_pixel(_scr_dst, dx, dy, color);                                                                                   \
            }                                                                                                                                   \
        }                                                                                                                                       \
    }

DGX_VSCREEN_DEFINE_ORIENTED_BLIT(dgx_vscreen_region_to_screen_normal, sx, sy);
DGX_VSCREEN_DEFINE_ORIENTED_BLIT(dgx_vscreen_region_to_screen_mirror_x, src_width - 1 - sx, sy);
DGX_VSCREEN_DEFINE_ORIENTED_BLIT(dgx_vscreen_region_to_screen_mirror_y, sx, src_height - 1 - sy);
DGX_VSCREEN_DEFINE_ORIENTED_BLIT(dgx_vscreen_region_to_screen_rotate_180, src_width - 1 - sx, src_height - 1 - sy);
DGX_VSCREEN_DEFINE_ORIENTED_BLIT(dgx_vscreen_region_to_screen_transpose, sy, sx);
DGX_VSCREEN_DEFINE_ORIENTED_BLIT(dgx_vscreen_region_to_screen_rotate_90_ccw, sy, src_width - 1 - sx);
DGX_VSCREEN_DEFINE_ORIENTED_BLIT(dgx_vscreen_region_to_screen_rotate_90_cw, src_height - 1 - sy, sx);
DGX_VSCREEN_DEFINE_ORIENTED_BLIT(dgx_vscreen_region_to_screen_transverse, src_height - 1 - sy, src_width - 1 - sx);

#undef DGX_VSCREEN_DEFINE_ORIENTED_BLIT

static dgx_vscreen_oriented_blit_func_t dgx_vscreen_output_blit_func(dgx_output_orientation_t orientation)
{
    switch (orientation) {
    case DgxOutputNormal:
        return dgx_vscreen_region_to_screen_normal;
    case DgxOutputMirrorX:
        return dgx_vscreen_region_to_screen_mirror_x;
    case DgxOutputMirrorY:
        return dgx_vscreen_region_to_screen_mirror_y;
    case DgxOutputRotate180:
        return dgx_vscreen_region_to_screen_rotate_180;
    case DgxOutputTranspose:
        return dgx_vscreen_region_to_screen_transpose;
    case DgxOutputRotate90CCW:
        return dgx_vscreen_region_to_screen_rotate_90_ccw;
    case DgxOutputRotate90CW:
        return dgx_vscreen_region_to_screen_rotate_90_cw;
    case DgxOutputTransverse:
        return dgx_vscreen_region_to_screen_transverse;
    default:
        return NULL;
    }
}

void dgx_vscreen_to_screen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src)
{
    dgx_vscreen_t *scr_src = (dgx_vscreen_t *)_scr_src;
    if (_scr_dst->color_bits != _scr_src->color_bits) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: source color bits %d != destination %d", _scr_src->color_bits, _scr_dst->color_bits);
        return;
    }
    if (_scr_src->screen_subtype != DgxVirtualScreen && _scr_src->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: source is not virtual");
        return;
    }
    int x1 = x_dst;
    int y1 = y_dst;
    int bw = _scr_src->width;
    int bh = _scr_src->height;
    DGX_INTERSECT_RECTANGLES(x1, y1, bw, bh, 0, 0, _scr_dst->width, _scr_dst->height);
    if (bh <= 0 || bw <= 0) return;
    int skip_x = x1 - x_dst;
    int skip_y = y1 - y_dst;
    /* a layer over the rectangle is mixed in on the way: not where the bus would take the rows as they lie */
    const dgx_bus_overlay_t *over = _scr_dst->color_bits == 16 ? dgx_overlay_over(_scr_dst, x1, y1, x1 + bw - 1, y1 + bh - 1) : NULL;
    dgx_screen_progress_up(_scr_dst);
    if (!over && dgx_vscreen_rows_to_bus(_scr_dst, x1, y1, scr_src, skip_x, skip_y, bw, bh)) {
        /* the bus has taken the pixels as they lie in memory */
    } else if (_scr_dst->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_bus_protocols_t *bus             = ((dgx_screen_with_bus_t *)_scr_dst)->bus;
        uint8_t             *draw_buffer     = bus->buffer;
        size_t               draw_buffer_len = bus->buffer_len;
        _scr_dst->set_area(_scr_dst, x1, x1 + bw - 1, y1, y1 + bh - 1);
        _scr_dst->wait_buffer(_scr_dst);
        uint32_t             bip = dgx_color_points_to_bytes(_scr_src->color_bits, 1);
        dgx_vscreen_halves_t halves;
        dgx_vscreen_halves_begin(&halves, _scr_dst, draw_buffer, draw_buffer_len, bip, (uint32_t)bw * bip, (uint32_t)bw * bip * (uint32_t)bh);
        for (int br = 0; br < bh; ++br) {
            if (_scr_dst->color_bits % 8u == 0) {
                uint32_t offset_src = DGX_VSCR_OFFSET(scr_src, skip_x, skip_y + br);
                uint8_t *lp_src     = DGX_VSCR_PTR(scr_src, offset_src);
                if (over) dgx_vscreen_send_row_over(&halves, lp_src, (uint32_t)bw * bip, over, x1, y1 + br);
                else dgx_vscreen_send_row(&halves, lp_src, (uint32_t)bw * bip);
            } else {
                for (int bc = 0; bc < bw; ++bc) {
                    uint32_t color = _scr_src->get_pixel(_scr_src, skip_x + bc, skip_y + br);
                    _scr_dst->set_pixel(_scr_dst, x1 + bc, y1 + br, color);
                }
            }
        }
        dgx_vscreen_halves_end(&halves);
    } else if (_scr_dst->screen_subtype == DgxVirtualScreen || _scr_dst->screen_subtype == DgxVirtualBackScreen) {
        dgx_vscreen_t *scr_dst = (dgx_vscreen_t *)_scr_dst;
        uint32_t       bip     = dgx_color_points_to_bytes(_scr_src->color_bits, 1);
        for (int br = 0; br < bh; ++br) {
            if (_scr_dst->color_bits % 8u == 0) {
                uint32_t offset_src = DGX_VSCR_OFFSET(scr_src, skip_x, skip_y + br);
                uint8_t *lp_src     = DGX_VSCR_PTR(scr_src, offset_src);
                uint32_t offset_dst = DGX_VSCR_OFFSET(scr_dst, x1, y1 + br);
                uint8_t *lp_dst     = DGX_VSCR_PTR(scr_dst, offset_dst);
                for (int bc = 0; bc < bw; ++bc) {
                    uint32_t color = _DGX_READ_COLOR(_scr_src->color_bits, lp_src);
                    if (over) color = dgx_overlay_pixel(over, x1 + bc, y1 + br, (uint16_t)color);
                    _DGX_WRITE_COLOR(_scr_dst->color_bits, lp_dst, color);
                    lp_src += bip;
                    lp_dst += bip;
                }
            } else {
                for (int bc = 0; bc < bw; ++bc) {
                    uint32_t color = _scr_src->get_pixel(_scr_src, bc + skip_x, br + skip_y);
                    _scr_dst->set_pixel(_scr_dst, x1 + bc, y1 + br, color);
                }
            }
        }
    }
    dgx_screen_touch(_scr_dst, x1, x1 + bw - 1, y1, y1 + bh - 1);
    dgx_screen_progress_down(_scr_dst);
}

void dgx_vscreen_region_to_screen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width, int height)
{
    dgx_vscreen_t *scr_src = (dgx_vscreen_t *)_scr_src;
    if (_scr_dst->color_bits != _scr_src->color_bits) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: source color bits %d != destination %d", _scr_src->color_bits, _scr_dst->color_bits);
        return;
    }
    if (_scr_src->screen_subtype != DgxVirtualScreen && _scr_src->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: source is not virtual");
        return;
    }
    /* Whatever is cut off on one side has to move the origin on the other side too. */
    int x_req = x_src;
    int y_req = y_src;
    DGX_INTERSECT_RECTANGLES(x_src, y_src, width, height, 0, 0, _scr_src->width, _scr_src->height);
    if (width <= 0 || height <= 0) return;
    x_dst += x_src - x_req;
    y_dst += y_src - y_req;
    x_req = x_dst;
    y_req = y_dst;
    DGX_INTERSECT_RECTANGLES(x_dst, y_dst, width, height, 0, 0, _scr_dst->width, _scr_dst->height);
    if (width <= 0 || height <= 0) return;
    x_src += x_dst - x_req;
    y_src += y_dst - y_req;
    /* a layer over the rectangle is mixed in on the way: not where the bus would take the rows as they lie */
    const dgx_bus_overlay_t *over = _scr_dst->color_bits == 16 ? dgx_overlay_over(_scr_dst, x_dst, y_dst, x_dst + width - 1, y_dst + height - 1) : NULL;
    if (!over && dgx_vscreen_rows_to_bus(_scr_dst, x_dst, y_dst, scr_src, x_src, y_src, width, height)) {
        /* the bus has taken the pixels as they lie in memory */
        dgx_screen_touch(_scr_dst, x_dst, x_dst + width - 1, y_dst, y_dst + height - 1);
        return;
    }
    uint8_t *draw_buffer;
    size_t   draw_buffer_len;
    if (_scr_dst->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)_scr_dst)->bus;
        draw_buffer_len          = bus->buffer_len;
        draw_buffer              = bus->buffer;
    } else {
        draw_buffer_len = 4000; // arbitrary
        draw_buffer     = calloc(draw_buffer_len, 1);
        if (!draw_buffer) {
            ESP_LOGE(TAG, "dgx_vscreen_to_screen: v-v requires %u free ram but it's impossible to allocate", draw_buffer_len);
            return;
        }
    }
    dgx_screen_progress_up(_scr_dst);
    _scr_dst->set_area(_scr_dst, x_dst, x_dst + width - 1, y_dst, y_dst + height - 1);
    _scr_dst->wait_buffer(_scr_dst);
    uint32_t             bip = dgx_color_points_to_bytes(_scr_src->color_bits, 1);
    dgx_vscreen_halves_t halves;
    dgx_vscreen_halves_begin(&halves, _scr_dst, draw_buffer, draw_buffer_len, bip, (uint32_t)width * bip, (uint32_t)width * bip * (uint32_t)height);
    for (int br = 0; br < height; ++br) {
        if (_scr_dst->color_bits % 8u == 0) {
            uint32_t offset_src = DGX_VSCR_OFFSET(scr_src, x_src, y_src + br);
            uint8_t *lp_src     = DGX_VSCR_PTR(scr_src, offset_src);
            if (over) dgx_vscreen_send_row_over(&halves, lp_src, (uint32_t)width * bip, over, x_dst, y_dst + br);
            else dgx_vscreen_send_row(&halves, lp_src, (uint32_t)width * bip);
        } else {
            for (int bc = 0; bc < width; ++bc) {
                uint32_t color = _scr_src->get_pixel(_scr_src, x_src + bc, y_src + br);
                _scr_dst->set_pixel(_scr_dst, x_dst + bc, y_dst + br, color);
            }
        }
    }
    dgx_vscreen_halves_end(&halves);
    if (_scr_dst->screen_subtype != DgxPhysicalScreenWithBus) {
        free(draw_buffer);
    }
    dgx_screen_touch(_scr_dst, x_dst, x_dst + width - 1, y_dst, y_dst + height - 1);
    dgx_screen_progress_down(_scr_dst);
}

/*
 * Is the table two colors and what lies between them? It is when every color of it is not further
 * than a step of red, of green and of blue from the straight way between its first color and its
 * last: a table a program has made for a pen with a soft edge, from the paper to the ink, however it
 * rounded. Such a table a bus with hardware that mixes two colors does not need: the first color, the
 * last one and the bytes of the screen are enough for it.
 */
static bool dgx_vscreen8_two_colors(const uint16_t *lut)
{
    static const uint8_t shift[3] = {11, 5, 0}, mask[3] = {31, 63, 31};
    for (int c = 0; c < 3; ++c) {
        const int from = (lut[0] >> shift[c]) & mask[c], way = ((lut[255] >> shift[c]) & mask[c]) - from;
        for (int a = 1; a < 255; ++a) {
            const int off = 255 * (((lut[a] >> shift[c]) & mask[c]) - from) - way * a;
            if (off > 255 || off < -255) return false;
        }
    }
    return true;
}

/* n indices made into colors of two bytes each, the high one first, as a display takes them */
static inline void dgx_vscreen8_lut_run(uint8_t *restrict to, const uint8_t *restrict ink, int n, const uint16_t *restrict lut)
{
    for (int i = 0; i < n; ++i) {
        const uint16_t color = lut[ink[i]];
        to[2 * i]            = (uint8_t)(color >> 8);
        to[2 * i + 1]        = (uint8_t)color;
    }
}

/*
 * n pixels of a row of indices made `scale` times as wide, from the pixel
 * `at` of the widened row on, into colors as dgx_vscreen8_lut_run() makes
 * them.
 */
static void dgx_vscreen8_lut_scaled(uint8_t *restrict to, const uint8_t *restrict ink, uint32_t at, int n, uint32_t scale, const uint16_t *restrict lut)
{
    if (scale == 1) {
        dgx_vscreen8_lut_run(to, ink + at, n, lut);
        return;
    }
    const uint8_t *index = ink + at / scale;
    uint32_t       run   = scale - at % scale;
    while (n > 0) {
        const int      k     = (int)run < n ? (int)run : n;
        const uint16_t color = lut[*index];
        for (int i = 0; i < k; ++i) {
            to[2 * i]     = (uint8_t)(color >> 8);
            to[2 * i + 1] = (uint8_t)color;
        }
        to += 2 * k;
        n -= k;
        ++index;
        run = scale;
    }
}

/*
 * A smoothed picture where the display has no faster way for it: a row of
 * the rectangle at a time, its indices made by dgx_indexed_line() and put
 * through the table as the destination takes them.
 */
static void dgx_vscreen8_smooth_put(dgx_screen_t *_scr_dst, int rxd, int ryd, int bw, int bh, const dgx_bus_indexed_t *picture, bool transparent)
{
    const dgx_bus_overlay_t *over = picture->overlay;
    uint8_t *line = malloc((size_t)bw + dgx_indexed_lines_scratch((uint32_t)bw));
    if (!line) {
        ESP_LOGE(TAG, "vscreen8_to_screen16: no memory for a smoothed row of %d", bw);
        return;
    }
    dgx_indexed_lines_t lines;
    dgx_indexed_lines_begin(&lines, picture, false, (int32_t)picture->x, (uint32_t)bw, line + bw);
    const uint16_t *lut = picture->lut;
    if (_scr_dst->screen_subtype == DgxPhysicalScreenWithBus && !transparent) {
        dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)_scr_dst)->bus;
        _scr_dst->set_area(_scr_dst, rxd, rxd + bw - 1, ryd, ryd + bh - 1);
        _scr_dst->wait_buffer(_scr_dst);
        const uint32_t       row_bytes = 2u * (uint32_t)bw;
        dgx_vscreen_halves_t halves;
        dgx_vscreen_halves_begin(&halves, _scr_dst, bus->buffer, bus->buffer_len, 2u, row_bytes, row_bytes * (uint32_t)bh);
        for (int br = 0; br < bh; ++br) {
            dgx_indexed_line(&lines, (int32_t)picture->y + br, line);
            for (int bc = 0; bc < bw;) {
                /* as much of the row as the half has room for, then the half is sent if it is full */
                const int room = (int)(dgx_vscreen_halves_room(&halves) / 2u);
                const int n    = bw - bc < room ? bw - bc : room;
                dgx_vscreen8_lut_run(halves.at, line + bc, n, lut);
                if (over) dgx_overlay_row(over, halves.at, rxd + bc, ryd + br, n);
                halves.at += 2 * n;
                bc += n;
                if (n == room) dgx_vscreen_halves_send(&halves);
            }
        }
        dgx_vscreen_halves_end(&halves);
    } else if (_scr_dst->screen_subtype == DgxPhysicalScreenWithBus) {
        for (int br = 0; br < bh; ++br) {
            dgx_indexed_line(&lines, (int32_t)picture->y + br, line);
            for (int bc = 0; bc < bw; ++bc) {
                if (!line[bc]) continue;
                const uint16_t color = lut[line[bc]];
                _scr_dst->set_pixel(_scr_dst, rxd + bc, ryd + br, over ? dgx_overlay_pixel(over, rxd + bc, ryd + br, color) : color);
            }
        }
    } else {
        dgx_vscreen_t *scr_dst = (dgx_vscreen_t *)_scr_dst;
        for (int br = 0; br < bh; ++br) {
            dgx_indexed_line(&lines, (int32_t)picture->y + br, line);
            uint8_t *lp_dst = DGX_VSCR_PTR(scr_dst, DGX_VSCR_OFFSET(scr_dst, rxd, ryd + br));
            for (int bc = 0; bc < bw; ++bc) {
                if (!line[bc] && transparent) continue;
                const uint16_t color = over ? dgx_overlay_pixel(over, rxd + bc, ryd + br, lut[line[bc]]) : lut[line[bc]];
                lp_dst[2 * bc]       = (uint8_t)(color >> 8);
                lp_dst[2 * bc + 1]   = (uint8_t)color;
            }
        }
    }
    free(line);
}

void dgx_vscreen8_region_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width,
                                     int height, uint16_t *lut, uint32_t flags)
{
    const bool has_transparency = (flags & DGX_LUT_TRANSPARENT) != 0;
    const int  scale            = (flags >> 8) & 0xffu ? (int)((flags >> 8) & 0xffu) : 1;
    dgx_vscreen_t *scr_src = (dgx_vscreen_t *)_scr_src;
    if (_scr_dst->color_bits != 16 || _scr_src->color_bits != 8) {
        ESP_LOGE(TAG, "vscreen8_to_screen16: source color bits %d != 8 or destination %d != 16", _scr_src->color_bits, _scr_dst->color_bits);
        return;
    }
    if (_scr_src->screen_subtype != DgxVirtualScreen && _scr_src->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "vscreen8_to_screen16: source is not virtual");
        return;
    }
    /* Whatever is cut off on one side has to move the origin on the other side too. */
    int x_req = x_src;
    int y_req = y_src;
    DGX_INTERSECT_RECTANGLES(x_src, y_src, width, height, 0, 0, _scr_src->width, _scr_src->height);
    if (width <= 0 || height <= 0) return;
    x_dst += (x_src - x_req) * scale;
    y_dst += (y_src - y_req) * scale;
    int bw  = width * scale;
    int bh  = height * scale;
    int rxd = x_dst;
    int ryd = y_dst;
    DGX_INTERSECT_RECTANGLES(rxd, ryd, bw, bh, 0, 0, _scr_dst->width, _scr_dst->height);
    if (bh <= 0 || bw <= 0) return;
    /*
     * The pixel of the destination the rectangle starts at is in the square
     * of the index (skip_x, skip_y) of the source, that many pixels of the
     * square cut off left of it and above it.
     */
    const int      skip_x = x_src + (rxd - x_dst) / scale;
    const int      skip_y = y_src + (ryd - y_dst) / scale;
    const uint32_t cut_x  = (uint32_t)((rxd - x_dst) % scale);
    const uint32_t cut_y  = (uint32_t)((ryd - y_dst) % scale);
    const uint32_t sw     = _scr_src->width;
    const uint8_t *first  = scr_src->v_array + (size_t)skip_y * sw + (size_t)skip_x;
    const bool     smooth = (flags & DGX_LUT_SMOOTH) && (scale == 2 || scale == 4);
    /* the whole source, and where the rectangle starts in it made `scale` times as large */
    const dgx_bus_indexed_t picture = {
        .pixels = scr_src->v_array,
        .pitch  = sw,
        .width  = (uint16_t)_scr_src->width,
        .height = (uint16_t)_scr_src->height,
        .x      = (uint32_t)skip_x * (uint32_t)scale + cut_x,
        .y      = (uint32_t)skip_y * (uint32_t)scale + cut_y,
        .scale  = (uint8_t)scale,
        .smooth = smooth,
        .lut    = lut,
        /* the overlay layer of the screen, when it is over the rectangle */
        .overlay = dgx_overlay_over(_scr_dst, rxd, ryd, rxd + bw - 1, ryd + bh - 1),
    };
    const dgx_bus_overlay_t *over = picture.overlay;
    /* the index the pixel (bc, br) of the rectangle has */
#define DGX_VSCREEN8_INDEX(bc, br) first[((uint32_t)(br) + cut_y) / (uint32_t)scale * sw + ((uint32_t)(bc) + cut_x) / (uint32_t)scale]
    dgx_screen_progress_up(_scr_dst);
    if (_scr_dst->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_bus_protocols_t *bus             = ((dgx_screen_with_bus_t *)_scr_dst)->bus;
        uint8_t             *draw_buffer     = bus->buffer;
        size_t               draw_buffer_len = bus->buffer_len;
        if (!has_transparency && scale == 1 && !over && !(flags & DGX_LUT_EXACT) && bus->write_ink && dgx_vscreen8_two_colors(lut) &&
            bus->write_ink(bus, (uint16_t)rxd, (uint16_t)ryd, (uint16_t)bw, (uint16_t)bh, first, sw, lut[0], lut[255])) {
            /* the bus has mixed the two colors of the table itself */
        } else if (!has_transparency && bus->write_indexed && bus->write_indexed(bus, (uint16_t)rxd, (uint16_t)ryd, (uint16_t)bw, (uint16_t)bh, &picture)) {
            /* the bus has made the colors itself and put them its own faster way */
        } else if (smooth) {
            dgx_vscreen8_smooth_put(_scr_dst, rxd, ryd, bw, bh, &picture, has_transparency);
        } else if (!has_transparency) {
            _scr_dst->set_area(_scr_dst, rxd, rxd + bw - 1, ryd, ryd + bh - 1);
            _scr_dst->wait_buffer(_scr_dst);
            /* the colors of the next half are made while the one before it goes to the display */
            const uint32_t       row_bytes = 2u * (uint32_t)bw;
            dgx_vscreen_halves_t halves;
            dgx_vscreen_halves_begin(&halves, _scr_dst, draw_buffer, draw_buffer_len, 2u, row_bytes, row_bytes * (uint32_t)bh);
            const bool whole_rows = halves.len % row_bytes == 0;
            for (int br = 0; br < bh; ++br) {
                const uint8_t *ink = first + ((uint32_t)br + cut_y) / (uint32_t)scale * sw;
                if (whole_rows) {
                    /* a row is never cut: the half is looked at after it */
                    dgx_vscreen8_lut_scaled(halves.at, ink, cut_x, bw, (uint32_t)scale, lut);
                    if (over) dgx_overlay_row(over, halves.at, rxd, ryd + br, bw);
                    halves.at += row_bytes;
                    if (!dgx_vscreen_halves_room(&halves)) dgx_vscreen_halves_send(&halves);
                    continue;
                }
                for (int bc = 0; bc < bw;) {
                    /* as much of the row as the half has room for, then the half is sent if it is full */
                    const int room = (int)(dgx_vscreen_halves_room(&halves) / 2u);
                    const int n    = bw - bc < room ? bw - bc : room;
                    dgx_vscreen8_lut_scaled(halves.at, ink, cut_x + (uint32_t)bc, n, (uint32_t)scale, lut);
                    if (over) dgx_overlay_row(over, halves.at, rxd + bc, ryd + br, n);
                    halves.at += 2 * n;
                    bc += n;
                    if (n == room) dgx_vscreen_halves_send(&halves);
                }
            }
            dgx_vscreen_halves_end(&halves);
        } else {
            for (int br = 0; br < bh; ++br) {
                for (int bc = 0; bc < bw; ++bc) {
                    uint8_t cidx = DGX_VSCREEN8_INDEX(bc, br);
                    if (!cidx) continue;
                    _scr_dst->set_pixel(_scr_dst, rxd + bc, ryd + br, over ? dgx_overlay_pixel(over, rxd + bc, ryd + br, lut[cidx]) : lut[cidx]);
                }
            }
        }
    } else if (smooth && (_scr_dst->screen_subtype == DgxVirtualScreen || _scr_dst->screen_subtype == DgxVirtualBackScreen)) {
        dgx_vscreen8_smooth_put(_scr_dst, rxd, ryd, bw, bh, &picture, has_transparency);
    } else if (_scr_dst->screen_subtype == DgxVirtualScreen || _scr_dst->screen_subtype == DgxVirtualBackScreen) {
        dgx_vscreen_t *scr_dst = (dgx_vscreen_t *)_scr_dst;
        for (int br = 0; br < bh; ++br) {
            if (_scr_dst->color_bits % 8u == 0) {
                uint32_t offset_dst = DGX_VSCR_OFFSET(scr_dst, rxd, ryd + br);
                uint8_t *lp_dst     = DGX_VSCR_PTR(scr_dst, offset_dst);
                for (int bc = 0; bc < bw; ++bc) {
                    uint8_t  cidx  = DGX_VSCREEN8_INDEX(bc, br);
                    uint32_t color = over ? dgx_overlay_pixel(over, rxd + bc, ryd + br, lut[cidx]) : lut[cidx];
                    if (cidx || !has_transparency) {
                        uint8_t *dst_ptr = lp_dst + bc * 2u;
                        dst_ptr[0]       = (uint8_t)(color >> 8);
                        dst_ptr[1]       = (uint8_t)color;
                    }
                }
            } else {
                for (int bc = 0; bc < bw; ++bc) {
                    uint8_t cidx = DGX_VSCREEN8_INDEX(bc, br);
                    if (cidx || !has_transparency) {
                        _scr_dst->set_pixel(_scr_dst, rxd + bc, ryd + br, over ? dgx_overlay_pixel(over, rxd + bc, ryd + br, lut[cidx]) : lut[cidx]);
                    }
                }
            }
        }
    }
#undef DGX_VSCREEN8_INDEX
    dgx_screen_touch(_scr_dst, rxd, rxd + bw - 1, ryd, ryd + bh - 1);
    dgx_screen_progress_down(_scr_dst);
}

void dgx_vscreen8_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, uint16_t *lut, uint32_t flags)
{
    dgx_vscreen8_region_to_screen16(_scr_dst, x_dst, y_dst, _scr_src, 0, 0, _scr_src->width, _scr_src->height, lut, flags);
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

/*
 * A turned region for a display behind a bus. A pixel set alone costs such a
 * display a window and a transfer of its own, so the region goes through one
 * window: the rows of the place it takes on the display, each gathered from
 * the source in the order the turn asks for and sent through the buffer of
 * the bus. Along a row of the display the source is walked by a step of its
 * own, a pixel or a row of it, forth or back.
 *
 * (left, top) .. (right, bottom) is what of the turned region lies on the
 * display, (x_dst, y_dst) where its corner would be. Returns false when the
 * pixels have to be set one by one: the source is not rows of whole bytes in
 * memory, or the bus has no buffer for a pixel.
 */
static bool dgx_vscreen_oriented_to_bus(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width,
                                        int height, dgx_output_orientation_t orientation, int left, int top, int right, int bottom)
{
    if (_scr_dst->screen_subtype != DgxPhysicalScreenWithBus || !dgx_vscreen_is_linear(_scr_src)) return false;
    const uint8_t bits = _scr_src->color_bits;
    if (bits != 8 && bits != 16 && bits != 18 && bits != 24 && bits != 32) return false;
    const dgx_vscreen_t *scr_src = (const dgx_vscreen_t *)_scr_src;
    dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)_scr_dst)->bus;
    const size_t         bip = dgx_color_points_to_bytes(bits, 1), pitch = (size_t)_scr_src->width * bip;
    uint8_t *const       draw_buffer = bus->buffer;
    const size_t         draw_buffer_len = bus->buffer_len - bus->buffer_len % bip;
    if (!draw_buffer || draw_buffer_len < bip || !_scr_dst->set_area || !_scr_dst->write_area || !_scr_dst->wait_buffer) return false;
    const bool swapped = dgx_vscreen_output_is_swapped(orientation);
    /* which way the source is walked along a row of the display, and whether its columns and rows are counted from their far end */
    bool across_back, down_back;
    switch (orientation) {
    case DgxOutputMirrorX:
        across_back = true, down_back = false;
        break;
    case DgxOutputMirrorY:
        across_back = false, down_back = true;
        break;
    case DgxOutputRotate180:
        across_back = true, down_back = true;
        break;
    case DgxOutputTranspose:
        across_back = false, down_back = false;
        break;
    case DgxOutputRotate90CCW:
        across_back = true, down_back = false;
        break;
    case DgxOutputRotate90CW:
        across_back = false, down_back = true;
        break;
    case DgxOutputTransverse:
        across_back = true, down_back = true;
        break;
    default:
        return false;
    }
    _scr_dst->set_area(_scr_dst, (uint16_t)left, (uint16_t)right, (uint16_t)top, (uint16_t)bottom);
    _scr_dst->wait_buffer(_scr_dst);
    const uint32_t       row_bytes = (uint32_t)(right - left + 1) * (uint32_t)bip;
    dgx_vscreen_halves_t halves;
    dgx_vscreen_halves_begin(&halves, _scr_dst, draw_buffer, draw_buffer_len, (uint32_t)bip, row_bytes, row_bytes * (uint32_t)(bottom - top + 1));
    for (int dy = top; dy <= bottom; ++dy) {
        /* the pixel of the region the first pixel of this row of the display shows */
        const int u = left - x_dst, v = dy - y_dst;
        int       sx, sy;
        ptrdiff_t step;
        if (!swapped) {
            sx = across_back ? width - 1 - u : u;
            sy = down_back ? height - 1 - v : v;
            step = across_back ? -(ptrdiff_t)bip : (ptrdiff_t)bip;
        } else {
            /* a row of the display runs down a column of the source */
            sx = across_back ? width - 1 - v : v;
            sy = down_back ? height - 1 - u : u;
            step = down_back ? -(ptrdiff_t)pitch : (ptrdiff_t)pitch;
        }
        const uint8_t *from = scr_src->v_array + (size_t)(y_src + sy) * pitch + (size_t)(x_src + sx) * bip;
        for (int count = right - left + 1; count;) {
            /* as many pixels as the half has room for, then the half is sent if it is full */
            const int room = (int)(dgx_vscreen_halves_room(&halves) / bip);
            const int n    = count < room ? count : room;
            uint8_t  *to   = halves.at;
            for (int i = 0; i < n; ++i, from += step) {
                for (size_t k = 0; k < bip; ++k) *to++ = from[k];
            }
            halves.at = to;
            count -= n;
            if (n == room) dgx_vscreen_halves_send(&halves);
        }
    }
    dgx_vscreen_halves_end(&halves);
    return true;
}

void dgx_vscreen_to_screen_oriented(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, dgx_output_orientation_t orientation)
{
    dgx_vscreen_region_to_screen_oriented(_scr_dst, x_dst, y_dst, _scr_src, 0, 0, _scr_src->width, _scr_src->height, orientation);
}

void dgx_vscreen_region_to_screen_oriented(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width,
                                           int height, dgx_output_orientation_t orientation)
{
    if (dgx_vscreen_output_is_canonical(orientation)) {
        dgx_vscreen_region_to_screen(_scr_dst, x_dst, y_dst, _scr_src, x_src, y_src, width, height);
        return;
    }
    if (_scr_dst->color_bits != _scr_src->color_bits) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: source color bits %d != destination %d", _scr_src->color_bits, _scr_dst->color_bits);
        return;
    }
    if (_scr_src->screen_subtype != DgxVirtualScreen && _scr_src->screen_subtype != DgxVirtualBackScreen) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: source is not virtual");
        return;
    }
    int x_req = x_src, y_req = y_src, w_req = width, h_req = height;
    DGX_INTERSECT_RECTANGLES(x_src, y_src, width, height, 0, 0, _scr_src->width, _scr_src->height);
    if (width <= 0 || height <= 0) return;
    /* Keep the surviving pixels where the uncropped region would have put them. */
    int cut_left   = x_src - x_req;
    int cut_top    = y_src - y_req;
    int cut_right  = (x_req + w_req) - (x_src + width);
    int cut_bottom = (y_req + h_req) - (y_src + height);
    switch (orientation) {
    case DgxOutputMirrorX:
        x_dst += cut_right, y_dst += cut_top;
        break;
    case DgxOutputMirrorY:
        x_dst += cut_left, y_dst += cut_bottom;
        break;
    case DgxOutputRotate180:
        x_dst += cut_right, y_dst += cut_bottom;
        break;
    case DgxOutputTranspose:
        x_dst += cut_top, y_dst += cut_left;
        break;
    case DgxOutputRotate90CCW:
        x_dst += cut_top, y_dst += cut_right;
        break;
    case DgxOutputRotate90CW:
        x_dst += cut_bottom, y_dst += cut_left;
        break;
    case DgxOutputTransverse:
        x_dst += cut_bottom, y_dst += cut_right;
        break;
    default:
        break;
    }

    dgx_vscreen_oriented_blit_func_t blit_func = dgx_vscreen_output_blit_func(orientation);
    if (!blit_func) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: unknown output orientation %d", (int)orientation);
        return;
    }

    int out_width  = dgx_vscreen_output_width(width, height, orientation);
    int out_height = dgx_vscreen_output_height(width, height, orientation);
    int left       = DGX_MAX(0, x_dst);
    int top        = DGX_MAX(0, y_dst);
    int right      = DGX_MIN(_scr_dst->width - 1, x_dst + out_width - 1);
    int bottom     = DGX_MIN(_scr_dst->height - 1, y_dst + out_height - 1);
    if (right < left || bottom < top) return;

    dgx_screen_progress_up(_scr_dst);
    if (!dgx_vscreen_oriented_to_bus(_scr_dst, x_dst, y_dst, _scr_src, x_src, y_src, width, height, orientation, left, top, right, bottom)) {
        blit_func(_scr_dst, x_dst, y_dst, _scr_src, x_src, y_src, width, height, false);
    }
    dgx_screen_touch(_scr_dst, left, right, top, bottom);
    dgx_screen_progress_down(_scr_dst);
}
