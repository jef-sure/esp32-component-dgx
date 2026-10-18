#pragma once
/*
 * dgx_indexed_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Lines of a picture of 8-bit indices made `scale` times as large, the
 *  pixels repeated or smoothed by Scale2x, along its rows or along its
 *  columns: what a display gets in the order its panel keeps the pixels,
 *  however the screen lies on it. Nothing here depends on the platform.
 */

#include <stddef.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/*
 * A line is a row of the larger picture, or a column of it when `along_y`.
 * Its points `from` to `from + count - 1` are made, the same ones for every
 * line. Lines made one after another, forward or back, reuse what the lines
 * before them have made of the smaller pictures.
 */
typedef struct {
    const dgx_bus_indexed_t *picture;
    bool                     along_y;
    int32_t                  from;
    uint32_t                 count;
    /* lines kept of the picture (1) and of it twice as large (2): a line at slot `coordinate & 3` */
    int32_t                  base1, base2; /* the point at [0] of a kept line */
    uint32_t                 len1, len2;
    int32_t                  tag1[4], tag2[4];
    uint8_t                 *line1[4], *line2[4];
    uint16_t                *spare; /* a row of colors for a half not asked for */
} dgx_indexed_lines_t;

/** @brief Bytes of memory dgx_indexed_lines_begin() needs for lines of `count` points. */
static inline size_t dgx_indexed_lines_scratch(uint32_t count)
{
    return 6u * (size_t)count + 64u;
}

/** @brief Start making lines; `scratch` holds dgx_indexed_lines_scratch(count) bytes and lasts as long as they are made. */
void dgx_indexed_lines_begin(dgx_indexed_lines_t *lines, const dgx_bus_indexed_t *picture, bool along_y, int32_t from, uint32_t count,
                             uint8_t *scratch);

/** @brief The `count` indices of the line `at` of the larger picture, into `out`. */
void dgx_indexed_line(dgx_indexed_lines_t *lines, int32_t at, uint8_t *out);

/**
 * @brief The line `at` of a smoothed picture as colors of its table, and the other line of its pair with it.
 *
 * Scale2x makes the lines of the larger picture in pairs, `at` and `at ^ 1`,
 * from the same indices; making both at once costs little more than one.
 *
 * @param row  Where the first point of the line `at` goes.
 * @param pair Where the first point of the line `at ^ 1` goes, or NULL when it is not wanted.
 * @param step From a point to the next one: 1, or -1 for rows made from their ends.
 */
void dgx_indexed_colors(dgx_indexed_lines_t *lines, int32_t at, uint16_t *row, uint16_t *pair, ptrdiff_t step);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
