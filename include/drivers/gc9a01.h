#pragma once

#include "driver/gpio.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "bus/dgx_bus_protocols.h"

/**
 * @brief Create a GC9A01 round panel screen on top of a bus (SPI or P8).
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_p8_init().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param color_bits Pixel depth in bits (typically 16 for RGB565).
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_gc9a01_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo);

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

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif