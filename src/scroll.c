/*
 * scroll.c
 *
 *  What a rectangle of a screen shows, moved within it any way.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dgx_draw.h"
#include "dgx_spi_area_protocol.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if !defined(ESP_PLATFORM) || defined(CONFIG_DGX_ENABLE_VSCREEN)
#include "drivers/vscreen.h"
#define DGX_SCROLL_MEMORY 1 /* a virtual screen is moved in its memory */
#endif

bool dgx_scroll(dgx_screen_t *scr, int x, int y, int w, int h, int dx, int dy, uint32_t color)
{
    /* of the rectangle, what the screen has */
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w > scr->width - x) w = scr->width - x;
    if (h > scr->height - y) h = scr->height - y;
    if (w <= 0 || h <= 0 || (!dx && !dy)) return true;
    /*
     * A display that keeps its pixels in the panel behind a bus is not read. A panel that scrolls
     * itself moves the picture when the move is one it can make; no pixel is sent then.
     */
    if (scr->screen_subtype == DgxPhysicalScreenWithBus && !dgx_screen_is_readable(scr)) {
        return dgx_screen_with_bus_scroll((dgx_screen_with_bus_t *)scr, x, y, w, h, dx, dy, color);
    }
    /* a move as large as the rectangle leaves nothing of the picture: no pixel is read */
    if (dx >= w || dx <= -w || dy >= h || dy <= -h) {
        scr->fill_rectangle(scr, x, y, w, h, color);
        return true;
    }
    if (!dgx_screen_is_readable(scr)) return false;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    /* what stays of the picture: so many columns and rows, from here to there */
    const int columns = w - ax, rows = h - ay;
    const int from_x = dx < 0 ? x + ax : x, to_x = dx > 0 ? x + ax : x;
    const int from_y = dy < 0 ? y + ay : y, to_y = dy > 0 ? y + ay : y;
    dgx_screen_progress_up(scr);
    bool moved = false;
    /* a display whose pixels are memory of its bus, a DSI display: the bus moves the rectangle in its frame buffer */
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) {
        dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)scr)->bus;
        if (bus->move && bus->move(bus, (uint16_t)from_x, (uint16_t)from_y, (uint16_t)columns, (uint16_t)rows, (uint16_t)to_x, (uint16_t)to_y)) {
            dgx_screen_touch(scr, to_x, to_x + columns - 1, to_y, to_y + rows - 1);
            moved = true;
        }
    }
#ifdef DGX_SCROLL_MEMORY
    /*
     * Pixels of whole bytes in rows one after another: a row is moved at once. A picture that goes down
     * is moved from its last row, so that no row is written over before it is read; within a row
     * memmove() sees to the same.
     */
    if (!moved && dgx_vscreen_is_linear(scr) && scr->color_bits >= 8 && scr->color_bits != 12) {
        const size_t bytes = (scr->color_bits + 7u) / 8u, pitch = (size_t)scr->width * bytes;
        uint8_t     *memory = ((dgx_vscreen_t *)scr)->v_array;
        for (int i = 0; i < rows; ++i) {
            const int row = dy > 0 ? rows - 1 - i : i;
            memmove(memory + (size_t)(to_y + row) * pitch + (size_t)to_x * bytes, memory + (size_t)(from_y + row) * pitch + (size_t)from_x * bytes,
                    (size_t)columns * bytes);
        }
        dgx_screen_touch(scr, to_x, to_x + columns - 1, to_y, to_y + rows - 1);
        moved = true;
    }
#endif
    if (!moved) {
        /* pixel by pixel, from the side the picture goes to */
        for (int i = 0; i < rows; ++i) {
            const int row = dy > 0 ? rows - 1 - i : i;
            for (int k = 0; k < columns; ++k) {
                const int column = dx > 0 ? columns - 1 - k : k;
                scr->set_pixel(scr, to_x + column, to_y + row, scr->get_pixel(scr, from_x + column, from_y + row));
            }
        }
    }
    /* what the picture has left: the rows above or below it, and the columns beside it */
    if (ay) scr->fill_rectangle(scr, x, dy > 0 ? y : y + rows, w, ay, color);
    if (ax) scr->fill_rectangle(scr, dx > 0 ? x : x + columns, to_y, ax, rows, color);
    dgx_screen_progress_down(scr);
    return true;
}
