/*
 * The SSD1322 driver against a model of the controller: 128 rows of 120
 * columns of four pixels, two pixels to a byte with the left one in the
 * high nibble, the glass of each module from its first column and row 0
 * (two nibbles a pixel on the NHD-2.7). As the datasheet has it, a byte with D/C# low is always a command
 * and the arguments come with D/C# high; a column is written when both of
 * its bytes have come; 5Ch only lets the data into memory, the address is
 * where 15h and 75h put it. What the panel shows has to be the screen as it
 * lies on the panel, on every module, in all eight orientations, at any of
 * the 16 grays.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h"

/* the pins the driver drives, as the test sees them: a pin that is not wired is never touched */
static int rst_pin = GPIO_NUM_NC, unwired_calls;

static esp_err_t pin_set_level(gpio_num_t pin, uint32_t level)
{
    (void)level;
    if (pin < 0) ++unwired_calls;
    return ESP_OK;
}

static esp_err_t pin_set_direction(gpio_num_t pin, gpio_mode_t mode)
{
    (void)mode;
    if (pin < 0) ++unwired_calls;
    return ESP_OK;
}

#define dgx_gpio_set_level(pin, level)    pin_set_level(pin, level)
#define dgx_gpio_set_direction(pin, mode) pin_set_direction(pin, mode)

#include "drivers/ssd1322.c"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/TerminusTTFMedium12.h"

#define COLS 120
#define ROWS 128

/* the module under test: its glass, the column it starts at, segments a pixel */
static int pw, ph, col0, seg;

/* ---- the controller ---- */

static uint8_t gddram[ROWS][COLS][2]; /* two bytes a column, four pixels */
static int col_start, col_end = COLS - 1, row_start, row_end = ROWS - 1, col, row;
static uint8_t first_byte;  /* the first byte of a column, held until the second comes */
static bool    half_column; /* first_byte holds a byte */
static uint8_t pending_cmd;
static int     pending_args; /* the arguments that did not come yet */
static uint8_t args[16];
static int     args_left, args_of_cmd;
static bool    writeram; /* the data that come write the memory */
static bool    display_on;
static long    data_bytes, transfers, lost_bytes, odd_pixel_transfers;
static int     remap_a = -1, remap_b = -1, mux = -1, phase = -1;

static int arity(uint8_t cmd)
{
    switch (cmd) {
    case 0x15: /* set column address: start and end */
    case 0x75: /* set row address: start and end */
        return 2;
    case 0xA1:
    case 0xA2:
    case 0xAB:
    case 0xB1:
    case 0xB3:
    case 0xB5:
    case 0xB6:
    case 0xBB:
    case 0xBE:
    case 0xC1:
    case 0xC7:
    case 0xCA:
    case 0xFD:
        return 1;
    case 0xA0:
    case 0xB4:
    case 0xD1:
        return 2;
    case 0xB8: /* the gray scale table: fifteen levels, GS1 to GS15, as data after the command */
        return 15;
    default:
        return 0;
    }
}

static void complete(void);

static void command(uint8_t cmd)
{
    /* the command before did not get all its arguments: the chip takes this byte as a new command */
    CHECK(pending_args == 0);
    if (half_column) ++lost_bytes; /* the second byte of a column did not come */
    half_column  = false;
    writeram     = false;
    pending_cmd  = cmd;
    args_of_cmd  = arity(cmd);
    args_left    = args_of_cmd;
    pending_args = args_left;
    if (!pending_args) complete();
}

static void complete(void)
{
    switch (pending_cmd) {
    case 0x15:
        col_start = args[0], col_end = args[1];
        CHECK(col_start <= col_end && col_end < COLS);
        col = col_start;
        break;
    case 0x75:
        row_start = args[0], row_end = args[1];
        CHECK(row_start <= row_end && row_end < ROWS);
        row = row_start;
        break;
    case 0x5C:
        writeram = true;
        break;
    case 0xA0:
        remap_a = args[0], remap_b = args[1];
        break;
    case 0xCA:
        mux = args[0];
        break;
    case 0xB1:
        phase = args[0];
        break;
    case 0xAF:
        /* the glass shows memory that was cleared, not the noise of power-up */
        for (int r = 0; r < ph; ++r)
            for (int c = col0; c < col0 + pw * seg / 4; ++c) CHECK(gddram[r][c][0] == 0 && gddram[r][c][1] == 0);
        display_on = true;
        break;
    default:
        break;
    }
}

static void data(uint8_t value)
{
    ++data_bytes;
    if (pending_args) {
        args[args_of_cmd - args_left] = value;
        --args_left;
        --pending_args;
        if (!pending_args) complete();
        return;
    }
    CHECK(writeram);
    if (!writeram) return;
    if (!half_column) {
        first_byte  = value;
        half_column = true;
        return;
    }
    half_column = false;
    /* two pixels in a byte, the left one in the high nibble (A[2] of the remap); a column takes two bytes */
    gddram[row][col][0] = first_byte;
    gddram[row][col][1] = value;
    if (++col > col_end) {
        col = col_start;
        if (++row > row_end) row = row_start;
    }
}


/* ---- the bus: the commands and the data of one transfer ---- */

static void line_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    command(cmd);
}

static void line_write_commands(struct _dgx_bus_protocols_t *bus, const uint8_t *cmds, uint32_t len)
{
    (void)bus;
    const uint32_t bytes = len / 8;
    ++transfers;
    for (uint32_t i = 0; i < bytes; ++i) command(cmds[i]);
}

static void line_write_data(struct _dgx_bus_protocols_t *bus, const uint8_t *d, uint32_t len)
{
    (void)bus;
    const uint32_t bytes = len / 8;
    ++transfers;
    for (uint32_t i = 0; i < bytes; ++i) data(d[i]);
}

static void line_write_data_async(struct _dgx_bus_protocols_t *bus, const uint8_t *d, uint32_t len)
{
    /* the I2S bus of an ESP32 pads an odd number of bytes with one more */
    if ((len / 8) & 1) ++odd_pixel_transfers;
    line_write_data(bus, d, len);
}

static void line_sync_write(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

static int line_malloc_given;

static uint8_t *line_malloc(uint32_t len)
{
    ++line_malloc_given;
    return (uint8_t *)malloc(len);
}

/* where a pixel of the screen lies on the panel: as the orientation of the driver puts it */
static void on_panel(const dgx_screen_t *scr, int x, int y, int *px, int *py)
{
    const int ax = scr->swap_xy ? y : x, ay = scr->swap_xy ? x : y;
    *px = scr->dir_x == DgxScreenRightLeft ? pw - 1 - ax : ax;
    *py = scr->dir_y == DgxScreenBottomTop ? ph - 1 - ay : ay;
}

/* the gray of the pixel (px, py) of the panel as the controller holds it: the row is the row, the columns of the glass start at col0 */
static int panel_gray(int px, int py)
{
    CHECK(px >= 0 && px < pw && py >= 0 && py < ph);
    const int     at  = px * seg; /* its first segment from the start of the glass */
    const int     row = py;
    const int     col = col0 + at / 4;
    const uint8_t b0  = gddram[row][col][0], b1 = gddram[row][col][1];
    const uint8_t ws[4] = {(uint8_t)(b0 >> 4), (uint8_t)(b0 & 0xf), (uint8_t)(b1 >> 4), (uint8_t)(b1 & 0xf)};
    if (seg == 2) CHECK(ws[at & 3] == ws[(at & 3) + 1]); /* both segments of a pixel show it */
    return ws[at & 3];
}
static int mismatches(const dgx_screen_t *scr)
{
    int wrong = 0;
    for (int y = 0; y < scr->height; ++y) {
        for (int x = 0; x < scr->width; ++x) {
            int px, py;
            on_panel(scr, x, y, &px, &py);
            wrong += panel_gray(px, py) != (int)(dgx_get_pixel((dgx_screen_t *)scr, x, y) >> 4);
        }
    }
    return wrong;
}

static uint32_t seed = 8128;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static const struct {
    ssd1322_resolution_t resolution;
    int                  width, height, col0, segments, remap_a, remap_b, phase;
} modules[] = {
    {SSD1322_256X64, 256, 64, 28, 1, 0x14, 0x11, 0xE2},         //
    {SSD1322_256X64_ZJY, 256, 64, 24, 1, 0x16, 0x11, 0xE2},     //
    {SSD1322_240X128, 240, 128, 24, 1, 0x36, 0x11, 0xE2},       //
    {SSD1322_240X128_TOPWIN, 240, 128, 24, 1, 0x36, 0x01, 0xE2}, //
    {SSD1322_128X64, 128, 64, 28, 2, 0x16, 0x11, 0xF2},         //
};

/* one module: what the driver sends at start, then the pictures in all eight orientations */
static void run(dgx_bus_protocols_t *bus, int m)
{
    pw = modules[m].width, ph = modules[m].height, col0 = modules[m].col0, seg = modules[m].segments;
    memset(gddram, 0xa5, sizeof(gddram)); /* what the memory of the controller holds at power-up is not known */
    display_on = false;
    const int     given = line_malloc_given;
    dgx_screen_t *scr   = dgx_ssd1322_init(bus, modules[m].resolution, rst_pin);
    CHECK(line_malloc_given == given + 1); /* the picture, its rows sent straight from it, from the bus */
    CHECK(scr && scr->width == pw && scr->height == ph);
    if (!scr) return;
    CHECK(remap_a == modules[m].remap_a && remap_b == modules[m].remap_b);
    CHECK(mux == ph - 1);
    CHECK(phase == modules[m].phase);
    CHECK(display_on);

    for (int turn = 0; turn < 8; ++turn) {
        const dgx_orientation_t dir_x = turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight;
        const dgx_orientation_t dir_y = turn & 2 ? DgxScreenBottomTop : DgxScreenTopBottom;
        const bool              swap = (turn & 4) != 0;
        /* something on the screen before the call: it stays when the size does, and is cleared when the size changes */
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0);
        dgx_fill_rectangle(scr, 3, 5, 40, 9, 8 << 4);
        dgx_draw_line(scr, 0, 0, scr->width - 1, scr->height - 1, 15 << 4);
        const bool resized = (scr->width == pw) == swap;
        dgx_ssd1322_orientation(scr, dir_x, dir_y, swap);
        CHECK(scr->width == (swap ? ph : pw) && scr->height == (swap ? pw : ph));
        CHECK(mismatches(scr) == 0);
        int lit = 0;
        for (int y = 0; y < scr->height; ++y) {
            for (int x = 0; x < scr->width; ++x) lit += dgx_get_pixel(scr, x, y) != 0;
        }
        CHECK(resized ? lit == 0 : lit > 400);

        /* the picture: a corner mark that tells the orientations apart, then whatever */
        dgx_fill_rectangle(scr, 0, 0, 5, 2, 15 << 4);
        int px, py;
        on_panel(scr, 0, 0, &px, &py);
        CHECK(px == ((turn & 1) ? pw - 1 : 0) && py == ((turn & 2) ? ph - 1 : 0));
        CHECK(panel_gray(px, py) == 15);
        /* the mark is 5 pixels long along x of the screen: along the rows of the panel, or down its columns when the axes are swapped */
        on_panel(scr, 4, 0, &px, &py);
        CHECK(panel_gray(px, py) == 15);
        CHECK(swap ? py == ((turn & 2) ? ph - 5 : 4) : px == ((turn & 1) ? pw - 5 : 4));

        for (int round = 0; round < 100; ++round) {
            const int x = rnd(-5, scr->width + 4), y = rnd(-5, scr->height + 4);
            switch (rnd(0, 5)) {
            case 0:
                dgx_fill_rectangle(scr, x, y, rnd(1, 60), rnd(1, 30), (uint32_t)rnd(0, 15) << 4);
                break;
            case 1:
                dgx_set_pixel(scr, x, y, (uint32_t)rnd(0, 15) << 4);
                break;
            case 2:
                dgx_draw_line(scr, x, y, rnd(-5, scr->width + 4), rnd(-5, scr->height + 4), (uint32_t)rnd(0, 15) << 4);
                break;
            case 3:
                dgx_solid_circle(scr, x, y, rnd(0, 20), (uint32_t)rnd(0, 15) << 4);
                break;
            case 4:
                dgx_font_string_utf8_screen(scr, (int16_t)x, (int16_t)y, "SSD1322", 15 << 4, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
                break;
            default:
                /* a batch: one update for all of it */
                dgx_screen_progress_up(scr);
                dgx_draw_circle(scr, x, y, rnd(1, 25), 15 << 4);
                dgx_fill_rectangle(scr, x, y, 3, 3, 0);
                dgx_screen_progress_down(scr);
                break;
            }
            const int wrong = mismatches(scr);
            if (wrong) {
                for (int yy = 0; yy < scr->height; ++yy)
                    for (int xx = 0; xx < scr->width; ++xx) {
                        int ppx, ppy;
                        on_panel(scr, xx, yy, &ppx, &ppy);
                        const int want = (int)(dgx_get_pixel(scr, xx, yy) >> 4), got = panel_gray(ppx, ppy);
                        if (want != got) {
                            fprintf(stderr, "module %d turn %d round %d first at (%d,%d) want %d got %d\n", m, turn, round, xx, yy, want, got);
                            yy = scr->height;
                            break;
                        }
                    }
            }
            CHECK(wrong == 0);
            if (wrong) break;
        }
    }
    dgx_screen_destroy(&scr);
}

int main(void)
{
    dgx_bus_protocols_t bus = {
        .bus_type = DGX_BUS_P8,
        .write_command = line_write_command,
        .write_commands = line_write_commands,
        .write_data = line_write_data,
        .write_data_async = line_write_data_async,
        .sync_write = line_sync_write,
        .memory_allocator = line_malloc,
    };
    /* a bus whose buffer cannot hold a row is refused, and so is a module there is none of */
    CHECK(dgx_ssd1322_init(&bus, SSD1322_256X64, rst_pin) == NULL);
    bus.buffer_len = 4000;
    bus.buffer     = line_malloc((uint32_t)bus.buffer_len);
    CHECK(dgx_ssd1322_init(&bus, (ssd1322_resolution_t)(sizeof(modules) / sizeof(modules[0])), rst_pin) == NULL);
    for (int m = 0; m < (int)(sizeof(modules) / sizeof(modules[0])); ++m) run(&bus, m);

    /*
     * The levels of gray: rising, at most 180, and what a wrong table does.
     * The driver sends GS1 to GS15 (fifteen bytes) and puts the table in
     * force by its own command 00h.
     */
    pw = 256, ph = 64, col0 = 28, seg = 1;
    uint8_t levels[16];
    for (int i = 0; i < 16; ++i) levels[i] = (uint8_t)(i * 11);
    dgx_screen_t *scr2 = dgx_ssd1322_init(&bus, SSD1322_256X64, rst_pin);
    CHECK(scr2 != NULL);
    if (scr2) {
        const long before = data_bytes;
        CHECK(dgx_ssd1322_grayscale(scr2, levels));
        CHECK(data_bytes - before == 15); /* GS1..GS15: level 0 is not set */
        CHECK(pending_cmd == 0x00);       /* the table went in force by 00h */
        levels[7] = levels[6];
        CHECK(!dgx_ssd1322_grayscale(scr2, levels));
        levels[7] = 200;
        CHECK(!dgx_ssd1322_grayscale(scr2, levels));
        dgx_ssd1322_contrast(scr2, 0x80);
        dgx_ssd1322_brightness(scr2, 20);
        dgx_ssd1322_display_mode(scr2, 3);
        dgx_ssd1322_display_mode(scr2, 0);
        dgx_screen_destroy(&scr2);
    }
    CHECK(unwired_calls == 0);
    CHECK(lost_bytes == 0);
    CHECK(odd_pixel_transfers == 0);
    free(bus.buffer);
    CHECK_DONE();
}
