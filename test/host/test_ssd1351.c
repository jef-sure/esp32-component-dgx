/*
 * The SSD1351 driver against a model of the controller: a window of panel
 * columns and panel rows, 0x15 and 0x75, the memory written from its corner
 * a pixel at a time, by rows or, with bit 0 of the remap 0xA0, by columns. A
 * pixel is two bytes, or at 262k colors (bits 7:6 of the remap 10b) three
 * bytes with six bits low in each, D5..D0 (Table 8-8 of the datasheet). What
 * the memory holds has to be the screen, turned or not: a pixel (x, y) of a
 * turned screen in panel row x and panel column y. On P8 the window goes
 * with every piece of pixels, from where the piece begins: the pieces of a
 * turned screen have to begin where the controller, a column first, goes on.
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

static uint32_t gddram[SIDE][SIDE]; /* [row][column] */
static int      col_start, col_end, row_start, row_end, col, row;
static uint8_t  last_cmd, remap, start_line, args[4], arg_count, part[3], parts;

static void bus_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    last_cmd = cmd, arg_count = 0, parts = 0;
    if (cmd == 0x5C) col = col_start, row = row_start;
}

static void bus_write_commands(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len / 8; ++i) bus_write_command(bus, data[i]);
}

static void pixel(uint32_t value)
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
            const bool deep = (remap >> 6) == 2;
            part[parts++]   = b;
            if (parts < (deep ? 3 : 2)) continue;
            parts = 0;
            pixel(deep ? (uint32_t)(part[0] & 0x3f) << 12 | (uint32_t)(part[1] & 0x3f) << 6 | (part[2] & 0x3f) : (uint32_t)part[0] << 8 | part[1]);
            continue;
        }
        if (arg_count < sizeof(args)) args[arg_count] = b;
        ++arg_count;
        if (last_cmd == 0x15 && arg_count == 2) col_start = args[0], col_end = args[1], col = col_start;
        if (last_cmd == 0x75 && arg_count == 2) row_start = args[0], row_end = args[1], row = row_start;
        if (last_cmd == 0xA0 && arg_count == 1) remap = args[0];
        if (last_cmd == 0xA1 && arg_count == 1) start_line = args[0];
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

/* the same figure on the display and on a virtual screen of the same size; `sprite` is a picture of the same depth */
static void figure(dgx_screen_t *a, dgx_screen_t *b, dgx_screen_t *sprite)
{
    const int      x = rnd(-5, SIDE + 4), y = rnd(-5, SIDE + 4);
    const uint32_t color = (uint32_t)rnd(1, a->color_bits == 18 ? 0xffffff : 0xffff);
    switch (rnd(0, 5)) {
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
    case 4: {
        const uint32_t flags = DGX_VSCREEN_ORIENTED(rnd(0, 7));
        dgx_vscreen_to_screen(a, x - 10, y - 10, sprite, flags), dgx_vscreen_to_screen(b, x - 10, y - 10, sprite, flags);
        break;
    }
    default:
        dgx_font_string_utf8_screen(a, (int16_t)x, (int16_t)y, "SSD1351", color, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
        dgx_font_string_utf8_screen(b, (int16_t)x, (int16_t)y, "SSD1351", color, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
        break;
    }
}

/* what the memory of the controller holds for a pixel of a screen: at 18 bits the six high bits of each byte */
static uint32_t held(uint32_t value, uint8_t bits)
{
    if (bits != 18) return value;
    return (value >> 18 & 0x3f) << 12 | (value >> 10 & 0x3f) << 6 | (value >> 2 & 0x3f);
}

/* a bus buffer of `buffer_len` bytes: more than the 768 the driver sends at a time at 18 bits makes it send in pieces */
static void run(enum dgx_bus_type type, uint8_t bits, bool swap_xy, size_t buffer_len)
{
    static uint8_t      buffer[2048];
    dgx_bus_protocols_t bus = {.bus_type         = type,
                               .write_command    = bus_write_command,
                               .write_commands   = bus_write_commands,
                               .write_data       = bus_write_data,
                               .write_data_async = bus_write_data_async,
                               .sync_write       = bus_sync,
                               .buffer           = buffer,
                               .buffer_len       = buffer_len};
    memset(gddram, 0xa5, sizeof(gddram)); /* what the memory holds at power-up is not known: the first fill has to reach all of it */
    remap = 0;
    dgx_screen_t *scr = dgx_ssd1351_init(&bus, GPIO_NUM_NC, bits, DgxScreenRGB);
    dgx_ssd1351_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, swap_xy);
    CHECK(((remap & 1) != 0) == swap_xy && (remap >> 6) == (bits == 18 ? 2 : 1));
    dgx_screen_t *ref    = dgx_vscreen_init(SIDE, SIDE, bits, DgxScreenRGB);
    dgx_screen_t *sprite = dgx_vscreen_init(23, 17, bits, DgxScreenRGB);
    for (int y = 0; y < sprite->height; ++y) {
        for (int x = 0; x < sprite->width; ++x)
            dgx_set_pixel(sprite, x, y, (uint32_t)rnd(0, bits == 18 ? 0xffffff : 0xffff));
    }
    dgx_fill_rectangle(scr, 0, 0, SIDE, SIDE, 0), dgx_fill_rectangle(ref, 0, 0, SIDE, SIDE, 0);
    for (int round = 0; round < 300; ++round) {
        figure(scr, ref, sprite);
        int wrong = 0;
        for (int y = 0; y < SIDE; ++y) {
            for (int x = 0; x < SIDE; ++x)
                wrong += (swap_xy ? gddram[x][y] : gddram[y][x]) != held(dgx_get_pixel(ref, x, y), bits);
        }
        if (wrong) {
            fprintf(stderr, "%s, %d bits, swap %d, round %d: %d pixels wrong\n", type == DGX_BUS_P8 ? "P8" : "SPI", bits, swap_xy, round, wrong);
            ++check_failures;
            break;
        }
    }
    dgx_screen_destroy(&sprite);
    dgx_screen_destroy(&ref);
    dgx_screen_destroy(&scr);
}

/*
 * The four turns setRotation() of Adafruit_SSD1351 gives its modules, the
 * remap and the start line, the first of them upright: a screen starts as
 * the first and turns as the others, never mirrored.
 */
static void adafruit_turns(void)
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
    static const struct {
        dgx_output_orientation_t turn;
        uint8_t                  remap, start_line;
    } rotations[] = {
        {DgxOutputNormal,      0x74, 128}, /* setRotation(0): the COM lines scanned from the last */
        {DgxOutputRotate90CW,  0x77, 128}, /* 1: so, the columns from the right, a column first */
        {DgxOutputRotate180,   0x66, 0  }, /* 2: the columns from the right */
        {DgxOutputRotate90CCW, 0x65, 0  }, /* 3: a column first */
    };
    dgx_screen_t *scr = dgx_ssd1351_init(&bus, GPIO_NUM_NC, 16, DgxScreenRGB);
    CHECK(remap == rotations[0].remap && start_line == rotations[0].start_line);
    for (size_t i = 0; i < sizeof(rotations) / sizeof(rotations[0]); ++i) {
        const dgx_output_orientation_t turn = rotations[i].turn;
        dgx_ssd1351_orientation(scr, dgx_output_orientation_xdir(turn), dgx_output_orientation_ydir(turn), dgx_output_orientation_swap_xy(turn));
        CHECK(remap == rotations[i].remap && start_line == rotations[i].start_line);
    }
    dgx_screen_destroy(&scr);
}

int main(void)
{
    adafruit_turns();
    /* a depth the controller is not set up for gives no screen, and nothing is sent */
    dgx_bus_protocols_t untouched = {.bus_type = DGX_BUS_SPI, .write_command = bus_write_command, .write_data = bus_write_data};
    last_cmd                      = 0;
    CHECK(dgx_ssd1351_init(&untouched, GPIO_NUM_NC, 12, DgxScreenRGB) == NULL && last_cmd == 0);
    const enum dgx_bus_type types[] = {DGX_BUS_SPI, DGX_BUS_P8};
    for (int t = 0; t < 2; ++t) {
        run(types[t], 16, false, 512);
        run(types[t], 16, true, 2048);
        run(types[t], 18, false, 512);
        run(types[t], 18, true, 2048);
    }
    CHECK_DONE();
}
