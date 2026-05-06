/*
 * dgx_slow_bus_optimized_funcs.c
 *
 *  Created on: 16.12.2022
 *      Author: KYMJ
 */

#include "dgx_bits.h"
#include "dgx_spi_area_protocol.h"
#include "dgx_screen.h"
#include "dgx_screen_with_bus.h"
#include <malloc.h>
#include "esp_heap_caps.h"

/*
 * Slow bus optimized functions
 */

#include "esp_log.h"

// static const char TAG[] = "DGX SLOW BUS FUNCTIONS";

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
            while (draw_buffer_len > 64) {
                draw_buffer = heap_caps_malloc(draw_buffer_len, MALLOC_CAP_DMA);
                if (draw_buffer != NULL) break;
                draw_buffer_len /= 2;
            }
            if (draw_buffer == NULL) {
                ESP_LOGE("DGX SLOW BUS FUNC", "Failed to allocate draw buffer for fill_rectangle_sb");
                dgx_screen_progress_down(scr);
                return;
            }
        }
    }
    uint8_t *lp  = draw_buffer;
    uint32_t pic = dgx_bytes_to_color_points(scr->color_bits, draw_buffer_len);
    if (pic > fill_scr_size) pic = fill_scr_size;
    if (scr->wait_buffer) scr->wait_buffer(scr);
    DGX_FILL_BUFFER(scr->color_bits, lp, x, pic, color);
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
    if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, x, x + w - 1, y, y + h - 1);
}

static void dgx_scr_set_pixel_sb(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    if (x < 0 || x >= scr->width || y < 0 || y >= scr->height) return;
    uint8_t  draw_buffer[4];
    uint8_t *lp = draw_buffer;
    if (scr->wait_buffer) scr->wait_buffer(scr);
    DGX_FILL_BUFFER(scr->color_bits, lp, x, 1, color);
    scr->set_area(scr, x, x, y, y);
    scr->write_area(scr, draw_buffer, dgx_color_points_to_bytes(scr->color_bits, 1) * 8u);
    if (!scr->in_progress) scr->update_screen(scr, x, x, y, y);
}

static uint32_t dgx_scr_get_pixel_sb(dgx_screen_t *_scr, int x, int y)
{
    if (_scr->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_screen_with_bus_t *scr = (dgx_screen_with_bus_t *)_scr;
        dgx_bus_protocols_t   *bus = scr->bus;
        if (scr->rcmd_send != 0) {
            _scr->set_area(_scr, x, x, y, y);
            bus->write_command(bus, scr->rcmd_send);
            // somehow we should receive data here
            return 0;
        }
    }
    return 0;
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

static void dgx_scr_line_sb(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color)
{
    int area_left   = DGX_MIN(x1, x2);
    int area_right  = DGX_MAX(x1, x2);
    int area_top    = DGX_MIN(y1, y2);
    int area_bottom = DGX_MAX(y1, y2);
    if (area_bottom < 0 || area_top >= scr->height || area_left >= scr->width || area_right < 0) return;
    int dx = x2 - x1;
    int dy = y2 - y1;
    if (dx == 0 && dy == 0) {
        scr->set_pixel(scr, x1, y1, color);
        return;
    }
    if (dx == 0 || dy == 0) {
        if (dx < 0) {
            x1 = x2;
            dx = -dx;
        }
        if (dy < 0) {
            y1 = y2;
            dy = -dy;
        }
        scr->fill_rectangle(scr, x1, y1, dx + 1, dy + 1, color);
        return;
    }
    int sy = dy < 0 ? -1 : 1;
    int sx = dx < 0 ? -1 : 1;
    dx     = DGX_ABS(dx);
    dy     = DGX_ABS(dy);
    ++dx;
    ++dy;
    dgx_screen_progress_up(scr);
    if (dx == dy) {
        while (1) {
            scr->set_pixel(scr, x1, y1, color);
            if (x1 == x2) break;
            x1 += sx;
            y1 += sy;
        };
        if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, area_left, area_right, area_top, area_bottom);
        return;
    }
    int err;
    if (dx > dy) {
        err = dy;
        dy *= 2;
        dx *= 2;
        int sp = x1;
        while (1) {
            err += dy;
            if (err >= dx || x1 == x2) {
                scr->fill_rectangle(scr, (x1 > sp ? sp : x1), y1, (x1 > sp ? x1 - sp : sp - x1) + 1, 1, color);
                if (x1 == x2) break;
                sp = x1 + sx;
                err -= dx;
                y1 += sy;
            }
            x1 += sx;
        }
    } else {
        err = dx;
        dy *= 2;
        dx *= 2;
        int sp = y1;
        while (1) {
            err += dx;
            if (err >= dy || y1 == y2) {
                scr->fill_rectangle(scr, x1, (y1 > sp ? sp : y1), 1, (y1 > sp ? y1 - sp : sp - y1) + 1, color);
                if (y1 == y2) break;
                sp = y1 + sy;
                err -= dy;
                x1 += sx;
            }
            y1 += sy;
        }
    }
    if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, area_left, area_right, area_top, area_bottom);
}

static void dgx_scr_circle_emit_outline_spans(dgx_screen_t *scr, int x, int y, int yoff, int xstart, int xend, uint32_t color)
{
    int rw = xend - xstart + 1;

    if (rw <= 0) return;

    scr->fill_rectangle(scr, x + xstart, y + yoff, rw, 1, color);
    scr->fill_rectangle(scr, x - xend, y + yoff, rw, 1, color);
    if (yoff != 0) {
        scr->fill_rectangle(scr, x + xstart, y - yoff, rw, 1, color);
        scr->fill_rectangle(scr, x - xend, y - yoff, rw, 1, color);
    }

    scr->fill_rectangle(scr, x + yoff, y + xstart, 1, rw, color);
    scr->fill_rectangle(scr, x + yoff, y - xend, 1, rw, color);
    if (yoff != 0) {
        scr->fill_rectangle(scr, x - yoff, y + xstart, 1, rw, color);
        scr->fill_rectangle(scr, x - yoff, y - xend, 1, rw, color);
    }
}

static void dgx_scr_circle_sb(dgx_screen_t *scr, int x, int y, int r, uint32_t color)
{
    if (r < 0) return;
    int area_left   = x - r;
    int area_right  = x + r;
    int area_top    = y - r;
    int area_bottom = y + r;
    if (area_bottom < 0 || area_top >= scr->height || area_left >= scr->width || area_right < 0) return;
    if (r == 0) {
        scr->set_pixel(scr, x, y, color);
        return;
    }
    int xs           = 0;
    int ys           = r;
    int dx           = 2;
    int dy           = 4 * r - 2;
    int error        = 0;
    int run_start_xs = 0;
    int prev_ys      = ys;
    int prev_xs      = xs;
    dgx_screen_progress_up(scr);
    while (ys > 0) {
        prev_xs = xs;
        if (dx < dy) {
            ++xs;
            error -= dx;
            dx += 4;
            if (-error > dy / 2) {
                --ys;
                error += dy;
                dy -= 4;
            }
        } else if (dy < dx) {
            --ys;
            error += dy;
            dy -= 4;
            if (error > dx / 2) {
                ++xs;
                error -= dx;
                dx += 4;
            }
        } else {
            ++xs;
            --ys;
            error += dy - dx;
            dx += 4;
            dy -= 4;
        }
        if (ys != prev_ys) {
            dgx_scr_circle_emit_outline_spans(scr, x, y, prev_ys, run_start_xs, prev_xs, color);
            run_start_xs = xs;
            prev_ys      = ys;
        }
    }
    dgx_scr_circle_emit_outline_spans(scr, x, y, 0, run_start_xs, xs, color);
    if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, area_left, area_right, area_top, area_bottom);
}

static void dgx_scr_solid_circle_sb(dgx_screen_t *scr, int x, int y, int r, uint32_t color)
{
    if (r < 0) return;
    int area_left   = x - r;
    int area_right  = x + r;
    int area_top    = y - r;
    int area_bottom = y + r;
    if (area_bottom < 0 || area_top >= scr->height || area_left >= scr->width || area_right < 0) return;
    if (r == 0) {
        scr->set_pixel(scr, x, y, color);
        return;
    }
    int xs    = 0;
    int ys    = r;
    int dx    = 2;
    int dy    = 4 * r - 2;
    int error = 0;
    int px    = xs;
    dgx_screen_progress_up(scr);
    scr->fill_rectangle(scr, x, y - ys, 1, 2 * ys + 1, color);
    while (ys > 0) {
        if (dx < dy) {
            ++xs;
            error -= dx;
            dx += 4;
            if (-error > dy / 2) {
                --ys;
                error += dy;
                dy -= 4;
            }
        } else if (dy < dx) {
            --ys;
            error += dy;
            dy -= 4;
            if (error > dx / 2) {
                ++xs;
                error -= dx;
                dx += 4;
            }
        } else {
            ++xs;
            --ys;
            error += dy - dx;
            dx += 4;
            dy -= 4;
        }
        if (px != xs) {
            scr->fill_rectangle(scr, x + xs, y - ys, 1, 2 * ys + 1, color);
            scr->fill_rectangle(scr, x - xs, y - ys, 1, 2 * ys + 1, color);
            px = xs;
        }
    }
    if (!dgx_screen_progress_down(scr)) scr->update_screen(scr, area_left, area_right, area_top, area_bottom);
}

static void dgx_scr_set_area_sb(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_screen_with_bus_t *sbus = (dgx_screen_with_bus_t *)scr;
        if (left > right) DGX_INT_SWAP(left, right);
        if (top > bottom) DGX_INT_SWAP(top, bottom);
        if (left >= scr->width || top >= scr->height) return;
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
        dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)scr)->bus;
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
    scr->circle         = dgx_scr_circle_sb;
    scr->destroy        = dgx_scr_with_bus_destroy;
    scr->set_pixel      = dgx_scr_set_pixel_sb;
    scr->fill_rectangle = dgx_scr_fill_rectangle_sb;
    scr->draw_line      = dgx_scr_line_sb;
    scr->get_pixel      = dgx_scr_get_pixel_sb;
    scr->set_area       = dgx_scr_set_area_sb;
    scr->read_area      = dgx_scr_read_area_sb;
    scr->write_area     = dgx_scr_write_area_sb;
    scr->wait_buffer    = dgx_scr_wait_buffer_sb;
    scr->solid_circle   = dgx_scr_solid_circle_sb;
    scr->update_screen  = dgx_scr_update_area_sb;
    scr->dir_x          = DgxScreenLeftRight;
    scr->dir_y          = DgxScreenTopBottom;
    scr->swap_xy        = false;
}
