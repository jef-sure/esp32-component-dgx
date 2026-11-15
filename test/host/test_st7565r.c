/*
 * The ST7565R driver against a model of the controller: 8 pages of 132
 * columns, a column and a page before every run of bytes. What the model
 * holds has to be the screen as it lies on the panel, in all eight
 * orientations, which the driver makes in software.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "drivers/st7565r.c"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/TerminusTTFMedium12.h"

#define COLUMNS 132
#define PAGES   8

/* ---- the controller ---- */

static uint8_t ddram[PAGES][COLUMNS];
static int     col, page;
static int     pending_cmd, pending_args; /* a command waiting for its argument */
static int     display_on = -1, adc = -1, scan = -1, contrast = -1, ratio = -1;
static long    command_bytes, data_bytes;

/* the two-byte commands: the contrast and the booster ratio */
static int arity(uint8_t cmd)
{
    return cmd == 0x81 || cmd == 0xF8;
}

static void complete(uint8_t cmd, uint8_t arg)
{
    if (cmd < 0x10) {
        col = (col & 0xF0) | cmd;
    } else if (cmd < 0x20) {
        col = (col & 0x0F) | ((cmd & 0x0F) << 4);
    } else if ((cmd & 0xF0) == 0xB0) {
        page = cmd & 0x0F;
    } else if (cmd == 0xA0 || cmd == 0xA1) {
        adc = cmd & 1;
    } else if (cmd == 0xC0 || cmd == 0xC8) {
        scan = cmd == 0xC8;
    } else if (cmd == 0xAE || cmd == 0xAF) {
        display_on = cmd & 1;
    } else if (cmd == 0x81) {
        contrast = arg;
    } else if ((cmd & 0xF8) == 0x20) {
        ratio = cmd & 7;
    }
}

static void command_byte(uint8_t value)
{
    ++command_bytes;
    if (pending_args) {
        pending_args = 0;
        complete((uint8_t)pending_cmd, value);
        return;
    }
    pending_cmd  = value;
    pending_args = arity(value);
    if (pending_args == 0) complete(value, 0);
}

static void bus_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    command_byte(cmd);
}

static void bus_write_commands(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    CHECK(len % 8 == 0);
    for (uint32_t i = 0; i < len / 8; ++i) command_byte(data[i]);
}

static void bus_write_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    CHECK(len % 8 == 0);
    CHECK(pending_args == 0);
    for (uint32_t i = 0; i < len / 8; ++i) {
        ++data_bytes;
        CHECK(col < COLUMNS && page < PAGES);
        if (col < COLUMNS && page < PAGES) ddram[page][col] = data[i];
        ++col;
    }
}

static uint8_t *bus_memory(uint32_t len)
{
    uint8_t *m = (uint8_t *)malloc(len);
    if (m) memset(m, 0xa5, len); /* not cleared, as malloc() gives it */
    return m;
}

/* where a pixel of the screen lies on the panel: as the MX, MY and MV bits of a color controller put it */
static void on_panel(const dgx_screen_t *scr, int x, int y, int *px, int *py)
{
    const int pw = scr->swap_xy ? scr->height : scr->width, ph = scr->swap_xy ? scr->width : scr->height;
    const int ax = scr->swap_xy ? y : x, ay = scr->swap_xy ? x : y;
    *px = scr->dir_x == DgxScreenRightLeft ? pw - 1 - ax : ax;
    *py = scr->dir_y == DgxScreenBottomTop ? ph - 1 - ay : ay;
}

/* the picture the controller holds against the screen as it lies on the panel */
static int mismatches(dgx_screen_t *scr)
{
    int wrong = 0;
    for (int y = 0; y < scr->height; ++y) {
        for (int x = 0; x < scr->width; ++x) {
            int px, py;
            on_panel(scr, x, y, &px, &py);
            wrong += ((ddram[py >> 3][px] >> (py & 7)) & 1) != (dgx_get_pixel(scr, x, y) != 0);
        }
    }
    return wrong;
}

static uint32_t seed = 7565;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static void scribble(dgx_screen_t *scr, int rounds)
{
    const dgx_font_context_t text = {.scr = scr, .font = TerminusTTFMedium12(), .color = 1};
    for (int round = 0; round < rounds; ++round) {
        const int x = rnd(-5, scr->width + 4), y = rnd(-5, scr->height + 4);
        switch (rnd(0, 4)) {
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
            dgx_fill_circle(scr, x, y, rnd(0, 20), (uint32_t)rnd(0, 1));
            break;
        default:
            /* a batch: one update for all of it */
            dgx_screen_batch_begin(scr);
            dgx_font_draw(&text, x, y, "ST7565R");
            dgx_fill_rectangle(scr, x, y, 3, 3, 0);
            dgx_screen_batch_end(scr);
            break;
        }
        const int wrong = mismatches(scr);
        CHECK(wrong == 0);
        if (wrong) break;
    }
}

int main(void)
{
    dgx_bus_protocols_t bus = {.bus_type = DGX_BUS_SPI, .write_command = bus_write_command, .write_commands = bus_write_commands,
                               .write_data = bus_write_data, .memory_allocator = bus_memory};
    memset(ddram, 0x55, sizeof(ddram)); /* what the memory holds at power-up is not known */
    dgx_screen_t *scr = dgx_st7565r_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC);
    CHECK(scr && scr->width == 128 && scr->height == 64);
    if (!scr) CHECK_DONE();
    /* the start-up: the columns from the left, the rows scanned the other way, the contrast set, the display on */
    CHECK(adc == 0 && scan == 1 && ratio == 7 && contrast == 12 && display_on == 1);
    CHECK(pending_args == 0 && data_bytes == 0);

    /* a whole screen: a page at a time, 3 command bytes and 128 bytes of data each */
    long before = data_bytes, cmds = command_bytes;
    dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0);
    CHECK(data_bytes - before == 128 * PAGES && command_bytes - cmds == 3 * PAGES);
    CHECK(mismatches(scr) == 0);

    /* a pixel costs its byte */
    before = data_bytes;
    dgx_set_pixel(scr, 17, 13, 1);
    CHECK(data_bytes - before == 1 && (ddram[1][17] & 0x20) && mismatches(scr) == 0);
    scribble(scr, 300);

    /*
     * All eight orientations through the call every display has. What was
     * drawn stays when the size does and is cleared when the axes are
     * swapped; a corner mark tells the orientations apart; a pixel still
     * costs one byte.
     */
    for (int turn = 0; turn < 8; ++turn) {
        const dgx_orientation_t dir_x = turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight;
        const dgx_orientation_t dir_y = turn & 2 ? DgxScreenBottomTop : DgxScreenTopBottom;
        const bool              swap  = (turn & 4) != 0;
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0);
        dgx_fill_rectangle(scr, 3, 5, 20, 9, 1);
        dgx_draw_line(scr, 0, 0, scr->width - 1, scr->height - 1, 1);
        const bool resized = (scr->width == 128) == swap;
        CHECK(dgx_screen_orientation(scr, dir_x, dir_y, swap) == ESP_OK);
        CHECK(scr->width == (swap ? 64 : 128) && scr->height == (swap ? 128 : 64));
        CHECK(mismatches(scr) == 0);
        int lit = 0;
        for (int y = 0; y < scr->height; ++y) {
            for (int x = 0; x < scr->width; ++x) lit += dgx_get_pixel(scr, x, y) != 0;
        }
        CHECK(resized ? lit == 0 : lit > 180);

        dgx_fill_rectangle(scr, 0, 0, 5, 2, 1);
        int px, py;
        on_panel(scr, 0, 0, &px, &py);
        CHECK(px == ((turn & 1) ? 127 : 0) && py == ((turn & 2) ? 63 : 0));
        CHECK(ddram[py >> 3][px] >> (py & 7) & 1);
        /* the mark is 5 pixels long along x of the screen: along the rows of the panel, or down its columns when the axes are swapped */
        on_panel(scr, 4, 0, &px, &py);
        CHECK(swap ? py == ((turn & 2) ? 59 : 4) : px == ((turn & 1) ? 123 : 4));
        CHECK(ddram[py >> 3][px] >> (py & 7) & 1);

        scribble(scr, 150);
        before = data_bytes;
        dgx_set_pixel(scr, scr->width / 2, scr->height / 2, 1);
        CHECK(data_bytes - before == 1 && mismatches(scr) == 0);
    }
    CHECK(dgx_screen_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false) == ESP_OK);
    CHECK(scr->width == 128 && scr->height == 64 && mismatches(scr) == 0);

    /* the picture goes off and on with no data; no backlight line was given; the controller has no brightness call */
    before = data_bytes, cmds = command_bytes;
    CHECK(dgx_screen_display(scr, false) == ESP_OK && display_on == 0 && command_bytes - cmds == 1);
    CHECK(dgx_screen_display(scr, true) == ESP_OK && display_on == 1 && data_bytes == before);
    CHECK(dgx_screen_backlight(scr, true) == ESP_ERR_NOT_SUPPORTED);
    CHECK(dgx_screen_brightness(scr, 128) == ESP_ERR_NOT_SUPPORTED);

    /* the voltage of the glass: the two values the sample code of a module names, three command bytes and no data */
    before = data_bytes, cmds = command_bytes;
    CHECK(dgx_st7565r_contrast(scr, 5, 40) == ESP_OK && ratio == 5 && contrast == 40 && pending_args == 0);
    CHECK(command_bytes - cmds == 3 && data_bytes == before);
    CHECK(dgx_st7565r_contrast(scr, 8, 40) == ESP_ERR_INVALID_ARG && dgx_st7565r_contrast(scr, 5, 64) == ESP_ERR_INVALID_ARG);
    CHECK(ratio == 5 && contrast == 40 && command_bytes - cmds == 3);
    CHECK(dgx_st7565r_contrast(scr, 0, 0) == ESP_OK && ratio == 0 && contrast == 0);
    CHECK(dgx_st7565r_contrast(scr, 7, 63) == ESP_OK && ratio == 7 && contrast == 63);

    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
