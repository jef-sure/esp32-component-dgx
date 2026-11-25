/*
 * The AXS15231B driver against a model of its controller. The controller
 * stores its picture by blocks of 4x4 pixels, so every window the driver
 * sends must start and end on whole blocks, whatever is drawn: points, lines
 * one pixel wide, rectangles at any place, text, fills. The pixels of a
 * window go in pieces of whole rows. MADCTL never swaps the axes, which the
 * controller does not do on QSPI; the driver lays the picture onto the glass
 * itself, and the glass must show what a panel turned by MADCTL shows for the
 * same drawing, in all eight orientations. Sent again whole, the picture
 * changes nothing on the glass, and a turn changes nothing on it either. The
 * model: memory of the size of the glass, MADCTL as the window on the
 * computer has it (MV exchanges the counters, MX turns the columns of the
 * memory, MY its rows).
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
#include "drivers/axs15231b.c"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/GLCD5x7.h"

/* ---- the controller ---- */

#define GW 320
#define GH 480
static uint8_t  glass[GH][GW][2];
static uint8_t  command, madctl, params[8], part[2];
static int      params_n, part_n, bad_windows, bad_pieces, swapped, outside, commands;
static bool     display_on, writing;
static uint16_t sc, ec, sp, ep, column, page;

static void pixel(const uint8_t *b)
{
    const bool swap = madctl & 0x20;
    int        x = swap ? page : column, y = swap ? column : page;
    if (madctl & 0x40) x = GW - 1 - x;
    if (madctl & 0x80) y = GH - 1 - y;
    if (x >= 0 && x < GW && y >= 0 && y < GH) glass[y][x][0] = b[0], glass[y][x][1] = b[1];
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
    writing = value == 0x2C;
    if (value == 0x2C) {
        column = sc, page = sp;
        if ((sc & 3) || (ec & 3) != 3 || (sp & 3) || (ep & 3) != 3) ++bad_windows;
    }
    if (value == 0x28 || value == 0x29) display_on = value == 0x29;
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    for (uint32_t i = 0; i < bits / 8; ++i) {
        if (writing) {
            part[part_n++] = data[i];
            if (part_n == 2) pixel(part), part_n = 0;
            continue;
        }
        if (params_n < (int)sizeof(params)) params[params_n] = data[i];
        const int n = ++params_n;
        if (command == 0x2A && n == 4) sc = (uint16_t)(params[0] << 8 | params[1]), ec = (uint16_t)(params[2] << 8 | params[3]);
        if (command == 0x2B && n == 4) sp = (uint16_t)(params[0] << 8 | params[1]), ep = (uint16_t)(params[2] << 8 | params[3]);
        if (command == 0x36 && n == 1) {
            madctl = params[0];
            if (madctl & 0x20) ++swapped;
        }
    }
    /* a piece of pixels ends where a row of the window ends */
    if (writing && (part_n || column != sc)) ++bad_pieces;
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

/* the picture sent again whole changes nothing on the glass: the glass shows what the picture has */
static bool glass_is_picture(dgx_screen_t *scr)
{
    static uint8_t before[GH][GW][2];
    memcpy(before, glass, sizeof(glass));
    scr->update_screen(scr, 0, scr->width - 1, 0, scr->height - 1);
    return !memcmp(before, glass, sizeof(glass));
}

static bool glass_black(void)
{
    for (int y = 0; y < GH; ++y)
        for (int x = 0; x < GW; ++x)
            if (glass[y][x][0] || glass[y][x][1]) return false;
    return true;
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

/* what a controller of blocks draws wrong when it is sent as it is: points, lines one pixel wide, rectangles anywhere */
static void draw_scene(dgx_screen_t *scr)
{
    const uint32_t white = dgx_rgb_to_16(255, 255, 255), red = dgx_rgb_to_16(255, 0, 0), green = dgx_rgb_to_16(0, 255, 0);
    for (int i = 0; i < 9; ++i) dgx_set_pixel(scr, 31 + i, 41 + 2 * i, white);
    dgx_fill_rectangle(scr, 51, 61, 1, 37, red);
    dgx_fill_rectangle(scr, 63, 70, 41, 1, green);
    dgx_fill_rectangle(scr, 77, 101, 41, 31, white);
    dgx_draw_line(scr, 5, 200, 201, 251, red);
    dgx_draw_circle(scr, 140, 150, 41, green);
    /* fills on whole blocks go to the glass by the color itself, in every orientation */
    dgx_fill_rectangle(scr, 40, 120, 100, 60, dgx_rgb_to_16(0, 200, 100));
    dgx_fill_rectangle(scr, 8, 236, scr->width - 16, 4, red);
    dgx_fill_rectangle(scr, 0, 0, 12, 8, dgx_rgb_to_16(20, 40, 200));
    const dgx_font_context_t ctx = {.scr = scr, .font = &GLCD5x7, .color = white, .scale = 3};
    dgx_font_draw(&ctx, 33, 290, "F 0.12");
    /* the same inside one batch */
    dgx_screen_batch_begin(scr);
    for (int i = 0; i < 9; ++i) dgx_set_pixel(scr, 101 + i, 21, red);
    dgx_fill_rectangle(scr, 120, 20, 40, 40, green);
    dgx_fill_rectangle(scr, 201, 17, 3, 61, white);
    dgx_screen_batch_end(scr);
}

int main(void)
{
    static uint8_t      buffer[16000];
    static uint8_t      reference[GH][GW][2];
    dgx_bus_protocols_t bus = {.bus_type         = DGX_BUS_QSPI,
                               .write_command    = bus_command,
                               .write_data       = bus_data,
                               .write_data_async = bus_data,
                               .sync_write       = bus_sync,
                               .buffer           = buffer,
                               .buffer_len       = sizeof(buffer)};
    /* a depth not set up, or a glass there is none of, gives no screen, and nothing is sent */
    CHECK(dgx_axs15231b_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 18, DgxScreenRGB, DGX_AXS15231B_320X480) == NULL && commands == 0);
    CHECK(dgx_axs15231b_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, (dgx_axs15231b_resolution_t)0) == NULL && commands == 0);
    CHECK(dgx_axs15231b_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, (dgx_axs15231b_resolution_t)2) == NULL && commands == 0);

    /* a panel of the same glass turned by MADCTL, for what the glass must show */
    const dgx_panel_t turned = {.name = "turned", .width = GW, .height = GH, .colmod = {0, 0x55}, .init = axs15231b_init_cmds};

    for (size_t w = 0; w < sizeof(ways) / sizeof(ways[0]); ++w) {
        memset(glass, 0x55, sizeof(glass));
        dgx_screen_t *ref = dgx_panel_init(&bus, &turned, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB);
        CHECK(ref != NULL);
        if (!ref) continue;
        CHECK(dgx_screen_orientation(ref, ways[w].x, ways[w].y, ways[w].swap) == ESP_OK);
        dgx_fill_rectangle(ref, 0, 0, ref->width, ref->height, 0);
        draw_scene(ref);
        memcpy(reference, glass, sizeof(glass));
        dgx_screen_destroy(&ref);

        memset(glass, 0x55, sizeof(glass));
        bad_windows = bad_pieces = swapped = outside = 0, display_on = false, madctl = 0xff;
        dgx_screen_t *scr = dgx_axs15231b_init(&bus, GPIO_NUM_NC, (gpio_num_t)1, 16, DgxScreenRGB, DGX_AXS15231B_320X480);
        CHECK(scr && scr->width == GW && scr->height == GH);
        if (!scr) continue;
        /* upright, the glass black and the display on */
        CHECK(madctl == 0x00 && display_on && glass_black());
        CHECK(dgx_screen_orientation(scr, ways[w].x, ways[w].y, ways[w].swap) == ESP_OK);
        CHECK(scr->width == (ways[w].swap ? GH : GW) && glass_black());
        draw_scene(scr);
        CHECK(!memcmp(glass, reference, sizeof(glass)));
        CHECK(glass_is_picture(scr));
        CHECK(bad_windows == 0 && bad_pieces == 0 && swapped == 0 && outside == 0 && madctl == 0x00);
        /* a point is on the glass where the picture has it */
        CHECK(dgx_get_pixel(scr, 31, 41) == dgx_rgb_to_16(255, 255, 255) && dgx_get_pixel(scr, 51, 97) == dgx_rgb_to_16(255, 0, 0));
        dgx_screen_destroy(&scr);
        CHECK(scr == NULL);
    }

    /* the controls: the backlight by its line, the display on and off, no brightness of its own */
    {
        dgx_screen_t *scr = dgx_axs15231b_init(&bus, GPIO_NUM_NC, (gpio_num_t)1, 16, DgxScreenRGB, DGX_AXS15231B_320X480);
        CHECK(scr != NULL);
        CHECK(dgx_screen_backlight(scr, false) == ESP_OK && dgx_screen_backlight(scr, true) == ESP_OK);
        CHECK(dgx_screen_brightness(scr, 0x80) == ESP_ERR_NOT_SUPPORTED);
        CHECK(dgx_screen_display(scr, false) == ESP_OK && !display_on);
        CHECK(dgx_screen_display(scr, true) == ESP_OK && display_on);
        dgx_screen_destroy(&scr);
        scr = dgx_axs15231b_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, DGX_AXS15231B_320X480);
        CHECK(scr && dgx_screen_backlight(scr, true) == ESP_ERR_NOT_SUPPORTED);
        dgx_screen_destroy(&scr);
    }

    /* a turn keeps the glass; drawn turned, the glass still shows the picture */
    {
        static const int chain[] = {1, 5, 2, 7, 4, 3, 6, 0, 6, 5, 3, 1, 7, 2, 4, 0};
        memset(glass, 0x55, sizeof(glass));
        bad_windows = bad_pieces = swapped = outside = 0;
        dgx_screen_t *scr = dgx_axs15231b_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, DGX_AXS15231B_320X480);
        CHECK(scr != NULL);
        if (scr) {
            draw_scene(scr);
            int kept = 0;
            for (size_t c = 0; c < sizeof(chain) / sizeof(chain[0]); ++c) {
                const int     w = chain[c];
                static uint8_t before[GH][GW][2];
                memcpy(before, glass, sizeof(glass));
                CHECK(dgx_screen_orientation(scr, ways[w].x, ways[w].y, ways[w].swap) == ESP_OK);
                CHECK(scr->width == (ways[w].swap ? GH : GW));
                kept += !memcmp(before, glass, sizeof(glass)) && glass_is_picture(scr);
                dgx_set_pixel(scr, 3 + (int)c, 5, dgx_rgb_to_16(255, 255, 255));
                dgx_fill_rectangle(scr, 11, 13 + (int)c, 7, 3, dgx_rgb_to_16(255, 0, 0));
                kept += glass_is_picture(scr);
            }
            CHECK(kept == 2 * (int)(sizeof(chain) / sizeof(chain[0])));
            CHECK(bad_windows == 0 && bad_pieces == 0 && swapped == 0 && outside == 0);
            dgx_screen_destroy(&scr);
        }
    }
    CHECK_DONE();
}
