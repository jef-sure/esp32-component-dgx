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
    DGX_BUS_DSI,     //
    DGX_BUS_QSPI     //
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

/**
 * @brief An overlay layer as the bus sees it: 8-bit indices over a part of the screen, mixed with what goes under them.
 *
 * Index 0 is empty and leaves the pixel under it as it is. Another index
 * gives its color mixed with the pixel under it: `pre[index]` is the color
 * times the weight of the layer, and `under` the weight of the pixel under
 * it, the two adding up to 32. A color is spread over a word as
 * 0x07E0F81F masks it: green in the high half, red and blue in the low one.
 */
typedef struct {
    const uint8_t  *pixels; /*!< the layer, row after row */
    uint32_t        pitch;  /*!< bytes from a row to the next one */
    int32_t         x;      /*!< where its pixel (0, 0) lies on the screen */
    int32_t         y;
    /*! the part of the screen outside which the layer has nothing, both ends included; inside the screen and the layer */
    int32_t         left, top, right, bottom;
    const uint32_t *pre;    /*!< 256 colors times the weight of the layer, spread */
    uint32_t        under;  /*!< the weight of what lies under the layer, 0 to 32 */
    /*! a byte for every 32 by 32 pixels of the layer, row after row, 0 where it has nothing; NULL when it may have something anywhere */
    const uint8_t  *tiles;
    uint32_t        tiles_across; /*!< tiles in a row of them */
} dgx_bus_overlay_t;

/** @brief The tiles of a layer are 1 << this pixels on a side. */
#define DGX_BUS_OVERLAY_TILE_SHIFT 5

/** @brief A color of the layer mixed with the RGB565 color under it. */
static inline uint16_t dgx_bus_overlay_mix(uint16_t color, uint32_t pre, uint32_t under)
{
    uint32_t spread = (color | (uint32_t)color << 16) & 0x07E0F81Fu;
    spread          = ((spread * under + pre) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(spread | spread >> 16);
}

/** @brief dgx_bus_indexed_t::smooth: Scale2x at scale 2, Scale3x at 3, Scale2x twice at 4. */
#define DGX_BUS_SMOOTH 1
/** @brief dgx_bus_indexed_t::smooth: Scale2xSFX at scale 2, Scale3xSFX at 3, Scale2xSFX twice at 4. */
#define DGX_BUS_SMOOTH_SFX 2

/**
 * @brief A picture of 8-bit indices put through a table of colors, every index a square of `scale` pixels.
 *
 * With `smooth`, the picture is made larger by Scale2x or Scale3x on the
 * indices, or by their SFX variants: a corner of a square where two equal
 * neighbours meet takes their index. The smoothing looks at the neighbours
 * of the pixels on the edge of a rectangle too, so the whole picture is
 * given; past the edges of the picture its last pixel goes on.
 *
 * The larger picture is turned by `orientation` on the screen, around its
 * own corner: with DgxOutputRotate90CW its left column is the top row of
 * the rectangle. Smoothing is the same turned or not.
 */
typedef struct {
    const uint8_t  *pixels;      /*!< the whole picture, row after row */
    uint32_t        pitch;       /*!< bytes from a row to the next one */
    uint32_t        width;       /*!< of the whole picture */
    uint32_t        height;
    uint32_t        x;           /*!< where the pixel (left, top) of the rectangle lies in the picture made `scale` times as large and turned */
    uint32_t        y;
    uint8_t         scale;       /*!< 1 to 255 */
    uint8_t         smooth;      /*!< 0, DGX_BUS_SMOOTH or DGX_BUS_SMOOTH_SFX; only at scale 2, 3 and 4 */
    uint8_t         orientation; /*!< a dgx_output_orientation_t: bit 0 the columns of the picture counted from the right, bit 1 its rows from the bottom, bit 2 its axes swapped */
    const uint16_t *lut;         /*!< the 256 colors, RGB565 */
    /*! the overlay layer of the screen to mix in, NULL when none or when it is not over the rectangle */
    const dgx_bus_overlay_t *overlay;
} dgx_bus_indexed_t;

/**
 * @brief Put a picture of 8-bit indices into a rectangle of the display through a table of colors, when the bus has a faster way than its window.
 *
 * @param left,top     The rectangle on the screen, inside it.
 * @param width,height Its size in pixels of the screen.
 * @param picture      What goes there.
 * @return false when the bus has not done it, and has put nothing: the pixels are to be sent another way then.
 */
typedef bool (*dgx_bus_write_indexed_func)(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                                           const dgx_bus_indexed_t *picture);

/**
 * @brief Move the pixels of a rectangle of the display to another place of it, when the pixels are memory of the bus.
 *
 * The two places may overlap; what the picture leaves behind stays as it was.
 *
 * @param left,top       The rectangle on the screen, inside it.
 * @param width,height   Its size in pixels.
 * @param to_left,to_top Where its corner goes; the rectangle is inside the screen there too.
 * @return false when the bus has not done it: the pixels are to be moved another way then.
 */
typedef bool (*dgx_bus_move_func)(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, uint16_t to_left,
                                  uint16_t to_top);

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
    dgx_bus_move_func move;             /*!< NULL for a bus whose pixels are not memory at hand */
    dgx_bus_write_indexed_func write_indexed; /*!< NULL for a bus that has only its window for indices */
    /*!
     * The last pixel of the window has been handed over: a bus that holds its
     * chip select over all the pixels of a window lets it go when they are
     * sent. NULL for a bus that does not hold it.
     */
    dgx_bus_sync_write_func end_pixels;
} dgx_bus_protocols_t;

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

