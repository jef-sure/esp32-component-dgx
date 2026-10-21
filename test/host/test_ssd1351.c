/*
 * The SSD1351 driver against a model of the controller: a window of panel
 * columns and panel rows, 0x15 and 0x75, the memory written from its corner
 * a pixel of two bytes at a time, by rows or, with bit 0 of the remap 0xA0,
 * by columns. What the memory holds has to be the screen, turned or not: a
 * pixel (x, y) of a turned screen in panel row x and panel column y.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h" /* the waits of the init table, for nothing on the host */
#include "drivers/dgx_lcd_init.c"
#include "drivers/ssd1351.c"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

#define SIDE 128

/* ---- the controller ---- */

static uint16_t gddram[SIDE][SIDE]; /* [row][column] */
static int      col_start, col_end, row_start, row_end, col, row;
static uint8_t  last_cmd, remap, args[4], arg_count, high;
static bool     have_high;

static void bus_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    last_cmd = cmd, arg_count = 0, have_high = false;
    if (cmd == 0x5C) col = col_start, row = row_start;
}

static void bus_write_commands(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len / 8; ++i) bus_write_command(bus, data[i]);
}

static void pixel(uint16_t value)
{
    CHECK(col >= col_start && col <= col_end && row >= row_start && row <= row_end);
    if (col < SIDE && row < SIDE) gddram[row][col] = value;
    if (remap & 1) {
        /* vertical: down the column, then the next column */
        if (++row > row_end) {
            row = row_start;
            if (++col > col_end) col = col_start;
        }
    } else if (++col > col_end) {
        col = col_start;
        if (++row > row_end) row = row_start;
    }
}

static void bus_write_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    CHECK(len % 8 == 0);
    for (uint32_t i = 0; i < len / 8; ++i) {
        const uint8_t b = data[i];
        if (last_cmd == 0x5C) {
            if (!have_high) {
                high = b, have_high = true;
                continue;
            }
            have_high = false;
            pixel((uint16_t)(high << 8 | b));
            continue;
        }
        if (arg_count < sizeof(args)) args[arg_count] = b;
        ++arg_count;
        if (last_cmd == 0x15 && arg_count == 2) col_start = args[0], col_end = args[1], col = col_start;
        if (last_cmd == 0x75 && arg_count == 2) row_start = args[0], row_end = args[1], row = row_start;
        if (last_cmd == 0xA0 && arg_count == 1) remap = args[0];
    }
}

static void bus_write_data_async(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    bus_write_data(bus, data, len);
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

static uint32_t seed = 1351;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

/* the same figure on the display and on a virtual screen of the same size */
static void figure(dgx_screen_t *a, dgx_screen_t *b)
{
    const int      x = rnd(-5, SIDE + 4), y = rnd(-5, SIDE + 4);
    const uint32_t color = (uint32_t)rnd(1, 0xffff);
    switch (rnd(0, 4)) {
    case 0: {
        const int w = rnd(1, 40), h = rnd(1, 30);
        dgx_fill_rectangle(a, x, y, w, h, color), dgx_fill_rectangle(b, x, y, w, h, color);
        break;
    }
    case 1:
        dgx_set_pixel(a, x, y, color), dgx_set_pixel(b, x, y, color);
        break;
    case 2: {
        const int x2 = rnd(-5, SIDE + 4), y2 = rnd(-5, SIDE + 4);
        dgx_draw_line(a, x, y, x2, y2, color), dgx_draw_line(b, x, y, x2, y2, color);
        break;
    }
    case 3: {
        const int r = rnd(0, 20);
        dgx_solid_circle(a, x, y, r, color), dgx_solid_circle(b, x, y, r, color);
        break;
    }
    default:
        dgx_font_string_utf8_screen(a, (int16_t)x, (int16_t)y, "SSD1351", color, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
        dgx_font_string_utf8_screen(b, (int16_t)x, (int16_t)y, "SSD1351", color, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
        break;
    }
}

static void run(bool swap_xy)
{
    static uint8_t      buffer[512];
    dgx_bus_protocols_t bus = {.bus_type         = DGX_BUS_SPI,
                               .write_command    = bus_write_command,
                               .write_commands   = bus_write_commands,
                               .write_data       = bus_write_data,
                               .write_data_async = bus_write_data_async,
                               .sync_write       = bus_sync,
                               .buffer           = buffer,
                               .buffer_len       = sizeof(buffer)};
    memset(gddram, 0, sizeof(gddram));
    remap = 0;
    dgx_screen_t *scr = dgx_ssd1351_init(&bus, GPIO_NUM_NC, 16, DgxScreenRGB);
    dgx_ssd1351_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, swap_xy);
    CHECK(((remap & 1) != 0) == swap_xy);
    dgx_screen_t *ref = dgx_vscreen_init(SIDE, SIDE, 16, DgxScreenRGB);
    dgx_fill_rectangle(scr, 0, 0, SIDE, SIDE, 0), dgx_fill_rectangle(ref, 0, 0, SIDE, SIDE, 0);
    for (int round = 0; round < 300; ++round) {
        figure(scr, ref);
        int wrong = 0;
        for (int y = 0; y < SIDE; ++y) {
            for (int x = 0; x < SIDE; ++x) wrong += (swap_xy ? gddram[x][y] : gddram[y][x]) != dgx_get_pixel(ref, x, y);
        }
        if (wrong) {
            fprintf(stderr, "swap %d round %d: %d pixels wrong\n", swap_xy, round, wrong);
            ++check_failures;
            break;
        }
    }
    dgx_screen_destroy(&ref);
    dgx_screen_destroy(&scr);
}

int main(void)
{
    run(false);
    run(true);
    CHECK_DONE();
}
