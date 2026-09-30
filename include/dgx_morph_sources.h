#pragma once

#include <stdint.h>

#include "dgx_font.h"
#include "dgx_matrix_morph.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Rasterize a glyph into a matrix of the font-wide box.
 *
 * All glyphs of a font get the same box (union of all glyph boxes) with a
 * shared baseline, so matrices of different glyphs line up. A missing
 * or blank glyph yields an empty matrix. Works for dot and bitmap fonts.
 *
 * @return Owned matrix, or NULL on failure. Release with dgx_matrix_destroy().
 */
dgx_bit_matrix_t *dgx_morph_glyph_matrix(dgx_font_t *font, uint32_t code_point);

#ifdef __cplusplus
}
#endif
