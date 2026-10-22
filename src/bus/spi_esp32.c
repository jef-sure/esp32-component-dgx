/*
 * spi_esp32.c
 *
 *  Created on: 14.10.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <string.h>

#include "bus/dgx_spi_esp32.h"
#include "dgx_arch_esp32.h"
#include "dgx_spi_esp32_priv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_reg.h"

static const char TAG[] = "DGX SPI BUS";

/* transfers the device may hold at once; two of pixels are queued at most, one behind the other, the number is the one the bus has always been made with */
#define DGX_SPI_QUEUE_SIZE 7

// This function is called (in irq context!) just before a transmission starts. It will
// set the D/C line to the value indicated in the user field.
static void IRAM_ATTR dgx_spi_set_dc(spi_transaction_t *t)
{
    uint32_t dc = get_dc_pin(t->user);
    // dgx_gpio_set_level((dc >> 1), dc & 1);
    if (dc & 1) {
        REG_WRITE(GPIO_OUT_W1TS_REG, 1u << (dc >> 1));
    } else {
        REG_WRITE(GPIO_OUT_W1TC_REG, 1u << (dc >> 1));
    }
}

static void IRAM_ATTR dgx_spi_set_dc_high(spi_transaction_t *t)
{
#ifdef GPIO_OUT1_W1TS_REG
    uint32_t dc = get_dc_pin(t->user);
    if (dc & 1) {
        REG_WRITE(GPIO_OUT1_W1TS_REG, 1u << ((dc >> 1u) - 32u));
    } else {
        REG_WRITE(GPIO_OUT1_W1TC_REG, 1u << ((dc >> 1u) - 32u));
    }
#endif
}

void dgx_spi_wait_pending(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_bus_t     *bus = (dgx_spi_bus_t *)_bus;
    spi_transaction_t *rtrans;
    /* the only transfers left on their way when a call returns are those of pixels, in trans_data */
    while (bus->data_queued) {
        esp_err_t ret = spi_device_get_trans_result(bus->spi, &rtrans, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "dgx_spi_wait_pending:spi_device_get_trans_result failed");
            assert(0); // Crash!
        }
        if (rtrans == &bus->trans_data[0] || rtrans == &bus->trans_data[1]) --bus->data_queued;
    }
}

static void dgx_spi_sync_write(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_wait_pending(_bus);
}

static void dgx_spi_send_command(struct _dgx_bus_protocols_t *_bus, uint8_t cmd)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    esp_err_t      ret;
    memset(&bus->trans_sync, 0, sizeof(bus->trans_sync));
    bus->trans_sync.length     = 8; // Command is 8 bits
    bus->trans_sync.flags      = SPI_TRANS_USE_TXDATA;
    bus->trans_sync.tx_data[0] = cmd;
    bus->trans_sync.user       = set_dc_pin(bus->dcio, 0); // D/C needs to be set to 0
    dgx_spi_wait_pending(_bus);
    ret = spi_device_queue_trans(bus->spi, &bus->trans_sync, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_command/spi_device_queue_trans failed");
        return; // nothing was queued: waiting for a result would block forever
    }
    spi_transaction_t *ret_trans = NULL;
    ret = spi_device_get_trans_result(bus->spi, &ret_trans, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_command/spi_device_get_trans_result failed");
        return;
    }
    if (ret_trans != &bus->trans_sync) {
        ESP_LOGE(TAG, "dgx_spi_send_command/ret_trans != &bus->trans_sync failed");
    }
}

static void dgx_spi_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    esp_err_t      ret;
    if (bus->data_queued) dgx_spi_sync_write(_bus);
    memset(&bus->trans_sync, 0, sizeof(bus->trans_sync));
    bus->trans_sync.length    = len; // Command is 8 bits
    bus->trans_sync.flags     = 0;
    bus->trans_sync.tx_buffer = cmds;
    bus->trans_sync.user      = set_dc_pin(bus->dcio, 0); // D/C needs to be set to 0
    dgx_spi_wait_pending(_bus);
    ret = spi_device_transmit(bus->spi, &bus->trans_sync);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_commands failed");
    }
}

static void dgx_spi_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    esp_err_t      ret;
    size_t         len_bytes;
    if (len == 0) return;
    memset(&bus->trans_sync, 0, sizeof(spi_transaction_t));
    if (len <= 32) {
        len_bytes             = (len + 7u) / 8u;
        bus->trans_sync.flags = SPI_TRANS_USE_TXDATA;
        memcpy(bus->trans_sync.tx_data, data, len_bytes);
    } else {
        bus->trans_sync.tx_buffer = data;
    }
    bus->trans_sync.length = len;                      // Data length in bits
    bus->trans_sync.user   = set_dc_pin(bus->dcio, 1); // DC = 1 when data
    dgx_spi_wait_pending(_bus);
    ret = spi_device_queue_trans(bus->spi, &bus->trans_sync, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_data/spi_device_queue_trans failed");
        return; // nothing was queued: waiting for a result would block forever
    }
    spi_transaction_t *ret_trans = NULL;
    ret = spi_device_get_trans_result(bus->spi, &ret_trans, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_data/spi_device_get_trans_result failed");
        return;
    }
    if (ret_trans != &bus->trans_sync) {
        ESP_LOGE(TAG, "dgx_spi_send_data/ret_trans != &bus->trans_sync failed");
    }
}

static uint32_t dgx_spi_read_data(struct _dgx_bus_protocols_t *_bus, uint8_t *data, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    esp_err_t      ret;
    if (len == 0) return len;
    memset(&bus->trans_sync, 0, sizeof(spi_transaction_t));
    bus->trans_sync.rx_buffer = data;
    bus->trans_sync.length    = len;                      // Data length in bits
    bus->trans_sync.user      = set_dc_pin(bus->dcio, 1); // DC = 1 when data
    dgx_spi_wait_pending(_bus);
    ret = spi_device_transmit(bus->spi, &bus->trans_sync);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_read_data failed");
        return 0;
    }
    return bus->trans_sync.rxlength;
}

/* the next of trans_data made a transfer of pixels; the transfers before it no longer use it */
static spi_transaction_t *dgx_spi_data_trans(dgx_spi_bus_t *bus, const uint8_t *data, uint32_t len)
{
    spi_transaction_t *trans = &bus->trans_data[bus->data_slot];
    bus->data_slot ^= 1u;
    memset(trans, 0, sizeof(spi_transaction_t));
    if (len <= 32) {
        trans->flags = SPI_TRANS_USE_TXDATA;
        memcpy(trans->tx_data, data, (len + 7u) / 8u);
    } else {
        trans->tx_buffer = data;
    }
    trans->length = len;
    trans->user   = set_dc_pin(bus->dcio, 1);
    return trans;
}

/*
 * Pixels queued right behind those under way, and then the ones under way
 * waited for: the bus goes from one transfer to the next with no pause for the
 * processor to queue it, and only this one is on its way when it returns, as
 * before. A short one is polled, after all the others.
 */
static void dgx_spi_send_data_async(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    if (len == 0) {
        ESP_LOGE(TAG, "dgx_spi_send_data_async len == 0!");
        return;
    }
    if (len <= 2048) {
        if (bus->data_queued) dgx_spi_wait_pending(_bus);
        esp_err_t rc = spi_device_polling_transmit(bus->spi, dgx_spi_data_trans(bus, data, len));
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "spi_device_polling_transmit/dgx_spi_send_data_async failed");
            assert(0);
        }
        return;
    }
    esp_err_t rc = spi_device_queue_trans(bus->spi, dgx_spi_data_trans(bus, data, len), portMAX_DELAY);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_queue_trans/dgx_spi_send_data_async failed");
        assert(0);
    }
    /* results come in the order the transfers were queued: the first ones are those before this one */
    for (++bus->data_queued; bus->data_queued > 1; --bus->data_queued) {
        spi_transaction_t *rtrans;
        if (spi_device_get_trans_result(bus->spi, &rtrans, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "dgx_spi_send_data_async:spi_device_get_trans_result failed");
            assert(0);
        }
    }
}

/*
 * The SPI hosts DGX initializes, shared by the DGX displays on a host and freed
 * with the last of them. A host the program initialized and named to
 * dgx_spi_init_on_bus() is the program's: DGX only adds its device to it.
 */
typedef struct {
    uint16_t       users;
    bool           taken;
    gpio_num_t     mosi, miso, sclk;
    spi_dma_chan_t dma_chan;
} dgx_spi_host_t;

static dgx_spi_host_t dgx_spi_hosts[SOC_SPI_PERIPH_NUM];

static esp_err_t dgx_spi_take_host(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk)
{
    if (host_id < 0 || host_id >= SOC_SPI_PERIPH_NUM) {
        ESP_LOGE(TAG, "SPI host %d does not exist", (int)host_id);
        return ESP_ERR_INVALID_ARG;
    }
    dgx_spi_host_t *h = &dgx_spi_hosts[host_id];
    if (h->taken) {
        if (h->mosi != mosi || h->miso != miso || h->sclk != sclk || h->dma_chan != dma_chan) {
            ESP_LOGE(TAG, "SPI host %d already uses MOSI %d, MISO %d, SCLK %d and DMA %d, not MOSI %d, MISO %d, SCLK %d and DMA %d", (int)host_id,
                     (int)h->mosi, (int)h->miso, (int)h->sclk, (int)h->dma_chan, (int)mosi, (int)miso, (int)sclk, (int)dma_chan);
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }
    spi_bus_config_t buscfg;
    memset(&buscfg, 0, sizeof(buscfg));
    buscfg.miso_io_num     = miso;
    buscfg.mosi_io_num     = mosi;
    buscfg.sclk_io_num     = sclk;
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = -1; // the default of a bus with DMA, 4092 bytes
    esp_err_t rc           = spi_bus_initialize(host_id, &buscfg, dma_chan);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "SPI host %d: spi_bus_initialize failed (%s); a host the program has initialized itself goes to dgx_spi_init_on_bus()",
                 (int)host_id, esp_err_to_name(rc));
        return rc;
    }
    *h = (dgx_spi_host_t){.taken = true, .mosi = mosi, .miso = miso, .sclk = sclk, .dma_chan = dma_chan};
    return ESP_OK;
}

/* a host freed when no DGX display uses it; kept when the program has added devices of its own to it */
static void dgx_spi_release_host(spi_host_device_t host_id)
{
    dgx_spi_host_t *h = &dgx_spi_hosts[host_id];
    if (h->users || !h->taken) return;
    esp_err_t rc = spi_bus_free(host_id);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "SPI host %d kept, other devices are on it (%s)", (int)host_id, esp_err_to_name(rc));
        return;
    }
    h->taken = false;
}

static void dgx_spi_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    dgx_spi_wait_pending(_bus); // the device cannot be removed with transactions in flight
    spi_bus_remove_device(bus->spi);
    if (bus->owns_host && dgx_spi_hosts[bus->host_id].users) {
        --dgx_spi_hosts[bus->host_id].users;
        dgx_spi_release_host(bus->host_id);
    }
    free(bus->protocols.buffer);
    heap_caps_free(bus); // for symmetry with heap_caps_calloc
}

static uint8_t *dgx_spi_memory_allocator(uint32_t len)
{
    return (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_DMA);
}

/* the display's device added to an initialized host: the part both constructors share */
static dgx_bus_protocols_t *dgx_spi_attach(spi_host_device_t host_id, bool owns_host, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso,
                                           gpio_num_t sclk, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)heap_caps_calloc(1, sizeof(dgx_spi_bus_t), MALLOC_CAP_DMA);
    if (bus == 0) {
        ESP_LOGE(TAG, "dgx_spi_init failed to allocate dgx_spi_bus_t");
        return NULL;
    }
    bus->protocols.bus_type         = DGX_BUS_SPI;
    bus->protocols.bus_name         = "SPI";
    bus->protocols.write_command    = dgx_spi_send_command;
    bus->protocols.write_commands   = dgx_spi_send_commands;
    bus->protocols.read_data        = dgx_spi_read_data;
    bus->protocols.write_data       = dgx_spi_send_data;
    bus->protocols.write_data_async = dgx_spi_send_data_async;
    bus->protocols.sync_write       = dgx_spi_sync_write;
    bus->protocols.dispose          = dgx_spi_dispose_bus_func;
    bus->protocols.memory_allocator = dgx_spi_memory_allocator;
    bus->protocols.buffer_len       = 4000;
    bus->protocols.buffer           = bus->protocols.memory_allocator(bus->protocols.buffer_len);
    if (!bus->protocols.buffer) {
        ESP_LOGE(TAG, "dgx_spi_init buffer allocation failed");
        heap_caps_free(bus);
        return NULL;
    }
    bus->owns_host             = owns_host;
    bus->host_id               = host_id;
    bus->dma_channel           = dma_chan;
    bus->dcio                  = dc;
    bus->csio                  = cs;
    bus->miso                  = miso;
    bus->mosi                  = mosi;
    bus->sclk                  = sclk;
    bus->clock_speed_hz        = clock_speed_hz;
    bus->max_transfer_sz       = -1;
    bus->data_queued           = 0;
    bus->devcfg.clock_speed_hz = clock_speed_hz;
    bus->devcfg.spics_io_num   = cs;
    bus->devcfg.queue_size     = DGX_SPI_QUEUE_SIZE;
    if (dc >= 0) {
        dgx_gpio_set_direction(dc, GPIO_MODE_OUTPUT);
        if (dc > 31) {
            bus->devcfg.pre_cb = dgx_spi_set_dc_high;
        } else {
            bus->devcfg.pre_cb = dgx_spi_set_dc;
        }
    }
    bus->devcfg.mode = cpolpha_mode;
    esp_err_t rc     = spi_bus_add_device(bus->host_id, &bus->devcfg, &bus->spi);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_init spi_bus_add_device failed: %s", esp_err_to_name(rc));
        free(bus->protocols.buffer);
        heap_caps_free(bus);
        return NULL;
    }
    return (dgx_bus_protocols_t *)bus;
}

dgx_bus_protocols_t *dgx_spi_init(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk,
                                  gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t /* default = 0 */ cpolpha_mode)
{
    if (dgx_spi_take_host(host_id, dma_chan, mosi, miso, sclk) != ESP_OK) return NULL;
    dgx_bus_protocols_t *bus = dgx_spi_attach(host_id, true, dma_chan, mosi, miso, sclk, cs, dc, clock_speed_hz, cpolpha_mode);
    if (!bus) {
        dgx_spi_release_host(host_id);
        return NULL;
    }
    ++dgx_spi_hosts[host_id].users;
    return bus;
}

dgx_bus_protocols_t *dgx_spi_init_on_bus(spi_host_device_t host_id, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode)
{
    if (host_id < 0 || host_id >= SOC_SPI_PERIPH_NUM) {
        ESP_LOGE(TAG, "SPI host %d does not exist", (int)host_id);
        return NULL;
    }
    return dgx_spi_attach(host_id, false, SPI_DMA_CH_AUTO, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, cs, dc, clock_speed_hz, cpolpha_mode);
}
