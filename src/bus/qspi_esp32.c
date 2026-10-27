/*
 * qspi_esp32.c
 *
 *  Created on: 23.10.2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <string.h>

#include "bus/dgx_qspi_esp32.h"
#include "dgx_spi_esp32_priv.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char TAG[] = "DGX QSPI BUS";

/*
 * A transfer of a QSPI panel is an opcode of 8 bits and an address of 24 on
 * one line, then its data. The command of the panel is the middle byte of the
 * address. With opcode 0x02 the data are the parameters of the command, on one
 * line; with 0x32 they are pixels, on four.
 *
 * All the pixels of a window go in one selection of the chip, as the maker of
 * the Guition JC4827W543 sends them (Arduino_ESP32QSPI of its Arduino_GFX):
 * the first piece with the opcode 0x32 and the command that writes memory,
 * 0x2C, the pieces after it with no opcode and no address at all, and the
 * chip select held low between them while the next piece is made. The
 * NV3041A does not take a piece that goes on with the 0x3C of the MIPI DCS
 * after the chip select has risen: stripes came at the start of a window. So
 * the chip select is a GPIO driven here, not the one of the SPI hardware,
 * which rises after every transfer; the screen tells the bus when the last
 * pixel of the window has gone (end_pixels), and any command ends the
 * selection too.
 */
#define DGX_QSPI_OP_COMMAND 0x02u
#define DGX_QSPI_OP_PIXELS  0x32u
/* pixels with no window begun before them go on where the panel stands, as the maker's driver sends its pixels */
#define DGX_QSPI_RAMWRC 0x3Cu

/* transfers the device may hold at once, as on the SPI bus: two of pixels are queued at most */
#define DGX_QSPI_QUEUE_SIZE 7

typedef struct _dgx_qspi_bus_t
{
    dgx_bus_protocols_t protocols;
    spi_host_device_t   host_id;
    bool                owns_host; /* the host was initialized by DGX, shared by its displays on it */
    spi_device_handle_t spi;
    gpio_num_t          cs;
    bool                selected; /* the chip select is low: a window has begun and has not ended */
    bool                acquired; /* the host is held for the selection, as it is shared with devices DGX does not know */
    /*
     * A command goes in one transfer with its data, so it is held back until
     * the data come, or until it is known that there are none: another
     * command, or sync_write().
     */
    uint8_t             command;
    bool                command_pending;
    bool                complained;  /* parameters with no command have been refused, and the log told of it once */
    uint8_t             data_queued; /* transfers of pixels in trans_data on their way: 0, 1, or 2 inside write_data_async() */
    uint8_t             data_slot;   /* the one of trans_data the next transfer of pixels takes */
    spi_transaction_ext_t trans_data[2]; /* pixels, queued in turn: one may be under way when a call returns */
    spi_transaction_t   trans_sync;    /* commands and parameters, waited for */
} dgx_qspi_bus_t;

static void dgx_qspi_wait_pending(dgx_qspi_bus_t *bus)
{
    spi_transaction_t *rtrans;
    while (bus->data_queued) {
        if (spi_device_get_trans_result(bus->spi, &rtrans, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "dgx_qspi_wait_pending:spi_device_get_trans_result failed");
            assert(0);
        }
        --bus->data_queued;
    }
}

/*
 * The chip select low for a transfer or a window. On a host shared with
 * devices DGX does not know the host is held as long as it is low: a transfer
 * of another device would go into the panel.
 */
static void dgx_qspi_select(dgx_qspi_bus_t *bus)
{
    if (!bus->owns_host && !bus->acquired) {
        spi_device_acquire_bus(bus->spi, portMAX_DELAY);
        bus->acquired = true;
    }
    gpio_set_level(bus->cs, 0);
    bus->selected = true;
}

/* the chip select high once all that was sent in the selection is out */
static void dgx_qspi_deselect(dgx_qspi_bus_t *bus)
{
    if (!bus->selected) return;
    dgx_qspi_wait_pending(bus);
    gpio_set_level(bus->cs, 1);
    bus->selected = false;
    if (bus->acquired) {
        spi_device_release_bus(bus->spi);
        bus->acquired = false;
    }
}

/* a command and its parameters on one line, in a selection of its own; a window begun ends before it */
static void dgx_qspi_send_command_with(dgx_qspi_bus_t *bus, uint8_t cmd, const uint8_t *data, uint32_t len)
{
    spi_transaction_t *trans = &bus->trans_sync;
    memset(trans, 0, sizeof(*trans));
    trans->cmd  = DGX_QSPI_OP_COMMAND;
    trans->addr = (uint32_t)cmd << 8;
    if (len > 32) {
        trans->tx_buffer = data;
    } else if (len) { // a command alone has no data phase at all
        trans->flags = SPI_TRANS_USE_TXDATA;
        memcpy(trans->tx_data, data, (len + 7u) / 8u);
    }
    trans->length = len;
    dgx_qspi_deselect(bus);
    dgx_qspi_wait_pending(bus);
    dgx_qspi_select(bus);
    esp_err_t ret = spi_device_polling_transmit(bus->spi, trans);
    dgx_qspi_deselect(bus);
    if (ret != ESP_OK) ESP_LOGE(TAG, "command 0x%02x with %u bits failed: %s", cmd, (unsigned)len, esp_err_to_name(ret));
}

/* the command that was held back has no data */
static void dgx_qspi_flush_command(dgx_qspi_bus_t *bus)
{
    if (!bus->command_pending) return;
    bus->command_pending = false;
    dgx_qspi_send_command_with(bus, bus->command, NULL, 0);
}

static void dgx_qspi_send_command(struct _dgx_bus_protocols_t *_bus, uint8_t cmd)
{
    dgx_qspi_bus_t *bus = (dgx_qspi_bus_t *)_bus;
    dgx_qspi_flush_command(bus);
    bus->command         = cmd;
    bus->command_pending = true;
}

static void dgx_qspi_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len)
{
    dgx_qspi_bus_t *bus      = (dgx_qspi_bus_t *)_bus;
    uint32_t        byte_len = (len + 7u) / 8u;
    for (uint32_t i = 0; i < byte_len; i++) {
        dgx_qspi_send_command(_bus, cmds[i]);
    }
    dgx_qspi_flush_command(bus);
}

static void dgx_qspi_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_qspi_bus_t *bus = (dgx_qspi_bus_t *)_bus;
    if (len == 0) return;
    if (!bus->command_pending) {
        if (!bus->complained) {
            bus->complained = true;
            ESP_LOGE(TAG, "parameters with no command before them cannot be sent on this bus, they are dropped");
        }
        return;
    }
    bus->command_pending = false;
    dgx_qspi_send_command_with(bus, bus->command, data, len);
}

/*
 * The next of trans_data made a transfer of pixels. With a command held back
 * (0x2C, which starts the window) a new selection begins and the transfer
 * carries the opcode and the command; inside a selection begun it is the
 * pixels alone, going on where the piece before ended.
 */
static spi_transaction_t *dgx_qspi_data_trans(dgx_qspi_bus_t *bus, const uint8_t *data, uint32_t len)
{
    spi_transaction_ext_t *ext   = &bus->trans_data[bus->data_slot];
    spi_transaction_t     *trans = &ext->base;
    bus->data_slot ^= 1u;
    memset(ext, 0, sizeof(*ext));
    if (len <= 32) {
        trans->flags = SPI_TRANS_MODE_QIO | SPI_TRANS_USE_TXDATA;
        memcpy(trans->tx_data, data, (len + 7u) / 8u);
    } else {
        trans->flags     = SPI_TRANS_MODE_QIO;
        trans->tx_buffer = data;
    }
    if (bus->command_pending || !bus->selected) {
        dgx_qspi_deselect(bus);
        dgx_qspi_select(bus);
        trans->cmd           = DGX_QSPI_OP_PIXELS;
        trans->addr          = (uint32_t)(bus->command_pending ? bus->command : DGX_QSPI_RAMWRC) << 8;
        bus->command_pending = false;
    } else {
        trans->flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
        ext->command_bits = 0;
        ext->address_bits = 0;
        ext->dummy_bits   = 0;
    }
    trans->length = len;
    return trans;
}

/* as on the SPI bus: queued right behind the pixels under way, then those waited for; a short piece is polled */
static void dgx_qspi_send_data_async(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_qspi_bus_t *bus = (dgx_qspi_bus_t *)_bus;
    if (len == 0) {
        ESP_LOGE(TAG, "dgx_qspi_send_data_async len == 0!");
        return;
    }
    if (len <= 2048) {
        dgx_qspi_wait_pending(bus);
        esp_err_t rc = spi_device_polling_transmit(bus->spi, dgx_qspi_data_trans(bus, data, len));
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "spi_device_polling_transmit/dgx_qspi_send_data_async failed");
            assert(0);
        }
        return;
    }
    esp_err_t rc = spi_device_queue_trans(bus->spi, dgx_qspi_data_trans(bus, data, len), portMAX_DELAY);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_queue_trans/dgx_qspi_send_data_async failed");
        assert(0);
    }
    for (++bus->data_queued; bus->data_queued > 1; --bus->data_queued) {
        spi_transaction_t *rtrans;
        if (spi_device_get_trans_result(bus->spi, &rtrans, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "dgx_qspi_send_data_async:spi_device_get_trans_result failed");
            assert(0);
        }
    }
}

/* the pixels on their way are out; a window begun stays selected, for more of its pixels */
static void dgx_qspi_sync_write(struct _dgx_bus_protocols_t *_bus)
{
    dgx_qspi_bus_t *bus = (dgx_qspi_bus_t *)_bus;
    dgx_qspi_flush_command(bus);
    dgx_qspi_wait_pending(bus);
}

/* the window has all its pixels: the selection ends once they are out */
static void dgx_qspi_end_pixels(struct _dgx_bus_protocols_t *_bus)
{
    dgx_qspi_deselect((dgx_qspi_bus_t *)_bus);
}

/* the display is not read through this bus: nothing is read, and the buffer is left as it was */
static uint32_t dgx_qspi_read_data(struct _dgx_bus_protocols_t *_bus, uint8_t *data, uint32_t len)
{
    (void)_bus, (void)data, (void)len;
    return 0;
}

static void dgx_qspi_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_qspi_bus_t *bus = (dgx_qspi_bus_t *)_bus;
    dgx_qspi_sync_write(_bus); // the device cannot be removed with transactions in flight
    dgx_qspi_deselect(bus);
    spi_bus_remove_device(bus->spi);
    if (bus->owns_host) dgx_spi_host_leave(bus->host_id);
    free(bus->protocols.buffer);
    heap_caps_free(bus);
}

static uint8_t *dgx_qspi_memory_allocator(uint32_t len)
{
    return (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_DMA);
}

static dgx_bus_protocols_t *dgx_qspi_attach(spi_host_device_t host_id, bool owns_host, gpio_num_t cs, int clock_speed_hz, uint8_t cpolpha_mode)
{
    dgx_qspi_bus_t *bus = (dgx_qspi_bus_t *)heap_caps_calloc(1, sizeof(dgx_qspi_bus_t), MALLOC_CAP_DMA);
    if (bus == 0) {
        ESP_LOGE(TAG, "dgx_qspi_init failed to allocate dgx_qspi_bus_t");
        return NULL;
    }
    bus->protocols.bus_type         = DGX_BUS_QSPI;
    bus->protocols.bus_name         = "QSPI";
    bus->protocols.write_command    = dgx_qspi_send_command;
    bus->protocols.write_commands   = dgx_qspi_send_commands;
    bus->protocols.read_data        = dgx_qspi_read_data;
    bus->protocols.write_data       = dgx_qspi_send_data;
    bus->protocols.write_data_async = dgx_qspi_send_data_async;
    bus->protocols.sync_write       = dgx_qspi_sync_write;
    bus->protocols.end_pixels       = dgx_qspi_end_pixels;
    bus->protocols.dispose          = dgx_qspi_dispose_bus_func;
    bus->protocols.memory_allocator = dgx_qspi_memory_allocator;
    bus->owns_host = owns_host;
    bus->host_id   = host_id;
    bus->cs        = cs;
    /* the chip select is driven here: high, the panel not selected, from the start */
    gpio_set_level(cs, 1);
    const gpio_config_t cs_config = {.pin_bit_mask = 1ULL << cs, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&cs_config) != ESP_OK) {
        ESP_LOGE(TAG, "dgx_qspi_init: GPIO %d cannot be the chip select", (int)cs);
        heap_caps_free(bus);
        return NULL;
    }
    gpio_set_level(cs, 1);
    spi_device_interface_config_t devcfg;
    memset(&devcfg, 0, sizeof(devcfg));
    devcfg.command_bits   = 8;
    devcfg.address_bits   = 24;
    devcfg.mode           = cpolpha_mode;
    devcfg.clock_speed_hz = clock_speed_hz;
    devcfg.spics_io_num   = -1; /* driven by dgx_qspi_select() over a whole window */
    devcfg.flags          = SPI_DEVICE_HALFDUPLEX;
    devcfg.queue_size     = DGX_QSPI_QUEUE_SIZE;
    esp_err_t rc          = spi_bus_add_device(host_id, &devcfg, &bus->spi);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "dgx_qspi_init spi_bus_add_device failed: %s", esp_err_to_name(rc));
        heap_caps_free(bus);
        return NULL;
    }
    /* four lines send a buffer four times as long as one line in the same time */
    bus->protocols.buffer_len = dgx_spi_buffer_len(host_id, bus->spi, 4);
    bus->protocols.buffer     = bus->protocols.memory_allocator(bus->protocols.buffer_len);
    if (!bus->protocols.buffer) {
        ESP_LOGE(TAG, "dgx_qspi_init buffer allocation failed");
        spi_bus_remove_device(bus->spi);
        heap_caps_free(bus);
        return NULL;
    }
    {
        int khz = 0;
        if (spi_device_get_actual_freq(bus->spi, &khz) == ESP_OK && khz > 0)
            ESP_LOGI(TAG, "QSPI clock %d kHz (asked %d)", khz, clock_speed_hz / 1000);
    }
    return (dgx_bus_protocols_t *)bus;
}

dgx_bus_protocols_t *dgx_qspi_init(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t d0, gpio_num_t d1, gpio_num_t d2, gpio_num_t d3,
                                   gpio_num_t sclk, gpio_num_t cs, int clock_speed_hz, uint8_t cpolpha_mode)
{
    if (d2 == GPIO_NUM_NC || d3 == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "a QSPI bus needs all four data lines");
        return NULL;
    }
    if (dgx_spi_take_host(host_id, dma_chan, d0, d1, sclk, d2, d3) != ESP_OK) return NULL;
    dgx_bus_protocols_t *bus = dgx_qspi_attach(host_id, true, cs, clock_speed_hz, cpolpha_mode);
    if (!bus) {
        dgx_spi_release_host(host_id);
        return NULL;
    }
    dgx_spi_host_join(host_id);
    return bus;
}

dgx_bus_protocols_t *dgx_qspi_init_on_bus(spi_host_device_t host_id, gpio_num_t cs, int clock_speed_hz, uint8_t cpolpha_mode)
{
    if (host_id < 0 || host_id >= SOC_SPI_PERIPH_NUM) {
        ESP_LOGE(TAG, "SPI host %d does not exist", (int)host_id);
        return NULL;
    }
    return dgx_qspi_attach(host_id, false, cs, clock_speed_hz, cpolpha_mode);
}
