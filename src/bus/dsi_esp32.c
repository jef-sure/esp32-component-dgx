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
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include "sdkconfig.h"

#include "bus/dgx_dsi_esp32.h"
#include "dgx_fb_area_priv.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"

#define DGX_DSI_BUFFER_LEN 4096

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

typedef struct _dgx_dsi_bus_t {
    dgx_bus_protocols_t       protocols;
    esp_lcd_dsi_bus_handle_t  dsi;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
    esp_ldo_channel_handle_t  phy_power;
    dgx_fb_area_t             area; /* area.fb is NULL until dgx_dsi_video_setup() */
    uint8_t                   data_lanes;
    /*
     * A DSI packet carries a command together with its parameters, so a
     * command is held back until its data comes, or until it is known that
     * there is none: another command, or sync_write().
     */
    uint8_t command;
    bool    command_pending;
} dgx_dsi_bus_t;

static const char TAG[] = "DGX DSI BUS";

static void dgx_dsi_send_pending(dgx_dsi_bus_t *bus)
{
    if (!bus->command_pending) return;
    bus->command_pending = false;
    esp_err_t ret        = esp_lcd_panel_io_tx_param(bus->io, bus->command, NULL, 0);
    if (ret != ESP_OK) ESP_LOGE(TAG, "command 0x%02x failed: %s", bus->command, esp_err_to_name(ret));
}

static inline void dgx_dsi_written(dgx_dsi_bus_t *bus)
{
    if (bus->area.dirty_weight > DGX_DSI_SHOW_WEIGHT) dgx_dsi_show(&bus->protocols);
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
    if (bus->area.fb && dgx_fb_area_data(&bus->area, data, byte_len)) return;
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

bool dgx_dsi_video_setup(dgx_bus_protocols_t *_bus, const dgx_dsi_video_t *video, uint8_t color_bits, bool swap_rb)
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
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = (float)video->pixel_clock_khz / 1000.0f,
        .in_color_format    = color_bits == 16 ? LCD_COLOR_FMT_RGB565 : LCD_COLOR_FMT_RGB888,
        .num_fbs            = 1,
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
    void *fb = NULL;
    ret      = esp_lcd_dpi_panel_get_frame_buffer(bus->panel, 1, &fb);
    if (ret != ESP_OK || !fb) {
        ESP_LOGE(TAG, "esp_lcd_dpi_panel_get_frame_buffer failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(bus->panel);
        bus->panel = NULL;
        return false;
    }
    dgx_fb_area_init(&bus->area, (uint8_t *)fb, video->width, video->height, color_bits / 8u, swap_rb);
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
    dgx_fb_area_orientation(&bus->area, mirror_x, mirror_y, swap_xy);
}

void dgx_dsi_show(dgx_bus_protocols_t *_bus)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    uint32_t       from, length;
    if (dgx_fb_area_take_dirty(&bus->area, &from, &length)) {
        esp_cache_msync(bus->area.fb + from, length, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
}

uint8_t dgx_dsi_data_lanes(dgx_bus_protocols_t *_bus)
{
    return ((dgx_dsi_bus_t *)_bus)->data_lanes;
}

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
    dgx_fb_area_set_pixel(&bus->area, (uint16_t)x, (uint16_t)y, color);
    dgx_dsi_written(bus);
    dgx_screen_touch(scr, x, x, y, y);
}

static uint32_t dgx_dsi_scr_get_pixel(dgx_screen_t *scr, int x, int y)
{
    if (x < 0 || x >= scr->width || y < 0 || y >= scr->height) return 0;
    return dgx_fb_area_get_pixel(&dgx_dsi_bus_of(scr)->area, (uint16_t)x, (uint16_t)y);
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
    dgx_fb_area_fill(&bus->area, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
    dgx_dsi_written(bus);
    dgx_screen_touch(scr, x, x + w - 1, y, y + h - 1);
}

static void dgx_dsi_scr_update_screen(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    dgx_dsi_show(((dgx_screen_with_bus_t *)scr)->bus);
}

void dgx_dsi_screen_functions(dgx_screen_t *scr)
{
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->set_pixel      = dgx_dsi_scr_set_pixel;
    scr->get_pixel      = dgx_dsi_scr_get_pixel;
    scr->fill_rectangle = dgx_dsi_scr_fill_rectangle;
    scr->update_screen  = dgx_dsi_scr_update_screen;
}
