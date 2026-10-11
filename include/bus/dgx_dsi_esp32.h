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

/**
 * @brief What a display on this bus may spend to show a picture whole; given to the constructor of a display.
 *
 * The display is sent a frame buffer all the time, while it is being drawn
 * into as well. There is no way that suits every program, so the program
 * chooses.
 */
typedef enum {
    /**
     * One frame buffer, and drawing goes into it. No memory beyond the one
     * frame, and what is drawn shows at once. A picture that changes over
     * much of the screen may be seen half updated for a frame (tearing): the
     * more the longer it takes to draw against the 1/60 s of a frame.
     */
    DgxDsiOneFrame = 1,
    /**
     * One frame buffer and no memory beyond it; what is put to the display
     * waits for the scan of the display to be out of the way. A compromise:
     * a picture that comes from a virtual screen is seen whole, drawing on
     * the display itself only in part.
     *
     * A part of a virtual screen that goes by the 2D DMA and a large
     * rectangle filled by the accelerator are started when the scan is not
     * in their rows. Both write faster than the display reads and stay
     * ahead of the scan, so the part or the rectangle is seen whole, the
     * whole screen as well.
     *
     * Pixels the processor sends through the window (small parts, a turned
     * screen, 24-bit color, text) wait until the scan has left the rows of
     * the window, when they can be written before it is back. That holds
     * for about 160 rows of 1024 pixels; a larger window goes as it comes,
     * as with DgxDsiOneFrame. What a batch has left in the cache of the
     * chip is brought out the same way at its end.
     *
     * Single pixels, lines and small rectangles drawn on the display itself
     * are not held back. And a batch is not shown as one: of many tall
     * rectangles some are seen a frame later than the others. What has to
     * appear at once is drawn in a virtual screen and put to the display.
     *
     * The wait does not set the pace: it is over at once when the scan is
     * elsewhere, so a program may update more than once a frame.
     */
    DgxDsiOneFrameAfterScan = 3,
    /**
     * Two frame buffers. Drawing goes into the one the display is not
     * reading, and a batch of drawing is shown whole, from the next frame
     * on. Costs one more frame of PSRAM (1.2 MB for 1024x600 at 16 bits)
     * and 0.8 KB of RAM. What a batch has drawn is given to the other
     * frame buffer afterwards, by tiles of 32x8 pixels: only the tiles that
     * were drawn in, and only what the next batch does not write over
     * whole. Drawing that follows a batch waits until the display has
     * taken it, so there is at most a batch a frame, 60 a second: put what
     * belongs to one picture between dgx_screen_progress_up() and
     * dgx_screen_progress_down(), a primitive drawn outside a batch is a
     * batch of its own.
     */
    DgxDsiTwoFrames = 2,
} dgx_dsi_frames_t;

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
 * @param frames     The way to update the display the program asked its constructor for.
 * @return false on failure.
 */
bool dgx_dsi_video_setup(dgx_bus_protocols_t *bus, const dgx_dsi_video_t *video, uint8_t color_bits, bool swap_rb, dgx_dsi_frames_t frames);

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
 * rectangles, so a long batch shows in the order it is drawn. With two
 * frame buffers this is where the display is told to show what was drawn.
 */
void dgx_dsi_show(dgx_bus_protocols_t *bus);

/** @brief Number of data lanes the bus was made with. */
uint8_t dgx_dsi_data_lanes(dgx_bus_protocols_t *bus);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
