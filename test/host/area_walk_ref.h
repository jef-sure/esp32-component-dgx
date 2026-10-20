#pragma once
/*
 * How a pixel at a time walked a window in the order a screen with its
 * orientation takes pixels, the way glyphs were drawn before runs were
 * merged: the first pixel of the window, and the one after a pixel. Kept
 * for the tests as the model of a turned glyph; the library no longer has
 * it.
 */

#include <stdbool.h>

#include "dgx_screen.h"

static dgx_point_2d_t ref_area_first_pixel(int left, int right, int top, int bottom, dgx_orientation_t dir_x, dgx_orientation_t dir_y)
{
    dgx_point_2d_t ret;
    ret.x = (int16_t)(dir_x == DgxScreenLeftRight ? left : right);
    ret.y = (int16_t)(dir_y == DgxScreenTopBottom ? top : bottom);
    return ret;
}

/* one step along a run: forward from `from` to `to`, wrapping round; true when it wrapped */
static bool ref_area_step(int16_t *v, int lo, int hi, bool forward)
{
    if (forward) {
        if (*v >= hi) {
            *v = (int16_t)lo;
            return true;
        }
        ++*v;
        return false;
    }
    if (*v <= lo) {
        *v = (int16_t)hi;
        return true;
    }
    --*v;
    return false;
}

static dgx_point_2d_t ref_area_next_pixel(dgx_point_2d_t p, int left, int right, int top, int bottom, dgx_orientation_t dir_x, dgx_orientation_t dir_y,
                                          bool swap_xy)
{
    const bool right_ward = dir_x == DgxScreenLeftRight, down_ward = dir_y == DgxScreenTopBottom;
    if (!swap_xy) {
        if (ref_area_step(&p.x, left, right, right_ward)) ref_area_step(&p.y, top, bottom, down_ward);
    } else {
        if (ref_area_step(&p.y, top, bottom, down_ward)) ref_area_step(&p.x, left, right, right_ward);
    }
    return p;
}
