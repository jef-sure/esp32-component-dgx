#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bus/dgx_spi_esp32.h"

typedef struct _dgx_spi_bus_t
{
    dgx_bus_protocols_t           protocols;
    spi_host_device_t             host_id;
    spi_dma_chan_t                dma_channel;
    gpio_num_t                    mosi;
    gpio_num_t                    miso;
    gpio_num_t                    sclk;
    gpio_num_t                    csio;
    gpio_num_t                    dcio;
    int                           max_transfer_sz;
    bool                          owns_host; /* the host was initialized by DGX, shared by its displays on it */
    int                           clock_speed_hz;
    uint8_t                       data_queued; /* transfers of pixels in trans_data on their way: 0, 1, or 2 inside write_data_async() */
    uint8_t                       data_slot;   /* the one of trans_data the next transfer of pixels takes */
    bool                          psram_late;  /* pixels from PSRAM go through a copy: the wire is faster than DMA from PSRAM keeps up with, or it fell behind once */
    spi_device_handle_t           spi;
    spi_device_interface_config_t devcfg;
    spi_transaction_t trans_data[2]; /* pixels, queued in turn: one may be under way when a call returns */
    spi_transaction_t trans_sync; /* commands and parameters, waited for */
} dgx_spi_bus_t;

static inline void *set_dc_pin(gpio_num_t dc, uint8_t val)
{
    return (void *)(dc >= 0 ? ((val & 1u) | ((uint32_t)dc << 1)) : 0);
}

static inline gpio_num_t get_dc_pin(void *user_ptr)
{
    uint32_t dc = (uint32_t)user_ptr;
    return dc;
}

void dgx_spi_wait_pending(struct _dgx_bus_protocols_t *_bus);

/*
 * The hosts DGX initializes itself, shared by its SPI and QSPI displays. A
 * host is taken, or checked to have the same lines when it is taken already;
 * GPIO_NUM_NC for quadwp and quadhd is a display of one data line, which goes
 * on a host of four too. A display joins the host once it is on it and leaves
 * it when disposed; the last to leave frees the host. A host taken for a
 * display that then failed is released: freed if no display is on it.
 */
esp_err_t dgx_spi_take_host(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk, gpio_num_t quadwp,
                            gpio_num_t quadhd);
void dgx_spi_release_host(spi_host_device_t host_id);
void dgx_spi_host_join(spi_host_device_t host_id);
void dgx_spi_host_leave(spi_host_device_t host_id);

/*
 * The length of the buffer of a display on a host, once its device is on it:
 * half of it takes about 390 us on the wire at the clock the device has got
 * over `data_lines`, it is 4000 bytes at least, 32000 at most and no more
 * than the host takes in one transfer. `most`, when it is not 0, is the
 * program's limit: no longer than it, in whole words, and 64 bytes at least.
 */
size_t dgx_spi_buffer_len(spi_host_device_t host_id, spi_device_handle_t dev, unsigned data_lines, size_t most);

/*
 * The DMA of the host takes pixels from there as they lie: internal memory it
 * reaches, from a whole word, or PSRAM on a chip whose DMA reaches PSRAM (not
 * from SPI3 of an ESP32-S2). Pixels in PSRAM go with SPI_TRANS_DMA_USE_PSRAM,
 * or the driver copies them into internal memory first.
 */
bool dgx_spi_dma_reaches(spi_host_device_t host_id, const void *pixels);

/*
 * The result of a transfer of pixels. One from PSRAM that the DMA fell
 * behind on has ended with wrong pixels (ESP_ERR_INVALID_STATE with
 * SPI_TRANS_DMA_TX_FAIL): PSRAM does not keep up with the bus at its clock,
 * so *psram_late is set and pixels in PSRAM go through internal memory from
 * then on, as the driver copies them without SPI_TRANS_DMA_USE_PSRAM. The
 * log tells of it once. Any other failure stops the program, as before.
 */
void dgx_spi_check_result(esp_err_t rc, const spi_transaction_t *trans, bool *psram_late, const char *what);

/*
 * The fastest wire DMA from PSRAM was seen to keep up with: 26.7 MHz on four
 * lines, on an ESP32-S3 with octal PSRAM at 80 MHz, also while the other
 * core wrote PSRAM all the time. At 40 MHz on four lines it fell behind on
 * the first frame with nothing else running. A device whose wire is faster
 * starts with pixels from PSRAM going through a copy.
 */
#define DGX_SPI_PSRAM_DMA_KBITS 106667u

/* the wire of the device is faster than DMA from PSRAM keeps up with */
bool dgx_spi_psram_too_slow(spi_device_handle_t dev, unsigned data_lines);
