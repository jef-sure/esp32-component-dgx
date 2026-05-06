#pragma once

#include <stdint.h>

#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef uint32_t (*dgx_gauge_color_fn_t)(int value);

typedef struct {
    dgx_screen_t          *scr;
    int                    center_x;
    int                    center_y;
    int                    inner_radius;
    int                    width;
    int                    min_value;
    int                    max_value;
    int                    value;
    int                    active_steps;
    int                    sweep_degrees;
    float                  start_angle;
    uint32_t               background_color;
    dgx_gauge_color_fn_t   color_fn;
} dgx_gauge_t;

void dgx_gauge_init(dgx_gauge_t *gauge, dgx_screen_t *scr, int center_x, int center_y, int inner_radius, int width,
                    float start_angle, int sweep_degrees, int min_value, int max_value, uint32_t background_color,
                    dgx_gauge_color_fn_t color_fn);
void dgx_gauge_set_value(dgx_gauge_t *gauge, int value);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif