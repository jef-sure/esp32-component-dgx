#pragma once
/*
 * dgx_p8_esp32.h
 *
 *  Created on: 28.10.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */


#include "bus/dgx_bus_protocols.h"
#include "driver/gpio.h"
#include "dgx_arch.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Initialize an 8-bit parallel (Intel 8080 / I80) bus backend.
 *
 * @param lcd_d0..lcd_d7 The eight parallel data GPIOs.
 * @param lcd_wr         Write strobe GPIO.
 * @param lcd_rd         Read strobe GPIO (GPIO_NUM_NC if unused).
 * @param cs             Chip-select GPIO.
 * @param dc             Data/command select GPIO.
 * @param pclk_hz        Parallel clock in Hz.
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_p8_init(gpio_num_t lcd_d0, gpio_num_t lcd_d1, gpio_num_t lcd_d2, gpio_num_t lcd_d3, gpio_num_t lcd_d4, gpio_num_t lcd_d5,
        gpio_num_t lcd_d6, gpio_num_t lcd_d7, gpio_num_t lcd_wr, gpio_num_t lcd_rd, gpio_num_t cs, gpio_num_t dc, int pclk_hz);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

