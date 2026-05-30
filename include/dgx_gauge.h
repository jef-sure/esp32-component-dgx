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

/**
 * @brief Initialize an arc/ring gauge widget.
 *
 * @param gauge            Gauge object to initialize.
 * @param scr              Target screen.
 * @param center_x         Arc center X in pixels.
 * @param center_y         Arc center Y in pixels.
 * @param inner_radius     Inner radius of the ring in pixels.
 * @param width            Ring thickness in pixels.
 * @param start_angle      Angle of the first step, in radians.
 * @param sweep_degrees    Total angular span of the gauge, in degrees.
 * @param min_value        Value mapped to the first step.
 * @param max_value        Value mapped to the last step.
 * @param background_color Color of inactive steps / background.
 * @param color_fn         Callback returning the color for a given value.
 */
void dgx_gauge_init(dgx_gauge_t *gauge, dgx_screen_t *scr, int center_x, int center_y, int inner_radius, int width,
                    float start_angle, int sweep_degrees, int min_value, int max_value, uint32_t background_color,
                    dgx_gauge_color_fn_t color_fn);

/**
 * @brief Set the gauge value and redraw only the steps that changed.
 * @param gauge Gauge initialized with dgx_gauge_init().
 * @param value New value (clamped to [min_value, max_value]).
 */
void dgx_gauge_set_value(dgx_gauge_t *gauge, int value);

/**
 * @brief Redraw the entire gauge (all steps and background).
 * @param gauge Gauge initialized with dgx_gauge_init().
 */
void dgx_gauge_redraw(dgx_gauge_t *gauge);
#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif