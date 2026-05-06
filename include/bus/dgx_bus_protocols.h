#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dgx_arch.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

enum dgx_bus_type {  //
    DGX_BUS_SPI,     //
    DGX_BUS_I2C,     //
    DGX_BUS_P8       //
};

enum {                 //
    DGX_BUS_READ = 0,  //
    DGX_BUS_WRITE = 1  //
};

struct _dgx_bus_protocols_t;

/*
 * Bus functions
 */
typedef void (*dgx_bus_send_command_func)(struct _dgx_bus_protocols_t *bus, uint8_t cmd);
typedef uint8_t *(*dgx_bus_malloc_func)(uint32_t len);

/*
 * len is always in bits
 */
typedef void (*dgx_bus_write_commands_func)(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len);
typedef void (*dgx_bus_write_data_func)(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len);
typedef uint32_t (*dgx_bus_read_data_func)(struct _dgx_bus_protocols_t *bus, uint8_t *data, uint32_t len);
typedef void (*dgx_bus_write_data_async_func)(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len);
typedef void (*dgx_bus_sync_write_func)(struct _dgx_bus_protocols_t *bus);

/*
 * Special protocol functions
 */
typedef void (*dgx_bus_dispose_bus_func)(struct _dgx_bus_protocols_t *bus);

typedef struct _dgx_bus_protocols_t {
    enum dgx_bus_type bus_type;
    const char *bus_name;
    dgx_bus_send_command_func write_command;
    dgx_bus_write_commands_func write_commands;
    dgx_bus_write_data_func write_data;
    dgx_bus_read_data_func read_data;
    dgx_bus_write_data_async_func write_data_async;
    dgx_bus_sync_write_func sync_write;
    dgx_bus_malloc_func memory_allocator;
    dgx_bus_dispose_bus_func dispose;
    size_t buffer_len;
    uint8_t *buffer;
} dgx_bus_protocols_t;

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

