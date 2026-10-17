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
 * @brief Create a JD9365 panel screen on top of a MIPI-DSI bus: 800x1280, as in 10.1-inch displays.
 *
 * The commands a JD9365 is set up with belong to the glass it drives. The
 * driver has those of the 10.1-inch panel of the Guition JC8012P4A1C board,
 * as its factory firmware sends them. The board has shipped with two panels;
 * these are the commands of the newer one (a "V2" label), at 80 MHz and
 * 60 frames a second.
 * The panel is portrait: dgx_jd9365_orientation() with swap_xy turns it.
 *
 * @param bus        Bus backend returned by dgx_dsi_init(): 2 data lanes at 1000 Mbit/s for that panel.
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param color_bits Pixel depth in bits: 16 (RGB565) or 24.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @param frames     DgxDsiOneFrame: drawing shows at once, no memory beyond one frame, a moving picture may tear.
 *                   DgxDsiTwoFrames: a batch of drawing is shown whole, for one more frame of PSRAM.
 *                   DgxDsiOneFrameAfterScan: one frame, large pieces wait for the scan to leave their rows. See dgx_dsi_frames_t.
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_jd9365_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo, dgx_dsi_frames_t frames);

/**
 * @brief Set how the screen lies on the panel (rotation/mirroring).
 *
 * The bus does it where it puts pixels into the frame buffer; what is shown
 * already stays as it is. With @p swap_xy the screen is 1280x800 (landscape).
 *
 * @param scr     Screen created by dgx_jd9365_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes (landscape <-> portrait).
 */
void dgx_jd9365_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
