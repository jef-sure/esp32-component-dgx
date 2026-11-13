/*
 * dgx_backlight_priv.h
 *
 *  The backlight line of a display whose driver is not made of a
 *  dgx_panel_t: JD9165, JD9365, ST7920, ST7565R. High is on. panel.c has
 *  the same for the panels, with the line off until the screen is black for
 *  DGX_PANEL_CLEAR_FIRST.
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#pragma once

#include <stdbool.h>

#include "dgx_arch.h"
#include "esp_err.h"

/* the line an output and on, once the display is set up; nothing for GPIO_NUM_NC */
static inline void dgx_backlight_on_start(gpio_num_t line)
{
    if (line < 0) return;
    dgx_gpio_set_direction(line, GPIO_MODE_OUTPUT);
    dgx_gpio_set_level(line, 1);
}

/* the backlight on or off; ESP_ERR_NOT_SUPPORTED for a display made without its line */
static inline esp_err_t dgx_backlight_set(gpio_num_t line, bool on)
{
    if (line < 0) return ESP_ERR_NOT_SUPPORTED;
    dgx_gpio_set_level(line, on ? 1 : 0);
    return ESP_OK;
}
