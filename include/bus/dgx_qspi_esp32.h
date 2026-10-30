#pragma once
/*
 * dgx_qspi_esp32.h
 *
 *  Created on: 23.10.2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */


#include "bus/dgx_bus_protocols.h"
#include "bus/dgx_spi_esp32.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"


#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Initialize a QSPI bus backend for a display panel, on a host DGX initializes.
 *
 * A QSPI panel (NV3041A and the like) has no D/C line: every transfer begins
 * with an opcode and a 24-bit address on one line, the command of the panel
 * in the middle byte of the address. Commands and their parameters go on one
 * line, pixels on four.
 *
 * DGX initializes the host with the first display on it, as dgx_spi_init()
 * does, and shares it with further displays on it that name the same lines.
 * To put a display on a host the program already uses, initialize the host
 * in the program with the four data lines and call dgx_qspi_init_on_bus().
 *
 * @param host_id        SPI host (e.g. SPI2_HOST).
 * @param dma_chan       DMA channel (e.g. SPI_DMA_CH_AUTO).
 * @param d0,d1,d2,d3    The four data GPIOs: D0 is MOSI, D1 MISO, D2 WP and D3 HD of the host.
 * @param sclk           Serial clock GPIO.
 * @param cs             Chip-select GPIO, driven by the SPI hardware for every transfer.
 * @param clock_speed_hz SPI clock in Hz (e.g. 40 * 1000 * 1000).
 * @param cpolpha_mode   SPI mode 0..3 (CPOL/CPHA); pass 0 for most panels.
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_qspi_init(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t d0, gpio_num_t d1, gpio_num_t d2, gpio_num_t d3,
                                   gpio_num_t sclk, gpio_num_t cs, int clock_speed_hz, uint8_t cpolpha_mode);

/**
 * @brief Initialize a QSPI bus backend for a display panel, on a host the program owns.
 *
 * The host must have been initialized by spi_bus_initialize() with its four
 * data lines, DMA (e.g. SPI_DMA_CH_AUTO) and a max_transfer_sz of 0 or at
 * least 4092 bytes. The buffer of the display, the longest piece of pixels
 * DGX sends, is as long as the wire takes about 0.95 ms to send at its clock,
 * 4000 bytes at least, 32000 at most and no more than the host takes: about
 * 19000 bytes at 40 MHz, so give the host a max_transfer_sz of 19000 or more
 * for the full speed. DGX adds its device to the host and removes it when the backend is disposed; the
 * host stays the program's to free, after the backend.
 *
 * @param host_id        SPI host the program has initialized.
 * @param cs             Chip-select GPIO of the panel, driven by the SPI hardware for every transfer.
 * @param clock_speed_hz SPI clock of this device in Hz.
 * @param cpolpha_mode   SPI mode 0..3 (CPOL/CPHA); pass 0 for most panels.
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_qspi_init_on_bus(spi_host_device_t host_id, gpio_num_t cs, int clock_speed_hz, uint8_t cpolpha_mode);

/**
 * @brief dgx_qspi_init() with choices of the program, such as the longest buffer (dgx_spi_options_t).
 * @param options NULL for the defaults, as dgx_qspi_init().
 */
dgx_bus_protocols_t *dgx_qspi_init_with(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t d0, gpio_num_t d1, gpio_num_t d2, gpio_num_t d3,
                                        gpio_num_t sclk, gpio_num_t cs, int clock_speed_hz, uint8_t cpolpha_mode, const dgx_spi_options_t *options);

/**
 * @brief dgx_qspi_init_on_bus() with choices of the program, such as the longest buffer (dgx_spi_options_t).
 * @param options NULL for the defaults, as dgx_qspi_init_on_bus().
 */
dgx_bus_protocols_t *dgx_qspi_init_on_bus_with(spi_host_device_t host_id, gpio_num_t cs, int clock_speed_hz, uint8_t cpolpha_mode,
                                               const dgx_spi_options_t *options);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
