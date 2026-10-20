/*
 * draw.c
 *
 *  Created on: Apr 2, 2023
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include "dgx_draw.h"
#include "esp_log.h"

static const char TAG[] = "DGX DRAW";

/*
 * Draws a rectangle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the top-left corner of the rectangle.
 * @param y The y coordinate of the top-left corner of the rectangle.
 * @param w The width of the rectangle.
 * @param h The height of the rectangle.
 * @param color The color of the rectangle.
*/
void dgx_fill_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color) {
    if (!scr->fill_rectangle) {
        ESP_LOGE(TAG, "fill_rectangle is not implemented");
        return;
    }
    scr->fill_rectangle(scr, x, y, w, h, color);
}

/*
 * Draws a circle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the center of the circle.
 * @param y The y coordinate of the center of the circle.
 * @param r The radius of the circle
 * @param color The color of the circle.
 */
void dgx_draw_circle(dgx_screen_t *scr, int x, int y, int r, uint32_t color) {
    if (!scr->circle) {
        ESP_LOGE(TAG, "circle is not implemented");
        return;
    }
    scr->circle(scr, x, y, r, color);
}

/*
 * Draws a line on the screen.
 *
 * @param scr The screen to draw on.
 * @param x1 The x coordinate of the first point of the line.
 * @param y1 The y coordinate of the first point of the line.
 * @param x2 The x coordinate of the second point of the line.
 * @param y2 The y coordinate of the second point of the line.
 * @param color The color of the line.
 */
void dgx_draw_line(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color) {
    if (!scr->draw_line) {
        ESP_LOGE(TAG, "draw_line is not implemented");
        return;
    }
    scr->draw_line(scr, x1, y1, x2, y2, color);
}

/*
 * Draws a circle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the center of the circle.
 * @param y The y coordinate of the center of the circle.
 * @param r The radius of the circle.
 * @param color The color of the circle.
*/
void dgx_solid_circle(dgx_screen_t *scr, int x, int y, int r, uint32_t color) {
    if (!scr->solid_circle) {
        ESP_LOGE(TAG, "solid_circle is not implemented");
        return;
    }
    scr->solid_circle(scr, x, y, r, color);
}

/*
 * Set a pixel on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the pixel.
 * @param y The y coordinate of the pixel.
 * @param color The color of the pixel.
*/
void dgx_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color) {
    if (!scr->set_pixel) {
        ESP_LOGE(TAG, "set_pixel is not implemented");
        return;
    }
    scr->set_pixel(scr, x, y, color);
}

/*
 * Get a pixel on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the pixel.
 * @param y The y coordinate of the pixel.
 * @return The color of the pixel.
 * @note If the screen does not have a get_pixel function, this function will
 * return 0.
*/
uint32_t dgx_get_pixel(dgx_screen_t *scr, int x, int y) {
    if (!scr->get_pixel) {
        ESP_LOGE(TAG, "get_pixel is not implemented");
        return 0;
    }
    return scr->get_pixel(scr, x, y);
}
