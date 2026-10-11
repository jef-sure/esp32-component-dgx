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
 * @brief Create a JD9165 panel screen on top of a MIPI-DSI bus: 1024x600, as in 7-inch displays.
 *
 * The commands a JD9165 is set up with belong to the glass it drives. The
 * driver has those of the 7-inch panel of the Guition JC1060P470 board, as
 * the firmware the board comes with sends them.
 *
 * @param bus        Bus backend returned by dgx_dsi_init(): 2 data lanes at 750 Mbit/s for that panel.
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param color_bits Pixel depth in bits: 16 (RGB565) or 24.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @param frames     DgxDsiOneFrame: drawing shows at once, no memory beyond one frame, a moving picture may tear.
 *                   DgxDsiTwoFrames: a batch of drawing is shown whole, for one more frame of PSRAM. See dgx_dsi_frames_t.
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_jd9165_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo, dgx_dsi_frames_t frames);

/**
 * @brief Set how the screen lies on the panel (rotation/mirroring).
 *
 * The bus does it where it puts pixels into the frame buffer; what is shown
 * already stays as it is. With @p swap_xy the screen is 600x1024.
 *
 * @param scr     Screen created by dgx_jd9165_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes (landscape <-> portrait).
 */
void dgx_jd9165_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
