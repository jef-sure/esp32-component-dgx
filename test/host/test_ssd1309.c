/*
 * The SSD1309 driver against a model of the controller: 8 pages of 128
 * segments written in the horizontal addressing mode, a column and a page
 * range for every update. The glass of the panel is wired to a run of the
 * segments starting at some offset; what it shows has to be the screen.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "drivers/ssd1309.c"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/TerminusTTFMedium12.h"

#define SEGMENTS 128
#define PAGES    8

/* ---- the controller ---- */

static uint8_t gddram[PAGES][SEGMENTS];
static int     col_start, col_end, page_start, page_end, col, page;
static int     pending_cmd, pending_args; /* a command waiting for its arguments */
static uint8_t args[2];
static int     addr_mode = -1, mux = -1, com_pins = -1, display_on = -1;
static long    command_bytes, data_bytes, transfers;

static int arity(uint8_t cmd)
{
    switch (cmd) {
    case 0x21:
    case 0x22:
        return 2;
    case 0x20:
    case 0x81:
    case 0xA8:
    case 0xD3:
    case 0xD5:
    case 0xD9:
    case 0xDA:
    case 0xDB:
        return 1;
    default:
        return 0;
    }
}

static void complete(uint8_t cmd)
{
    switch (cmd) {
    case 0x20:
        addr_mode = args[0];
        break;
    case 0x21:
        col_start = args[0], col_end = args[1];
        col = col_start, page = page_start;
        break;
    case 0x22:
        page_start = args[0], page_end = args[1];
        page = page_start, col = col_start;
        break;
    case 0xA8:
        mux = args[0];
        break;
    case 0xDA:
        com_pins = args[0];
        break;
    case 0xAE:
        display_on = 0;
        break;
    case 0xAF:
        display_on = 1;
        break;
    default:
        break;
    }
}

static void command_byte(uint8_t value)
{
    ++command_bytes;
    if (pending_args) {
        args[arity(pending_cmd) - pending_args] = value;
        if (--pending_args == 0) complete(pending_cmd);
        return;
    }
    pending_cmd  = value;
    pending_args = arity(value);
    if (pending_args == 0) complete(value);
}

static void bus_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    ++transfers;
    command_byte(cmd);
}

static void bus_write_commands(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    ++transfers;
    CHECK(len % 8 == 0);
    for (uint32_t i = 0; i < len / 8; ++i) command_byte(data[i]);
}

static void bus_write_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    ++transfers;
    CHECK(len % 8 == 0);
    CHECK(pending_args == 0);
    CHECK(addr_mode == 0); /* the ranges below mean nothing in the page mode */
    for (uint32_t i = 0; i < len / 8; ++i) {
        ++data_bytes;
        CHECK(col < SEGMENTS && page < PAGES);
        if (col < SEGMENTS && page < PAGES) gddram[page][col] = data[i];
        if (++col > col_end) {
            col = col_start;
            if (++page > page_end) page = page_start;
        }
    }
}

/* the picture on the glass against the screen, the glass wired from segment `offset`: its columns past the last segment show nothing */
static int mismatches(dgx_screen_t *scr, int offset)
{
    int wrong = 0;
    for (int y = 0; y < scr->height; ++y) {
        for (int x = 0; x + offset < SEGMENTS && x < scr->width; ++x) {
            const int shown = (gddram[y >> 3][x + offset] >> (y & 7)) & 1;
            wrong += shown != (dgx_get_pixel(scr, x, y) != 0);
        }
    }
    return wrong;
}

static uint32_t seed = 1309;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static void scribble(dgx_screen_t *scr, int offset, int rounds)
{
    for (int round = 0; round < rounds; ++round) {
        const int x = rnd(-5, scr->width + 4), y = rnd(-5, scr->height + 4);
        switch (rnd(0, 5)) {
        case 0:
            dgx_fill_rectangle(scr, x, y, rnd(1, 40), rnd(1, 30), (uint32_t)rnd(0, 1));
            break;
        case 1:
            dgx_set_pixel(scr, x, y, (uint32_t)rnd(0, 1));
            break;
        case 2:
            dgx_draw_line(scr, x, y, rnd(-5, scr->width + 4), rnd(-5, scr->height + 4), (uint32_t)rnd(0, 1));
            break;
        case 3:
            dgx_solid_circle(scr, x, y, rnd(0, 20), (uint32_t)rnd(0, 1));
            break;
        case 4:
            dgx_font_string_utf8_screen(scr, (int16_t)x, (int16_t)y, "SSD1309", 1, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
            break;
        default:
            /* a batch: one update for all of it */
            dgx_screen_progress_up(scr);
            dgx_draw_circle(scr, x, y, rnd(1, 25), 1);
            dgx_fill_rectangle(scr, x, y, 3, 3, 0);
            dgx_screen_progress_down(scr);
            break;
        }
        const int wrong = mismatches(scr, offset);
        CHECK(wrong == 0);
        if (wrong) break;
    }
}

static void run(ssd1309_resolution_t resolution, int width, int height)
{
    dgx_bus_protocols_t bus = {.bus_type = DGX_BUS_SPI, .write_command = bus_write_command, .write_commands = bus_write_commands, .write_data = bus_write_data};
    memset(gddram, 0x55, sizeof(gddram)); /* what the memory holds at power-up is not known */
    addr_mode = mux = com_pins = display_on = -1;
    command_bytes = data_bytes = transfers = 0;

    dgx_screen_t *scr = dgx_ssd1309_init(&bus, resolution, GPIO_NUM_NC);
    CHECK(scr && scr->width == width && scr->height == height);
    if (!scr) return;
    /* the start-up: horizontal addressing, the multiplex ratio of the panel, the COM pins of its height, the display on at the end */
    CHECK(addr_mode == 0);
    CHECK(mux == height - 1);
    CHECK(com_pins == (height == 64 ? 0x12 : 0x02));
    CHECK(display_on == 1);
    CHECK(pending_args == 0);
    CHECK(data_bytes == 0);
    CHECK(dgx_bw_screen_is_paged(scr));

    /* a whole screen goes as one transfer of width * pages bytes after one of 6 command bytes */
    long before = data_bytes, sent = transfers, cmds = command_bytes;
    dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0);
    CHECK(data_bytes - before == width * (height / 8) && transfers - sent == 2 && command_bytes - cmds == 6);
    CHECK(mismatches(scr, 0) == 0);

    /* a pixel costs its byte: the column and the page it lies in */
    before = data_bytes, sent = transfers;
    dgx_set_pixel(scr, 17, 13, 1);
    CHECK(data_bytes - before == 1 && transfers - sent == 2);
    CHECK((gddram[1][17] & 0x20) && mismatches(scr, 0) == 0);

    /* a rectangle over several pages: a transfer a page, the bytes of its columns only */
    before = data_bytes, sent = transfers;
    dgx_fill_rectangle(scr, 10, 5, 20, 20, 1);
    CHECK(data_bytes - before == 20 * 4 && transfers - sent == 1 + 4);
    CHECK(mismatches(scr, 0) == 0);

    scribble(scr, 0, 400);

    /* a module wired from segment 2: the picture is sent again, less the two columns past the last segment; the two spare segments keep what they held */
    const uint8_t spare0 = gddram[3][0], spare1 = gddram[3][1];
    gddram[3][0] = (uint8_t)~spare0, gddram[3][1] = (uint8_t)~spare1;
    before = data_bytes;
    dgx_ssd1309_column_offset(scr, 2);
    CHECK(data_bytes - before == (width - 2) * (height / 8));
    CHECK(mismatches(scr, 2) == 0);
    CHECK(gddram[3][0] == (uint8_t)~spare0 && gddram[3][1] == (uint8_t)~spare1);
    scribble(scr, 2, 200);
    /* a rectangle wholly past the last segment is not sent at all, one over it is cut there */
    before = data_bytes;
    dgx_fill_rectangle(scr, width - 2, 0, 2, 8, 1);
    CHECK(data_bytes == before);
    dgx_fill_rectangle(scr, width - 4, 8, 4, 8, 1);
    CHECK(data_bytes - before == 2 && mismatches(scr, 2) == 0);
    /* the offset is clamped at the last segment: the first column alone is shown */
    dgx_ssd1309_column_offset(scr, 200);
    CHECK(mismatches(scr, 127) == 0);
    dgx_ssd1309_column_offset(scr, 0);
    CHECK(mismatches(scr, 0) == 0);

    /* the contrast is two command bytes and no data */
    before = data_bytes, cmds = command_bytes;
    dgx_ssd1309_contrast(scr, 0x80);
    CHECK(data_bytes == before && command_bytes - cmds == 2);

    dgx_screen_destroy(&scr);
}

int main(void)
{
    run(SSD1309_128X64, 128, 64);
    run(SSD1309_128X32, 128, 32);
    CHECK_DONE();
}
