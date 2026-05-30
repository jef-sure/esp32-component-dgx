#pragma once
#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"
#include "driver/gpio.h"
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Create an ST7920 monochrome LCD screen on top of a bus.
 *
 * The driver renders into a color virtual screen and flushes it to the
 * controller, so CONFIG_DGX_ENABLE_VSCREEN must be enabled.
 *
 * @param bus Bus backend returned by dgx_spi_init(), dgx_i2c_init() or dgx_p8_init().
 * @param rst Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param cs  Chip-select GPIO.
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t* dgx_st7920_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t cs);

/**
 * @brief Reprogram the logical scan direction (rotation/mirroring).
 * @param scr     Screen created by dgx_st7920_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 */
void dgx_st7920_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

