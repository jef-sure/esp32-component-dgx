#pragma once
/*
 * st77916.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "driver/gpio.h"
#include "drivers/dgx_panel.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a screen of an ST77916 panel, 360x360, on a bus: dgx_qspi_init() as a rule, dgx_spi_init() for one of one data line.
 *
 * The backlight is off until the screen is black, then on.
 *
 * @param bus        Bus of the panel.
 * @param rst        Reset GPIO (GPIO_NUM_NC if the panel has no reset line).
 * @param backlight  Backlight GPIO (GPIO_NUM_NC if the panel has no backlight control).
 * @param color_bits 16 (RGB565) or 18.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @return Newly allocated screen, or NULL on failure or another depth. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_st77916_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo);

/**
 * @brief Reprogram the controller scan direction (rotation/mirroring).
 *
 * @param scr     Screen created by dgx_st77916_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 */
void dgx_st77916_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Control the backlight (if a backlight GPIO was provided at initialization).
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED when the screen was given no backlight GPIO.
 */
esp_err_t dgx_st77916_backlight(dgx_screen_t *scr, bool on);

/**
 * @brief The panel dgx_st77916_init() makes its screen of: a copy of it with what a module has otherwise, the table of its own
 * glass above all, goes to dgx_panel_init().
 */
extern const dgx_panel_t dgx_panel_st77916;

#ifdef __cplusplus
}
#endif
