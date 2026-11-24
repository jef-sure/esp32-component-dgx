/*
 * The CO5300 driver against a model of its controller. The controller draws
 * a window right only from an even column and row to odd ones, so every
 * window the driver sends must be such a one, whatever is drawn: points,
 * lines one pixel wide, rectangles at odd places, text, fills. The glass must
 * show what the picture in memory has: sent again whole, the picture changes
 * nothing on it. A turn changes nothing on the glass either, in all eight
 * orientations of all four glasses, two of them not square. The model: 480
 * columns of memory whatever the glass, the rows of the glass, MADCTL as the
 * window on the computer has it (MV exchanges the counters, MX turns the
 * columns of the memory, MY its rows).
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
#include "drivers/co5300.c"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/GLCD5x7.h"

/* ---- the controller ---- */

#define MW 480
static int      mh;
static uint8_t  glass[480][MW][2];
static uint8_t  command, madctl, params[8], part[2], brightness;
static int      params_n, part_n, bad_windows, outside, commands;
static bool     display_on;
static uint16_t sc, ec, sp, ep, column, page;

static void pixel(const uint8_t *b)
{
    const bool swap = madctl & 0x20;
    int        x = swap ? page : column, y = swap ? column : page;
    if (madctl & 0x40) x = MW - 1 - x;
    if (madctl & 0x80) y = mh - 1 - y;
    if (x >= 0 && x < MW && y >= 0 && y < mh) glass[y][x][0] = b[0], glass[y][x][1] = b[1];
    else ++outside;
    if (++column > ec) {
        column = sc;
        if (++page > ep) page = sp;
    }
}

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value)
{
    (void)bus;
    command = value, params_n = 0, part_n = 0, ++commands;
    if (value == 0x2C) {
        column = sc, page = sp;
        if ((sc & 1) || !(ec & 1) || (sp & 1) || !(ep & 1)) ++bad_windows;
    }
    if (value == 0x28 || value == 0x29) display_on = value == 0x29;
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    for (uint32_t i = 0; i < bits / 8; ++i) {
        if (command == 0x2C || command == 0x3C) {
            part[part_n++] = data[i];
            if (part_n == 2) pixel(part), part_n = 0;
            continue;
        }
        if (params_n < (int)sizeof(params)) params[params_n] = data[i];
        const int n = ++params_n;
        if (command == 0x2A && n == 4) sc = (uint16_t)(params[0] << 8 | params[1]), ec = (uint16_t)(params[2] << 8 | params[3]);
        if (command == 0x2B && n == 4) sp = (uint16_t)(params[0] << 8 | params[1]), ep = (uint16_t)(params[2] << 8 | params[3]);
        if (command == 0x36 && n == 1) madctl = params[0];
        if (command == 0x51 && n == 1) brightness = params[0];
    }
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

/* the picture sent again whole changes nothing on the glass: the glass shows what the picture has */
static bool glass_is_picture(dgx_screen_t *scr)
{
    static uint8_t before[480][MW][2];
    memcpy(before, glass, sizeof(glass));
    scr->update_screen(scr, 0, scr->width - 1, 0, scr->height - 1);
    return !memcmp(before, glass, sizeof(glass));
}

static const struct {
    dgx_orientation_t x, y;
    bool              swap;
} ways[] = {
    {DgxScreenLeftRight, DgxScreenTopBottom, false},
    {DgxScreenRightLeft, DgxScreenTopBottom, false},
    {DgxScreenLeftRight, DgxScreenBottomTop, false},
    {DgxScreenRightLeft, DgxScreenBottomTop, false},
    {DgxScreenLeftRight, DgxScreenTopBottom, true },
    {DgxScreenRightLeft, DgxScreenTopBottom, true },
    {DgxScreenLeftRight, DgxScreenBottomTop, true },
    {DgxScreenRightLeft, DgxScreenBottomTop, true },
};

int main(void)
{
    static uint8_t      buffer[16000];
    dgx_bus_protocols_t bus = {.bus_type         = DGX_BUS_QSPI,
                               .write_command    = bus_command,
                               .write_data       = bus_data,
                               .write_data_async = bus_data,
                               .sync_write       = bus_sync,
                               .buffer           = buffer,
                               .buffer_len       = sizeof(buffer)};
    /* a depth not set up, or a glass there is none of, gives no screen, and nothing is sent */
    CHECK(dgx_co5300_init(&bus, GPIO_NUM_NC, 18, DgxScreenRGB, DGX_CO5300_466X466) == NULL && commands == 0);
    CHECK(dgx_co5300_init(&bus, GPIO_NUM_NC, 16, DgxScreenRGB, (dgx_co5300_resolution_t)0) == NULL && commands == 0);
    CHECK(dgx_co5300_init(&bus, GPIO_NUM_NC, 16, DgxScreenRGB, (dgx_co5300_resolution_t)5) == NULL && commands == 0);

    static const struct {
        dgx_co5300_resolution_t resolution;
        int                     width, height;
        uint8_t                 madctl;
    } glasses[] = {
        {DGX_CO5300_466X466, 466, 466, 0xC0},
        {DGX_CO5300_480X480, 480, 480, 0xA0},
        {DGX_CO5300_368X448, 368, 448, 0x00},
        {DGX_CO5300_280X456, 280, 456, 0x00},
    };
    static const int chain[] = {1, 5, 2, 7, 4, 3, 6, 0, 6, 5, 3, 1, 7, 2, 4, 0};
    for (size_t g = 0; g < sizeof(glasses) / sizeof(glasses[0]); ++g) {
        mh = glasses[g].height, bad_windows = 0, outside = 0, display_on = false, madctl = 0xff;
        memset(glass, 0x55, sizeof(glass));
        dgx_screen_t *scr = dgx_co5300_init(&bus, GPIO_NUM_NC, 16, DgxScreenRGB, glasses[g].resolution);
        CHECK(scr && scr->width == glasses[g].width && scr->height == glasses[g].height);
        if (!scr) continue;
        /* upright as the vendor turns it, the glass black and lit */
        CHECK(madctl == glasses[g].madctl);
        CHECK(display_on);
        CHECK(glass_is_picture(scr));
        CHECK(dgx_screen_backlight(scr, true) == ESP_ERR_NOT_SUPPORTED);
        CHECK(dgx_screen_brightness(scr, 0x80) == ESP_OK && brightness == 0x80);
        CHECK(dgx_screen_display(scr, false) == ESP_OK && !display_on);
        CHECK(dgx_screen_display(scr, true) == ESP_OK && display_on);

        /* what the controller draws wrong when it is sent as it is: points, lines one pixel wide, rectangles at odd places */
        const uint32_t white = dgx_rgb_to_16(255, 255, 255), red = dgx_rgb_to_16(255, 0, 0), green = dgx_rgb_to_16(0, 255, 0);
        for (int i = 0; i < 9; ++i) dgx_set_pixel(scr, 31 + i, 41 + 2 * i, white);
        dgx_fill_rectangle(scr, 51, 61, 1, 37, red);
        dgx_fill_rectangle(scr, 63, 70, 41, 1, green);
        dgx_fill_rectangle(scr, 77, 101, 41, 31, white);
        dgx_draw_line(scr, 5, 200, 201, 251, red);
        dgx_draw_circle(scr, 140, 150, 41, green);
        /* a fill on pairs of columns and rows goes to the glass by the color itself */
        dgx_fill_rectangle(scr, 40, 120, 100, 60, dgx_rgb_to_16(0, 200, 100));
        dgx_fill_rectangle(scr, 40, 300, scr->width - 80, 2, red);
        const dgx_font_context_t ctx = {.scr = scr, .font = &GLCD5x7, .color = white, .scale = 3};
        dgx_font_draw(&ctx, 33, 250, "F 0.12");
        /* the same inside one batch */
        dgx_screen_batch_begin(scr);
        for (int i = 0; i < 9; ++i) dgx_set_pixel(scr, 101 + i, 21, red);
        dgx_fill_rectangle(scr, 120, 20, 40, 40, green);
        dgx_screen_batch_end(scr);
        CHECK(bad_windows == 0);
        CHECK(glass_is_picture(scr));
        /* a point is on the glass where the picture has it */
        CHECK(dgx_get_pixel(scr, 31, 41) == white && dgx_get_pixel(scr, 51, 97) == red);

        /* a turn keeps the glass; a glass that is not square swaps the sides of the screen */
        int kept = 0;
        for (size_t c = 0; c < sizeof(chain) / sizeof(chain[0]); ++c) {
            const int w = chain[c];
            CHECK(dgx_screen_orientation(scr, ways[w].x, ways[w].y, ways[w].swap) == ESP_OK);
            CHECK(scr->width == (ways[w].swap ? glasses[g].height : glasses[g].width));
            kept += glass_is_picture(scr);
            /* drawn turned, the glass still shows the picture */
            dgx_set_pixel(scr, 3 + (int)c, 5, white);
            dgx_fill_rectangle(scr, 11, 13 + (int)c, 7, 3, red);
            kept += glass_is_picture(scr);
        }
        CHECK(kept == 2 * (int)(sizeof(chain) / sizeof(chain[0])));
        CHECK(bad_windows == 0 && outside == 0);
        dgx_screen_destroy(&scr);
        CHECK(scr == NULL);
    }
    CHECK_DONE();
}
