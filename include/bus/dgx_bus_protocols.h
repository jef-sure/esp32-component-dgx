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
    DGX_BUS_P8,      //
    DGX_BUS_DSI      //
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

/**
 * @brief Put rows of pixels straight into a rectangle of the display, when the bus has a faster way than its window.
 *
 * @param left,top     The rectangle on the screen, inside it.
 * @param width,height Its size in pixels.
 * @param rows         The pixels as DGX keeps them in a buffer, row after row.
 * @param pitch        Bytes from the start of a row to the start of the next one.
 * @return false when the bus has not done it: the pixels are to be sent through the window then.
 */
typedef bool (*dgx_bus_write_rows_func)(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                                        const uint8_t *rows, uint32_t pitch);

/**
 * @brief Put a picture of amounts of ink into a rectangle of the display, when the bus has hardware that mixes two colors.
 *
 * A byte of the picture is an amount of ink: 0 leaves the pixel the color of
 * the paper, 255 gives it the color of the ink, what is between mixes the two.
 *
 * @param left,top     The rectangle on the screen, inside it.
 * @param width,height Its size in pixels.
 * @param ink          The amounts, a byte a pixel, row after row.
 * @param pitch        Bytes from the start of a row to the start of the next one.
 * @param paper,color  The two colors, as the screen takes a color.
 * @return false when the bus has not done it: the pixels are to be sent another way then.
 */
typedef bool (*dgx_bus_write_ink_func)(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                                       const uint8_t *ink, uint32_t pitch, uint32_t paper, uint32_t color);

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
    dgx_bus_write_rows_func write_rows; /*!< NULL for a bus that has only its window */
    dgx_bus_write_ink_func write_ink;   /*!< NULL for a bus that cannot mix two colors by an amount itself */
} dgx_bus_protocols_t;

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

