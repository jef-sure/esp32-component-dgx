/*
 * screen.c
 *
 *  What a screen does by default: a display behind a bus sets a window and
 *  sends pixels into it, and every screen draws its lines and circles by
 *  its own pixels and rectangles.
 *  Created on: 16.12.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <malloc.h>

#include "dgx_bits.h"
#include "dgx_screen.h"
#include "dgx_screen_with_bus.h"
#include "dgx_spi_area_protocol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "../draw/dgx_draw_priv.h"

static void dgx_scr_set_area_sb(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom);

static void dgx_scr_fill_rectangle_sb(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > scr->width) {
        w = scr->width - x;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > scr->height) {
        h = scr->height - y;
    }
    if (w <= 0 || x + w <= 0 || x >= scr->width || h <= 0 || y + h <= 0 || y >= scr->height) return;
    dgx_screen_progress_up(scr);
    uint32_t fill_scr_size   = h * w;
    uint8_t *draw_buffer     = 0;
    size_t   draw_buffer_len = 0;
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) {
        draw_buffer_len = scr->draw_buffer_len;
        draw_buffer     = scr->draw_buffer;
    } else {
        draw_buffer_len = scr->draw_buffer_len;
        draw_buffer     = scr->draw_buffer;
        if (draw_buffer == NULL || draw_buffer_len == 0) {
            draw_buffer_len = dgx_color_points_to_bytes(scr->color_bits, fill_scr_size);
            if (draw_buffer_len > 4096) draw_buffer_len = 4096; // limit temporary buffer size for virtual screens
            /* as much as there is memory for, down to 64 bytes; a small rectangle needs less than that */
            for (;;) {
                draw_buffer = heap_caps_malloc(draw_buffer_len, MALLOC_CAP_DMA);
                if (draw_buffer != NULL || draw_buffer_len <= 64) break;
                draw_buffer_len /= 2;
            }
        }
    }
    uint8_t *lp  = draw_buffer;
    uint32_t pic = draw_buffer ? dgx_bytes_to_color_points(scr->color_bits, draw_buffer_len) : 0;
    if (pic > fill_scr_size) pic = fill_scr_size;
    if (pic == 0) {
        /* no buffer to send from, as a bus that was given none has, or a depth pixels are not counted in */
        ESP_LOGE("DGX SLOW BUS FUNC", "no draw buffer for fill_rectangle");
        if (draw_buffer != scr->draw_buffer) free(draw_buffer);
        dgx_screen_progress_down(scr);
        return;
    }
    if (scr->wait_buffer) scr->wait_buffer(scr);
    dgx_fill_pixels(scr->color_bits, lp, 0, pic, color); // index inside the buffer, not on the screen
    uint32_t pic_lenbits = dgx_color_points_to_bytes(scr->color_bits, pic) * 8u;
    scr->set_area(scr, x, x + w - 1, y, y + h - 1);
    while (fill_scr_size != 0) {
        uint32_t lenbits;
        if (fill_scr_size >= pic) {
            lenbits = pic_lenbits;
            fill_scr_size -= pic;
        } else {
            lenbits       = dgx_color_points_to_bytes(scr->color_bits, fill_scr_size) * 8u;
            fill_scr_size = 0;
        }
        scr->write_area(scr, draw_buffer, lenbits);
        if (fill_scr_size && scr->wait_buffer) scr->wait_buffer(scr);
    }
    if (draw_buffer != scr->draw_buffer) {
        free(draw_buffer);
    }
    dgx_screen_touch(scr, x, x + w - 1, y, y + h - 1);
    dgx_screen_progress_down(scr);
}

static void dgx_scr_set_pixel_sb(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    if (x < 0 || x >= scr->width || y < 0 || y >= scr->height) return;
    uint8_t  draw_buffer[4] = { 0 };
    uint8_t *lp             = draw_buffer;
    if (scr->wait_buffer) scr->wait_buffer(scr);
    dgx_fill_buf_value(scr->color_bits, lp, 0, color);
    scr->set_area(scr, x, x, y, y);
    scr->write_area(scr, draw_buffer, dgx_color_points_to_bytes(scr->color_bits, 1) * 8u);
    // draw_buffer lives on the stack: an asynchronous bus must finish with it before we return
    if (scr->wait_buffer) scr->wait_buffer(scr);
    dgx_screen_touch(scr, x, x, y, y);
}

/*
 * A display behind a bus is not read back: many SPI panels have no MISO, and
 * where the command that reads memory works it is too slow. Nothing is sent.
 */
static uint32_t dgx_scr_get_pixel_sb(dgx_screen_t *_scr, int x, int y)
{
    (void)_scr, (void)x, (void)y;
    return 0;
}

bool dgx_screen_is_readable(const dgx_screen_t *scr)
{
    return scr && scr->get_pixel && scr->get_pixel != dgx_scr_get_pixel_sb;
}

// NOP for physical not virtually backed screens

static void dgx_scr_update_area_sb(struct _dgx_screen_t *scr, int left, int right, int top, int bottom)
{
}

static void dgx_scr_with_bus_destroy(struct _dgx_screen_t **pscr)
{
    if (*pscr) {
        free(*pscr);
        *pscr = 0;
    }
}

static void dgx_scr_set_area_sb(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_screen_with_bus_t *sbus = (dgx_screen_with_bus_t *)scr;
        if (left > right) DGX_INT_SWAP(left, right);
        if (top > bottom) DGX_INT_SWAP(top, bottom);
        /* a window off the screen is no window: what is sent into it must not land in the one before */
        sbus->area_void = left >= scr->width || top >= scr->height;
        if (sbus->area_void) return;
        if (right > scr->width - 1) right = scr->width - 1;
        if (bottom > scr->height - 1) bottom = scr->height - 1;
        left += scr->cg_col_shift;
        right += scr->cg_col_shift;
        top += scr->cg_row_shift;
        bottom += scr->cg_row_shift;
        dgx_screen_with_bus_set_area_window(sbus, left, right, top, bottom);
    }
}

static void dgx_scr_write_area_sb(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_screen_with_bus_t *sbus = (dgx_screen_with_bus_t *)scr;
        dgx_bus_protocols_t   *bus = sbus->bus;
        if (sbus->area_void) return;
        if (dgx_screen_with_bus_window_is_deferred(sbus)) {
            /* a bus that takes pixels only with their command: the window goes with them */
            dgx_screen_with_bus_write_window(sbus, data, lenbits);
            return;
        }
        bus->write_data_async(bus, data, lenbits);
    }
}

static uint32_t dgx_scr_read_area_sb(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_screen_with_bus_t *sbus = (dgx_screen_with_bus_t *)scr;
        dgx_bus_protocols_t   *bus  = sbus->bus;
        if (sbus->rcmd_send != 0) {
            return bus->read_data(bus, data, lenbits);
        }
    }
    return 0;
}

static void dgx_scr_wait_buffer_sb(dgx_screen_t *scr)
{
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)scr)->bus;
        bus->sync_write(bus);
    }
}

void dgx_scr_init_slow_bus_optimized_funcs(dgx_screen_t *scr)
{
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)scr)->bus;
        scr->draw_buffer = bus->buffer;
        scr->draw_buffer_len = bus->buffer_len;
    }
    scr->circle         = dgx_draw_circle_runs;
    scr->destroy        = dgx_scr_with_bus_destroy;
    scr->set_pixel      = dgx_scr_set_pixel_sb;
    scr->fill_rectangle = dgx_scr_fill_rectangle_sb;
    scr->draw_line      = dgx_draw_line_runs;
    scr->get_pixel      = dgx_scr_get_pixel_sb;
    scr->set_area       = dgx_scr_set_area_sb;
    scr->read_area      = dgx_scr_read_area_sb;
    scr->write_area     = dgx_scr_write_area_sb;
    scr->wait_buffer    = dgx_scr_wait_buffer_sb;
    scr->solid_circle   = dgx_draw_solid_circle_rows;
    scr->update_screen  = dgx_scr_update_area_sb;
    scr->dir_x          = DgxScreenLeftRight;
    scr->dir_y          = DgxScreenTopBottom;
    scr->swap_xy        = false;
}
