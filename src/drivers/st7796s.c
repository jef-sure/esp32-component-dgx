/*
 * st7796s.c
 *
 *  The ST7796S, the controller of 320x480 panels such as the common 4" SPI
 *  modules. The glass fills the controller memory, so the window always
 *  starts at the memory origin and every orientation is a change of MADCTL
 *  alone.
 *
 *  The init sequence is the one TFT_eSPI (TFT_Drivers/ST7796_Init.h),
 *  LovyanGFX (Panel_ST7796.hpp) and Espressif's esp_lcd_st7796 component
 *  share: command set 2 unlocked by its two keys, then display inversion,
 *  display function, output, power, VCOM and gamma, then command set 2
 *  locked again. The order and the delays are those of TFT_eSPI. A module
 *  with other glass passes its own table to dgx_panel_init() in a copy of
 *  dgx_panel_st7796s. Such tables usually differ in display inversion
 *  (INVON on panels whose colors come out inverted), VCOM (C5h) and gamma
 *  (E0h, E1h). The color order is set by the cbo argument of
 *  dgx_st7796s_init().
 */

#include "drivers/st7796s.h"

#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "esp_log.h"

// ST7796S commands, named as in the datasheet
#define ST7796S_SWRESET 0x01 // Software reset
#define ST7796S_SLPIN   0x10 // Sleep in
#define ST7796S_SLPOUT  0x11 // Sleep out
#define ST7796S_NORON   0x13 // Normal display mode on
#define ST7796S_INVOFF  0x20 // Display inversion off
#define ST7796S_INVON   0x21 // Display inversion on
#define ST7796S_DISPOFF 0x28 // Display off
#define ST7796S_DISPON  0x29 // Display on
#define ST7796S_CASET   0x2A // Column address set
#define ST7796S_RASET   0x2B // Row address set
#define ST7796S_RAMWR   0x2C // Memory write
#define ST7796S_MADCTL  0x36 // Memory data access control
#define ST7796S_COLMOD  0x3A // Interface pixel format

#define ST7796S_DIC     0xB4 // Display inversion control
#define ST7796S_DFC     0xB6 // Display function control
#define ST7796S_PWR2    0xC1 // Power control 2
#define ST7796S_PWR3    0xC2 // Power control 3
#define ST7796S_VCMPCTL 0xC5 // VCOM control
#define ST7796S_PGC     0xE0 // Positive gamma control
#define ST7796S_NGC     0xE1 // Negative gamma control
#define ST7796S_DOCA    0xE8 // Display output control adjust
#define ST7796S_CSCON   0xF0 // Command set control

#define ST7796S_MADCTL_MY  0x80
#define ST7796S_MADCTL_MX  0x40
#define ST7796S_MADCTL_MV  0x20
#define ST7796S_MADCTL_ML  0x10
#define ST7796S_MADCTL_BGR 0x08

static const char TAG[] = "DGX ST7796S";

/*
 * Reset and sleep out first, then the vendor commands with command set 2
 * unlocked. F0h takes C3h and 96h to unlock parts I and II of command set 2,
 * and 3Ch and 69h to lock them again. The display is turned on later, by
 * st7796s_on_cmds, once the memory is cleared.
 */
static const dgx_lcd_init_cmd_t st7796s_init_cmds[] = {
    {ST7796S_SWRESET, 0,                    0,    120, {0}                                                                     }, //
    {ST7796S_SLPOUT,  0,                    0,    120, {0}                                                                     }, //
    {ST7796S_CSCON,   0,                    1,    0,   {0xC3}                                                                  }, // unlock command set 2, part I
    {ST7796S_CSCON,   0,                    1,    0,   {0x96}                                                                  }, // unlock command set 2, part II
    {ST7796S_MADCTL,  dgx_panel_adj_madctl, 1,    0,   {0}                                                                     }, // upright, in the color order of the screen
    {ST7796S_COLMOD,  dgx_panel_adj_colmod, 1,    0,   {0x55}                                                                  }, // 05h for 16 bits, 06h for 18
    {ST7796S_DIC,     0,                    1,    0,   {0x01}                                                                  }, // 1-dot inversion
    {ST7796S_DFC,     0,                    3,    0,   {0x80, 0x02, 0x3B}                                                      }, // memory written by the MCU interface, 480 lines
    {ST7796S_DOCA,    0,                    8,    0,   {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}                        }, //
    {ST7796S_PWR2,    0,                    1,    0,   {0x06}                                                                  }, //
    {ST7796S_PWR3,    0,                    1,    0,   {0xA7}                                                                  }, //
    {ST7796S_VCMPCTL, 0,                    1,    120, {0x18}                                                                  }, // VCOM
    {ST7796S_PGC,     0,                    14,   0,   {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}}, //
    {ST7796S_NGC,     0,                    14,   120, {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}}, //
    {ST7796S_CSCON,   0,                    1,    0,   {0x3C}                                                                  }, // lock command set 2, part I
    {ST7796S_CSCON,   0,                    1,    0,   {0x69}                                                                  }, // lock command set 2, part II
    {ST7796S_INVOFF,  0,                    0,    0,   {0}                                                                     }, // the reset default; INVON for a panel whose colors come out inverted
    {0,               0,                    0xff, 0,   {0}                                                                     }  // Stop
};

/* the display is turned on once its memory is cleared, so whatever was left in it is never shown */
static const dgx_lcd_init_cmd_t st7796s_on_cmds[] = {
    {ST7796S_DISPON, 0, 0,    20, {0}}, //
    {0,              0, 0xff, 0,  {0}}  // Stop
};

/*
 * The controller memory is 320 columns by 480 rows, the size of the glass.
 * Upright is portrait with MADCTL 0x48, MX and the BGR bit: the value
 * TFT_eSPI writes at init. The BGR bit tells the controller the glass has
 * BGR color filters; DgxScreenBGR as the cbo of dgx_st7796s_init() clears it
 * for a module that shows red and blue swapped.
 */
const dgx_panel_t dgx_panel_st7796s = {
    .name           = "ST7796S",
    .width          = 320,
    .height         = 480,
    .colmod         = {0, 0x55, 0x66},
    .madctl         = ST7796S_MADCTL_MX | ST7796S_MADCTL_BGR,
    .reset_low_ms   = 10,
    .reset_after_ms = 120,
    .flags          = DGX_PANEL_CLEAR_FIRST,
    .init           = st7796s_init_cmds,
    .on             = st7796s_on_cmds,
};

dgx_screen_t *dgx_st7796s_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
        dgx_st7796s_resolution_t resolution)
{
    if (resolution != DGX_ST7796S_320X480 && resolution != DGX_ST7796S_480X320) {
        ESP_LOGE(TAG, "Unsupported ST7796S resolution %d", (int)resolution);
        return NULL;
    }
    dgx_screen_t *scr = dgx_panel_init(bus, &dgx_panel_st7796s, rst, backlight, color_bits, cbo);
    /*
     * Landscape is the upright picture turned a quarter, as rotation 1 of
     * TFT_eSPI: MADCTL becomes MV. dgx_panel_init() sets the screen up
     * upright; dgx_screen_orientation() then changes MADCTL and the size of
     * the screen together.
     */
    if (scr && resolution == DGX_ST7796S_480X320)
        dgx_screen_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true);
    return scr;
}
