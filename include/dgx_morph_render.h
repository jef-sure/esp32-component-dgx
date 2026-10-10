#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dgx_morph.h"
#include "dgx_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Additive glow renderer with phosphor persistence.
 *
 * Each dot is a disc of radius max(cell*3/4, 2) with smoothstep falloff,
 * summed into an 8-bit brightness map. present() blends it with the previous
 * frame and sends it to the screen, mapping brightness through a 256-entry
 * LUT on the way. No frame of colors is kept: the renderer takes two bytes a
 * pixel, the two brightness maps, at any color depth.
 */
typedef struct dgx_morph_glow dgx_morph_glow_t;

/**
 * @param width,height Pixel size, usually morph grid * cell_width.
 * @param color_bits   Screen color depth: 16, 18 or 24. The LUT starts gray.
 * @return Owned renderer, or NULL on failure.
 */
dgx_morph_glow_t *dgx_morph_glow_create(int width, int height, int cell_width, uint8_t color_bits);

void dgx_morph_glow_destroy(dgx_morph_glow_t **glow);

/** @param lut 256 colors in the screen format, e.g. dgx_rgb_to_16(); copied. */
void dgx_morph_glow_set_lut(dgx_morph_glow_t *glow, const uint32_t *lut);

/** @brief Forget the phosphor; the next present() shows its frame unblended. */
void dgx_morph_glow_reset(dgx_morph_glow_t *glow);

/** dgx_morph_dot_func_t; pass the renderer as user_data. */
void dgx_morph_glow_dot(void *glow, const dgx_point_2d_t *point, uint8_t intensity);

/**
 * @brief An extra filter of every frame right before it goes to the screen.
 *
 * @p brightness is the finished frame: what the dots have added up to,
 * already blended with the phosphor, @p width x @p height bytes, row after
 * row, 0 is black. The filter may change it in any way, blur it, say; what it
 * leaves is mapped to colors and shown. It is a copy: nothing the filter does
 * gets into the glow or into the frames that follow.
 */
typedef void (*dgx_morph_glow_filter_t)(void *user_data, uint8_t *brightness, int width, int height);

/**
 * @brief Sets the filter of the frames of a glow renderer; NULL removes it.
 *
 * A filter takes one more byte a pixel for the copy it works on. The
 * exception is dgx_morph_glow_blur() in one pass, which is made row by row on
 * the way to the screen and takes four rows of the frame; what is shown is
 * the same. @p user_data is read anew for every frame, so the number of its
 * passes may change on the go, and the copy is made when it is first needed.
 *
 * @return false when there is no memory for what the filter needs; the
 *         frames are then shown unfiltered.
 */
bool dgx_morph_glow_set_filter(dgx_morph_glow_t *glow, dgx_morph_glow_filter_t filter, void *user_data);

/**
 * @brief A ready filter: blurs the frame.
 *
 * Glyphs of a font turned into dots keep the steps of its one-bit picture;
 * a blur smooths them out. A pass takes every pixel with half of its
 * neighbours on each side, across and down; @p user_data points to an int
 * with the number of passes, NULL is one pass.
 *
 * @code
 * static const int passes = 2;
 * dgx_morph_glow_set_filter(glow, dgx_morph_glow_blur, (void *)&passes);
 * @endcode
 */
void dgx_morph_glow_blur(void *user_data, uint8_t *brightness, int width, int height);

/**
 * @brief Blend the accumulated dots into the phosphor and blit to (x, y).
 * @param t Morph progress; the blend weight is smoothstep(t).
 */
void dgx_morph_glow_present(dgx_morph_glow_t *glow, float t, dgx_screen_t *screen, int x, int y);

/**
 * @brief Dot sprite renderer: a soft 8-bit dot blitted through a 256-entry
 * LUT, scaled by dot intensity. Black pixels are transparent.
 */
typedef struct dgx_morph_sprite dgx_morph_sprite_t;

/** Gray until dgx_morph_sprite_set_lut() is called. */
dgx_morph_sprite_t *dgx_morph_sprite_create(int diameter);

void dgx_morph_sprite_destroy(dgx_morph_sprite_t **sprite);

/** @param lut 256 colors in the target screen format; copied. */
void dgx_morph_sprite_set_lut(dgx_morph_sprite_t *sprite, const uint32_t *lut);

/** @param vscreen 16-, 18- or 24-bit virtual screen to draw into. */
void dgx_morph_sprite_set_target(dgx_morph_sprite_t *sprite, dgx_screen_t *vscreen);

/** dgx_morph_dot_func_t; draws the sprite centered on the point. */
void dgx_morph_sprite_dot(void *sprite, const dgx_point_2d_t *point, uint8_t intensity);

#ifdef __cplusplus
}
#endif
