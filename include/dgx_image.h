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
    DGX_IMAGE_1BPP  = 1,
    DGX_IMAGE_4BPP  = 4,
    DGX_IMAGE_8BPP  = 8,
    DGX_IMAGE_16BPP = 16, /**< RGB565, the high byte of a pixel first, as a 16-bit virtual screen keeps it: colors, not values */
} dgx_image_format_t;

/**
 * @brief An indexed picture, such as a sprite or an icon, stored as it is drawn.
 *
 * The data is uncompressed: rows one after another, each row starting on a new
 * byte, the leftmost pixel in the high bits (MSB first, the layout of
 * dgx_bw_bitmap_t with is_stream == false). A row takes
 * (width * format + 7) / 8 bytes. The data may stay in flash.
 *
 * A pixel value of 0 is transparent, unless DGX_IMAGE_OPAQUE gives it a
 * color. A picture without holes simply has no zeros.
 *
 * The origin is a pixel of the picture, like the base line and base column of
 * a glyph: dgx_image_blit() puts it at the point it is given, and mirrors and
 * rotates the picture around it, so a sprite that turns round stays where it
 * stands. The origin may lie outside the picture.
 *
 * A picture of DGX_IMAGE_16BPP holds colors, RGB565, two bytes a pixel, the
 * high byte first; it has no transparent value, and may have a plane of
 * opacity in `alpha`: a byte a pixel, 0 transparent to 255 opaque, rows one
 * after another as the pixels are. The pictures of 16 bits of a .dgxi file
 * have one where their PNG is transparent.
 */
typedef struct {
    uint16_t       width;
    uint16_t       height;
    int16_t        origin_x;
    int16_t        origin_y;
    uint8_t        format; /**< dgx_image_format_t: bits per pixel */
    const uint8_t *data;
    const uint8_t *alpha;  /**< DGX_IMAGE_16BPP: the opacity of each pixel, 0 to 255; NULL for an opaque picture */
} dgx_image_t;

/** Every pixel that is not 0 becomes dgx_image_blit_t::color; the map is not used. A silhouette can be drawn on any screen. */
#define DGX_IMAGE_SILHOUETTE 1u
/** Draw only inside the clip rectangle of dgx_image_blit_t, and inside the clip of the destination, dgx_screen_clip(). Without it the clip of the destination alone. */
#define DGX_IMAGE_CLIP 2u
/**
 * A pixel 0 is not transparent: it puts dgx_image_blit_t::bg. The box of the
 * picture is written whole, every pixel once, so an icon drawn again over
 * itself on a display does not blink: on a display behind a bus by one window
 * and the bus buffer, a row at a time; on another screen by runs of the two
 * colors. At a scale the same: one window over the box of the scaled picture
 * on a display behind a bus, every pixel of it sent once.
 */
#define DGX_IMAGE_OPAQUE 4u
/**
 * Soft edges, mixed by the opacity of every pixel: the value of a picture of 4
 * or 8 bits with DGX_IMAGE_SILHOUETTE is how much of `color` covers the pixel,
 * 0 to 15 or 0 to 255, as a letter of a soft font; a picture with `colors`
 * takes the opacity of each value from `alpha`; a picture of 16 bits that of
 * its plane. A pixel partly covered is mixed with what lies under it on a
 * screen that can be read (a virtual screen, a DSI display), or with `bg` with
 * DGX_IMAGE_OPAQUE, which writes the whole box, every pixel once, so a soft
 * icon on a display behind a bus does not blink either. A screen that cannot
 * be read, without DGX_IMAGE_OPAQUE, takes a pixel whole from half on, and a
 * screen of 1 or 32 bits does the same, having no colors DGX mixes. The
 * indices of an 8-bit virtual screen drawn by values or through a map are not
 * mixed: the flag does nothing to them.
 */
#define DGX_IMAGE_SOFT 8u

/**
 * @brief How a picture is drawn. A zero-initialized structure draws it as it
 * is: no map, no rotation, no clip.
 *
 * What a pixel p != 0 puts on the destination (a pixel 0 puts nothing, or bg
 * with DGX_IMAGE_OPAQUE):
 * - 1 bit: color;
 * - 4 bits: map[p - 1], a map of 15 indices; without a map, p itself;
 * - 8 bits: map[p - 1], a map of 255 indices; without a map, p itself;
 * - with DGX_IMAGE_SILHOUETTE, at any depth: color.
 *
 * The same pictures in other colors are the same data with another map.
 *
 * Values and maps are indices, so they need a linear 8-bit virtual screen as
 * the destination, and color is an index there too. A picture in one color,
 * either a 1-bit picture or a silhouette, can be drawn on any screen; color
 * is then a color of that screen, as dgx_fill_rectangle() takes it: RGB565
 * on a 16-bit display, 1 or 0 on a monochrome one.
 */
typedef struct {
    uint32_t                 flags;       /**< DGX_IMAGE_SILHOUETTE, DGX_IMAGE_CLIP, DGX_IMAGE_OPAQUE, DGX_IMAGE_SOFT */
    const uint8_t           *map;         /**< 4 bits: 15 indices; 8 bits: 255 indices; or NULL */
    uint32_t                 color;       /**< for 1 bit and DGX_IMAGE_SILHOUETTE: an index of an 8-bit virtual screen, a color of any other screen */
    dgx_output_orientation_t orientation; /**< mirror or rotation around the origin */
    int16_t                  clip_x;      /**< with DGX_IMAGE_CLIP: the rectangle that may change */
    int16_t                  clip_y;
    int16_t                  clip_w;
    int16_t                  clip_h;
    uint32_t                 bg;          /**< with DGX_IMAGE_OPAQUE: what a pixel 0 puts, as color: an index of an 8-bit virtual screen, a color of any other screen */
    uint8_t                  scale;       /**< every pixel of the picture a square of scale x scale pixels; 0 is the same as 1 */
    /**
     * The color of each value of a picture of 1, 4 or 8 bits on a screen of colors, as color is given: 2, 16 or 256
     * entries, dgx_image_file_colors() fills them from the palette of a file. With it the picture goes to any screen,
     * its values not put as indices; NULL draws by values, a map or one color.
     */
    const uint32_t          *colors;
    /** With colors: the opacity of each value, 0 to 255; NULL for 0 transparent and every other value opaque. */
    const uint8_t           *alpha;
} dgx_image_blit_t;

/**
 * @brief Draw a picture with its origin at (x, y) of the destination.
 *
 * Unlike dgx_vscreen_to_screen(), the source is not a screen but constant
 * data, of 1, 4 or 8 bits a pixel, and it is clipped, mirrored and rotated
 * around its origin. Nothing is allocated. The changed area is marked as by
 * other drawing functions, and the screen is updated once.
 *
 * A picture drawn by its values or through a map needs a linear 8-bit
 * virtual screen as the destination: one of dgx_vscreen_init() or
 * dgx_vscreen_from_array().
 *
 * A 1-bit picture, and a picture of any depth with DGX_IMAGE_SILHOUETTE,
 * can be drawn on any screen: a display of any depth, a monochrome page
 * screen, a virtual screen of any depth. This is how an icon or a logo kept
 * as a bitmap is put straight on a display in a given color. Nonzero pixels
 * are drawn in runs along the rows of the screen, one filled rectangle per
 * run, like the rows of a glyph; zero pixels leave the screen as it was, or
 * take bg with DGX_IMAGE_OPAQUE.
 *
 * A picture of 16 bits, and a picture of 1, 4 or 8 bits with
 * dgx_image_blit_t::colors, go on any screen in their colors, converted by
 * dgx_screen_rgb() to a screen of another depth than 16. Without
 * DGX_IMAGE_SOFT a pixel of an opacity under 128 is left out and the others
 * are put whole; with it the edges are soft, see DGX_IMAGE_SOFT. Pixels go
 * as runs of a row, each through a window of the screen.
 *
 * With dgx_image_blit_t::scale every pixel of the picture is a square of
 * scale x scale pixels, on any screen, at any depth and turn. The square of
 * the origin has its top left corner at (x, y), and the picture is mirrored
 * and rotated around it as at scale 1, the way the glyphs of a font stand at
 * a scale. The clips cut the squares at their edges.
 *
 * @param dst   Destination screen.
 * @param x,y   Where the origin of the picture goes.
 * @param image The picture.
 * @param how   How it is drawn; NULL is the same as a zero-initialized
 *              structure (a 1-bit picture then draws in the color 0).
 * @return ESP_OK, also when nothing of the picture is visible;
 *         ESP_ERR_INVALID_ARG for a NULL screen or picture, an unknown
 *         format or orientation, or no data;
 *         ESP_ERR_NO_MEM when a picture drawn by its colors needs a row of
 *         memory and a screen without a buffer of its bus gets none;
 *         ESP_ERR_NOT_SUPPORTED for a picture of 4 or 8 bits without
 *         DGX_IMAGE_SILHOUETTE when the destination is not a linear 8-bit
 *         virtual screen; it is not changed then.
 */
esp_err_t dgx_image_blit(dgx_screen_t *dst, int x, int y, const dgx_image_t *image, const dgx_image_blit_t *how);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
