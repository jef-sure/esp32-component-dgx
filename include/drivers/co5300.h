#pragma once
/*
 * co5300.h
 *
 *  The CO5300: round AMOLED panels on QSPI. The screen keeps its picture in
 *  memory, as the monochrome displays do, because the controller takes only
 *  windows from an even column and row to odd ones.
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

/**
 * @brief Choices of dgx_co5300_init_with(); zero for each is the default.
 */
typedef struct dgx_co5300_options_t {
    /**
     * Send in step with the scan of the panel, so that no frame is seen torn:
     * the TE line of the panel, @c te, tells when a scan starts, and a
     * rectangle goes out when the scan has passed it and before the next one
     * comes to it, its rows in the order of the scan. A drawing call waits up
     * to one scan for that, 16.7 ms at 60 Hz. With the axes swapped the rows
     * of the screen lie across the scan, and the screen is sent as without
     * the line. Takes three buffers of 8 KB of DMA memory.
     */
    bool       tear_free;
    gpio_num_t te; /**< The TE line, for @c tear_free; GPIO 9 on the Waveshare ESP32-S3-Touch-AMOLED-1.32. */
} dgx_co5300_options_t;

/**
 * @brief dgx_co5300_init() with choices of the program.
 * @param options NULL for the defaults, as dgx_co5300_init().
 * @return The screen, or NULL as dgx_co5300_init(), and when the TE line of @c tear_free cannot be listened to.
 */
dgx_screen_t *dgx_co5300_init_with(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo,
                                   dgx_co5300_resolution_t resolution, const dgx_co5300_options_t *options);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
