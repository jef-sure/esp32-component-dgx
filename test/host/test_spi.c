/*
 * The SPI and QSPI buses, src/bus/spi_esp32.c and src/bus/qspi_esp32.c, with
 * a screen on them, against a model of the SPI master driver of IDF and of
 * the display behind it.
 *
 * The driver is modelled where DGX leans on it. A queued transfer goes out
 * when its result is taken, and its bytes are read only then, as the DMA
 * reads them on its way: a buffer written over before that is caught, and so
 * is a descriptor changed while it is queued. A polled transfer goes out at
 * once, and only when nothing of its device is queued. A host with DMA takes
 * whole descriptors of 4092 bytes, and a transfer longer than the host takes
 * is refused, as the driver refuses it. A transfer from PSRAM with
 * SPI_TRANS_DMA_USE_PSRAM is counted as sent where it lies, from any byte and
 * of any length, as on an ESP32-S3; test_spi_p4 stands for a P4, whose
 * driver under flash encryption copies one that starts or ends off 16 bytes
 * first; a transfer from PSRAM the DMA falls behind on ends with wrong pixels,
 * ESP_ERR_INVALID_STATE and SPI_TRANS_DMA_TX_FAIL, as at too fast a clock.
 * As on an ESP32 whose SPI pins go through the GPIO matrix, a full-duplex
 * device faster than 26.7 MHz that may add dummy cycles to reads is refused.
 *
 * The SPI display is a controller of the MIPI display command set behind a
 * D/C line, which the callback of the bus sets through the GPIO registers
 * before every transfer. The QSPI display takes opcode 0x02 for a command
 * with its parameters and 0x32 for pixels, the command in the middle byte of
 * the address: 0x2C writes from the start of the window, 0x3C goes on where
 * the pixels before ended. The commands of a window and their parameters,
 * four bytes at most, are polled on both buses: none of them is queued.
 *
 * The SPI display holds the bus from a window to the end of its drawing, and
 * meanwhile sends short transfers by the registers of the host. The model
 * holds another device of the host off while the bus is held, as the driver
 * would hold it off for ever; a transfer by the registers starts only on a
 * host set for it (one line, no phases, no DMA) after the last transfer of the
 * driver, with nothing of the device queued; neither a new transfer, nor one
 * of the driver, nor a change of the D/C line comes while one is under way.
 */
#define CONFIG_DGX_ENABLE_SPI 1
#define CONFIG_SPIRAM         1
#define SOC_PSRAM_DMA_CAPABLE 1
#ifndef SOC_AHB_GDMA_VERSION
#define SOC_AHB_GDMA_VERSION 1 /* an ESP32-S3, whose GDMA aligns by itself */
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

/* both buses name their log tag TAG */
#define TAG TAG_SPI
#include "bus/spi_esp32.c"
#undef TAG
#define TAG TAG_QSPI
#include "bus/qspi_esp32.c"
#undef TAG

#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

#define PW 64
#define PH 48

/* ---- the display ---- */

static uint32_t panel[PW * PH];
static int      panel_pixel_bytes = 2;
static int      panel_command     = -1; /* the command the data that comes belongs to */
static int      win_l, win_r = PW - 1, win_t, win_b = PH - 1;
static int      at_x, at_y; /* the next pixel written */
static uint8_t  part[4];    /* bytes of a pixel or of a bound that is not whole yet */
static int      part_bytes;
static long     stray_bytes; /* data the display took for parameters of a command it does not know */
static long     unknown_commands;
static long     bare_memory_writes; /* QSPI: 0x2C with no pixels, the pixels after it going on as 0x3C */

static void panel_reset(int pixel_bytes)
{
    memset(panel, 0, sizeof(panel));
    panel_pixel_bytes = pixel_bytes;
    panel_command     = -1, part_bytes = 0;
    win_l = win_t = 0, win_r = PW - 1, win_b = PH - 1;
    at_x = at_y = 0;
    stray_bytes = unknown_commands = bare_memory_writes = 0;
}

static void panel_take_command(uint8_t command)
{
    part_bytes = 0; /* a pixel that is not whole is dropped */
    switch (command) {
    case 0x3C:
        command = 0x2C; /* the memory write goes on where it was */
        break;
    case 0x2C:
        at_x = win_l, at_y = win_t;
        break;
    case 0x2A:
    case 0x2B:
    case 0x36:
    case 0x3A:
        break;
    default:
        ++unknown_commands;
    }
    panel_command = command;
}

static void panel_take_data(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        switch (panel_command) {
        case 0x2A:
        case 0x2B:
            if (part_bytes < 4) part[part_bytes++] = data[i];
            if (part_bytes == 4) {
                int from = part[0] << 8 | part[1], to = part[2] << 8 | part[3];
                CHECK(from <= to && to < (panel_command == 0x2A ? PW : PH));
                if (panel_command == 0x2A) win_l = from, win_r = to;
                else win_t = from, win_b = to;
                ++part_bytes; /* more than four is a mistake of the sender, and is not taken */
            }
            break;
        case 0x2C:
            part[part_bytes++] = data[i];
            if (part_bytes == panel_pixel_bytes) {
                uint32_t color = 0;
                for (int k = 0; k < panel_pixel_bytes; ++k) color = color << 8 | part[k];
                panel[at_x + at_y * PW] = color;
                part_bytes              = 0;
                if (++at_x > win_r) {
                    at_x = win_l;
                    if (++at_y > win_b) at_y = win_t;
                }
            }
            break;
        case 0x36:
        case 0x3A:
            break;
        default:
            ++stray_bytes;
        }
    }
}

static int panel_wrong(dgx_screen_t *model)
{
    int wrong = 0;
    for (int y = 0; y < PH; ++y) {
        for (int x = 0; x < PW; ++x) wrong += panel[x + y * PW] != dgx_get_pixel(model, x, y);
    }
    return wrong;
}

/* ---- the GPIO registers the callback of the SPI bus writes ---- */

static uint64_t gpio_levels;
static int      dc_pin; /* the D/C line of the display under test */

/* the registers of the hosts, see hal/spi_ll.h */
spi_dev_t model_spi_hw[SOC_SPI_PERIPH_NUM];

static bool model_running(void)
{
    for (int i = 0; i < SOC_SPI_PERIPH_NUM; ++i)
        if (model_spi_hw[i].running) return true;
    return false;
}

void dgx_host_reg_write(uint32_t reg, uint32_t value)
{
    const uint64_t before = gpio_levels;
    switch (reg) {
    case GPIO_OUT_W1TS_REG:
        gpio_levels |= value;
        break;
    case GPIO_OUT_W1TC_REG:
        gpio_levels &= ~(uint64_t)value;
        break;
    case GPIO_OUT1_W1TS_REG:
        gpio_levels |= (uint64_t)value << 32;
        break;
    case GPIO_OUT1_W1TC_REG:
        gpio_levels &= ~((uint64_t)value << 32);
        break;
    default:
        CHECK(!"a register that sets no GPIO output");
    }
    /* the D/C line does not change under a transfer */
    if (((before ^ gpio_levels) >> dc_pin) & 1u) CHECK(!model_running());
}

/* ---- memory: a part of it stands for PSRAM ---- */

static uint8_t psram[PW * PH * 4 + 256] __attribute__((aligned(64)));

bool esp_ptr_external_ram(const void *p)
{
    const uint8_t *b = p;
    return b >= psram && b < psram + sizeof(psram);
}

bool esp_ptr_dma_capable(const void *p)
{
    return !esp_ptr_external_ram(p);
}

bool esp_ptr_dma_ext_capable(const void *p)
{
    return esp_ptr_external_ram(p);
}

/* the flash is encrypted: on a chip whose GDMA does not align by itself, DMA from PSRAM asks 16 bytes */
static bool flash_encrypted;

bool efuse_hal_flash_encryption_enabled(void)
{
    return flash_encrypted;
}

static inline bool psram_asks_16(void)
{
    return SOC_AHB_GDMA_VERSION != 1 && flash_encrypted;
}

/* ---- the SPI driver of IDF ---- */

#define MODEL_QUEUE 16

typedef struct {
    bool                initialized;
    int                 max_transfer_sz;
    int                 devices;
    spi_device_handle_t holder; /* the device that has acquired the bus */
} model_host_t;

static model_host_t model_hosts[SOC_SPI_PERIPH_NUM];

struct spi_device_t {
    spi_host_device_t             host;
    spi_device_interface_config_t cfg;
    spi_transaction_t            *queue[MODEL_QUEUE];
    spi_transaction_t             queued_as[MODEL_QUEUE]; /* each as it was when it was queued */
    uint64_t                      queued_sum[MODEL_QUEUE]; /* and a sum of the bytes it sends */
    bool                          queued_late[MODEL_QUEUE]; /* and whether DMA from PSRAM falls behind on it */
    int                           queued;
};

static long transfers, longest_transfer;
static long direct_transfers, acquires; /* transfers by the registers of the host; spi_device_acquire_bus() calls */
static long queued_while_held;          /* pixels queued for DMA by a device that holds the bus */
static spi_device_handle_t model_other; /* a device of the program on the host, not the display: what it sends goes elsewhere */
static long                other_transfers;
static long queue_calls, refused_call; /* the queued transfer of that number is refused, as when the driver has no memory to copy it */
static long queued_short;              /* transfers of 4 bytes at most that were queued, not polled */
#ifdef SPI_TRANS_DMA_USE_PSRAM
static long psram_in_place, psram_copied; /* transfers with SPI_TRANS_DMA_USE_PSRAM: sent as they lie, or copied by the driver first */
static long psram_dma_calls, late_dma_call; /* the transfer with SPI_TRANS_DMA_USE_PSRAM of that number has the DMA fall behind the wire */
#endif

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config, spi_dma_chan_t dma_chan)
{
    CHECK(dma_chan != SPI_DMA_DISABLED);
    model_host_t *h = &model_hosts[host_id];
    if (h->initialized) return ESP_ERR_INVALID_STATE;
    h->initialized = true;
    /* whole DMA descriptors of 4092 bytes, one at least (spicommon_dma_desc_alloc() of IDF) */
    const int asked    = bus_config->max_transfer_sz ? bus_config->max_transfer_sz : 4092;
    h->max_transfer_sz = (asked + 4091) / 4092 * 4092;
    return ESP_OK;
}

/* a sum of the bytes a transfer sends, to tell them changed */
static uint64_t model_sum(const spi_transaction_t *t)
{
    const uint8_t *bytes = (t->flags & SPI_TRANS_USE_TXDATA) ? t->tx_data : t->tx_buffer;
    uint64_t       sum   = 14695981039346656037u;
    for (size_t i = 0; i < (t->length + 7u) / 8u; ++i) sum = (sum ^ bytes[i]) * 1099511628211u;
    return sum;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    model_host_t *h = &model_hosts[host_id];
    if (!h->initialized || h->devices) return ESP_ERR_INVALID_STATE;
    h->initialized = false;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t host_id, const spi_device_interface_config_t *dev_config, spi_device_handle_t *handle)
{
    model_host_t *h = &model_hosts[host_id];
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    CHECK(dev_config->queue_size >= 2 && dev_config->queue_size <= MODEL_QUEUE);
    /* as an ESP32 on pins of the GPIO matrix: a full-duplex device above 26.7 MHz that may add dummy cycles is refused */
    if (!(dev_config->flags & (SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY)) && dev_config->clock_speed_hz > 80 * 1000 * 1000 / 3) return ESP_ERR_NOT_SUPPORTED;
    spi_device_handle_t d = calloc(1, sizeof(*d));
    d->host               = host_id;
    d->cfg                = *dev_config;
    ++h->devices;
    *handle = d;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
    CHECK(handle->queued == 0); /* the driver refuses a device with transfers on their way */
    CHECK(model_hosts[handle->host].holder != handle);
    --model_hosts[handle->host].devices;
    free(handle);
    return ESP_OK;
}

esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t host_id, size_t *max_bytes)
{
    if (!model_hosts[host_id].initialized) return ESP_ERR_INVALID_ARG;
    *max_bytes = (size_t)model_hosts[host_id].max_transfer_sz;
    return ESP_OK;
}

esp_err_t spi_device_get_actual_freq(spi_device_handle_t handle, int *freq_khz)
{
    *freq_khz = handle->cfg.clock_speed_hz / 1000;
    return ESP_OK;
}

/* what the driver checks before it takes a transfer */
static esp_err_t model_check(spi_device_handle_t dev, const spi_transaction_t *t)
{
    if (t->length > (size_t)model_hosts[dev->host].max_transfer_sz * 8u) return ESP_ERR_INVALID_ARG; /* "txdata transfer > host maximum" */
    if ((t->flags & SPI_TRANS_USE_TXDATA) && t->length > 32) return ESP_ERR_INVALID_ARG;
#ifdef SPI_TRANS_DMA_USE_PSRAM
    if (t->flags & SPI_TRANS_DMA_USE_PSRAM) {
        CHECK(!(t->flags & SPI_TRANS_USE_TXDATA) && esp_ptr_external_ram(t->tx_buffer));
        if (psram_asks_16() && ((uintptr_t)t->tx_buffer | (t->length + 7u) / 8u) % 16u) ++psram_copied;
        else ++psram_in_place;
    }
#endif
    return ESP_OK;
}

/* DMA from PSRAM falls behind the wire on this transfer, the one of late_dma_call */
static bool model_late(const spi_transaction_t *t)
{
#ifdef SPI_TRANS_DMA_USE_PSRAM
    return (t->flags & SPI_TRANS_DMA_USE_PSRAM) && ++psram_dma_calls == late_dma_call;
#else
    (void)t;
    return false;
#endif
}

/* how a transfer DMA from PSRAM fell behind on ends: the driver sets SPI_TRANS_DMA_TX_FAIL in it and tells ESP_ERR_INVALID_STATE */
static esp_err_t model_result(spi_transaction_t *t, bool late)
{
#ifdef SPI_TRANS_DMA_USE_PSRAM
    if (late) {
        t->flags |= SPI_TRANS_DMA_TX_FAIL;
        return ESP_ERR_INVALID_STATE;
    }
#endif
    (void)t, (void)late;
    return ESP_OK;
}

/* a transfer of the driver: none by the registers is under way, and the host is set as the transfer asks, by DMA */
static void model_driver_sets(spi_device_handle_t dev, const spi_transaction_t *t)
{
    spi_dev_t *hw = &model_spi_hw[dev->host];
    CHECK(!hw->running);
    const uint8_t data_lines = (t->flags & SPI_TRANS_MODE_QIO) ? 4 : 1;
    hw->lines                = (spi_line_mode_t){1, 1, data_lines};
    hw->user.fwrite_quad     = data_lines == 4;
    hw->command_bits         = dev->cfg.command_bits;
    hw->addr_bits            = dev->cfg.address_bits;
    hw->dummy                = dev->cfg.dummy_bits;
    hw->mosi                 = true;
    hw->miso                 = false;
    hw->dma_tx               = true;
}

/* the transfer goes out on the wire now, and the display takes it; bytes DMA from PSRAM came too late for are wrong ones */
static void model_wire(spi_device_handle_t dev, spi_transaction_t *t, bool late)
{
    const uint8_t *bytes = (t->flags & SPI_TRANS_USE_TXDATA) ? t->tx_data : t->tx_buffer;
    const size_t   n     = (t->length + 7u) / 8u;
    uint8_t       *wrong = NULL;
    model_driver_sets(dev, t);
    if (dev == model_other) {
        ++other_transfers;
        return;
    }
    if (late) {
        wrong = malloc(n);
        for (size_t i = 0; i < n; ++i) wrong[i] = (uint8_t)~bytes[i];
        bytes = wrong;
    }
    ++transfers;
    if ((long)n > longest_transfer) longest_transfer = (long)n;
    if (dev->cfg.command_bits) {
        /* QSPI: the command of the panel in the middle byte of the address */
        const uint8_t command = (uint8_t)(t->addr >> 8);
        if (t->cmd == 0x02) {
            CHECK(!(t->flags & SPI_TRANS_MODE_QIO));
            if (command == 0x2C && !n) ++bare_memory_writes;
            panel_take_command(command);
            if (n) panel_take_data(bytes, n);
        } else {
            CHECK(t->cmd == 0x32 && (t->flags & SPI_TRANS_MODE_QIO));
            CHECK(command == 0x2C || command == 0x3C);
            CHECK(n % (size_t)panel_pixel_bytes == 0); /* a piece is whole pixels */
            panel_take_command(command);
            panel_take_data(bytes, n);
        }
    } else {
        if (dev->cfg.pre_cb) dev->cfg.pre_cb(t);
        if ((gpio_levels >> dc_pin) & 1u) {
            panel_take_data(bytes, n);
        } else {
            for (size_t i = 0; i < n; ++i) panel_take_command(bytes[i]);
        }
    }
    free(wrong);
}

/* a device other than the one that holds the bus would wait for it for ever */
static void model_not_held_off(spi_device_handle_t handle)
{
    const spi_device_handle_t holder = model_hosts[handle->host].holder;
    if (holder && holder != handle) {
        fprintf(stderr, "a transfer of a device while another one holds the bus: the task would wait for ever\n");
        exit(EXIT_FAILURE);
    }
}

esp_err_t spi_device_acquire_bus(spi_device_handle_t device, TickType_t wait)
{
    (void)wait;
    model_not_held_off(device);
    CHECK(model_hosts[device->host].holder == NULL); /* the driver refuses a device that holds it already */
    CHECK(device->queued == 0);                      /* DGX waits for its pixels first */
    model_hosts[device->host].holder = device;
    ++acquires;
    return ESP_OK;
}

void spi_device_release_bus(spi_device_handle_t dev)
{
    CHECK(model_hosts[dev->host].holder == dev);
    CHECK(!model_spi_hw[dev->host].running);
    model_hosts[dev->host].holder = NULL;
}

/* a transfer by the registers: the bytes of the buffer, a command or data by the D/C line */
void spi_ll_user_start(spi_dev_t *hw)
{
    const int                 host   = (int)(hw - model_spi_hw);
    const spi_device_handle_t holder = model_hosts[host].holder;
    CHECK(holder != NULL);            /* only on a held bus */
    CHECK(!hw->running);              /* the one before is done */
    CHECK(hw->applied);               /* the setting reached the host */
    CHECK(holder == NULL || holder->queued == 0);
    CHECK(!hw->miso && !hw->dma_tx && hw->dummy == 0);
    CHECK(hw->lines.cmd_lines == 1 && hw->lines.addr_lines == 1);
    const int      data_lines = hw->user.fwrite_quad ? 4 : 1; /* what the host writes by: the field, whatever was set by the line mode */
    const uint8_t *bytes = (const uint8_t *)hw->data_buf;
    const size_t   n     = hw->mosi ? (hw->bitlen + 7u) / 8u : 0;
    if (hw->mosi) CHECK(hw->bitlen > 0 && hw->bitlen <= SOC_SPI_MAXIMUM_BUFFER_SIZE * 8u);
    if (holder && holder->cfg.command_bits) {
        /* QSPI: the opcode and the command in the middle byte of the address, as model_wire() takes them */
        CHECK(hw->command_bits == 8 && hw->addr_bits == 24);
        const uint8_t command = (uint8_t)(hw->address >> 8);
        if (hw->command == 0x02) {
            CHECK(data_lines == 1);
            if (command == 0x2C && !n) ++bare_memory_writes;
            panel_take_command(command);
            if (n) panel_take_data(bytes, n);
        } else {
            CHECK(hw->command == 0x32 && data_lines == 4 && n > 0);
            CHECK(command == 0x2C || command == 0x3C);
            CHECK(n % (size_t)panel_pixel_bytes == 0); /* a piece is whole pixels */
            panel_take_command(command);
            panel_take_data(bytes, n);
        }
    } else {
        CHECK(data_lines == 1 && hw->command_bits == 0 && hw->addr_bits == 0 && hw->mosi);
        if ((gpio_levels >> dc_pin) & 1u) {
            panel_take_data(bytes, n);
        } else {
            for (size_t i = 0; i < n; ++i) panel_take_command(bytes[i]);
        }
    }
    ++direct_transfers;
    if ((long)n > longest_transfer) longest_transfer = (long)n;
    hw->applied = false;
    hw->running = true;
}

/* a transfer under way is done the first time it is asked about */
uint32_t spi_ll_get_running_cmd(spi_dev_t *hw)
{
    const bool running = hw->running;
    hw->running        = false;
    return running;
}

esp_err_t spi_device_queue_trans(spi_device_handle_t handle, spi_transaction_t *trans_desc, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    model_not_held_off(handle);
    CHECK(!model_spi_hw[handle->host].running);
    if (model_hosts[handle->host].holder == handle) ++queued_while_held;
    esp_err_t rc = model_check(handle, trans_desc);
    if (rc != ESP_OK) return rc;
    if (++queue_calls == refused_call) return ESP_ERR_NO_MEM;
    for (int i = 0; i < handle->queued; ++i) CHECK(handle->queue[i] != trans_desc); /* a descriptor queued twice */
    CHECK(handle->queued < handle->cfg.queue_size);
    if (handle->queued == MODEL_QUEUE) return ESP_ERR_INVALID_STATE;
    if (trans_desc->length <= 32) ++queued_short;
    handle->queue[handle->queued] = trans_desc;
    memcpy(&handle->queued_as[handle->queued], trans_desc, sizeof(*trans_desc));
    handle->queued_sum[handle->queued]  = model_sum(trans_desc);
    handle->queued_late[handle->queued] = model_late(trans_desc);
    ++handle->queued;
    return ESP_OK;
}

esp_err_t spi_device_get_trans_result(spi_device_handle_t handle, spi_transaction_t **trans_desc, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (!handle->queued) {
        fprintf(stderr, "spi_device_get_trans_result: nothing is queued, the task would wait for ever\n");
        exit(EXIT_FAILURE);
    }
    spi_transaction_t *t = handle->queue[0];
    CHECK(memcmp(t, &handle->queued_as[0], sizeof(*t)) == 0); /* the descriptor was changed while it was queued */
    CHECK(model_sum(t) == handle->queued_sum[0]);              /* its bytes were written over while on their way */
    const bool late = handle->queued_late[0];
    model_wire(handle, t, late);
    --handle->queued;
    memmove(handle->queue, handle->queue + 1, sizeof(handle->queue[0]) * (size_t)handle->queued);
    memmove(handle->queued_as, handle->queued_as + 1, sizeof(handle->queued_as[0]) * (size_t)handle->queued);
    memmove(handle->queued_sum, handle->queued_sum + 1, sizeof(handle->queued_sum[0]) * (size_t)handle->queued);
    memmove(handle->queued_late, handle->queued_late + 1, sizeof(handle->queued_late[0]) * (size_t)handle->queued);
    *trans_desc = t;
    return model_result(t, late);
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *trans_desc)
{
    model_not_held_off(handle);
    CHECK(handle->queued == 0); /* the transfers queued before go first */
    esp_err_t rc = model_check(handle, trans_desc);
    if (rc != ESP_OK) return rc;
    const bool late = model_late(trans_desc);
    model_wire(handle, trans_desc, late);
    return model_result(trans_desc, late);
}

esp_err_t spi_device_transmit(spi_device_handle_t handle, spi_transaction_t *trans_desc)
{
    esp_err_t rc = spi_device_queue_trans(handle, trans_desc, portMAX_DELAY);
    if (rc != ESP_OK) return rc;
    spi_transaction_t *done = NULL;
    rc                      = spi_device_get_trans_result(handle, &done, portMAX_DELAY);
    CHECK(done == trans_desc);
    return rc;
}

/* ---- the tests ---- */

static uint32_t seed = 31337;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static uint32_t rnd_color(int bits)
{
    return bits == 16 ? (uint32_t)rnd(0, 0xffff) : (uint32_t)rnd(0, 0xffff) << 8 | (uint32_t)rnd(0, 0xff);
}

static void screen_on(dgx_screen_with_bus_t *s, dgx_bus_protocols_t *bus, int bits)
{
    memset(s, 0, sizeof(*s));
    s->bus             = bus;
    s->scr.width       = PW, s->scr.height = PH, s->scr.color_bits = (uint8_t)bits;
    s->scr.screen_subtype = DgxPhysicalScreenWithBus;
    dgx_screen_with_bus_init_area(s, 0x2A, 0x2B, 0x2C, 0, DGX_SCREEN_AREA_PROTO_STD16);
    dgx_scr_init_slow_bus_optimized_funcs(&s->scr);
}

/* what is drawn on the display is drawn on a virtual screen too, and the two are compared now and then */
static void draw_and_compare(dgx_screen_t *lcd, int bits, int rounds)
{
    dgx_screen_t *model   = dgx_vscreen_init(PW, PH, (uint8_t)bits, DgxScreenRGB);
    dgx_screen_t *picture = dgx_vscreen_init(37, 23, (uint8_t)bits, DgxScreenRGB);
    CHECK(model && picture);
    for (int y = 0; y < picture->height; ++y) {
        for (int x = 0; x < picture->width; ++x) dgx_set_pixel(picture, x, y, rnd_color(bits));
    }
    dgx_screen_t *both[2] = {lcd, model};
    for (int round = 0; round < rounds; ++round) {
        const int      what = rnd(0, 9), x = rnd(-10, PW + 5), y = rnd(-10, PH + 5);
        const int      w = rnd(1, PW + 10), h = rnd(1, PH + 10), r = rnd(0, 30);
        const uint32_t color = rnd_color(bits);
        const dgx_output_orientation_t turn = (dgx_output_orientation_t)rnd(0, 7);
        const int      part_x = rnd(-3, 20), part_y = rnd(-3, 12), part_w = rnd(1, 40), part_h = rnd(1, 25);
        const dgx_point_2d_t quad[4] = {{(int16_t)x, (int16_t)y}, {(int16_t)(x + w / 2), (int16_t)(y + rnd(-5, 5))},
                                        {(int16_t)(x + w / 2 + rnd(-5, 5)), (int16_t)(y + h / 2)}, {(int16_t)(x + rnd(-5, 5)), (int16_t)(y + h / 2)}};
        for (int k = 0; k < 2; ++k) {
            dgx_screen_t *scr = both[k];
            switch (what) {
            case 0:
                dgx_fill_rectangle(scr, x, y, w, h, color);
                break;
            case 1:
                dgx_fill_rectangle(scr, 0, 0, PW, PH, color);
                break;
            case 2:
                dgx_set_pixel(scr, x, y, color);
                break;
            case 3:
                dgx_draw_line(scr, x, y, x + w - 20, y + h - 20, color);
                break;
            case 4:
                dgx_solid_circle(scr, x, y, r, color);
                break;
            case 5:
                dgx_font_string_utf8_screen(scr, (int16_t)x, (int16_t)y, "SPI bus", color, DgxOutputNormal, 1 + (round & 1), TerminusTTFMedium12(), NULL, NULL);
                break;
            case 6:
                dgx_vscreen_to_screen(scr, x, y, picture, 0);
                break;
            case 7:
                dgx_vscreen_region_to_screen(scr, x, y, picture, part_x, part_y, part_w, part_h, 0);
                break;
            case 8:
                dgx_draw_texture_quad(scr, quad, picture, 0, 0, picture->width, picture->height);
                break;
            default:
                dgx_vscreen_to_screen(scr, x, y, picture, DGX_VSCREEN_ORIENTED(turn));
                break;
            }
        }
        if (round % 16 == 15 || round == rounds - 1) {
            lcd->wait_buffer(lcd);
            const int wrong = panel_wrong(model);
            CHECK(wrong == 0);
            if (wrong) break;
        }
    }
    dgx_screen_destroy(&model);
    dgx_screen_destroy(&picture);
}

/* an SPI display of one data line and a D/C line, with the buffer DGX picks or a shorter one */
static void test_spi(int bits, size_t buffer_max, int dc)
{
    panel_reset(bits == 16 ? 2 : 3);
    dc_pin                          = dc;
    const dgx_spi_options_t options = {.buffer_max = buffer_max};
    dgx_bus_protocols_t *bus = dgx_spi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 13, 12, 14, 15, dc, 40 * 1000 * 1000, 0, &options);
    CHECK(bus && bus->bus_type == DGX_BUS_SPI && model_hosts[SPI2_HOST].initialized);
    if (!bus) return;
    CHECK(bus->buffer_len == (buffer_max ? buffer_max - buffer_max % 16u : 4736u)); /* 40 MHz: 4750 bytes, in multiples of 16 */
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, bits);
    longest_transfer = queued_short = 0;
    draw_and_compare(&s.scr, bits, 1500);
    CHECK(stray_bytes == 0 && unknown_commands == 0);
    CHECK(longest_transfer <= (long)bus->buffer_len);
    CHECK(queued_short == 0);
    bus->dispose(bus);
    CHECK(!model_hosts[SPI2_HOST].initialized); /* the host DGX set up goes with its last display */
}

/* a QSPI display: commands on one line, pixels on four */
static void test_qspi(int bits, size_t buffer_max)
{
    panel_reset(bits == 16 ? 2 : 3);
    const dgx_spi_options_t options = {.buffer_max = buffer_max};
    dgx_bus_protocols_t *bus = dgx_qspi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 40, 39, 47, 45, 40 * 1000 * 1000, 0, &options);
    CHECK(bus && bus->bus_type == DGX_BUS_QSPI);
    if (!bus) return;
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, bits);
    longest_transfer = queued_short = 0;
    draw_and_compare(&s.scr, bits, 1500);
    CHECK(stray_bytes == 0 && unknown_commands == 0);
    CHECK(longest_transfer <= (long)bus->buffer_len);
    CHECK(queued_short == 0);
    bus->dispose(bus);
    CHECK(!model_hosts[SPI2_HOST].initialized);
}

/*
 * A whole frame handed to write_area() at once, longer than the buffer of the
 * bus, from internal memory and from PSRAM: it goes in pieces the host takes.
 */
static void test_whole_frame(bool qspi)
{
    panel_reset(2);
    dc_pin                          = 2;
    const dgx_spi_options_t options = {.buffer_max = 1000};
    dgx_bus_protocols_t    *bus     = qspi ? dgx_qspi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 40, 39, 47, 45, 40 * 1000 * 1000, 0, &options)
                                           : dgx_spi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 13, 12, 14, 15, 2, 40 * 1000 * 1000, 0, &options);
    CHECK(bus != NULL);
    if (!bus) return;
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, 16);
    dgx_screen_t *lcd    = &s.scr;
    uint8_t      *memory = malloc(PW * PH * 2);
    for (int from_psram = 0; from_psram < 2; ++from_psram) {
        uint8_t *frame = from_psram ? psram : memory;
        for (int i = 0; i < PW * PH * 2; ++i) frame[i] = (uint8_t)rnd(0, 255);
        longest_transfer = 0;
        lcd->wait_buffer(lcd);
        lcd->set_area(lcd, 0, PW - 1, 0, PH - 1);
        lcd->write_area(lcd, frame, PW * PH * 16u);
        lcd->wait_buffer(lcd);
        int wrong = 0;
        for (int i = 0; i < PW * PH; ++i) wrong += panel[i] != (uint32_t)(frame[2 * i] << 8 | frame[2 * i + 1]);
        CHECK(wrong == 0);
        CHECK(longest_transfer <= (long)bus->buffer_len);
    }
    free(memory);
    bus->dispose(bus);
}

/* the window of w by h pixels at the top left of the display holds the pixels of frame, 16 bits each */
static int window_wrong(const uint8_t *frame, int w, int h)
{
    int wrong = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) wrong += panel[x + y * PW] != (uint32_t)(frame[2 * (x + y * w)] << 8 | frame[2 * (x + y * w) + 1]);
    }
    return wrong;
}

/* w by h pixels of frame sent into the window at the top left of the display, in one write_area() */
static void send_window(dgx_screen_t *lcd, const uint8_t *frame, int w, int h)
{
    lcd->wait_buffer(lcd);
    lcd->set_area(lcd, 0, (uint16_t)(w - 1), 0, (uint16_t)(h - 1));
    lcd->write_area(lcd, (uint8_t *)frame, (uint32_t)(w * h) * 16u);
    lcd->wait_buffer(lcd);
}

/*
 * A buffer of 4 bytes, shorter than a piece of 12, as a program may ask for:
 * a frame handed whole still goes in pieces the host takes, of 12 bytes, and
 * not in one transfer the host of the program refuses.
 */
static void test_short_buffer(bool qspi)
{
    panel_reset(2);
    dc_pin                     = 2;
    const spi_bus_config_t cfg = {.mosi_io_num = 13, .miso_io_num = 12, .sclk_io_num = 14, .quadwp_io_num = 27, .quadhd_io_num = 26, .max_transfer_sz = 4092};
    CHECK(spi_bus_initialize(SPI3_HOST, &cfg, SPI_DMA_CH_AUTO) == ESP_OK);
    const dgx_spi_options_t options = {.buffer_max = 5};
    dgx_bus_protocols_t    *bus     = qspi ? dgx_qspi_init_on_bus_with(SPI3_HOST, 45, 40 * 1000 * 1000, 0, &options)
                                           : dgx_spi_init_on_bus_with(SPI3_HOST, 15, 2, 40 * 1000 * 1000, 0, &options);
    CHECK(bus && bus->buffer_len == 4);
    if (bus) {
        static dgx_screen_with_bus_t s;
        screen_on(&s, bus, 16);
        uint8_t *frame = malloc(PW * PH * 2);
        for (int i = 0; i < PW * PH * 2; ++i) frame[i] = (uint8_t)rnd(0, 255);
        longest_transfer = 0;
        send_window(&s.scr, frame, PW, PH);
        CHECK(window_wrong(frame, PW, PH) == 0);
        CHECK(longest_transfer == 12);
        dgx_fill_rectangle(&s.scr, 0, 0, PW, PH, 0); /* as the virtual screen it is compared with starts */
        draw_and_compare(&s.scr, 16, 64);
        free(frame);
        bus->dispose(bus);
    }
    CHECK(spi_bus_free(SPI3_HOST) == ESP_OK);
}

/*
 * A host of the program asked for 2048 bytes in one transfer: the driver
 * gives it a whole DMA descriptor, 4092 bytes, so the 4000 bytes of buffer
 * DGX takes at least are never more than the host takes.
 */
static void test_small_host_max(bool qspi)
{
    panel_reset(2);
    dc_pin                     = 2;
    const spi_bus_config_t cfg = {.mosi_io_num = 13, .miso_io_num = 12, .sclk_io_num = 14, .quadwp_io_num = 27, .quadhd_io_num = 26, .max_transfer_sz = 2048};
    CHECK(spi_bus_initialize(SPI3_HOST, &cfg, SPI_DMA_CH_AUTO) == ESP_OK);
    dgx_bus_protocols_t *bus = qspi ? dgx_qspi_init_on_bus(SPI3_HOST, 45, 40 * 1000 * 1000, 0) : dgx_spi_init_on_bus(SPI3_HOST, 15, 2, 40 * 1000 * 1000, 0);
    /* SPI at 40 MHz would take 4736 bytes and QSPI 18992: both get what the host takes, in multiples of 16 */
    CHECK(bus && bus->buffer_len == 4080u);
    if (bus) {
        static dgx_screen_with_bus_t s;
        screen_on(&s, bus, 16);
        uint8_t *frame = malloc(PW * PH * 2);
        for (int i = 0; i < PW * PH * 2; ++i) frame[i] = (uint8_t)rnd(0, 255);
        send_window(&s.scr, frame, PW, PH);
        CHECK(window_wrong(frame, PW, PH) == 0);
        free(frame);
        bus->dispose(bus);
    }
    CHECK(spi_bus_free(SPI3_HOST) == ESP_OK);
}

/*
 * A window of a QSPI display set and then waited for, before its pixels: the
 * 0x2C stays held and goes with the first of them, never alone. A window
 * left without pixels has its 0x2C sent bare by the next command, which does
 * no harm.
 */
static void test_wait_after_window(void)
{
    panel_reset(2);
    dgx_bus_protocols_t *bus = dgx_qspi_init(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 40, 39, 47, 45, 40 * 1000 * 1000, 0);
    CHECK(bus != NULL);
    if (!bus) return;
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, 16);
    dgx_screen_t *lcd = &s.scr;
    uint8_t       frame[10 * 7 * 2];
    for (size_t i = 0; i < sizeof(frame); ++i) frame[i] = (uint8_t)rnd(0, 255);
    lcd->wait_buffer(lcd);
    lcd->set_area(lcd, 0, 9, 0, 6);
    lcd->wait_buffer(lcd);
    lcd->write_area(lcd, frame, sizeof(frame) * 8u);
    lcd->wait_buffer(lcd);
    CHECK(bare_memory_writes == 0);
    CHECK(window_wrong(frame, 10, 7) == 0);
    /* a window with no pixels, then another one with them */
    lcd->set_area(lcd, 20, 29, 20, 26);
    lcd->wait_buffer(lcd);
    send_window(lcd, frame, 10, 7);
    CHECK(bare_memory_writes == 1);
    CHECK(window_wrong(frame, 10, 7) == 0);
    bus->dispose(bus);
}

#ifndef TEST_SPI_IDF54
/*
 * Pixels in PSRAM go where they lie, with SPI_TRANS_DMA_USE_PSRAM, and the
 * driver copies none of them. Where DMA from PSRAM asks nothing, as on an
 * ESP32-S3, that is whatever byte they start on and however long the last
 * piece is. Where it asks 16 bytes, as on a P4 under flash encryption, pixels
 * that start off them do not go in place at all, and a last piece that ends
 * off them goes without the flag, for the driver to copy into internal
 * memory. QSPI too at a clock DMA from PSRAM keeps up with.
 */
static void test_psram_pieces(bool qspi)
{
    panel_reset(2);
    dc_pin                          = 2;
    const dgx_spi_options_t options = {.buffer_max = 1000};
    /* QSPI at 26.7 MHz, where DMA from PSRAM keeps up with its four lines */
    dgx_bus_protocols_t *bus = qspi ? dgx_qspi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 40, 39, 47, 45, 26666667, 0, &options)
                                    : dgx_spi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 13, 12, 14, 15, 2, 40 * 1000 * 1000, 0, &options);
    CHECK(bus != NULL);
    if (!bus) return;
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, 16);
    const bool asks = psram_asks_16();
    CHECK(bus->sends_from(bus, psram + 16) && bus->sends_from(bus, psram + 8) == !asks);
    /* the whole width, a width whose last piece ends off 16 bytes, and pixels that start off them */
    const struct {
        int offset, w, h;
    } sends[] = {{0, PW, PH}, {0, PW - 1, PH - 1}, {8, PW, PH - 1}};
    for (size_t k = 0; k < sizeof(sends) / sizeof(sends[0]); ++k) {
        uint8_t *frame = psram + sends[k].offset;
        for (int i = 0; i < sends[k].w * sends[k].h * 2; ++i) frame[i] = (uint8_t)rnd(0, 255);
        psram_in_place = psram_copied = 0;
        send_window(&s.scr, frame, sends[k].w, sends[k].h);
        CHECK(window_wrong(frame, sends[k].w, sends[k].h) == 0);
        CHECK(psram_copied == 0);
        CHECK(asks && sends[k].offset ? psram_in_place == 0 : psram_in_place >= 6);
    }
    bus->dispose(bus);
}

/*
 * DMA from PSRAM falls behind the wire on a piece of a virtual screen in
 * PSRAM that goes in place: that piece reaches the panel wrong. The bus then
 * sends nothing from PSRAM where it lies, the rectangle is sent again through
 * the halves of the bus buffer and comes out right, and so does the next one,
 * with no piece from PSRAM in place. The log tells of it once.
 */
static void test_psram_late(bool qspi)
{
    panel_reset(2);
    dc_pin                          = 2;
    const dgx_spi_options_t options = {.buffer_max = 1000};
    /* QSPI at 26.7 MHz, where DMA from PSRAM is taken to keep up with its four lines */
    dgx_bus_protocols_t *bus = qspi ? dgx_qspi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 40, 39, 47, 45, 26666667, 0, &options)
                                    : dgx_spi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 13, 12, 14, 15, 2, 40 * 1000 * 1000, 0, &options);
    CHECK(bus != NULL);
    if (!bus) return;
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, 16);
    /* the pixels of a virtual screen of the whole width put in PSRAM, so that it goes in place */
    dgx_screen_t  *picture = dgx_vscreen_init(PW, PH, 16, DgxScreenRGB);
    dgx_vscreen_t *v       = (dgx_vscreen_t *)picture;
    uint8_t       *own     = v->v_array;
    v->v_array             = psram;
    CHECK(bus->sends_from(bus, psram));
    for (int round = 0; round < 2; ++round) {
        for (int y = 0; y < PH; ++y) {
            for (int x = 0; x < PW; ++x) dgx_set_pixel(picture, x, y, rnd_color(16));
        }
        psram_in_place = psram_dma_calls = 0;
        late_dma_call                    = round ? 0 : 2; /* the second piece from PSRAM of the first rectangle */
        dgx_vscreen_to_screen(&s.scr, 0, 0, picture, 0);
        s.scr.wait_buffer(&s.scr);
        CHECK(round ? psram_in_place == 0 : psram_dma_calls >= 2);
        CHECK(panel_wrong(picture) == 0);
        CHECK(!bus->sends_from(bus, psram));
    }
    late_dma_call = 0;
    v->v_array    = own;
    dgx_screen_destroy(&picture);
    bus->dispose(bus);
}
#endif

#ifdef NDEBUG
/*
 * With assertions off, a transfer of pixels the driver refuses, as when it
 * has no memory for a copy of them, loses those pixels and no more: the bus
 * neither waits for its result, which never comes, nor takes the descriptor
 * of the transfer still on its way for the next one, and the window after it
 * comes out right. A turned picture goes through the two halves of the bus
 * buffer: the half before a refused piece is not written while it goes.
 */
static void test_refused(bool qspi)
{
    panel_reset(2);
    dc_pin                          = 2;
    const dgx_spi_options_t options = {.buffer_max = 1000};
    dgx_bus_protocols_t    *bus     = qspi ? dgx_qspi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 40, 39, 47, 45, 40 * 1000 * 1000, 0, &options)
                                           : dgx_spi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, 13, 12, 14, 15, 2, 40 * 1000 * 1000, 0, &options);
    CHECK(bus != NULL);
    if (!bus) return;
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, 16);
    uint8_t *frame = malloc(PW * PH * 2);
    for (int i = 0; i < PW * PH * 2; ++i) frame[i] = (uint8_t)rnd(0, 255);
    for (long refused = 1; refused <= 3; ++refused) {
        queue_calls = 0, refused_call = refused;
        send_window(&s.scr, frame, PW, PH);
        CHECK(queue_calls > refused);
        refused_call = 0;
        send_window(&s.scr, frame, PW, PH);
        CHECK(window_wrong(frame, PW, PH) == 0);
    }
    free(frame);
    dgx_screen_t  *picture = dgx_vscreen_init(PW, PH, 16, DgxScreenRGB);
    dgx_screen_t  *model   = dgx_vscreen_init(PW, PH, 16, DgxScreenRGB);
    const uint32_t turned  = DGX_VSCREEN_ORIENTED(DgxOutputMirrorX);
    for (int y = 0; y < PH; ++y) {
        for (int x = 0; x < PW; ++x) dgx_set_pixel(picture, x, y, rnd_color(16));
    }
    dgx_vscreen_to_screen(model, 0, 0, picture, turned);
    for (long refused = 2; refused <= 4; ++refused) {
        queue_calls = 0, refused_call = refused;
        dgx_vscreen_to_screen(&s.scr, 0, 0, picture, turned);
        s.scr.wait_buffer(&s.scr);
        CHECK(queue_calls > refused);
        refused_call = 0;
        dgx_vscreen_to_screen(&s.scr, 0, 0, picture, turned);
        s.scr.wait_buffer(&s.scr);
        CHECK(panel_wrong(model) == 0);
    }
    dgx_screen_destroy(&picture);
    dgx_screen_destroy(&model);
    bus->dispose(bus);
}
#endif

/*
 * Where the bus sends from in place: internal memory from a whole word, and
 * PSRAM the DMA reaches when the driver has SPI_TRANS_DMA_USE_PSRAM; before
 * IDF 5.5 pixels in PSRAM go through the buffer of the bus.
 */
static void test_sends_from(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, 13, 12, 14, 15, 2, 40 * 1000 * 1000, 0);
    CHECK(bus && bus->sends_from);
    if (!bus) return;
    static uint32_t words[4];
    CHECK(bus->sends_from(bus, words));
    CHECK(!bus->sends_from(bus, (uint8_t *)words + 2));
#ifdef TEST_SPI_IDF54
    CHECK(!bus->sends_from(bus, psram));
#else
    CHECK(bus->sends_from(bus, psram));
#endif
    bus->dispose(bus);
}

/* an SPI display and a QSPI one on a host DGX sets up: one host, freed with the second */
/*
 * A display on a host of the program holds the bus from a window to the end
 * of the drawing. A pixel, a line and a small rectangle go by the registers,
 * the driver sending none of their transfers, and the bus is let go of after
 * each; a batch holds it once for all it draws. A device of the program on
 * the host sends between drawings. A rectangle too long to poll lets go of
 * the bus before its pixels are queued, and they go by DMA as before.
 */
static void test_hold(void)
{
    panel_reset(2);
    dc_pin                     = 2;
    const spi_bus_config_t hcfg = {.mosi_io_num = 13, .miso_io_num = 12, .sclk_io_num = 14, .quadwp_io_num = -1, .quadhd_io_num = -1};
    CHECK(spi_bus_initialize(SPI3_HOST, &hcfg, SPI_DMA_CH_AUTO) == ESP_OK);
    dgx_bus_protocols_t *bus = dgx_spi_init_on_bus(SPI3_HOST, 15, 2, 40 * 1000 * 1000, 0);
    CHECK(bus != NULL);
    if (!bus) return;
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, 16);
    dgx_screen_t                 *lcd       = &s.scr;
    dgx_screen_t                 *model     = dgx_vscreen_init(PW, PH, 16, DgxScreenRGB);
    const spi_device_interface_config_t cfg = {.clock_speed_hz = 1000 * 1000, .spics_io_num = 5, .queue_size = 2};
    CHECK(spi_bus_add_device(SPI3_HOST, &cfg, &model_other) == ESP_OK);
    spi_transaction_t ping = {.length = 8, .flags = SPI_TRANS_USE_TXDATA};
    const long        t0 = transfers, d0 = direct_transfers, a0 = acquires, o0 = other_transfers, q0 = queued_while_held;

    dgx_set_pixel(lcd, 3, 4, 0x1234), dgx_set_pixel(model, 3, 4, 0x1234);
    CHECK(transfers == t0 && direct_transfers > d0 && acquires == a0 + 1);
    CHECK(model_hosts[SPI3_HOST].holder == NULL);
    CHECK(spi_device_polling_transmit(model_other, &ping) == ESP_OK);
    dgx_draw_line(lcd, 0, 0, 20, 9, 0x5678), dgx_draw_line(model, 0, 0, 20, 9, 0x5678);
    dgx_fill_rectangle(lcd, 30, 30, 5, 3, 0x9abc), dgx_fill_rectangle(model, 30, 30, 5, 3, 0x9abc);
    CHECK(transfers == t0 && acquires == a0 + 3); /* the pings are counted apart */
    CHECK(model_hosts[SPI3_HOST].holder == NULL);
    CHECK(spi_device_polling_transmit(model_other, &ping) == ESP_OK);

    /* a batch holds the bus once, and lets go of it at its end */
    dgx_screen_progress_up(lcd);
    for (int i = 0; i < 40; ++i) dgx_set_pixel(lcd, i, 40, (uint32_t)i * 1000u), dgx_set_pixel(model, i, 40, (uint32_t)i * 1000u);
    dgx_draw_circle(lcd, 40, 20, 7, 0xffff), dgx_draw_circle(model, 40, 20, 7, 0xffff);
    CHECK(model_hosts[SPI3_HOST].holder != NULL && acquires == a0 + 4);
    dgx_screen_progress_down(lcd);
    CHECK(model_hosts[SPI3_HOST].holder == NULL && acquires == a0 + 4);
    CHECK(spi_device_polling_transmit(model_other, &ping) == ESP_OK);

    /* a long rectangle: its window by the registers, its pixels by DMA with the bus let go of */
    const long t1 = transfers;
    dgx_fill_rectangle(lcd, 0, 0, PW, PH / 2, 0x0f0f), dgx_fill_rectangle(model, 0, 0, PW, PH / 2, 0x0f0f);
    lcd->wait_buffer(lcd);
    CHECK(transfers > t1 && queued_while_held == q0 && model_hosts[SPI3_HOST].holder == NULL);
    CHECK(spi_device_polling_transmit(model_other, &ping) == ESP_OK);
    CHECK(other_transfers == o0 + 4);

    CHECK(panel_wrong(model) == 0);
    CHECK(stray_bytes == 0 && unknown_commands == 0);
    CHECK(spi_bus_remove_device(model_other) == ESP_OK);
    model_other = NULL;
    dgx_screen_destroy(&model);
    bus->dispose(bus);
    CHECK(spi_bus_free(SPI3_HOST) == ESP_OK);
}

/*
 * The only display on a host DGX has set up keeps the bus between its
 * drawings: acquired once, and its long pixels queued while it holds it. A
 * second display that joins the host takes it from the first, and the two
 * take turns at every drawing; when the second goes, the first keeps the
 * bus again from its next drawing.
 */
static void test_keep(void)
{
    panel_reset(2);
    dc_pin                   = 2;
    dgx_bus_protocols_t *bus = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, 13, 12, 14, 15, 2, 40 * 1000 * 1000, 0);
    CHECK(bus != NULL);
    if (!bus) return;
    static dgx_screen_with_bus_t s;
    screen_on(&s, bus, 16);
    dgx_screen_t *lcd   = &s.scr;
    dgx_screen_t *model = dgx_vscreen_init(PW, PH, 16, DgxScreenRGB);
    const long    a0 = acquires, q0 = queued_while_held;
    for (int i = 0; i < 20; ++i) dgx_set_pixel(lcd, i, 3, 0x1111), dgx_set_pixel(model, i, 3, 0x1111);
    dgx_draw_line(lcd, 0, 0, 30, 20, 0x2222), dgx_draw_line(model, 0, 0, 30, 20, 0x2222);
    dgx_fill_rectangle(lcd, 0, 24, PW, PH - 24, 0x3333), dgx_fill_rectangle(model, 0, 24, PW, PH - 24, 0x3333);
    dgx_draw_circle(lcd, 30, 30, 9, 0x4444), dgx_draw_circle(model, 30, 30, 9, 0x4444);
    lcd->wait_buffer(lcd);
    CHECK(acquires == a0 + 1 && model_hosts[SPI2_HOST].holder != NULL && queued_while_held > q0);
    CHECK(panel_wrong(model) == 0);

    /* a second display on the host: the first lets go of the bus, and then takes it for each drawing */
    dgx_bus_protocols_t *second = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, 13, 12, 14, 16, 2, 40 * 1000 * 1000, 0);
    CHECK(second != NULL && model_hosts[SPI2_HOST].holder == NULL);
    const long a1 = acquires;
    dgx_set_pixel(lcd, 50, 40, 0x5555), dgx_set_pixel(model, 50, 40, 0x5555);
    dgx_set_pixel(lcd, 51, 40, 0x5555), dgx_set_pixel(model, 51, 40, 0x5555);
    CHECK(acquires == a1 + 2 && model_hosts[SPI2_HOST].holder == NULL);
    if (second) second->dispose(second);
    dgx_set_pixel(lcd, 52, 40, 0x6666), dgx_set_pixel(model, 52, 40, 0x6666);
    dgx_set_pixel(lcd, 53, 40, 0x6666), dgx_set_pixel(model, 53, 40, 0x6666);
    CHECK(acquires == a1 + 3 && model_hosts[SPI2_HOST].holder != NULL);
    CHECK(panel_wrong(model) == 0);
    CHECK(stray_bytes == 0 && unknown_commands == 0);
    dgx_screen_destroy(&model);
    bus->dispose(bus);
    CHECK(model_hosts[SPI2_HOST].holder == NULL && !model_hosts[SPI2_HOST].initialized);
}

static void test_shared_host(void)
{
    dgx_bus_protocols_t *q = dgx_qspi_init(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 40, 39, 47, 45, 40 * 1000 * 1000, 0);
    dgx_bus_protocols_t *s = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 47, 10, 2, 40 * 1000 * 1000, 0);
    CHECK(q && s && model_hosts[SPI2_HOST].devices == 2);
    /* a QSPI display does not go on a host of one data line, nor on other lines */
    CHECK(dgx_qspi_init(SPI2_HOST, SPI_DMA_CH_AUTO, 21, 48, 40, 38, 47, 11, 40 * 1000 * 1000, 0) == NULL);
    if (q) q->dispose(q);
    CHECK(model_hosts[SPI2_HOST].initialized);
    if (s) s->dispose(s);
    CHECK(!model_hosts[SPI2_HOST].initialized && model_hosts[SPI2_HOST].devices == 0);
}

int main(void)
{
    test_spi(16, 0, 2);
    test_spi(24, 0, 2);
    test_spi(16, 1000, 2);
    test_spi(16, 0, 40); /* a D/C line above GPIO 31 */
    test_qspi(16, 0);
    test_qspi(16, 1000);
    test_qspi(24, 1000);
    test_whole_frame(false);
    test_whole_frame(true);
    test_short_buffer(false);
    test_short_buffer(true);
    test_small_host_max(false);
    test_small_host_max(true);
    test_wait_after_window();
#ifndef TEST_SPI_IDF54
    test_psram_pieces(false);
    test_psram_pieces(true);
    test_psram_late(false);
    test_psram_late(true);
#if SOC_AHB_GDMA_VERSION != 1
    flash_encrypted = true;
    test_psram_pieces(false);
    test_psram_pieces(true);
    flash_encrypted = false;
#endif
#endif
#ifdef NDEBUG
    test_refused(false);
    test_refused(true);
#endif
    test_sends_from();
    test_shared_host();
    test_hold();
    test_keep();
    CHECK_DONE();
}
