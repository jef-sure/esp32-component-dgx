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


#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Initialize an SPI bus backend for a display panel, on a host DGX initializes.
 *
 * DGX initializes the host with the first display on it; further displays on
 * the same host share it and must name the same MOSI, MISO, SCLK and DMA
 * channel. The host is freed when the last of them is disposed, unless the
 * program has added devices of its own to it. To put a display on a host the
 * program already uses for other devices, initialize the host in the program
 * and call dgx_spi_init_on_bus().
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

/**
 * @brief Initialize an SPI bus backend for a display panel, on a host the program owns.
 *
 * For a display that shares the bus with other devices of the program: a touch
 * controller, an SD card. The host must have been initialized by
 * spi_bus_initialize() with DMA (e.g. SPI_DMA_CH_AUTO) and a max_transfer_sz of
 * 0 or at least 4092 bytes. The buffer of the display, the longest piece of
 * pixels DGX sends, is as long as the wire takes about 0.8 ms to send at its
 * clock, 4000 bytes at least, 32000 at most and no more than the host takes:
 * 4000 bytes at 40 MHz, 8000 at 80 MHz. DGX adds its device to the host and removes it when the backend is disposed;
 * the host stays the program's to free, after the backend. The ESP-IDF driver
 * takes turns between the devices; a long picture keeps the bus for the time
 * of each piece.
 *
 * @param host_id        SPI host the program has initialized.
 * @param cs             Chip-select GPIO of the panel.
 * @param dc             Data/command select GPIO.
 * @param clock_speed_hz SPI clock of this device in Hz.
 * @param cpolpha_mode   SPI mode 0..3 (CPOL/CPHA); pass 0 for most panels.
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_spi_init_on_bus(spi_host_device_t host_id, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

