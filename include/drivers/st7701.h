#pragma once
#include "dgx_screen.h"
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "bus/dgx_bus_protocols.h"
#include "bus/dgx_dsi_esp32.h"
#include "driver/gpio.h"

/**
 * @brief Create an ST7701S panel screen on top of a MIPI-DSI bus: 480x800, as in 4.3-inch displays.
 *
 * The commands an ST7701S is set up with belong to the glass it drives. The
 * driver has those of the 4.3-inch panel of the Guition JC4880P443C board,
 * as its factory firmware sends them, at 61 frames a second.
 * The panel is portrait: dgx_screen_orientation() with swap_xy turns it.
 * Only the MIPI-DSI kind of the controller is driven; one on an RGB bus is not.
 *
 * @param bus        Bus backend returned by dgx_dsi_init(): 2 data lanes at 1000 Mbit/s for that panel.
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param backlight  Backlight GPIO, high for on (use GPIO_NUM_NC if the panel has no backlight control). It is on once the
 *                   panel is set up; dgx_screen_backlight() turns it off and on.
 * @param color_bits Pixel depth in bits: 16 (RGB565) or 24.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @param frames     DgxDsiOneFrame: drawing shows at once, no memory beyond one frame, a moving picture may tear.
 *                   DgxDsiTwoFrames: a batch of drawing is shown whole, for one more frame of PSRAM.
 *                   DgxDsiOneFrameAfterScan: one frame, large pieces wait for the scan to leave their rows. See dgx_dsi_frames_t.
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_st7701_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
                              dgx_dsi_frames_t frames);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
