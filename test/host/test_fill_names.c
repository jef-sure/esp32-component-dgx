/*
 * The filled figures under one scheme of names, dgx_fill_*, and the names of before, which are the same
 * functions; dgx_draw_thick_line() and dgx_draw_line_thick() likewise. dgx_fill_ellipse() is the whole
 * turn of dgx_fill_sector() and, with equal radii, the circle of dgx_fill_circle(); it is the outline of
 * dgx_draw_ellipse() and all within it. dgx_draw_triangle() is three lines of dgx_draw_line().
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

#define W 96
#define H 80

static uint32_t seed = 4711;

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

static long ink(dgx_screen_t *s)
{
    long n = 0;
    for (size_t i = 0; i < size_of(s); ++i) n += memory(s)[i] != 0;
    return n;
}

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

/* the types the calls have, by either name */
typedef void (*circle_func)(dgx_screen_t *scr, int x, int y, int r, uint32_t color);
typedef void (*sector_func)(dgx_screen_t *scr, int x, int y, int rx, int ry, float from, float to, uint32_t color);
typedef void (*triangle_func)(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);
typedef void (*polygon4_func)(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, uint32_t color);
typedef void (*line_func)(dgx_screen_t *scr, int x1, int y1, int x2, int y2, int width, uint32_t color);

/*
 * A name of before is its function of now: taken as a pointer it is the same address, and called, it
 * draws the same picture. A program written with the old names compiles as it did.
 */
static void test_names(dgx_screen_t *a, dgx_screen_t *b)
{
    const circle_func   circle_old = dgx_solid_circle, circle_new = dgx_fill_circle;
    const sector_func   sector_old = dgx_solid_sector, sector_new = dgx_fill_sector;
    const triangle_func triangle_old = dgx_draw_triangle_solid, triangle_new = dgx_fill_triangle;
    const polygon4_func polygon4_old = dgx_draw_polygon4_solid, polygon4_new = dgx_fill_polygon4;
    const line_func     line_old = dgx_draw_line_thick, line_new = dgx_draw_thick_line;
    CHECK(circle_old == circle_new && sector_old == sector_new && triangle_old == triangle_new && polygon4_old == polygon4_new &&
          line_old == line_new);

    int bad = 0;
    for (int i = 0; i < 300; ++i) {
        const int x = rnd(-10, W + 10), y = rnd(-10, H + 10), r = rnd(0, 30), rx = rnd(0, 30), ry = rnd(0, 30);
        const int x1 = rnd(-10, W + 10), y1 = rnd(-10, H + 10), x2 = rnd(-10, W + 10), y2 = rnd(-10, H + 10), x3 = rnd(0, W), y3 = rnd(0, H);
        const float from = (float)rnd(-360, 360), to = (float)rnd(-360, 720);
        const int   soft = a->color_bits == 8 && (i & 1) ? DGX_PEN_SOFT : 0;
        clear(a), clear(b);
        dgx_solid_circle(a, x, y, r | soft, color_of(a));
        dgx_fill_circle(b, x, y, r | soft, color_of(b));
        bad += differ(a, b) != 0;
        clear(a), clear(b);
        dgx_solid_sector(a, x, y, rx, ry, from, to, color_of(a));
        dgx_fill_sector(b, x, y, rx, ry, from, to, color_of(b));
        bad += differ(a, b) != 0;
        clear(a), clear(b);
        dgx_draw_triangle_solid(a, x, y, x1, y1, x2, y2, color_of(a));
        dgx_fill_triangle(b, x, y, x1, y1, x2, y2, color_of(b));
        bad += differ(a, b) != 0;
        clear(a), clear(b);
        dgx_draw_polygon4_solid(a, x, y, x1, y1, x2, y2, x3, y3, color_of(a));
        dgx_fill_polygon4(b, x, y, x1, y1, x2, y2, x3, y3, color_of(b));
        bad += differ(a, b) != 0;
    }
    CHECK(bad == 0);

    /* the thick line by both names, with the same arguments */
    bad = 0;
    for (int i = 0; i < 300; ++i) {
        const int x1 = rnd(-10, W + 10), y1 = rnd(-10, H + 10), x2 = rnd(-10, W + 10), y2 = rnd(-10, H + 10), width = rnd(1, 11);
        clear(a), clear(b);
        dgx_draw_line_thick(a, x1, y1, x2, y2, width, color_of(a));
        dgx_draw_thick_line(b, x1, y1, x2, y2, width, color_of(b));
        bad += differ(a, b) != 0;
    }
    CHECK(bad == 0);
}

/*
 * dgx_fill_ellipse() against the whole turn of dgx_fill_sector(), pixel for pixel: every pair of radii
 * from 0, the center on the screen, at its edges and off it. With equal radii it is the hard circle.
 */
static void test_ellipse(dgx_screen_t *a, dgx_screen_t *b)
{
    static const int centers[][2] = {{W / 2, H / 2}, {0, 0}, {W - 1, H - 1}, {3, H - 2}, {W + 5, H / 2}, {-7, -4}, {W / 2, -12}, {-40, H + 40}};
    int              unlike = 0, uncircle = 0, drawn = 0;
    for (size_t c = 0; c < sizeof(centers) / sizeof(*centers); ++c) {
        const int x = centers[c][0], y = centers[c][1];
        for (int rx = 0; rx <= 44; ++rx) {
            for (int ry = 0; ry <= 44; ry += rx % 3 ? 3 : 1) {
                clear(a), clear(b);
                dgx_fill_ellipse(a, x, y, rx, ry, color_of(a));
                dgx_fill_sector(b, x, y, rx, ry, 0, 360, color_of(b));
                unlike += differ(a, b) != 0;
                drawn += ink(a) != 0;
                if (rx == ry) {
                    clear(b);
                    dgx_fill_circle(b, x, y, rx, color_of(b));
                    uncircle += differ(a, b) != 0;
                }
            }
        }
    }
    CHECK(unlike == 0);
    CHECK(uncircle == 0);
    CHECK(drawn > 1000);
    if (unlike || uncircle) fprintf(stderr, "%d ellipses on %d bits are not the whole sector, %d of equal radii not the circle\n", unlike, a->color_bits, uncircle);
}

/* the filled ellipse is its outline and all within it: a row of it goes from the first pixel of the outline in the row to the last */
static void test_ellipse_outline(dgx_screen_t *fill, dgx_screen_t *line)
{
    int bad = 0;
    for (int i = 0; i < 400; ++i) {
        const int x = rnd(-10, W + 10), y = rnd(-10, H + 10), rx = rnd(0, 50), ry = rnd(0, 50);
        clear(fill), clear(line);
        dgx_fill_ellipse(fill, x, y, rx, ry, color_of(fill));
        dgx_draw_ellipse(line, x, y, rx, ry, color_of(line));
        for (int row = 0; row < H; ++row) {
            int first = -1, last = -1, from = -1, to = -1, filled = 0;
            for (int col = 0; col < W; ++col) {
                if (dgx_get_pixel(line, col, row)) {
                    if (first < 0) first = col;
                    last = col;
                }
                if (dgx_get_pixel(fill, col, row)) {
                    if (from < 0) from = col;
                    to = col;
                    ++filled;
                }
            }
            /* an outline cut by the edge of the screen has one end in the row, or none: the row is then filled to the edge */
            const bool whole = y - ry <= row && row <= y + ry;
            if (!whole) {
                bad += filled != 0;
                continue;
            }
            if (filled != (from < 0 ? 0 : to - from + 1)) ++bad;
            if (first >= 0 && (dgx_get_pixel(fill, first, row) == 0 || dgx_get_pixel(fill, last, row) == 0)) ++bad;
            /* where both ends of the row are on the screen, the outline's ends are the fill's */
            if (x - rx >= 0 && x + rx < W && (first != from || last != to)) ++bad;
        }
    }
    CHECK(bad == 0);
    if (bad) fprintf(stderr, "%d rows of 400 filled ellipses on %d bits are not the rows of their outline\n", bad, fill->color_bits);
}

/* a radius below 0 or past 32767, the bits of a pen among them, and a center past 32767 draw nothing; the edge is always hard */
static void test_ellipse_limits(dgx_screen_t *a, dgx_screen_t *b)
{
    clear(a), clear(b);
    dgx_fill_ellipse(a, 20, 20, -1, 5, color_of(a));
    dgx_fill_ellipse(a, 20, 20, 5, -1, color_of(a));
    dgx_fill_ellipse(a, 20, 20, 32768, 5, color_of(a));
    dgx_fill_ellipse(a, 20, 20, DGX_PEN_SOFT | 5, DGX_PEN_SOFT | 5, color_of(a));
    dgx_fill_ellipse(a, 20, 20, DGX_PEN_SOFT | 5, 5, color_of(a));
    dgx_fill_ellipse(a, 40000, 20, 5, 5, color_of(a));
    dgx_fill_ellipse(a, 20, -40000, 5, 5, color_of(a));
    CHECK(differ(a, b) == 0);
    /* the largest radii there are, the screen deep inside: all of it is filled */
    dgx_fill_ellipse(a, W / 2, H / 2, 32767, 32767, color_of(a));
    CHECK(ink(a) == (long)size_of(a));
    /* a dot and two lines */
    clear(a);
    dgx_fill_ellipse(a, 10, 10, 0, 0, color_of(a));
    CHECK(dgx_get_pixel(a, 10, 10) == color_of(a) && ink(a) == (long)(size_of(a) / (W * H)));
    clear(a), clear(b);
    dgx_fill_ellipse(a, 30, 30, 12, 0, color_of(a));
    dgx_fill_rectangle(b, 18, 30, 25, 1, color_of(b));
    CHECK(differ(a, b) == 0);
    clear(a), clear(b);
    dgx_fill_ellipse(a, 30, 30, 0, 9, color_of(a));
    dgx_fill_rectangle(b, 30, 21, 1, 19, color_of(b));
    CHECK(differ(a, b) == 0);
}

/* the outline of a triangle is its three lines; corners that are one point or lie in a line among them */
static void test_triangle(dgx_screen_t *a, dgx_screen_t *b)
{
    int bad = 0;
    for (int i = 0; i < 2000; ++i) {
        int x0 = rnd(-15, W + 15), y0 = rnd(-15, H + 15), x1 = rnd(-15, W + 15), y1 = rnd(-15, H + 15), x2 = rnd(-15, W + 15), y2 = rnd(-15, H + 15);
        if (i % 50 == 0) x1 = x0, y1 = y0;
        if (i % 70 == 0) x2 = x0, y2 = y0, x1 = x0, y1 = y0;
        if (i % 90 == 0) y0 = y1 = y2;
        clear(a), clear(b);
        dgx_draw_triangle(a, x0, y0, x1, y1, x2, y2, color_of(a));
        dgx_draw_line(b, x0, y0, x1, y1, color_of(b));
        dgx_draw_line(b, x1, y1, x2, y2, color_of(b));
        dgx_draw_line(b, x2, y2, x0, y0, color_of(b));
        bad += differ(a, b) != 0;
    }
    CHECK(bad == 0);
}

/*
 * The outline is the border of the filled triangle: every pixel of the three lines is in the fill, a row
 * of the fill begins and ends with a pixel of the outline and has no hole, its top and bottom rows are
 * outline all through, and nothing lies outside the box of the corners. Corners in a line, two or three
 * in one point and level sides are among the triangles.
 */
static void test_triangle_and_fill(dgx_screen_t *line, dgx_screen_t *fill)
{
    int outside = 0, beyond = 0, ends = 0, holes = 0, caps = 0;
    for (int i = 0; i < 3000; ++i) {
        int x0 = rnd(0, W - 1), y0 = rnd(0, H - 1), x1 = rnd(0, W - 1), y1 = rnd(0, H - 1), x2 = rnd(0, W - 1), y2 = rnd(0, H - 1);
        if (i % 7 == 0) y1 = y0;                             /* a level side */
        if (i % 11 == 0) x2 = x0, y2 = y0;                   /* two corners in one point */
        if (i % 13 == 0) x1 = (x0 + x2) / 2, y1 = (y0 + y2) / 2; /* nearly in a line */
        if (i % 17 == 0) y1 = y0 + 1;                        /* a side that runs nearly level */
        const int left = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), right = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        const int top = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), bottom = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        clear(line), clear(fill);
        dgx_draw_triangle(line, x0, y0, x1, y1, x2, y2, color_of(line));
        dgx_fill_triangle(fill, x0, y0, x1, y1, x2, y2, color_of(fill));
        for (int y = 0; y < H; ++y) {
            int first = -1, last = -1, count = 0, lined = 0;
            for (int x = 0; x < W; ++x) {
                const bool l = dgx_get_pixel(line, x, y) != 0, f = dgx_get_pixel(fill, x, y) != 0;
                outside += l && !f;
                beyond += (l || f) && (x < left || x > right || y < top || y > bottom);
                if (f) {
                    if (first < 0) first = x;
                    last = x, ++count, lined += l;
                }
            }
            if (first < 0) continue;
            ends += !dgx_get_pixel(line, first, y) || !dgx_get_pixel(line, last, y);
            holes += count != last - first + 1;
            if (y >= 0 && y < H && (y == top || y == bottom)) caps += lined != count;
        }
    }
    CHECK(outside == 0);
    CHECK(beyond == 0);
    CHECK(ends == 0);
    CHECK(holes == 0);
    CHECK(caps == 0);
}

/* each figure is one update of the screen; one that draws nothing is none */
static void test_updates(void)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    s->update_screen = record;
    calls = 0;
    dgx_fill_ellipse(s, 40, 40, 20, 9, 0xffff);
    CHECK(calls == 1);
    calls = 0;
    dgx_draw_triangle(s, 5, 5, 60, 20, 30, 70, 0xffff);
    CHECK(calls == 1);
    calls = 0;
    dgx_fill_triangle(s, 5, 5, 60, 20, 30, 70, 0xffff);
    CHECK(calls == 1);
    calls = 0;
    dgx_fill_polygon4(s, 5, 5, 60, 20, 50, 60, 10, 70, 0xffff);
    CHECK(calls == 1);
    calls = 0;
    dgx_fill_sector(s, 40, 40, 20, 20, 30, 200, 0xffff);
    CHECK(calls == 1);
    calls = 0;
    dgx_fill_circle(s, 40, 40, 15, 0xffff);
    CHECK(calls == 1);
    calls = 0;
    dgx_draw_thick_line(s, 5, 70, 80, 10, 4, 0xffff);
    CHECK(calls == 1);
    calls = 0;
    dgx_fill_ellipse(s, -100, -100, 20, 9, 0xffff);
    dgx_fill_ellipse(s, 40, 40, -2, 9, 0xffff);
    dgx_draw_triangle(s, -5, -5, -60, -20, -30, -70, 0xffff);
    CHECK(calls == 0);
    dgx_screen_destroy(&s);
}

int main(void)
{
    dgx_screen_t *a16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB), *b16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    dgx_screen_t *a8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *b8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    CHECK(a16 && b16 && a8 && b8);

    test_names(a16, b16);
    test_names(a8, b8);
    test_ellipse(a16, b16);
    test_ellipse(a8, b8);
    test_ellipse_outline(a16, b16);
    test_ellipse_limits(a16, b16);
    test_ellipse_limits(a8, b8);
    test_triangle(a16, b16);
    test_triangle(a8, b8);
    test_triangle_and_fill(a16, b16);
    test_updates();

    dgx_screen_destroy(&a16), dgx_screen_destroy(&b16), dgx_screen_destroy(&a8), dgx_screen_destroy(&b8);
    CHECK_DONE();
}
