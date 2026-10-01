#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dgx_bitmap.h"
#include "dgx_font.h"
#include "dgx_matrix_morph.h"
#include "dgx_morph.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Convert a 1-bpp bitmap into a matrix of the same size.
 * @return Owned matrix, or NULL for NULL/empty/oversized bitmap or allocation failure.
 */
dgx_bit_matrix_t *dgx_matrix_from_bw_bitmap(dgx_bw_bitmap_t *bmap);

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

/**
 * @brief Letter-by-letter morph of one string into another.
 *
 * Letter i morphs glyph i of `from` into glyph i of `to`; both share the
 * font-wide box of dgx_morph_glyph_matrix(), `width` x `height` cells.
 */
typedef struct {
    size_t        length;   ///< Number of letters.
    size_t        changed;  ///< 1 + index of the last letter whose glyph changes; 0 if none.
    int           width;    ///< Letter box width in cells.
    int           height;   ///< Letter box height in cells.
    dgx_morph_t **letters;  ///< `length` morphs, left to right.
} dgx_morph_text_t;

/** @return Number of UTF-8 code points in a string; 0 for NULL. */
size_t dgx_morph_text_length(const char *utf8);

/**
 * @brief Plan a per-letter morph between two UTF-8 strings.
 * @param from,to   Strings; NULL means empty. The shorter one is padded with spaces.
 * @param length    Letter count; 0 means the longer string's length. Longer strings are cut.
 * @param sources   Sources callback for every letter, see dgx_morph_create().
 * @return Owned text, or NULL on failure. Release with dgx_morph_text_destroy().
 */
dgx_morph_text_t *dgx_morph_text_create(
    dgx_font_t              *font,
    const char              *from,
    const char              *to,
    size_t                   length,
    dgx_morph_sources_func_t sources,
    void                    *user_data
);

void dgx_morph_text_destroy(dgx_morph_text_t **text);

/**
 * @brief Time until the last changed letter finishes when letter i starts at
 * i * stagger_us and each letter takes duration_us; 0 if nothing changes.
 */
int64_t dgx_morph_text_duration_us(const dgx_morph_text_t *text, int64_t duration_us, int64_t stagger_us);

#ifdef __cplusplus
}
#endif
