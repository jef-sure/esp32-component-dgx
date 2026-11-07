/*
 * dgx_draw_thick_segment() and dgx_draw_thick_segment_part(): a segment is the pen of the curves on one
 * straight piece, with a disc at an end or without; drawn by a growing share it is the segment drawn at
 * once; its ends are where they are given, fractions of a pixel among them.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

#define W 120
#define H 100

static const int both = DGX_PEN_START_DISC | DGX_PEN_END_DISC;

static int bytes_of(dgx_screen_t *s)
{
    return (s->color_bits + 7) / 8;
}

static uint8_t *memory(dgx_screen_t *s)
{
    return ((dgx_vscreen_t *)s)->v_array;
}

static size_t size_of(dgx_screen_t *s)
{
    return (size_t)W * H * bytes_of(s);
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

static long ink(dgx_screen_t *s)
{
    long n = 0;
    for (size_t i = 0; i < size_of(s); ++i) n += memory(s)[i];
    return n;
}

/* the color of a pen on a screen: all the ink of a byte, or a color with no zero byte */
static uint32_t color_of(dgx_screen_t *s)
{
    return s->color_bits == 8 ? 255 : s->color_bits == 16 ? 0xa5c3 : 0x5a3c96;
}

static int rnd(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

/* with discs at both ends a segment between whole points is the curve of one piece between them */
static void test_curve_of_one_piece(dgx_screen_t *a, dgx_screen_t *b, bool soft)
{
    int bad = 0;
    for (int i = 0; i < 3000; ++i) {
        const dgx_point_2d_t p[3] = {{(int16_t)rnd(-20, W + 20), (int16_t)rnd(-20, H + 20)}, {0, 0}, {(int16_t)rnd(-20, W + 20), (int16_t)rnd(-20, H + 20)}};
        const dgx_point_2d_t q[3] = {p[0], p[0], p[2]};
        const int            width = rnd(1, 12) | (soft ? DGX_PEN_SOFT : 0);
        clear(a), clear(b);
        dgx_draw_thick_segment(a, p[0].x, p[0].y, p[2].x, p[2].y, width | both, color_of(a));
        dgx_draw_bezier3(b, q, 1, width, color_of(b));
        bad += differ(a, b) != 0;
    }
    CHECK(bad == 0);
    if (bad) fprintf(stderr, "%d of 3000 segments on %d bits, %s pen, are not their curve\n", bad, a->color_bits, soft ? "a soft" : "a hard");
}

/*
 * Drawn on by a growing share, a segment never puts ink where the whole segment has less, a share not
 * greater than the one before draws nothing, and when the share reaches 1 it is the whole segment and the
 * call says so.
 */
static void test_steps(dgx_screen_t *step, dgx_screen_t *whole, bool soft)
{
    static uint8_t before[W * H * 4];
    int            outside = 0, unlike = 0, back = 0, said = 0;
    for (int i = 0; i < 2000; ++i) {
        const float x1 = rnd(0, 4 * W) / 4.0f - 10, y1 = rnd(0, 4 * H) / 4.0f - 10;
        const float x2 = rnd(0, 4 * W) / 4.0f - 10, y2 = rnd(0, 4 * H) / 4.0f - 10;
        const int   width = rnd(1, 9) | (soft ? DGX_PEN_SOFT : 0) | (i & 1 ? DGX_PEN_START_DISC : 0) | (i & 2 ? DGX_PEN_END_DISC : 0);
        const int   steps = rnd(1, 40);
        clear(whole), clear(step);
        dgx_draw_thick_segment(whole, x1, y1, x2, y2, width, color_of(whole));
        dgx_segment_part_t part = {0};
        for (int k = 1; k <= steps; ++k) {
            /* uneven steps, the last one 1 */
            part.t = k == steps ? 1 : (k + rnd(0, 99) / 100.0f) / steps;
            const bool done = dgx_draw_thick_segment_part(step, x1, y1, x2, y2, width, color_of(step), &part);
            said += done != (k == steps);
            const uint8_t *s = memory(step), *w = memory(whole);
            int            over = 0;
            if (step->color_bits == 8) {
                for (size_t j = 0; j < size_of(step); ++j) over += s[j] > w[j];
            } else {
                for (size_t j = 0; j < size_of(step); ++j) over += s[j] && !w[j];
            }
            outside += over != 0;
            /* the same share again, or a smaller one, draws nothing */
            if (k < steps && (k & 1)) {
                memcpy(before, s, size_of(step));
                part.t -= rnd(0, 10) / 1000.0f;
                dgx_draw_thick_segment_part(step, x1, y1, x2, y2, width, color_of(step), &part);
                back += memcmp(before, s, size_of(step)) != 0;
            }
        }
        unlike += differ(step, whole) != 0;
        /* a finished segment draws nothing more */
        part.t = 1;
        said += !dgx_draw_thick_segment_part(step, x1, y1, x2, y2, width, color_of(step), &part);
    }
    CHECK(outside == 0);
    CHECK(unlike == 0);
    CHECK(back == 0);
    CHECK(said == 0);
    if (outside || unlike || back || said)
        fprintf(stderr, "%s pen on %d bits: %d steps out of the segment, %d segments in steps unlike the whole, %d drawn back, %d wrong returns\n",
                soft ? "soft" : "hard", step->color_bits, outside, unlike, back, said);
}

/* the largest amount by which two 8-bit screens differ */
static int most_apart(dgx_screen_t *a, dgx_screen_t *b)
{
    int most = 0;
    for (size_t i = 0; i < size_of(a); ++i) {
        const int d = memory(a)[i] > memory(b)[i] ? memory(a)[i] - memory(b)[i] : memory(b)[i] - memory(a)[i];
        if (d > most) most = d;
    }
    return most;
}

/*
 * The discs are asked for by a call, whatever the calls before asked for: a disc not drawn yet is drawn
 * when it is asked for, once.
 */
static void test_discs_later(dgx_screen_t *step, dgx_screen_t *whole, bool soft)
{
    static uint8_t before[W * H * 4];
    const int      pen0 = soft ? DGX_PEN_SOFT : 0;
    int            end_last = 0, start_later = 0, still = 0, again = 0, at_zero = 0, after = 0;
    for (int i = 0; i < 600; ++i) {
        const float x1 = rnd(0, 4 * W) / 4.0f, y1 = rnd(0, 4 * H) / 4.0f, x2 = rnd(0, 4 * W) / 4.0f, y2 = rnd(0, 4 * H) / 4.0f;
        const int   pen = pen0 | rnd(2, 9), steps = rnd(2, 20);
        const int   tolerance = soft ? 1 : 0;
        /* the disc at the end asked for by the last call only: the segment with that disc, the tips on the way not drawn */
        clear(step), clear(whole);
        dgx_draw_thick_segment(whole, x1, y1, x2, y2, pen | DGX_PEN_END_DISC, color_of(whole));
        dgx_segment_part_t part = {0};
        for (int k = 1; k <= steps; ++k) part.t = (float)k / steps, dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen | (k == steps ? DGX_PEN_END_DISC : 0), color_of(step), &part);
        end_last += differ(step, whole) != 0;
        /* the disc at the start asked for half way: the segment with it, but for the seam of the bar before it */
        clear(step), clear(whole);
        dgx_draw_thick_segment(whole, x1, y1, x2, y2, pen | DGX_PEN_START_DISC, color_of(whole));
        part = (dgx_segment_part_t){0};
        for (int k = 1; k <= steps; ++k) part.t = (float)k / steps, dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen | (k > steps / 2 ? DGX_PEN_START_DISC : 0), color_of(step), &part);
        start_later += soft ? most_apart(step, whole) > tolerance : differ(step, whole) != 0;
        /* the disc where the pen stands, asked for while the pen stands: drawn once */
        clear(step);
        part = (dgx_segment_part_t){.t = 0.5f};
        dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen, color_of(step), &part);
        memcpy(before, memory(step), size_of(step));
        CHECK(!dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen | DGX_PEN_END_DISC, color_of(step), &part));
        /* the disc of the tip is two pixels smaller than the pen: of a hard pen of 4 and less it lies within the rectangle of the way */
        const bool long_enough = hypotf(x2 - x1, y2 - y1) > 2 && (pen & 0xff) > (soft ? 2 : 4);
        still += long_enough && memcmp(before, memory(step), size_of(step)) == 0;
        memcpy(before, memory(step), size_of(step));
        dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen | DGX_PEN_END_DISC, color_of(step), &part);
        again += memcmp(before, memory(step), size_of(step)) != 0;
        /* the disc at the start asked for before the pen goes: the disc alone, then the segment */
        clear(step), clear(whole);
        part = (dgx_segment_part_t){0};
        CHECK(!dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen | DGX_PEN_START_DISC, color_of(step), &part));
        /* the half of the disc behind the start may lie off the screen */
        const bool inside = x1 >= 10 && x1 <= W - 10 && y1 >= 10 && y1 <= H - 10;
        at_zero += inside && ink(step) == 0;
        part.t = 1;
        CHECK(dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen | DGX_PEN_START_DISC, color_of(step), &part));
        dgx_draw_thick_segment(whole, x1, y1, x2, y2, pen | DGX_PEN_START_DISC, color_of(whole));
        at_zero += differ(step, whole) != 0;
        /* both discs asked for when the segment is finished without them */
        clear(step), clear(whole);
        part = (dgx_segment_part_t){.t = 1};
        CHECK(dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen, color_of(step), &part));
        CHECK(dgx_draw_thick_segment_part(step, x1, y1, x2, y2, pen | both, color_of(step), &part));
        dgx_draw_thick_segment(whole, x1, y1, x2, y2, pen | both, color_of(whole));
        after += soft ? most_apart(step, whole) > tolerance : differ(step, whole) != 0;
    }
    CHECK(end_last == 0);
    CHECK(start_later == 0);
    CHECK(still == 0);
    CHECK(again == 0);
    CHECK(at_zero == 0);
    CHECK(after == 0);
    if (end_last || start_later || still || again || at_zero || after)
        fprintf(stderr, "%s pen: end disc last %d, start disc later %d, no disc while still %d, drawn again %d, at zero %d, after the end %d\n",
                soft ? "soft" : "hard", end_last, start_later, still, again, at_zero, after);
}

/* a disc at an end adds to the segment near that end only, and takes nothing away but two steps of rounding */
static void test_discs(dgx_screen_t *bare, dgx_screen_t *round, bool soft)
{
    int lost = 0, far = 0;
    for (int i = 0; i < 1000; ++i) {
        const float x1 = rnd(0, 4 * W) / 4.0f, y1 = rnd(0, 4 * H) / 4.0f, x2 = rnd(0, 4 * W) / 4.0f, y2 = rnd(0, 4 * H) / 4.0f;
        const int   pen = rnd(2, 11), width = pen | (soft ? DGX_PEN_SOFT : 0);
        clear(bare), clear(round);
        dgx_draw_thick_segment(bare, x1, y1, x2, y2, width, 255);
        dgx_draw_thick_segment(round, x1, y1, x2, y2, width | both, 255);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const uint8_t a = memory(bare)[y * W + x], b = memory(round)[y * W + x];
                /* the disc and the bar count a pixel on the seam between them each its own way, the disc within a hundredth of a pixel: two steps apart */
                lost += b + 2 < a;
                /* what is added lies within the disc of an end, of half the pen and the soft edge, a pixel more for rounding */
                if (b > a) {
                    const float reach = pen * 0.5f + 0.5f + 1.5f;
                    const float h = pen & 1 ? 0 : 0.5f;
                    far += hypotf(x - x1 - h, y - y1 - h) > reach && hypotf(x - x2 - h, y - y2 - h) > reach;
                }
            }
        }
    }
    CHECK(lost == 0);
    CHECK(far == 0);
    if (lost || far) fprintf(stderr, "%s pen: discs took %d pixels away, put %d far from the ends\n", soft ? "soft" : "hard", lost, far);
}

/* two segments one after another along the same way, without discs, are the one segment: no seam between them */
static void test_seam(dgx_screen_t *one, dgx_screen_t *two)
{
    int worst = 0;
    for (int i = 0; i < 2000; ++i) {
        const float x1 = rnd(0, 4 * W) / 4.0f, y1 = rnd(0, 4 * H) / 4.0f, x2 = rnd(0, 4 * W) / 4.0f, y2 = rnd(0, 4 * H) / 4.0f;
        const float share = rnd(10, 90) / 100.0f, xm = x1 + (x2 - x1) * share, ym = y1 + (y2 - y1) * share;
        const int   pen = rnd(1, 9), width = DGX_PEN_SOFT | pen;
        const float h = pen & 1 ? 0 : 0.5f, near = pen * 0.5f + 1.5f;
        clear(one), clear(two);
        dgx_draw_thick_segment(one, x1, y1, x2, y2, width, 255);
        dgx_draw_thick_segment(two, x1, y1, xm, ym, width, 255);
        dgx_draw_thick_segment(two, xm, ym, x2, y2, width, 255);
        /* around the seam: at the outer ends the ways of the two differ in the last bit, and a pixel on a straight cut may go either way */
        for (int j = 0; j < W * H; ++j) {
            if (hypotf(j % W - xm - h, j / W - ym - h) > near) continue;
            const int d = memory(one)[j] > memory(two)[j] ? memory(one)[j] - memory(two)[j] : 0;
            if (d > worst) worst = d;
        }
    }
    /* the two count their pixels from their own starts: a step of rounding at most, no hole */
    CHECK(worst <= 2);
    if (worst > 2) fprintf(stderr, "two segments along one way have %d of 255 less than one\n", worst);
}

/* the ends are where they are given: a soft line half a pixel aside has its ink half a pixel aside */
static void test_fractions(dgx_screen_t *s)
{
    for (int q = 0; q < 8; ++q) {
        const float shift = q / 8.0f;
        double      mass = 0, at = 0;
        clear(s);
        dgx_draw_thick_segment(s, 20, 40 + shift, 100, 40 + shift, DGX_PEN_SOFT | 3, 255);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) mass += memory(s)[y * W + x], at += (double)y * memory(s)[y * W + x];
        }
        CHECK(fabs(at / mass - (40 + shift)) < 0.02);
        /* along the way too: 3 wide, cut straight at x 20 and 100, the pixels on the cuts among it: 81 columns */
        CHECK(fabs(mass / 255 - 81 * 3) < 1);
    }
}

/* a dot, the ends far away or not numbers, a share of nothing */
static void test_edges(dgx_screen_t *s, dgx_screen_t *b)
{
    for (int width = 1; width <= 9; ++width) {
        for (int soft = 0; soft < 2; ++soft) {
            const int            pen = width | (soft ? DGX_PEN_SOFT : 0);
            const dgx_point_2d_t dot[3] = {{40, 30}, {40, 30}, {40, 30}};
            clear(s), clear(b);
            dgx_draw_thick_segment(s, 40, 30, 40, 30, pen | DGX_PEN_END_DISC, 255);
            dgx_draw_bezier3(b, dot, 1, pen, 255);
            CHECK(differ(s, b) == 0);
            clear(s);
            dgx_draw_thick_segment(s, 40, 30, 40, 30, pen, 255);
            /* without a disc a dot is nothing, but by a hard pen of one pixel, which draws the pixels of its way */
            CHECK(ink(s) == (width == 1 && !soft ? 255 : 0));
        }
    }
    clear(s);
    dgx_draw_thick_segment(s, 1e9f, 10, 20, 20, DGX_PEN_SOFT | 3 | both, 255);
    dgx_draw_thick_segment(s, NAN, 10, 20, 20, 3 | both, 255);
    dgx_draw_thick_segment(s, 10, 10, 20, -INFINITY, 3, 255);
    /* a share of nothing draws nothing, but a disc at the start asked for: the pen has not gone, a disc where it stands has nothing to end */
    static const float nothing[3] = {0, -1, NAN};
    for (int i = 0; i < 3; ++i) {
        dgx_segment_part_t part = {.t = nothing[i]};
        CHECK(!dgx_draw_thick_segment_part(s, 10, 10, 20, 20, (i & 1 ? 0 : DGX_PEN_SOFT) | 3 | DGX_PEN_END_DISC, 255, &part));
        CHECK(!dgx_draw_thick_segment_part(s, 10, 10, 20, 20, (i & 1 ? 0 : DGX_PEN_SOFT) | 3, 255, &part));
    }
    CHECK(ink(s) == 0);
    {
        dgx_segment_part_t part = {0};
        CHECK(!dgx_draw_thick_segment_part(s, 10, 10, 20, 20, DGX_PEN_SOFT | 3 | DGX_PEN_START_DISC, 255, &part));
        CHECK(ink(s) > 0);
        clear(s);
    }
    {
        /* a segment that cannot be drawn is done with at once */
        dgx_segment_part_t part = {.t = 0.5f};
        CHECK(dgx_draw_thick_segment_part(s, NAN, 10, 20, 20, 3 | both, 255, &part));
        part = (dgx_segment_part_t){.t = 0.5f};
        CHECK(dgx_draw_thick_segment_part(s, 10, 10, 20, 20, DGX_PEN_SOFT | both, 255, &part));
    }
    dgx_draw_thick_segment(s, 10, 10, 20, 20, 0 | both, 255);
    dgx_draw_thick_segment(s, 10, 10, 20, 20, DGX_PEN_SOFT | both, 255);
    dgx_draw_thick_segment(s, 10, 10, 20, 20, -3, 255);
    CHECK(ink(s) == 0);
    /* a share past 1 is the whole segment, the ends as far as a point of a curve may be and a pen of any width are clipped */
    clear(s), clear(b);
    dgx_segment_part_t past = {.t = 7};
    CHECK(dgx_draw_thick_segment_part(s, 10, 10, 90, 70, DGX_PEN_SOFT | 5 | both, 255, &past));
    dgx_draw_thick_segment(b, 10, 10, 90, 70, DGX_PEN_SOFT | 5 | both, 255);
    CHECK(differ(s, b) == 0);
    dgx_draw_thick_segment(s, -32767, -32767, 32767, 32767, DGX_PEN_SOFT | 7 | both, 255);
    dgx_draw_thick_segment(s, -32767, 50, 32767, 50, 7 | both, 255);
    dgx_draw_thick_segment(s, 60, 50, 61, 52, DGX_PEN_SOFT | 30000 | both, 255);
    dgx_draw_thick_segment(s, 60, 50, 61, 52, 30000 | both, 255);
    CHECK(ink(s) == 255L * W * H);
}

/* the bits of the discs are of segments: a curve and an arc always have round ends and are the same with them */
static void test_curves_take_no_notice(dgx_screen_t *a, dgx_screen_t *b)
{
    const dgx_point_2d_t p[4] = {{10, 80}, {30, 5}, {90, 95}, {110, 20}};
    for (int width = 1; width <= 7; ++width) {
        clear(a), clear(b);
        dgx_draw_bezier4(a, p, 0, DGX_PEN_SOFT | width | both, 255);
        dgx_draw_bezier4(b, p, 0, DGX_PEN_SOFT | width, 255);
        CHECK(differ(a, b) == 0);
        clear(a), clear(b);
        dgx_draw_arc(a, 60, 50, 30, 20, 10, 250, width | DGX_PEN_START_DISC, 255);
        dgx_draw_arc(b, 60, 50, 30, 20, 10, 250, width, 255);
        CHECK(differ(a, b) == 0);
    }
}

/* a soft pen with color 0 takes off what it drew, at once and in steps */
static void test_erase(dgx_screen_t *s)
{
    for (int i = 0; i < 300; ++i) {
        const float x1 = rnd(0, 4 * W) / 4.0f, y1 = rnd(0, 4 * H) / 4.0f, x2 = rnd(0, 4 * W) / 4.0f, y2 = rnd(0, 4 * H) / 4.0f;
        const int   width = DGX_PEN_SOFT | rnd(1, 9) | (i & 1 ? DGX_PEN_START_DISC : 0) | (i & 2 ? DGX_PEN_END_DISC : 0);
        clear(s);
        dgx_draw_thick_segment(s, x1, y1, x2, y2, width, 200);
        if (i & 4) {
            dgx_segment_part_t part = {0};
            for (int k = 1; k <= 10; ++k) part.t = k / 10.0f, dgx_draw_thick_segment_part(s, x1, y1, x2, y2, width, 0, &part);
        } else {
            dgx_draw_thick_segment(s, x1, y1, x2, y2, width, 0);
        }
        CHECK(ink(s) == 0);
    }
}

int main(void)
{
    dgx_screen_t *a8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *b8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_screen_t *a16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB), *b16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    /* a screen of 24 bits the pen asks for its rows, it does not write into its memory */
    dgx_screen_t *a24 = dgx_vscreen_init(W, H, 24, DgxScreenRGB), *b24 = dgx_vscreen_init(W, H, 24, DgxScreenRGB);
    CHECK(a8 && b8 && a16 && b16 && a24 && b24);
    srand(7);
    test_curve_of_one_piece(a8, b8, true);
    test_curve_of_one_piece(a8, b8, false);
    test_curve_of_one_piece(a16, b16, false);
    test_curve_of_one_piece(a24, b24, false);
    /* asked to be soft on a screen of colors, the pen is hard there: still the curve */
    test_curve_of_one_piece(a16, b16, true);
    test_steps(a8, b8, true);
    test_steps(a8, b8, false);
    test_steps(a16, b16, false);
    test_steps(a24, b24, false);
    test_discs_later(a8, b8, true);
    test_discs_later(a8, b8, false);
    test_discs_later(a16, b16, false);
    test_discs(a8, b8, true);
    test_discs(a8, b8, false);
    test_seam(a8, b8);
    test_fractions(a8);
    test_edges(a8, b8);
    test_curves_take_no_notice(a8, b8);
    test_erase(a8);
    dgx_screen_destroy(&a8), dgx_screen_destroy(&b8), dgx_screen_destroy(&a16), dgx_screen_destroy(&b16);
    dgx_screen_destroy(&a24), dgx_screen_destroy(&b24);
    CHECK_DONE();
}
