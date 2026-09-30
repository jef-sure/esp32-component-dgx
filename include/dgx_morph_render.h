#pragma once

#include <stdint.h>

#include "dgx_morph.h"
#include "dgx_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Additive glow renderer with phosphor persistence.
 *
 * Each dot is a disc of radius cell*3/4 with smoothstep falloff, summed into
 * an 8-bit brightness map; present() blends it with the previous frame, maps
 * brightness to colors through a 256-entry LUT into an owned vscreen and
 * blits that to the screen.
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
