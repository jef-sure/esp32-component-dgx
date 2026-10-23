/*
 * nv3041a.c
 *
 *  Created on: 23.10.2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include "drivers/nv3041a.h"

#include <stdlib.h>

#include "dgx_arch.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "dgx_spi_area_protocol.h"
#include "drivers/dgx_lcd_init.h"
#include "esp_log.h"

#define NV3041A_WIDTH  480
#define NV3041A_HEIGHT 272

#define NV3041A_SWRESET 0x01
#define NV3041A_SLPOUT  0x11
#define NV3041A_INVOFF  0x20
#define NV3041A_DISPON  0x29
#define NV3041A_CASET   0x2A
#define NV3041A_RASET   0x2B
#define NV3041A_RAMWR   0x2C
#define NV3041A_MADCTL  0x36
#define NV3041A_COLMOD  0x3A
#define NV3041A_PAGE    0xFF /* 0xA5 opens the registers of the maker, 0x00 closes them */

#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_BGR 0x08

/* the glass lies turned round against the memory of the controller: upright is both directions reversed */
#define NV3041A_MADCTL_UPRIGHT (MADCTL_MX | MADCTL_MY)

typedef struct _dgx_nv3041a_t
{
    dgx_screen_with_bus_t base;
    gpio_num_t            rst;
    gpio_num_t            backlight;
} dgx_nv3041a_t;

static const char TAG[] = "DGX NV3041A";

static uint8_t dgx_nv3041a_madctl(const dgx_screen_t *scr)
{
    uint8_t madctl = NV3041A_MADCTL_UPRIGHT;
    if (scr->dir_x == DgxScreenRightLeft) madctl ^= MADCTL_MX;
    if (scr->dir_y == DgxScreenBottomTop) madctl ^= MADCTL_MY;
    if (scr->swap_xy) madctl |= MADCTL_MV;
    if (scr->rgb_order == DgxScreenBGR) madctl |= MADCTL_BGR;
    return madctl;
}

static uint8_t *dgx_nv3041a_adj_madctl(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];
    (void)init_cmd;
    adj_data[0] = dgx_nv3041a_madctl(&scr->scr);
    return adj_data;
}

/*
 * The table of the maker of the panel, as its demos for the Guition
 * JC4827W543 send it: power, timings of the gates and sources, and gamma, in
 * its registers between the two writes of NV3041A_PAGE. The panel of that
 * board shows true colors with INVOFF, sent after the table. COLMOD 0x01 is
 * RGB565 on this controller, 0x00 would be RGB666; 0x41 = 0x03 is the width of
 * the bus to its memory, 16 bits.
 */
static const dgx_lcd_init_cmd_t nv3041a_init_cmds[] = {
    {NV3041A_SWRESET, 0, 0, 120, {0}},
    {NV3041A_PAGE, 0, 1, 0, {0xA5}},
    {NV3041A_MADCTL, dgx_nv3041a_adj_madctl, 1, 0, {0}},
    {NV3041A_COLMOD, 0, 1, 0, {0x01}},
    {0x41, 0, 1, 0, {0x03}},
    {0x44, 0, 1, 0, {0x15}},
    {0x45, 0, 1, 0, {0x15}},
    {0x7D, 0, 1, 0, {0x03}},
    {0xC1, 0, 1, 0, {0xAB}},
    {0xC2, 0, 1, 0, {0x17}},
    {0xC3, 0, 1, 0, {0x10}},
    {0xC6, 0, 1, 0, {0x3A}},
    {0xC7, 0, 1, 0, {0x25}},
    {0xC8, 0, 1, 0, {0x11}},
    {0x7A, 0, 1, 0, {0x49}},
    {0x6F, 0, 1, 0, {0x2F}},
    {0x78, 0, 1, 0, {0x4B}},
    {0xC9, 0, 1, 0, {0x00}},
    {0x67, 0, 1, 0, {0x33}},
    {0x51, 0, 1, 0, {0x4B}},
    {0x52, 0, 1, 0, {0x7C}},
    {0x53, 0, 1, 0, {0x1C}},
    {0x54, 0, 1, 0, {0x77}},
    {0x46, 0, 1, 0, {0x0A}},
    {0x47, 0, 1, 0, {0x2A}},
    {0x48, 0, 1, 0, {0x0A}},
    {0x49, 0, 1, 0, {0x1A}},
    {0x56, 0, 1, 0, {0x43}},
    {0x57, 0, 1, 0, {0x42}},
    {0x58, 0, 1, 0, {0x3C}},
    {0x59, 0, 1, 0, {0x64}},
    {0x5A, 0, 1, 0, {0x41}},
    {0x5B, 0, 1, 0, {0x3C}},
    {0x5C, 0, 1, 0, {0x02}},
    {0x5D, 0, 1, 0, {0x3C}},
    {0x5E, 0, 1, 0, {0x1F}},
    {0x60, 0, 1, 0, {0x80}},
    {0x61, 0, 1, 0, {0x3F}},
    {0x62, 0, 1, 0, {0x21}},
    {0x63, 0, 1, 0, {0x07}},
    {0x64, 0, 1, 0, {0xE0}},
    {0x65, 0, 1, 0, {0x01}},
    {0xCA, 0, 1, 0, {0x20}},
    {0xCB, 0, 1, 0, {0x52}},
    {0xCC, 0, 1, 0, {0x10}},
    {0xCD, 0, 1, 0, {0x42}},
    {0xD0, 0, 1, 0, {0x20}},
    {0xD1, 0, 1, 0, {0x52}},
    {0xD2, 0, 1, 0, {0x10}},
    {0xD3, 0, 1, 0, {0x42}},
    {0xD4, 0, 1, 0, {0x0A}},
    {0xD5, 0, 1, 0, {0x32}},
    {0x80, 0, 1, 0, {0x04}},
    {0xA0, 0, 1, 0, {0x00}},
    {0x81, 0, 1, 0, {0x07}},
    {0xA1, 0, 1, 0, {0x05}},
    {0x82, 0, 1, 0, {0x06}},
    {0xA2, 0, 1, 0, {0x04}},
    {0x86, 0, 1, 0, {0x2C}},
    {0xA6, 0, 1, 0, {0x2A}},
    {0x87, 0, 1, 0, {0x46}},
    {0xA7, 0, 1, 0, {0x44}},
    {0x83, 0, 1, 0, {0x39}},
    {0xA3, 0, 1, 0, {0x39}},
    {0x84, 0, 1, 0, {0x3A}},
    {0xA4, 0, 1, 0, {0x3A}},
    {0x85, 0, 1, 0, {0x3F}},
    {0xA5, 0, 1, 0, {0x3F}},
    {0x88, 0, 1, 0, {0x08}},
    {0xA8, 0, 1, 0, {0x08}},
    {0x89, 0, 1, 0, {0x0F}},
    {0xA9, 0, 1, 0, {0x0F}},
    {0x8A, 0, 1, 0, {0x17}},
    {0xAA, 0, 1, 0, {0x17}},
    {0x8B, 0, 1, 0, {0x10}},
    {0xAB, 0, 1, 0, {0x10}},
    {0x8C, 0, 1, 0, {0x16}},
    {0xAC, 0, 1, 0, {0x16}},
    {0x8D, 0, 1, 0, {0x14}},
    {0xAD, 0, 1, 0, {0x14}},
    {0x8E, 0, 1, 0, {0x11}},
    {0xAE, 0, 1, 0, {0x11}},
    {0x8F, 0, 1, 0, {0x14}},
    {0xAF, 0, 1, 0, {0x14}},
    {0x90, 0, 1, 0, {0x06}},
    {0xB0, 0, 1, 0, {0x06}},
    {0x91, 0, 1, 0, {0x0F}},
    {0xB1, 0, 1, 0, {0x0F}},
    {0x92, 0, 1, 0, {0x16}},
    {0xB2, 0, 1, 0, {0x16}},
    {NV3041A_PAGE, 0, 1, 0, {0x00}},
    {NV3041A_INVOFF, 0, 0, 0, {0}}, // after a reset the colors are inverted, INVON leaves them so
    {NV3041A_SLPOUT, 0, 0, 120, {0}},
    {0, 0, 0xff, 0, {0}} // Stop
};

/* the display turned on once its memory is cleared, so what was left in it is never seen */
static const dgx_lcd_init_cmd_t nv3041a_on_cmds[] = {
    {NV3041A_DISPON, 0, 0, 0, {0}},
    {0, 0, 0xff, 0, {0}} // Stop
};

dgx_screen_t *dgx_nv3041a_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo)
{
    if (color_bits != 16) {
        ESP_LOGE(TAG, "%u-bit color is not set up by this driver, only 16", (unsigned)color_bits);
        return NULL;
    }
    dgx_nv3041a_t *scr = (dgx_nv3041a_t *)calloc(1, sizeof(dgx_nv3041a_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }
    scr->rst                      = rst;
    scr->backlight                = backlight;
    scr->base.bus                 = bus;
    scr->base.scr.width           = NV3041A_WIDTH;
    scr->base.scr.height          = NV3041A_HEIGHT;
    scr->base.scr.dir_x           = DgxScreenLeftRight;
    scr->base.scr.dir_y           = DgxScreenTopBottom;
    scr->base.scr.swap_xy         = false;
    scr->base.scr.color_bits      = color_bits;
    scr->base.scr.rgb_order       = cbo;
    scr->base.scr.screen_name     = "NV3041A";
    scr->base.scr.screen_subtype  = DgxPhysicalScreenWithBus;
    dgx_screen_with_bus_init_area(&scr->base, NV3041A_CASET, NV3041A_RASET, NV3041A_RAMWR, 0, DGX_SCREEN_AREA_PROTO_STD16);
    if (backlight >= 0) {
        /* dark until there is a picture: the panel shows noise while it starts */
        dgx_gpio_set_direction(backlight, GPIO_MODE_OUTPUT);
        dgx_nv3041a_backlight((dgx_screen_t *)scr, false);
    }
    if (rst >= 0) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(120);
    }
    dgx_lcd_init(&scr->base, nv3041a_init_cmds);
    dgx_scr_init_slow_bus_optimized_funcs((dgx_screen_t *)scr);
    dgx_fill_rectangle((dgx_screen_t *)scr, 0, 0, NV3041A_WIDTH, NV3041A_HEIGHT, 0);
    scr->base.scr.wait_buffer((dgx_screen_t *)scr);
    dgx_lcd_init(&scr->base, nv3041a_on_cmds);
    if (backlight >= 0) dgx_nv3041a_backlight((dgx_screen_t *)scr, true);
    return (dgx_screen_t *)scr;
}

void dgx_nv3041a_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_nv3041a_t *scr = (dgx_nv3041a_t *)_scr;
    _scr->dir_x        = dir_x;
    _scr->dir_y        = dir_y;
    _scr->swap_xy      = swap_xy;
    _scr->width        = swap_xy ? NV3041A_HEIGHT : NV3041A_WIDTH;
    _scr->height       = swap_xy ? NV3041A_WIDTH : NV3041A_HEIGHT;
    uint8_t data[1]    = {dgx_nv3041a_madctl(_scr)};
    scr->base.bus->write_command(scr->base.bus, NV3041A_MADCTL);
    scr->base.bus->write_data(scr->base.bus, data, 8);
}

esp_err_t dgx_nv3041a_backlight(dgx_screen_t *_scr, bool on)
{
    dgx_nv3041a_t *scr = (dgx_nv3041a_t *)_scr;
    if (scr->backlight < 0) return ESP_ERR_NOT_SUPPORTED;
    dgx_gpio_set_level(scr->backlight, on ? 1 : 0);
    return ESP_OK;
}
