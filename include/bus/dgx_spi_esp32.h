#pragma once
/*
 * dgx_spi_esp32.h
 *
 *  Created on: 28.10.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */


#include "bus/dgx_bus_protocols.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "hal/gpio_ll.h"


#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Initialize an SPI master bus backend for a display panel.
 *
 * @param host_id        SPI host (e.g. SPI2_HOST, SPI3_HOST).
 * @param dma_chan       DMA channel (e.g. SPI_DMA_CH_AUTO).
 * @param mosi           MOSI / SDA data GPIO.
 * @param miso           MISO GPIO (GPIO_NUM_NC for write-only panels).
 * @param sclk           Serial clock GPIO.
 * @param cs             Chip-select GPIO.
 * @param dc             Data/command select GPIO.
 * @param clock_speed_hz SPI clock in Hz (e.g. 40 * 1000 * 1000).
 * @param cpolpha_mode   SPI mode 0..3 (CPOL/CPHA); pass 0 for most panels.
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t* dgx_spi_init(          //
    spi_host_device_t host_id,              //
    spi_dma_chan_t dma_chan,                //
    gpio_num_t mosi,                        //
    gpio_num_t miso,                        //
    gpio_num_t sclk,                        //
    gpio_num_t cs,                          //
    gpio_num_t dc,                          //
    int clock_speed_hz,                     //
    uint8_t /* default = 0 */ cpolpha_mode  //
);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

