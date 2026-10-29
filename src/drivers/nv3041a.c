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
#include "drivers/dgx_panel.h"
#include "esp_log.h"

#define NV3041A_WIDTH  480
#define NV3041A_HEIGHT 272

#define NV3041A_SWRESET 0x01
#define NV3041A_SLPOUT  0x11
#define NV3041A_INVON   0x21
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

/*
 * The table of the maker of the panel, as its demos for the Guition
 * JC4827W543 send it: power, timings of the gates and sources, and gamma, in
 * its registers between the two writes of NV3041A_PAGE. The panel of that
 * board is IPS: its true colors need inversion on, so INVON follows after
 * SLPOUT. COLMOD 0x01 is
 * RGB565 on this controller, 0x00 would be RGB666; 0x41 = 0x03 is the width of
 * the bus to its memory, 16 bits.
 */
static const dgx_lcd_init_cmd_t nv3041a_init_cmds[] = {
    {NV3041A_SWRESET, 0, 0, 120, {0}},
    {NV3041A_PAGE, 0, 1, 0, {0xA5}},
    {NV3041A_MADCTL, dgx_panel_adj_madctl, 1, 0, {0}},
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
    {NV3041A_SLPOUT, 0, 0, 120, {0}},
    /* sleep out loads the factory defaults into the registers during its 120 ms
     * (7.2.9): what the panel is to keep goes after it. MADCTL written only
     * before it was lost, and the orientation the caller asked for never
     * reached the running panel; the maker's demo writes it again after the
     * whole init. */
    {NV3041A_MADCTL, dgx_panel_adj_madctl, 1, 0, {0}},
    /* the glass of this board is IPS: its colors are inverted without INVON, which must
     * come after SLPOUT — the maker's defaults reload into the registers during its 120ms */
    {NV3041A_INVON, 0, 0, 20, {0}}, // display commands take effect from the next frame (7.2.10), ~17ms at 60Hz
    {0, 0, 0xff, 0, {0}} // Stop
};

/* the display turned on once its memory is cleared, so what was left in it is never seen */
static const dgx_lcd_init_cmd_t nv3041a_on_cmds[] = {
    {NV3041A_DISPON, 0, 0, 100, {0}}, // the maker's demo waits 100 ms before anything else
    {0, 0, 0xff, 0, {0}} // Stop
};

/*
 * Only RGB565 is set up (COLMOD 0x01 of the table). The panel shows noise
 * until its memory is written: dark until it is black, then turned on.
 */
const dgx_panel_t dgx_panel_nv3041a = {
    .name           = "NV3041A",
    .width          = NV3041A_WIDTH,
    .height         = NV3041A_HEIGHT,
    .colmod         = {0, 0x01, 0},
    .madctl         = NV3041A_MADCTL_UPRIGHT,
    .reset_high_ms  = 10,
    .reset_low_ms   = 10,
    .reset_after_ms = 120,
    .flags          = DGX_PANEL_CLEAR_FIRST,
    .init           = nv3041a_init_cmds,
    .on             = nv3041a_on_cmds,
};

dgx_screen_t *dgx_nv3041a_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo)
{
    return dgx_panel_init(bus, &dgx_panel_nv3041a, rst, backlight, color_bits, cbo);
}

void dgx_nv3041a_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_panel_orientation(scr, dir_x, dir_y, swap_xy);
}

esp_err_t dgx_nv3041a_backlight(dgx_screen_t *scr, bool on)
{
    return dgx_panel_backlight(scr, on);
}
