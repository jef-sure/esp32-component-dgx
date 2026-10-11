#pragma once
/*
 * dgx_dsi_esp32.h
 *
 *  Created on: 11.10.2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Initialize a MIPI-DSI bus backend.
 *
 * A display on this bus is refreshed all the time from a frame buffer in
 * PSRAM. For a panel driver the bus looks like a display with memory of its
 * own: it takes the window and the pixels that follow and puts them into the
 * frame buffer, in the byte order that buffer has. Other commands go to the
 * display as they are.
 *
 * @param bus_id             DSI controller, 0 on ESP32-P4.
 * @param data_lanes         Number of data lanes the display is wired with.
 * @param lane_bit_rate_mbps Bit rate of a lane in Mbit/s, as the display needs.
 * @param phy_ldo_chan       Channel of the chip's regulator that powers the DSI PHY
 *                           (3 on the usual ESP32-P4 boards), -1 when the PHY is powered otherwise.
 * @param phy_ldo_mv         Voltage of that channel in mV (2500 on the usual boards).
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_dsi_init(int bus_id, uint8_t data_lanes, uint32_t lane_bit_rate_mbps, int phy_ldo_chan, int phy_ldo_mv);

/*
 * The rest is for panel drivers.
 */

/** @brief The picture a display takes: its size and the timing of a frame. */
typedef struct _dgx_dsi_video_t {
    uint16_t width;             /*!< pixels in a row of the panel */
    uint16_t height;            /*!< rows of the panel */
    uint32_t pixel_clock_khz;   /*!< pixel clock in kHz */
    uint16_t hsync_pulse_width; /*!< horizontal sync, in pixels */
    uint16_t hsync_back_porch;
    uint16_t hsync_front_porch;
    uint16_t vsync_pulse_width; /*!< vertical sync, in rows */
    uint16_t vsync_back_porch;
    uint16_t vsync_front_porch;
} dgx_dsi_video_t;

/**
 * @brief Make the frame buffer of a display; it is black and not shown yet.
 *
 * Called by a panel driver once, before it sends its commands.
 *
 * @param bus        Bus from dgx_dsi_init().
 * @param video      Size and timing of the picture.
 * @param color_bits 16 or 24.
 * @param swap_rb    The display takes red and blue the other way round.
 * @return false on failure.
 */
bool dgx_dsi_video_setup(dgx_bus_protocols_t *bus, const dgx_dsi_video_t *video, uint8_t color_bits, bool swap_rb);

/**
 * @brief Start sending the frame buffer to the display, after its commands.
 * @return false on failure.
 */
bool dgx_dsi_video_start(dgx_bus_protocols_t *bus);

/**
 * @brief Set how the screen lies on the panel, as the MX, MY and MV bits of a display controller do.
 *
 * It is done by the bus where pixels are put, so it works with any display.
 * What the frame buffer holds stays as it is. The window becomes the whole
 * screen.
 *
 * @param mirror_x Columns of the panel are counted from the right.
 * @param mirror_y Rows of the panel are counted from the bottom.
 * @param swap_xy  x of the screen runs along the rows of the panel.
 */
void dgx_dsi_orientation(dgx_bus_protocols_t *bus, bool mirror_x, bool mirror_y, bool swap_xy);

/**
 * @brief Give a screen on this bus its drawing functions; called by a panel driver last, when the display is set up.
 *
 * The functions are those of any display behind a bus, but a pixel and a
 * rectangle are put straight into the frame buffer and a pixel is read
 * back from it, as a virtual screen does with its memory. update_screen()
 * brings out what was drawn, see dgx_dsi_show().
 *
 * @param scr A dgx_screen_with_bus_t whose bus is this one, with the size and the color depth set.
 */
void dgx_dsi_screen_functions(dgx_screen_t *scr);

/**
 * @brief Make what was written to the frame buffer since the last call seen by the display.
 *
 * Pixels are written through the cache of the chip, and the display is sent
 * the memory behind it. The update_screen() of a screen calls this, so
 * outside a batch every primitive is seen at once, and a batch is brought
 * out at its end: pixels set one by one cost several times less that way.
 * The bus also does it on its own when much has been drawn by pixels and
 * rectangles, so a long batch shows in the order it is drawn.
 */
void dgx_dsi_show(dgx_bus_protocols_t *bus);

/** @brief Number of data lanes the bus was made with. */
uint8_t dgx_dsi_data_lanes(dgx_bus_protocols_t *bus);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
