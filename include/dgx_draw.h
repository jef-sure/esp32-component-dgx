#pragma once
/*
 * dgx_draw.h
 *
 *  Created on: Apr 2, 2023
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */


#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Draws a filled rectangle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the top-left corner of the rectangle.
 * @param y The y coordinate of the top-left corner of the rectangle.
 * @param w The width of the rectangle.
 * @param h The height of the rectangle.
 * @param color The color of the rectangle.
*/
void dgx_fill_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color);

/**
 * @brief Draws a circle outline on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the center of the circle.
 * @param y The y coordinate of the center of the circle.
 * @param r The radius of the circle.
 * @param color The color of the circle.
 */
void dgx_draw_circle(dgx_screen_t *scr, int x, int y, int r, uint32_t color);

/**
 * @brief Draws a line on the screen.
 *
 * @param scr The screen to draw on.
 * @param x1 The x coordinate of the first point of the line.
 * @param y1 The y coordinate of the first point of the line.
 * @param x2 The x coordinate of the second point of the line.
 * @param y2 The y coordinate of the second point of the line.
 * @param color The color of the line.
 */
void dgx_draw_line(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color);

/**
 * @brief Draws a filled circle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the center of the circle.
 * @param y The y coordinate of the center of the circle.
 * @param r The radius of the circle.
 * @param color The color of the circle.
 */
void dgx_solid_circle(dgx_screen_t *scr, int x, int y, int r, uint32_t color);

/**
 * @brief Set a pixel on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the pixel.
 * @param y The y coordinate of the pixel.
 * @param color The color of the pixel.
*/
void dgx_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color);

/**
 * @brief Get a pixel from the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the pixel.
 * @param y The y coordinate of the pixel.
 * @return The color of the pixel.
 * @note If the screen does not have a get_pixel function, as most physical displays,
         this function will return 0.
 * 
*/
uint32_t dgx_get_pixel(dgx_screen_t *scr, int x, int y);

/**
 * @brief Draws a filled triangle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x0,y0 First vertex.
 * @param x1,y1 Second vertex.
 * @param x2,y2 Third vertex.
 * @param color Fill color.
 */
void dgx_draw_triangle_solid(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

/**
 * @brief Draws a filled simple quadrilateral on the screen.
 *
 * Vertices must be supplied in polygon order, either clockwise or
 * counterclockwise. Convex and concave simple quadrilaterals are supported.
 * Self-intersecting input is not supported.
 *
 * @param scr The screen to draw on.
 * @param x0,y0 First vertex.
 * @param x1,y1 Second vertex.
 * @param x2,y2 Third vertex.
 * @param x3,y3 Fourth vertex.
 * @param color Fill color.
 */
void dgx_draw_polygon4_solid(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3,
                             uint32_t color);

/**
 * @brief Draws a thick line on the screen as a parallelogram of the given width,
 * with rounded/triangular caps at the endpoints.
 *
 * @param scr The screen to draw on.
 * @param x1,y1 Line start.
 * @param x2,y2 Line end.
 * @param width Line thickness in pixels (>= 1).
 * @param color Line color.
 */
void dgx_draw_line_thick(dgx_screen_t *scr, int x1, int y1, int x2, int y2, int width, uint32_t color);

/**
 * @brief Draws a dotted/dashed line using a rotating bit pattern. Each bit of @p mask
 * (taken LSB-first as the first pixel) selects either @p color (1) or @p bg (0)
 * for one pixel along the line. After @p mask_bits pixels the pattern repeats.
 *
 * @param scr        The screen to draw on.
 * @param x1,y1      Line start.
 * @param x2,y2      Line end.
 * @param color      Foreground color (mask bit = 1).
 * @param bg         Background color (mask bit = 0).
 * @param mask       Bit pattern (low @p mask_bits bits used).
 * @param mask_bits  Pattern length in bits (1..32).
 * @return           Mask rotated by the number of pixels drawn, suitable to
 *                   continue the same pattern on a follow-up call.
 */
uint32_t dgx_draw_line_mask(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color, uint32_t bg, uint32_t mask,
                            uint8_t mask_bits);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

