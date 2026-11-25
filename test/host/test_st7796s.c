/*
 * The ST7796S driver against the commands it sends. The MADCTL at start and
 * after every orientation change has to match what the screen reports, its
 * size and orientation. The 320x480 glass fills the controller memory, so
 * the window covers the whole memory in every orientation. The depth is 16
 * or 18 bits a pixel, the two the serial interface takes: 12 and 24 give no
 * screen and nothing is sent, nor does an unknown resolution. Command set 2
 * is locked again at the end of the init, by 3Ch and 69h. The panel starts
 * with inversion off, as the common init sequence leaves it. The display is
 * turned on after the screen is cleared, by the `on` commands. Upright is
 * MADCTL 0x48, MX and the BGR bit; DgxScreenBGR as the cbo clears the bit.
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
#include "drivers/st7796s.c"

#include "dgx_draw.h"

/* ---- the controller, as far as the window, the pixel format and the MADCTL go ---- */

static uint8_t command, madctl, colmod, params[4];
static int     params_n, caset_first, raset_first;
static bool    inverted; /* INVON was sent last, not INVOFF */
static bool    display_on; /* DISPON was sent, from the commands of `on` */
static uint16_t cscon;     /* the last two keys sent to CSCON (F0h) */

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value)
{
    (void)bus;
    command = value, params_n = 0;
    if (value == 0x21 || value == 0x20) inverted = value == 0x21;
    if (value == 0x29) display_on = true;
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    if (command == 0x2C) return; /* pixels */
    for (uint32_t i = 0; i < len / 8; ++i) {
        if (params_n < (int)sizeof(params)) params[params_n] = data[i];
        ++params_n;
        if (command == 0x36 && params_n == 1) madctl = params[0];
        if (command == 0x3A && params_n == 1) colmod = params[0];
        if (command == 0xF0 && params_n == 1) cscon = (uint16_t)(cscon << 8 | params[0]);
        if (command == 0x2A && params_n == 4) caset_first = params[0] << 8 | params[1];
        if (command == 0x2B && params_n == 4) raset_first = params[0] << 8 | params[1];
    }
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

/* the screen as the MADCTL puts it, turned on the upright picture of the module: the bits of MADCTL above the upright ones of the
 * panel turn the screen the way the bits say — MX the columns of the panel from the other end of the memory, MY its rows from the bottom,
 * MV the axes swapped — and the screen agrees with the bits, or nothing does */
static bool agrees(const dgx_screen_t *scr, int width, int height)
{
    const uint8_t turned = madctl ^ dgx_panel_st7796s.madctl; /* the BGR bit of a cbo of the screen goes to the pixels, not the axes */
    return (scr->dir_x == DgxScreenRightLeft) == ((turned & 0x40) != 0) && (scr->dir_y == DgxScreenBottomTop) == ((turned & 0x80) != 0) &&
           scr->swap_xy == ((turned & 0x20) != 0) && scr->width == width && scr->height == height;
}

/*
 * The eight orientations of a screen of a glass that fills the memory of
 * the controller: its window is the whole of the memory every way, so the
 * first pixel of the screen is at the corner the axes count from and the
 * last one at the other corner of the same whole.
 */
static void turns(dgx_screen_t *scr, int mw, int mh)
{
    for (int turn = 0; turn < 8; ++turn) {
        const bool swap = (turn & 4) != 0, up = (turn & 2) != 0;
        dgx_screen_orientation(scr, turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight, up ? DgxScreenBottomTop : DgxScreenTopBottom, swap);
        CHECK(agrees(scr, swap ? mh : mw, swap ? mw : mh));
        dgx_set_pixel(scr, 0, 0, 0xffff);
        CHECK(caset_first == 0 && raset_first == 0);
        dgx_set_pixel(scr, scr->width - 1, scr->height - 1, 0xffff);
        CHECK(caset_first == (swap ? mh - 1 : mw - 1) && raset_first == (swap ? mw - 1 : mh - 1));
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
    /* a depth the controller is not set up for gives no screen, and nothing is sent; nor does a resolution there is none of */
    CHECK(dgx_st7796s_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 12, DgxScreenRGB, DGX_ST7796S_320X480) == NULL && command == 0);
    CHECK(dgx_st7796s_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 24, DgxScreenRGB, DGX_ST7796S_320X480) == NULL && command == 0);
    CHECK(dgx_st7796s_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, (dgx_st7796s_resolution_t)0) == NULL && command == 0);
    CHECK(dgx_st7796s_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, (dgx_st7796s_resolution_t)3) == NULL && command == 0);
    /* the screen it starts as: the glass upright or turned by dgx_st7796s_init() to landscape */
    static const struct {
        dgx_st7796s_resolution_t resolution;
        int                      width, height;
    } panels[] = {
        {DGX_ST7796S_320X480, 320, 480},
        {DGX_ST7796S_480X320, 480, 320},
    };
    for (size_t p = 0; p < sizeof(panels) / sizeof(panels[0]); ++p) {
        madctl = 0xff, colmod = 0, inverted = false, display_on = false, cscon = 0;
        dgx_screen_t *scr = dgx_st7796s_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, panels[p].resolution);
        CHECK(scr && agrees(scr, panels[p].width, panels[p].height));
        CHECK(!inverted);
        CHECK(colmod == 0x55);
        CHECK((madctl & 0x08) != 0); /* the BGR bit of the upright picture: the colors of the screen are RGB */
        CHECK(display_on);
        CHECK(cscon == 0x3C69); /* command set 2 locked, part I and part II */
        if (!scr) continue;
        turns(scr, 320, 480);
        dgx_screen_destroy(&scr);
    }
    /* 18 bits a pixel: the COLMOD of the control interface takes it too */
    madctl = 0xff, colmod = 0;
    dgx_screen_t *scr = dgx_st7796s_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 18, DgxScreenRGB, DGX_ST7796S_320X480);
    CHECK(scr && agrees(scr, 320, 480) && colmod == 0x66);
    dgx_screen_destroy(&scr);
    /* the colors of the module turned the other way: the BGR bit of MADCTL goes with the cbo of the screen, upright otherwise */
    madctl = 0xff;
    scr = dgx_st7796s_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenBGR, DGX_ST7796S_320X480);
    CHECK(scr && agrees(scr, 320, 480) && (madctl & 0x08) == 0);
    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
