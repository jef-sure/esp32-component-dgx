#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dgx_matrix_morph.h"
#include "dgx_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup dgx_morph Morphing framework
 *
 * Morphs one dgx_bit_matrix_t into another. Everything happens in cell
 * coordinates; placement and scale are chosen only when drawing:
 *
 *     m = dgx_morph_create(from, to, dgx_morph_sources_life, NULL);
 *     do {
 *         t = dgx_morph_progress(t0, esp_timer_get_time(), 1000000);
 *         dgx_morph_draw(m, t, 0, 0, cell, true, dgx_morph_glow_dot, glow);
 *         dgx_morph_glow_present(glow, t, screen, ox, oy);
 *     } while (t < 1.0f);
 *     dgx_morph_destroy(&m);
 *
 * Cells set in both matrices are static, cells only in `from` fade out unless
 * they became a source, and every cell only in `to` asks a sources callback
 * where its dots fly in from. If `to` is empty, all dots fly into the grid
 * center and fade there.
 */

#define DGX_MORPH_MAX_SOURCES 8
/** Sources callback result: retry this cell in the next pass. */
#define DGX_MORPH_DEFER       (-1)

/** One flight in cell coordinates. */
typedef struct {
    dgx_point_2d_t start;
    dgx_point_2d_t end;
    uint8_t start_intensity;
    uint8_t end_intensity;
} dgx_morph_segment_t;

typedef struct {
    dgx_morph_segment_t *segments;
    size_t number_of_segments;
    dgx_point_2d_t *static_points;
    size_t number_of_static_points;
    dgx_point_2d_t *fading_points;
    size_t number_of_fading_points;
    int width;  ///< Grid size in cells: union of both matrices.
    int height;
} dgx_morph_t;

/** Planning state passed to a sources callback. */
typedef struct dgx_morph_ctx dgx_morph_ctx_t;

/** @return true if cell (x, y) is set in the `from` matrix. */
bool dgx_morph_was_set(const dgx_morph_ctx_t *ctx, int x, int y);

/** @return true if cell (x, y) already feeds some target. */
bool dgx_morph_is_used(const dgx_morph_ctx_t *ctx, int x, int y);

/** @return Grid width/height in cells. */
int dgx_morph_ctx_width(const dgx_morph_ctx_t *ctx);
int dgx_morph_ctx_height(const dgx_morph_ctx_t *ctx);

/**
 * @brief Choose where the dots of new cell (x, y) fly in from.
 *
 * Called in scan order for every cell set only in `to`; deferred cells are
 * called again with pass + 1 after all cells of the current pass. A cell may
 * be deferred as many times as there are vectors to every cell of the grid,
 * see dgx_morph_scan_vector(), and 8 times at least.
 * Returned cells are marked used; brightness is split evenly between them.
 *
 * @return Number of cells written to out; 0 = appear from the grid center;
 *         DGX_MORPH_DEFER = retry in the next pass.
 */
typedef int (*dgx_morph_sources_func_t)(
    const dgx_morph_ctx_t *ctx,
    int                    x,
    int                    y,
    int                    pass,
    dgx_point_2d_t         out[DGX_MORPH_MAX_SOURCES],
    void                  *user_data
);

/** Game of Life: every live neighbor is a parent, parents may be shared. */
int dgx_morph_sources_life(
    const dgx_morph_ctx_t *ctx, int x, int y, int pass,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data);

/**
 * Dot glyphs: a new cell flies in from the nearest unused cell of the old
 * glyph, and the search goes vector by vector, not cell by cell. A pass is
 * one vector, the same for the whole grid: every new cell still without a
 * source looks at the one cell that lies by that vector from it, see
 * dgx_morph_scan_vector(). A line that has moved by a vector is thus found as
 * a whole, each of its cells flying from its own cell of the old line, and
 * the order in which the cells are asked decides nothing: two cells never
 * look at the same cell on a pass. With no unused cell left, the rest appear
 * from the grid center.
 */
int dgx_morph_sources_cells(
    const dgx_morph_ctx_t *ctx, int x, int y, int pass,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data);

/**
 * @brief The vector dgx_morph_sources_cells() looks by on a pass: from a new
 * cell to the cell that may be its source.
 *
 * The vectors go ring by ring, radius 1, then 2 and so on; a square ring of
 * radius r has 8 * r of them. On a ring: the axes first — up, down, right,
 * left; then a cell away from each axis toward the corners — from the top to
 * the right and to the left, from the bottom to the right and to the left,
 * from the right up and down, from the left up and down; then two cells away
 * in the same order, and so on to the corners.
 *
 * @param pass  From 0.
 * @param dx,dy Out: the vector; either may be NULL.
 * @return false for a negative pass.
 */
bool dgx_morph_scan_vector(int pass, int *dx, int *dy);

/**
 * @brief Find the first unused `from` cell on the square ring of exactly this
 * radius around (x, y): axis points first, then toward the corners.
 *
 * @return 1 when found (written to out[0]), 0 otherwise.
 */
int dgx_morph_ring_at(
    const dgx_morph_ctx_t *ctx, int x, int y, int radius,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES]);

/**
 * @brief Find the first unused `from` cell on rings of radius min_radius and up
 * (axis points first, then toward the corners).
 * @return 1 when found (written to out[0]), 0 otherwise.
 */
int dgx_morph_ring_find(
    const dgx_morph_ctx_t *ctx, int x, int y, int min_radius,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES]);

/**
 * @brief Plan a morph between two matrices.
 * @param from,to   Matrices; NULL means empty. Sizes may differ.
 * @param sources   Sources callback; NULL makes every new cell appear.
 * @return Owned morph, or NULL on allocation failure or if a side exceeds INT16_MAX.
 */
dgx_morph_t *dgx_morph_create(
    const dgx_bit_matrix_t  *from,
    const dgx_bit_matrix_t  *to,
    dgx_morph_sources_func_t sources,
    void                    *user_data
);

void dgx_morph_destroy(dgx_morph_t **morph);

/** Receives one dot in pixels. */
typedef void (*dgx_morph_dot_func_t)(
    void                 *user_data,
    const dgx_point_2d_t *point,
    uint8_t               intensity
);

/** @return (now - start) / duration clamped to [0, 1]; 1 if duration <= 0. */
float dgx_morph_progress(int64_t start_us, int64_t now_us, int64_t duration_us);

typedef enum {
    DGX_MORPH_LINEAR = 0,
    DGX_MORPH_EASE_IN_OUT,    ///< 2t² / 1-2(1-t)²
    DGX_MORPH_EASE_IN_OUT_3,  ///< cubic variant
    DGX_MORPH_SMOOTHSTEP,     ///< t²(3-2t)
} dgx_morph_easing_t;

float dgx_morph_ease(dgx_morph_easing_t easing, float t);

/**
 * @brief Emit the dots of one frame.
 *
 * Cell (cx, cy) maps to pixel (x + cx*cell_width + cell_width/2, ...).
 * With trail each flight emits a head and a lagging tail at half brightness
 * each; trails need an additive renderer. Dots outside the int16_t range are skipped.
 */
void dgx_morph_draw(
    const dgx_morph_t   *morph,
    float                t,
    int                  x,
    int                  y,
    int                  cell_width,
    bool                 trail,
    dgx_morph_dot_func_t dot,
    void                *user_data
);

#ifdef __cplusplus
}
#endif
