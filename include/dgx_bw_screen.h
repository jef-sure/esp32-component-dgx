#pragma once

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif
#include <stdint.h>
#include <stddef.h>
#include "dgx_bitmap.h"
#include "dgx_screen.h"

typedef struct _dgx_bw_vscreen_t {
    dgx_screen_t base;
    uint8_t *v_array;
    struct {
        uint16_t left;
        uint16_t right;
        uint16_t top;
        uint16_t bottom;
        uint16_t x_offset;
        uint16_t y_offset;
    } area;
} dgx_bw_vscreen_t;

dgx_screen_t *dgx_bw_init(int width, int height);
void dgx_bw_destroy_screen(dgx_screen_t **pscr);
void dgx_bw_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color);
uint32_t dgx_bw_get_pixel(dgx_screen_t *scr, int x, int y);
void dgx_bw_fill_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color);
void dgx_bw_fast_vline(dgx_screen_t *scr, int x, int y, int h, uint32_t color);
void dgx_bw_set_area(dgx_screen_t *scr, uint16_t caset_lo, uint16_t caset_hi, uint16_t raset_lo, uint16_t raset_hi);
void dgx_bw_wait_data(dgx_screen_t *scr);
void dgx_bw_write_data(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits);
/**
 * @brief Write one pixel at the current position of the area set with
 * dgx_bw_set_area() and move on to the next one, the way dgx_bw_write_data()
 * does for a whole run: left to right, top to bottom, wrapping inside the area.
 * @param value Non-zero sets the pixel, zero clears it.
 */
void dgx_bw_write_value(dgx_screen_t *scr, uint32_t value);

/**
 * @brief Tell whether a screen keeps its pixels in the page layout of dgx_bw_init():
 * one byte holds 8 pixels of a column, the top one in bit 0.
 */
bool dgx_bw_screen_is_paged(const dgx_screen_t *scr);

/**
 * @brief Set the pixels of a page screen where a row-major 1-bpp bitmap has set bits.
 *
 * Clear bits of the bitmap leave the screen untouched; the bitmap is clipped
 * to the screen. Whole empty bytes of the bitmap are skipped.
 *
 * @param scr  Screen created by dgx_bw_init() or a driver built on it.
 * @param x,y  Position of the bitmap's top-left corner.
 * @param bmap Bitmap with byte-aligned rows (not the stream layout).
 */
void dgx_bw_blit_or(dgx_screen_t *scr, int x, int y, const dgx_bw_bitmap_t *bmap);


#ifdef __cplusplus
// @formatter:off
}
// @formatter:on

#endif

