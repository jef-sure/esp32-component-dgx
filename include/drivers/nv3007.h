#pragma once
/*
 * nv3007.h
 *
 *  The NV3007: bar panels of 142x428 on SPI, in a controller memory of
 *  168x428.
 */

#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "driver/gpio.h"
#include "drivers/dgx_panel.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * NV3007 panels. The memory of the controller is 168 columns by 428 rows; the
 * 142x428 glass lies 12 columns into it, as the vendor's window has it, so the
 * two values below are the same panel, portrait or landscape.
 */
typedef enum dgx_nv3007_resolution_t {
    DGX_NV3007_142X428 = 1, /**< portrait, MADCTL 00h as Arduino_GFX starts it; the init table of the 2.79" module */
    DGX_NV3007_428X142 = 2, /**< 142x428 turned by dgx_screen_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true) */
} dgx_nv3007_resolution_t;

/**
 * @brief Create an NV3007 panel screen on top of a bus (SPI or P8).
 *
 * The backlight stays off until the screen is cleared to black.
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_p8_init().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param backlight  Backlight GPIO, high for on (use GPIO_NUM_NC if the panel has no backlight control). It is on once the
 *                   panel is set up; dgx_screen_backlight() turns it off and on.
 * @param color_bits Pixel depth in bits: 16 for RGB565, the usual one, or 18 (COLMOD 05h and 06h, as the vendors' tables have them).
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @param resolution The panel (see dgx_nv3007_resolution_t).
 * @return Newly allocated screen, or NULL on failure, an unknown resolution or another depth. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_nv3007_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
                              dgx_nv3007_resolution_t resolution);

/**
 * @brief The panel dgx_nv3007_init() builds its screen from. A module with other glass passes a copy of it, with its own
 * init table, to dgx_panel_init().
 */
extern const dgx_panel_t dgx_panel_nv3007_142x428;

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
