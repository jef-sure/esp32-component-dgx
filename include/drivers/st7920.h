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
 * The controller is driven by its serial protocol, a sync byte and two
 * nibbles for every byte, so the bus is an SPI one. Its chip select is active
 * high and is held for a whole transfer: the driver drives @p cs itself. A
 * panel with its CS tied high, alone on its lines, takes every transfer.
 *
 * @param bus Bus backend returned by dgx_spi_init().
 * @param rst Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param cs  Chip-select GPIO, driven by the driver: high for a transfer (use GPIO_NUM_NC if CS is tied high).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t* dgx_st7920_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t cs);

/**
 * @brief Set how the screen lies on the panel (rotation/mirroring).
 *
 * The controller has no command for it: the driver turns the picture when
 * it sends it to the panel. With @p swap_xy the screen is 64x128, and what
 * was drawn before the call is cleared, for its rows are of another length;
 * without a change of size the picture stays and is shown as it lies now.
 * A turned screen is gathered bit by bit, a whole update of it takes longer.
 *
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

