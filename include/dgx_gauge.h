#pragma once

#include <stdint.h>

#include "dgx_deprecated.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/** The color of a step of a gauge for its value; `user_data` is the one given to dgx_gauge_init_with(). */
typedef uint32_t (*dgx_gauge_color_with_fn_t)(int value, void *user_data);

/** The color function of dgx_gauge_init(), without user_data; deprecated with it. */
typedef uint32_t (*dgx_gauge_color_fn_t)(int value) DGX_DEPRECATED("use dgx_gauge_color_with_fn_t and dgx_gauge_init_with()");

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
    DGX_DEPRECATED_USE_BEGIN
    dgx_gauge_color_fn_t   color_fn; /* of dgx_gauge_init(), NULL from dgx_gauge_init_with() */
    DGX_DEPRECATED_USE_END
    dgx_gauge_color_with_fn_t color_with;
    void                     *user_data;
} dgx_gauge_t;

/**
 * @brief Initialize an arc/ring gauge widget and draw its empty ring.
 *
 * The gauge starts at @p min_value: every step is drawn in @p background_color.
 * What is under the ring is not cleared.
 *
 * @p start_angle is an angle of dgx_draw_arc() and dgx_fill_sector(): degrees
 * counter-clockwise from the ray that goes right, 90 is up. The sweep runs the
 * other way: a positive @p sweep_degrees goes clockwise from there, while the
 * angles of an arc and a sector grow counter-clockwise.
 *
 * @param gauge            Gauge object to initialize.
 * @param scr              Target screen.
 * @param center_x         Arc center X in pixels.
 * @param center_y         Arc center Y in pixels.
 * @param inner_radius     Inner radius of the ring in pixels.
 * @param width            Ring thickness in pixels.
 * @param start_angle      Angle the first step begins at, in degrees (counter-clockwise from +X).
 * @param sweep_degrees    Total angular span of the gauge, in degrees, a step a degree: the steps go
 *                         clockwise from @p start_angle, counter-clockwise when it is negative.
 * @param min_value        Value mapped to the first step.
 * @param max_value        Value mapped to the last step.
 * @param background_color Color of inactive steps / background.
 * @param color            Callback returning the color of an active step for its value; NULL draws it in
 *                         @p background_color.
 * @param user_data        Given to @p color with every value.
 */
void dgx_gauge_init_with(dgx_gauge_t *gauge, dgx_screen_t *scr, int center_x, int center_y, int inner_radius, int width,
                         float start_angle, int sweep_degrees, int min_value, int max_value, uint32_t background_color,
                         dgx_gauge_color_with_fn_t color, void *user_data);

/**
 * @brief dgx_gauge_init_with() with a color function that takes no user_data.
 * @deprecated Use dgx_gauge_init_with().
 */
DGX_DEPRECATED_USE_BEGIN
DGX_DEPRECATED("use dgx_gauge_init_with(), whose color function takes user_data")
void dgx_gauge_init(dgx_gauge_t *gauge, dgx_screen_t *scr, int center_x, int center_y, int inner_radius, int width,
                    float start_angle, int sweep_degrees, int min_value, int max_value, uint32_t background_color,
                    dgx_gauge_color_fn_t color_fn);
DGX_DEPRECATED_USE_END

/**
 * @brief Set the gauge value and redraw only the steps that changed.
 * @param gauge Gauge initialized with dgx_gauge_init_with().
 * @param value New value (clamped to [min_value, max_value]).
 */
void dgx_gauge_set_value(dgx_gauge_t *gauge, int value);

/**
 * @brief Redraw the entire gauge (all steps and background).
 * @param gauge Gauge initialized with dgx_gauge_init_with().
 */
void dgx_gauge_redraw(dgx_gauge_t *gauge);
#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif