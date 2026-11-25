#pragma once
/*
 * axs15231b.h
 *
 *  The AXS15231B: IPS panels on QSPI with the touch controller in the same
 *  chip. The screen keeps its picture in memory, as the monochrome displays
 *  do, because the controller takes only windows of whole blocks of 4x4
 *  pixels.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "driver/gpio.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/** AXS15231B panels. */
typedef enum dgx_axs15231b_resolution_t {
    DGX_AXS15231B_320X480 = 1, /**< Guition JC3248W535, 3.5", portrait */
} dgx_axs15231b_resolution_t;

/**
 * @brief Create an AXS15231B screen on a bus of dgx_qspi_init().
 *
 * The screen is a 16-bit virtual screen of the size of the glass, 300 KB for
 * 320x480, taken from the heap: PSRAM on a board that has it. Every drawing
 * call goes into it, and what changed is sent to the panel at the end of the
 * call, or of a batch (dgx_screen_batch_begin()), widened to whole blocks of
 * 4x4 pixels of the glass: the controller stores its picture by such blocks,
 * and a window that cuts one spoils the rest of it.
 *
 * The controller does not swap its axes on QSPI, so the driver lays the
 * picture onto the glass itself as it sends it, in all eight orientations.
 * dgx_screen_orientation() turns the picture in memory and sends nothing: the
 * glass keeps what it shows, as on other displays. A turn that swaps the axes
 * takes as much memory again as the picture for a moment.
 *
 * @param bus        Bus returned by dgx_qspi_init().
 * @param rst        Reset GPIO, GPIO_NUM_NC for none (the Guition JC3248W535 has none).
 * @param backlight  Backlight GPIO, high for on, GPIO_NUM_NC for none: GPIO 1 on the Guition JC3248W535.
 * @param color_bits 16; other depths are not supported yet.
 * @param cbo        Color order: DgxScreenRGB on the Guition JC3248W535.
 * @param resolution The panel (see dgx_axs15231b_resolution_t).
 * @return The screen, or NULL for another depth, an unknown resolution or no memory. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_axs15231b_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
                                 dgx_axs15231b_resolution_t resolution);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
