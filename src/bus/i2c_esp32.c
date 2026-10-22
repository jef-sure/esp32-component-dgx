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

static const char TAG[] = "DGX I2C BUS";

/*
 * How long a transfer of so many bytes may take before the bus is taken to be
 * stuck or the device missing: four times what the bytes take at the clock of
 * the bus, and 20 ms more. Without a limit such a transfer waits forever.
 */
static int dgx_i2c_timeout_ms(const dgx_i2c_bus_t *bus, uint32_t bytes)
{
    const uint32_t hz = bus->clock_speed_hz > 0 ? (uint32_t)bus->clock_speed_hz : 100000u;
    return (int)(20u + 4u * (bytes * 9000u / hz));
}

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
    RCCHECK(i2c_master_transmit(bus->dev_handle, buffer, 2, dgx_i2c_timeout_ms(bus, 2)), "dgx_i2c_send_command failed");
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
    RCCHECK(i2c_master_transmit(bus->dev_handle, _bus->buffer, len + 1, dgx_i2c_timeout_ms(bus, len + 1)), "dgx_i2c_send_commands failed");
}

static void dgx_i2c_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_i2c_bus_t   *bus = (dgx_i2c_bus_t *)_bus;
    esp_err_t        rc;
    len        = (len + 7u) / 8u;
    if(_bus->buffer_len < len + 1) {
        void *temp_buffer = realloc(_bus->buffer, len + 1);
        if(temp_buffer == NULL) {
            ESP_LOGE(TAG, "dgx_i2c_send_data failed to allocate buffer");
            return;
        }
        _bus->buffer = (uint8_t *)temp_buffer;
        _bus->buffer_len = len + 1;
    }
    _bus->buffer[0] = bus->data_stream;
    memcpy(&_bus->buffer[1], data, len);
    RCCHECK(i2c_master_transmit(bus->dev_handle, _bus->buffer, len + 1, dgx_i2c_timeout_ms(bus, len + 1)), "dgx_i2c_send_data failed");
}

static void dgx_i2c_send_data_async(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_i2c_send_data(_bus, data, len);
}

/* the display is not read through this bus: nothing is read, and the buffer is left as it was */
static uint32_t dgx_i2c_read_data(struct _dgx_bus_protocols_t *_bus, uint8_t *data, uint32_t len)
{
    (void)_bus, (void)data, (void)len;
    return 0;
}

/*
 * The master buses DGX creates, one a port, shared by the DGX displays on that
 * port and deleted with the last of them. A bus the program created and gave
 * to dgx_i2c_init_on_bus() is the program's: DGX only adds its device to it and
 * removes it again.
 */
static i2c_master_bus_handle_t dgx_i2c_buses[I2C_NUM_MAX];
static uint16_t                dgx_i2c_users[I2C_NUM_MAX];
static gpio_num_t              dgx_i2c_sda[I2C_NUM_MAX];
static gpio_num_t              dgx_i2c_scl[I2C_NUM_MAX];

static esp_err_t dgx_i2c_take_port(i2c_port_t i2c_num, gpio_num_t sda, gpio_num_t sclk, i2c_master_bus_handle_t *out)
{
    if (i2c_num < 0 || i2c_num >= I2C_NUM_MAX) {
        ESP_LOGE(TAG, "I2C port %d does not exist", (int)i2c_num);
        return ESP_ERR_INVALID_ARG;
    }
    if (dgx_i2c_buses[i2c_num]) {
        if (dgx_i2c_sda[i2c_num] != sda || dgx_i2c_scl[i2c_num] != sclk) {
            ESP_LOGE(TAG, "I2C port %d already uses SDA %d and SCL %d, not SDA %d and SCL %d", (int)i2c_num, (int)dgx_i2c_sda[i2c_num],
                     (int)dgx_i2c_scl[i2c_num], (int)sda, (int)sclk);
            return ESP_ERR_INVALID_ARG;
        }
        *out = dgx_i2c_buses[i2c_num];
        return ESP_OK;
    }
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = i2c_num,
        .sda_io_num                   = sda,
        .scl_io_num                   = sclk,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t rc = i2c_new_master_bus(&bus_cfg, &dgx_i2c_buses[i2c_num]);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "I2C port %d: no master bus (%s); a port the program has taken itself goes to dgx_i2c_init_on_bus()", (int)i2c_num,
                 esp_err_to_name(rc));
        dgx_i2c_buses[i2c_num] = NULL;
        return rc;
    }
    dgx_i2c_sda[i2c_num] = sda;
    dgx_i2c_scl[i2c_num] = sclk;
    *out                 = dgx_i2c_buses[i2c_num];
    return ESP_OK;
}

/* the bus of a port deleted when no DGX display uses it; kept when the program has added devices of its own to it */
static void dgx_i2c_release_port(i2c_port_t i2c_num)
{
    if (dgx_i2c_users[i2c_num] || !dgx_i2c_buses[i2c_num]) return;
    esp_err_t rc = i2c_del_master_bus(dgx_i2c_buses[i2c_num]);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "I2C port %d: master bus kept, other devices are on it (%s)", (int)i2c_num, esp_err_to_name(rc));
        return;
    }
    dgx_i2c_buses[i2c_num] = NULL;
}

static void dgx_i2c_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_i2c_bus_t *bus = (dgx_i2c_bus_t *)_bus;
    esp_err_t      rc;
    RCCHECK(i2c_master_bus_rm_device(bus->dev_handle), "dgx_i2c_dispose failed to remove I2C device");
    if (bus->owns_master_bus && dgx_i2c_users[bus->i2c_num]) {
        --dgx_i2c_users[bus->i2c_num];
        dgx_i2c_release_port(bus->i2c_num);
    }
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

/* the display's device added to a master bus: the part both constructors share */
static dgx_bus_protocols_t *dgx_i2c_attach(i2c_master_bus_handle_t bus_handle, bool owns_master_bus, i2c_port_t i2c_num, uint8_t i2c_address,
                                           gpio_num_t sda, gpio_num_t sclk, int clock_speed_hz)
{
    esp_err_t rc = i2c_master_probe(bus_handle, i2c_address, dgx_i2c_timeout_ms(&(dgx_i2c_bus_t){.clock_speed_hz = clock_speed_hz}, 1));
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "no device answers at 0x%02X: %s", i2c_address, esp_err_to_name(rc));
        return NULL;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = i2c_address,
        .scl_speed_hz    = clock_speed_hz,
    };
    i2c_master_dev_handle_t dev_handle;
    rc = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "the device 0x%02X is not added to the I2C bus: %s", i2c_address, esp_err_to_name(rc));
        return NULL;
    }
    dgx_i2c_bus_t *bus = (dgx_i2c_bus_t *)calloc(1, sizeof(dgx_i2c_bus_t));
    if (bus == 0) {
        ESP_LOGE(TAG, "dgx_i2c_init failed to allocate dgx_i2c_bus_t");
        i2c_master_bus_rm_device(dev_handle);
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
    bus->owns_master_bus             = owns_master_bus;
    return (dgx_bus_protocols_t *)bus;
}

dgx_bus_protocols_t *dgx_i2c_init(i2c_port_t i2c_num, uint8_t i2c_address, gpio_num_t sda, gpio_num_t sclk, int clock_speed_hz)
{
    i2c_master_bus_handle_t bus_handle;
    if (dgx_i2c_take_port(i2c_num, sda, sclk, &bus_handle) != ESP_OK) return NULL;
    dgx_bus_protocols_t *bus = dgx_i2c_attach(bus_handle, true, i2c_num, i2c_address, sda, sclk, clock_speed_hz);
    if (!bus) {
        dgx_i2c_release_port(i2c_num);
        return NULL;
    }
    ++dgx_i2c_users[i2c_num];
    return bus;
}

dgx_bus_protocols_t *dgx_i2c_init_on_bus(i2c_master_bus_handle_t i2c_bus, uint8_t i2c_address, int clock_speed_hz)
{
    if (!i2c_bus) {
        ESP_LOGE(TAG, "dgx_i2c_init_on_bus: no master bus");
        return NULL;
    }
    return dgx_i2c_attach(i2c_bus, false, I2C_NUM_MAX, i2c_address, GPIO_NUM_NC, GPIO_NUM_NC, clock_speed_hz);
}
