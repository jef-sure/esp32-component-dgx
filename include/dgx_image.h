#pragma once
/*
 * dgx_image.h
 *
 *  Created on: Oct 15, 2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdint.h>

#include "esp_err.h"

#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Bits per pixel of a picture.
 */
typedef enum {
    DGX_IMAGE_1BPP = 1,
    DGX_IMAGE_4BPP = 4,
    DGX_IMAGE_8BPP = 8,
} dgx_image_format_t;

/**
 * @brief An indexed picture, such as a sprite or an icon, stored as it is drawn.
 *
 * The data is uncompressed: rows one after another, each row starting on a new
 * byte, the leftmost pixel in the high bits (MSB first, the layout of
 * dgx_bw_bitmap_t with is_stream == false). A row takes
 * (width * format + 7) / 8 bytes. The data may stay in flash.
 *
 * A pixel value of 0 is always transparent. A picture without holes simply has
 * no zeros.
 *
 * The origin is a pixel of the picture, like the base line and base column of
 * a glyph: dgx_image_blit() puts it at the point it is given, and mirrors and
 * rotates the picture around it, so a sprite that turns round stays where it
 * stands. The origin may lie outside the picture.
 */
typedef struct {
    uint16_t       width;
    uint16_t       height;
    int16_t        origin_x;
    int16_t        origin_y;
    uint8_t        format; /**< dgx_image_format_t: bits per pixel */
    const uint8_t *data;
} dgx_image_t;

/** Every pixel that is not 0 becomes dgx_image_blit_t::color; the map is not used. */
#define DGX_IMAGE_SILHOUETTE 1u
/** Draw only inside the clip rectangle of dgx_image_blit_t. Without it the whole destination is the clip. */
#define DGX_IMAGE_CLIP 2u

/**
 * @brief How a picture is drawn. A zero-initialized structure draws it as it
 * is: no map, no rotation, no clip.
 *
 * The index of the destination a pixel p != 0 gets:
 * - 1 bit: color;
 * - 4 bits: map[p - 1], a map of 15 indices; without a map, p itself;
 * - 8 bits: map[p - 1], a map of 255 indices; without a map, p itself;
 * - with DGX_IMAGE_SILHOUETTE, at any depth: color.
 *
 * The same pictures in other colors are the same data with another map.
 */
typedef struct {
    uint32_t                 flags;       /**< DGX_IMAGE_SILHOUETTE, DGX_IMAGE_CLIP */
    const uint8_t           *map;         /**< 4 bits: 15 indices; 8 bits: 255 indices; or NULL */
    uint8_t                  color;       /**< for 1 bit and DGX_IMAGE_SILHOUETTE */
    dgx_output_orientation_t orientation; /**< mirror or rotation around the origin */
    int16_t                  clip_x;      /**< with DGX_IMAGE_CLIP: the rectangle that may change */
    int16_t                  clip_y;
    int16_t                  clip_w;
    int16_t                  clip_h;
} dgx_image_blit_t;

/**
 * @brief Draw a picture with its origin at (x, y) of the destination.
 *
 * Unlike dgx_vscreen_to_vscreen(), the source is not a screen but constant
 * data, of 1, 4 or 8 bits a pixel, mapped to indices of the destination, and
 * it is clipped, mirrored and rotated around its origin. Nothing is
 * allocated. The changed area is marked as by other drawing functions.
 *
 * The destination must be a linear 8-bit virtual screen.
 *
 * @param dst   Destination screen.
 * @param x,y   Where the origin of the picture goes.
 * @param image The picture.
 * @param how   How it is drawn; NULL is the same as a zero-initialized
 *              structure (a 1-bit picture then draws in index 0).
 * @return ESP_OK, also when nothing of the picture is visible;
 *         ESP_ERR_INVALID_ARG for a NULL screen or picture, an unknown
 *         format or orientation, or no data;
 *         ESP_ERR_NOT_SUPPORTED when the destination is not a linear 8-bit
 *         virtual screen; it is not changed then.
 */
esp_err_t dgx_image_blit(dgx_screen_t *dst, int x, int y, const dgx_image_t *image, const dgx_image_blit_t *how);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
