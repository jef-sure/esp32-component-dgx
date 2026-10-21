#pragma once
/*
 * dgx_scale_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Lines of a picture made `scale` times as large: every pixel a square of
 *  scale by scale, or the picture smoothed, by Scale2x at 2, Scale3x at 3
 *  and Scale2x twice at 4, or by their SFX variants. A line is a row of the
 *  larger picture or a column of it: whoever puts the picture somewhere
 *  takes its lines in the order the destination keeps its pixels, however
 *  the picture is turned there, for the four ways of smoothing do the same
 *  turned and mirrored. Nothing here depends on the platform.
 *
 *  The lines are of 8-bit indices, which go through a table of colors, or
 *  of the values of pixels of any depth, which go as they are. Pixels are
 *  equal when their indices or their values are.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/*
 * A picture of values, as the transfers of the same depth see it: rows of
 * whole bytes a pixel in memory, read straight, or any screen, read by its
 * get_pixel(). A value is what get_pixel() gives and set_pixel() takes.
 */
typedef struct {
    const dgx_screen_t *screen;
    const uint8_t      *memory; /* the pixel (0, 0) of rows one after another; NULL: read by get_pixel() */
    uint32_t            pitch;  /* bytes from a row to the next one */
    uint8_t             bytes;  /* of a pixel in memory: 1, 2, 3 or 4 */
} dgx_scale_source_t;

/*
 * The lines of a picture made larger, `count` points of each from `from`
 * on, the same points for every line. Lines asked for one after another,
 * forward or back, reuse what the lines before them have made: the lines
 * of the picture itself and of it twice as large are kept, and so are all
 * the lines of the last pass made from one line of the smaller picture.
 */
typedef struct {
    const dgx_bus_indexed_t *picture; /* lines of indices: the picture */
    dgx_scale_source_t       source;  /* lines of values: the source */
    uint32_t                 width, height;
    uint8_t                  scale;
    uint8_t                  smooth; /* 0 for squares, DGX_BUS_SMOOTH or DGX_BUS_SMOOTH_SFX; 0 at any scale but 2, 3 and 4 */
    uint8_t                  block;  /* the last pass makes so many lines from one line of the smaller picture: 2 or 3; `scale` for squares */
    bool                     along_y;
    int32_t                  from;
    uint32_t                 count;
    /* lines kept of the picture (0) and of it twice as large (1, at 4 only): a line at slot `coordinate & 7` */
    int32_t                  base[2]; /* the point at [0] of a kept line */
    uint32_t                 len[2];
    int32_t                  tag[2][8];
    void                    *line[2][8];
    /* the lines made last from one line of the smaller picture, `block` of them, and which line that was */
    int32_t                  made_from;
    void                    *made[3];
    uint16_t                *spare[2]; /* rows of colors not asked for */
} dgx_scale_lines_t;

/** @brief Bytes of scratch the lines need: for `count` points a line of a picture `scale` times as large, of pixels of `bytes` (1: indices, 4: values). */
size_t dgx_scale_scratch(uint8_t scale, uint8_t smooth, uint32_t count, size_t bytes);

/** @brief Start making lines of indices; `scratch` holds dgx_scale_scratch(..., 1) bytes, aligned for a word, and lasts as long as they are made. */
void dgx_scale_lines_begin(dgx_scale_lines_t *lines, const dgx_bus_indexed_t *picture, bool along_y, int32_t from, uint32_t count, uint8_t *scratch);

/** @brief The indices of the line `at` of the larger picture: `count` of them, the point `from` first. Good until the next call. */
const uint8_t *dgx_scale_line(dgx_scale_lines_t *lines, int32_t at);

/**
 * @brief How many lines from `at` on, going `dir` (1 or -1), are made from the same line of the smaller picture as `at`, it counted in.
 *
 * Those are made at once by dgx_scale_colors().
 */
static inline uint32_t dgx_scale_lines_left(const dgx_scale_lines_t *lines, int32_t at, int dir)
{
    /* the place of `at` in its block; a block of 2, the usual one, without a division */
    const int32_t n = lines->block, j = n == 2 ? (at & 1) : at >= 0 ? at % n : (n - 1) - ((-1 - at) % n);
    return dir > 0 ? (uint32_t)(n - j) : (uint32_t)(j + 1);
}

/**
 * @brief Lines of a smoothed picture as colors of its table, for a display.
 *
 * Lines `at`, `at + dir`, ... `asked` of them, all made from the same line
 * of the smaller picture (see dgx_scale_lines_left()), into rows `pitch`
 * pixels apart, `row` first. A point of a row is `step` pixels after the
 * one before it: 1, or -1 for a row made from its end backwards, `row`
 * pointing at the end then.
 */
void dgx_scale_colors(dgx_scale_lines_t *lines, int32_t at, int dir, uint32_t asked, uint16_t *row, ptrdiff_t pitch, ptrdiff_t step);

/** @brief Start making lines of values; `scratch` holds dgx_scale_scratch(..., 4) bytes, aligned for a word. */
void dgx_scale_values_begin(dgx_scale_lines_t *lines, const dgx_scale_source_t *source, uint8_t scale, uint8_t smooth, bool along_y, int32_t from,
                            uint32_t count, uint8_t *scratch);

/** @brief The values of the line `at` of the larger picture, as dgx_scale_line() gives indices. */
const uint32_t *dgx_scale_values(dgx_scale_lines_t *lines, int32_t at);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
