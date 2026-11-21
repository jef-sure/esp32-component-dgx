/*
 * rgb_esp32.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  A display on the parallel RGB interface of the chip. The panel has no
 *  memory: the LCD peripheral sends it the picture all the time. The frame
 *  buffer lies in PSRAM, and the bus copies it, ten rows at a time, into
 *  two bounce buffers in internal memory, which the GDMA sends from. A copy
 *  reads the frame buffer through the cache, so what the processor has
 *  drawn is seen without being brought out of the cache first, and the GDMA
 *  never waits for PSRAM. Drawing is a window of the frame buffer, kept by
 *  dgx_fb_area_t as on a MIPI-DSI display, or a pixel and a rectangle put
 *  straight into it.
 *
 *  The panel takes RGB565 the low byte first. The LCD peripheral exchanges
 *  the two bytes as it sends them (its lcd_byte_order bit, which the driver
 *  of IDF leaves alone), so the frame buffer keeps pixels the high byte
 *  first as DGX does, and the rows of a virtual screen are copied into it as
 *  they are.
 *
 *  With two frame buffers the copy reads one and drawing goes into the
 *  other. Where a frame begins, the copy takes the one it was last told to,
 *  and counts the frame: the frame buffers change places at that moment.
 *  Which of them drawing goes into, and what it lacks of the other, is kept
 *  by the same cores as on a MIPI-DSI display, dgx_fb_flip_t and
 *  dgx_fb_tiles_t, the tiles copied by the processor.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/lcd_ll.h"
#include "sdkconfig.h"
#include "esp_private/esp_clk.h"

#include "bus/dgx_rgb_esp32.h"
#include "dgx_fb_area_priv.h"
#if ESP_IDF_VERSION_MAJOR >= 6
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#elif CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/cache.h"
#elif CONFIG_IDF_TARGET_ESP32P4
#include "esp32p4/rom/cache.h"
#endif
#include "dgx_fb_flip_priv.h"
#include "dgx_fb_tiles_priv.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "../dgx_clip_priv.h"

/* rows of the panel in one bounce buffer; there are two of them in internal memory */
#ifdef CONFIG_DGX_RGB_BOUNCE_ROWS
#define DGX_RGB_BOUNCE_ROWS CONFIG_DGX_RGB_BOUNCE_ROWS
#else
#define DGX_RGB_BOUNCE_ROWS 10
#endif

/* the buffer of the window, in internal memory */
#define DGX_RGB_BUFFER_LEN 4096

/* tiles of 32x4 pixels for what one of two frame buffers lacks of the other, as on a MIPI-DSI display */
#define DGX_RGB_TILE_SHIFT_X 5
#define DGX_RGB_TILE_SHIFT_Y 2

/*
 * How close to the start of a frame a word to the copy is not relied on: the
 * copy looks which frame buffer to read where it starts a frame, and a word
 * from the other core in that moment may come too late for it.
 */
#define DGX_RGB_FLIP_GUARD_US 300u

typedef struct _dgx_rgb_bus_t {
    dgx_bus_protocols_t    protocols;
    dgx_rgb_pins_t         pins;
    uint32_t               pclk_hz;
    esp_lcd_panel_handle_t panel;
    dgx_fb_area_t          area;
    uint8_t               *frame[2];    /* the frame buffers; the second only with two */
    uint8_t                frames;      /* 1 or 2 */
    volatile uint8_t       shown;       /* the frame buffer the copy reads */
    volatile uint8_t       next;        /* and the one it takes where the next frame starts */
    volatile uint32_t      refreshes;   /* frames started */
    volatile uint32_t      refresh_us;  /* when the last one did, the low 32 bits of esp_timer_get_time() */
    uint32_t               frame_us;    /* the time of a frame, by the timing */
    SemaphoreHandle_t      refreshed;   /* given where a frame starts, with two frame buffers */
    dgx_fb_tiles_t         tiles;       /* the maps are there with two frame buffers only */
    dgx_fb_flip_t          flip;        /* which of them drawing goes into, and whether the copy was told to read it */
    bool                   window_open; /* two frame buffers: pixels go into a window whose tiles stay lacked until they end */
    uint32_t               window_got;  /* how many have come */
    bool                   rows_written; /* rows went into the frame buffer by write_rows, which the window does not see */
    /* what the copies into the bounce buffers have taken, in cycles of the processor, see dgx_rgb_counters() */
    volatile uint32_t copies, copy_most;
    volatile uint64_t copy_sum;
    volatile uint32_t last_copy;  /* the cycle count the last copy started at */
    volatile uint32_t gap_most;   /* the longest time between two copies a period apart, the blank of a frame left out */
    uint32_t          bounce_px;  /* pixels a bounce buffer holds */
    uint32_t          frame_px;   /* pixels of a frame buffer */
    uint32_t          period_us; /* what the panel takes to send one bounce buffer */
    /* what the times above are counted from, and what dgx_rgb_bench() needs */
    uint32_t row_clocks, frame_rows, bounce_rows;
    uint16_t width, height;
    uint8_t  core;                       /* the core the display was made on, its interrupts' */
    uint32_t sheet_min_hz, sheet_max_hz; /* what the glass allows, from its driver; 0 when not told */
} dgx_rgb_bus_t;

/*
 * esp_lcd that restarts every frame from the first bounce buffer and does
 * not count anew there: up to 5.5.6, 6.0.3 and 6.1. The release branches
 * of 5.5 and 6.0 count anew since, so their next versions do.
 */
#if CONFIG_LCD_RGB_RESTART_IN_VSYNC &&                                                                                                                        \
    (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 7) || (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 4)) ||  \
     (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 1, 1)))
#define DGX_RGB_RESTART_KEEPS_COUNT 1
#else
#define DGX_RGB_RESTART_KEEPS_COUNT 0
#endif

static const char TAG[] = "DGX RGB BUS";

/*
 * The rows of the next copy asked of the cache now, as esp_lcd does with a
 * frame buffer of its own: the cache reads them from PSRAM meanwhile, and
 * that copy reads the cache. On an ESP32-S3 at 33 MHz and 800x480 it cut
 * the share of a core the copies take from a half to an eighth while the
 * picture stands, and to a third while it draws.
 */
static IRAM_ATTR void dgx_rgb_preload(const uint8_t *from, uint32_t bytes)
{
#if ESP_IDF_VERSION_MAJOR >= 6
    cache_hal_preload(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA, (uint32_t)from, bytes, CACHE_PRELOAD_ORDER_ASCENDING);
#elif CONFIG_IDF_TARGET_ESP32S3
    Cache_Start_DCache_Preload((uint32_t)from, bytes, 0);
#elif CONFIG_IDF_TARGET_ESP32P4
    Cache_Start_L2_Cache_Preload((uint32_t)from, bytes, 0);
#endif
}

/*
 * The GDMA has sent one bounce buffer and goes on with the other: this one
 * gets the next rows of the frame buffer before the other is sent. The rows
 * of a frame start at 0: there the copy takes the frame buffer it was told
 * to, and the frame is counted.
 */
static IRAM_ATTR bool dgx_rgb_fill(esp_lcd_panel_handle_t panel, void *buffer, int pos_px, int len_bytes, void *user)
{
    dgx_rgb_bus_t *bus   = (dgx_rgb_bus_t *)user;
    BaseType_t     woken = pdFALSE;
    if (pos_px == 0) {
        bus->shown      = bus->next;
        bus->refresh_us = (uint32_t)esp_timer_get_time();
        ++bus->refreshes;
        if (bus->refreshed) xSemaphoreGiveFromISR(bus->refreshed, &woken);
    }
    uint8_t       *from  = bus->frame[bus->shown] + (size_t)pos_px * 2u;
    const uint32_t start = esp_cpu_get_cycle_count();
    /*
     * The copies come a period apart: a longer time between two means the
     * interrupt came late by the difference. The one after the rows of a
     * frame waits for its blank, the third copy of the next frame.
     */
    if (bus->copies && (uint32_t)pos_px != 2u * bus->bounce_px) {
        const uint32_t gap = start - bus->last_copy;
        if (gap > bus->gap_most) bus->gap_most = gap;
    }
    bus->last_copy = start;
    ++bus->copies;
    memcpy(buffer, from, (size_t)len_bytes);
    const uint32_t pos = (uint32_t)pos_px + bus->bounce_px;
    dgx_rgb_preload(pos < bus->frame_px ? bus->frame[bus->shown] + (size_t)pos * 2u : bus->frame[bus->next], (uint32_t)len_bytes);
    const uint32_t took = esp_cpu_get_cycle_count() - start;
    bus->copy_sum += took;
    if (took > bus->copy_most) bus->copy_most = took;
    return woken == pdTRUE;
}

/*
 * What the two frame buffers that change places are told of the display:
 * see dgx_fb_flip_priv.h. The frames are counted where the copy takes the
 * frame buffer of the next one.
 */

static void dgx_rgb_flip_show(void *context, uint8_t frame)
{
    ((dgx_rgb_bus_t *)context)->next = frame;
}

static bool dgx_rgb_flip_frames(void *context, uint32_t *ended, uint32_t *since_us, uint32_t *period_us)
{
    const dgx_rgb_bus_t *bus = (const dgx_rgb_bus_t *)context;
    /* the time before the count, the other way round from where they are set */
    const uint32_t at = bus->refresh_us;
    *ended            = bus->refreshes;
    *period_us        = bus->frame_us;
    *since_us         = (uint32_t)esp_timer_get_time() - at;
    return *period_us >= 1000u && *period_us <= 100000u && *since_us <= 2u * *period_us;
}

static bool dgx_rgb_flip_wait(void *context)
{
    return xSemaphoreTake(((dgx_rgb_bus_t *)context)->refreshed, pdMS_TO_TICKS(200)) == pdTRUE;
}

static uint32_t dgx_rgb_flip_now(void *context)
{
    return (uint32_t)esp_timer_get_time();
}

static const dgx_fb_flip_display_t dgx_rgb_flip_display = {
    .show   = dgx_rgb_flip_show,
    .frames = dgx_rgb_flip_frames,
    .wait   = dgx_rgb_flip_wait,
    .now_us = dgx_rgb_flip_now,
};

/* the frame buffer the copy reads, or is about to: it has what the one drawn into lacks */
static inline const uint8_t *dgx_rgb_other(const dgx_rgb_bus_t *bus)
{
    return bus->frame[bus->flip.drawn ^ 1u];
}

/*
 * The copy was told to read the frame buffer drawing went into, and
 * something is about to be drawn. The batch goes into the same picture when
 * the copy has not taken it and the frame does not start soon; otherwise
 * this waits until it does, and drawing goes into the other frame buffer,
 * which lacks what was drawn since it was last drawn into.
 */
static inline void dgx_rgb_draw_ready(dgx_rgb_bus_t *bus)
{
    if (bus->frames != 2 || !bus->flip.told || !dgx_fb_flip_draw(&bus->flip)) return;
    bus->area.fb = bus->frame[bus->flip.drawn];
    dgx_fb_tiles_changed(&bus->tiles);
}

/*
 * Pixels were being sent into the window, and now something else comes. If
 * they did not fill it, what they did not reach is as old as the frame
 * buffer was: it is taken from the other one. The pixels came row after row
 * of the screen, so that is the rest of a row and the rows after it.
 */
static void dgx_rgb_window_close(dgx_rgb_bus_t *bus)
{
    if (!bus->window_open) return;
    bus->window_open = false;
    const dgx_fb_area_t *area   = &bus->area;
    const uint32_t       width  = (uint32_t)area->right - area->left + 1u;
    const uint32_t       height = (uint32_t)area->bottom - area->top + 1u;
    /* the window on the panel, and the part of it the pixels did not reach */
    uint16_t x0, y0, x1, y1, px0, py0, px1, py1;
    dgx_fb_area_to_panel(area, area->left, area->right, area->top, area->bottom, &x0, &y0, &x1, &y1);
    uint32_t full_rows = bus->window_got / width;
    uint32_t rest      = bus->window_got % width;
    if (full_rows < height) {
        if (rest) {
            const int row = (int)(area->top + full_rows);
            dgx_fb_area_to_panel(area, (int)(area->left + rest), area->right, row, row, &px0, &py0, &px1, &py1);
            dgx_fb_tiles_unwritten(&bus->tiles, area->fb, dgx_rgb_other(bus), x0, y0, x1, y1, px0, py0, px1, py1);
            ++full_rows;
        }
        if (full_rows < height) {
            dgx_fb_area_to_panel(area, area->left, area->right, (int)(area->top + full_rows), area->bottom, &px0, &py0, &px1, &py1);
            dgx_fb_tiles_unwritten(&bus->tiles, area->fb, dgx_rgb_other(bus), x0, y0, x1, y1, px0, py0, px1, py1);
        }
    }
    dgx_fb_tiles_close(&bus->tiles, x0, y0, x1, y1);
}

/*
 * With two frame buffers, before a rectangle of the screen is written or
 * read: the frame buffer drawing goes into gets what it lacks there.
 * `whole` tells that every pixel of the rectangle is about to be written.
 */
static void dgx_rgb_tiles_ready(dgx_rgb_bus_t *bus, int left, int right, int top, int bottom, bool whole, bool drawn)
{
    uint16_t x0, y0, x1, y1;
    dgx_rgb_window_close(bus);
    dgx_fb_area_to_panel(&bus->area, left, right, top, bottom, &x0, &y0, &x1, &y1);
    dgx_fb_tiles_ready(&bus->tiles, bus->area.fb, dgx_rgb_other(bus), x0, y0, x1, y1, whole, drawn);
}

/*
 * The window of the frame buffer answers the commands of the window; the
 * panel itself takes none, so anything else goes nowhere.
 */
static void dgx_rgb_send_command(struct _dgx_bus_protocols_t *_bus, uint8_t cmd)
{
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)_bus;
    /* the window is about to change, or to be written from its first pixel again */
    if (bus->window_open && cmd != DGX_FB_AREA_WRITE_CONTINUE) dgx_rgb_window_close(bus);
    if (bus->area.fb && dgx_fb_area_command(&bus->area, cmd)) return;
    ESP_LOGD(TAG, "command 0x%02x: an RGB panel takes no commands", cmd);
}

static void dgx_rgb_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len)
{
    const uint32_t byte_len = (len + 7u) / 8u;
    for (uint32_t i = 0; i < byte_len; i++) dgx_rgb_send_command(_bus, cmds[i]);
}

static void dgx_rgb_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_rgb_bus_t *bus      = (dgx_rgb_bus_t *)_bus;
    const uint32_t byte_len = (len + 7u) / 8u;
    if (!byte_len || !bus->area.fb) return;
    const bool pixels = bus->area.command == DGX_FB_AREA_WRITE || bus->area.command == DGX_FB_AREA_WRITE_CONTINUE;
    if (bus->frames == 2 && pixels) {
        dgx_rgb_draw_ready(bus);
        if (bus->window_open) {
            bus->window_got += byte_len / 2u;
        } else if (bus->area.command == DGX_FB_AREA_WRITE && bus->area.x == bus->area.left && bus->area.y == bus->area.top) {
            /* pixels for the window from its first one on: most likely for all of it, which is known when they end */
            uint16_t x0, y0, x1, y1;
            dgx_fb_area_to_panel(&bus->area, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, &x0, &y0, &x1, &y1);
            dgx_fb_tiles_open(&bus->tiles, bus->area.fb, dgx_rgb_other(bus), x0, y0, x1, y1);
            bus->window_open = true;
            bus->window_got  = byte_len / 2u;
        } else {
            dgx_rgb_tiles_ready(bus, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, false, true);
        }
    }
    dgx_fb_area_data(&bus->area, data, byte_len);
}

static void dgx_rgb_sync_write(struct _dgx_bus_protocols_t *_bus)
{
}

/* the buffer of the window: what the processor writes there it should not carry through the cache of the PSRAM */
static uint8_t *dgx_rgb_memory_allocator(uint32_t len)
{
    uint8_t *memory = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return memory ? memory : (uint8_t *)malloc(len);
}

static void dgx_rgb_video_drop(dgx_rgb_bus_t *bus)
{
    if (bus->panel) esp_lcd_panel_del(bus->panel);
    if (bus->refreshed) vSemaphoreDelete(bus->refreshed);
    if (bus->tiles.owed) dgx_fb_tiles_free(&bus->tiles);
    heap_caps_free(bus->frame[0]);
    heap_caps_free(bus->frame[1]);
    bus->panel     = NULL;
    bus->refreshed = NULL;
    bus->frame[0]  = NULL;
    bus->frame[1]  = NULL;
    bus->area.fb   = NULL;
}

static void dgx_rgb_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)_bus;
    dgx_rgb_video_drop(bus);
    free(bus->protocols.buffer);
    free(bus);
}

dgx_bus_protocols_t *dgx_rgb_init(const dgx_rgb_pins_t *pins, uint32_t pclk_hz)
{
    if (!pins || !pclk_hz) {
        ESP_LOGE(TAG, "dgx_rgb_init needs the pins and the pixel clock");
        return NULL;
    }
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)calloc(1, sizeof(dgx_rgb_bus_t));
    if (!bus) {
        ESP_LOGE(TAG, "dgx_rgb_init failed to allocate dgx_rgb_bus_t");
        return NULL;
    }
    bus->pins                       = *pins;
    bus->pclk_hz                    = pclk_hz;
    bus->protocols.bus_type         = DGX_BUS_RGB;
    bus->protocols.bus_name         = "RGB";
    bus->protocols.write_command    = dgx_rgb_send_command;
    bus->protocols.write_commands   = dgx_rgb_send_commands;
    bus->protocols.write_data       = dgx_rgb_send_data;
    bus->protocols.write_data_async = dgx_rgb_send_data; /* a copy into the frame buffer: done when it returns */
    bus->protocols.sync_write       = dgx_rgb_sync_write;
    bus->protocols.dispose_bus          = dgx_rgb_dispose_bus_func;
    bus->protocols.memory_allocator = dgx_rgb_memory_allocator;
    bus->protocols.buffer_len       = DGX_RGB_BUFFER_LEN;
    bus->protocols.buffer           = bus->protocols.memory_allocator(bus->protocols.buffer_len);
    if (!bus->protocols.buffer) {
        ESP_LOGE(TAG, "dgx_rgb_init buffer allocation failed");
        dgx_rgb_dispose_bus_func(&bus->protocols);
        return NULL;
    }
    return &bus->protocols;
}

uint32_t dgx_rgb_pixel_clock(dgx_bus_protocols_t *bus)
{
    return ((dgx_rgb_bus_t *)bus)->pclk_hz;
}

/*
 * Rows of pixels of a program, kept as DGX keeps them, into a rectangle of
 * the screen: with the frame buffer in the same order a row is one copy,
 * when the screen lies on the panel as it is.
 */
static bool dgx_rgb_write_rows(struct _dgx_bus_protocols_t *_bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *rows,
                               uint32_t pitch)
{
    dgx_rgb_bus_t *bus  = (dgx_rgb_bus_t *)_bus;
    dgx_fb_area_t *area = &bus->area;
    if (!area->fb || !area->high_first || area->swap_rb || area->mirror_x || area->mirror_y || area->swap_xy) return false;
    if ((uint32_t)left + width > area->fb_width || (uint32_t)top + height > area->fb_height) return false;
    if (bus->frames == 2) {
        dgx_rgb_draw_ready(bus);
        /* every pixel of the rectangle is written: of its tiles only what lies outside it is brought up to date */
        dgx_rgb_tiles_ready(bus, left, left + width - 1, top, top + height - 1, true, true);
    }
    const size_t fb_pitch = (size_t)area->fb_width * 2u, row_bytes = (size_t)width * 2u;
    uint8_t     *to       = area->fb + (size_t)top * fb_pitch + (size_t)left * 2u;
    for (uint32_t r = 0; r < height; ++r, to += fb_pitch, rows += pitch) memcpy(to, rows, row_bytes);
    bus->rows_written = true;
    return true;
}

/* rows a bounce buffer holds: as asked, or fewer that the height divides into, as the driver of IDF wants */
/* the time of a frame and the time a copy has, by the timing and the pixel clock */
static void dgx_rgb_times(dgx_rgb_bus_t *bus)
{
    bus->frame_us  = (uint32_t)((uint64_t)bus->row_clocks * bus->frame_rows * 1000000u / bus->pclk_hz);
    bus->period_us = (uint32_t)((uint64_t)bus->row_clocks * bus->bounce_rows * 1000000u / bus->pclk_hz);
}

static uint32_t dgx_rgb_bounce_rows(uint32_t height)
{
    uint32_t rows = DGX_RGB_BOUNCE_ROWS < height ? DGX_RGB_BOUNCE_ROWS : height;
    while (rows > 1 && height % rows) --rows;
    return rows;
}

static uint8_t *dgx_rgb_frame_alloc(size_t bytes)
{
    uint8_t *fb = (uint8_t *)heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return fb ? fb : (uint8_t *)heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_8BIT);
}

bool dgx_rgb_video_setup(dgx_bus_protocols_t *_bus, const dgx_rgb_video_t *video, bool swap_rb, uint8_t frames)
{
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)_bus;
    if (bus->panel) {
        ESP_LOGE(TAG, "the bus already has a display");
        return false;
    }
    if (frames != 1 && frames != 2) {
        ESP_LOGE(TAG, "%u frame buffers: 1 or 2", frames);
        return false;
    }
    const size_t fb_bytes = (size_t)video->width * video->height * 2u;
    bus->frame[0]         = dgx_rgb_frame_alloc(fb_bytes);
    bus->frame[1]         = frames == 2 ? dgx_rgb_frame_alloc(fb_bytes) : NULL;
    if (!bus->frame[0] || (frames == 2 && !bus->frame[1])) {
        ESP_LOGE(TAG, "no memory for %u frame buffers of %u bytes", frames, (unsigned)fb_bytes);
        dgx_rgb_video_drop(bus);
        return false;
    }
    bus->frames     = frames;
    bus->shown      = 0;
    bus->next       = 0;
    bus->row_clocks  = (uint32_t)video->width + video->hsync_pulse_width + video->hsync_back_porch + video->hsync_front_porch;
    bus->frame_rows  = (uint32_t)video->height + video->vsync_pulse_width + video->vsync_back_porch + video->vsync_front_porch;
    bus->bounce_rows = dgx_rgb_bounce_rows(video->height);
    bus->width       = video->width;
    bus->height      = video->height;
    bus->core        = (uint8_t)xPortGetCoreID();
    dgx_rgb_times(bus);
    bus->bounce_px            = video->width * dgx_rgb_bounce_rows(video->height);
    bus->frame_px             = (uint32_t)video->width * video->height;
    if (frames == 2) {
        bus->refreshed = xSemaphoreCreateBinary();
        if (!bus->refreshed || !dgx_fb_tiles_init(&bus->tiles, video->width, video->height, 2, DGX_RGB_TILE_SHIFT_X, DGX_RGB_TILE_SHIFT_Y)) {
            ESP_LOGE(TAG, "no memory for the maps of two frame buffers");
            dgx_rgb_video_drop(bus);
            return false;
        }
    }
    /* the copy starts with the first one: with two, drawing goes into the second */
    dgx_fb_flip_init(&bus->flip, &dgx_rgb_flip_display, bus, frames == 2 ? 1 : 0, true, DGX_RGB_FLIP_GUARD_US);
    dgx_fb_area_init(&bus->area, bus->frame[bus->flip.drawn], video->width, video->height, 2, swap_rb);
    const uint32_t             rows   = dgx_rgb_bounce_rows(video->height);
    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings =
            {
                .pclk_hz           = bus->pclk_hz,
                .h_res             = video->width,
                .v_res             = video->height,
                .hsync_pulse_width = video->hsync_pulse_width,
                .hsync_back_porch  = video->hsync_back_porch,
                .hsync_front_porch = video->hsync_front_porch,
                .vsync_pulse_width = video->vsync_pulse_width,
                .vsync_back_porch  = video->vsync_back_porch,
                .vsync_front_porch = video->vsync_front_porch,
                .flags =
                    {
                        .hsync_idle_low  = video->hsync_idle_low,
                        .vsync_idle_low  = video->vsync_idle_low,
                        .de_idle_high    = video->de_idle_high,
                        .pclk_active_neg = video->pclk_active_neg,
                    },
            },
        .data_width            = 16,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        .in_color_format  = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
#else
        .bits_per_pixel = 16,
#endif
        .num_fbs               = 0,
        .bounce_buffer_size_px = video->width * rows,
        .hsync_gpio_num        = bus->pins.hsync,
        .vsync_gpio_num        = bus->pins.vsync,
        .de_gpio_num           = bus->pins.de,
        .pclk_gpio_num         = bus->pins.pclk,
        .disp_gpio_num         = bus->pins.disp,
        .flags                 = {.no_fb = 1},
    };
    memcpy(config.data_gpio_nums, bus->pins.data, sizeof(bus->pins.data));
    esp_err_t ret = esp_lcd_new_rgb_panel(&config, &bus->panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %s", esp_err_to_name(ret));
        bus->panel = NULL;
        dgx_rgb_video_drop(bus);
        return false;
    }
    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {.on_bounce_empty = dgx_rgb_fill};
    ret                                                 = esp_lcd_rgb_panel_register_event_callbacks(bus->panel, &callbacks, bus);
    if (ret == ESP_OK) ret = esp_lcd_panel_reset(bus->panel);
    if (ret == ESP_OK) ret = esp_lcd_panel_init(bus->panel);
    if (ret == ESP_OK && bus->pins.disp >= 0) ret = esp_lcd_panel_disp_on_off(bus->panel, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "the RGB panel did not start: %s", esp_err_to_name(ret));
        dgx_rgb_video_drop(bus);
        return false;
    }
    /* the bytes of a pixel exchanged by the peripheral from the next frame on; the frame buffer is black either way */
    lcd_ll_swap_dma_data_byte_order(&LCD_CAM, true);
    esp_lcd_rgb_panel_restart(bus->panel);
    dgx_fb_area_high_first(&bus->area, true);
    bus->protocols.write_rows = dgx_rgb_write_rows;
    ESP_LOGI(TAG, "%ux%u at %u.%03u MHz, %u frame buffer%s, bounce buffers of %u rows", video->width, video->height, (unsigned)(bus->pclk_hz / 1000000u),
             (unsigned)(bus->pclk_hz / 1000u % 1000u), frames, frames == 2 ? "s" : "", (unsigned)rows);
#if DGX_RGB_RESTART_KEEPS_COUNT
    ESP_LOGW(TAG, "CONFIG_LCD_RGB_RESTART_IN_VSYNC is set: with this IDF, after a copy that comes a period late the picture keeps short stripes until "
                  "the display is made again; leave it off, see include/bus/dgx_rgb_esp32.h");
#endif
    return true;
}

void dgx_rgb_counters(dgx_bus_protocols_t *_bus, dgx_rgb_counters_t *counters)
{
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)_bus;
    const uint32_t       mhz = (uint32_t)(esp_clk_cpu_freq() / 1000000);
    counters->frames         = bus->refreshes;
    counters->copies         = bus->copies;
    counters->copy_us        = (uint32_t)(bus->copy_sum / mhz);
    counters->copy_us_most   = bus->copy_most / mhz;
    counters->gap_us_most    = bus->gap_most / mhz;
    counters->period_us      = bus->period_us;
    /* the longest ones are of the time since the last look */
    bus->copy_most = 0;
    bus->gap_most  = 0;
}

void dgx_rgb_clock_range(dgx_bus_protocols_t *_bus, uint32_t min_hz, uint32_t max_hz)
{
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)_bus;
    bus->sheet_min_hz  = min_hz;
    bus->sheet_max_hz  = max_hz;
}

/*
 * The pixel clock changed where the next frame begins, as esp_lcd does it,
 * and the times counted from it; then two frames more, so that the copies
 * counted next are of the new clock.
 */
static void dgx_rgb_bench_clock(dgx_rgb_bus_t *bus, uint32_t hz)
{
    const uint32_t before_us = bus->frame_us;
    esp_lcd_rgb_panel_set_pclk(bus->panel, hz);
    bus->pclk_hz = hz;
    dgx_rgb_times(bus);
    vTaskDelay(pdMS_TO_TICKS((before_us + 2u * bus->frame_us) / 1000u + 2u));
}

/* filling and copying in PSRAM as fast as it can, for `ms`: a program drawing hard; a tick now and then for the rest of the program */
static void dgx_rgb_bench_load(uint8_t *a, uint8_t *b, size_t bytes, uint32_t ms)
{
    const int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    uint8_t       v   = 0;
    for (int n = 1; esp_timer_get_time() < end; ++n) {
        memset(a, v++, bytes);
        memcpy(b, a, bytes);
        if ((n & 3) == 0) vTaskDelay(1);
    }
}

/* one clock: the copies with nothing drawn, then while the bench draws */
static void dgx_rgb_bench_step(dgx_rgb_bus_t *bus, uint8_t *a, uint8_t *b, size_t bytes, dgx_rgb_bench_step_t *step)
{
    dgx_rgb_counters_t c0, c1, c2;
    dgx_rgb_counters(&bus->protocols, &c0); /* the longest of the change of the clock are forgotten */
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(500));
    dgx_rgb_counters(&bus->protocols, &c1);
    int64_t t1 = esp_timer_get_time();
    dgx_rgb_bench_load(a, b, bytes, 1000);
    dgx_rgb_counters(&bus->protocols, &c2);
    const int64_t t2    = esp_timer_get_time();
    const uint32_t gap  = c1.gap_us_most > c2.gap_us_most ? c1.gap_us_most : c2.gap_us_most;
    step->frames_x10    = (uint16_t)((uint64_t)(c1.frames - c0.frames) * 10000000u / (uint64_t)(t1 - t0));
    step->period_us     = (uint16_t)c1.period_us;
    step->gap_us        = (uint16_t)(gap < 65535u ? gap : 65535u);
    step->still_pct     = (uint8_t)((uint64_t)(c1.copy_us - c0.copy_us) * 100u / (uint64_t)(t1 - t0));
    step->drawing_pct   = (uint8_t)((uint64_t)(c2.copy_us - c1.copy_us) * 100u / (uint64_t)(t2 - t1));
    step->holds         = gap <= 2u * c1.period_us;
}

bool dgx_rgb_bench(dgx_bus_protocols_t *_bus, dgx_rgb_bench_t *bench)
{
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)_bus;
    if (!bus || !bench || !bus->panel) return false;
    memset(bench, 0, sizeof(*bench));
    bench->version      = DGX_RGB_BENCH_VERSION;
    bench->size         = sizeof(*bench);
    bench->width        = bus->width;
    bench->height       = bus->height;
    bench->pclk_hz      = bus->pclk_hz;
    bench->sheet_min_hz = bus->sheet_min_hz;
    bench->sheet_max_hz = bus->sheet_max_hz;
    bench->display_core = bus->core;
    bench->drawing_core = (uint8_t)xPortGetCoreID();
    if (bench->display_core != bench->drawing_core) bench->flags |= DGX_RGB_BENCH_OTHER_CORE;
#if !CONFIG_SPIRAM_XIP_FROM_PSRAM
    bench->flags |= DGX_RGB_BENCH_NO_XIP;
#endif
#if DGX_RGB_RESTART_KEEPS_COUNT
    bench->flags |= DGX_RGB_BENCH_RESTART;
#endif
    /* the drawing of the bench: two buffers of a third of the panel, where a program keeps what it draws */
    const size_t bytes = (size_t)bus->width * (bus->height / 3u) * 2u;
    uint8_t     *a = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM), *b = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!a || !b) {
        ESP_LOGE(TAG, "no memory for the bench: two buffers of %u bytes in PSRAM", (unsigned)bytes);
        heap_caps_free(a);
        heap_caps_free(b);
        return false;
    }
    const uint32_t own = bus->pclk_hz;
    /* from the top of the glass, or from the clock of the bus, an eighth lower at a time, down to the one that holds */
    uint32_t hz = bus->sheet_max_hz ? bus->sheet_max_hz : own;
    for (int i = 0; i < DGX_RGB_BENCH_STEPS && hz >= 4000000u; ++i, hz = hz / 8u * 7u) {
        dgx_rgb_bench_step_t *step = &bench->step[i];
        step->pclk_hz              = hz / 100000u * 100000u;
        dgx_rgb_bench_clock(bus, step->pclk_hz);
        dgx_rgb_bench_step(bus, a, b, bytes, step);
        bench->steps = (uint8_t)(i + 1);
        ESP_LOGI(TAG, "bench: %u.%u MHz, %u.%u frames a second: the copies take %u%% of core %u still, %u%% drawing; a copy %u us after the one before, of %u: %s",
                 (unsigned)(step->pclk_hz / 1000000u), (unsigned)(step->pclk_hz / 100000u % 10u), (unsigned)(step->frames_x10 / 10u),
                 (unsigned)(step->frames_x10 % 10u), (unsigned)step->still_pct, (unsigned)bus->core, (unsigned)step->drawing_pct, (unsigned)step->gap_us,
                 (unsigned)step->period_us, step->holds ? "holds" : "does not hold");
        if (step->holds) break;
    }
    dgx_rgb_bench_clock(bus, own);
    heap_caps_free(a);
    heap_caps_free(b);
    char text[720];
    dgx_rgb_bench_text(bench, text, sizeof(text));
    if (dgx_rgb_advise(bench) && !(bench->flags & (DGX_RGB_BENCH_OTHER_CORE | DGX_RGB_BENCH_RESTART))) ESP_LOGI(TAG, "%s", text);
    else ESP_LOGW(TAG, "%s", text);
    return true;
}

void dgx_rgb_orientation(dgx_bus_protocols_t *_bus, bool mirror_x, bool mirror_y, bool swap_xy)
{
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)_bus;
    dgx_rgb_window_close(bus); /* where the window lies on the panel is told by the orientation it was sent in */
    dgx_fb_area_orientation(&bus->area, mirror_x, mirror_y, swap_xy);
}

/*
 * The screen draws straight into the frame buffer: a pixel and a rectangle
 * need no window. The bounce copy reads through the cache, so nothing is
 * brought out of it at the end of a batch.
 */

static inline dgx_rgb_bus_t *dgx_rgb_bus_of(dgx_screen_t *scr)
{
    return (dgx_rgb_bus_t *)((dgx_screen_with_bus_t *)scr)->bus;
}

/* where a pixel of the screen lies on the panel */
static inline void dgx_rgb_point(const dgx_rgb_bus_t *bus, int x, int y, uint16_t *px, uint16_t *py)
{
    dgx_fb_area_point_to_panel(&bus->area, x, y, px, py);
}

static void dgx_rgb_scr_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    if (!dgx_clip_has(scr, x, y)) return;
    dgx_rgb_bus_t *bus = dgx_rgb_bus_of(scr);
    if (bus->frames == 2) {
        uint16_t px, py;
        dgx_rgb_draw_ready(bus);
        dgx_rgb_window_close(bus);
        dgx_rgb_point(bus, x, y, &px, &py);
        dgx_fb_tiles_pixel(&bus->tiles, bus->area.fb, dgx_rgb_other(bus), px, py);
    }
    dgx_fb_area_set_pixel(&bus->area, (uint16_t)x, (uint16_t)y, color);
    dgx_screen_touch_inside(scr, x, x, y, y);
}

static uint32_t dgx_rgb_scr_get_pixel(dgx_screen_t *scr, int x, int y)
{
    if (x < 0 || x >= scr->width || y < 0 || y >= scr->height) return 0;
    dgx_rgb_bus_t *bus = dgx_rgb_bus_of(scr);
    if (bus->frames == 2) {
        uint16_t px, py;
        dgx_rgb_window_close(bus);
        dgx_rgb_point(bus, x, y, &px, &py);
        /* a finished picture may still lack a tile: it is brought as anything lacked is */
        dgx_fb_tiles_pixel_read(&bus->tiles, bus->area.fb, dgx_rgb_other(bus), px, py);
    }
    return dgx_fb_area_get_pixel(&bus->area, (uint16_t)x, (uint16_t)y);
}

static void dgx_rgb_scr_fill_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    if (w < 0) {
        x += w;
        w = -w;
    }
    const int cl = dgx_clip_left(scr), cr = dgx_clip_right(scr), ct = dgx_clip_top(scr), cb = dgx_clip_bottom(scr);
    if (x < cl) {
        w -= cl - x;
        x = cl;
    }
    if (x + w > cr) w = cr - x;
    if (h < 0) {
        y += h;
        h = -h;
    }
    if (y < ct) {
        h -= ct - y;
        y = ct;
    }
    if (y + h > cb) h = cb - y;
    if (w <= 0 || h <= 0) return;
    dgx_rgb_bus_t *bus = dgx_rgb_bus_of(scr);
    if (bus->frames == 2) {
        dgx_rgb_draw_ready(bus);
        dgx_rgb_tiles_ready(bus, x, x + w - 1, y, y + h - 1, true, true);
    }
    dgx_fb_area_fill(&bus->area, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
    dgx_screen_touch_inside(scr, x, x + w - 1, y, y + h - 1);
}

/*
 * A rectangle of the screen moved to another place of it, for dgx_scroll():
 * rows of the frame buffer, each at once. With two frame buffers the one
 * drawing goes into gets what it lacks first, where the picture is taken
 * from and where it goes.
 */
static bool dgx_rgb_move(struct _dgx_bus_protocols_t *_bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, uint16_t to_left,
                         uint16_t to_top)
{
    dgx_rgb_bus_t *bus = (dgx_rgb_bus_t *)_bus;
    if (!bus->area.fb || !width || !height) return false;
    if (bus->frames == 2) {
        const int x0 = left < to_left ? left : to_left;
        const int x1 = (left < to_left ? to_left : left) + width - 1;
        const int y0 = top < to_top ? top : to_top;
        const int y1 = (top < to_top ? to_top : top) + height - 1;
        dgx_rgb_draw_ready(bus);
        dgx_rgb_tiles_ready(bus, x0, x1, y0, y1, false, true);
    }
    dgx_fb_area_move(&bus->area, left, top, width, height, to_left, to_top);
    return true;
}

/*
 * A batch has ended. With two frame buffers the picture is finished: the
 * frame buffer gets what it still lacks of the other one, and the copy is
 * told to read it from the start of the next frame.
 */
static void dgx_rgb_show(dgx_rgb_bus_t *bus)
{
    uint32_t   from, length;
    const bool drawn = dgx_fb_area_take_dirty(&bus->area, &from, &length);
    if (bus->frames != 2 || (!drawn && !bus->rows_written)) {
        bus->rows_written = false;
        return;
    }
    bus->rows_written = false;
    dgx_rgb_window_close(bus);
    dgx_fb_flip_drawn(&bus->flip);
    dgx_fb_tiles_complete(&bus->tiles, bus->area.fb, dgx_rgb_other(bus));
    dgx_fb_tiles_take_copied(&bus->tiles, &from, &length); /* the copy reads through the cache: nothing to bring out */
    dgx_fb_flip_finished(&bus->flip);
}

static void dgx_rgb_scr_update_screen(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    dgx_rgb_show(dgx_rgb_bus_of(scr));
}

void dgx_rgb_screen_functions(dgx_screen_t *scr)
{
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->set_pixel      = dgx_rgb_scr_set_pixel;
    scr->get_pixel      = dgx_rgb_scr_get_pixel;
    scr->fill_rectangle = dgx_rgb_scr_fill_rectangle;
    scr->update_screen  = dgx_rgb_scr_update_screen;
    ((dgx_screen_with_bus_t *)scr)->bus->move = dgx_rgb_move;
}
