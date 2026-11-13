#pragma once
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"
#include "drivers/dgx_panel.h"
#include "driver/gpio.h"

/**
 * The panels of an ST7735. A glass of 128x160 fills the memory of the
 * controller. The controller of the 0.96" IPS glass of 80x160 is set for
 * 128x160 on its module, and the glass lies at column 24, in the middle
 * either way; its colors need no inversion and usually go in BGR order,
 * DgxScreenBGR. This is the module Adafruit starts as INITR_MINI160x80. A
 * module that shows cyan for red and noise along two edges is of the other
 * kind, INITR_MINI160x80_PLUGIN: see "A panel of your own module" in
 * docs/panels-en.md.
 */
typedef enum {
    ST7735_RESOLUTION_128x160 = 1,
    ST7735_RESOLUTION_160x128 = 2, /**< 128x160 turned by dgx_st7735_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true) */
    ST7735_RESOLUTION_80x160  = 3, /**< the 0.96" glass, upright */
    ST7735_RESOLUTION_160x80  = 4, /**< the 0.96" glass turned as 160x128 is */
} st7735_resolution_t;

/**
 * @brief Create an ST7735 panel screen on top of a bus (SPI or P8).
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_p8_init().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param backlight  Backlight GPIO, high for on (use GPIO_NUM_NC if the panel has no backlight control). It is on once the
 *                   panel is set up; dgx_st7735_backlight() turns it off and on.
 * @param color_bits Pixel depth in bits: 16 for RGB565, the usual one, 12 or 18.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @param resolution The panel (see st7735_resolution_t).
 * @return Newly allocated screen, or NULL on failure, an unknown resolution or another depth. Free with dgx_screen_destroy().
 */
dgx_screen_t* dgx_st7735_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
                              st7735_resolution_t resolution);

/**
 * @brief Reprogram the controller scan direction (rotation/mirroring).
 *
 * @param scr     Screen created by dgx_st7735_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes (landscape <-> portrait).
 */
void dgx_st7735_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Control the backlight (if a backlight GPIO was provided at initialization).
 *
 * @param scr Screen created by dgx_st7735_init().
 * @param on  true to turn on the backlight, false to turn it off.
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED when the screen was given no backlight GPIO.
 */
esp_err_t dgx_st7735_backlight(dgx_screen_t *scr, bool on);

/**
 * @brief The panels dgx_st7735_init() makes its screen of: the glass of 128x160 and of 80x160. A copy of one with what a module
 * has otherwise, such as a glass of another size at another offset in the memory of the controller, goes to dgx_panel_init().
 */
extern const dgx_panel_t dgx_panel_st7735;
extern const dgx_panel_t dgx_panel_st7735_80x160;

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

