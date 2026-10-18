#pragma once
/*
 * dgx_fb_area_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  A frame buffer that is written the way a display controller is: a window
 *  is set, then pixels follow in the order DGX sends them to any display,
 *  the high byte of a pixel first. Here they are put into a frame buffer a
 *  video interface scans out, in the byte order that buffer has.
 *
 *  Nothing here depends on the platform: a bus keeps one and gives it the
 *  memory.
 */

#include <stdbool.h>
#include <stdint.h>

#include "../dgx_indexed_priv.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef struct _dgx_fb_area_t {
    uint8_t *fb;          /* the frame buffer: rows of the panel one after another */
    uint16_t fb_width;    /* of the panel, in pixels */
    uint16_t fb_height;
    uint8_t  pixel_bytes; /* 2: RGB565, low byte first; 3: blue, green, red */
    bool     swap_rb;     /* the panel takes red and blue the other way round */
    /* as the MX, MY and MV bits of a display controller */
    bool     mirror_x;    /* the columns of the panel are counted from the right */
    bool     mirror_y;    /* the rows of the panel are counted from the bottom */
    bool     swap_xy;     /* x of the screen runs along the rows of the panel */
    /* the window in screen coordinates, both ends included, and the next pixel in it */
    uint16_t left, right, top, bottom;
    uint16_t x, y;
    /* bytes of the frame buffer written since the range was last taken; none when dirty_to <= dirty_from */
    uint32_t dirty_from, dirty_to;
    /* about how many bytes of a cache that works by lines of 64 bytes what was written since then takes */
    uint32_t dirty_weight;
    uint8_t  command;     /* the command the data that follows belongs to, 0 when it is not one of the window */
} dgx_fb_area_t;

/* the commands of the MIPI display command set a frame buffer answers itself */
#define DGX_FB_AREA_SET_COLUMNS    0x2A
#define DGX_FB_AREA_SET_ROWS       0x2B
#define DGX_FB_AREA_WRITE          0x2C
#define DGX_FB_AREA_READ           0x2E
#define DGX_FB_AREA_WRITE_CONTINUE 0x3C
#define DGX_FB_AREA_READ_CONTINUE  0x3E

/** @brief Width of the screen: of the panel, or its height when the axes are swapped. */
static inline uint16_t dgx_fb_area_width(const dgx_fb_area_t *area)
{
    return area->swap_xy ? area->fb_height : area->fb_width;
}

/** @brief Height of the screen. */
static inline uint16_t dgx_fb_area_height(const dgx_fb_area_t *area)
{
    return area->swap_xy ? area->fb_width : area->fb_height;
}

/** @brief Take a frame buffer; the window becomes the whole screen. */
void dgx_fb_area_init(dgx_fb_area_t *area, uint8_t *fb, uint16_t fb_width, uint16_t fb_height, uint8_t pixel_bytes, bool swap_rb);

/** @brief Set how the screen lies on the panel; the window becomes the whole screen. */
void dgx_fb_area_orientation(dgx_fb_area_t *area, bool mirror_x, bool mirror_y, bool swap_xy);

/** @brief Set the columns of the window, both included; they are cut by the screen. */
void dgx_fb_area_set_columns(dgx_fb_area_t *area, uint16_t from, uint16_t to);

/** @brief Set the rows of the window, both included; they are cut by the screen. */
void dgx_fb_area_set_rows(dgx_fb_area_t *area, uint16_t from, uint16_t to);

/** @brief Put the next pixel to the top-left corner of the window. */
static inline void dgx_fb_area_rewind(dgx_fb_area_t *area)
{
    area->x = area->left;
    area->y = area->top;
}

/**
 * @brief Write pixels into the window, row by row; after the last pixel of the window comes its first.
 * @param data   Pixels as DGX keeps them in a buffer: two bytes, the high one first, or red, green, blue.
 * @param pixels Their number.
 */
void dgx_fb_area_write(dgx_fb_area_t *area, const uint8_t *data, uint32_t pixels);

/** @brief Read pixels of the window the same way, into a buffer as DGX keeps them. */
void dgx_fb_area_read(dgx_fb_area_t *area, uint8_t *data, uint32_t pixels);

/*
 * Drawing past the window: a screen that has the memory at hand does not
 * need to set a window for a pixel or a rectangle. The window and its next
 * pixel stay as they are. The coordinates have to be inside the screen.
 */

/** @brief Set one pixel; the color is as DGX passes colors, red in the high bits. */
void dgx_fb_area_set_pixel(dgx_fb_area_t *area, uint16_t x, uint16_t y, uint32_t color);

/** @brief Read one pixel. */
uint32_t dgx_fb_area_get_pixel(const dgx_fb_area_t *area, uint16_t x, uint16_t y);

/** @brief Fill a rectangle that lies inside the screen. */
void dgx_fb_area_fill(dgx_fb_area_t *area, uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint32_t color);

/**
 * @brief Move the pixels of a rectangle of the screen to another place of it; both lie inside the screen.
 *
 * A rectangle of the screen is a rectangle of the panel however the screen
 * lies on it, and a move of the one is a move of the other: rows of the
 * frame buffer are moved, each at once, from the side the picture goes to.
 * The two places may overlap.
 *
 * @param x,y          The corner of the rectangle.
 * @param width,height Its size.
 * @param to_x,to_y    Where its corner goes.
 */
void dgx_fb_area_move(dgx_fb_area_t *area, uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t to_x, uint16_t to_y);

/**
 * @brief Where a rectangle of the screen lies on the panel.
 *
 * @param left,right,top,bottom The rectangle in screen coordinates, both ends included, inside the screen.
 * @param x0,y0                 Out: its first column and row on the panel.
 * @param x1,y1                 Out: its last column and row on the panel.
 */
void dgx_fb_area_to_panel(const dgx_fb_area_t *area, int left, int right, int top, int bottom, uint16_t *x0, uint16_t *y0, uint16_t *x1,
                          uint16_t *y1);

/** @brief Where a pixel of the screen lies on the panel; it has to be inside the screen. */
static inline void dgx_fb_area_point_to_panel(const dgx_fb_area_t *area, int x, int y, uint16_t *px, uint16_t *py)
{
    uint32_t ax = area->swap_xy ? (uint32_t)y : (uint32_t)x, ay = area->swap_xy ? (uint32_t)x : (uint32_t)y;
    *px = (uint16_t)(area->mirror_x ? area->fb_width - 1u - ax : ax);
    *py = (uint16_t)(area->mirror_y ? area->fb_height - 1u - ay : ay);
}

/**
 * @brief Make rows of the panel from a picture of 8-bit indices through a table of colors.
 *
 * The rectangle `left, top, width, height` of the screen lies on the panel
 * as dgx_fb_area_to_panel() tells. This makes `rows` of its rows on the
 * panel, from its row `first` on, counted from its top there, into `to`:
 * one after another, as many pixels a row as the rectangle is wide on the
 * panel, in the byte order of the frame buffer. They are ready to be copied
 * into it as they are, whatever the orientation. Only for 2 bytes a pixel
 * and red and blue as the panel takes them.
 *
 * @param picture What goes into the rectangle, see dgx_bus_indexed_t.
 * @param lines   For a smoothed picture, what dgx_fb_area_indexed_lines() has begun for the same rectangle; NULL will do for one that is not.
 */
void dgx_fb_area_indexed_rows(const dgx_fb_area_t *area, uint8_t *to, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                              const dgx_bus_indexed_t *picture, uint16_t first, uint16_t rows, dgx_indexed_lines_t *lines);

/** @brief Bytes of scratch dgx_fb_area_indexed_lines() needs for a rectangle `columns` wide on the panel. */
static inline size_t dgx_fb_area_indexed_scratch(uint32_t columns)
{
    return dgx_indexed_lines_scratch(columns);
}

/**
 * @brief Begin the lines of a smoothed picture for a rectangle, once for all the rows of it dgx_fb_area_indexed_rows() makes.
 *
 * The rows made one call after another, in order, share what the lines of
 * the smaller pictures are made of; `scratch` holds
 * dgx_fb_area_indexed_scratch() bytes and lasts as long as they are made.
 */
void dgx_fb_area_indexed_lines(const dgx_fb_area_t *area, dgx_indexed_lines_t *lines, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                               const dgx_bus_indexed_t *picture, uint8_t *scratch);

/**
 * @brief Take a command if it is one of the window: set the columns or the rows, write or read the memory.
 * @return false when the command is not for the frame buffer and has to go to the display.
 */
bool dgx_fb_area_command(dgx_fb_area_t *area, uint8_t command);

/**
 * @brief Take the data that follows a command of the window: its bounds, two bytes each and the high one first, or pixels.
 * @return false when the last command was not one of the window, so the data is for the display.
 */
bool dgx_fb_area_data(dgx_fb_area_t *area, const uint8_t *data, uint32_t bytes);

/**
 * @brief Take the range of bytes written since the last call and forget it.
 * @return false when nothing was written.
 */
bool dgx_fb_area_take_dirty(dgx_fb_area_t *area, uint32_t *from, uint32_t *length);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
