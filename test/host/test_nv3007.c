/*
 * The NV3007 driver against a model of its controller: 168 columns by 428 rows
 * of memory, MADCTL as the window on the computer has it (MV exchanges the
 * counters, MX turns the columns of the memory, MY its rows). The glass is the
 * 142 columns from column 12 of the memory, as MADCTL 00h shows them upright.
 * A point drawn in each of the eight orientations must come out on the glass
 * where the turn puts it, and nothing may be written outside the glass. The
 * depth is 16 or 18 bits a pixel: 12 and 24 give no screen and nothing is
 * sent, nor does a resolution there is none of. The panel starts with the
 * inversion off, and the display goes on after the screen is cleared.
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
#include "drivers/nv3007.c"

#include "dgx_draw.h"

#define MW 168
#define MH 428
#define GW 142
#define GX 12

static uint8_t  memory[MH][MW][3];
static uint8_t  command, madctl, colmod, params[8], part[3];
static int      params_n, part_n, commands, outside;
static bool     inverted, display_on;
static uint16_t sc, ec, sp, ep, column, page;

static void pixel(const uint8_t *b)
{
    const bool swap = madctl & 0x20;
    int        x = swap ? page : column, y = swap ? column : page;
    if (madctl & 0x40) x = MW - 1 - x;
    if (madctl & 0x80) y = MH - 1 - y;
    if (x >= 0 && x < MW && y >= 0 && y < MH) {
        memcpy(memory[y][x], b, 3);
        if (x < GX || x >= GX + GW) ++outside;
    } else {
        ++outside;
    }
    if (++column > ec) {
        column = sc;
        if (++page > ep) page = sp;
    }
}

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value)
{
    (void)bus;
    command = value, params_n = 0, part_n = 0, ++commands;
    if (value == 0x2C) column = sc, page = sp;
    if (value == 0x20 || value == 0x21) inverted = value == 0x21;
    if (value == 0x29) display_on = true;
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    for (uint32_t i = 0; i < bits / 8; ++i) {
        if (command == 0x2C) {
            part[part_n++] = data[i];
            if (part_n == ((colmod & 7) == 6 ? 3 : 2)) {
                if (part_n == 2) part[2] = 0;
                pixel(part), part_n = 0;
            }
            continue;
        }
        if (params_n < (int)sizeof(params)) params[params_n] = data[i];
        const int n = ++params_n;
        if (command == 0x2A && n == 4) sc = (uint16_t)(params[0] << 8 | params[1]), ec = (uint16_t)(params[2] << 8 | params[3]);
        if (command == 0x2B && n == 4) sp = (uint16_t)(params[0] << 8 | params[1]), ep = (uint16_t)(params[2] << 8 | params[3]);
        if (command == 0x36 && n == 1) madctl = params[0];
        if (command == 0x3A && n == 1) colmod = params[0];
    }
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

/* where a point of the screen shows on the upright glass: the axes swapped first, then x and y mirrored */
static void on_glass(const dgx_screen_t *scr, int x, int y, int *gx, int *gy)
{
    const int ax = scr->swap_xy ? y : x, ay = scr->swap_xy ? x : y;
    *gx          = scr->dir_x == DgxScreenRightLeft ? GW - 1 - ax : ax;
    *gy          = scr->dir_y == DgxScreenBottomTop ? MH - 1 - ay : ay;
}

/* points at the corners and inside, each of its own color, found on the glass where the turn puts them */
static void turns(dgx_screen_t *scr)
{
    for (int turn = 0; turn < 8; ++turn) {
        const bool swap = (turn & 4) != 0;
        dgx_screen_orientation(scr, turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight, turn & 2 ? DgxScreenBottomTop : DgxScreenTopBottom, swap);
        CHECK(scr->width == (swap ? MH : GW) && scr->height == (swap ? GW : MH));
        const int pts[5][2] = {{0, 0}, {scr->width - 1, 0}, {0, scr->height - 1}, {scr->width - 1, scr->height - 1}, {37, 101}};
        for (int i = 0; i < 5; ++i) {
            const uint16_t color = (uint16_t)(0x1111 * (turn + 1) + i);
            dgx_set_pixel(scr, pts[i][0], pts[i][1], color);
            int gx, gy;
            on_glass(scr, pts[i][0], pts[i][1], &gx, &gy);
            CHECK(memory[gy][GX + gx][0] == color >> 8 && memory[gy][GX + gx][1] == (color & 0xff));
        }
    }
    CHECK(outside == 0);
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
    CHECK(dgx_nv3007_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 12, DgxScreenRGB, DGX_NV3007_142X428) == NULL && commands == 0);
    CHECK(dgx_nv3007_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 24, DgxScreenRGB, DGX_NV3007_142X428) == NULL && commands == 0);
    CHECK(dgx_nv3007_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, (dgx_nv3007_resolution_t)0) == NULL && commands == 0);
    CHECK(dgx_nv3007_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, (dgx_nv3007_resolution_t)3) == NULL && commands == 0);
    /* the screen it starts as: portrait, or turned by dgx_nv3007_init() to landscape */
    static const struct {
        dgx_nv3007_resolution_t resolution;
        int                     width, height;
        uint8_t                 madctl;
    } panels[] = {
        {DGX_NV3007_142X428, 142, 428, 0x00},
        {DGX_NV3007_428X142, 428, 142, 0x60},
    };
    for (size_t p = 0; p < sizeof(panels) / sizeof(panels[0]); ++p) {
        madctl = 0xff, colmod = 0, inverted = true, display_on = false, outside = 0;
        dgx_screen_t *scr = dgx_nv3007_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB, panels[p].resolution);
        CHECK(scr && scr->width == panels[p].width && scr->height == panels[p].height);
        CHECK(madctl == panels[p].madctl);
        CHECK(colmod == 0x05);
        CHECK(!inverted);
        CHECK(display_on);
        CHECK(outside == 0); /* the clear of the start covers the glass alone */
        if (!scr) continue;
        turns(scr);
        dgx_screen_destroy(&scr);
    }
    /* 18 bits a pixel; the colors turned the other way by the BGR bit */
    madctl = 0xff, colmod = 0;
    dgx_screen_t *scr = dgx_nv3007_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 18, DgxScreenRGB, DGX_NV3007_142X428);
    CHECK(scr && colmod == 0x06);
    dgx_screen_destroy(&scr);
    madctl = 0xff;
    scr    = dgx_nv3007_init(&bus, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenBGR, DGX_NV3007_142X428);
    CHECK(scr && madctl == 0x08);
    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
