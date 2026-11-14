/*
 * dgx_draw_polyline(): the segments of dgx_draw_thick_segment() between the points, a disc of the pen at
 * every joint; a pen of 1 not asked to be soft is the lines of dgx_draw_line(). No pixel inside a joint is left out and
 * none lies beyond the pen, whatever the turn; a soft pen leaves no more ink at a joint than on the line.
 * A line whose last point is its first is closed and has the same picture whichever corner it starts at.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

#define W 160
#define H 140

static const int both = DGX_PEN_START_DISC | DGX_PEN_END_DISC;

static uint32_t seed = 20261114;

static int rnd(int lo, int hi)
{
    seed = seed * 1103515245u + 12345u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static uint8_t *memory(dgx_screen_t *s)
{
    return ((dgx_vscreen_t *)s)->v_array;
}

static size_t size_of(dgx_screen_t *s)
{
    return (size_t)W * H * ((s->color_bits + 7) / 8);
}

static void clear(dgx_screen_t *s)
{
    memset(memory(s), 0, size_of(s));
}

static int differ(dgx_screen_t *a, dgx_screen_t *b)
{
    int n = 0;
    for (size_t i = 0; i < size_of(a); ++i) n += memory(a)[i] != memory(b)[i];
    return n;
}

/* the ink of a pen on a screen: an amount below all a byte holds, so that more of it would show; or a color with no zero byte */
static uint32_t color_of(dgx_screen_t *s)
{
    return s->color_bits == 8 ? 200 : 0xa5c3;
}

static int calls;

static void record(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
    ++calls;
}

/*
 * The line by hand: the segments one by one, as the header says they go. A hard pen of an even width has
 * its axis half a pixel to the right of the points and below them all along the line: a segment that
 * goes down would run to the left of its points and one that goes left above them, and those are given
 * their ends a pixel further.
 */
static void chain(dgx_screen_t *s, const dgx_point_2d_t *p, int n, int width, uint32_t color)
{
    const int  pen = width & ~both;
    const bool even_hard = !(pen & 1) && !((pen & DGX_PEN_SOFT) && s->color_bits == 8);
    for (int i = 0; i + 1 < n; ++i) {
        int discs = 0;
        if (i == 0) discs |= width & DGX_PEN_START_DISC;
        if (i + 2 < n) discs |= DGX_PEN_END_DISC;
        else discs |= width & DGX_PEN_END_DISC;
        const int right = even_hard && p[i + 1].y > p[i].y, down = even_hard && p[i + 1].x < p[i].x;
        dgx_draw_thick_segment(s, p[i].x + right, p[i].y + down, p[i + 1].x + right, p[i + 1].y + down, pen | discs, color);
    }
}

static void random_points(dgx_point_2d_t *p, int n, int margin)
{
    for (int i = 0; i < n; ++i) p[i] = (dgx_point_2d_t){(int16_t)rnd(-margin, W + margin), (int16_t)rnd(-margin, H + margin)};
    /* open: a line that happens to end where it starts is a closed one, checked apart */
    while (n > 1 && p[n - 1].x == p[0].x && p[n - 1].y == p[0].y) p[n - 1].x = (int16_t)rnd(-margin, W + margin);
    /* and no point twice in a row */
    for (int i = 1; i < n; ++i)
        while (p[i].x == p[i - 1].x && p[i].y == p[i - 1].y) p[i].y = (int16_t)(p[i].y + 1);
    if (n > 1 && p[n - 1].x == p[0].x && p[n - 1].y == p[0].y) p[n - 1].x = (int16_t)(p[n - 1].x + 1);
}

/* a pen of 1 not asked to be soft is the lines of dgx_draw_line(), with discs asked for or without */
static void test_thin(dgx_screen_t *a, dgx_screen_t *b)
{
    int bad = 0;
    for (int i = 0; i < 2000; ++i) {
        dgx_point_2d_t p[8];
        const int      n = rnd(2, 8);
        random_points(p, n, 20);
        const int width = 1 | (i & 1 ? DGX_PEN_START_DISC : 0) | (i & 2 ? DGX_PEN_END_DISC : 0);
        clear(a), clear(b);
        dgx_draw_polyline(a, p, n, width, color_of(a));
        for (int k = 0; k + 1 < n; ++k) dgx_draw_line(b, p[k].x, p[k].y, p[k + 1].x, p[k + 1].y, color_of(b));
        bad += differ(a, b) != 0;
    }
    CHECK(bad == 0);
    if (bad) fprintf(stderr, "%d of 2000 lines of one pixel on %d bits are not their lines\n", bad, a->color_bits);
}

/*
 * A wider pen, and a soft one of any width: the segments with a disc at every joint, the ends as the
 * line asks. `least` is the narrowest pen that goes by segments: 2 for one not asked to be soft, whose
 * line of one pixel is that of dgx_draw_line(), not the pixels of a segment.
 */
static void test_chain(dgx_screen_t *a, dgx_screen_t *b, bool soft, int least)
{
    int bad = 0;
    for (int i = 0; i < 1500; ++i) {
        dgx_point_2d_t p[7];
        const int      n = rnd(2, 7);
        random_points(p, n, 15);
        const int width = rnd(least, 13) | (soft ? DGX_PEN_SOFT : 0) | (i & 1 ? DGX_PEN_START_DISC : 0) | (i & 2 ? DGX_PEN_END_DISC : 0);
        clear(a), clear(b);
        dgx_draw_polyline(a, p, n, width, color_of(a));
        chain(b, p, n, width, color_of(b));
        bad += differ(a, b) != 0;
    }
    CHECK(bad == 0);
    if (bad) fprintf(stderr, "%d of 1500 lines on %d bits, %s pen, are not their segments\n", bad, a->color_bits, soft ? "a soft" : "a hard");
}

static double distance_to_segment(double px, double py, double ax, double ay, double bx, double by)
{
    const double ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
    double       t = len2 > 0 ? ((px - ax) * ex + (py - ay) * ey) / len2 : 0;
    t              = t < 0 ? 0 : t > 1 ? 1 : t;
    const double dx = px - (ax + t * ex), dy = py - (ay + t * ey);
    return sqrt(dx * dx + dy * dy);
}

/* how far a pixel is from the line through the points, the points moved by `shift` both ways */
static double distance_to_line(const dgx_point_2d_t *p, int n, double shift, int x, int y)
{
    double best = 1e9;
    for (int i = 0; i + 1 < n; ++i) {
        const double d = distance_to_segment(x, y, p[i].x + shift, p[i].y + shift, p[i + 1].x + shift, p[i + 1].y + shift);
        if (d < best) best = d;
    }
    return best;
}

/*
 * What a line must have and must not: every pixel well inside the pen is drawn with all the ink, the
 * joints among them, and no pixel beyond the pen has any. `inside` and `outside` are how far from the
 * edge of the pen a pixel is sure of either; counts the pixels that fail.
 */
static void judge(dgx_screen_t *s, const dgx_point_2d_t *p, int n, int pen, double shift, double inside, double outside, int *holes, int *spill,
                  int *over)
{
    const uint32_t ink = color_of(s);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint32_t v = dgx_get_pixel(s, x, y);
            const double   d = distance_to_line(p, n, shift, x, y);
            if (d <= pen / 2.0 - inside && v != ink) ++*holes;
            if (d > pen / 2.0 + outside && v != 0) ++*spill;
            if (s->color_bits == 8 && v > ink) ++*over;
        }
    }
}

/*
 * Two segments that meet at every angle, acute to nearly straight, and the widths of a pen: the joint
 * has no hole and nothing sticks out of it. A pen of an even width runs half a pixel to the right of
 * the points and below them, the same for both segments, so it is judged as closely as an odd one.
 */
static void test_joints(dgx_screen_t *s, bool soft)
{
    static const int turns[] = {8, 20, 45, 60, 90, 120, 135, 160, 172, 180};
    int              holes = 0, spill = 0, over = 0, lines = 0;
    for (int width = soft ? 1 : 2; width <= 21; ++width) {
        for (size_t t = 0; t < sizeof(turns) / sizeof(*turns); ++t) {
            for (int start = 0; start < 360; start += 15) {
                /* the angle between the two segments at the joint is turns[t]: 180 is a straight line, 8 nearly back */
                const double a0 = start * M_PI / 180, a1 = a0 + M_PI - turns[t] * M_PI / 180;
                const double jx = W / 2.0, jy = H / 2.0, len = 48;
                const dgx_point_2d_t p[3] = {{(int16_t)lround(jx - cos(a0) * len), (int16_t)lround(jy + sin(a0) * len)},
                                             {(int16_t)jx, (int16_t)jy},
                                             {(int16_t)lround(jx + cos(a1) * len), (int16_t)lround(jy - sin(a1) * len)}};
                const bool even = !(width & 1);
                clear(s);
                dgx_draw_polyline(s, p, 3, width | (soft ? DGX_PEN_SOFT : 0) | both, color_of(s));
                /* a pen of an even width lies half a pixel right and down of the points, hard or soft, all along the line */
                const double shift = even ? 0.5 : 0, slack = 0;
                judge(s, p, 3, width, shift, (soft ? 1.0 : 0.5) + slack, (soft ? 1.0 : 0.5) + slack, &holes, &spill, &over);
                ++lines;
            }
        }
    }
    CHECK(holes == 0);
    CHECK(spill == 0);
    CHECK(over == 0);
    if (holes || spill || over)
        fprintf(stderr, "%s pen on %d bits, %d joints: %d pixels inside left out, %d beyond the pen, %d with more ink than the pen has\n",
                soft ? "a soft" : "a hard", s->color_bits, lines, holes, spill, over);
}

/* random lines of several points, on the screen and off it: the same of all their joints */
static void test_lines(dgx_screen_t *s, bool soft)
{
    int holes = 0, spill = 0, over = 0;
    for (int i = 0; i < 600; ++i) {
        dgx_point_2d_t p[6];
        const int      n = rnd(2, 6), width = rnd(soft ? 1 : 2, 15);
        random_points(p, n, 10);
        const bool even = !(width & 1);
        clear(s);
        dgx_draw_polyline(s, p, n, width | (soft ? DGX_PEN_SOFT : 0) | both, color_of(s));
        const double shift = even ? 0.5 : 0, slack = 0;
        judge(s, p, n, width, shift, (soft ? 1.0 : 0.5) + slack, (soft ? 1.0 : 0.5) + slack, &holes, &spill, &over);
    }
    CHECK(holes == 0);
    CHECK(spill == 0);
    CHECK(over == 0);
    if (holes || spill || over)
        fprintf(stderr, "%s pen on %d bits, 600 lines: %d pixels inside left out, %d beyond the pen, %d with more ink than the pen has\n",
                soft ? "a soft" : "a hard", s->color_bits, holes, spill, over);
}

/* a soft line drawn twice is the line drawn once: a pixel keeps the larger amount */
static void test_twice(dgx_screen_t *a, dgx_screen_t *b)
{
    int bad = 0;
    for (int i = 0; i < 300; ++i) {
        dgx_point_2d_t p[6];
        const int      n = rnd(2, 6), width = rnd(1, 12) | DGX_PEN_SOFT | both;
        random_points(p, n, 10);
        clear(a), clear(b);
        dgx_draw_polyline(a, p, n, width, color_of(a));
        dgx_draw_polyline(b, p, n, width, color_of(b));
        dgx_draw_polyline(b, p, n, width, color_of(b));
        bad += differ(a, b) != 0;
    }
    CHECK(bad == 0);
}

/*
 * A closed line: the last point is the first. The joint there is closed like the others, with no disc
 * asked for: nothing is left out around the first point, and the picture is the same whichever corner
 * the line starts at.
 */
static void test_closed(dgx_screen_t *a, dgx_screen_t *b, bool soft)
{
    int unlike = 0, holes = 0, spill = 0, over = 0, open = 0;
    for (int i = 0; i < 600; ++i) {
        dgx_point_2d_t p[7], q[7];
        const int      corners = rnd(3, 6), width = rnd(soft ? 1 : 2, 13), flags = soft ? DGX_PEN_SOFT : 0;
        random_points(p, corners, 0);
        p[corners] = p[0];
        const int turn = rnd(1, corners - 1);
        for (int k = 0; k <= corners; ++k) q[k] = p[(k + turn) % corners];
        const bool even = !(width & 1);
        clear(a), clear(b);
        dgx_draw_polyline(a, p, corners + 1, width | flags, color_of(a));
        dgx_draw_polyline(b, q, corners + 1, width | flags, color_of(b));
        unlike += differ(a, b) != 0;
        const double shift = even ? 0.5 : 0, slack = 0;
        if (width > 1 || soft) judge(a, p, corners + 1, width, shift, (soft ? 1.0 : 0.5) + slack, (soft ? 1.0 : 0.5) + slack, &holes, &spill, &over);
        /* the same corners left open have a notch at the first one, or the closing joint would prove nothing */
        if (width >= 7) {
            int missing = 0;
            clear(b);
            dgx_draw_polyline(b, p, corners, width | flags, color_of(b));
            dgx_draw_thick_segment(b, p[corners - 1].x, p[corners - 1].y, p[0].x, p[0].y, width | flags, color_of(b));
            for (size_t j = 0; j < size_of(a); ++j) missing += memory(b)[j] < memory(a)[j];
            open += missing != 0;
        }
    }
    CHECK(unlike == 0);
    CHECK(holes == 0 && spill == 0 && over == 0);
    CHECK(open > 0);
    if (unlike || holes || spill || over)
        fprintf(stderr, "%s pen on %d bits, 600 closed lines: %d differ by the corner they start at; %d pixels left out, %d beyond the pen, %d over\n",
                soft ? "a soft" : "a hard", a->color_bits, unlike, holes, spill, over);
}

/* points that repeat the one before add nothing; a line of one point is the segment from it to itself */
static void test_repeats(dgx_screen_t *a, dgx_screen_t *b, bool soft)
{
    int bad = 0, dots = 0;
    for (int i = 0; i < 600; ++i) {
        dgx_point_2d_t p[6], q[24];
        const int      n = rnd(1, 6), width = rnd(1, 11) | (soft ? DGX_PEN_SOFT : 0) | (i & 1 ? DGX_PEN_START_DISC : 0) | (i & 2 ? DGX_PEN_END_DISC : 0);
        random_points(p, n, 10);
        int m = 0;
        for (int k = 0; k < n; ++k)
            for (int r = rnd(1, 4); r > 0; --r) q[m++] = p[k];
        clear(a), clear(b);
        dgx_draw_polyline(a, p, n, width, color_of(a));
        dgx_draw_polyline(b, q, m, width, color_of(b));
        bad += differ(a, b) != 0;
        if (n == 1) {
            clear(b);
            dgx_draw_thick_segment(b, p[0].x, p[0].y, p[0].x, p[0].y, width, color_of(b));
            dots += differ(a, b) != 0;
        }
    }
    CHECK(bad == 0);
    CHECK(dots == 0);
}

/* nothing to draw, and nothing that crashes: no points, no pen, points as far as they go */
static void test_nothing(dgx_screen_t *s)
{
    const dgx_point_2d_t one[1] = {{10, 10}};
    const dgx_point_2d_t far[5] = {{-INT16_MAX, -INT16_MAX}, {INT16_MAX, INT16_MAX}, {-INT16_MAX, INT16_MAX}, {INT16_MAX, -INT16_MAX}, {-INT16_MAX, -INT16_MAX}};
    /* -32768 is a point no segment takes: a pen wider than 1 leaves its segments out, and draws the rest */
    const dgx_point_2d_t past[4] = {{INT16_MIN, 40}, {20, 40}, {60, 40}, {60, INT16_MIN}};
    static dgx_screen_t *blank;
    if (!blank) blank = dgx_vscreen_init(W, H, s->color_bits, DgxScreenRGB);
    else if (blank->color_bits != s->color_bits) dgx_screen_destroy(&blank), blank = dgx_vscreen_init(W, H, s->color_bits, DgxScreenRGB);
    clear(blank), clear(s);
    dgx_draw_polyline(s, NULL, 3, 3, color_of(s));
    dgx_draw_polyline(s, one, 0, 3, color_of(s));
    dgx_draw_polyline(s, one, -5, 3, color_of(s));
    dgx_draw_polyline(s, one, 1, 0, color_of(s));
    dgx_draw_polyline(s, one, 1, -3, color_of(s));
    dgx_draw_polyline(s, far, 5, 0, color_of(s));
    dgx_draw_polyline(s, far, 5, DGX_PEN_SOFT, color_of(s));
    /* a pen wider than 1 at one point with no disc asked for is nothing, as the segment is */
    dgx_draw_polyline(s, one, 1, 5, color_of(s));
    CHECK(differ(s, blank) == 0);
    /* a hard pen of 1 at one point is its pixel; a wider one with a disc is its dot */
    dgx_draw_polyline(s, one, 1, 1, color_of(s));
    CHECK(dgx_get_pixel(s, 10, 10) == color_of(s));
    dgx_set_pixel(s, 10, 10, 0);
    CHECK(differ(s, blank) == 0);
    dgx_draw_polyline(s, one, 1, 7 | DGX_PEN_END_DISC, color_of(s));
    CHECK(dgx_get_pixel(s, 10, 10) == color_of(s) && dgx_get_pixel(s, 13, 10) == color_of(s) && dgx_get_pixel(s, 14, 10) == 0);
    /* lines across the whole range of a point, every pen: they end, and what falls on the screen is drawn */
    for (int width = 1; width <= 9; width += 4) {
        clear(s);
        dgx_draw_polyline(s, far, 5, width, color_of(s));
        dgx_draw_polyline(s, far, 5, width | DGX_PEN_SOFT | both, color_of(s));
        CHECK(dgx_get_pixel(s, 50, 50) == color_of(s));
        clear(s);
        dgx_draw_polyline(s, past, 4, width, color_of(s));
        CHECK(dgx_get_pixel(s, 40, 40) == color_of(s));
        CHECK((dgx_get_pixel(s, 10, 40) != 0) == (width == 1) && (dgx_get_pixel(s, 60, 20) != 0) == (width == 1));
    }
}

/* a line is one update of the screen, however many points it has; nothing drawn is none */
static void test_updates(void)
{
    dgx_screen_t *s[2] = {dgx_vscreen_init(W, H, 16, DgxScreenRGB), dgx_vscreen_init(W, H, 8, DgxScreenRGB)};
    for (int k = 0; k < 2; ++k) {
        s[k]->update_screen = record;
        for (int i = 0; i < 60; ++i) {
            dgx_point_2d_t p[8];
            const int      n = rnd(2, 8), width = rnd(1, 9) | (i & 1 ? DGX_PEN_SOFT : 0) | (i & 2 ? both : 0);
            random_points(p, n, 0);
            calls = 0;
            dgx_draw_polyline(s[k], p, n, width, color_of(s[k]));
            CHECK(calls == 1);
        }
        const dgx_point_2d_t off[3] = {{-50, -50}, {-20, -90}, {-70, -30}};
        calls = 0;
        dgx_draw_polyline(s[k], off, 3, 5, 1);
        dgx_draw_polyline(s[k], off, 0, 5, 1);
        CHECK(calls == 0);
        dgx_screen_destroy(&s[k]);
    }
}

int main(void)
{
    dgx_screen_t *a16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB), *b16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    dgx_screen_t *a8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *b8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    CHECK(a16 && b16 && a8 && b8);

    test_thin(a16, b16);
    test_thin(a8, b8);
    test_chain(a16, b16, false, 2);
    test_chain(a8, b8, false, 2);
    test_chain(a8, b8, true, 1);
    /* a pen asked to be soft on a screen of colors is the hard one, and goes by segments even at one pixel */
    test_chain(a16, b16, true, 1);

    test_joints(a16, false);
    test_joints(a8, false);
    test_joints(a8, true);
    test_lines(a16, false);
    test_lines(a8, true);
    test_twice(a8, b8);

    test_closed(a16, b16, false);
    test_closed(a8, b8, false);
    test_closed(a8, b8, true);

    test_repeats(a16, b16, false);
    test_repeats(a8, b8, true);
    test_nothing(a16);
    test_nothing(a8);
    test_updates();

    dgx_screen_destroy(&a16), dgx_screen_destroy(&b16), dgx_screen_destroy(&a8), dgx_screen_destroy(&b8);
    CHECK_DONE();
}
