/*
 * The scroll of the panel itself, on an ILI9341: the driver and the window
 * of the bus against a model of the controller as its datasheet has it, the
 * memory, the three bits of the orientation, the window and the scroll. What
 * the panel shows has to be what a virtual screen holds after the same
 * drawing and the same moves, made there in memory, in all eight
 * orientations: a picture is drawn where it is seen, wherever the scroll has
 * put its rows.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h"

#include "drivers/dgx_lcd_init.c"
#include "drivers/ili9341.c"

#include "dgx_draw.h"
#include "drivers/vscreen.h"

#define COLUMNS 240
#define PAGES   320

/* ---- the controller ---- */

static uint16_t memory[PAGES][COLUMNS];
static uint8_t  command, madctl;
static uint8_t  params[16];
static int      params_n;
static int      sc, ec, sp, ep;         /* the window, as addresses */
static int      column, page;           /* the next address written */
static int      tfa, vsa = PAGES, bfa, vsp;
static uint8_t  half[2];
static int      half_n;
static long     pixels_sent, windows_sent, commands_sent;

static void lcd_command(uint8_t value)
{
    command  = value;
    params_n = 0;
    half_n   = 0;
    ++commands_sent;
    if (value == 0x2c) {
        column = sc, page = sp;
        ++windows_sent;
    }
}

static void lcd_pixel(uint16_t color)
{
    CHECK(sc <= ec && sp <= ep);
    const bool swap = madctl & 0x20;
    /* the column address is that of the rows of memory when the axes are exchanged; MY turns the rows, MX the columns */
    int row = swap ? column : page, col = swap ? page : column;
    CHECK(row >= 0 && row < PAGES && col >= 0 && col < COLUMNS);
    if (madctl & 0x80) row = PAGES - 1 - row;
    if (madctl & 0x40) col = COLUMNS - 1 - col;
    if (row >= 0 && row < PAGES && col >= 0 && col < COLUMNS) memory[row][col] = color;
    ++pixels_sent;
    if (++column > ec) {
        column = sc;
        if (++page > ep) page = sp;
    }
}

static void lcd_data(const uint8_t *data, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; ++i) {
        if (command == 0x2c) {
            half[half_n++] = data[i];
            if (half_n == 2) lcd_pixel((uint16_t)(half[0] << 8 | half[1])), half_n = 0;
            continue;
        }
        if (params_n < (int)sizeof(params)) params[params_n++] = data[i];
        switch (command) {
        case 0x36:
            madctl = params[0];
            break;
        case 0x2a:
            if (params_n == 4) sc = params[0] << 8 | params[1], ec = params[2] << 8 | params[3];
            break;
        case 0x2b:
            if (params_n == 4) sp = params[0] << 8 | params[1], ep = params[2] << 8 | params[3];
            break;
        case 0x33:
            if (params_n == 6) {
                tfa = params[0] << 8 | params[1], vsa = params[2] << 8 | params[3], bfa = params[4] << 8 | params[5];
                /* otherwise the scroll is undefined */
                CHECK(tfa + vsa + bfa == PAGES);
            }
            break;
        case 0x37:
            if (params_n == 2) {
                vsp = params[0] << 8 | params[1];
                CHECK(vsp >= tfa && vsp < tfa + vsa);
            }
            break;
        default:
            break;
        }
    }
}

/* what a place of the panel shows: a row of the area scrolled is shown from the row vsp of memory on */
static uint16_t lcd_shows(int row, int col)
{
    if (row >= tfa && row < tfa + vsa) row = tfa + (row - tfa + vsp - tfa) % vsa;
    return memory[row][col];
}

/* ---- the bus: commands and data as they come ---- */

static uint8_t bus_buffer[1024];

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    lcd_command(cmd);
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    CHECK(len % 8 == 0);
    lcd_data(data, len / 8);
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

/* where a pixel of the screen is on the panel */
static void on_panel(const dgx_screen_t *scr, int x, int y, int *row, int *col)
{
    const int along = scr->swap_xy ? x : y, across = scr->swap_xy ? y : x;
    *row = scr->dir_y == DgxScreenBottomTop ? PAGES - 1 - along : along;
    *col = scr->dir_x == DgxScreenRightLeft ? COLUMNS - 1 - across : across;
}

static int mismatches(dgx_screen_t *scr, dgx_screen_t *ref)
{
    int wrong = 0;
    for (int y = 0; y < scr->height; ++y) {
        for (int x = 0; x < scr->width; ++x) {
            int row, col;
            on_panel(scr, x, y, &row, &col);
            wrong += lcd_shows(row, col) != (uint16_t)dgx_get_pixel(ref, x, y);
        }
    }
    return wrong;
}

static uint32_t seed = 987654321u;

static int rnd(int lo, int hi)
{
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return lo + (int)(seed % (uint32_t)(hi - lo + 1));
}

int main(void)
{
    dgx_bus_protocols_t bus = {.bus_type         = DGX_BUS_I2C, /* any that is not served apart: the window goes by the commands of the bus */
                               .write_command    = bus_command,
                               .write_data       = bus_data,
                               .write_data_async = bus_data,
                               .sync_write       = bus_sync,
                               .buffer           = bus_buffer,
                               .buffer_len       = sizeof(bus_buffer)};
    dgx_screen_t *scr = dgx_ili9341_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB);
    CHECK(scr != NULL);
    if (!scr) CHECK_DONE();
    CHECK(!dgx_screen_is_readable(scr));
    dgx_screen_t *picture = dgx_vscreen_init(97, 61, 16, DgxScreenRGB);
    for (int y = 0; y < picture->height; ++y) {
        for (int x = 0; x < picture->width; ++x) dgx_set_pixel(picture, x, y, (uint32_t)rnd(0, 0xffff));
    }
    long by_panel = 0, refused = 0, cut_windows = 0;
    for (int turn = 0; turn < 8; ++turn) {
        const bool swap = (turn & 4) != 0;
        dgx_ili9341_orientation(scr, turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight, turn & 2 ? DgxScreenBottomTop : DgxScreenTopBottom, swap);
        CHECK(scr->width == (swap ? PAGES : COLUMNS) && scr->height == (swap ? COLUMNS : PAGES));
        dgx_screen_t *ref = dgx_vscreen_init(scr->width, scr->height, 16, DgxScreenRGB);
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0x1234);
        dgx_fill_rectangle(ref, 0, 0, ref->width, ref->height, 0x1234);
        CHECK(mismatches(scr, ref) == 0);
        /* along the rows of memory the screen is so long; the area the panel scrolls and how far it is shifted, not at all at first */
        const int along = swap ? scr->width : scr->height;
        int       area_from = 0, area_count = 0, shifted = 0;
        for (int round = 0; round < 3000; ++round) {
            const int      x = rnd(-9, scr->width + 8), y = rnd(-9, scr->height + 8);
            const uint32_t color = (uint32_t)rnd(0, 0xffff);
            const int      what = rnd(0, 9);
            if (what <= 3) {
                /* a move along the rows of memory, of a rectangle from edge to edge across them: the area shifted, at times another */
                int from = area_from, count = area_count;
                if (!shifted || rnd(0, 40) == 0) {
                    from  = rnd(0, along - 2);
                    count = rnd(2, along - from);
                    if (rnd(0, 3) == 0) from = 0, count = along;
                }
                const int  move = rnd(0, 5) ? rnd(-12, 12) : rnd(-count - 3, count + 3);
                const int  rx = swap ? from : 0, ry = swap ? 0 : from, rw = swap ? count : scr->width, rh = swap ? scr->height : count;
                const int  dx = swap ? move : 0, dy = swap ? 0 : move;
                const long before = pixels_sent;
                const esp_err_t result = dgx_scroll(scr, rx, ry, rw, rh, dx, dy, color);
                const bool      done = result == ESP_OK;
                CHECK(done || result == ESP_ERR_NOT_SUPPORTED);
                /* the panel has one area: while it is shifted another cannot be moved, only filled */
                const bool fills = move >= count || move <= -count;
                const bool same = !shifted || (area_from == from && area_count == count);
                CHECK(done == (move == 0 || fills || same));
                if (done) {
                    CHECK(dgx_scroll(ref, rx, ry, rw, rh, dx, dy, color) == ESP_OK);
                    if (move && !fills) {
                        /* no pixel is sent but those of the strip that is filled */
                        CHECK(pixels_sent - before == (long)(move < 0 ? -move : move) * (swap ? scr->height : scr->width));
                        area_from = from, area_count = count;
                        shifted = ((shifted - move) % count + count) % count;
                        ++by_panel;
                    } else if (fills && same) {
                        shifted = 0; /* the area is let go */
                    }
                } else {
                    CHECK(pixels_sent == before);
                    ++refused;
                }
            } else if (what == 4) {
                /* a move the panel cannot make: aslant, across its rows, or of a rectangle that is not from edge to edge */
                const int  kind = rnd(0, 2), move = rnd(1, 9) * (rnd(0, 1) ? 1 : -1);
                const int  short_by = kind == 2 ? rnd(1, 20) : 0;
                const int  rw = swap ? scr->width : scr->width - short_by, rh = swap ? scr->height - short_by : scr->height;
                int        dx = swap ? move : 0, dy = swap ? 0 : move;
                if (kind == 0) dx = move, dy = -move;
                if (kind == 1) dx = swap ? 0 : move, dy = swap ? move : 0;
                const long before = pixels_sent;
                CHECK(dgx_scroll(scr, 0, 0, rw, rh, dx, dy, color) == ESP_ERR_NOT_SUPPORTED);
                CHECK(pixels_sent == before);
                ++refused;
            } else if (what == 5) {
                const int w = rnd(1, 90), h = rnd(1, 90);
                dgx_fill_rectangle(scr, x, y, w, h, color);
                dgx_fill_rectangle(ref, x, y, w, h, color);
            } else if (what == 6) {
                const int x2 = rnd(-9, scr->width + 8), y2 = rnd(-9, scr->height + 8);
                dgx_draw_line(scr, x, y, x2, y2, color);
                dgx_draw_line(ref, x, y, x2, y2, color);
            } else if (what == 7) {
                const int r = rnd(0, 60);
                dgx_solid_circle(scr, x, y, r, color);
                dgx_solid_circle(ref, x, y, r, color);
                dgx_draw_ellipse(scr, x, y, r + 5, r / 2 + 3, ~color & 0xffff);
                dgx_draw_ellipse(ref, x, y, r + 5, r / 2 + 3, ~color & 0xffff);
            } else if (what == 8) {
                /* a picture sent through a window, whole or a part of it, and one as wide as the screen */
                const int sx = rnd(0, 40), sy = rnd(0, 30), w = rnd(1, picture->width - sx), h = rnd(1, picture->height - sy);
                const long before = windows_sent;
                dgx_vscreen_region_to_screen(scr, x, y, picture, sx, sy, w, h);
                dgx_vscreen_region_to_screen(ref, x, y, picture, sx, sy, w, h);
                if (windows_sent - before > 1) ++cut_windows;
            } else {
                dgx_set_pixel(scr, x, y, color);
                dgx_set_pixel(ref, x, y, color);
            }
            if (round % 16 == 0 || round > 2990) {
                const int wrong = mismatches(scr, ref);
                if (wrong) {
                    fprintf(stderr, "orientation %d, round %d, after %d: %d pixels wrong (area %d+%d shifted %d)\n", turn, round, what, wrong,
                            area_from, area_count, shifted);
                    ++check_failures;
                    break;
                }
            }
        }
        dgx_screen_destroy(&ref);
    }
    printf("%ld moves made by the panel, %ld refused, %ld pictures sent in pieces\n", by_panel, refused, cut_windows);
    CHECK(by_panel > 3000 && refused > 500 && cut_windows > 200);
    dgx_screen_destroy(&picture);
    free(scr);
    CHECK_DONE();
}
