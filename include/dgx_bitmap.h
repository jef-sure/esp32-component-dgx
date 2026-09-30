#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dgx_screen.h"
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef struct {
    uint8_t *bitmap;
    int width;
    int height;
    bool is_stream;
} dgx_bw_bitmap_t;

bool dgx_bw_bitmap_get_pixel(dgx_bw_bitmap_t *bmap, int x, int y);
void dgx_bw_bitmap_set_pixel(dgx_bw_bitmap_t *bmap, int x, int y, bool color);

/**
 * @brief Callback for dgx_bw_bitmap_foreach_set().
 * @param user_data Caller context.
 * @param x,y       Coordinates of a set pixel.
 * @return false to stop iteration early.
 */
typedef bool (*dgx_bw_bitmap_pixel_func_t)(void *user_data, int x, int y);

/**
 * @brief Invoke a callback for every set pixel in scan order (rows top to
 * bottom, x ascending).
 *
 * Optimized for the linear MSB-first layout (is_stream == false): whole zero
 * bytes skip 8 pixels at once and non-zero bytes are walked set-bit by
 * set-bit, so cost is proportional to the number of set pixels rather than to
 * the area. Row padding bits beyond width are ignored. The legacy Arduino
 * stream layout (is_stream == true) falls back to per-pixel reads.
 *
 * @param bmap      Bitmap to scan.
 * @param func      Callback; NULL only counts set pixels.
 * @param user_data Opaque context passed to the callback.
 * @return Number of set pixels visited before iteration stopped.
 */
int dgx_bw_bitmap_foreach_set(dgx_bw_bitmap_t *bmap, dgx_bw_bitmap_pixel_func_t func, void *user_data);

/**
 * @brief OR-blit a 1-bpp bitmap onto another at (@p x, @p y) with full clipping.
 *
 * Source bits set to 1 set the corresponding destination bits; source bits set
 * to 0 leave the destination unchanged. Both bitmaps must use the linear
 * MSB-first row-major layout (is_stream == false). Stream-mode sources or
 * destinations are not supported and the call is a no-op.
 *
 * Used as the inner kernel for stamping monochrome glyphs / sprites onto a
 * 1-bpp virtual screen (e.g. ST7920).
 */
void dgx_bw_bitmap_blit_or(dgx_bw_bitmap_t *bmap_dst, int x, int y, const dgx_bw_bitmap_t *bmap_src);

static inline dgx_bw_bitmap_t dgx_bw_bitmap_make_of(uint8_t *bitmap, int width, int height, bool is_stream) {
    dgx_bw_bitmap_t ret = {
        .bitmap = bitmap,       //
        .width = width,         //
        .height = height,       //
        .is_stream = is_stream  //
    };
    return ret;
}

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

