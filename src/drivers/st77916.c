/*
 * st77916.c
 *
 *  The ST77916 of round 360x360 panels, on QSPI as a rule.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include "drivers/st77916.h"

#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"

#define ST77916_SWRESET 0x01
#define ST77916_DISPON  0x29
#define ST77916_VSCRDEF 0x33
#define ST77916_MADCTL  0x36
#define ST77916_VSCSAD  0x37
#define ST77916_COLMOD  0x3A

/*
 * The pixel format and the access control first, then the table of the maker
 * of the glass: its pages of registers (0xF0, 0xF1, 0xF2), power, timings and
 * gamma, TEON, INVON and SLPOUT with its 120 ms. It is
 * st77916_150_init_operations of Arduino_GFX (moononournation, BSD License),
 * seen right on the module "TFT 1.53 1.0" with the glass T150XV-C16-04, but
 * for VCOMS (0xB2): 0x28 here for its 0x2F, as with the higher voltage the
 * columns that hold bright pixels stay lighter than the black beside them.
 * Such a table is the glass's: on that module the table of Espressif's
 * esp_lcd_st77916 shows white stripes, and that of ESPHome's JC3636W518V2 the
 * lighter columns at once, its VCOM offsets (0xDD, 0xDE) being high.
 * A module of another glass gives its own to dgx_panel_init() in a copy of
 * dgx_panel_st77916. SWRESET comes first, so that a panel with no reset line
 * starts too.
 */
static const dgx_lcd_init_cmd_t st77916_init_cmds[] = {
    {ST77916_SWRESET, 0, 0, 120, {0}},
    {ST77916_MADCTL, dgx_panel_adj_madctl, 1, 0, {0}},
    {ST77916_COLMOD, dgx_panel_adj_colmod, 1, 0, {0x55}},
    {0xF0, 0, 1, 0, {0x28}},
    {0xF2, 0, 1, 0, {0x28}},
    {0x73, 0, 1, 0, {0xF0}},
    {0x7C, 0, 1, 0, {0xD1}},
    {0x83, 0, 1, 0, {0xE0}},
    {0x84, 0, 1, 0, {0x61}},
    {0xF2, 0, 1, 0, {0x82}},
    {0xF0, 0, 1, 0, {0x00}},
    {0xF0, 0, 1, 0, {0x01}},
    {0xF1, 0, 1, 0, {0x01}},
    {0xB0, 0, 1, 0, {0x69}},
    {0xB1, 0, 1, 0, {0x4A}},
    {0xB2, 0, 1, 0, {0x28}},
    {0xB3, 0, 1, 0, {0x01}},
    {0xB4, 0, 1, 0, {0x69}},
    {0xB5, 0, 1, 0, {0x45}},
    {0xB6, 0, 1, 0, {0xAB}},
    {0xB7, 0, 1, 0, {0x41}},
    {0xB8, 0, 1, 0, {0x86}},
    {0xB9, 0, 1, 0, {0x15}},
    {0xBA, 0, 1, 0, {0x00}},
    {0xBB, 0, 1, 0, {0x08}},
    {0xBC, 0, 1, 0, {0x08}},
    {0xBD, 0, 1, 0, {0x00}},
    {0xBE, 0, 1, 0, {0x00}},
    {0xBF, 0, 1, 0, {0x07}},
    {0xC0, 0, 1, 0, {0x80}},
    {0xC1, 0, 1, 0, {0x10}},
    {0xC2, 0, 1, 0, {0x37}},
    {0xC3, 0, 1, 0, {0x80}},
    {0xC4, 0, 1, 0, {0x10}},
    {0xC5, 0, 1, 0, {0x37}},
    {0xC6, 0, 1, 0, {0xA9}},
    {0xC7, 0, 1, 0, {0x41}},
    {0xC8, 0, 1, 0, {0x01}},
    {0xC9, 0, 1, 0, {0xA9}},
    {0xCA, 0, 1, 0, {0x41}},
    {0xCB, 0, 1, 0, {0x01}},
    {0xCC, 0, 1, 0, {0x7F}},
    {0xCD, 0, 1, 0, {0x7F}},
    {0xCE, 0, 1, 0, {0xFF}},
    {0xD0, 0, 1, 0, {0x91}},
    {0xD1, 0, 1, 0, {0x68}},
    {0xD2, 0, 1, 0, {0x68}},
    {0xF5, 0, 2, 0, {0x00, 0xA5}},
    {0xF1, 0, 1, 0, {0x10}},
    {0xF0, 0, 1, 0, {0x00}},
    {0xF0, 0, 1, 0, {0x02}},
    {0xE0, 0, 14, 0, {0xF0, 0x10, 0x18, 0x0D, 0x0C, 0x38, 0x3E, 0x44, 0x51, 0x39, 0x15, 0x15, 0x30, 0x34}},
    {0xE1, 0, 14, 0, {0xF0, 0x0F, 0x17, 0x0D, 0x0B, 0x07, 0x3E, 0x33, 0x51, 0x39, 0x15, 0x15, 0x30, 0x34}},
    {0xF0, 0, 1, 0, {0x10}},
    {0xF3, 0, 1, 0, {0x10}},
    {0xE0, 0, 1, 0, {0x08}},
    {0xE1, 0, 1, 0, {0x00}},
    {0xE2, 0, 1, 0, {0x00}},
    {0xE3, 0, 1, 0, {0x00}},
    {0xE4, 0, 1, 0, {0xE0}},
    {0xE5, 0, 1, 0, {0x06}},
    {0xE6, 0, 1, 0, {0x21}},
    {0xE7, 0, 1, 0, {0x03}},
    {0xE8, 0, 1, 0, {0x05}},
    {0xE9, 0, 1, 0, {0x02}},
    {0xEA, 0, 1, 0, {0xE9}},
    {0xEB, 0, 1, 0, {0x00}},
    {0xEC, 0, 1, 0, {0x00}},
    {0xED, 0, 1, 0, {0x14}},
    {0xEE, 0, 1, 0, {0xFF}},
    {0xEF, 0, 1, 0, {0x00}},
    {0xF8, 0, 1, 0, {0xFF}},
    {0xF9, 0, 1, 0, {0x00}},
    {0xFA, 0, 1, 0, {0x00}},
    {0xFB, 0, 1, 0, {0x30}},
    {0xFC, 0, 1, 0, {0x00}},
    {0xFD, 0, 1, 0, {0x00}},
    {0xFE, 0, 1, 0, {0x00}},
    {0xFF, 0, 1, 0, {0x00}},
    {0x60, 0, 1, 0, {0x40}},
    {0x61, 0, 1, 0, {0x05}},
    {0x62, 0, 1, 0, {0x00}},
    {0x63, 0, 1, 0, {0x42}},
    {0x64, 0, 1, 0, {0xDA}},
    {0x65, 0, 1, 0, {0x00}},
    {0x66, 0, 1, 0, {0x00}},
    {0x67, 0, 1, 0, {0x00}},
    {0x68, 0, 1, 0, {0x00}},
    {0x69, 0, 1, 0, {0x00}},
    {0x6A, 0, 1, 0, {0x00}},
    {0x6B, 0, 1, 0, {0x00}},
    {0x70, 0, 1, 0, {0x40}},
    {0x71, 0, 1, 0, {0x04}},
    {0x72, 0, 1, 0, {0x00}},
    {0x73, 0, 1, 0, {0x42}},
    {0x74, 0, 1, 0, {0xD9}},
    {0x75, 0, 1, 0, {0x00}},
    {0x76, 0, 1, 0, {0x00}},
    {0x77, 0, 1, 0, {0x00}},
    {0x78, 0, 1, 0, {0x00}},
    {0x79, 0, 1, 0, {0x00}},
    {0x7A, 0, 1, 0, {0x00}},
    {0x7B, 0, 1, 0, {0x00}},
    {0x80, 0, 1, 0, {0x48}},
    {0x81, 0, 1, 0, {0x00}},
    {0x82, 0, 1, 0, {0x07}},
    {0x83, 0, 1, 0, {0x02}},
    {0x84, 0, 1, 0, {0xD7}},
    {0x85, 0, 1, 0, {0x04}},
    {0x86, 0, 1, 0, {0x00}},
    {0x87, 0, 1, 0, {0x00}},
    {0x88, 0, 1, 0, {0x48}},
    {0x89, 0, 1, 0, {0x00}},
    {0x8A, 0, 1, 0, {0x09}},
    {0x8B, 0, 1, 0, {0x02}},
    {0x8C, 0, 1, 0, {0xD9}},
    {0x8D, 0, 1, 0, {0x04}},
    {0x8E, 0, 1, 0, {0x00}},
    {0x8F, 0, 1, 0, {0x00}},
    {0x90, 0, 1, 0, {0x48}},
    {0x91, 0, 1, 0, {0x00}},
    {0x92, 0, 1, 0, {0x0B}},
    {0x93, 0, 1, 0, {0x02}},
    {0x94, 0, 1, 0, {0xDB}},
    {0x95, 0, 1, 0, {0x04}},
    {0x96, 0, 1, 0, {0x00}},
    {0x97, 0, 1, 0, {0x00}},
    {0x98, 0, 1, 0, {0x48}},
    {0x99, 0, 1, 0, {0x00}},
    {0x9A, 0, 1, 0, {0x0D}},
    {0x9B, 0, 1, 0, {0x02}},
    {0x9C, 0, 1, 0, {0xDD}},
    {0x9D, 0, 1, 0, {0x04}},
    {0x9E, 0, 1, 0, {0x00}},
    {0x9F, 0, 1, 0, {0x00}},
    {0xA0, 0, 1, 0, {0x48}},
    {0xA1, 0, 1, 0, {0x00}},
    {0xA2, 0, 1, 0, {0x06}},
    {0xA3, 0, 1, 0, {0x02}},
    {0xA4, 0, 1, 0, {0xD6}},
    {0xA5, 0, 1, 0, {0x04}},
    {0xA6, 0, 1, 0, {0x00}},
    {0xA7, 0, 1, 0, {0x00}},
    {0xA8, 0, 1, 0, {0x48}},
    {0xA9, 0, 1, 0, {0x00}},
    {0xAA, 0, 1, 0, {0x08}},
    {0xAB, 0, 1, 0, {0x02}},
    {0xAC, 0, 1, 0, {0xD8}},
    {0xAD, 0, 1, 0, {0x04}},
    {0xAE, 0, 1, 0, {0x00}},
    {0xAF, 0, 1, 0, {0x00}},
    {0xB0, 0, 1, 0, {0x48}},
    {0xB1, 0, 1, 0, {0x00}},
    {0xB2, 0, 1, 0, {0x0A}},
    {0xB3, 0, 1, 0, {0x02}},
    {0xB4, 0, 1, 0, {0xDA}},
    {0xB5, 0, 1, 0, {0x04}},
    {0xB6, 0, 1, 0, {0x00}},
    {0xB7, 0, 1, 0, {0x00}},
    {0xB8, 0, 1, 0, {0x48}},
    {0xB9, 0, 1, 0, {0x00}},
    {0xBA, 0, 1, 0, {0x0C}},
    {0xBB, 0, 1, 0, {0x02}},
    {0xBC, 0, 1, 0, {0xDC}},
    {0xBD, 0, 1, 0, {0x04}},
    {0xBE, 0, 1, 0, {0x00}},
    {0xBF, 0, 1, 0, {0x00}},
    {0xC0, 0, 1, 0, {0x10}},
    {0xC1, 0, 1, 0, {0x47}},
    {0xC2, 0, 1, 0, {0x56}},
    {0xC3, 0, 1, 0, {0x65}},
    {0xC4, 0, 1, 0, {0x74}},
    {0xC5, 0, 1, 0, {0x88}},
    {0xC6, 0, 1, 0, {0x99}},
    {0xC7, 0, 1, 0, {0x01}},
    {0xC8, 0, 1, 0, {0xBB}},
    {0xC9, 0, 1, 0, {0xAA}},
    {0xD0, 0, 1, 0, {0x10}},
    {0xD1, 0, 1, 0, {0x47}},
    {0xD2, 0, 1, 0, {0x56}},
    {0xD3, 0, 1, 0, {0x65}},
    {0xD4, 0, 1, 0, {0x74}},
    {0xD5, 0, 1, 0, {0x88}},
    {0xD6, 0, 1, 0, {0x99}},
    {0xD7, 0, 1, 0, {0x01}},
    {0xD8, 0, 1, 0, {0xBB}},
    {0xD9, 0, 1, 0, {0xAA}},
    {0xF3, 0, 1, 0, {0x01}},
    {0xF0, 0, 1, 0, {0x00}},
    {0x35, 0, 1, 0, {0x00}},
    {0x21, 0, 0, 0, {0}},
    {0x11, 0, 0, 120, {0}},
    {0, 0, 0xff, 0, {0}} // Stop
};

/* the display turned on once its memory is cleared, so what was left in it is never seen */
static const dgx_lcd_init_cmd_t st77916_on_cmds[] = {
    {ST77916_DISPON, 0, 0, 0, {0}},
    {0, 0, 0xff, 0, {0}} // Stop
};

/*
 * The memory of the controller has 390 rows by its datasheet, but with the
 * rows going up the glass stays at the same 360 addresses: counting them from
 * the end of the 390, as the ST7789 has its 240 in 320, moved the picture by
 * 30 rows on a panel. Its scroll goes round those 360 rows too.
 */
const dgx_panel_t dgx_panel_st77916 = {
    .name             = "ST77916",
    .width            = 360,
    .height           = 360,
    .colmod           = {0, 0x55, 0x66},
    .reset_low_ms     = 10,
    .reset_after_ms   = 120,
    .flags            = DGX_PANEL_CLEAR_FIRST,
    .scroll_lines     = 360,
    .scroll_area_cmd  = ST77916_VSCRDEF,
    .scroll_start_cmd = ST77916_VSCSAD,
    .init             = st77916_init_cmds,
    .on               = st77916_on_cmds,
};

dgx_screen_t *dgx_st77916_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo)
{
    return dgx_panel_init(bus, &dgx_panel_st77916, rst, backlight, color_bits, cbo);
}

void dgx_st77916_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_panel_orientation(scr, dir_x, dir_y, swap_xy);
}

esp_err_t dgx_st77916_backlight(dgx_screen_t *scr, bool on)
{
    return dgx_panel_backlight(scr, on);
}
