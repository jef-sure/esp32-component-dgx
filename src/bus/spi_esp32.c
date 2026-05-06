/*
 * spi_esp32.c
 *
 *  Created on: 14.10.2022
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <string.h>

#include "bus/dgx_spi_esp32.h"
#include "dgx_arch_esp32.h"
#include "dgx_spi_esp32_priv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char TAG[] = "DGX SPI BUS";

// This function is called (in irq context!) just before a transmission starts. It will
// set the D/C line to the value indicated in the user field.
static void IRAM_ATTR dgx_spi_set_dc(spi_transaction_t *t)
{
    uint32_t dc = get_dc_pin(t->user);
    // dgx_gpio_set_level((dc >> 1), dc & 1);
    // ESP_EARLY_LOGD(TAG, "dgx_spi_set_dc: gpio=%" PRIu32 " level=%" PRIu32, (dc >> 1), dc & 1);
    if (dc & 1) {
        GPIO.out_w1ts = 1u << (dc >> 1);
    } else {
        GPIO.out_w1tc = 1u << (dc >> 1);
    }
}

static void IRAM_ATTR dgx_spi_set_dc_high(spi_transaction_t *t)
{
    uint32_t dc = get_dc_pin(t->user);
    if (dc & 1) {
        GPIO.out1_w1ts.val = 1u << ((dc >> 1u) - 32u);
    } else {
        GPIO.out1_w1tc.val = 1u << ((dc >> 1u) - 32u);
    }
}

void dgx_spi_wait_pending(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_bus_t     *bus = (dgx_spi_bus_t *)_bus;
    spi_transaction_t *rtrans;
    while (bus->pending_transactions > 0 || !bus->is_trans_data_free) {
        esp_err_t ret = spi_device_get_trans_result(bus->spi, &rtrans, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "dgx_spi_wait_pending:spi_device_get_trans_result failed");
            assert(0); // Crash!
        }
        if (rtrans == &bus->trans_data) {
            bus->is_trans_data_free = true;
        } else {
            if (rtrans >= bus->trans_area && rtrans <= &bus->trans_area[DGX_SPI_TRN_DCMD_IDX]) {
                bus->pending_transactions--;
            }
        }
    }
}

static void dgx_spi_sync_write(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_bus_t     *bus = (dgx_spi_bus_t *)_bus;
    spi_transaction_t *rtrans;
    while (!bus->is_trans_data_free) {
        esp_err_t ret = spi_device_get_trans_result(bus->spi, &rtrans, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "dgx_spi_wait_pending:spi_device_get_trans_result failed");
            assert(0); // Crash!
        }
        if (rtrans == &bus->trans_data) {
            bus->is_trans_data_free = true;
        } else {
            if (rtrans >= bus->trans_area && rtrans <= &bus->trans_area[DGX_SPI_TRN_DCMD_IDX]) {
                bus->pending_transactions--;
            }
        }
    }
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
    }
    spi_transaction_t *ret_trans;
    ret = spi_device_get_trans_result(bus->spi, &ret_trans, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_command/spi_device_get_trans_result failed");
    }
    if (ret_trans != &bus->trans_sync) {
        ESP_LOGE(TAG, "dgx_spi_send_command/ret_trans != &bus->trans_sync failed");
    }
}

static void dgx_spi_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    esp_err_t      ret;
    if (!bus->is_trans_data_free) dgx_spi_sync_write(_bus);
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
    }
    spi_transaction_t *ret_trans;
    ret = spi_device_get_trans_result(bus->spi, &ret_trans, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_data/spi_device_get_trans_result failed");
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
    ret                       = spi_device_transmit(bus->spi, &bus->trans_sync);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_data failed");
    }
    return bus->trans_sync.rxlength;
}

static void dgx_spi_send_data_async(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    size_t         len_bytes;
    if (len == 0) {
        ESP_LOGE(TAG, "dgx_spi_send_data_async len == 0!");
        return;
    }
    if (!bus->is_trans_data_free) dgx_spi_wait_pending(_bus);
    memset(&bus->trans_data, 0, sizeof(spi_transaction_t));
    if (len <= 32) {
        len_bytes             = (len + 7u) / 8u;
        bus->trans_data.flags = SPI_TRANS_USE_TXDATA;
        memcpy(bus->trans_data.tx_data, data, len_bytes);
    } else {
        bus->trans_data.tx_buffer = data;
    }
    bus->trans_data.length = len;
    bus->trans_data.user   = set_dc_pin(bus->dcio, 1);
    if (len > 2048) {
        esp_err_t rc = spi_device_queue_trans(bus->spi, &bus->trans_data, portMAX_DELAY);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "spi_device_queue_trans/dgx_spi_send_data_async failed");
            assert(0);
        }
        bus->is_trans_data_free = false;
    } else {
        if (bus->pending_transactions > 0) dgx_spi_wait_pending(_bus);
        esp_err_t rc = spi_device_polling_transmit(bus->spi, &bus->trans_data);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "spi_device_polling_transmit/dgx_spi_send_data_async failed");
            assert(0);
        }
    }
}

static void dgx_spi_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    spi_bus_remove_device(bus->spi);
    spi_bus_free(bus->host_id);
    free(bus->protocols.buffer);
    heap_caps_free(bus); // for symmetry with heap_caps_calloc
}

static uint8_t *dgx_spi_memory_allocator(uint32_t len)
{
    return (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_DMA);
}

dgx_bus_protocols_t *dgx_spi_init(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk,
                                  gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t /* default = 0 */ cpolpha_mode)
{
    spi_bus_config_t buscfg;
    memset(&buscfg, 0, sizeof(buscfg));
    buscfg.miso_io_num     = miso;
    buscfg.mosi_io_num     = mosi;
    buscfg.sclk_io_num     = sclk;
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = -1; // 4092;
    if (spi_bus_initialize(host_id, &buscfg, dma_chan) != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_init: spi_bus_initialize failed");
        return NULL;
    }
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)heap_caps_calloc(1, sizeof(dgx_spi_bus_t), MALLOC_CAP_DMA);
    if (bus == 0) {
        ESP_LOGE(TAG, "dgx_spi_init failed to allocate dgx_spi_bus_t");
        spi_bus_free(host_id);
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
        spi_bus_free(host_id);
        free(bus);
        return NULL;
    }
    bus->host_id               = host_id;
    bus->dma_channel           = dma_chan;
    bus->dcio                  = dc;
    bus->csio                  = cs;
    bus->miso                  = miso;
    bus->mosi                  = mosi;
    bus->sclk                  = sclk;
    bus->clock_speed_hz        = clock_speed_hz;
    bus->max_transfer_sz       = buscfg.max_transfer_sz;
    bus->pending_transactions  = 0;
    bus->is_trans_data_free    = true;
    bus->devcfg.clock_speed_hz = clock_speed_hz;
    bus->devcfg.spics_io_num   = cs;
    bus->devcfg.queue_size     = DGX_SPI_TRN_DCMD_IDX + 3;
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
        ESP_LOGE(TAG, "dgx_spi_init spi_bus_add_device failed");
        spi_bus_free(host_id);
        free(bus);
        return NULL;
    }
    return (dgx_bus_protocols_t *)bus;
}
