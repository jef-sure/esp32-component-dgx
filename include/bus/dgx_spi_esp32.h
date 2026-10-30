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
 * channel. The host is freed when the last of them is disposed. It takes no
 * devices of the program: the only display on it keeps the host between its
 * drawings, and a device added to it would wait for ever. To put a display on
 * a host the program uses for other devices, initialize the host in the
 * program and call dgx_spi_init_on_bus().
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
 * pixels DGX sends, is as long as the wire takes about 0.95 ms to send at its
 * clock, 4000 bytes at least, 32000 at most and no more than the host takes:
 * 4736 bytes at 40 MHz, 9488 at 80 MHz. DGX adds its device to the host and removes it when the backend is disposed;
 * the host stays the program's to free, after the backend. The display holds
 * the host from a window to the end of its drawing, or of the batch around it;
 * the ESP-IDF driver takes turns between the devices outside that.
 *
 * @param host_id        SPI host the program has initialized.
 * @param cs             Chip-select GPIO of the panel.
 * @param dc             Data/command select GPIO.
 * @param clock_speed_hz SPI clock of this device in Hz.
 * @param cpolpha_mode   SPI mode 0..3 (CPOL/CPHA); pass 0 for most panels.
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_spi_init_on_bus(spi_host_device_t host_id, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode);

/**
 * @brief Choices of the program for an SPI or QSPI display; the constructors without _with take the defaults.
 */
typedef struct {
    /*!
     * The most bytes the buffer of the display may take, 0 for as long as DGX
     * picks by the clock. The buffer is in internal memory that DMA reaches,
     * on every chip, and a fast bus takes a long one: up to 32000 bytes for
     * QSPI. A shorter buffer saves that memory and sends pixels in more
     * pieces, each of which costs about 25 us to start. The buffer is never
     * longer than this, also below the 4000 bytes DGX takes at least: DGX
     * cuts it down to a multiple of 16 bytes (of 4 under 16, and takes 4 for
     * less than that).
     */
    size_t buffer_max;
} dgx_spi_options_t;

/**
 * @brief dgx_spi_init() with choices of the program.
 * @param options NULL for the defaults, as dgx_spi_init().
 */
dgx_bus_protocols_t *dgx_spi_init_with(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk,
                                       gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode, const dgx_spi_options_t *options);

/**
 * @brief dgx_spi_init_on_bus() with choices of the program.
 * @param options NULL for the defaults, as dgx_spi_init_on_bus().
 */
dgx_bus_protocols_t *dgx_spi_init_on_bus_with(spi_host_device_t host_id, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode,
                                              const dgx_spi_options_t *options);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

