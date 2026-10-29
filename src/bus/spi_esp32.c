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
#include "dgx_arch.h"
#include "dgx_spi_esp32_priv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "hal/gpio_ll.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "soc/gpio_reg.h"

/* a chip whose GDMA may ask DGX_SPI_PSRAM_ALIGN of PSRAM, see dgx_spi_psram_align() */
#if CONFIG_SPIRAM && SOC_PSRAM_DMA_CAPABLE && DGX_SPI_DMA_PSRAM && defined(SOC_AHB_GDMA_VERSION) && SOC_AHB_GDMA_VERSION != 1
#define DGX_SPI_PSRAM_MAY_ALIGN 1
#include "hal/efuse_hal.h"
#if CONFIG_SPIRAM_ECC_ENABLE
#include "esp_idf_version.h"
#endif
#else
#define DGX_SPI_PSRAM_MAY_ALIGN 0
#endif

static const char TAG[] = "DGX SPI BUS";

/*
 * Every piece of pixels costs the same time to start however long it is,
 * DGX_SPI_PIECE_COST_US as measured on an ESP32-S3 with IDF 6.1, so a bus of
 * four data lines, or a faster clock, needs longer pieces to lose as little.
 * A virtual screen goes in halves of the buffer of a display, and a half is
 * as long as the wire takes to send in the time that makes that cost
 * DGX_SPI_LOSS_PERCENT of the whole: cost * (100 - loss) / loss, about
 * 390 us. The buffer is DGX_SPI_MIN_BUFFER at least and no longer than
 * DGX_SPI_MAX_TRANSFER, which is what a host DGX initializes takes and less
 * than the 32768 bytes of one transfer of an ESP32-S3.
 */
#define DGX_SPI_PIECE_COST_US 25u
#define DGX_SPI_LOSS_PERCENT  6u
#define DGX_SPI_MIN_BUFFER    4000u
#define DGX_SPI_MAX_TRANSFER  32000u

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
        rtrans        = NULL;
        esp_err_t ret = spi_device_get_trans_result(bus->spi, &rtrans, portMAX_DELAY);
        dgx_spi_check_result(ret, rtrans, &bus->psram_late, "dgx_spi_wait_pending:spi_device_get_trans_result");
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
#if DGX_SPI_DMA_PSRAM
        if (!bus->psram_late && esp_ptr_external_ram(data) && dgx_spi_dma_reaches(bus->host_id, data) && len % (8u * dgx_spi_psram_align()) == 0)
            trans->flags = SPI_TRANS_DMA_USE_PSRAM;
#endif
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
static void dgx_spi_send_piece(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    if (len <= 2048) {
        if (bus->data_queued) dgx_spi_wait_pending(&bus->protocols);
        spi_transaction_t *trans = dgx_spi_data_trans(bus, data, len);
        dgx_spi_check_result(spi_device_polling_transmit(bus->spi, trans), trans, &bus->psram_late, "spi_device_polling_transmit/dgx_spi_send_data_async");
        return;
    }
    esp_err_t rc = spi_device_queue_trans(bus->spi, dgx_spi_data_trans(bus, data, len), portMAX_DELAY);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_queue_trans/dgx_spi_send_data_async failed: %s", esp_err_to_name(rc));
        assert(0);
        /*
         * With assertions off these pixels are lost: nothing was queued, so no
         * result is waited for, and its descriptor is free again. The pieces
         * before it are waited for, as when it goes: a caller that fills two
         * halves of a buffer in turn writes the other half next.
         */
        bus->data_slot ^= 1u;
        dgx_spi_wait_pending(&bus->protocols);
        return;
    }
    /* results come in the order the transfers were queued: the first ones are those before this one */
    for (++bus->data_queued; bus->data_queued > 1; --bus->data_queued) {
        spi_transaction_t *rtrans = NULL;
        dgx_spi_check_result(spi_device_get_trans_result(bus->spi, &rtrans, portMAX_DELAY), rtrans, &bus->psram_late,
                             "dgx_spi_send_data_async:spi_device_get_trans_result");
    }
}

void dgx_spi_send_pieces(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len, dgx_spi_piece_func send_piece)
{
    /* a piece no longer than the buffer goes whole, the buffer itself above all */
    if (len <= bus->buffer_len * 8u) {
        send_piece(bus, data, len);
        return;
    }
    /* from PSRAM whose DMA asks 16 bytes, in multiples of 48 where the buffer is that long: every piece then starts and ends on 16 bytes if the first starts there */
    const uint32_t step  = bus->buffer_len >= 48u && esp_ptr_external_ram(data) && dgx_spi_psram_align() > 1u ? 48u : 12u;
    uint32_t       piece = (uint32_t)(bus->buffer_len - bus->buffer_len % step);
    if (!piece) piece = 12u; /* a buffer shorter than 12 bytes still sends pieces, of 12 bytes, which any host takes */
    while (len > piece * 8u) {
        send_piece(bus, data, piece * 8u);
        data += piece;
        len -= piece * 8u;
    }
    send_piece(bus, data, len);
}

/* pixels, in pieces no longer than the buffer of the bus, see dgx_spi_send_pieces() */
static void dgx_spi_send_data_async(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    if (len == 0) {
        ESP_LOGE(TAG, "dgx_spi_send_data_async len == 0!");
        return;
    }
    dgx_spi_send_pieces(_bus, data, len, dgx_spi_send_piece);
}

/*
 * The SPI hosts DGX initializes, shared by the DGX displays on a host and freed
 * with the last of them, those of dgx_qspi_init() too. A host the program
 * initialized and named to dgx_spi_init_on_bus() or dgx_qspi_init_on_bus() is
 * the program's: DGX only adds its device to it.
 */
typedef struct {
    uint16_t       users;
    bool           taken;
    gpio_num_t     mosi, miso, sclk, quadwp, quadhd;
    spi_dma_chan_t dma_chan;
} dgx_spi_host_t;

static dgx_spi_host_t dgx_spi_hosts[SOC_SPI_PERIPH_NUM];

esp_err_t dgx_spi_take_host(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk, gpio_num_t quadwp,
                            gpio_num_t quadhd)
{
    if (host_id < 0 || host_id >= SOC_SPI_PERIPH_NUM) {
        ESP_LOGE(TAG, "SPI host %d does not exist", (int)host_id);
        return ESP_ERR_INVALID_ARG;
    }
    dgx_spi_host_t *h = &dgx_spi_hosts[host_id];
    if (h->taken) {
        /* a display of one data line goes on a host of four as well, not the other way round */
        const bool quad_differs = quadwp != GPIO_NUM_NC && (h->quadwp != quadwp || h->quadhd != quadhd);
        if (h->mosi != mosi || h->miso != miso || h->sclk != sclk || h->dma_chan != dma_chan || quad_differs) {
            ESP_LOGE(TAG,
                     "SPI host %d already uses MOSI %d, MISO %d, SCLK %d, WP %d, HD %d and DMA %d, not MOSI %d, MISO %d, SCLK %d, WP %d, HD %d and DMA %d",
                     (int)host_id, (int)h->mosi, (int)h->miso, (int)h->sclk, (int)h->quadwp, (int)h->quadhd, (int)h->dma_chan, (int)mosi, (int)miso,
                     (int)sclk, (int)quadwp, (int)quadhd, (int)dma_chan);
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }
    spi_bus_config_t buscfg;
    memset(&buscfg, 0, sizeof(buscfg));
    buscfg.miso_io_num     = miso;
    buscfg.mosi_io_num     = mosi;
    buscfg.sclk_io_num     = sclk;
    buscfg.quadwp_io_num   = quadwp;
    buscfg.quadhd_io_num   = quadhd;
    /* not 0: ESP-IDF 6.0 and 6.1 take a quad bus for an octal one in the GPIO matrix and route data4 to data7 to GPIO 0 */
    buscfg.data4_io_num = buscfg.data5_io_num = buscfg.data6_io_num = buscfg.data7_io_num = -1;
    buscfg.max_transfer_sz = DGX_SPI_MAX_TRANSFER; // only the DMA descriptors depend on it, a few for 32000 bytes
    if (quadwp != GPIO_NUM_NC) buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    esp_err_t rc = spi_bus_initialize(host_id, &buscfg, dma_chan);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "SPI host %d: spi_bus_initialize failed (%s); a host the program has initialized itself goes to dgx_spi_init_on_bus()",
                 (int)host_id, esp_err_to_name(rc));
        return rc;
    }
    *h = (dgx_spi_host_t){.taken = true, .mosi = mosi, .miso = miso, .sclk = sclk, .quadwp = quadwp, .quadhd = quadhd, .dma_chan = dma_chan};
    return ESP_OK;
}

void dgx_spi_release_host(spi_host_device_t host_id)
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

size_t dgx_spi_buffer_len(spi_host_device_t host_id, spi_device_handle_t dev, unsigned data_lines, size_t most)
{
    int khz = 0;
    if (spi_device_get_actual_freq(dev, &khz) != ESP_OK) khz = 0;
    /* the time of a half in microseconds, and what the wire sends in two of them: a line sends khz / 1000 bits a microsecond */
    const uint64_t half_us = (uint64_t)DGX_SPI_PIECE_COST_US * (100u - DGX_SPI_LOSS_PERCENT) / DGX_SPI_LOSS_PERCENT;
    uint64_t       bits    = (uint64_t)khz * data_lines * 2u * half_us / 1000u;
    uint64_t       len     = bits / 8u;
    if (len > DGX_SPI_MAX_TRANSFER) len = DGX_SPI_MAX_TRANSFER;
    size_t host_most = 0;
    if (spi_bus_get_max_transaction_len(host_id, &host_most) == ESP_OK && host_most && len > host_most) len = host_most;
    if (len < DGX_SPI_MIN_BUFFER) len = DGX_SPI_MIN_BUFFER;
    /* the program's limit is the most, below the least DGX takes too; the length goes down to a multiple of 16
     * bytes, or of a word under 16, one word at least. The SPI driver asks nothing of a buffer it sends from
     * internal memory (alignment 1 for TX in IDF 5.5 and 6.1); 16 bytes is what DMA from PSRAM asks under flash
     * encryption or PSRAM ECC, and costs at most 12 bytes. */
    if (most && len > most) len = most;
    const uint64_t words = len & ~(uint64_t)3;
    len &= ~(uint64_t)15;
    if (!len) len = words ? words : 4u;
    return (size_t)len;
}

void dgx_spi_check_result(esp_err_t rc, const spi_transaction_t *trans, bool *psram_late, const char *what)
{
    if (rc == ESP_OK) return;
#if DGX_SPI_DMA_PSRAM
    if (rc == ESP_ERR_INVALID_STATE && trans && (trans->flags & SPI_TRANS_DMA_TX_FAIL)) {
        if (!*psram_late) ESP_LOGE(TAG, "pixels from PSRAM came too late for the DMA at this clock; they go through internal memory from now on");
        *psram_late = true;
        return;
    }
#else
    (void)trans, (void)psram_late;
#endif
    ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(rc));
    assert(0);
}

bool dgx_spi_psram_too_slow(spi_device_handle_t dev, unsigned data_lines)
{
    int khz = 0;
    if (spi_device_get_actual_freq(dev, &khz) != ESP_OK || khz <= 0) return true;
    return (uint64_t)khz * data_lines > DGX_SPI_PSRAM_DMA_KBITS;
}

bool dgx_spi_dma_reaches(spi_host_device_t host_id, const void *pixels)
{
    (void)host_id;
    if (esp_ptr_external_ram(pixels)) {
#if CONFIG_SPIRAM && SOC_PSRAM_DMA_CAPABLE && DGX_SPI_DMA_PSRAM
#if CONFIG_IDF_TARGET_ESP32S2
        if (host_id == SPI3_HOST) return false;
#endif
        return esp_ptr_dma_ext_capable(pixels) && (uintptr_t)pixels % dgx_spi_psram_align() == 0;
#else
        return false;
#endif
    }
    return esp_ptr_dma_capable(pixels) && ((uintptr_t)pixels & 3u) == 0;
}

unsigned dgx_spi_psram_align(void)
{
#if DGX_SPI_PSRAM_MAY_ALIGN && CONFIG_SPIRAM_ECC_ENABLE && ESP_IDF_VERSION_MAJOR >= 6
    return DGX_SPI_PSRAM_ALIGN;
#elif DGX_SPI_PSRAM_MAY_ALIGN
    return efuse_hal_flash_encryption_enabled() ? DGX_SPI_PSRAM_ALIGN : 1u;
#else
    return 1u;
#endif
}

static bool dgx_spi_sends_from(struct _dgx_bus_protocols_t *_bus, const void *pixels)
{
    const dgx_spi_bus_t *bus = (const dgx_spi_bus_t *)_bus;
    return !(bus->psram_late && esp_ptr_external_ram(pixels)) && dgx_spi_dma_reaches(bus->host_id, pixels);
}

void dgx_spi_host_join(spi_host_device_t host_id)
{
    ++dgx_spi_hosts[host_id].users;
}

void dgx_spi_host_leave(spi_host_device_t host_id)
{
    if (!dgx_spi_hosts[host_id].users) return;
    --dgx_spi_hosts[host_id].users;
    dgx_spi_release_host(host_id);
}

static void dgx_spi_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    dgx_spi_wait_pending(_bus); // the device cannot be removed with transactions in flight
    spi_bus_remove_device(bus->spi);
    if (bus->owns_host) dgx_spi_host_leave(bus->host_id);
    free(bus->protocols.buffer);
    heap_caps_free(bus); // for symmetry with heap_caps_calloc
}

static uint8_t *dgx_spi_memory_allocator(uint32_t len)
{
    return (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_DMA);
}

/* the display's device added to an initialized host: the part both constructors share */
static dgx_bus_protocols_t *dgx_spi_attach(spi_host_device_t host_id, bool owns_host, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso,
                                           gpio_num_t sclk, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode,
                                           const dgx_spi_options_t *options)
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
    bus->protocols.sends_from       = dgx_spi_sends_from;
    bus->protocols.dispose          = dgx_spi_dispose_bus_func;
    bus->protocols.memory_allocator = dgx_spi_memory_allocator;
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
    /*
     * No dummy cycles to make up for the delay of MISO through the GPIO
     * matrix: DGX does not read the display back, and an ESP32 refuses a
     * full-duplex device that would need them, one faster than 26.7 MHz on
     * pins of the GPIO matrix (spi_hal_cal_clock_conf() of IDF). read_data()
     * at such a clock may take wrong bits.
     */
    bus->devcfg.flags = SPI_DEVICE_NO_DUMMY;
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
        heap_caps_free(bus);
        return NULL;
    }
    /* the clock the device has really got decides the length of the buffer, and whether DMA from PSRAM keeps up */
    bus->protocols.buffer_len = dgx_spi_buffer_len(host_id, bus->spi, 1, options ? options->buffer_max : 0);
    bus->psram_late           = dgx_spi_psram_too_slow(bus->spi, 1);
    bus->protocols.buffer     = bus->protocols.memory_allocator(bus->protocols.buffer_len);
    if (!bus->protocols.buffer) {
        ESP_LOGE(TAG, "dgx_spi_init buffer allocation failed");
        spi_bus_remove_device(bus->spi);
        heap_caps_free(bus);
        return NULL;
    }
    return (dgx_bus_protocols_t *)bus;
}

dgx_bus_protocols_t *dgx_spi_init_with(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk,
                                       gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode, const dgx_spi_options_t *options)
{
    if (dgx_spi_take_host(host_id, dma_chan, mosi, miso, sclk, GPIO_NUM_NC, GPIO_NUM_NC) != ESP_OK) return NULL;
    dgx_bus_protocols_t *bus = dgx_spi_attach(host_id, true, dma_chan, mosi, miso, sclk, cs, dc, clock_speed_hz, cpolpha_mode, options);
    if (!bus) {
        dgx_spi_release_host(host_id);
        return NULL;
    }
    dgx_spi_host_join(host_id);
    return bus;
}

dgx_bus_protocols_t *dgx_spi_init(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk,
                                  gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t /* default = 0 */ cpolpha_mode)
{
    return dgx_spi_init_with(host_id, dma_chan, mosi, miso, sclk, cs, dc, clock_speed_hz, cpolpha_mode, NULL);
}

dgx_bus_protocols_t *dgx_spi_init_on_bus_with(spi_host_device_t host_id, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode,
                                              const dgx_spi_options_t *options)
{
    if (host_id < 0 || host_id >= SOC_SPI_PERIPH_NUM) {
        ESP_LOGE(TAG, "SPI host %d does not exist", (int)host_id);
        return NULL;
    }
    return dgx_spi_attach(host_id, false, SPI_DMA_CH_AUTO, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, cs, dc, clock_speed_hz, cpolpha_mode, options);
}

dgx_bus_protocols_t *dgx_spi_init_on_bus(spi_host_device_t host_id, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode)
{
    return dgx_spi_init_on_bus_with(host_id, cs, dc, clock_speed_hz, cpolpha_mode, NULL);
}
