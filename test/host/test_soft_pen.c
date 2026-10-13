#include <math.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "drivers/vscreen.h"
#include "fonts/font0ss.h"

#define W 140
#define H 140

/* a virtual screen of a byte a pixel: a byte is an amount of ink, a pen may be soft on it */
static dgx_screen_t *soft_screen(void)
{
    return dgx_vscreen_init(W, H, 8, DgxScreenRGB);
}

static const uint8_t *bytes(dgx_screen_t *s)
{
    return ((dgx_vscreen_t *)s)->v_array;
}

static int differ(dgx_screen_t *a, dgx_screen_t *b)
{
    int n = 0;
    for (int i = 0; i < W * H; ++i) n += bytes(a)[i] != bytes(b)[i];
    return n;
}

static long ink(dgx_screen_t *s)
{
    long n = 0;
    for (int i = 0; i < W * H; ++i) n += bytes(s)[i];
    return n;
}

/* a curve by a soft pen of a width */
static void begin(dgx_bezier_t *b, dgx_screen_t *s, const dgx_point_2d_t *p, int count, int pieces, int width, uint32_t color)
{
    if (count == 3) dgx_bezier3_begin(b, s, p, pieces, DGX_PEN_SOFT | width, color);
    else dgx_bezier4_begin(b, s, p, pieces, DGX_PEN_SOFT | width, color);
}

static void draw(dgx_screen_t *s, const dgx_point_2d_t *p, int count, int pieces, int width, uint32_t color)
{
    dgx_bezier_t b;
    begin(&b, s, p, count, pieces, width, color);
    dgx_bezier_draw_to(&b, 1);
}

static float segment_distance(float x, float y, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return hypotf(x - ax - t * dx, y - ay - t * dy);
}

/*
 * The points of a soft line: the formula of the library, nothing rounded but
 * the ends of the curve. An even pen runs half a pixel to the right and down.
 */
static void pieces_of(const dgx_point_2d_t *p, int count, int pieces, int width, float *x, float *y)
{
    float kx[4], ky[4], half = width & 1 ? 0 : 0.5f;
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
        x[i] = ((kx[0] * t + kx[1]) * t + kx[2]) * t + kx[3] + half;
        y[i] = ((ky[0] * t + ky[1]) * t + ky[2]) * t + ky[3] + half;
    }
    x[pieces] = p[count - 1].x + half, y[pieces] = p[count - 1].y + half;
}

/*
 * A round pen of the same width along the same pieces is the reference. A
 * pixel half a pixel or more inside its edge has all the ink, one half a
 * pixel or more outside it has none, and between the two the ink is the
 * share 0.5 + depth. Where the line turns a little, the joint is a corner
 * and not round: it may give a pixel a sixteenth of its ink more.
 */
static void check_curve(const dgx_point_2d_t *p, int count, int pieces, int width)
{
    dgx_screen_t *s = soft_screen();
    float         lx[31], ly[31], r = width * 0.5f;
    int           thin = 0, thick = 0;
    draw(s, p, count, pieces, width, 0xff);
    pieces_of(p, count, pieces, width, lx, ly);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float d = 1e9f;
            for (int i = 0; i < pieces; ++i) {
                float di = segment_distance(x, y, lx[i], ly[i], lx[i + 1], ly[i + 1]);
                if (di < d) d = di;
            }
            float share = 0.5f + r - d;
            float got = dgx_get_pixel(s, x, y) / 255.0f;
            if (share > 1) share = 1;
            if (share < 0) share = 0;
            if (got < share - 0.02f) ++thin;
            if (got > share + 0.02f + 1.0f / 16) ++thick;
        }
    }
    if (thin || thick) fprintf(stderr, "%d points, %d pieces, width %d: %d pixels with less ink, %d with more\n", count, pieces, width, thin, thick);
    CHECK(thin == 0);
    CHECK(thick == 0);
    dgx_screen_destroy(&s);
}

/*
 * Drawn on by t in any number of calls: ink is only added, never over what
 * the finished curve has, and at 1 the picture is the one the whole curve
 * gives, byte for byte. Taken off the same way with color 0, nothing is left.
 */
static void check_steps(const dgx_point_2d_t *p, int count, int pieces, int width, int steps, uint32_t color)
{
    dgx_screen_t *whole = soft_screen(), *s = soft_screen(), *before = soft_screen();
    dgx_bezier_t  b;
    int           over = 0, less = 0;
    draw(whole, p, count, pieces, width, color);
    CHECK(ink(whole) > 0);
    begin(&b, s, p, count, pieces, width, color);
    CHECK(b.pieces >= 1 && b.done == 0 && ink(s) == 0);
    CHECK(!dgx_bezier_draw_to(&b, 0) && ink(s) == 0);
    for (int i = 1; i <= steps; ++i) {
        bool finished = dgx_bezier_draw_to(&b, (float)i / steps);
        CHECK(finished == (i == steps));
        for (int k = 0; k < W * H; ++k) {
            over += bytes(s)[k] > bytes(whole)[k];
            less += bytes(s)[k] < bytes(before)[k];
        }
        dgx_vscreen_copy(before, s);
    }
    int d = differ(s, whole);
    if (over || less || d) {
        fprintf(stderr, "%d points, %d pieces, width %d, %d steps, color %u: %d over the curve, %d lost, %d differ\n", count, pieces, width, steps,
                (unsigned)color, over, less, d);
    }
    CHECK(over == 0 && less == 0 && d == 0);
    /* drawn once more it is the same */
    draw(s, p, count, pieces, width, color);
    CHECK(differ(s, whole) == 0);
    /* a pen with less ink over it changes nothing */
    if (color > 1) {
        draw(s, p, count, pieces, width, color / 2);
        CHECK(differ(s, whole) == 0);
    }
    /* taken off in the same steps: nothing is left at the end, and at once */
    begin(&b, s, p, count, pieces, width, 0);
    for (int i = 1; i <= steps; ++i) dgx_bezier_draw_to(&b, (float)i / steps);
    CHECK(ink(s) == 0);
    draw(whole, p, count, pieces, width, 0);
    CHECK(ink(whole) == 0);
    dgx_screen_destroy(&s);
    dgx_screen_destroy(&whole);
    dgx_screen_destroy(&before);
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
        {{50, 50}, {53, 47}, {57, 55}, {60, 51}},     /* a small one: pieces shorter than the pen is wide */
    };
    static const dgx_point_2d_t quadratics[][3] = {
        {{20, 20}, {70, 130}, {120, 20}},
        {{20, 60}, {125, 70}, {25, 80}},
        {{30, 30}, {30, 30}, {110, 100}},
        {{60, 60}, {60, 60}, {60, 60}}, /* a dot */
    };
    static const int pieces[] = {4, 7, 13, 30};
    static const int steps[] = {1, 2, 7, 40, 333};
    const unsigned   nc = sizeof(cubics) / sizeof(cubics[0]), nq = sizeof(quadratics) / sizeof(quadratics[0]);

    for (int width = 1; width <= 9; ++width) {
        for (unsigned n = 0; n < sizeof(pieces) / sizeof(pieces[0]); ++n) {
            for (unsigned i = 0; i < nc; ++i) check_curve(cubics[i], 4, pieces[n], width);
            for (unsigned i = 0; i < nq; ++i) check_curve(quadratics[i], 3, pieces[n], width);
        }
    }
    for (int width = 1; width <= 7; ++width) {
        for (unsigned n = 0; n < sizeof(steps) / sizeof(steps[0]); ++n) {
            for (unsigned i = 0; i < nc; ++i) {
                check_steps(cubics[i], 4, 0, width, steps[n], 0xff);
                check_steps(cubics[i], 4, 5, width, steps[n], 0xa0);
            }
            for (unsigned i = 0; i < nq - 1; ++i) {
                check_steps(quadratics[i], 3, 0, width, steps[n], 0xff);
                check_steps(quadratics[i], 3, 3, width, steps[n], 3);
            }
        }
    }

    /* a line through the centres of pixels with an odd pen is the hard one away from its ends: whole rows, nothing beside them */
    dgx_screen_t               *s = soft_screen();
    static const dgx_point_2d_t level[3] = {{20, 70}, {70, 70}, {120, 70}};
    draw(s, level, 3, 0, 3, 0xff);
    for (int x = 22; x <= 118; ++x) {
        CHECK(dgx_get_pixel(s, x, 68) == 0 && dgx_get_pixel(s, x, 72) == 0);
        CHECK(dgx_get_pixel(s, x, 69) == 0xff && dgx_get_pixel(s, x, 70) == 0xff && dgx_get_pixel(s, x, 71) == 0xff);
    }
    /* an even pen runs between pixels: two whole rows, the spare one below, as the hard pen has it going right */
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    draw(s, level, 3, 0, 2, 0xff);
    for (int x = 22; x <= 118; ++x) {
        CHECK(dgx_get_pixel(s, x, 70) == 0xff && dgx_get_pixel(s, x, 71) == 0xff);
        CHECK(dgx_get_pixel(s, x, 69) == 0 && dgx_get_pixel(s, x, 72) == 0);
    }
    /* the color is the ink of a pixel covered whole: nothing gets more */
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    draw(s, cubics[1], 4, 0, 5, 100);
    int most = 0, between = 0;
    for (int i = 0; i < W * H; ++i) {
        if (bytes(s)[i] > most) most = bytes(s)[i];
        between += bytes(s)[i] > 0 && bytes(s)[i] < 100;
    }
    CHECK(most == 100 && between > 0);
    /* a dot is a disc of the pen */
    static const dgx_point_2d_t dot[3] = {{60, 60}, {60, 60}, {60, 60}};
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    draw(s, dot, 3, 0, 2, 0xff);
    /* of an even pen it lies between pixels: four of them, each covered by the disc as much as the others */
    CHECK(dgx_get_pixel(s, 60, 60) == 202 && dgx_get_pixel(s, 61, 60) == 202 && dgx_get_pixel(s, 60, 61) == 202 && dgx_get_pixel(s, 61, 61) == 202);
    CHECK(ink(s) == 4 * 202);
    /* of an odd pen around a pixel: the pixel, half the ink beside it, a trace at the corners */
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    draw(s, dot, 3, 0, 3, 0xff);
    CHECK(dgx_get_pixel(s, 60, 60) == 0xff && dgx_get_pixel(s, 61, 60) == 0xff && dgx_get_pixel(s, 60, 59) == 0xff && dgx_get_pixel(s, 62, 60) == 0);
    CHECK(dgx_get_pixel(s, 61, 61) > 128 && dgx_get_pixel(s, 61, 61) < 0xff && dgx_get_pixel(s, 62, 61) == 0);
    /* a soft pen of one pixel along a row of pixels is that row, and a slanting one is two pixels of a part of the ink each */
    static const dgx_point_2d_t slant[3] = {{20, 20}, {60, 50}, {100, 80}};
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    draw(s, level, 3, 0, 1, 0xff);
    for (int x = 22; x <= 118; ++x) CHECK(dgx_get_pixel(s, x, 70) == 0xff && dgx_get_pixel(s, x, 69) == 0 && dgx_get_pixel(s, x, 71) == 0);
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    draw(s, slant, 3, 0, 1, 0xff);
    between = 0;
    for (int i = 0; i < W * H; ++i) between += bytes(s)[i] > 0 && bytes(s)[i] < 0xff;
    CHECK(between > 0);

    /*
     * The pen is what it is asked to be, not what the screen is: without DGX_PEN_SOFT the edge is hard on
     * this screen too, a pixel has all the ink or none, and the picture is the one a 16-bit screen gets.
     */
    dgx_screen_t *deep = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    for (int width = 1; width <= 6; ++width) {
        int unlike = 0;
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_fill_rectangle(deep, 0, 0, W, H, 0);
        dgx_draw_bezier4(s, cubics[1], 0, width, 0xff);
        dgx_draw_bezier4(deep, cubics[1], 0, width, 0xffff);
        between = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                uint32_t here = dgx_get_pixel(s, x, y);
                between += here > 0 && here < 0xff;
                unlike += (here != 0) != (dgx_get_pixel(deep, x, y) != 0);
            }
        }
        CHECK(between == 0 && unlike == 0 && ink(s) > 0);
    }
    /*
     * On a screen of 8 or 16 bits a pixel a hard pen writes its rows into the memory of the screen, on
     * one of 24 bits it asks the screen for them: the pixels are the same, at once and in steps, on the
     * screen and past its edges, and a pixel of two bytes has its color whole.
     */
    {
        static const dgx_point_2d_t across[4] = {{-30, 20}, {60, -40}, {90, 200}, {170, 110}};
        dgx_screen_t               *asked = dgx_vscreen_init(W, H, 24, DgxScreenRGB);
        for (int width = 1; width <= 41; width += width < 9 ? 1 : 16) {
            for (int steps = 1; steps <= 7; steps += 3) {
                const dgx_point_2d_t *p = steps == 4 ? across : cubics[1];
                dgx_screen_t         *to[3] = {s, deep, asked};
                const uint32_t        colors[3] = {0xa5, 0xa5c3, 0xa5c3e1};
                for (int i = 0; i < 3; ++i) {
                    dgx_bezier_t b;
                    dgx_fill_rectangle(to[i], 0, 0, W, H, 0);
                    dgx_bezier4_begin(&b, to[i], p, 0, width, colors[i]);
                    for (int k = 1; k <= steps; ++k) dgx_bezier_draw_to(&b, (float)k / steps);
                }
                int unlike = 0, lit = 0;
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        const uint32_t byte = dgx_get_pixel(s, x, y), two = dgx_get_pixel(deep, x, y), three = dgx_get_pixel(asked, x, y);
                        lit += three != 0;
                        unlike += byte != (three ? 0xa5u : 0u) || two != (three ? 0xa5c3u : 0u) || (three != 0 && three != 0xa5c3e1);
                    }
                }
                CHECK(unlike == 0 && lit > 0);
            }
        }
        dgx_screen_destroy(&asked);
    }
    /* a hard pen with color 0 takes off a hard line whole, and of a soft line it leaves the edge */
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_draw_bezier4(s, cubics[1], 0, 4, 0xff);
    dgx_draw_bezier4(s, cubics[1], 0, 4, 0);
    CHECK(ink(s) == 0);
    draw(s, cubics[1], 4, 0, 4, 0xff);
    dgx_draw_bezier4(s, cubics[1], 0, 4, 0);
    CHECK(ink(s) > 0);
    draw(s, cubics[1], 4, 0, 4, 0);
    CHECK(ink(s) == 0);
    /* a width below 1 is no pen, whatever bits it has */
    dgx_draw_bezier4(s, cubics[1], 0, -3, 0xff);
    dgx_draw_bezier4(s, cubics[1], 0, DGX_PEN_SOFT, 0xff);
    CHECK(ink(s) == 0);
    dgx_screen_destroy(&s);

    /* a screen of more than a byte a pixel has no ink to take the larger of: a pen asked to be soft draws there as a hard one */
    dgx_screen_t *plain = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    dgx_fill_rectangle(deep, 0, 0, W, H, 0);
    dgx_draw_bezier4(deep, cubics[1], 0, DGX_PEN_SOFT | 4, 0xffff);
    dgx_draw_bezier4(plain, cubics[1], 0, 4, 0xffff);
    int other = 0, lit = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            other += dgx_get_pixel(deep, x, y) != dgx_get_pixel(plain, x, y);
            lit += dgx_get_pixel(deep, x, y) == 0xffff;
        }
    }
    CHECK(other == 0 && lit > 0);
    dgx_screen_destroy(&deep);
    dgx_screen_destroy(&plain);

    /* a pen wider than anything, a curve off the screen */
    s = soft_screen();
    static const int wide[] = {46341, 100000, INT32_MAX};
    for (unsigned i = 0; i < sizeof(wide) / sizeof(wide[0]); ++i) {
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        draw(s, cubics[2], 4, 0, wide[i], 0xff);
        CHECK(ink(s) == 255L * W * H);
    }
    static const dgx_point_2d_t away[4] = {{-300, -300}, {-50, -60}, {180, 200}, {32000, 31000}};
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    draw(s, away, 4, 0, 6, 0xff);
    CHECK(ink(s) > 0);
    dgx_bezier_t part;
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_bezier4_begin(&part, s, away, 0, DGX_PEN_SOFT | 6, 0xff);
    for (int i = 1; i <= 50; ++i) dgx_bezier_draw_to(&part, i / 50.0f);
    CHECK(ink(s) > 0);
    dgx_screen_destroy(&s);

    /* a text written in any number of steps is the text drawn at once; written with color 0 over itself it is gone */
    dgx_font_t   *font = font0ss();
    dgx_screen_t *whole = soft_screen(), *stepped = soft_screen();
    for (int pen = 1; pen <= 3; ++pen) {
        dgx_hw_writing_t w;
        float            effort = dgx_hw_text_effort(font, "Weing", true, 0);
        dgx_fill_rectangle(whole, 0, 0, W, H, 0);
        dgx_fill_rectangle(stepped, 0, 0, W, H, 0);
        dgx_hw_draw_text_xy(whole, 6, 80, font, "Weing", 0.19f, 0.21f, DGX_PEN_SOFT | pen, 0xff, true);
        dgx_hw_writing_begin_xy(&w, stepped, 6, 80, font, "Weing", 0.19f, 0.21f, DGX_PEN_SOFT | pen, 0xff, true, 0);
        for (int i = 1; i <= 97; ++i) dgx_hw_writing_draw_to(&w, effort * i / 97);
        dgx_hw_writing_draw_to(&w, INFINITY);
        CHECK(ink(whole) > 0 && differ(whole, stepped) == 0);
        dgx_hw_writing_begin_xy(&w, stepped, 6, 80, font, "Weing", 0.19f, 0.21f, DGX_PEN_SOFT | pen, 0, true, 0);
        for (int i = 1; i <= 31; ++i) dgx_hw_writing_draw_to(&w, effort * i / 31);
        dgx_hw_writing_draw_to(&w, INFINITY);
        CHECK(ink(stepped) == 0);
    }
    dgx_screen_destroy(&whole);
    dgx_screen_destroy(&stepped);
    CHECK_DONE();
}
