#pragma once
/*
 * dgx_font_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  What the morphs take of a bitmap glyph of any model: its pixels of ink.
 */

#include "dgx_bitmap.h"
#include "dgx_font.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/* the data of a glyph, as dgx_font_glyph_data() gives it, or NULL when its font has none */
static inline const uint8_t *dgx_glyph_bits(const dgx_font_t *font, const dgx_glyph_t *g)
{
    return font->data ? font->data + g->offset : NULL;
}

/*
 * Every pixel of ink of a bitmap glyph, as dgx_bw_bitmap_foreach_set() gives the set pixels of a bitmap: in scan order,
 * func NULL to count them, func returning false to stop. Ink is a set pixel of a font of one bit, and a pixel covered
 * by 110/255 or more of a font of 2 or 4 bits a pixel, as DGX_FONT_MONO draws it. Returns the pixels visited.
 */
int dgx_font_glyph_foreach_ink(const dgx_font_t *font, const dgx_glyph_t *g, dgx_bw_bitmap_pixel_func_t func, void *user_data);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
