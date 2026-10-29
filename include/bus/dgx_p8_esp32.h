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
#include "esp_lcd_panel_io.h"
/* from IDF 5.3 the i80 bus has a header of its own; in IDF 5.2 its handle is in esp_lcd_panel_io.h */
#if __has_include("esp_lcd_io_i80.h")
#include "esp_lcd_io_i80.h"
#endif

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Initialize an 8-bit parallel (Intel 8080 / I80) bus backend, on a bus DGX creates.
 *
 * DGX creates the bus with the display and deletes it on dispose. A display
 * needs a bus of its own: a second one on any of the same data, WR and DC pins
 * is refused. The I80 driver of IDF does not keep two panels on one bus apart,
 * a command for one cuts off the pixels of the other that are on their way. To
 * put a display on a bus the program created itself, call dgx_p8_init_on_bus().
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

/**
 * @brief Initialize an 8-bit parallel bus backend on an I80 bus the program owns.
 *
 * The bus must have been made by esp_lcd_new_i80_bus() 8 bits wide, with its DC
 * pin and a max_transfer_bytes of at least 4096. DGX adds its panel IO with its
 * own CS and removes it when the backend is disposed; the bus stays the
 * program's to delete, after the backend.
 *
 * Put one DGX display on such a bus. A panel the program drives on it through
 * esp_lcd is sent to only after the DGX screen has sent its pixels, its
 * wait_buffer(), or a command for one panel cuts off the pixels of the other.
 *
 * @param i80_bus  I80 bus made by the program.
 * @param lcd_rd   Read strobe GPIO (GPIO_NUM_NC if unused).
 * @param cs       Chip-select GPIO of the panel.
 * @param pclk_hz  Parallel clock of this panel in Hz.
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_p8_init_on_bus(esp_lcd_i80_bus_handle_t i80_bus, gpio_num_t lcd_rd, gpio_num_t cs, int pclk_hz);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

