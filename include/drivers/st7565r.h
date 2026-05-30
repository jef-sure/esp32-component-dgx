#pragma once
#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"
#include "driver/gpio.h"
#include "dgx_arch.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Create an ST7565R monochrome LCD screen on top of an SPI bus.
 *
 * The driver keeps a 1-bit RAM page buffer; drawing updates RAM and is flushed
 * to the controller via the screen's update_screen function.
 *
 * @param bus Bus backend returned by dgx_spi_init().
 * @param rst Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_st7565r_init(dgx_bus_protocols_t *bus, gpio_num_t rst);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif

