#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "dgx_arch.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "drivers/st7735.h"
#include "dgx_screen_functions.h"

// ST7735 specific commands used in init
#define ST7735_NOP          0x00
#define ST7735_SWRESET      0x01
#define ST7735_RDDID        0x04
#define ST7735_RDDST        0x09

#define ST7735_SLPIN        0x10
#define ST7735_SLPOUT       0x11
#define ST7735_PTLON        0x12
#define ST7735_NORON        0x13

#define ST7735_INVOFF       0x20
#define ST7735_INVON        0x21
#define ST7735_DISPOFF      0x28
#define ST7735_DISPON       0x29
#define ST7735_CASET        0x2A
#define ST7735_RASET        0x2B
#define ST7735_RAMWR        0x2C
#define ST7735_RAMRD        0x2E

#define ST7735_PTLAR        0x30
#define ST7735_VSCRDEF      0x33
#define ST7735_COLMOD       0x3A
#define ST7735_MADCTL       0x36
#define ST7735_VSCRSADD     0x37

#define ST7735_FRMCTR1      0xB1
#define ST7735_FRMCTR2      0xB2
#define ST7735_FRMCTR3      0xB3
#define ST7735_INVCTR       0xB4
#define ST7735_DISSET5      0xB6

#define ST7735_PWCTR1       0xC0
#define ST7735_PWCTR2       0xC1
#define ST7735_PWCTR3       0xC2
#define ST7735_PWCTR4       0xC3
#define ST7735_PWCTR5       0xC4
#define ST7735_VMCTR1       0xC5

#define ST7735_RDID1        0xDA
#define ST7735_RDID2        0xDB
#define ST7735_RDID3        0xDC
#define ST7735_RDID4        0xDD

#define ST7735_PWCTR6       0xFC

#define ST7735_GMCTRP1      0xE0
#define ST7735_GMCTRN1      0xE1

#define ST77XX_MADCTL_MY    0x80
#define ST77XX_MADCTL_MX    0x40
#define ST77XX_MADCTL_MV    0x20
#define ST77XX_MADCTL_ML    0x10
#define ST77XX_MADCTL_RGB   0x00
#define ST77XX_MADCTL_BGR   0x08

static const dgx_lcd_init_cmd_t st7735_init_cmds[] = {
//
        { ST7735_SWRESET, 0, 0, 150, { 0 } }, //
        { ST7735_SLPOUT, 0, 0, 255, { 0 } }, //
        { ST7735_FRMCTR1, 0, 3, 0, { 0x01, 0x2C, 0x2D } }, //
        { ST7735_FRMCTR2, 0, 3, 0, { 0x01, 0x2C, 0x2D } }, //
        { ST7735_FRMCTR3, 0, 6, 0, { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D } }, //
        { ST7735_INVCTR, 0, 1, 0, { 0x07 } }, //
        { ST7735_PWCTR1, 0, 3, 0, { 0xA2, 0x02, 0x84 } }, //
        { ST7735_PWCTR2, 0, 1, 0, { 0xC5 } }, //
        { ST7735_PWCTR3, 0, 2, 0, { 0x0A, 0x00 } }, //
        { ST7735_PWCTR4, 0, 2, 0, { 0x8A, 0x2A } }, //
        { ST7735_PWCTR5, 0, 2, 0, { 0x8A, 0xEE } }, //
        { ST7735_VMCTR1, 0, 1, 9, { 0x0E } }, //
        { ST7735_MADCTL, dgx_panel_adj_madctl, 1, 0, { 0x00 | 0x00 } }, // MY=0, MX=0, MV=0, ML=0, MH=0 | 00 - RGB, 08 - BGR
        { ST7735_COLMOD, dgx_panel_adj_colmod, 1, 0, { 0x06 } }, // Bits per Pixel: 3 - 12, 5 - 16, 6 - 18
        { ST7735_CASET, 0, 4, 0, { 0x00, 0x00, 0x00, 0x7F } }, //
        { ST7735_RASET, 0, 4, 0, { 0x00, 0x00, 0x00, 0x9F } }, //
        { ST7735_GMCTRP1, 0, 16, 0, { 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 } }, //
        { ST7735_GMCTRN1, 0, 16, 0, { 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 } }, //
        { ST7735_NORON, 0, 0, 10, { 0 } }, //
        { ST7735_DISPON, 0, 0, 100, { 0 } }, //  4: Main screen turn on, no args w/delay
        { 0, 0, 0xff, 0, { 0 } } // Stop
};

const dgx_panel_t dgx_panel_st7735 = {
    .name           = "ST 7735",
    .width          = 128,
    .height         = 160,
    .colmod         = {0x03, 0x05, 0x06},
    .reset_low_ms   = 100,
    .reset_after_ms = 100,
    .init           = st7735_init_cmds,
};

/*
 * The 80 columns of the 0.96" glass from column 24 of the 128 of the
 * memory, its 160 rows all of the 160: in the middle either way. Its
 * controller is set for 128x160, and it starts as the 128x160 does, with
 * no inversion, as Adafruit starts INITR_MINI160x80.
 */
const dgx_panel_t dgx_panel_st7735_80x160 = {
    .name           = "ST 7735",
    .width          = 80,
    .height         = 160,
    .memory_width   = 128,
    .memory_height  = 160,
    .x_offset       = 24,
    .y_offset       = 0,
    .colmod         = {0x03, 0x05, 0x06},
    .reset_low_ms   = 100,
    .reset_after_ms = 100,
    .init           = st7735_init_cmds,
};

static const char TAG[] = "DGX ST 7735";

//Initialize the display
dgx_screen_t* dgx_st7735_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
                              st7735_resolution_t resolution) {
    const dgx_panel_t *panel;
    switch (resolution) {
    case ST7735_RESOLUTION_128x160:
    case ST7735_RESOLUTION_160x128:
        panel = &dgx_panel_st7735;
        break;
    case ST7735_RESOLUTION_80x160:
    case ST7735_RESOLUTION_160x80:
        panel = &dgx_panel_st7735_80x160;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported ST7735 resolution %d", (int)resolution);
        return NULL;
    }
    dgx_screen_t *scr = dgx_panel_init(bus, panel, rst, backlight, color_bits, cbo);
    /* landscape is a turn, MV with MX, as on the ST7789; after the functions of the screen, which take it as not turned */
    if (scr && (resolution == ST7735_RESOLUTION_160x128 || resolution == ST7735_RESOLUTION_160x80))
        dgx_panel_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true);
    return scr;
}

void dgx_st7735_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy) {
    dgx_panel_orientation(scr, dir_x, dir_y, swap_xy);
}

esp_err_t dgx_st7735_backlight(dgx_screen_t *scr, bool on) {
    return dgx_panel_backlight(scr, on);
}
