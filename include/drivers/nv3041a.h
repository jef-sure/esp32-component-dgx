#pragma once
#ifdef __cplusplus
// @formatter:off
extern "C"
{
// @formatter:on
#endif

#include "bus/dgx_bus_protocols.h"
#include "drivers/dgx_panel.h"
#include "dgx_screen.h"
#include "driver/gpio.h"

/**
 * @brief Create an NV3041A panel screen, 480x272, on top of a QSPI bus.
 *
 * The panel starts upright in landscape, 480 pixels across.
 *
 * @param bus        Bus backend returned by dgx_qspi_init() or dgx_qspi_init_on_bus().
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line: it is reset by a command then).
 * @param backlight  Backlight GPIO (use GPIO_NUM_NC if the panel has no backlight control).
 * @param color_bits Pixel depth in bits: 16 (RGB565), the only one the driver sets up.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR). If red and blue are exchanged on a module, pass the other value.
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_nv3041a_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo);

/**
 * @brief Reprogram the controller scan direction (rotation/mirroring).
 *
 * (DgxScreenLeftRight, DgxScreenTopBottom, false) is the upright landscape the panel starts in.
 *
 * @param scr     Screen created by dgx_nv3041a_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes (landscape <-> portrait).
 */
void dgx_nv3041a_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Control the backlight (if a backlight GPIO was provided at initialization).
 *
 * @param scr Screen created by dgx_nv3041a_init().
 * @param on  true to turn on the backlight, false to turn it off.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t dgx_nv3041a_backlight(dgx_screen_t *scr, bool on);

/**
 * @brief The panel dgx_nv3041a_init() makes its screen of: a copy of it with what a module has otherwise goes to dgx_panel_init().
 */
extern const dgx_panel_t dgx_panel_nv3041a;

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
