/*
 * The ST7735 driver against what it sends: the MADCTL of the start and of
 * every orientation has to agree with what the screen says of itself, its
 * size and the way it lies. The 0.96" glass of 80x160 lies at column 24 of
 * the 128x160 of the memory, in the middle, so its window starts there in
 * every orientation; the glass of 128x160 fills the memory. Neither is
 * started inverted: the 0.96" module tried shows its colors right without
 * INVON. The other kind of the 0.96" module, set up by a copy of the panel
 * as the docs say, has its window at column 26 and row 1 of 132x162 and
 * sends INVON.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h" /* the waits of the init table, for nothing on the host */
#include "drivers/dgx_lcd_init.c"
/* the panel and the driver both name their log tag TAG */
#define TAG TAG_PANEL
#include "drivers/panel.c"
#undef TAG
#include "drivers/st7735.c"

#include "dgx_draw.h"

/* ---- the controller, as far as the window and the MADCTL go ---- */

static uint8_t command, madctl, params[4];
static int     params_n, caset_first, raset_first;

static bool inverted; /* INVON was sent last, not INVOFF */

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value)
{
    (void)bus;
    command = value, params_n = 0;
    if (value == 0x21 || value == 0x20) inverted = value == 0x21;
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    if (command == 0x2C) return; /* pixels */
    for (uint32_t i = 0; i < len / 8; ++i) {
        if (params_n < (int)sizeof(params)) params[params_n] = data[i];
        ++params_n;
        if (command == 0x36 && params_n == 1) madctl = params[0];
        if (command == 0x2A && params_n == 4) caset_first = params[0] << 8 | params[1];
        if (command == 0x2B && params_n == 4) raset_first = params[0] << 8 | params[1];
    }
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

/* the screen as the MADCTL puts it: MX the columns of the panel from the right, MY its rows from the bottom, MV the axes swapped */
static bool agrees(const dgx_screen_t *scr, int width, int height)
{
    return (scr->dir_x == DgxScreenRightLeft) == ((madctl & 0x40) != 0) && (scr->dir_y == DgxScreenBottomTop) == ((madctl & 0x80) != 0) &&
           scr->swap_xy == ((madctl & 0x20) != 0) && scr->width == width && scr->height == height;
}

/* the eight orientations of a screen of a glass gw by gh at x_offset, y_offset of the memory mw by mh: its window begins and ends on the glass */
static void turns(dgx_screen_t *scr, int gw, int gh, int mw, int mh, int x_offset, int y_offset)
{
    for (int turn = 0; turn < 8; ++turn) {
        const bool swap = (turn & 4) != 0, up = (turn & 2) != 0;
        dgx_st7735_orientation(scr, turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight, up ? DgxScreenBottomTop : DgxScreenTopBottom, swap);
        CHECK(agrees(scr, swap ? gh : gw, swap ? gw : gh));
        /* the first pixel of the screen, and where its window begins: a mirrored axis counts the memory from its other end */
        dgx_set_pixel(scr, 0, 0, 0xffff);
        const int col = turn & 1 ? mw - gw - x_offset : x_offset;
        const int row = up ? mh - gh - y_offset : y_offset;
        CHECK(caset_first == (swap ? row : col) && raset_first == (swap ? col : row));
        /* the last pixel of the screen: the window ends where the glass does */
        dgx_set_pixel(scr, scr->width - 1, scr->height - 1, 0xffff);
        const int col_end = turn & 1 ? mw - 1 - x_offset : x_offset + gw - 1;
        const int row_end = up ? mh - 1 - y_offset : y_offset + gh - 1;
        CHECK(caset_first == (swap ? row_end : col_end) && raset_first == (swap ? col_end : row_end));
    }
}

int main(void)
{
    static uint8_t      buffer[1024];
    dgx_bus_protocols_t bus = {.bus_type         = DGX_BUS_SPI,
                               .write_command    = bus_command,
                               .write_data       = bus_data,
                               .write_data_async = bus_data,
                               .sync_write       = bus_sync,
                               .buffer           = buffer,
                               .buffer_len       = sizeof(buffer)};
    /* the screen it starts as; the glass upright, the memory and where the glass lies in it */
    static const struct {
        st7735_resolution_t resolution;
        int                 width, height, glass_w, glass_h, memory_w, memory_h, x_offset, y_offset;
    } panels[] = {
        {ST7735_RESOLUTION_128x160, 128, 160, 128, 160, 128, 160, 0, 0},
        {ST7735_RESOLUTION_160x128, 160, 128, 128, 160, 128, 160, 0, 0},
        {ST7735_RESOLUTION_80x160, 80, 160, 80, 160, 128, 160, 24, 0},
        {ST7735_RESOLUTION_160x80, 160, 80, 80, 160, 128, 160, 24, 0},
    };
    /* a depth the controller is not set up for gives no screen, and nothing is sent; nor does a panel there is none of */
    CHECK(dgx_st7735_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 24, DgxScreenRGB, ST7735_RESOLUTION_128x160) == NULL && command == 0);
    CHECK(dgx_st7735_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, (st7735_resolution_t)0) == NULL && command == 0);
    CHECK(dgx_st7735_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, (st7735_resolution_t)5) == NULL && command == 0);
    for (size_t p = 0; p < sizeof(panels) / sizeof(panels[0]); ++p) {
        madctl = 0xff, inverted = false;
        dgx_screen_t *scr = dgx_st7735_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, panels[p].resolution);
        CHECK(scr && agrees(scr, panels[p].width, panels[p].height));
        CHECK(!inverted);
        if (!scr) continue;
        turns(scr, panels[p].glass_w, panels[p].glass_h, panels[p].memory_w, panels[p].memory_h, panels[p].x_offset, panels[p].y_offset);
        dgx_screen_destroy(&scr);
    }
    /* the other kind of the 0.96" module, set up as docs/panels-en.md has it: 132x162, the glass at column 26 and row 1, INVON once the
     * screen is black */
    static const dgx_lcd_init_cmd_t invert[] = {{0x21, 0, 0, 0, {0}}, {0, 0, 0xff, 0, {0}}};
    static dgx_panel_t              plugin;
    plugin               = dgx_panel_st7735_80x160;
    plugin.memory_width  = 132;
    plugin.memory_height = 162;
    plugin.x_offset      = 26;
    plugin.y_offset      = 1;
    plugin.flags        |= DGX_PANEL_CLEAR_FIRST;
    plugin.on            = invert;
    madctl = 0xff, inverted = false;
    dgx_screen_t *scr = dgx_panel_init(&bus, &plugin, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenBGR);
    CHECK(scr && agrees(scr, 80, 160) && inverted);
    if (scr) turns(scr, 80, 160, 132, 162, 26, 1);
    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
