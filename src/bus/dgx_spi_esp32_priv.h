#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bus/dgx_spi_esp32.h"

/*
 * The flags of the SPI driver for DMA straight from PSRAM,
 * SPI_TRANS_DMA_USE_PSRAM and SPI_TRANS_DMA_TX_FAIL, came with IDF 5.5.
 * Before it nothing is sent from PSRAM where it lies: the driver copies such
 * pixels into internal memory itself, as it does without the flag.
 */
#if defined(SPI_TRANS_DMA_USE_PSRAM) && defined(SPI_TRANS_DMA_TX_FAIL)
#define DGX_SPI_DMA_PSRAM 1
#else
#define DGX_SPI_DMA_PSRAM 0
#endif

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
    return (void *)(uintptr_t)(dc >= 0 ? ((val & 1u) | ((uint32_t)dc << 1)) : 0);
}

static inline gpio_num_t get_dc_pin(void *user_ptr)
{
    uint32_t dc = (uint32_t)(uintptr_t)user_ptr;
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
 * than the host takes in one transfer. A host with DMA takes 4092 bytes at
 * least: the driver rounds max_transfer_sz up to whole DMA descriptors of
 * 4092 bytes. `most`, when it is not 0, is the program's limit: no longer
 * than it, in whole words, and 4 bytes at least.
 */
size_t dgx_spi_buffer_len(spi_host_device_t host_id, spi_device_handle_t dev, unsigned data_lines, size_t most);

/*
 * What DMA from PSRAM asks of where a transfer starts and of its length, in
 * bytes: DGX_SPI_PSRAM_ALIGN on a chip whose GDMA does not align by itself
 * (AHB GDMA from version 2: P4, C5, C61) under flash encryption, or with
 * PSRAM ECC from IDF 6.0, and 1 on any other: an ESP32-S2 and an ESP32-S3
 * send from any byte and of any length (gdma_config_transfer() of IDF). The
 * driver copies a transfer from PSRAM that is off it into PSRAM of its own
 * first, so DGX sends from PSRAM where it lies only from that alignment and
 * in pieces of a multiple of it; a piece of another length goes without
 * SPI_TRANS_DMA_USE_PSRAM, for the driver to copy into internal memory. The
 * weighted arbitration of GDMA, experimental in IDF 6, asks more and is not
 * counted.
 */
#define DGX_SPI_PSRAM_ALIGN 16u
unsigned dgx_spi_psram_align(void);

/*
 * The DMA of the host takes pixels from there as they lie: internal memory it
 * reaches, from a whole word, or PSRAM on a chip whose DMA reaches PSRAM (not
 * from SPI3 of an ESP32-S2), from dgx_spi_psram_align(), with an IDF that has
 * SPI_TRANS_DMA_USE_PSRAM. Pixels in PSRAM go with that flag, or the driver
 * copies them into internal memory first.
 */
bool dgx_spi_dma_reaches(spi_host_device_t host_id, const void *pixels);

/*
 * The result of a transfer of pixels. One from PSRAM that the DMA fell
 * behind on has ended with wrong pixels (ESP_ERR_INVALID_STATE with
 * SPI_TRANS_DMA_TX_FAIL): PSRAM does not keep up with the bus at its clock,
 * so *psram_late is set and pixels in PSRAM go through internal memory from
 * then on, as the driver copies them without SPI_TRANS_DMA_USE_PSRAM. The
 * log tells of it once. Any other failure stops the program, as before. An
 * IDF without those flags sends nothing from PSRAM by DMA, and has no such
 * failure.
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

/*
 * Pixels sent by `send_piece` in pieces no longer than the buffer of the bus:
 * the host takes no transfer longer than that, and a caller of write_area
 * may hand a whole frame. A piece is a multiple of 12 bytes, whole pixels of
 * 2, 3 and 4 bytes and whole words for the DMA; the panel goes on writing
 * its memory from one to the next, as it does between the pieces of a fill.
 * From PSRAM whose DMA asks DGX_SPI_PSRAM_ALIGN, a piece is a multiple of 48
 * bytes where the buffer is that long, so that the pieces stay on it from a
 * first one that starts there. A buffer shorter than 12 bytes gets pieces of
 * 12. Pixels no
 * longer than the buffer go whole, the buffer itself above all. The SPI and
 * QSPI buses each send a piece their own way.
 */
typedef void (*dgx_spi_piece_func)(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len);
void dgx_spi_send_pieces(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len, dgx_spi_piece_func send_piece);
