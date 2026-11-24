#pragma once
/*
 * co5300.h
 *
 *  The CO5300: round AMOLED panels on QSPI. The screen keeps its picture in
 *  memory, as the monochrome displays do, because the controller takes only
 *  windows from an even column and row to odd ones.
 */

#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "driver/gpio.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * CO5300 panels: the glass, where it lies in the memory of the controller,
 * and its upright MADCTL, as the vendors' code has them. Only the 466x466 has
 * been tried on a module.
 */
typedef enum dgx_co5300_resolution_t {
    DGX_CO5300_466X466 = 1, /**< round, Waveshare ESP32-S3-Touch-AMOLED-1.32, 1.75 and 1.75C; ESP32-C6-Touch-AMOLED-1.32 */
    DGX_CO5300_480X480 = 2, /**< Waveshare ESP32-S3-Touch-AMOLED-2.16 */
    DGX_CO5300_368X448 = 3, /**< Waveshare ESP32-S3-Touch-AMOLED-1.8 and ESP32-C6-Touch-AMOLED-1.8 of the second revision */
    DGX_CO5300_280X456 = 4, /**< Waveshare ESP32-S3-Touch-AMOLED-1.64, LilyGO T-Display S3 AMOLED 1.64 */
} dgx_co5300_resolution_t;

/**
 * @brief Create a CO5300 screen on a bus of dgx_qspi_init().
 *
 * The screen is a 16-bit virtual screen of the size of the glass, 434 KB for
 * 466x466, taken from the heap: PSRAM on a board that has it. A glass that is
 * not square takes as much again for a moment when its axes are swapped. Every drawing
 * call goes into it, and what changed is sent to the panel at the end of the
 * call, or of a batch (dgx_screen_batch_begin()), widened to whole pairs of
 * columns and rows. dgx_screen_orientation() turns the picture by the panel
 * and sends nothing: the glass keeps what it shows, as on other displays. The
 * panel has no backlight line:
 * dgx_screen_brightness() sets its brightness, 0 to 255.
 *
 * @param bus        Bus returned by dgx_qspi_init().
 * @param rst        Reset GPIO, GPIO_NUM_NC for none.
 * @param color_bits 16; other depths are not supported yet.
 * @param cbo        Color order: DgxScreenRGB on the Waveshare ESP32-S3-Touch-AMOLED-1.32.
 * @param resolution The panel (see dgx_co5300_resolution_t).
 * @return The screen, or NULL for another depth, an unknown resolution or no memory. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_co5300_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo,
                              dgx_co5300_resolution_t resolution);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
