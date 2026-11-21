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
 * @param bus       Bus backend returned by dgx_spi_init().
 * @param rst       Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param backlight Backlight GPIO, high for on (use GPIO_NUM_NC if the module has no backlight control). It is on once the
 *                  panel is set up; dgx_st7565r_backlight() turns it off and on.
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_st7565r_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight);

/**
 * @brief Control the backlight (if a backlight GPIO was provided at initialization).
 *
 * @param scr Screen created by dgx_st7565r_init().
 * @param on  true to turn on the backlight, false to turn it off.
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED when the screen was given no backlight GPIO.
 * @deprecated Use dgx_screen_backlight().
 */
DGX_DEPRECATED("use dgx_screen_backlight()")
esp_err_t dgx_st7565r_backlight(dgx_screen_t *scr, bool on);

/**
 * @brief Set the voltage that drives the glass, which the contrast of the picture depends on.
 *
 * Modules differ in their glass and supply, and one module wants another
 * voltage in the cold. With too low a voltage the pixels that are on hardly
 * differ from the rest; with too high a voltage the pixels that are off show
 * as well, until the picture is lost. The driver starts with a ratio of 7
 * and a volume of 12. The sample code of a module names both values: the
 * command 0x20 + ratio, and the byte after the command 0x81; an EA DOGM128,
 * for one, wants 7 and 24.
 *
 * @param scr            Screen created by dgx_st7565r_init().
 * @param resistor_ratio The coarse step, 0..7: the gain of the voltage regulator, from 3.0 to 6.4.
 * @param volume         The fine step, 0..63: the electronic volume.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG for a value out of its range.
 */
esp_err_t dgx_st7565r_contrast(dgx_screen_t *scr, uint8_t resistor_ratio, uint8_t volume);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif

