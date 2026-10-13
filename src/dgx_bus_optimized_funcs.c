/*
 * dgx_slow_bus_optimized_funcs.c
 *
 *  Created on: 16.12.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
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

/*
 * Figures made of pixels and rectangles: every screen draws them with these,
 * a display behind a bus and a virtual screen alike, each through its own
 * set_pixel() and fill_rectangle(). So a figure has the same pixels wherever
 * it is drawn.
 */

/* a row of pixels from xa to xb, either may be the left one; one pixel is not worth a rectangle */
static inline void dgx_scr_row_run(dgx_screen_t *scr, int xa, int xb, int y, uint32_t color)
{
    if (xa == xb) {
        scr->set_pixel(scr, xa, y, color);
    } else if (xa < xb) {
        scr->fill_rectangle(scr, xa, y, xb - xa + 1, 1, color);
    } else {
        scr->fill_rectangle(scr, xb, y, xa - xb + 1, 1, color);
    }
}

/* a column of pixels from ya to yb */
static inline void dgx_scr_column_run(dgx_screen_t *scr, int x, int ya, int yb, uint32_t color)
{
    if (ya == yb) {
        scr->set_pixel(scr, x, ya, color);
    } else if (ya < yb) {
        scr->fill_rectangle(scr, x, ya, 1, yb - ya + 1, color);
    } else {
        scr->fill_rectangle(scr, x, yb, 1, ya - yb + 1, color);
    }
}

/*
 * A line by Bresenham's algorithm. The pixels that lie in one row of a
 * shallow line, or in one column of a steep one, go as one rectangle. The
 * line is always walked from its upper end, so it has the same pixels
 * whichever end it is given from.
 */
static void dgx_scr_line_sb(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color)
{
    int area_left   = DGX_MIN(x1, x2);
    int area_right  = DGX_MAX(x1, x2);
    int area_top    = DGX_MIN(y1, y2);
    int area_bottom = DGX_MAX(y1, y2);
    if (area_bottom < 0 || area_top >= scr->height || area_left >= scr->width || area_right < 0) return;
    if (x1 == x2 && y1 == y2) {
        scr->set_pixel(scr, x1, y1, color);
        return;
    }
    if (x1 == x2 || y1 == y2) {
        scr->fill_rectangle(scr, area_left, area_top, area_right - area_left + 1, area_bottom - area_top + 1, color);
        return;
    }
    if (y1 > y2) {
        DGX_INT_SWAP(x1, x2);
        DGX_INT_SWAP(y1, y2);
    }
    int adx = DGX_ABS(x2 - x1);
    int ady = y2 - y1;
    int sx  = x1 < x2 ? 1 : -1;
    int err = adx - ady;
    dgx_screen_progress_up(scr);
    if (adx == ady) {
        /* the diagonal: a pixel a row, nothing to join */
        for (; y1 <= y2; ++y1, x1 += sx) scr->set_pixel(scr, x1, y1, color);
    } else if (adx > ady) {
        /* shallow: every step goes along x, and a row ends where the line goes down */
        int run = x1;
        for (int steps = adx; steps; --steps) {
            int e2 = 2 * err;
            err -= ady;
            if (e2 <= adx) {
                dgx_scr_row_run(scr, run, x1, y1, color);
                err += adx;
                ++y1;
                run = x1 + sx;
            }
            x1 += sx;
        }
        dgx_scr_row_run(scr, run, x1, y1, color);
    } else {
        /* steep: every step goes down, and a column ends where the line goes to the side */
        int run = y1;
        for (int steps = ady; steps; --steps) {
            int e2 = 2 * err;
            err += adx;
            if (e2 >= -ady) {
                dgx_scr_column_run(scr, x1, run, y1, color);
                err -= ady;
                x1 += sx;
                run = y1 + 1;
            }
            ++y1;
        }
        dgx_scr_column_run(scr, x1, run, y1, color);
    }
    dgx_screen_progress_down(scr);
}

/*
 * The points (xa..xb, yoff) of the upper right eighth of a circle and their
 * seven reflections: four rows and four columns. A run that starts on an
 * axis meets its reflection there, and the two go as one.
 *
 * The order matters to a display that takes a window: its columns and its
 * rows are set by two commands, and one that has not changed is not sent
 * again. So the runs follow one another with the columns or the rows kept:
 * of the eight only the first and the fifth set both.
 */
static void dgx_scr_circle_runs(dgx_screen_t *scr, int x, int y, int yoff, int xa, int xb, uint32_t color)
{
    if (xa == 0) {
        dgx_scr_row_run(scr, x - xb, x + xb, y - yoff, color);
        dgx_scr_row_run(scr, x - xb, x + xb, y + yoff, color);    /* the same columns */
        dgx_scr_column_run(scr, x - yoff, y - xb, y + xb, color);
        dgx_scr_column_run(scr, x + yoff, y - xb, y + xb, color); /* the same rows */
        return;
    }
    dgx_scr_row_run(scr, x + xa, x + xb, y - yoff, color);
    dgx_scr_row_run(scr, x - xb, x - xa, y - yoff, color);    /* the same row */
    dgx_scr_row_run(scr, x - xb, x - xa, y + yoff, color);    /* the same columns */
    dgx_scr_row_run(scr, x + xa, x + xb, y + yoff, color);    /* the same row */
    dgx_scr_column_run(scr, x + yoff, y + xa, y + xb, color);
    dgx_scr_column_run(scr, x - yoff, y + xa, y + xb, color); /* the same rows */
    dgx_scr_column_run(scr, x - yoff, y - xb, y - xa, color); /* the same column */
    dgx_scr_column_run(scr, x + yoff, y - xb, y - xa, color); /* the same rows */
}

/*
 * A circle by the midpoint algorithm: one eighth is walked, from the top to
 * the diagonal, and every point of it is reflected into the other seven.
 * The points of the eighth that lie in one row go as one run.
 */
static void dgx_scr_circle_sb(dgx_screen_t *scr, int x, int y, int r, uint32_t color)
{
    if (r < 0) return;
    if (y + r < 0 || y - r >= scr->height || x - r >= scr->width || x + r < 0) return;
    if (r == 0) {
        scr->set_pixel(scr, x, y, color);
        return;
    }
    int f   = 1 - r;
    int ddx = 1;
    int ddy = -2 * r;
    int xs  = 0;
    int ys  = r;
    int run = 0; /* the first point of the row the walk is in */
    dgx_screen_progress_up(scr);
    while (xs < ys) {
        int row = ys;
        int end = xs;
        if (f >= 0) {
            --ys;
            ddy += 2;
            f += ddy;
        }
        ++xs;
        ddx += 2;
        f += ddx;
        if (ys != row) {
            dgx_scr_circle_runs(scr, x, y, row, run, end, color);
            run = xs;
        }
    }
    dgx_scr_circle_runs(scr, x, y, ys, run, xs, color);
    dgx_screen_progress_down(scr);
}

/*
 * A solid circle by the same walk, filled by rows. A row at the height of a
 * point of the eighth reaches as far to the sides as that point lies from
 * the top; rows that reach equally far, as those near the middle do, go as
 * one rectangle. A row of the cap is done when the walk leaves it.
 *
 * Rows and not columns: where a screen is memory, a row is one piece of it
 * and a column is a pixel in every row it crosses. To a display that takes a
 * window it is all the same.
 */
static void dgx_scr_solid_circle_sb(dgx_screen_t *scr, int x, int y, int r, uint32_t color)
{
    if (r < 0) return;
    if (y + r < 0 || y - r >= scr->height || x - r >= scr->width || x + r < 0) return;
    if (r == 0) {
        scr->set_pixel(scr, x, y, color);
        return;
    }
    int f    = 1 - r;
    int ddx  = 1;
    int ddy  = -2 * r;
    int xs   = 0;
    int ys   = r;
    int band = 0; /* the nearest to the middle of the rows that reach ys to each side */
    dgx_screen_progress_up(scr);
    while (1) {
        int  reach = ys;
        int  end   = xs;
        bool more  = xs < ys;
        if (more) {
            if (f >= 0) {
                --ys;
                ddy += 2;
                f += ddy;
            }
            ++xs;
            ddx += 2;
            f += ddx;
            if (ys == reach) continue;
        }
        /* the rows from band to end reach `reach` to each side: above and below the middle, or across it */
        if (band == 0) {
            scr->fill_rectangle(scr, x - reach, y - end, 2 * reach + 1, 2 * end + 1, color);
        } else {
            scr->fill_rectangle(scr, x - reach, y - end, 2 * reach + 1, end - band + 1, color);
            scr->fill_rectangle(scr, x - reach, y + band, 2 * reach + 1, end - band + 1, color);
        }
        /* the row of the cap the walk has left reaches as far as the walk came in it; the middle row is filled already */
        if (end > 0 && reach > 0) {
            scr->fill_rectangle(scr, x - end, y - reach, 2 * end + 1, 1, color);
            scr->fill_rectangle(scr, x - end, y + reach, 2 * end + 1, 1, color);
        }
        if (!more) break;
        band = xs;
    }
    dgx_screen_progress_down(scr);
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
