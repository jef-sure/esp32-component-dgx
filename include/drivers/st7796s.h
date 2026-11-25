#pragma once
/*
 * st7796s.h
 *
 *  The ST7796S: 320x480 panels on SPI or the 8-bit parallel bus. The glass
 *  fills the controller memory.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "driver/gpio.h"
#include "drivers/dgx_panel.h"
#include "esp_err.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * ST7796S panels. The glass is 320x480 and fills the controller memory, so
 * both values are the same panel, portrait or landscape. With the cbo
 * DgxScreenRGB the driver sets the BGR bit of MADCTL, as TFT_eSPI does; a
 * module that shows red and blue swapped takes DgxScreenBGR.
 */
typedef enum dgx_st7796s_resolution_t {
    DGX_ST7796S_320X480 = 1, /**< portrait, as rotation 0 of TFT_eSPI */
    DGX_ST7796S_480X320 = 2, /**< 320x480 turned by dgx_screen_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true), as rotation 1 of TFT_eSPI */
} dgx_st7796s_resolution_t;

/**
 * @brief Create an ST7796S panel screen on top of a bus (SPI or P8).
 *
 * The backlight stays off until the screen is cleared to black.
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_p8_init().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param backlight  Backlight GPIO, high for on (use GPIO_NUM_NC if the panel has no backlight control). It is on once the
 *                   panel is set up; dgx_screen_backlight() turns it off and on.
 * @param color_bits Pixel depth in bits: 16 for RGB565, the usual one, or 18 for RGB666. The serial interface of the ST7796S
 *                   takes only these two (COLMOD 05h and 06h); its 24-bit format is for the 8080 parallel buses, and DGX
 *                   does not set it up.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @param resolution The panel (see dgx_st7796s_resolution_t).
 * @return Newly allocated screen, or NULL on failure, an unknown resolution or another depth. Free with dgx_screen_destroy().
 */
dgx_screen_t* dgx_st7796s_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
        dgx_st7796s_resolution_t resolution);

/**
 * @brief The panel dgx_st7796s_init() builds its screen from. A module with other glass passes a copy of it, with its
 * own init table, to dgx_panel_init().
 */
extern const dgx_panel_t dgx_panel_st7796s;

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
