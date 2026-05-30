#pragma once
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "driver/gpio.h"

/**
 * @brief Create an SSD1351 OLED panel screen on top of a bus (SPI or P8).
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_p8_init().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param color_bits Pixel depth in bits (typically 16 for RGB565).
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_ssd1351_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo);

/**
 * @brief Reprogram the controller scan direction (rotation/mirroring).
 * @param _scr    Screen created by dgx_ssd1351_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 */
void dgx_ssd1351_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Set the panel master brightness.
 * @param _scr       Screen created by dgx_ssd1351_init().
 * @param brightness Brightness level, 0 (dim) to 255 (bright).
 */
void dgx_ssd1351_brightness(dgx_screen_t *_scr, uint8_t brightness /* 0 - 255 */);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

