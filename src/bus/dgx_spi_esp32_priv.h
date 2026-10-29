#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bus/dgx_spi_esp32.h"

/*
 * What the SPI and the QSPI bus share, and what each begins with: the device
 * of the display on its host and the transfers of pixels on their way. The
 * functions of dgx_spi_dev_* are written once, in spi_esp32.c; the two buses
 * differ only in what goes before the pixels of a transfer, dgx_spi_head_t.
 */
typedef struct _dgx_spi_dev_t
{
    dgx_bus_protocols_t protocols;
    spi_host_device_t   host_id;
    bool                owns_host;   /* the host was initialized by DGX, shared by its displays on it */
    bool                psram_late;  /* pixels from PSRAM go through a copy: the wire is faster than DMA from PSRAM keeps up with, or it fell behind once */
    uint8_t             data_queued; /* transfers of pixels in trans_data on their way: 0, 1, or 2 inside write_data_async() */
    uint8_t             data_slot;   /* the one of trans_data the next transfer of pixels takes */
    spi_device_handle_t spi;
    spi_transaction_t   trans_data[2]; /* pixels, queued in turn: one may be under way when a call returns */
    spi_transaction_t   trans_sync;    /* commands and parameters, waited for */
} dgx_spi_dev_t;

/* what a transfer of pixels carries before them: the D/C line of SPI in `user`; the lines, the opcode and the command of QSPI */
typedef struct
{
    uint32_t flags;
    uint16_t cmd;
    uint32_t addr;
    void    *user;
} dgx_spi_head_t;

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

/*
 * The device of a display added to its host, for a bus whose protocols are
 * filled: the buffer of the bus is as long as the clock the device has got
 * asks over `data_lines`, and whether DMA from PSRAM keeps up with it is
 * known. On failure the bus is freed and NULL returned.
 */
dgx_bus_protocols_t *dgx_spi_dev_attach(dgx_spi_dev_t *dev, spi_host_device_t host_id, bool owns_host, const spi_device_interface_config_t *devcfg,
                                        unsigned data_lines, const dgx_spi_options_t *options);

/* the transfers of pixels on their way waited for: none is when this returns. Inline, as no call is made when none is on its way */
void dgx_spi_dev_wait_queued(dgx_spi_dev_t *dev);

static inline void dgx_spi_dev_wait(dgx_spi_dev_t *dev)
{
    if (dev->data_queued) dgx_spi_dev_wait_queued(dev);
}

/*
 * One piece of pixels, no longer than the host takes: queued right behind
 * the pixels under way, and then those waited for, so the bus goes from one
 * transfer to the next with no pause for the processor to queue it, and only
 * this one is on its way when it returns. A short one is polled, after all
 * the others. False when the driver refused it, with assertions off: its
 * pixels are lost, and the pieces before it have been waited for.
 */
bool dgx_spi_dev_send_piece(dgx_spi_dev_t *dev, const uint8_t *data, uint32_t len, const dgx_spi_head_t *head);

/* sends_from, memory_allocator and dispose of both buses */
bool     dgx_spi_dev_sends_from(struct _dgx_bus_protocols_t *bus, const void *pixels);
uint8_t *dgx_spi_dev_memory(uint32_t len);
void     dgx_spi_dev_dispose(struct _dgx_bus_protocols_t *bus);

/*
 * Pixels sent by `send_piece` in pieces no longer than the buffer of the bus:
 * the host takes no transfer longer than that, and a caller of write_area
 * may hand a whole frame. A piece is a multiple of 12 bytes, whole pixels of
 * 2, 3 and 4 bytes and whole words for the DMA; the panel goes on writing
 * its memory from one to the next, as it does between the pieces of a fill.
 * From PSRAM whose DMA asks 16 bytes, as on a P4 under flash encryption, a
 * piece is a multiple of 48 bytes where the buffer is that long, so that the
 * pieces stay on 16 bytes from a first one that starts there. A buffer shorter than 12 bytes gets pieces of
 * 12. Pixels no longer than the buffer go whole, the buffer itself above
 * all. The SPI and QSPI buses each send a piece their own way.
 */
typedef void (*dgx_spi_piece_func)(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len);
void dgx_spi_send_pieces(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len, dgx_spi_piece_func send_piece);
