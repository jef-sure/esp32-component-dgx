/*
 * i2c_esp32.c
 *
 *  Created on: 14.10.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bus/dgx_i2c_esp32.h"

#define DGX_I2C_TIMEOUT 10

static const char TAG[] = "DGX I2C BUS";

#define RCCHECK(exp, err)       \
    do {                        \
        rc = exp;               \
        if (rc != ESP_OK) {     \
            ESP_LOGE(TAG, err); \
        }                       \
    } while (0)

static void dgx_i2c_send_command(struct _dgx_bus_protocols_t *_bus, uint8_t cmd)
{
    dgx_i2c_bus_t   *bus = (dgx_i2c_bus_t *)_bus;
    esp_err_t        rc;
    uint8_t buffer[2];
    buffer[0] = bus->cmd_single;
    buffer[1] = cmd;
    RCCHECK(i2c_master_transmit(bus->dev_handle, buffer, 2, -1), "dgx_i2c_send_command failed");
}

static void dgx_i2c_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len)
{
    dgx_i2c_bus_t   *bus = (dgx_i2c_bus_t *)_bus;
    esp_err_t        rc;
    len        = (len + 7u) / 8u;
    if(_bus->buffer_len < len + 1) {
        void *temp_buffer = realloc(_bus->buffer, len + 1);
        if(temp_buffer == NULL) {
            ESP_LOGE(TAG, "dgx_i2c_send_commands failed to allocate buffer");
            return;
        }
        _bus->buffer = (uint8_t *)temp_buffer;
        _bus->buffer_len = len + 1;
    }
    _bus->buffer[0] = bus->cmd_stream;
    memcpy(&_bus->buffer[1], cmds, len);
    RCCHECK(i2c_master_transmit(bus->dev_handle, _bus->buffer, len + 1, -1), "dgx_i2c_send_commands failed");
}

static void dgx_i2c_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_i2c_bus_t   *bus = (dgx_i2c_bus_t *)_bus;
    esp_err_t        rc;
    len        = (len + 7u) / 8u;
    if(_bus->buffer_len < len + 1) {
        void *temp_buffer = realloc(_bus->buffer, len + 1);
        if(temp_buffer == NULL) {
            ESP_LOGE(TAG, "dgx_i2c_send_commands failed to allocate buffer");
            return;
        }
        _bus->buffer = (uint8_t *)temp_buffer;
        _bus->buffer_len = len + 1;
    }
    _bus->buffer[0] = bus->data_stream;
    memcpy(&_bus->buffer[1], data, len);
    RCCHECK(i2c_master_transmit(bus->dev_handle, _bus->buffer, len + 1, -1), "dgx_i2c_send_data failed");
}

static void dgx_i2c_send_data_async(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_i2c_send_data(_bus, data, len);
}

static uint32_t dgx_i2c_read_data(struct _dgx_bus_protocols_t *_bus, uint8_t *data, uint32_t len)
{
    memset(data, 0, len / 8);
    return len;
}

static void dgx_i2c_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_i2c_bus_t *bus = (dgx_i2c_bus_t *)_bus;
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(bus->dev_handle));
    ESP_ERROR_CHECK(i2c_del_master_bus(bus->bus_handle));
    free(bus->protocols.buffer);
    free(bus);
}

static uint8_t *dgx_i2c_memory_allocator(uint32_t len)
{
    return (uint8_t *)malloc(len);
}

static void dgx_i2c_sync_write(struct _dgx_bus_protocols_t *_bus)
{
}

dgx_bus_protocols_t *dgx_i2c_init(i2c_port_t i2c_num, uint8_t i2c_address, gpio_num_t sda, gpio_num_t sclk, int clock_speed_hz)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = i2c_num, // e.g., I2C_NUM_0
        .sda_io_num                   = sda,
        .scl_io_num                   = sclk,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,    // Recommended default for noise filtering
        .flags.enable_internal_pullup = true, // Replaces SDA/SCL pullup_en
    };

    esp_err_t               rc;
    i2c_master_bus_handle_t bus_handle;
    RCCHECK(i2c_new_master_bus(&bus_cfg, &bus_handle), "dgx_i2c_init failed to create I2C master bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, //
        .device_address  = i2c_address,        //
        .scl_speed_hz    = clock_speed_hz,     //
    };

    i2c_master_dev_handle_t dev_handle;
    rc = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle); 
    if (rc != ESP_OK) { 
        ESP_LOGE(TAG, "dgx_i2c_init failed to add I2C device"); 
        i2c_del_master_bus(bus_handle);
        return NULL;
    }
    dgx_i2c_bus_t *bus = (dgx_i2c_bus_t *)calloc(1, sizeof(dgx_i2c_bus_t));
    if (bus == 0) {
        ESP_LOGE(TAG, "dgx_i2c_init failed to allocate dgx_i2c_bus_t");
        i2c_master_bus_rm_device(dev_handle);
        i2c_del_master_bus(bus_handle);
        return NULL;
    }
    bus->protocols.bus_type          = DGX_BUS_I2C;
    bus->protocols.bus_name          = "I2C";
    bus->protocols.write_command     = dgx_i2c_send_command;
    bus->protocols.write_commands    = dgx_i2c_send_commands;
    bus->protocols.read_data         = dgx_i2c_read_data;
    bus->protocols.write_data        = dgx_i2c_send_data;
    bus->protocols.write_data_async  = dgx_i2c_send_data_async;
    bus->protocols.sync_write        = dgx_i2c_sync_write;
    bus->protocols.dispose           = dgx_i2c_dispose_bus_func;
    bus->protocols.memory_allocator  = dgx_i2c_memory_allocator;
    bus->protocols.buffer_len        = 0;
    bus->protocols.buffer            = NULL;
    bus->i2c_address                 = i2c_address;
    bus->i2c_num                     = i2c_num;
    bus->sda                         = sda;
    bus->sclk                        = sclk;
    bus->clock_speed_hz              = clock_speed_hz;
    bus->dev_handle                  = dev_handle;
    bus->bus_handle                  = bus_handle;
    return (dgx_bus_protocols_t *)bus;
}
