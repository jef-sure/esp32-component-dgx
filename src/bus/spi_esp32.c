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
#include "hal/spi_ll.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "soc/gpio_reg.h"

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
 * 475 us. The buffer is DGX_SPI_MIN_BUFFER at least and no longer than
 * DGX_SPI_MAX_TRANSFER, which is what a host DGX initializes takes and less
 * than the 32768 bytes of one transfer of an ESP32-S3.
 */
#define DGX_SPI_PIECE_COST_US 25u
#define DGX_SPI_LOSS_PERCENT  5u
#define DGX_SPI_MIN_BUFFER    4000u
#define DGX_SPI_MAX_TRANSFER  32000u

/* transfers the device may hold at once; two of pixels are queued at most, one behind the other, the number is the one the bus has always been made with */
#define DGX_SPI_QUEUE_SIZE 7

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

/*
 * The fastest wire DMA from PSRAM was seen to keep up with: 26.7 MHz on four
 * lines, on an ESP32-S3 with octal PSRAM at 80 MHz, also while the other
 * core wrote PSRAM all the time. At 40 MHz on four lines it fell behind on
 * the first frame with nothing else running. A device whose wire is faster
 * starts with pixels from PSRAM going through a copy.
 */
#define DGX_SPI_PSRAM_DMA_KBITS 106667u

typedef struct _dgx_spi_bus_t
{
    dgx_spi_dev_t    dev;
    gpio_num_t       dcio;   /* the D/C line of the display */
    transaction_cb_t set_dc; /* what sets it before a transfer: dgx_spi_set_dc(), dgx_spi_set_dc_high() or, without one, dgx_spi_no_dc() */
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

/* below, with the hosts */
static bool     dgx_spi_keeps(dgx_spi_dev_t *dev);
static void     dgx_spi_let_go(dgx_spi_dev_t *dev);
static unsigned dgx_spi_psram_align(void);
static bool     dgx_spi_dma_reaches(spi_host_device_t host_id, const void *pixels);
static void     dgx_spi_check_result(dgx_spi_dev_t *dev, esp_err_t rc, const spi_transaction_t *trans, const char *what);

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

/* a display without a D/C line: for the transfers by the registers, which call the callback the driver is given, and it has none */
static void dgx_spi_no_dc(spi_transaction_t *t)
{
    (void)t;
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

/*
 * Short transfers by the registers of the host, while the display holds the
 * bus: from hold(), which a window calls, to release() at the end of its
 * drawing. On the CYD (an ESP32 at 240 MHz, IDF 5.5.4) a polled transfer of
 * one byte takes the driver 12.3 us, though the wire takes 0.2 us at 40 MHz,
 * and a window with its pixel is four transfers; by the registers a byte
 * takes 1 us and the four 4.7 us. The data goes into the buffer of the host,
 * DGX_SPI_DIRECT_BYTES at most a transfer, and the transfer is started there.
 * spi_device_acquire_bus() sets the host for this device; what a transfer of
 * the driver sets for itself, the lines, the phases and DMA, is set back
 * before the first transfer here after one. The driver waits for none of
 * these: one may still be under way when a call returns, and is waited for
 * before the next transfer, the next of the driver and the end of the hold.
 * The host marks every transfer done in the flag it starts its interrupt
 * by, as after a transfer of the driver, and the flag is left set.
 */
#define DGX_SPI_DIRECT_BYTES SOC_SPI_MAXIMUM_BUFFER_SIZE

static inline void dgx_spi_direct_wait(dgx_spi_dev_t *dev)
{
    while (spi_ll_get_running_cmd((spi_dev_t *)dev->hw)) {
    }
}

void dgx_spi_dev_direct_end(dgx_spi_dev_t *dev)
{
    if (dev->direct) {
        dgx_spi_direct_wait(dev);
        dev->direct = false;
    }
}

void *dgx_spi_dev_direct_begin(dgx_spi_dev_t *dev)
{
    spi_dev_t *hw = (spi_dev_t *)dev->hw;
    dgx_spi_direct_wait(dev);
    if (!dev->direct) {
        dgx_spi_dev_wait(dev); /* nothing of the driver is on its way */
        const spi_line_mode_t one_line = {.cmd_lines = 1, .addr_lines = 1, .data_lines = 1};
        spi_ll_master_set_line_mode(hw, one_line);
        spi_ll_set_command_bitlen(hw, 0);
        spi_ll_set_addr_bitlen(hw, 0);
        spi_ll_set_dummy(hw, 0);
        spi_ll_enable_miso(hw, 0);
        spi_ll_enable_mosi(hw, 1);
        spi_ll_dma_tx_enable(hw, false);
        dev->direct = true;
    }
    return (void *)hw; /* volatile on some chips: the caller takes it back as spi_dev_t */
}

void dgx_spi_dev_direct_start(dgx_spi_dev_t *dev, const uint8_t *bytes, uint32_t len)
{
    spi_dev_t *hw = (spi_dev_t *)dev->hw;
    /* the host takes whole words: the bytes go through a copy that has them */
    uint32_t words[DGX_SPI_DIRECT_BYTES / 4];
    memcpy(words, bytes, (len + 7u) / 8u);
    spi_ll_set_mosi_bitlen(hw, len);
    spi_ll_write_buffer(hw, (const uint8_t *)words, len);
    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);
}

static void dgx_spi_direct_send(dgx_spi_bus_t *bus, bool is_data, const uint8_t *bytes, uint32_t len)
{
    dgx_spi_dev_direct_begin(&bus->dev);
    /* the D/C line set by the callback the driver calls before its own transfers, which reads only `user` */
    spi_transaction_t dc;
    dc.user = set_dc_pin(bus->dcio, is_data);
    bus->set_dc(&dc);
    dgx_spi_dev_direct_start(&bus->dev, bytes, len);
}

void dgx_spi_dev_wait_queued(dgx_spi_dev_t *dev)
{
    /* the only transfers left on their way when a call returns are those of pixels, in trans_data */
    while (dev->data_queued) {
        /* the result first: rtrans is the driver's only after the call, and arguments are taken in no set order */
        spi_transaction_t *rtrans = NULL;
        const esp_err_t    rc     = spi_device_get_trans_result(dev->spi, &rtrans, portMAX_DELAY);
        dgx_spi_check_result(dev, rc, rtrans, "spi_device_get_trans_result of pixels");
        --dev->data_queued;
    }
}

void dgx_spi_dev_sync(dgx_spi_dev_t *dev)
{
    dgx_spi_dev_wait(dev);
    if (dev->direct) dgx_spi_direct_wait(dev);
}

static void dgx_spi_sync_write(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_dev_sync((dgx_spi_dev_t *)_bus);
}

/*
 * The bus acquired for the display, after its pixels on their way: see
 * dgx_spi_direct_send(). The only display on a host DGX has set up keeps
 * it between its drawings, and acquires it once: dgx_spi_init() says that
 * nothing else is on that host. On a host of the program, or with other
 * displays of DGX on the host, the bus is let go of at the end of every
 * drawing. Acquiring it costs 6.9 us on the CYD.
 */
void dgx_spi_dev_hold(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_dev_t *dev = (dgx_spi_dev_t *)_bus;
    if (dev->held) return;
    dgx_spi_dev_wait(dev);
    const esp_err_t rc = spi_device_acquire_bus(dev->spi, portMAX_DELAY);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_acquire_bus failed: %s", esp_err_to_name(rc));
        return;
    }
    dev->held   = true;
    dev->direct = false;
    dgx_spi_keeps(dev);
}

/* the end of a drawing: the bus let go of for the other devices on the host, kept where there are none */
void dgx_spi_dev_release(struct _dgx_bus_protocols_t *_bus)
{
    dgx_spi_dev_t *dev = (dgx_spi_dev_t *)_bus;
    if (dev->held && !dgx_spi_keeps(dev)) dgx_spi_let_go(dev);
}

void dgx_spi_dev_to_queue(dgx_spi_dev_t *dev)
{
    dgx_spi_dev_direct_end(dev);
    if (dev->held && !dgx_spi_keeps(dev)) dgx_spi_let_go(dev);
}

/*
 * A command or parameters of 4 bytes at most, polled: that takes a fraction
 * of the time a queued, interrupt-driven transfer needs, and the window of a
 * single pixel or a short line is mostly such transfers. The pixels on their
 * way have gone before. Inlined: a call more for each of the five transfers
 * of a window made a window 1.5 us slower on the CYD.
 */
static inline __attribute__((always_inline)) void dgx_spi_send_short(dgx_spi_bus_t *bus, bool is_data, const uint8_t *bytes, uint32_t len)
{
    if (bus->dev.held) {
        dgx_spi_direct_send(bus, is_data, bytes, len);
        return;
    }
    spi_transaction_t *trans = &bus->dev.trans_sync;
    memset(trans, 0, sizeof(*trans));
    trans->length = len;
    trans->flags  = SPI_TRANS_USE_TXDATA;
    memcpy(trans->tx_data, bytes, (len + 7u) / 8u);
    trans->user   = set_dc_pin(bus->dcio, is_data);
    esp_err_t ret = spi_device_polling_transmit(bus->dev.spi, trans);
    if (ret != ESP_OK) ESP_LOGE(TAG, "%s of %u bits failed: %s", is_data ? "parameters" : "a command", (unsigned)len, esp_err_to_name(ret));
}

static void dgx_spi_send_command(struct _dgx_bus_protocols_t *_bus, uint8_t cmd)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    dgx_spi_dev_wait(&bus->dev);
    dgx_spi_send_short(bus, false, &cmd, 8);
}

static void dgx_spi_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    dgx_spi_dev_direct_end(&bus->dev);
    dgx_spi_dev_wait(&bus->dev);
    spi_transaction_t *trans = &bus->dev.trans_sync;
    memset(trans, 0, sizeof(*trans));
    trans->length    = len;
    trans->tx_buffer = cmds;
    trans->user      = set_dc_pin(bus->dcio, 0); // D/C needs to be set to 0
    if (spi_device_transmit(bus->dev.spi, trans) != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_commands failed");
    }
}

static void dgx_spi_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    if (len == 0) return;
    dgx_spi_dev_wait(&bus->dev);
    if (len <= 32) {
        dgx_spi_send_short(bus, true, data, len);
        return;
    }
    dgx_spi_dev_direct_end(&bus->dev);
    spi_transaction_t *trans = &bus->dev.trans_sync;
    memset(trans, 0, sizeof(*trans));
    trans->tx_buffer = data;
    trans->length    = len;                      // Data length in bits
    trans->user      = set_dc_pin(bus->dcio, 1); // DC = 1 when data
    if (spi_device_transmit(bus->dev.spi, trans) != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_send_data failed");
    }
}

static uint32_t dgx_spi_read_data(struct _dgx_bus_protocols_t *_bus, uint8_t *data, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    if (len == 0) return len;
    dgx_spi_dev_direct_end(&bus->dev);
    dgx_spi_dev_wait(&bus->dev);
    spi_transaction_t *trans = &bus->dev.trans_sync;
    memset(trans, 0, sizeof(*trans));
    trans->rx_buffer = data;
    trans->length    = len;                      // Data length in bits
    trans->user      = set_dc_pin(bus->dcio, 1); // DC = 1 when data
    if (spi_device_transmit(bus->dev.spi, trans) != ESP_OK) {
        ESP_LOGE(TAG, "dgx_spi_read_data failed");
        return 0;
    }
    return trans->rxlength;
}

/* the next of trans_data made a transfer of pixels with the head of its bus; the transfers before it no longer use it */
static spi_transaction_t *dgx_spi_dev_trans(dgx_spi_dev_t *dev, const uint8_t *data, uint32_t len, const dgx_spi_head_t *head)
{
    spi_transaction_t *trans = &dev->trans_data[dev->data_slot];
    dev->data_slot ^= 1u;
    memset(trans, 0, sizeof(spi_transaction_t));
    trans->flags = head->flags;
    if (len <= 32) {
        trans->flags |= SPI_TRANS_USE_TXDATA;
        memcpy(trans->tx_data, data, (len + 7u) / 8u);
    } else {
        trans->tx_buffer = data;
#if DGX_SPI_DMA_PSRAM
        if (!dev->psram_late && esp_ptr_external_ram(data) && dgx_spi_dma_reaches(dev->host_id, data) && len % (8u * dgx_spi_psram_align()) == 0)
            trans->flags |= SPI_TRANS_DMA_USE_PSRAM;
#endif
    }
    trans->cmd    = head->cmd;
    trans->addr   = head->addr;
    trans->user   = head->user;
    trans->length = len;
    return trans;
}

bool dgx_spi_dev_send_piece(dgx_spi_dev_t *dev, const uint8_t *data, uint32_t len, const dgx_spi_head_t *head)
{
    if (len <= 2048) {
        dgx_spi_dev_wait(dev);
        spi_transaction_t *trans = dgx_spi_dev_trans(dev, data, len, head);
        dgx_spi_check_result(dev, spi_device_polling_transmit(dev->spi, trans), trans, "spi_device_polling_transmit of pixels");
        return true;
    }
    esp_err_t rc = spi_device_queue_trans(dev->spi, dgx_spi_dev_trans(dev, data, len, head), portMAX_DELAY);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "%s: spi_device_queue_trans of pixels failed: %s", dev->protocols.bus_name, esp_err_to_name(rc));
        assert(0);
        /*
         * With assertions off these pixels are lost: nothing was queued, so no
         * result is waited for, and its descriptor is free again. The pieces
         * before it are waited for, as when it goes: a caller that fills two
         * halves of a buffer in turn writes the other half next.
         */
        dev->data_slot ^= 1u;
        dgx_spi_dev_wait(dev);
        return false;
    }
    /* results come in the order the transfers were queued: the first ones are those before this one */
    for (++dev->data_queued; dev->data_queued > 1; --dev->data_queued) {
        spi_transaction_t *rtrans = NULL;
        const esp_err_t    rc     = spi_device_get_trans_result(dev->spi, &rtrans, portMAX_DELAY);
        dgx_spi_check_result(dev, rc, rtrans, "spi_device_get_trans_result of pixels");
    }
    return true;
}

/*
 * Pixels after the D/C line set for data. Held, a piece that is polled
 * anyway goes by the registers, in transfers of DGX_SPI_DIRECT_PIECE; a
 * longer one is queued for DMA as before, so that the processor fills what
 * comes next while it goes: see dgx_spi_dev_to_queue().
 */
static void dgx_spi_send_piece(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)_bus;
    if (bus->dev.held) {
        if (len <= 2048) {
            for (; len > DGX_SPI_DIRECT_PIECE * 8u; data += DGX_SPI_DIRECT_PIECE, len -= DGX_SPI_DIRECT_PIECE * 8u)
                dgx_spi_direct_send(bus, true, data, DGX_SPI_DIRECT_PIECE * 8u);
            dgx_spi_direct_send(bus, true, data, len);
            return;
        }
        dgx_spi_dev_to_queue(&bus->dev);
    }
    const dgx_spi_head_t head = {.user = set_dc_pin(bus->dcio, 1)};
    dgx_spi_dev_send_piece(&bus->dev, data, len, &head);
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
    dgx_spi_dev_t *keeper; /* the display that keeps the bus between its drawings, the only one on the host */
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

/*
 * The length of the buffer of a display on a host, once its device is on it:
 * half of it takes about 475 us on the wire at the clock the device has got
 * over `data_lines`, it is 4000 bytes at least, 32000 at most and no more
 * than the host takes in one transfer. A host with DMA takes 4092 bytes at
 * least: the driver rounds max_transfer_sz up to whole DMA descriptors of
 * 4092 bytes. `most`, when it is not 0, is the program's limit: no longer
 * than it, in whole words, and 4 bytes at least.
 */
static size_t dgx_spi_buffer_len(spi_host_device_t host_id, spi_device_handle_t dev, unsigned data_lines, size_t most)
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

/*
 * The result of a transfer of pixels. One from PSRAM that the DMA fell
 * behind on has ended with wrong pixels (ESP_ERR_INVALID_STATE with
 * SPI_TRANS_DMA_TX_FAIL): PSRAM does not keep up with the bus at its clock,
 * so psram_late is set and pixels in PSRAM go through internal memory from
 * then on, as the driver copies them without SPI_TRANS_DMA_USE_PSRAM. The
 * log tells of it once. Any other failure stops the program, as before. An
 * IDF without those flags sends nothing from PSRAM by DMA, and has no such
 * failure.
 */
static void dgx_spi_check_result(dgx_spi_dev_t *dev, esp_err_t rc, const spi_transaction_t *trans, const char *what)
{
    if (rc == ESP_OK) return;
#if DGX_SPI_DMA_PSRAM
    if (rc == ESP_ERR_INVALID_STATE && trans && (trans->flags & SPI_TRANS_DMA_TX_FAIL)) {
        if (!dev->psram_late)
            ESP_LOGE(TAG, "%s: pixels from PSRAM came too late for the DMA at this clock; they go through internal memory from now on",
                     dev->protocols.bus_name);
        dev->psram_late = true;
        return;
    }
#else
    (void)trans;
#endif
    ESP_LOGE(TAG, "%s: %s failed: %s", dev->protocols.bus_name, what, esp_err_to_name(rc));
    assert(0);
}

/* the wire of the device is faster than DMA from PSRAM keeps up with */
static bool dgx_spi_psram_too_slow(spi_device_handle_t dev, unsigned data_lines)
{
    int khz = 0;
    if (spi_device_get_actual_freq(dev, &khz) != ESP_OK || khz <= 0) return true;
    return (uint64_t)khz * data_lines > DGX_SPI_PSRAM_DMA_KBITS;
}

/*
 * The DMA of the host takes pixels from there as they lie: internal memory it
 * reaches, from a whole word, or PSRAM on a chip whose DMA reaches PSRAM (not
 * from SPI3 of an ESP32-S2), from dgx_spi_psram_align(), with an IDF that has
 * SPI_TRANS_DMA_USE_PSRAM. Pixels in PSRAM go with that flag, or the driver
 * copies them into internal memory first.
 */
static bool dgx_spi_dma_reaches(spi_host_device_t host_id, const void *pixels)
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

static unsigned dgx_spi_psram_align(void)
{
#if DGX_SPI_PSRAM_MAY_ALIGN && CONFIG_SPIRAM_ECC_ENABLE && ESP_IDF_VERSION_MAJOR >= 6
    return DGX_SPI_PSRAM_ALIGN;
#elif DGX_SPI_PSRAM_MAY_ALIGN
    return efuse_hal_flash_encryption_enabled() ? DGX_SPI_PSRAM_ALIGN : 1u;
#else
    return 1u;
#endif
}

bool dgx_spi_dev_sends_from(struct _dgx_bus_protocols_t *bus, const void *pixels)
{
    const dgx_spi_dev_t *dev = (const dgx_spi_dev_t *)bus;
    return !(dev->psram_late && esp_ptr_external_ram(pixels)) && dgx_spi_dma_reaches(dev->host_id, pixels);
}

/* whether the display keeps the bus between its drawings: it is the only one on a host DGX has set up */
static bool dgx_spi_keeps(dgx_spi_dev_t *dev)
{
    dgx_spi_host_t *h = &dgx_spi_hosts[dev->host_id];
    if (!dev->owns_host || h->users != 1) return false;
    if (dev->held) h->keeper = dev;
    return true;
}

/* the bus let go of: nothing of the display is on its way then */
static void dgx_spi_let_go(dgx_spi_dev_t *dev)
{
    if (!dev->held) return;
    dgx_spi_dev_direct_end(dev);
    dgx_spi_dev_wait(dev);
    spi_device_release_bus(dev->spi);
    dev->held = false;
    if (dgx_spi_hosts[dev->host_id].keeper == dev) dgx_spi_hosts[dev->host_id].keeper = NULL;
}

/*
 * A display joins a host DGX has set up. One that has kept the bus lets go
 * of it now, or the new display would wait for it for ever; the two take
 * turns at every drawing from then on. A display that kept the bus may be
 * drawing in another task meanwhile: the displays of a host are made before
 * they draw, or in the task that draws on them.
 */
void dgx_spi_host_join(spi_host_device_t host_id)
{
    dgx_spi_host_t *h = &dgx_spi_hosts[host_id];
    ++h->users;
    if (h->keeper) dgx_spi_let_go(h->keeper);
}

static void dgx_spi_host_leave(spi_host_device_t host_id)
{
    if (!dgx_spi_hosts[host_id].users) return;
    --dgx_spi_hosts[host_id].users;
    dgx_spi_release_host(host_id);
}

void dgx_spi_dev_dispose(struct _dgx_bus_protocols_t *bus)
{
    dgx_spi_dev_t *dev = (dgx_spi_dev_t *)bus;
    dgx_spi_let_go(dev);
    dgx_spi_dev_wait(dev); // the device cannot be removed with transactions in flight
    spi_bus_remove_device(dev->spi);
    if (dev->owns_host) dgx_spi_host_leave(dev->host_id);
    free(dev->protocols.buffer);
    heap_caps_free(dev); // for symmetry with heap_caps_calloc
}

uint8_t *dgx_spi_dev_memory(uint32_t len)
{
    return (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_DMA);
}

dgx_bus_protocols_t *dgx_spi_dev_attach(dgx_spi_dev_t *dev, spi_host_device_t host_id, bool owns_host, const spi_device_interface_config_t *devcfg,
                                        unsigned data_lines, const dgx_spi_options_t *options)
{
    dev->host_id        = host_id;
    dev->owns_host      = owns_host;
    dev->hw             = (void *)SPI_LL_GET_HW(host_id);
    dev->protocols.wire = &dgx_spi_hosts[host_id];
    esp_err_t rc   = spi_bus_add_device(host_id, devcfg, &dev->spi);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "%s: spi_bus_add_device failed: %s", dev->protocols.bus_name, esp_err_to_name(rc));
        heap_caps_free(dev);
        return NULL;
    }
    /* the clock the device has really got decides the length of the buffer, and whether DMA from PSRAM keeps up */
    dev->protocols.buffer_len = dgx_spi_buffer_len(host_id, dev->spi, data_lines, options ? options->buffer_max : 0);
    dev->psram_late           = dgx_spi_psram_too_slow(dev->spi, data_lines);
    dev->protocols.buffer     = dev->protocols.memory_allocator(dev->protocols.buffer_len);
    if (!dev->protocols.buffer) {
        ESP_LOGE(TAG, "%s: buffer allocation failed", dev->protocols.bus_name);
        spi_bus_remove_device(dev->spi);
        heap_caps_free(dev);
        return NULL;
    }
    return &dev->protocols;
}

/* the display's device added to an initialized host: the part both constructors share */
static dgx_bus_protocols_t *dgx_spi_attach(spi_host_device_t host_id, bool owns_host, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode,
                                           const dgx_spi_options_t *options)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)heap_caps_calloc(1, sizeof(dgx_spi_bus_t), MALLOC_CAP_DMA);
    if (bus == 0) {
        ESP_LOGE(TAG, "dgx_spi_init failed to allocate dgx_spi_bus_t");
        return NULL;
    }
    dgx_bus_protocols_t *protocols = &bus->dev.protocols;
    protocols->bus_type            = DGX_BUS_SPI;
    protocols->bus_name            = "SPI";
    protocols->write_command       = dgx_spi_send_command;
    protocols->write_commands      = dgx_spi_send_commands;
    protocols->read_data           = dgx_spi_read_data;
    protocols->write_data          = dgx_spi_send_data;
    protocols->write_data_async    = dgx_spi_send_data_async;
    protocols->sync_write          = dgx_spi_sync_write;
    protocols->sends_from          = dgx_spi_dev_sends_from;
    protocols->dispose             = dgx_spi_dev_dispose;
    protocols->memory_allocator    = dgx_spi_dev_memory;
    protocols->hold                = dgx_spi_dev_hold;
    protocols->release             = dgx_spi_dev_release;
    bus->dcio                      = dc;
    bus->set_dc                    = dgx_spi_no_dc;
    spi_device_interface_config_t devcfg;
    memset(&devcfg, 0, sizeof(devcfg));
    devcfg.clock_speed_hz = clock_speed_hz;
    devcfg.spics_io_num   = cs;
    devcfg.queue_size     = DGX_SPI_QUEUE_SIZE;
    /*
     * No dummy cycles to make up for the delay of MISO through the GPIO
     * matrix: DGX does not read the display back, and an ESP32 refuses a
     * full-duplex device that would need them, one faster than 26.7 MHz on
     * pins of the GPIO matrix (spi_hal_cal_clock_conf() of IDF). read_data()
     * at such a clock may take wrong bits.
     */
    devcfg.flags = SPI_DEVICE_NO_DUMMY;
    if (dc >= 0) {
        dgx_gpio_set_direction(dc, GPIO_MODE_OUTPUT);
        if (dc > 31) {
            devcfg.pre_cb = dgx_spi_set_dc_high;
        } else {
            devcfg.pre_cb = dgx_spi_set_dc;
        }
        bus->set_dc = devcfg.pre_cb;
    }
    devcfg.mode = cpolpha_mode;
    return dgx_spi_dev_attach(&bus->dev, host_id, owns_host, &devcfg, 1, options);
}

dgx_bus_protocols_t *dgx_spi_init_with(spi_host_device_t host_id, spi_dma_chan_t dma_chan, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk,
                                       gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode, const dgx_spi_options_t *options)
{
    if (dgx_spi_take_host(host_id, dma_chan, mosi, miso, sclk, GPIO_NUM_NC, GPIO_NUM_NC) != ESP_OK) return NULL;
    dgx_bus_protocols_t *bus = dgx_spi_attach(host_id, true, cs, dc, clock_speed_hz, cpolpha_mode, options);
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
    return dgx_spi_attach(host_id, false, cs, dc, clock_speed_hz, cpolpha_mode, options);
}

dgx_bus_protocols_t *dgx_spi_init_on_bus(spi_host_device_t host_id, gpio_num_t cs, gpio_num_t dc, int clock_speed_hz, uint8_t cpolpha_mode)
{
    return dgx_spi_init_on_bus_with(host_id, cs, dc, clock_speed_hz, cpolpha_mode, NULL);
}
