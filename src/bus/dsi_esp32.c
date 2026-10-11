/*
 * dsi_esp32.c
 *
 *  Created on: 11.10.2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  MIPI-DSI bus on the esp_lcd driver. The display is in video mode: the
 *  chip sends it a frame buffer over and over. The bus takes the commands
 *  of the window and the pixels itself and puts them into that buffer.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_attr.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "bus/dgx_dsi_esp32.h"
#include "dgx_fb_area_priv.h"
#include "dgx_fb_tiles_priv.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"

#define DGX_DSI_BUFFER_LEN 4096

/*
 * The 2D DMA of the chip moves a rectangle from one picture into another
 * past the processor and its cache, twice as fast as the display reads, and
 * can exchange the two bytes of a 16-bit pixel on the way. Only the private
 * interface of IDF gives that: the public one, esp_async_color_convert.h of
 * IDF 6.0.3 and 6.1, copies and converts colors but does not exchange bytes.
 */
#if defined(__has_include) && SOC_DMA2D_SUPPORTED && CONFIG_DGX_DSI_HARDWARE_COPY
#if __has_include("esp_private/dma2d.h")
#include "esp_private/dma2d.h"
#include "hal/dma2d_types.h"
#include "soc/dma2d_channel.h"
#include "soc/dma2d_struct.h"
#define DGX_DSI_HARDWARE_COPY 1
#endif
#endif

/*
 * The pixel processing accelerator of the chip fills a rectangle of a
 * picture with a color past the processor and its cache, four times as fast.
 * This one has a public interface.
 */
#if defined(__has_include) && SOC_PPA_SUPPORTED && CONFIG_DGX_DSI_HARDWARE_COPY
#if __has_include("driver/ppa.h")
#include "driver/ppa.h"
#define DGX_DSI_HARDWARE_FILL 1
#endif
#endif

/*
 * The driver of the display tells where it takes the frame buffer for the
 * next frame by on_frame_buf_complete, on every chip: IDF 5.5.5, 6.0.3 and
 * 6.1 have it, the release branches got it at different times. So it is
 * told by the header and not by the number of the IDF: CMakeLists.txt looks
 * for the name there and defines DGX_DSI_FRAME_BUF_COMPLETE.
 */

/*
 * The end of a frame is told where the display takes the frame buffer for
 * the next one: where the last row has been read. With the older name,
 * on_refresh_done, a chip of revision 3 and later tells it at the vertical
 * sync that follows, which is later.
 */
#if DGX_DSI_FRAME_BUF_COMPLETE || !(defined(CONFIG_ESP32P4_REV_MIN_FULL) && CONFIG_ESP32P4_REV_MIN_FULL >= 300)
#define DGX_DSI_REFRESH_AT_SWITCH 1
#endif

/*
 * What is written to the frame buffer lies in the cache of the chip until it
 * is brought out. Past a part of the size of that cache the chip starts to
 * put lines out on its own, in an order of its own, and a long drawing shows
 * with holes that fill later. So when pixels and rectangles are drawn, the
 * bus brings out what was written before it comes to that, and the drawing
 * shows in the order it is made. Pixels sent through the window go one
 * after another in memory and leave the cache in that order by themselves;
 * bringing them out by force only costs time, a tenth on a whole screen.
 */
#ifdef CONFIG_CACHE_L2_CACHE_SIZE
#define DGX_DSI_SHOW_WEIGHT (CONFIG_CACHE_L2_CACHE_SIZE / 4)
#else
#define DGX_DSI_SHOW_WEIGHT (32 * 1024)
#endif

typedef struct _dgx_dsi_bus_t dgx_dsi_bus_t;
#if DGX_DSI_HARDWARE_COPY || DGX_DSI_HARDWARE_FILL
static bool dgx_dsi_wait_scan_outside(dgx_dsi_bus_t *bus, uint32_t first, uint32_t last);
#endif

struct _dgx_dsi_bus_t {
    dgx_bus_protocols_t       protocols;
    esp_lcd_dsi_bus_handle_t  dsi;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
    esp_ldo_channel_handle_t  phy_power;
    dgx_fb_area_t             area; /* area.fb is NULL until dgx_dsi_video_setup(); the frame buffer drawing goes into */
    uint8_t                   data_lanes;
    /*
     * With two frame buffers drawing goes into the one the display is not
     * reading. A batch of drawing ends with the display told to read that
     * one from the next frame on; when it does, the other one is free and
     * becomes the one drawing goes into. It lacks what the last batch drew:
     * `tiles` keeps where that is, and it is taken from the frame buffer
     * the display reads tile by tile, when a tile is drawn in or read, and
     * the rest before the next change.
     */
    uint8_t          *frame[2];
    dgx_fb_tiles_t    tiles;        /* the maps are there with two frame buffers only */
#if DGX_DSI_HARDWARE_COPY
    /* the 2D DMA: a place among those who use it, two descriptors of a picture one after another, a transfer, and the word that it is done */
    dma2d_pool_handle_t dma_pool; /* NULL when there is none: the processor copies then */
    dma2d_descriptor_t *dma_desc;
    dma2d_trans_t      *dma_trans;
    SemaphoreHandle_t   dma_done;
    bool                dma_exchange; /* the transfer under way exchanges the bytes of 16-bit pixels */
#endif
#if DGX_DSI_HARDWARE_FILL
    ppa_client_handle_t filler; /* the accelerator that fills rectangles; NULL when there is none: the processor fills then */
#endif
    bool              rows_written; /* pixels were put into the frame buffer past the processor: there is something to show */
    bool              turned;       /* the screen does not lie on the panel pixel for pixel: mirrored, or its axes swapped */
    bool              window_open;  /* pixels are being sent into the window from its first one on: its tiles wait to be told how far they went */
    uint32_t          window_got;   /* how many came */
    uint8_t           frames;       /* 1 or 2 */
    bool              after_scan;   /* one frame buffer, a batch brought out behind the scan of the display */
    bool              batch_seen;   /* a part of the batch being drawn is on the display already: holding the rest back would split it */
    uint16_t          lines_total;  /* rows of a frame with the blanking */
    uint16_t          lines_before; /* rows of blanking between the end of a frame, where it is counted, and its first row */
    uint8_t           drawn;        /* which of them drawing goes into */
    bool              flip_pending; /* the display was told to show `drawn` and may not be reading it yet */
    uint32_t          flip_at;      /* the count of refreshes from which it surely does */
    SemaphoreHandle_t refreshed;
    volatile uint32_t refreshes;         /* frames the display has been sent, counted where a frame ends */
    volatile uint32_t refresh_us;        /* when the last one ended, the low 32 bits of the time: read whole from a task */
    volatile uint32_t refresh_period_us; /* and how long it took */
    /*
     * A DSI packet carries a command together with its parameters, so a
     * command is held back until its data comes, or until it is known that
     * there is none: another command, or sync_write().
     */
    uint8_t command;
    bool    command_pending;
};

static const char TAG[] = "DGX DSI BUS";

static void dgx_dsi_send_pending(dgx_dsi_bus_t *bus)
{
    if (!bus->command_pending) return;
    bus->command_pending = false;
    esp_err_t ret        = esp_lcd_panel_io_tx_param(bus->io, bus->command, NULL, 0);
    if (ret != ESP_OK) ESP_LOGE(TAG, "command 0x%02x failed: %s", bus->command, esp_err_to_name(ret));
}

/*
 * The tiles the panel is cut into to tell what one of two frame buffers
 * lacks: 32 pixels wide and 8 high. A row of tiles is a word of each of the
 * two maps, so a panel 600 rows high takes 600 bytes for them. Measured on
 * an ESP32-P4 against tiles 32 rows high, which take 152 bytes: a band of
 * 200 rows sent every frame is 1 ms faster, pixels scattered over the
 * screen twice as fast, the rest within a tenth.
 */
#ifndef DGX_DSI_TILE_SHIFT_X
#define DGX_DSI_TILE_SHIFT_X 5
#endif
#ifndef DGX_DSI_TILE_SHIFT_Y
#define DGX_DSI_TILE_SHIFT_Y 3
#endif

/* how close to the end of a frame the display is told to change the frame buffer and may not do it at once */
#define DGX_DSI_FLIP_GUARD_US 2000

/*
 * A rectangle goes to the 2D DMA when it is large enough. Measured on an
 * ESP32-P4 with the display running: the DMA takes 7 us a KB and about
 * 0.2 ms to be asked, with the cache of the rows it writes taken out and
 * the wait for it; the processor 1.6 us a row and 1.4 us for every 32
 * pixels of it. A rectangle of 64x48 is still faster by the processor, one
 * of 320x100 or a bar of 40x548 twice as fast by the DMA.
 */
#define DGX_DSI_HARDWARE_COPY_FROM 8000u

/*
 * A rectangle is filled by the accelerator when rows + bytes / 64 is this
 * or more. Measured on an ESP32-P4: the accelerator takes 57 us to be asked,
 * 0.2 us a row and 3 us a KB, the processor 0.9 us a row and 14 us a KB.
 * They meet at about 80; more is taken, for what the accelerator has filled
 * is not in the cache, and what is drawn over it next has to fetch it.
 * A square of 48x48 stays with the processor, one of 100x100 and a strip of
 * 4x548 go to the accelerator.
 */
#define DGX_DSI_HARDWARE_FILL_FROM 200u

/*
 * Rows of pixels from a program go to the 2D DMA from this many bytes on:
 * through the window the processor moves about 36 KB in a millisecond, so
 * less than this it has moved before the DMA is asked.
 */
#define DGX_DSI_HARDWARE_ROWS_FROM 16384u

/* a frame has been sent: the display starts the next one from the frame buffer it was last told to read */
static IRAM_ATTR bool dgx_dsi_on_refresh(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)user_ctx;
    uint32_t       now = (uint32_t)esp_timer_get_time();
    BaseType_t     woken = pdFALSE;
    bus->refresh_period_us = now - bus->refresh_us;
    bus->refresh_us        = now;
    ++bus->refreshes;
    xSemaphoreGiveFromISR(bus->refreshed, &woken);
    return woken == pdTRUE;
}

static void dgx_dsi_bring_out(uint8_t *from, size_t length)
{
    esp_cache_msync(from, length, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/*
 * Before anything is drawn: if the display was told to show the frame buffer
 * drawing went into, wait until it reads that one and draw into the other
 * one from now on.
 */
static void dgx_dsi_draw_ready(dgx_dsi_bus_t *bus)
{
    if (!bus->flip_pending) return;
    while ((int32_t)(bus->refreshes - bus->flip_at) < 0) {
        if (xSemaphoreTake(bus->refreshed, pdMS_TO_TICKS(200)) != pdTRUE) break; /* no frames are sent: nothing to wait for */
    }
    bus->flip_pending = false;
    bus->drawn ^= 1u;
    bus->area.fb = bus->frame[bus->drawn];
}

/* the frame buffer the display reads, or is about to: it has what the one drawn into lacks */
static inline const uint8_t *dgx_dsi_other(const dgx_dsi_bus_t *bus)
{
    return bus->frame[bus->drawn ^ 1u];
}

/*
 * Pixels were being sent into the window, and now something else comes. If
 * they did not fill it, what they did not reach is as old as the frame
 * buffer was: it is taken from the other one. The pixels came row after row
 * of the screen, so that is the rest of a row and the rows after it.
 */
static void dgx_dsi_window_close(dgx_dsi_bus_t *bus)
{
    if (!bus->window_open) return;
    bus->window_open = false;
    const dgx_fb_area_t *area  = &bus->area;
    const uint32_t       width = (uint32_t)area->right - area->left + 1u, height = (uint32_t)area->bottom - area->top + 1u;
    uint16_t             x0, y0, x1, y1, px0, py0, px1, py1;
    dgx_fb_area_to_panel(area, area->left, area->right, area->top, area->bottom, &x0, &y0, &x1, &y1);
    uint32_t rows = bus->window_got / width, rest = bus->window_got % width;
    if (rows < height) {
        if (rest) {
            dgx_fb_area_to_panel(area, (int)(area->left + rest), area->right, (int)(area->top + rows), (int)(area->top + rows), &px0, &py0, &px1, &py1);
            dgx_fb_tiles_unwritten(&bus->tiles, area->fb, dgx_dsi_other(bus), x0, y0, x1, y1, px0, py0, px1, py1);
            ++rows;
        }
        if (rows < height) {
            dgx_fb_area_to_panel(area, area->left, area->right, (int)(area->top + rows), area->bottom, &px0, &py0, &px1, &py1);
            dgx_fb_tiles_unwritten(&bus->tiles, area->fb, dgx_dsi_other(bus), x0, y0, x1, y1, px0, py0, px1, py1);
        }
    }
    dgx_fb_tiles_close(&bus->tiles, x0, y0, x1, y1);
}

/*
 * With two frame buffers, before a rectangle of the screen is written or
 * read: the frame buffer drawing goes into gets what it lacks there.
 * `whole` tells that every pixel of the rectangle is about to be written.
 */
static void dgx_dsi_tiles_ready(dgx_dsi_bus_t *bus, int left, int right, int top, int bottom, bool whole, bool drawn)
{
    uint16_t x0, y0, x1, y1;
    dgx_dsi_window_close(bus);
    dgx_fb_area_to_panel(&bus->area, left, right, top, bottom, &x0, &y0, &x1, &y1);
    dgx_fb_tiles_ready(&bus->tiles, bus->area.fb, dgx_dsi_other(bus), x0, y0, x1, y1, whole, drawn);
}

/* a batch has been drawn into the frame buffer the display is not reading: show it from the next frame on */
static void dgx_dsi_flip(dgx_dsi_bus_t *bus)
{
    /* a frame buffer of its own given to the panel is not copied: the panel reads it from the next frame on */
    esp_lcd_panel_draw_bitmap(bus->panel, 0, 0, bus->area.fb_width, 1, bus->area.fb);
#if DGX_DSI_REFRESH_AT_SWITCH
    /*
     * The display takes the new frame buffer where a frame ends, and that
     * is where the ends of frames are counted: the next one counted is it.
     * One counted between the telling and this line makes it wait a frame
     * more than needed, which does no harm.
     */
    bus->flip_at = bus->refreshes + 1u;
#else
    /*
     * The display takes the new frame buffer where a frame ends, a little
     * before that is counted. Told so just before that moment, it may have
     * started the next frame from the old one already: then it is the
     * frame after the next.
     */
    uint32_t period = bus->refresh_period_us;
    uint32_t since  = (uint32_t)esp_timer_get_time() - bus->refresh_us;
    bool     late   = period == 0 || since + DGX_DSI_FLIP_GUARD_US > period;
    bus->flip_at    = bus->refreshes + (late ? 2u : 1u);
#endif
    bus->flip_pending = true;
}

#if DGX_DSI_HARDWARE_COPY
/* the 2D DMA has finished a transfer */
static IRAM_ATTR bool dgx_dsi_dma_done(dma2d_channel_handle_t channel, dma2d_event_data_t *event, void *user)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(((dgx_dsi_bus_t *)user)->dma_done, &woken);
    return woken == pdTRUE;
}

/* the 2D DMA has channels for the transfer: set them up and start it */
static IRAM_ATTR bool dgx_dsi_dma_picked(uint32_t count, const dma2d_trans_channel_info_t *channels, void *user)
{
    dgx_dsi_bus_t         *bus = (dgx_dsi_bus_t *)user;
    dma2d_channel_handle_t tx = NULL, rx = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (channels[i].dir == DMA2D_CHANNEL_DIRECTION_TX) tx = channels[i].chan;
        if (channels[i].dir == DMA2D_CHANNEL_DIRECTION_RX) rx = channels[i].chan;
    }
    dma2d_trigger_t memory = {.periph = DMA2D_TRIG_PERIPH_M2M, .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_TX};
    dma2d_connect(tx, &memory);
    memory.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX;
    dma2d_connect(rx, &memory);
    /*
     * The channels are left with the bursts they are given when connected,
     * 128 bytes: the structure that sets them is not the same in every IDF.
     */
    if (bus->dma_exchange) {
        /* of the three bytes the DMA may put in another order a 16-bit pixel has the lower two: they change places */
        dma2d_csc_config_t exchange = {
            .tx_csc_option = DMA2D_CSC_TX_SCRAMBLE,
            .pre_scramble  = DMA2D_SCRAMBLE_ORDER_BYTE2_0_1,
        };
        dma2d_configure_color_space_conversion(tx, &exchange);
    }
    dma2d_rx_event_callbacks_t callbacks = {.on_recv_eof = dgx_dsi_dma_done};
    dma2d_register_rx_event_callbacks(rx, &callbacks, bus);
    dma2d_set_desc_addr(tx, (intptr_t)&bus->dma_desc[0]);
    dma2d_set_desc_addr(rx, (intptr_t)&bus->dma_desc[1]);
    if (bus->dma_exchange) {
        /*
         * The chip has two ways to take pixels whose bytes it puts in
         * another order: 2, "other 2 byte/pixel type", and 3, the same for
         * 3 bytes. IDF 5.5.4 sets 2, IDF 6.1 sets 3 and says that only
         * pixels of 3 bytes are supported; with 2-byte pixels the transfer
         * never ends then (esp-idf issue 19226). So the field is set here,
         * in the channel that was given the descriptor: the driver does not
         * tell its number.
         */
        for (size_t i = 0; i < sizeof(DMA2D.out_channel) / sizeof(DMA2D.out_channel[0]); ++i) {
            if (DMA2D.out_channel[i].out_link_addr.outlink_addr_chn == (uint32_t)(intptr_t)&bus->dma_desc[0]) {
                DMA2D.out_channel[i].out_color_convert.out_color_input_sel_chn = 2;
            }
        }
    }
    dma2d_start(tx);
    dma2d_start(rx);
    return false;
}

/* a picture that is `rows` rows of `stride` pixels from `first` on, and the rectangle `width` wide from its column `x` */
static void dgx_dsi_dma_describe(dma2d_descriptor_t *desc, const uint8_t *first, uint32_t stride, uint32_t x, uint32_t width, uint32_t rows, uint8_t pixel_bytes)
{
    memset(desc, 0, sizeof(*desc));
    desc->buffer    = (void *)first;
    desc->dma2d_en  = 1;
    desc->suc_eof   = 1;
    desc->ha_length = stride;
    desc->va_size   = rows;
    desc->hb_length = width;
    desc->vb_size   = rows;
    desc->x         = x;
    desc->pbyte     = pixel_bytes == 2 ? DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL : DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL;
    desc->mode      = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
    desc->owner     = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
}

/*
 * Move a rectangle of `width` by `rows` pixels by the 2D DMA and wait until
 * it is there. `from` and `to` are the first rows of the rectangle in their
 * pictures, rows of `from_stride` pixels and of the width of the panel, and
 * the rectangle starts at the columns `from_x` and `to_x` of them.
 *
 * The DMA reads and writes memory itself, so what the cache holds of the
 * source is brought out first, and what it holds of the place written is
 * brought out and forgotten: what is read there afterwards comes from
 * memory.
 *
 * `behind_scan` makes it start when the display is not reading the rows:
 * the DMA writes faster than the display reads and stays ahead of it.
 */
static bool dgx_dsi_dma(dgx_dsi_bus_t *bus, const uint8_t *from, uint32_t from_stride, uint32_t from_x, uint8_t *to, uint32_t to_x, uint32_t width,
                        uint32_t rows, bool exchange, bool behind_scan)
{
    const uint32_t bytes   = bus->area.pixel_bytes;
    const size_t   to_pitch = (size_t)bus->area.fb_width * bytes;
    esp_cache_msync((void *)(from + (size_t)from_x * bytes), (size_t)(rows - 1u) * from_stride * bytes + (size_t)width * bytes,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(to + (size_t)to_x * bytes, (size_t)(rows - 1u) * to_pitch + (size_t)width * bytes,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    dgx_dsi_dma_describe(&bus->dma_desc[0], from, from_stride, from_x, width, rows, (uint8_t)bytes);
    dgx_dsi_dma_describe(&bus->dma_desc[1], to, bus->area.fb_width, to_x, width, rows, (uint8_t)bytes);
    esp_cache_msync(bus->dma_desc, 2u * sizeof(dma2d_descriptor_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    bus->dma_exchange = exchange;
    if (behind_scan) {
        const uint32_t first = (uint32_t)((size_t)(to - bus->area.fb) / to_pitch);
        dgx_dsi_wait_scan_outside(bus, first, first + rows - 1u);
    }
    static const dma2d_trans_config_t transfer = {.tx_channel_num = 1, .rx_channel_num = 1, .on_job_picked = dgx_dsi_dma_picked};
    dma2d_trans_config_t              config   = transfer;
    config.channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING | (exchange ? DMA2D_CHANNEL_FUNCTION_FLAG_TX_CSC : 0u);
    config.user_config   = bus;
    xSemaphoreTake(bus->dma_done, 0); /* nothing is left over from a transfer that was given up */
    if (dma2d_enqueue(bus->dma_pool, &config, bus->dma_trans) != ESP_OK) return false;
    xSemaphoreTake(bus->dma_done, portMAX_DELAY);
    return true;
}

/* what is left to copy from one frame buffer into the other before they change places */
static bool dgx_dsi_hardware_copy(void *context, uint8_t *to, const uint8_t *from, uint32_t left, uint32_t top, uint32_t width, uint32_t rows)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)context;
    if (rows * (width + 53u) < DGX_DSI_HARDWARE_COPY_FROM) return false;
    const size_t pitch = (size_t)bus->area.fb_width * bus->area.pixel_bytes;
    return dgx_dsi_dma(bus, from + (size_t)top * pitch, bus->area.fb_width, left, to + (size_t)top * pitch, left, width, rows, false, false);
}

static void dgx_dsi_dma_free(dgx_dsi_bus_t *bus)
{
    if (bus->dma_pool) dma2d_release_pool(bus->dma_pool);
    if (bus->dma_done) vSemaphoreDelete(bus->dma_done);
    free(bus->dma_trans);
    free(bus->dma_desc);
    bus->dma_pool  = NULL;
    bus->dma_done  = NULL;
    bus->dma_trans = NULL;
    bus->dma_desc  = NULL;
}

/* take a place among the users of the 2D DMA; without it the processor does the work */
static void dgx_dsi_dma_init(dgx_dsi_bus_t *bus)
{
    const dma2d_pool_config_t pool_config = {0};
    /* the descriptors are read by the DMA: memory it can reach, on a boundary of a line of the cache */
    bus->dma_desc  = (dma2d_descriptor_t *)heap_caps_aligned_calloc(64, 1, 64, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    bus->dma_trans = (dma2d_trans_t *)heap_caps_calloc(1, dma2d_get_trans_elm_size(), MALLOC_CAP_INTERNAL);
    bus->dma_done  = xSemaphoreCreateBinary();
    if (!bus->dma_desc || !bus->dma_trans || !bus->dma_done || dma2d_acquire_pool(&pool_config, &bus->dma_pool) != ESP_OK) {
        ESP_LOGW(TAG, "no 2D DMA: the processor copies");
        bus->dma_pool = NULL;
        dgx_dsi_dma_free(bus);
    }
}
#endif

/*
 * Measured on an ESP32-P4: what the cache brings out in a millisecond and
 * what a window takes in from a buffer in one, in KB, with room; and how far
 * ahead of the row it shows the display reads memory (it takes the first
 * rows of a frame as soon as the frame before has ended).
 */
#define DGX_DSI_OUT_KB_A_MS  256u
#define DGX_DSI_IN_KB_A_MS   28u
#define DGX_DSI_SCAN_LEAD    6u
#define DGX_DSI_SCAN_ROOM_US 150u

/*
 * Wait, if needed, for the moment the rows `first`..`last` of the frame
 * buffer can be changed for `takes` microseconds without the display
 * reading them meanwhile. The display reads the frame buffer row after row,
 * a frame in the time between two ends of a frame; where it is now follows
 * from the time since the last one. The best moment is when it has just left
 * the rows: then it is a whole frame away from them.
 */
static bool dgx_dsi_wait_scan(dgx_dsi_bus_t *bus, uint32_t first, uint32_t last, uint32_t takes)
{
    uint32_t period = bus->refresh_period_us;
    uint32_t since  = (uint32_t)esp_timer_get_time() - bus->refresh_us;
    if (period < 1000u || period > 100000u || since > 2u * period) return false; /* no frames counted: nothing to go by */
    if (since >= period) since -= period;                                  /* the end of a frame is being counted right now */
    const uint64_t line   = ((uint64_t)period << 16) / bus->lines_total; /* a row in 1/65536 us */
    const uint32_t before = bus->lines_before + first;
    /* the scan is in the rows from `enter` to `leave`, counted from the end of the last frame */
    const uint32_t enter = first < DGX_DSI_SCAN_LEAD ? 0u : (uint32_t)(((before - DGX_DSI_SCAN_LEAD) * line) >> 16);
    const uint32_t leave = (uint32_t)(((bus->lines_before + last + 1u) * line) >> 16);
    takes += DGX_DSI_SCAN_ROOM_US;
    if (takes >= period - (leave - enter)) return false;               /* too much for any moment */
    if (since + takes < enter) return true;                            /* done before the scan comes */
    if (since >= leave && since + takes < enter + period) return true; /* the scan has left and is not back in time */
    /* the scan is in the rows, or comes too soon: when it has left them, in this frame or in the next */
    uint32_t until = since < leave ? leave : leave + period;
    uint32_t wait  = until - since;
    uint32_t start = bus->refresh_us + since;
    if (wait > 2u * portTICK_PERIOD_MS * 1000u) vTaskDelay(wait / (portTICK_PERIOD_MS * 1000u) - 1u);
    while ((uint32_t)esp_timer_get_time() - start < wait) {
    }
    return true;
}

/* the rows of the frame buffer `length` bytes from `from` on lie in */
static void dgx_dsi_wait_scan_bytes(dgx_dsi_bus_t *bus, uint32_t from, uint32_t length)
{
    const uint32_t pitch = (uint32_t)bus->area.fb_width * bus->area.pixel_bytes;
    /* what is still in the cache is what takes time to bring out */
    uint32_t kept = length;
#ifdef CONFIG_CACHE_L2_CACHE_SIZE
    if (kept > (uint32_t)CONFIG_CACHE_L2_CACHE_SIZE) kept = (uint32_t)CONFIG_CACHE_L2_CACHE_SIZE;
#endif
    dgx_dsi_wait_scan(bus, from / pitch, (from + length - 1u) / pitch, kept / DGX_DSI_OUT_KB_A_MS * 1000u / 1024u);
}

/*
 * Pixels are about to be sent for the whole window: wait until the scan has
 * left its rows. The cache keeps only so much, and what it lets out during
 * the transfer is seen at once; sent behind the scan, it is seen whole in
 * the next frame all the same. A window that cannot be sent before the scan
 * is back goes as it comes, and the batch with it.
 */
static void dgx_dsi_wait_scan_window(dgx_dsi_bus_t *bus)
{
    uint16_t x0, y0, x1, y1;
    dgx_fb_area_to_panel(&bus->area, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, &x0, &y0, &x1, &y1);
    uint32_t bytes = (uint32_t)(x1 - x0 + 1u) * (uint32_t)(y1 - y0 + 1u) * bus->area.pixel_bytes;
    if (!dgx_dsi_wait_scan(bus, y0, y1, bytes / DGX_DSI_IN_KB_A_MS * 1000u / 1024u)) bus->batch_seen = true;
}

#if DGX_DSI_HARDWARE_COPY || DGX_DSI_HARDWARE_FILL
/* how long it may take from here to the first byte the 2D DMA or the accelerator writes */
#define DGX_DSI_DMA_START_US 400u

/*
 * Wait, if needed, until the display is not reading the rows `first`..`last`
 * and will not start to in the next moments. What is written from then on
 * faster than the display reads stays ahead of its scan: the rows are seen
 * whole in the frame that follows.
 */
static bool dgx_dsi_wait_scan_outside(dgx_dsi_bus_t *bus, uint32_t first, uint32_t last)
{
    uint32_t period = bus->refresh_period_us;
    uint32_t since  = (uint32_t)esp_timer_get_time() - bus->refresh_us;
    if (period < 1000u || period > 100000u || since > 2u * period) return false; /* no frames counted: nothing to go by */
    if (since >= period) since -= period;
    const uint64_t line   = ((uint64_t)period << 16) / bus->lines_total;
    const uint32_t before = bus->lines_before + first;
    uint32_t       enter  = first < DGX_DSI_SCAN_LEAD ? 0u : (uint32_t)(((before - DGX_DSI_SCAN_LEAD) * line) >> 16);
    const uint32_t leave  = (uint32_t)(((bus->lines_before + last + 1u) * line) >> 16);
    enter = enter > DGX_DSI_DMA_START_US ? enter - DGX_DSI_DMA_START_US : 0u;
    if (since < enter || since >= leave) return true;
    uint32_t wait  = leave - since;
    uint32_t start = bus->refresh_us + since;
    if (wait > 2u * portTICK_PERIOD_MS * 1000u) vTaskDelay(wait / (portTICK_PERIOD_MS * 1000u) - 1u);
    while ((uint32_t)esp_timer_get_time() - start < wait) {
    }
    return true;
}
#endif

#if DGX_DSI_HARDWARE_COPY
/*
 * Rows of pixels of a program, kept as DGX keeps them, straight into a
 * rectangle of the screen by the 2D DMA. It does what sending them through
 * the window does, several times as fast, when the pixels are 16-bit, the
 * screen lies on the panel as it is and the rectangle is not small.
 */
static bool dgx_dsi_write_rows(struct _dgx_bus_protocols_t *_bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *rows,
                               uint32_t pitch)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (!bus->area.fb || bus->area.pixel_bytes != 2 || bus->area.swap_rb || bus->turned || (pitch & 1u)) return false;
    if ((uint32_t)width * height * 2u < DGX_DSI_HARDWARE_ROWS_FROM) return false;
    if ((uint32_t)left + width > bus->area.fb_width || (uint32_t)top + height > bus->area.fb_height) return false;
    if (bus->frames == 2) {
        dgx_dsi_draw_ready(bus);
        /* every pixel of the rectangle is written: of its tiles only what lies outside it is brought up to date */
        dgx_dsi_tiles_ready(bus, left, left + width - 1, top, top + height - 1, true, true);
    }
    uint8_t *to = bus->area.fb + (size_t)top * bus->area.fb_width * 2u;
    if (!dgx_dsi_dma(bus, rows, pitch / 2u, 0, to, left, width, height, true, bus->after_scan)) return false;
    bus->rows_written = true;
    return true;
}
#endif

static inline void dgx_dsi_written(dgx_dsi_bus_t *bus)
{
    /* with two frame buffers what is drawn is not seen until it is shown: nothing to bring out on the way */
    if (bus->frames != 1 || bus->area.dirty_weight <= DGX_DSI_SHOW_WEIGHT) return;
    /*
     * More than the cache keeps to itself: it goes to the display as it is
     * drawn. Behind the scan that means the batch is not held back at its
     * end either: a part of it is seen, the rest must not come a frame
     * later.
     */
    uint32_t from, length;
    bus->batch_seen = true;
    if (dgx_fb_area_take_dirty(&bus->area, &from, &length)) dgx_dsi_bring_out(bus->area.fb + from, length);
}

static bool dgx_dsi_memory_command(const dgx_dsi_bus_t *bus)
{
    switch (bus->area.command) {
    case DGX_FB_AREA_WRITE:
    case DGX_FB_AREA_WRITE_CONTINUE:
    case DGX_FB_AREA_READ:
    case DGX_FB_AREA_READ_CONTINUE:
        return bus->area.fb != NULL;
    default:
        return false;
    }
}

static void dgx_dsi_send_command(struct _dgx_bus_protocols_t *_bus, uint8_t cmd)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    dgx_dsi_send_pending(bus);
    /* the window is about to change, or to be written from its first pixel again */
    if (bus->window_open && cmd != DGX_FB_AREA_WRITE_CONTINUE) dgx_dsi_window_close(bus);
    if (bus->area.fb && dgx_fb_area_command(&bus->area, cmd)) return;
    bus->command         = cmd;
    bus->command_pending = true;
}

static void dgx_dsi_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len)
{
    uint32_t byte_len = (len + 7u) / 8u;
    for (uint32_t i = 0; i < byte_len; i++) {
        dgx_dsi_send_command(_bus, cmds[i]);
    }
    dgx_dsi_send_pending((dgx_dsi_bus_t *)_bus);
}

static void dgx_dsi_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_dsi_bus_t *bus      = (dgx_dsi_bus_t *)_bus;
    uint32_t       byte_len = (len + 7u) / 8u;
    if (byte_len == 0) return;
    if (bus->area.fb) {
        if (bus->after_scan && bus->area.command == DGX_FB_AREA_WRITE && bus->area.x == bus->area.left && bus->area.y == bus->area.top) {
            dgx_dsi_wait_scan_window(bus);
        }
        if (bus->frames == 2 && (bus->area.command == DGX_FB_AREA_WRITE || bus->area.command == DGX_FB_AREA_WRITE_CONTINUE)) {
            dgx_dsi_draw_ready(bus);
            if (bus->window_open) {
                bus->window_got += byte_len / bus->area.pixel_bytes;
            } else if (bus->area.command == DGX_FB_AREA_WRITE && bus->area.x == bus->area.left && bus->area.y == bus->area.top) {
                /* pixels for the window from its first one on: most likely for all of it, which is known when they end */
                uint16_t x0, y0, x1, y1;
                dgx_fb_area_to_panel(&bus->area, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, &x0, &y0, &x1, &y1);
                dgx_fb_tiles_open(&bus->tiles, bus->area.fb, dgx_dsi_other(bus), x0, y0, x1, y1);
                bus->window_open = true;
                bus->window_got  = byte_len / bus->area.pixel_bytes;
            } else {
                dgx_dsi_tiles_ready(bus, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, false, true);
            }
        }
        if (dgx_fb_area_data(&bus->area, data, byte_len)) return;
    }
    bus->command_pending = false;
    esp_err_t ret        = esp_lcd_panel_io_tx_param(bus->io, bus->command, data, byte_len);
    if (ret != ESP_OK) ESP_LOGE(TAG, "command 0x%02x with %u bytes failed: %s", bus->command, (unsigned)byte_len, esp_err_to_name(ret));
}

static uint32_t dgx_dsi_read_data(struct _dgx_bus_protocols_t *_bus, uint8_t *data, uint32_t len)
{
    dgx_dsi_bus_t *bus      = (dgx_dsi_bus_t *)_bus;
    uint32_t       byte_len = (len + 7u) / 8u;
    if (byte_len == 0) return 0;
    if (dgx_dsi_memory_command(bus)) {
        uint32_t pixels = byte_len / bus->area.pixel_bytes;
        /* until the frame buffers have changed places the one at hand is the one that was drawn into, and lacks nothing */
        if (bus->frames == 2 && !bus->flip_pending) dgx_dsi_tiles_ready(bus, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, false, false);
        dgx_fb_area_read(&bus->area, data, pixels);
        return pixels * bus->area.pixel_bytes * 8u;
    }
    bus->command_pending = false;
    esp_err_t ret        = esp_lcd_panel_io_rx_param(bus->io, bus->command, data, byte_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reading 0x%02x failed: %s", bus->command, esp_err_to_name(ret));
        memset(data, 0, byte_len);
        return 0;
    }
    return len;
}

static void dgx_dsi_sync_write(struct _dgx_bus_protocols_t *_bus)
{
    dgx_dsi_send_pending((dgx_dsi_bus_t *)_bus);
}

static uint8_t *dgx_dsi_memory_allocator(uint32_t len)
{
    return (uint8_t *)malloc(len);
}

static void dgx_dsi_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (bus->panel) esp_lcd_panel_del(bus->panel);
    if (bus->refreshed) vSemaphoreDelete(bus->refreshed);
    if (bus->tiles.owed) dgx_fb_tiles_free(&bus->tiles);
#if DGX_DSI_HARDWARE_COPY
    dgx_dsi_dma_free(bus);
#endif
#if DGX_DSI_HARDWARE_FILL
    if (bus->filler) ppa_unregister_client(bus->filler);
#endif
    if (bus->io) esp_lcd_panel_io_del(bus->io);
    if (bus->dsi) esp_lcd_del_dsi_bus(bus->dsi);
    if (bus->phy_power) esp_ldo_release_channel(bus->phy_power);
    free(bus->protocols.buffer);
    free(bus);
}

dgx_bus_protocols_t *dgx_dsi_init(int bus_id, uint8_t data_lanes, uint32_t lane_bit_rate_mbps, int phy_ldo_chan, int phy_ldo_mv)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)calloc(1, sizeof(dgx_dsi_bus_t));
    if (!bus) {
        ESP_LOGE(TAG, "dgx_dsi_init failed to allocate dgx_dsi_bus_t");
        return NULL;
    }
    esp_err_t ret;
    if (phy_ldo_chan >= 0) {
        /* without it the PHY has no power and the bus cannot be made */
        esp_ldo_channel_config_t ldo_config = {
            .chan_id    = phy_ldo_chan,
            .voltage_mv = phy_ldo_mv,
        };
        ret = esp_ldo_acquire_channel(&ldo_config, &bus->phy_power);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ldo_acquire_channel failed: %s", esp_err_to_name(ret));
            dgx_dsi_dispose_bus_func(&bus->protocols);
            return NULL;
        }
    }
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id             = bus_id,
        .num_data_lanes     = data_lanes,
        .phy_clk_src        = 0, /* the default of the chip revision */
        .lane_bit_rate_mbps = lane_bit_rate_mbps,
    };
    ret = esp_lcd_new_dsi_bus(&bus_config, &bus->dsi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_dsi_bus failed: %s", esp_err_to_name(ret));
        dgx_dsi_dispose_bus_func(&bus->protocols);
        return NULL;
    }
    esp_lcd_dbi_io_config_t io_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(bus->dsi, &io_config, &bus->io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_dbi failed: %s", esp_err_to_name(ret));
        dgx_dsi_dispose_bus_func(&bus->protocols);
        return NULL;
    }
    bus->data_lanes                 = data_lanes;
    bus->protocols.bus_type         = DGX_BUS_DSI;
    bus->protocols.bus_name         = "DSI";
    bus->protocols.write_command    = dgx_dsi_send_command;
    bus->protocols.write_commands   = dgx_dsi_send_commands;
    bus->protocols.read_data        = dgx_dsi_read_data;
    bus->protocols.write_data       = dgx_dsi_send_data;
    bus->protocols.write_data_async = dgx_dsi_send_data; /* a copy into the frame buffer: done when it returns */
    bus->protocols.sync_write       = dgx_dsi_sync_write;
    bus->protocols.dispose          = dgx_dsi_dispose_bus_func;
    bus->protocols.memory_allocator = dgx_dsi_memory_allocator;
    bus->protocols.buffer_len       = DGX_DSI_BUFFER_LEN;
    bus->protocols.buffer           = bus->protocols.memory_allocator(bus->protocols.buffer_len);
    if (!bus->protocols.buffer) {
        ESP_LOGE(TAG, "dgx_dsi_init buffer allocation failed");
        dgx_dsi_dispose_bus_func(&bus->protocols);
        return NULL;
    }
    return (dgx_bus_protocols_t *)bus;
}

bool dgx_dsi_video_setup(dgx_bus_protocols_t *_bus, const dgx_dsi_video_t *video, uint8_t color_bits, bool swap_rb, dgx_dsi_frames_t frames)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (bus->panel) {
        ESP_LOGE(TAG, "the bus already has a display");
        return false;
    }
    if (color_bits != 16 && color_bits != 24) {
        ESP_LOGE(TAG, "%u bits a pixel: only 16 and 24 are supported", color_bits);
        return false;
    }
    if (frames != DgxDsiOneFrame && frames != DgxDsiTwoFrames && frames != DgxDsiOneFrameAfterScan) {
        ESP_LOGE(TAG, "%d is not a way to update the display", (int)frames);
        return false;
    }
    const bool two = frames == DgxDsiTwoFrames, after_scan = frames == DgxDsiOneFrameAfterScan;
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = (float)video->pixel_clock_khz / 1000.0f,
        .in_color_format    = color_bits == 16 ? LCD_COLOR_FMT_RGB565 : LCD_COLOR_FMT_RGB888,
        .num_fbs            = two ? 2 : 1,
        .video_timing = {
            .h_size            = video->width,
            .v_size            = video->height,
            .hsync_pulse_width = video->hsync_pulse_width,
            .hsync_back_porch  = video->hsync_back_porch,
            .hsync_front_porch = video->hsync_front_porch,
            .vsync_pulse_width = video->vsync_pulse_width,
            .vsync_back_porch  = video->vsync_back_porch,
            .vsync_front_porch = video->vsync_front_porch,
        },
    };
    esp_err_t ret = esp_lcd_new_panel_dpi(bus->dsi, &dpi_config, &bus->panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_dpi failed: %s", esp_err_to_name(ret));
        bus->panel = NULL;
        return false;
    }
    void *fb = NULL, *fb2 = NULL;
    ret      = two ? esp_lcd_dpi_panel_get_frame_buffer(bus->panel, 2, &fb, &fb2) : esp_lcd_dpi_panel_get_frame_buffer(bus->panel, 1, &fb);
    if (ret == ESP_OK && (two || after_scan)) {
        /* told where a frame ends: to know when the display has changed the frame buffer it reads, or where its scan is */
        bus->refreshed = xSemaphoreCreateBinary();
#if DGX_DSI_FRAME_BUF_COMPLETE
        esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_frame_buf_complete = dgx_dsi_on_refresh};
#else
        esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_refresh_done = dgx_dsi_on_refresh};
#endif
        ret = bus->refreshed ? esp_lcd_dpi_panel_register_event_callbacks(bus->panel, &callbacks, bus) : ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK && two && !dgx_fb_tiles_init(&bus->tiles, video->width, video->height, color_bits / 8u, DGX_DSI_TILE_SHIFT_X, DGX_DSI_TILE_SHIFT_Y)) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK || !fb || (two && !fb2)) {
        ESP_LOGE(TAG, "the frame buffers of the display: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(bus->panel);
        bus->panel = NULL;
        if (bus->refreshed) vSemaphoreDelete(bus->refreshed);
        bus->refreshed = NULL;
        if (bus->tiles.owed) dgx_fb_tiles_free(&bus->tiles);
        return false;
    }
    bus->frames      = two ? 2 : 1;
    bus->after_scan  = after_scan;
    bus->lines_total = (uint16_t)(video->height + video->vsync_front_porch + video->vsync_pulse_width + video->vsync_back_porch);
    /*
     * The end of a frame is counted where the last row has been read, on a
     * chip of revision 3 and later at the vertical sync that follows.
     */
#if DGX_DSI_REFRESH_AT_SWITCH
    bus->lines_before = (uint16_t)(video->vsync_front_porch + video->vsync_pulse_width + video->vsync_back_porch);
#else
    bus->lines_before = (uint16_t)(video->vsync_pulse_width + video->vsync_back_porch);
#endif
#if DGX_DSI_HARDWARE_COPY
    dgx_dsi_dma_init(bus);
    if (bus->dma_pool) {
        bus->protocols.write_rows = dgx_dsi_write_rows;
        if (two) {
            bus->tiles.copier         = dgx_dsi_hardware_copy;
            bus->tiles.copier_context = bus;
        }
    }
#endif
#if DGX_DSI_HARDWARE_FILL
    {
        const ppa_client_config_t filler_config = {.oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1};
        if (ppa_register_client(&filler_config, &bus->filler) != ESP_OK) {
            ESP_LOGW(TAG, "no accelerator to fill rectangles: the processor fills");
            bus->filler = NULL;
        }
    }
#endif
    bus->frame[0] = (uint8_t *)fb;
    bus->frame[1] = (uint8_t *)fb2;
    /* the display starts with the first one: with two, drawing goes into the second */
    bus->drawn = two ? 1 : 0;
    dgx_fb_area_init(&bus->area, bus->frame[bus->drawn], video->width, video->height, color_bits / 8u, swap_rb);
    return true;
}

bool dgx_dsi_video_start(dgx_bus_protocols_t *_bus)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (!bus->panel) {
        ESP_LOGE(TAG, "dgx_dsi_video_start before dgx_dsi_video_setup");
        return false;
    }
    dgx_dsi_send_pending(bus);
    esp_err_t ret = esp_lcd_panel_init(bus->panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

void dgx_dsi_orientation(dgx_bus_protocols_t *_bus, bool mirror_x, bool mirror_y, bool swap_xy)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    dgx_dsi_window_close(bus); /* where the window lies on the panel is told by the orientation it was sent in */
    dgx_fb_area_orientation(&bus->area, mirror_x, mirror_y, swap_xy);
    bus->turned = mirror_x || mirror_y || swap_xy;
}

void dgx_dsi_show(dgx_bus_protocols_t *_bus)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    uint32_t       from, length;
    const bool     drawn = dgx_fb_area_take_dirty(&bus->area, &from, &length);
    if (drawn) {
        if (bus->after_scan && !bus->batch_seen) dgx_dsi_wait_scan_bytes(bus, from, length);
        dgx_dsi_bring_out(bus->area.fb + from, length);
    }
    bus->batch_seen = false;
    /* what the 2D DMA has put is in memory already, but with two frame buffers it is still to be shown */
    if (!drawn && !bus->rows_written) return;
    bus->rows_written = false;
    if (bus->frames == 2) {
        /* the frame buffer that is about to be shown gets all it still lacks */
        dgx_dsi_window_close(bus);
        dgx_fb_tiles_finish(&bus->tiles, bus->area.fb, dgx_dsi_other(bus));
        if (dgx_fb_tiles_take_copied(&bus->tiles, &from, &length)) dgx_dsi_bring_out(bus->area.fb + from, length);
        dgx_dsi_flip(bus);
    }
}

uint8_t dgx_dsi_data_lanes(dgx_bus_protocols_t *_bus)
{
    return ((dgx_dsi_bus_t *)_bus)->data_lanes;
}

#if DGX_DSI_HARDWARE_FILL
/*
 * Fill a rectangle of the screen, which lies inside it, by the accelerator.
 * Returns false when it has not been done: the processor fills then.
 */
static bool dgx_dsi_hardware_fill(dgx_dsi_bus_t *bus, int x, int y, int w, int h, uint32_t color)
{
    const dgx_fb_area_t *area  = &bus->area;
    const uint32_t       bytes = area->pixel_bytes;
    uint16_t             x0, y0, x1, y1;
    dgx_fb_area_to_panel(area, x, x + w - 1, y, y + h - 1, &x0, &y0, &x1, &y1);
    /* the color as 8 bits of red, green and blue; 5 and 6 bits are spread so that the accelerator gets the same 5 and 6 back */
    uint32_t r, g, b;
    if (bytes == 2) {
        r = (color >> 11) & 0x1fu, g = (color >> 5) & 0x3fu, b = color & 0x1fu;
        r = r << 3 | r >> 2, g = g << 2 | g >> 4, b = b << 3 | b >> 2;
    } else {
        r = (color >> 16) & 0xffu, g = (color >> 8) & 0xffu, b = color & 0xffu;
    }
    const size_t pitch = (size_t)area->fb_width * bytes;
    uint8_t     *first = area->fb + (size_t)y0 * pitch + (size_t)x0 * bytes;
    const size_t span  = (size_t)(y1 - y0) * pitch + (size_t)(x1 - x0 + 1u) * bytes;
    /* what the cache holds of the place goes out before the wait for the scan, not after it */
    esp_cache_msync(first, span, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    /* the accelerator fills four times as fast as the display reads: started when the scan is not in the rows, it stays ahead of it */
    if (bus->after_scan) dgx_dsi_wait_scan_outside(bus, y0, y1);
    const ppa_fill_oper_config_t fill = {
        .out = {
            .buffer         = area->fb,
            .buffer_size    = (uint32_t)(pitch * area->fb_height),
            .pic_w          = area->fb_width,
            .pic_h          = area->fb_height,
            .block_offset_x = x0,
            .block_offset_y = y0,
            .fill_cm        = bytes == 2 ? PPA_FILL_COLOR_MODE_RGB565 : PPA_FILL_COLOR_MODE_RGB888,
        },
        .fill_block_w    = (uint32_t)(x1 - x0 + 1u),
        .fill_block_h    = (uint32_t)(y1 - y0 + 1u),
        .fill_argb_color = {.a = 0xff, .r = area->swap_rb ? b : r, .g = g, .b = area->swap_rb ? r : b},
        .mode            = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(bus->filler, &fill) == ESP_OK;
}
#endif

/*
 * Drawing functions of a screen on this bus. The pixels are memory at hand,
 * so a pixel and a rectangle go straight into it, past the window a display
 * behind a bus is drawn through; lines, circles and the rest are made of
 * these two.
 */

static inline dgx_dsi_bus_t *dgx_dsi_bus_of(dgx_screen_t *scr)
{
    return (dgx_dsi_bus_t *)((dgx_screen_with_bus_t *)scr)->bus;
}

static void dgx_dsi_scr_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    if (x < 0 || x >= scr->width || y < 0 || y >= scr->height) return;
    dgx_dsi_bus_t *bus = dgx_dsi_bus_of(scr);
    if (bus->frames == 2) {
        uint16_t px = (uint16_t)x, py = (uint16_t)y;
        if (bus->flip_pending) dgx_dsi_draw_ready(bus);
        if (bus->window_open) dgx_dsi_window_close(bus);
        if (bus->turned) dgx_fb_area_point_to_panel(&bus->area, x, y, &px, &py);
        dgx_fb_tiles_pixel(&bus->tiles, bus->area.fb, dgx_dsi_other(bus), px, py);
    }
    dgx_fb_area_set_pixel(&bus->area, (uint16_t)x, (uint16_t)y, color);
    dgx_dsi_written(bus);
    dgx_screen_touch(scr, x, x, y, y);
}

static uint32_t dgx_dsi_scr_get_pixel(dgx_screen_t *scr, int x, int y)
{
    if (x < 0 || x >= scr->width || y < 0 || y >= scr->height) return 0;
    dgx_dsi_bus_t *bus = dgx_dsi_bus_of(scr);
    /* until the frame buffers have changed places the one at hand is the one that was drawn into, and lacks nothing */
    if (bus->frames == 2 && !bus->flip_pending) {
        uint16_t px = (uint16_t)x, py = (uint16_t)y;
        if (bus->window_open) dgx_dsi_window_close(bus);
        if (bus->turned) dgx_fb_area_point_to_panel(&bus->area, x, y, &px, &py);
        dgx_fb_tiles_pixel_read(&bus->tiles, bus->area.fb, dgx_dsi_other(bus), px, py);
    }
    return dgx_fb_area_get_pixel(&bus->area, (uint16_t)x, (uint16_t)y);
}

static void dgx_dsi_scr_fill_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > scr->width) w = scr->width - x;
    if (h < 0) {
        y += h;
        h = -h;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > scr->height) h = scr->height - y;
    if (w <= 0 || h <= 0) return;
    dgx_dsi_bus_t *bus = dgx_dsi_bus_of(scr);
    if (bus->frames == 2) {
        dgx_dsi_draw_ready(bus);
        dgx_dsi_tiles_ready(bus, x, x + w - 1, y, y + h - 1, true, true);
    }
#if DGX_DSI_HARDWARE_FILL
    /*
     * Most rectangles are the short runs figures and letters are made of:
     * they are told apart by one sum, for no rectangle the accelerator takes
     * has less than 64 in it. Then the rows the rectangle takes on the
     * panel, its width when the axes are swapped, and its bytes.
     */
    if (w + h >= 64 && bus->filler &&
        (uint32_t)(bus->area.swap_xy ? w : h) + (uint32_t)w * h * bus->area.pixel_bytes / 64u >= DGX_DSI_HARDWARE_FILL_FROM &&
        dgx_dsi_hardware_fill(bus, x, y, w, h, color)) {
        bus->rows_written = true;
        dgx_screen_touch(scr, x, x + w - 1, y, y + h - 1);
        return;
    }
#endif
    dgx_fb_area_fill(&bus->area, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
    dgx_dsi_written(bus);
    dgx_screen_touch(scr, x, x + w - 1, y, y + h - 1);
}

static void dgx_dsi_scr_update_screen(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    dgx_dsi_show(&dgx_dsi_bus_of(scr)->protocols);
}

void dgx_dsi_screen_functions(dgx_screen_t *scr)
{
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->set_pixel      = dgx_dsi_scr_set_pixel;
    scr->get_pixel      = dgx_dsi_scr_get_pixel;
    scr->fill_rectangle = dgx_dsi_scr_fill_rectangle;
    scr->update_screen  = dgx_dsi_scr_update_screen;
}
