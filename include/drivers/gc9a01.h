#pragma once

#include "driver/gpio.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "bus/dgx_bus_protocols.h"
#include "drivers/dgx_panel.h"

/**
 * @brief Create a GC9A01 round panel screen on top of a bus (SPI or P8).
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_p8_init().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param backlight  Backlight GPIO, high for on (use GPIO_NUM_NC if the panel has no backlight control). It is on once the
 *                   panel is set up; dgx_gc9a01_backlight() turns it off and on.
 * @param color_bits Pixel depth in bits: 16 for RGB565, the usual one, 12 or 18.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @return Newly allocated screen, or NULL on failure or another depth. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_gc9a01_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo);

/**
 * @brief Blank the panel by sending the DISPOFF command. Does not enter sleep mode.
 * @param scr Screen created by dgx_gc9a01_init().
 */
void dgx_gc9a01_display_off(dgx_screen_t *scr);

/**
 * @brief Re-enable the panel by sending the DISPON command.
 * @param scr Screen created by dgx_gc9a01_init().
 */
void dgx_gc9a01_display_on(dgx_screen_t *scr);

/**
 * @brief Reprogram the controller scan direction (rotation/mirroring).
 *
 * @param scr     Screen created by dgx_gc9a01_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 */
void dgx_gc9a01_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Control the backlight (if a backlight GPIO was provided at initialization).
 *
 * @param scr Screen created by dgx_gc9a01_init().
 * @param on  true to turn on the backlight, false to turn it off.
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED when the screen was given no backlight GPIO.
 */
esp_err_t dgx_gc9a01_backlight(dgx_screen_t *scr, bool on);

/**
 * @brief The panel dgx_gc9a01_init() makes its screen of: a copy of it with what a module has otherwise goes to dgx_panel_init().
 */
extern const dgx_panel_t dgx_panel_gc9a01;

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif