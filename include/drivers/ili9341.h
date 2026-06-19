#pragma once
#ifdef __cplusplus
// @formatter:off
extern "C"
{
// @formatter:on
#endif

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "driver/gpio.h"

/**
 * @brief Create an ILI9341 panel screen on top of a bus (SPI or P8).
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_p8_init().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param backlight  Backlight GPIO (use GPIO_NUM_NC if the panel has no backlight control).
 * @param color_bits Pixel depth in bits (typically 16 for RGB565).
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_ili9341_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo);

/**
 * @brief Reprogram the controller scan direction (rotation/mirroring).
 *
 * @param scr     Screen created by dgx_ili9341_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes (landscape <-> portrait).
 */
void dgx_ili9341_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Control the backlight (if a backlight GPIO was provided at initialization).
 *
 * @param scr Screen created by dgx_ili9341_init().
 * @param on  true to turn on the backlight, false to turn it off.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t dgx_ili9341_backlight(dgx_screen_t *_scr, bool on);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
