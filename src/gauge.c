#include "dgx_gauge.h"

#include <math.h>
#include <stdbool.h>

#include "dgx_draw.h"

static dgx_point_2d_t dgx_gauge_point(int center_x, int center_y, int radius, float angle_deg)
{
    dgx_point_2d_t point;
    float          rad = angle_deg * (M_PI / 180.0f);

    point.x = center_x + (int)lroundf(cosf(rad) * radius);
    point.y = center_y - (int)lroundf(sinf(rad) * radius);
    return point;
}

static void dgx_gauge_step_quad(const dgx_gauge_t *gauge, int step, uint32_t color)
{
    float prev_angle;
    float angle;

    if (step < 1 || step > gauge->sweep_degrees) {
        return;
    }

    prev_angle                = gauge->start_angle - (float)(step - 1);
    angle                     = gauge->start_angle - (float)step;
    dgx_point_2d_t prev_inner = dgx_gauge_point(gauge->center_x, gauge->center_y, gauge->inner_radius, prev_angle);
    dgx_point_2d_t prev_outer = dgx_gauge_point(gauge->center_x, gauge->center_y, gauge->inner_radius + gauge->width, prev_angle);
    dgx_point_2d_t inner      = dgx_gauge_point(gauge->center_x, gauge->center_y, gauge->inner_radius, angle);
    dgx_point_2d_t outer      = dgx_gauge_point(gauge->center_x, gauge->center_y, gauge->inner_radius + gauge->width, angle);

    dgx_draw_polygon4_solid(gauge->scr, prev_inner.x, prev_inner.y, inner.x, inner.y, outer.x, outer.y, prev_outer.x, prev_outer.y, color);
}

void dgx_gauge_init(dgx_gauge_t *gauge, dgx_screen_t *scr, int center_x, int center_y, int inner_radius, int width, float start_angle,
                    int sweep_degrees, int min_value, int max_value, uint32_t background_color, dgx_gauge_color_fn_t color_fn)
{
    gauge->scr              = scr;
    gauge->center_x         = center_x;
    gauge->center_y         = center_y;
    gauge->inner_radius     = inner_radius;
    gauge->width            = width;
    gauge->min_value        = min_value;
    gauge->max_value        = max_value;
    gauge->value            = min_value;
    gauge->active_steps     = 0;
    gauge->color_fn         = color_fn;
    gauge->sweep_degrees    = sweep_degrees;
    gauge->start_angle      = start_angle;
    gauge->background_color = background_color;
    for (int step = 0; step < gauge->active_steps; ++step) {
        dgx_gauge_step_quad(gauge, step, gauge->background_color);
    }
}

static int dgx_gauge_value_for_step(const dgx_gauge_t *gauge, int step)
{
    int range = gauge->max_value - gauge->min_value;

    if (range <= 0) {
        return gauge->min_value;
    }
    if (step < 0) {
        step = 0;
    }
    if (step > gauge->sweep_degrees) {
        step = gauge->sweep_degrees;
    }

    return gauge->min_value + (int)lroundf((float)step * (float)range / (float)gauge->sweep_degrees);
}

void dgx_gauge_set_value(dgx_gauge_t *gauge, int value)
{
    int next_value;
    if (value < gauge->min_value) {
        next_value = gauge->min_value;
    } else if (value > gauge->max_value) {
        next_value = gauge->max_value;
    } else {
        next_value = value;
    }
    int range = gauge->max_value - gauge->min_value;
    //
    int next_active_steps = ((next_value - gauge->min_value) * gauge->sweep_degrees) / range;

    if (next_active_steps > gauge->active_steps) {
        for (int step = gauge->active_steps + 1; step <= next_active_steps; ++step) {
            int step_value = dgx_gauge_value_for_step(gauge, step);
            // If a color function is provided, use it to determine the color for this step based on the step's value.
            uint32_t color = gauge->color_fn ? gauge->color_fn(step_value) : gauge->background_color;
            dgx_gauge_step_quad(gauge, step, color);
        }
    } else if (next_active_steps < gauge->active_steps) {
        for (int step = gauge->active_steps; step > next_active_steps; --step) {
            dgx_gauge_step_quad(gauge, step, gauge->background_color);
        }
    }
    gauge->value        = next_value;
    gauge->active_steps = next_active_steps;
}

void dgx_gauge_redraw(dgx_gauge_t *gauge)
{
    for (int step = 1; step <= gauge->active_steps; ++step) {
        int step_value = dgx_gauge_value_for_step(gauge, step);
        uint32_t color = gauge->color_fn ? gauge->color_fn(step_value) : gauge->background_color;
        dgx_gauge_step_quad(gauge, step, color);
    }
    for (int step = gauge->active_steps + 1; step <= gauge->sweep_degrees; ++step) {
        dgx_gauge_step_quad(gauge, step, gauge->background_color);
    }
}
