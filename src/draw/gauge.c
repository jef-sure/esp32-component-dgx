#include "dgx_gauge.h"

#include <math.h>
#include <stdbool.h>

#include "dgx_draw.h"

/* a step is a degree of the sweep, whichever way the sweep goes */
static int dgx_gauge_steps(const dgx_gauge_t *gauge)
{
    return gauge->sweep_degrees < 0 ? -gauge->sweep_degrees : gauge->sweep_degrees;
}

/*
 * The sine and the cosine of `deg`, 0 to 360 degrees, found in place: the
 * functions of the C library lie elsewhere in flash, and the ring has a
 * couple of them for every step. Within a ten-millionth.
 */
static void dgx_gauge_sincos(float deg, float *sine, float *cosine)
{
    int q = (int)(deg * (1.0f / 90.0f));
    if (q > 3) q = 3;
    float      r    = deg - 90.0f * (float)q;
    const bool over = r > 45.0f;
    if (over) r = 90.0f - r;
    const float x = r * (float)(M_PI / 180.0), x2 = x * x;
    float s = x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
    float c = 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
    if (over) {
        const float t = s;
        s = c, c = t;
    }
    switch (q) {
    case 0: *sine = s, *cosine = c; break;
    case 1: *sine = c, *cosine = -s; break;
    case 2: *sine = -s, *cosine = -c; break;
    default: *sine = -c, *cosine = s; break;
    }
}

/*
 * The edge `k` of the steps, a unit vector with y up: the edge between the
 * steps k and k + 1. It is a function of `k` alone, the edge 360 of a whole
 * circle the edge 0, so the two steps of an edge get it alike to the last
 * bit, whichever is drawn when, and a pixel on it is given to one of them.
 */
typedef struct {
    int   k;
    float x, y;
} dgx_gauge_edge_t;

static void dgx_gauge_edge(const dgx_gauge_t *gauge, int k, dgx_gauge_edge_t *e)
{
    if (e->k == k) return;
    float start = gauge->start_angle;
    if (!(start >= 0 && start < 360.0f)) {
        start -= 360.0f * (float)(int)(start * (1.0f / 360.0f));
        if (start < 0) start += 360.0f;
        if (!(start >= 0 && start < 360.0f)) start = 0;
    }
    float angle = start + (gauge->sweep_degrees < 0 ? (float)(k % 360) : -(float)(k % 360));
    if (angle < 0) angle += 360.0f;
    if (angle >= 360.0f) angle -= 360.0f;
    e->k = k;
    dgx_gauge_sincos(angle, &e->y, &e->x);
}

static inline int dgx_gauge_floor(float v)
{
    const int i = (int)v;
    return i - (v < (float)i);
}

static inline int dgx_gauge_ceil(float v)
{
    const int i = (int)v;
    return i + (v > (float)i);
}

/*
 * A step: the pixels whose centres lie in its degree and in the band of the
 * ring, from inner_radius - 1/2 to inner_radius + width - 1/2 from the centre
 * of the ring. A centre on the edge the step starts at is the step's, one on
 * the edge it ends at the next step's: no pixel is of two steps, and each is
 * written once, whatever value the gauge came from. `from` and `to` keep the
 * edges a call has found, for the step beside.
 */
static void dgx_gauge_step_draw(const dgx_gauge_t *gauge, int step, uint32_t color, dgx_gauge_edge_t *from, dgx_gauge_edge_t *to)
{
    if (step < 1 || step > dgx_gauge_steps(gauge) || gauge->width < 1 || gauge->inner_radius < 0) {
        return;
    }
    /* the edge found for the step beside: the end of the one before is the start of this one, and the start of the one after its end */
    if (to->k == step - 1) *from = *to;
    else if (from->k == step) *to = *from;
    dgx_gauge_edge(gauge, step - 1, from);
    dgx_gauge_edge(gauge, step, to);
    const float ax = from->x, ay = from->y, bx = to->x, by = to->y;
    /* the side a step lies on from its start edge: clockwise for a positive sweep, y up */
    const float side = gauge->sweep_degrees < 0 ? 1.0f : -1.0f;
    /* the band in squares of half pixels, so that it is counted in whole numbers */
    const int64_t lo = 2 * (int64_t)gauge->inner_radius - 1, hi = 2 * ((int64_t)gauge->inner_radius + gauge->width) - 1;
    const int64_t lo2 = lo > 0 ? lo * lo : 0, hi2 = hi * hi;
    /* a ring of up to 16000 pixels is counted in 32 bits */
    const bool small = hi < 32000;
    /* the box of the step: its four corners, and a pixel more for the arcs between them */
    const float r_in = gauge->inner_radius - 0.5f, r_out = gauge->inner_radius + gauge->width - 0.5f;
    const float xs[4] = {ax * r_in, ax * r_out, bx * r_in, bx * r_out}, ys[4] = {ay * r_in, ay * r_out, by * r_in, by * r_out};
    float       x_lo = xs[0], x_hi = xs[0], y_lo = ys[0], y_hi = ys[0];
    for (int i = 1; i < 4; ++i) {
        if (xs[i] < x_lo) x_lo = xs[i];
        if (xs[i] > x_hi) x_hi = xs[i];
        if (ys[i] < y_lo) y_lo = ys[i];
        if (ys[i] > y_hi) y_hi = ys[i];
    }
    const int left = dgx_gauge_floor(x_lo) - 1, right = dgx_gauge_ceil(x_hi) + 1;
    /* y up here, down on the screen */
    const int top = -(dgx_gauge_ceil(y_hi) + 1), bottom = -(dgx_gauge_floor(y_lo) - 1);
    /*
     * Where a row meets an edge, x = slope * y on the side the edge goes. An
     * edge nearly along the rows meets none of them in the box.
     */
    const bool  a_up = ay > 1e-6f, a_down = ay < -1e-6f, b_up = by > 1e-6f, b_down = by < -1e-6f;
    const float a_slope = a_up || a_down ? ax / ay : 0, b_slope = b_up || b_down ? bx / by : 0;
    for (int dy = top; dy <= bottom; ++dy) {
        const float py = (float)-dy;
        /*
         * The step lies between where the row meets its two edges, a pixel more either way for the rounding. An
         * edge that does not reach the row on its side leaves it open toward the side the other one goes; a row
         * neither reaches, by the centre, is gone through whole. Only these pixels are looked at.
         */
        const bool a_meets = a_up ? py > 0 : a_down && py < 0, b_meets = b_up ? py > 0 : b_down && py < 0;
        int        from_x = left, to_x = right;
        if (a_meets || b_meets) {
            float lo_x, hi_x;
            if (a_meets && b_meets) {
                const float xa = a_slope * py, xb = b_slope * py;
                lo_x = xa < xb ? xa : xb, hi_x = xa < xb ? xb : xa;
            } else {
                const float x = a_meets ? a_slope * py : b_slope * py, other = a_meets ? bx : ax;
                lo_x = other > 0 ? x : (float)left, hi_x = other > 0 ? (float)right : x;
            }
            if (dgx_gauge_floor(lo_x) - 1 > from_x) from_x = dgx_gauge_floor(lo_x) - 1;
            if (dgx_gauge_ceil(hi_x) + 1 < to_x) to_x = dgx_gauge_ceil(hi_x) + 1;
        }
        const int64_t dy2 = (int64_t)dy * dy;
        int           run = 0;
        for (int dx = from_x; dx <= to_x + 1; ++dx) {
            bool in = false;
            if (dx <= to_x) {
                const int64_t r2 = small ? (int64_t)(4 * (dx * dx + (int32_t)dy2)) : 4 * ((int64_t)dx * dx + dy2);
                if (r2 >= lo2 && r2 < hi2) {
                    const float px = (float)dx;
                    /* on the step's side of its start edge or on it, strictly on the near side of its end edge, and not across the centre */
                    const float on_from = (ax * py - ay * px) * side, on_to = (bx * py - by * px) * side;
                    in = on_from >= 0 && on_to < 0 && px * (ax + bx) + py * (ay + by) > 0;
                }
            }
            if (in) {
                ++run;
            } else if (run) {
                dgx_fill_rectangle(gauge->scr, gauge->center_x + dx - run, gauge->center_y + dy, run, 1, color);
                run = 0;
            }
        }
    }
}

/* the color of an active step for its value: by the function of either init, or the background without one */
static uint32_t dgx_gauge_color(const dgx_gauge_t *gauge, int value)
{
    if (gauge->color_with) return gauge->color_with(value, gauge->user_data);
    DGX_DEPRECATED_USE_BEGIN
    if (gauge->color_fn) return gauge->color_fn(value);
    DGX_DEPRECATED_USE_END
    return gauge->background_color;
}

void dgx_gauge_init_with(dgx_gauge_t *gauge, dgx_screen_t *scr, int center_x, int center_y, int inner_radius, int width, float start_angle,
                         int sweep_degrees, int min_value, int max_value, uint32_t background_color, dgx_gauge_color_with_fn_t color,
                         void *user_data)
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
    DGX_DEPRECATED_USE_BEGIN
    gauge->color_fn = NULL;
    DGX_DEPRECATED_USE_END
    gauge->color_with       = color;
    gauge->user_data        = user_data;
    gauge->sweep_degrees    = sweep_degrees;
    gauge->start_angle      = start_angle;
    gauge->background_color = background_color;
    /* the empty ring: no step is active yet, so every one of them is drawn in the background color */
    dgx_gauge_redraw(gauge);
}

/* deprecated until DGX 1.0: dgx_gauge_init_with() */
DGX_DEPRECATED_USE_BEGIN
void dgx_gauge_init(dgx_gauge_t *gauge, dgx_screen_t *scr, int center_x, int center_y, int inner_radius, int width, float start_angle,
                    int sweep_degrees, int min_value, int max_value, uint32_t background_color, dgx_gauge_color_fn_t color_fn)
{
    dgx_gauge_init_with(gauge, scr, center_x, center_y, inner_radius, width, start_angle, sweep_degrees, min_value, max_value, background_color, NULL,
                        NULL);
    /* the ring is drawn empty, so the function is only needed from the first value on */
    gauge->color_fn = color_fn;
}
DGX_DEPRECATED_USE_END

static int dgx_gauge_value_for_step(const dgx_gauge_t *gauge, int step)
{
    int range = gauge->max_value - gauge->min_value;
    int steps = dgx_gauge_steps(gauge);

    if (range <= 0 || steps <= 0) {
        return gauge->min_value;
    }
    if (step < 0) {
        step = 0;
    }
    if (step > steps) {
        step = steps;
    }

    return gauge->min_value + (int)lroundf((float)step * (float)range / (float)steps);
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
    if (range <= 0) return;
    /* in 64 bits: a range of millions, as of bytes, times the degrees of the sweep is more than an int holds */
    int next_active_steps = (int)((int64_t)(next_value - gauge->min_value) * dgx_gauge_steps(gauge) / range);

    /* the steps that change are one picture: shown together where a screen shows by batches */
    dgx_gauge_edge_t from = {.k = -1}, to = {.k = -1};
    dgx_screen_batch_begin(gauge->scr);
    if (next_active_steps > gauge->active_steps) {
        for (int step = gauge->active_steps + 1; step <= next_active_steps; ++step) {
            int step_value = dgx_gauge_value_for_step(gauge, step);
            uint32_t color = dgx_gauge_color(gauge, step_value);
            dgx_gauge_step_draw(gauge, step, color, &from, &to);
        }
    } else if (next_active_steps < gauge->active_steps) {
        for (int step = gauge->active_steps; step > next_active_steps; --step) {
            dgx_gauge_step_draw(gauge, step, gauge->background_color, &from, &to);
        }
    }
    dgx_screen_batch_end(gauge->scr);
    gauge->value        = next_value;
    gauge->active_steps = next_active_steps;
}

void dgx_gauge_redraw(dgx_gauge_t *gauge)
{
    dgx_gauge_edge_t from = {.k = -1}, to = {.k = -1};
    dgx_screen_batch_begin(gauge->scr);
    for (int step = 1; step <= gauge->active_steps; ++step) {
        int step_value = dgx_gauge_value_for_step(gauge, step);
        uint32_t color = dgx_gauge_color(gauge, step_value);
        dgx_gauge_step_draw(gauge, step, color, &from, &to);
    }
    for (int step = gauge->active_steps + 1; step <= dgx_gauge_steps(gauge); ++step) {
        dgx_gauge_step_draw(gauge, step, gauge->background_color, &from, &to);
    }
    dgx_screen_batch_end(gauge->scr);
}
