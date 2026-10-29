/*
 * The ST7789 driver against what it sends: the MADCTL of the start and of
 * every orientation has to agree with what the screen says of itself, its
 * size and the way it lies. A 240x240 panel shows the first 240 rows of the
 * 320 of the memory: with the rows going up its window lies 80 further
 * along them, in the columns of the window when the axes are swapped.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h" /* the waits of the init table, for nothing on the host */
#include "drivers/dgx_lcd_init.c"
#include "drivers/st7789.c"

#include "dgx_draw.h"

/* ---- the controller, as far as the window and the MADCTL go ---- */

static uint8_t command, madctl, params[4];
static int     params_n, caset_first, raset_first;

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value)
{
    (void)bus;
    command = value, params_n = 0;
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
    static const struct {
        st7789_resolution_t resolution;
        int                 width, height;
    } panels[] = {
        {ST7789_RESOLUTION_240x240, 240, 240},
        {ST7789_RESOLUTION_240x320, 240, 320},
        {ST7789_RESOLUTION_320x240, 320, 240},
    };
    /* a depth the controller is not set up for gives no screen, and nothing is sent */
    CHECK(dgx_st7789_init(&bus, GPIO_NUM_NC, 24, DgxScreenRGB, ST7789_RESOLUTION_240x320) == NULL && command == 0);
    for (size_t p = 0; p < sizeof(panels) / sizeof(panels[0]); ++p) {
        madctl            = 0xff;
        dgx_screen_t *scr = dgx_st7789_init(&bus, GPIO_NUM_NC, 16, DgxScreenRGB, panels[p].resolution);
        CHECK(scr && agrees(scr, panels[p].width, panels[p].height));
        if (!scr) continue;
        const bool square = panels[p].resolution == ST7789_RESOLUTION_240x240;
        for (int turn = 0; turn < 8; ++turn) {
            const bool swap = (turn & 4) != 0, up = (turn & 2) != 0;
            dgx_st7789_orientation(scr, turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight, up ? DgxScreenBottomTop : DgxScreenTopBottom, swap);
            CHECK(agrees(scr, square || !swap ? 240 : 320, square || swap ? 240 : 320));
            /* the first pixel of the screen, and where its window begins */
            dgx_set_pixel(scr, 0, 0, 0xffff);
            const int shift = square && up ? 80 : 0;
            CHECK(caset_first == (swap ? shift : 0) && raset_first == (swap ? 0 : shift));
        }
        dgx_screen_destroy(&scr);
    }
    CHECK_DONE();
}
