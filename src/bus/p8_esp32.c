/*
 * p8_esp32.c
 *
 *  Created on: 14.10.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Migrated to esp_lcd I80 peripheral driver (DMA-backed)
 */

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

#include "bus/dgx_p8_esp32.h"
#include "dgx_p8_esp32_priv.h"

static const char TAG[] = "DGX P8 BUS";

static bool dgx_p8_on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    dgx_p8_bus_t *bus = (dgx_p8_bus_t *)user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(bus->transfer_done_sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

void dgx_p8_wait_pending(dgx_p8_bus_t *bus) {
    if (bus->transfer_pending) {
        xSemaphoreTake(bus->transfer_done_sem, portMAX_DELAY);
        bus->transfer_pending = false;
    }
}

/*
 * The driver of IDF under this bus sends a command and then its data in one
 * transfer. Data alone it sends only on the chips whose bus is the LCD_CAM
 * peripheral, ESP32-S3 and ESP32-P4, when the command is -1. On an ESP32 and
 * an ESP32-S2 the bus is made of the I2S peripheral, and that driver sends
 * the command byte before every block whatever the command is: -1 goes out
 * as 0xFF, and the display takes the pixels after it for parameters of that.
 *
 * So nothing here is sent as data alone when it can be helped. A command
 * waits for its data and goes in one transfer with it, and a screen on this
 * bus sends every block of pixels with the command that writes memory, see
 * dgx_screen_with_bus_write_window().
 */

/* the parameters of a command go through a buffer of the driver, LCD_I80_IO_FORMAT_BUF_SIZE of IDF; more than that goes as pixels do */
#define DGX_P8_PARAM_BYTES 32u

/* the command that was held back has no data */
static void dgx_p8_flush_command(dgx_p8_bus_t *bus) {
    if (!bus->command_pending) return;
    bus->command_pending = false;
    dgx_p8_wait_pending(bus);
    esp_err_t ret = esp_lcd_panel_io_tx_param(bus->io, bus->command, NULL, 0);
    if (ret != ESP_OK) ESP_LOGE(TAG, "command 0x%02x failed: %s", bus->command, esp_err_to_name(ret));
}

/*
 * The command the data that follows belongs to: the one held back, or -1
 * for data alone. false when data alone cannot be sent on this chip.
 */
static bool dgx_p8_take_command(dgx_p8_bus_t *bus, int *command) {
    if (bus->command_pending) {
        bus->command_pending = false;
        *command = bus->command;
        return true;
    }
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    *command = -1;
    return true;
#else
    if (!bus->complained) {
        bus->complained = true;
        ESP_LOGE(TAG, "data with no command before it cannot be sent on this chip, it is dropped");
    }
    return false;
#endif
}

static void dgx_p8_send_command(struct _dgx_bus_protocols_t *_bus, uint8_t cmd) {
    dgx_p8_bus_t *bus = (dgx_p8_bus_t *)_bus;
    dgx_p8_flush_command(bus);
    bus->command = cmd;
    bus->command_pending = true;
}

static void dgx_p8_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len) {
    dgx_p8_bus_t *bus = (dgx_p8_bus_t *)_bus;
    uint32_t byte_len = (len + 7u) / 8u;
    for (uint32_t i = 0; i < byte_len; i++) {
        dgx_p8_send_command(_bus, cmds[i]);
    }
    dgx_p8_flush_command(bus);
}

static void dgx_p8_send_data_async(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len) {
    dgx_p8_bus_t *bus = (dgx_p8_bus_t *)_bus;
    uint32_t byte_len = (len + 7u) / 8u;
    int command;
    if (byte_len == 0) return;
    if (!dgx_p8_take_command(bus, &command)) return;
    dgx_p8_wait_pending(bus);
    bus->transfer_pending = true;
    esp_err_t ret = esp_lcd_panel_io_tx_color(bus->io, command, data, byte_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_io_tx_color failed: %s", esp_err_to_name(ret));
        bus->transfer_pending = false; // nothing was queued: no completion callback will come
    }
}

static void dgx_p8_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len) {
    dgx_p8_bus_t *bus = (dgx_p8_bus_t *)_bus;
    uint32_t byte_len = (len + 7u) / 8u;
    if (byte_len == 0) return;
    if (byte_len > DGX_P8_PARAM_BYTES) {
        /* too much for the parameters of a command: sent as pixels are, and waited for */
        dgx_p8_send_data_async(_bus, data, len);
        dgx_p8_wait_pending(bus);
        return;
    }
    int command;
    if (!dgx_p8_take_command(bus, &command)) return;
    dgx_p8_wait_pending(bus);
    esp_err_t ret = esp_lcd_panel_io_tx_param(bus->io, command, data, byte_len);
    if (ret != ESP_OK) ESP_LOGE(TAG, "command 0x%02x with %u bytes failed: %s", (unsigned)(command & 0xff), (unsigned)byte_len, esp_err_to_name(ret));
}

static void dgx_p8_sync_write_func(struct _dgx_bus_protocols_t *_bus) {
    dgx_p8_bus_t *bus = (dgx_p8_bus_t *)_bus;
    dgx_p8_flush_command(bus);
    dgx_p8_wait_pending(bus);
}

static uint32_t dgx_p8_read_data(struct _dgx_bus_protocols_t *_bus, uint8_t *data, uint32_t len) {
    memset(data, 0, (len + 7u) / 8u);
    return len;
}

static void dgx_p8_dispose_bus_func(struct _dgx_bus_protocols_t *_bus) {
    dgx_p8_bus_t *bus = (dgx_p8_bus_t *)_bus;
    if (bus->io) dgx_p8_flush_command(bus);
    dgx_p8_wait_pending(bus);
    if (bus->io) esp_lcd_panel_io_del(bus->io);
    if (bus->i80_bus) esp_lcd_del_i80_bus(bus->i80_bus);
    if (bus->transfer_done_sem) vSemaphoreDelete(bus->transfer_done_sem);
    free(bus->protocols.buffer);
    free(bus);
}

static uint8_t *dgx_p8_memory_allocator(uint32_t len) {
    return (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_DMA);
}

dgx_bus_protocols_t *dgx_p8_init(gpio_num_t lcd_d0, gpio_num_t lcd_d1, gpio_num_t lcd_d2, gpio_num_t lcd_d3, gpio_num_t lcd_d4, gpio_num_t lcd_d5,
        gpio_num_t lcd_d6, gpio_num_t lcd_d7, gpio_num_t lcd_wr, gpio_num_t lcd_rd, gpio_num_t cs, gpio_num_t dc, int pclk_hz)
{
    dgx_p8_bus_t *bus = (dgx_p8_bus_t *)calloc(1, sizeof(dgx_p8_bus_t));
    if (!bus) {
        ESP_LOGE(TAG, "dgx_p8_init failed to allocate dgx_p8_bus_t");
        return NULL;
    }

    // Create I80 bus
    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = dc,
        .wr_gpio_num = lcd_wr,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = { lcd_d0, lcd_d1, lcd_d2, lcd_d3, lcd_d4, lcd_d5, lcd_d6, lcd_d7 },
        .bus_width = 8,
        .max_transfer_bytes = 4096,
        .dma_burst_size = 64,
    };
    esp_err_t ret = esp_lcd_new_i80_bus(&bus_config, &bus->i80_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_i80_bus failed: %s", esp_err_to_name(ret));
        free(bus);
        return NULL;
    }

    // Create panel IO
    bus->transfer_done_sem = xSemaphoreCreateBinary();
    if (!bus->transfer_done_sem) {
        ESP_LOGE(TAG, "failed to create semaphore");
        esp_lcd_del_i80_bus(bus->i80_bus);
        free(bus);
        return NULL;
    }

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = cs,
        .pclk_hz = pclk_hz,
        .trans_queue_depth = 10,
        .on_color_trans_done = dgx_p8_on_color_trans_done,
        .user_ctx = bus,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .flags = {
            .pclk_idle_low = 0,
        },
    };
    ret = esp_lcd_new_panel_io_i80(bus->i80_bus, &io_config, &bus->io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i80 failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(bus->transfer_done_sem);
        esp_lcd_del_i80_bus(bus->i80_bus);
        free(bus);
        return NULL;
    }

    // Set RD pin high (inactive) — I80 peripheral doesn't manage it
    if (lcd_rd >= 0) {
        dgx_gpio_set_direction(lcd_rd, GPIO_MODE_OUTPUT);
        gpio_set_level(lcd_rd, 1);
    }

    bus->protocols.bus_type = DGX_BUS_P8;
    bus->protocols.bus_name = "P8";
    bus->protocols.write_command = dgx_p8_send_command;
    bus->protocols.write_commands = dgx_p8_send_commands;
    bus->protocols.read_data = dgx_p8_read_data;
    bus->protocols.write_data = dgx_p8_send_data;
    bus->protocols.write_data_async = dgx_p8_send_data_async;
    bus->protocols.sync_write = dgx_p8_sync_write_func;
    bus->protocols.dispose = dgx_p8_dispose_bus_func;
    bus->protocols.memory_allocator = dgx_p8_memory_allocator;
    bus->protocols.buffer_len = 4000;
    bus->protocols.buffer = bus->protocols.memory_allocator(bus->protocols.buffer_len);
    if (!bus->protocols.buffer) {
        ESP_LOGE(TAG, "dgx_p8_init buffer allocation failed");
        dgx_p8_dispose_bus_func(&bus->protocols);
        return NULL;
    }
    bus->transfer_pending = false;
    bus->command_pending = false;
    return (dgx_bus_protocols_t *)bus;
}
