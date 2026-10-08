#include <math.h>
#include <stdint.h>

#include "check.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

#define W 140
#define H 140

static int calls;

static void record(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
    ++calls;
}

static float segment_distance(float x, float y, dgx_point_2d_t a, dgx_point_2d_t b)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return hypotf(x - a.x - t * dx, y - a.y - t * dy);
}

/* the pieces the library draws: same formula, same rounding */
static void pieces_of(const dgx_point_2d_t *p, int count, int pieces, dgx_point_2d_t *out)
{
    float kx[4], ky[4];
    if (count == 3) {
        kx[0] = 0, kx[1] = p[0].x - 2 * p[1].x + p[2].x, kx[2] = 2 * (p[1].x - p[0].x), kx[3] = p[0].x;
        ky[0] = 0, ky[1] = p[0].y - 2 * p[1].y + p[2].y, ky[2] = 2 * (p[1].y - p[0].y), ky[3] = p[0].y;
    } else {
        kx[3] = p[0].x, kx[2] = 3 * (p[1].x - p[0].x), kx[1] = 3 * (p[2].x - p[1].x) - kx[2];
        kx[0] = p[3].x - p[0].x - kx[2] - kx[1];
        ky[3] = p[0].y, ky[2] = 3 * (p[1].y - p[0].y), ky[1] = 3 * (p[2].y - p[1].y) - ky[2];
        ky[0] = p[3].y - p[0].y - ky[2] - ky[1];
    }
    for (int i = 0; i < pieces; ++i) {
        float t = (float)i / pieces;
        out[i].x = (int16_t)lroundf(((kx[0] * t + kx[1]) * t + kx[2]) * t + kx[3]);
        out[i].y = (int16_t)lroundf(((ky[0] * t + ky[1]) * t + ky[2]) * t + ky[3]);
    }
    out[pieces] = p[count - 1];
}

/*
 * A round pen of the same width along the same pieces is the reference: no
 * pixel may be missing deeper than one pixel from its edge, none may be set
 * further than one and a half pixels outside it.
 */
static void check_curve(const dgx_point_2d_t *p, int count, int pieces, int width)
{
    dgx_screen_t  *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_point_2d_t line[31];
    int            holes = 0, spikes = 0;

    s->update_screen = record;
    calls = 0;
    if (count == 3) dgx_draw_bezier3(s, p, pieces, width, 0xff);
    else dgx_draw_bezier4(s, p, pieces, width, 0xff);
    CHECK(calls == 1);

    pieces_of(p, count, pieces, line);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float d = 1e9f;
            for (int i = 0; i < pieces; ++i) {
                float di = segment_distance(x, y, line[i], line[i + 1]);
                if (di < d) d = di;
            }
            bool set = dgx_get_pixel(s, x, y) != 0;
            if (!set && d <= (width - 1) / 2 - 1.0f) ++holes;
            if (set && d > width / 2 + 1.5f) ++spikes;
        }
    }
    if (holes || spikes) {
        fprintf(stderr, "%d points, %d pieces, width %d: %d holes, %d spikes\n", count, pieces, width, holes, spikes);
    }
    CHECK(holes == 0);
    CHECK(spikes == 0);
    dgx_screen_destroy(&s);
}

static int ink(dgx_screen_t *s)
{
    int n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) n += dgx_get_pixel(s, x, y) != 0;
    }
    return n;
}

/*
 * Drawn on by t in any number of calls: ink is only added, never outside the
 * finished curve, a call flushes once at most, and at 1 the picture is the
 * one the whole curve gives.
 */
static void check_steps(const dgx_point_2d_t *p, int count, int pieces, int width, int steps)
{
    dgx_screen_t *whole = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_screen_t *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_bezier_t  b;

    if (count == 3) {
        dgx_draw_bezier3(whole, p, pieces, width, 0xff);
        dgx_bezier3_begin(&b, s, p, pieces, width, 0xff);
    } else {
        dgx_draw_bezier4(whole, p, pieces, width, 0xff);
        dgx_bezier4_begin(&b, s, p, pieces, width, 0xff);
    }
    CHECK(b.pieces >= 1 && b.done == 0);
    CHECK(ink(s) == 0);
    CHECK(!dgx_bezier_draw_to(&b, 0) && ink(s) == 0);
    s->update_screen = record;
    calls = 0;
    int outside = 0, before = 0;
    for (int i = 1; i <= steps; ++i) {
        bool finished = dgx_bezier_draw_to(&b, (float)i / steps);
        CHECK(finished == (i == steps));
        CHECK(calls <= i);
        int now = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                bool set = dgx_get_pixel(s, x, y) != 0;
                now += set;
                outside += set && !dgx_get_pixel(whole, x, y);
            }
        }
        CHECK(now >= before);
        before = now;
        /* a smaller t draws nothing */
        int flushed = calls;
        dgx_bezier_draw_to(&b, (float)(i - 1) / steps);
        CHECK(calls == flushed);
    }
    if (outside) fprintf(stderr, "%d points, %d pieces, width %d, %d steps: %d pixels outside the curve\n", count, pieces, width, steps, outside);
    CHECK(outside == 0);
    CHECK(calls > 0 && b.done == b.pieces);
    CHECK(dgx_bezier_draw_to(&b, 1));
    int differ = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) differ += dgx_get_pixel(s, x, y) != dgx_get_pixel(whole, x, y);
    }
    if (differ) fprintf(stderr, "%d points, %d pieces, width %d, %d steps: %d pixels differ\n", count, pieces, width, steps, differ);
    CHECK(differ == 0);
    dgx_screen_destroy(&s);
    dgx_screen_destroy(&whole);
}

/* the pen moves evenly whatever the pieces are: a straight line of one piece is drawn by its share */
static void check_pace(int width)
{
    static const dgx_point_2d_t line[3] = {{20, 70}, {70, 70}, {120, 70}};
    static const dgx_point_2d_t slant[3] = {{20, 20}, {60, 50}, {100, 80}};
    dgx_screen_t               *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_bezier_t                b;
    dgx_bezier3_begin(&b, s, line, 0, width, 0xff);
    CHECK(b.pieces == 1 && fabsf(dgx_bezier_length(&b) - 100) < 0.01f);
    for (int i = 1; i <= 10; ++i) {
        dgx_bezier_draw_to(&b, i / 10.0f);
        int reach = 20 + 10 * i;
        CHECK(dgx_get_pixel(s, reach - 3, 70) != 0);
        if (i < 10) CHECK(dgx_get_pixel(s, reach + width / 2 + 2, 70) == 0);
    }
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_bezier3_begin(&b, s, slant, 0, width, 0xff);
    CHECK(b.pieces == 1 && fabsf(dgx_bezier_length(&b) - 100) < 0.01f);
    dgx_bezier_draw_to(&b, 0.5f);
    CHECK(dgx_get_pixel(s, 56, 47) != 0 && dgx_get_pixel(s, 60 + width, 50 + width) == 0);
    dgx_bezier_draw_to(&b, 1);
    CHECK(dgx_get_pixel(s, 100, 80) != 0);
    dgx_screen_destroy(&s);
}

static void curve_point(const dgx_point_2d_t *p, int count, float t, float *x, float *y)
{
    float u = 1 - t;
    if (count == 3) {
        *x = u * u * p[0].x + 2 * u * t * p[1].x + t * t * p[2].x;
        *y = u * u * p[0].y + 2 * u * t * p[1].y + t * t * p[2].y;
    } else {
        *x = u * u * u * p[0].x + 3 * u * u * t * p[1].x + 3 * u * t * t * p[2].x + t * t * t * p[3].x;
        *y = u * u * u * p[0].y + 3 * u * u * t * p[1].y + 3 * u * t * t * p[2].y + t * t * t * p[3].y;
    }
}

/* the number of pieces found from the points keeps the polyline within half a pixel of the curve */
static void check_estimate(const dgx_point_2d_t *p, int count)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_bezier_t  b;
    if (count == 3) dgx_bezier3_begin(&b, s, p, 0, 3, 0xff);
    else dgx_bezier4_begin(&b, s, p, 0, 3, 0xff);
    CHECK(b.pieces >= 1);
    float worst = 0;
    for (int i = 0; i < b.pieces; ++i) {
        float ax, ay, bx, by;
        curve_point(p, count, (float)i / b.pieces, &ax, &ay);
        curve_point(p, count, (float)(i + 1) / b.pieces, &bx, &by);
        for (int k = 1; k < 16; ++k) {
            float cx, cy;
            curve_point(p, count, (i + k / 16.0f) / b.pieces, &cx, &cy);
            float len = hypotf(bx - ax, by - ay);
            float d = len > 0 ? fabsf((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) / len : hypotf(cx - ax, cy - ay);
            if (d > worst) worst = d;
        }
    }
    if (worst > 0.5f) fprintf(stderr, "%d points, %d pieces found: %.2f pixels off\n", count, b.pieces, worst);
    CHECK(worst <= 0.5f);
    dgx_screen_destroy(&s);
}

int main(void)
{
    static const dgx_point_2d_t cubics[][4] = {
        {{42, 64}, {21, 122}, {83, 75}, {40, 79}},    /* a tight turn */
        {{20, 100}, {130, 20}, {10, 20}, {120, 100}}, /* a loop */
        {{20, 20}, {120, 20}, {120, 120}, {20, 120}}, /* a wide arc */
        {{30, 30}, {110, 110}, {110, 30}, {30, 110}}, /* crossing */
        {{20, 70}, {60, 70}, {80, 70}, {120, 70}},    /* straight */
        {{70, 20}, {70, 120}, {70, 120}, {70, 20}},   /* there and back */
    };
    static const dgx_point_2d_t quadratics[][3] = {
        {{20, 20}, {70, 130}, {120, 20}},
        {{20, 60}, {125, 70}, {25, 80}},
        {{30, 30}, {30, 30}, {110, 100}},
    };
    static const int pieces[] = {4, 7, 13, 30};

    for (int width = 1; width <= 9; ++width) {
        for (unsigned n = 0; n < sizeof(pieces) / sizeof(pieces[0]); ++n) {
            for (unsigned i = 0; i < sizeof(cubics) / sizeof(cubics[0]); ++i) {
                check_curve(cubics[i], 4, pieces[n], width);
            }
            for (unsigned i = 0; i < sizeof(quadratics) / sizeof(quadratics[0]); ++i) {
                check_curve(quadratics[i], 3, pieces[n], width);
            }
        }
    }

    static const int steps[] = {1, 2, 7, 40, 333};
    for (int width = 1; width <= 7; ++width) {
        for (unsigned n = 0; n < sizeof(steps) / sizeof(steps[0]); ++n) {
            for (unsigned i = 0; i < sizeof(cubics) / sizeof(cubics[0]); ++i) {
                check_steps(cubics[i], 4, 0, width, steps[n]);
                check_steps(cubics[i], 4, 5, width, steps[n]);
            }
            for (unsigned i = 0; i < sizeof(quadratics) / sizeof(quadratics[0]); ++i) {
                check_steps(quadratics[i], 3, 0, width, steps[n]);
                check_steps(quadratics[i], 3, 3, width, steps[n]);
            }
        }
        check_pace(width);
    }

    for (unsigned i = 0; i < sizeof(cubics) / sizeof(cubics[0]); ++i) check_estimate(cubics[i], 4);
    for (unsigned i = 0; i < sizeof(quadratics) / sizeof(quadratics[0]); ++i) check_estimate(quadratics[i], 3);
    /* a straight line with evenly set points takes one piece, a given number is taken as it is */
    static const dgx_point_2d_t even3[3] = {{10, 10}, {50, 40}, {90, 70}};
    static const dgx_point_2d_t even4[4] = {{10, 10}, {40, 30}, {70, 50}, {100, 70}};
    dgx_bezier_t                e;
    dgx_screen_t               *es = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_bezier3_begin(&e, es, even3, 0, 3, 0xff);
    CHECK(e.pieces == 1);
    dgx_bezier4_begin(&e, es, even4, 0, 3, 0xff);
    CHECK(e.pieces == 1);
    dgx_bezier4_begin(&e, es, cubics[1], 17, 3, 0xff);
    CHECK(e.pieces == 17);
    dgx_screen_destroy(&es);

    /* without a number of pieces it is found from the points; a point is still drawn */
    dgx_screen_t *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_draw_bezier4(s, cubics[2], 0, 5, 0xff);
    CHECK(dgx_get_pixel(s, 20, 20) && dgx_get_pixel(s, 20, 120) && dgx_get_pixel(s, 95, 70));
    static const dgx_point_2d_t dot[4] = {{9, 9}, {9, 9}, {9, 9}, {9, 9}};
    dgx_draw_bezier4(s, dot, 0, 5, 0xff);
    CHECK(dgx_get_pixel(s, 9, 9) && dgx_get_pixel(s, 11, 9) && !dgx_get_pixel(s, 13, 9));
    dgx_draw_bezier3(s, dot, 0, 0, 0xff);
    dgx_bezier_t none;
    dgx_bezier4_begin(&none, s, cubics[0], 0, 0, 0xff);
    CHECK(none.pieces == 0 && dgx_bezier_draw_to(&none, 0.5f));
    dgx_screen_destroy(&s);
    CHECK_DONE();
}
