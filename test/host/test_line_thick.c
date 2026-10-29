#include <math.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

/*
 * dgx_draw_line_thick(). A slanted line is the rectangle around its way, as
 * wide as asked at every angle and cut square at the end points: a pixel
 * whose centre lies well in it is drawn, one well out of it is not. Its rows
 * and its columns are runs without a gap. An odd width gives the same pixels
 * either way, an even one has its spare pixel to the right of the way, and a
 * width of 1 is dgx_draw_line(). A line whose ends lie far off the screen:
 * the square of its length is more than an int holds, and what of it is on
 * the screen is the line of the same width along the same way drawn near.
 */

#define W 200
#define H 200

static uint32_t seed = 31337;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

static uint8_t *pixels(dgx_screen_t *s)
{
    return ((dgx_vscreen_t *)s)->v_array;
}

static void clear(dgx_screen_t *s)
{
    memset(pixels(s), 0, W * H);
}

static int differ(dgx_screen_t *a, dgx_screen_t *b, int top, int bottom)
{
    int n = 0;
    for (int i = top * W; i < (bottom + 1) * W; ++i) n += pixels(a)[i] != pixels(b)[i];
    return n;
}

/* how far a pixel is into the rectangle of the line, the least of the four ways out of it; below 0: how far out */
static double depth(int x1, int y1, int x2, int y2, int width, int x, int y)
{
    const int    right = width / 2, left = width - right - 1;
    const double dx = x2 - x1, dy = y2 - y1, len = hypot(dx, dy);
    /* along the way and to the right of it */
    const double along = ((x - x1) * dx + (y - y1) * dy) / len, across = ((y - y1) * dx - (x - x1) * dy) / len;
    double       d     = along < len - along ? along : len - along;
    if (right + 0.5 - across < d) d = right + 0.5 - across;
    if (left + 0.5 + across < d) d = left + 0.5 + across;
    return d;
}

/* rows and columns that are not one run */
static int gaps(dgx_screen_t *s)
{
    int n = 0;
    for (int y = 0; y < H; ++y) {
        int runs = 0;
        for (int x = 0; x < W; ++x) runs += pixels(s)[y * W + x] && (x == 0 || !pixels(s)[y * W + x - 1]);
        n += runs > 1;
    }
    for (int x = 0; x < W; ++x) {
        int runs = 0;
        for (int y = 0; y < H; ++y) runs += pixels(s)[y * W + x] && (y == 0 || !pixels(s)[(y - 1) * W + x]);
        n += runs > 1;
    }
    return n;
}

static void rectangles(void)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *back = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    int           in = 0, out = 0, odd = 0, even = 0, gapped = 0;
    for (int i = 0; i < 3000; ++i) {
        const int x1 = rnd(20, 180), y1 = rnd(20, 180), x2 = rnd(20, 180), y2 = rnd(20, 180), width = rnd(2, 12);
        /* a line from a point to itself is a dot, not a rectangle */
        if (x1 == x2 && y1 == y2) continue;
        clear(s);
        clear(back);
        dgx_draw_line_thick(s, x1, y1, x2, y2, width, 1);
        dgx_draw_line_thick(back, x2, y2, x1, y1, width, 1);
        /* the middle of the line across it: to the right of the way by half a pixel for an even width */
        const double dx = x2 - x1, dy = y2 - y1, len = hypot(dx, dy);
        double       middle = 0, middle_back = 0;
        int          count = 0, count_back = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const double d = depth(x1, y1, x2, y2, width, x, y), across = ((y - y1) * dx - (x - x1) * dy) / len;
                const int    on = pixels(s)[y * W + x] != 0;
                in += d > 0.02 && !on;
                out += d < -0.02 && on;
                if (on) middle += across, ++count;
                if (pixels(back)[y * W + x]) middle_back += across, ++count_back;
            }
        }
        if (width & 1) odd += differ(s, back, 0, H - 1) != 0;
        else even += !(middle / count > 0.25 && middle_back / count_back < -0.25);
        gapped += gaps(s) != 0;
    }
    if (in || out) fprintf(stderr, "%d pixels in the rectangles of lines left out, %d out of them drawn\n", in, out);
    CHECK(in == 0 && out == 0);
    CHECK(odd == 0 && even == 0 && gapped == 0);
    dgx_screen_destroy(&s);
    dgx_screen_destroy(&back);
}

/* pixels of the middle half of a line per pixel of its length: its width */
static double width_of(dgx_screen_t *s, int x1, int y1, int x2, int y2)
{
    const double dx = x2 - x1, dy = y2 - y1, len = hypot(dx, dy);
    int          n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const double along = ((x - x1) * dx + (y - y1) * dy) / len;
            n += pixels(s)[y * W + x] && along >= len / 4 && along < len * 3 / 4;
        }
    }
    return n / (len / 2);
}

/*
 * As wide as asked at every angle. At 45 degrees the centres of the pixels lie on lines along the way a
 * pixel over the root of 2 apart, and a line takes so many of them: its width is within that of the one
 * asked for, and grows with it.
 */
static void widths(void)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    static const int degrees[] = {0, 25, 30, 45};
    for (unsigned d = 0; d < sizeof(degrees) / sizeof(degrees[0]); ++d) {
        const double angle = degrees[d] * atan(1.0) / 45, c = cos(angle), sn = sin(angle);
        double       before = 0;
        for (int width = 2; width <= 9; ++width) {
            double sum = 0;
            /* lines of the angle a little longer one after another: no one rounding of the ends decides it */
            for (int k = 0; k < 8; ++k) {
                const double r = 70 + k * 0.37;
                const int    x1 = 100 - (int)lround(r * c), y1 = 100 + (int)lround(r * sn), x2 = 100 + (int)lround(r * c), y2 = 100 - (int)lround(r * sn);
                clear(s);
                dgx_draw_line_thick(s, x1, y1, x2, y2, width, 1);
                sum += width_of(s, x1, y1, x2, y2);
            }
            const double got = sum / 8, slack = degrees[d] == 45 ? 0.75 : 0.1;
            if (fabs(got - width) >= slack) fprintf(stderr, "a line of %d at %d degrees is %.2f wide\n", width, degrees[d], got);
            CHECK(fabs(got - width) < slack);
            CHECK(got > before);
            before = got;
        }
    }
    dgx_screen_destroy(&s);
}

static void thin(void)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *line = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    int           wrong = 0;
    for (int i = 0; i < 1000; ++i) {
        const int x1 = rnd(0, W - 1), y1 = rnd(0, H - 1), x2 = rnd(0, W - 1), y2 = rnd(0, H - 1);
        clear(s);
        clear(line);
        dgx_draw_line_thick(s, x1, y1, x2, y2, 1, 1);
        dgx_draw_line(line, x1, y1, x2, y2, 1);
        wrong += differ(s, line, 0, H - 1) != 0;
    }
    CHECK(wrong == 0);
    dgx_screen_destroy(&s);
    dgx_screen_destroy(&line);
}

static void far_ends(void)
{
    dgx_screen_t *far = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *near = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    /* the ways through (0, 0): one aslant, one of a slope of a half; the rows near the ends of the near line are left out */
    static const int ways[][4]  = {{-32900, -32900, 33100, 33100}, {-30000, -15000, 30200, 15100}};
    static const int nears[][4] = {{0, 0, 199, 199}, {0, 0, 198, 99}};
    for (int i = 0; i < 2; ++i) {
        for (int width = 1; width <= 9; ++width) {
            clear(far);
            clear(near);
            dgx_draw_line_thick(far, ways[i][0], ways[i][1], ways[i][2], ways[i][3], width, 1);
            dgx_draw_line_thick(near, nears[i][0], nears[i][1], nears[i][2], nears[i][3], width, 1);
            CHECK(differ(far, near, 20, nears[i][3] - 20) == 0);
        }
    }
    dgx_screen_destroy(&far);
    dgx_screen_destroy(&near);
}

int main(void)
{
    rectangles();
    widths();
    thin();
    far_ends();
    CHECK_DONE();
}
