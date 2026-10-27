#pragma once
#include "dgx_screen.h"
#include "driver/gpio.h"
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"

/**
 * The panels of an ST7789. Its memory is 240x320 in every one of them: a
 * 240x240 panel shows the first 240 rows of it, a 320x240 one is a 240x320
 * turned to landscape.
 */
typedef enum {
    ST7789_RESOLUTION_240x240 = 1,
    ST7789_RESOLUTION_240x320 = 2,
    ST7789_RESOLUTION_320x240 = 3 /**< 240x320 turned by dgx_st7789_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true) */
} st7789_resolution_t;

/**
 * @brief Create an ST7789 panel screen on top of a bus (SPI or P8).
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_p8_init().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param color_bits Pixel depth in bits (typically 16 for RGB565).
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @param resolution The panel (see st7789_resolution_t).
 * @return Newly allocated screen, or NULL on failure or an unknown resolution. Free with dgx_screen_destroy().
 */
dgx_screen_t* dgx_st7789_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo, st7789_resolution_t resolution);

/**
 * @brief Reprogram the controller scan direction (rotation/mirroring).
 *
 * @param scr     Screen created by dgx_st7789_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes (landscape <-> portrait). On a
 *                240x320 panel the screen becomes 320x240 and back.
 */
void dgx_st7789_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
