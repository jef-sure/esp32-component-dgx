#pragma once
/*
 * dgx_hw_font_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  What the writing of a handwritten text, hw_font.c, and its morphs,
 *  hw_morph.c, both take strokes by: every element is drawn as a cubic
 *  Bezier curve.
 */

#include "dgx_hw_font.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/* any element as a cubic curve drawing the same; x and y have room for its 4 points */
static inline void dgx_hw_cubic(int type, float *x, float *y)
{
    if (type == DGX_HW_CURVE3P) {
        x[3] = x[2], y[3] = y[2];
        x[2] = x[3] + (x[1] - x[3]) * 2 / 3, y[2] = y[3] + (y[1] - y[3]) * 2 / 3;
        x[1] = x[0] + (x[1] - x[0]) * 2 / 3, y[1] = y[0] + (y[1] - y[0]) * 2 / 3;
    } else if (type == DGX_HW_LINE) {
        x[3] = x[1], y[3] = y[1];
        x[1] = x[0] + (x[3] - x[0]) / 3, y[1] = y[0] + (y[3] - y[0]) / 3;
        x[2] = x[0] + (x[3] - x[0]) * 2 / 3, y[2] = y[0] + (y[3] - y[0]) * 2 / 3;
    } else if (type == DGX_HW_DOT) {
        x[1] = x[2] = x[3] = x[0], y[1] = y[2] = y[3] = y[0];
    }
}

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
