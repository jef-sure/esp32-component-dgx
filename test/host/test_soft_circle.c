/*
 * dgx_solid_circle() with DGX_PEN_SOFT: on a virtual screen of a byte a pixel it covers what is under it
 * by the share of each pixel it covers; with DGX_PEN_MAX it is the dot of a soft pen of the width 2r + 1
 * byte for byte, drawn by rows; on other screens it is the hard circle; the pens take no notice of
 * DGX_PEN_MAX.
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

static uint8_t *memory(dgx_screen_t *s)
{
    return ((dgx_vscreen_t *)s)->v_array;
}

static size_t size_of(dgx_screen_t *s)
{
    return (size_t)W * H * ((s->color_bits + 7) / 8);
}

static int differ(dgx_screen_t *a, dgx_screen_t *b)
{
    int n = 0;
    for (size_t i = 0; i < size_of(a); ++i) n += memory(a)[i] != memory(b)[i];
    return n;
}

/* the same paper on both: random, all of one ink, or, with -2, clean but for a pixel of ink here and there */
static void paper(dgx_screen_t *a, dgx_screen_t *b, int ink)
{
    for (size_t i = 0; i < size_of(a); ++i) memory(a)[i] = (uint8_t)(ink == -1 ? rand() & 0xff : ink == -2 ? (rand() % 23 ? 0 : rand() & 0xff) : ink);
    memcpy(memory(b), memory(a), size_of(a));
}

/* the dot of a soft pen of the width 2r + 1 around (x, y) */
static void pen_dot(dgx_screen_t *s, int x, int y, int r, uint32_t color)
{
    dgx_draw_thick_segment(s, (float)x, (float)y, (float)x, (float)y, DGX_PEN_SOFT | DGX_PEN_START_DISC | (2 * r + 1), color);
}

/* centres in the middle, by the edges and past them, so that the circle is cut by the screen every way */
static void centres(int r, int out[][2], int *n)
{
    const int c[][2] = {{W / 2, H / 2}, {3, 4}, {W - 2, H - 3}, {-5, H / 3}, {W / 3, -r + 2}, {W + r - 3, H / 2}, {W / 2, H + r - 1}, {-r, -r}};
    *n = (int)(sizeof c / sizeof c[0]);
    memcpy(out, c, sizeof c);
}

static void test_dot(dgx_screen_t *a, dgx_screen_t *b)
{
    const int      radii[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 12, 17, 25, 33, 48, 60, 99, 150, 400, 716, 717};
    const uint32_t inks[] = {255, 140, 1, 0};
    for (size_t ri = 0; ri < sizeof radii / sizeof radii[0]; ++ri) {
        const int r = radii[ri];
        int       cs[8][2], n;
        centres(r, cs, &n);
        for (int ci = 0; ci < n; ++ci) {
            /* on a clean paper with some ink here and there the inside goes by words of 0 and by bytes */
            for (int kind = -2; kind <= -1; ++kind) {
                for (size_t ii = 0; ii < sizeof inks / sizeof inks[0]; ++ii) {
                    paper(a, b, kind);
                    dgx_solid_circle(a, cs[ci][0], cs[ci][1], DGX_PEN_SOFT | DGX_PEN_MAX | r, inks[ii]);
                    pen_dot(b, cs[ci][0], cs[ci][1], r, inks[ii]);
                    const int d = differ(a, b);
                    if (d) printf("r %d at %d,%d ink %u: %d pixels differ from the dot of the pen\n", r, cs[ci][0], cs[ci][1], (unsigned)inks[ii], d);
                    CHECK(d == 0);
                }
            }
        }
        /* the edge right through the screen: the centre far off, the radius large */
        if (r > H) {
            paper(a, b, -1);
            dgx_solid_circle(a, W / 2, H / 2 + r - 20, DGX_PEN_SOFT | DGX_PEN_MAX | r, 200);
            pen_dot(b, W / 2, H / 2 + r - 20, r, 200);
            CHECK(differ(a, b) == 0);
        }
    }
}

/*
 * Past a radius of 717 the disc of the pen counts the way of a pixel by a root within 5 millionths of
 * it, and past 787 it may give a pixel just past r one more ink than all of it, 256, which is 0 in a
 * byte. The circle counts closer: it is checked against the share counted exactly, all the ink up to
 * r, nothing from r + 1, between them (r + 1 - way) of it, give or take one.
 */
static void test_large(dgx_screen_t *a)
{
    const int radii[] = {2, 3, 5, 40, 717, 718, 788, 800, 1000, 2047, 5000, 16383};
    const int inks[] = {255, 140};
    for (size_t ri = 0; ri < sizeof radii / sizeof radii[0]; ++ri) {
        const int r = radii[ri];
        const int cs[][2] = {{W / 2, H / 2 + r - 20}, {W / 2 - r + 30, H / 2}, {W + r - 40, H / 2 + 7}, {W / 3, -r + 60}};
        for (size_t ci = 0; ci < sizeof cs / sizeof cs[0]; ++ci) {
            for (size_t ii = 0; ii < sizeof inks / sizeof inks[0]; ++ii) {
                const int full = inks[ii];
                memset(memory(a), 0, size_of(a));
                dgx_solid_circle(a, cs[ci][0], cs[ci][1], DGX_PEN_SOFT | DGX_PEN_MAX | r, (uint32_t)full);
                int wrong = 0;
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        const long long dx = x - cs[ci][0], dy = y - cs[ci][1], d2 = dx * dx + dy * dy;
                        const int       v = memory(a)[y * W + x];
                        if (d2 <= (long long)r * r) {
                            wrong += v != full;
                        } else if (d2 >= (long long)(r + 1) * (r + 1)) {
                            wrong += v != 0;
                        } else {
                            const int e = (int)((r + 1 - sqrt((double)d2)) * full + 0.5);
                            wrong += v > full || v < e - 1 || v > e + 1;
                        }
                    }
                }
                if (wrong) printf("large r %d at %d,%d ink %d: %d pixels wrong\n", r, cs[ci][0], cs[ci][1], full, wrong);
                CHECK(wrong == 0);
            }
        }
    }
}

/* what the circle touches is its box, cut by the screen: no more and no less */
static void test_dirty(dgx_screen_t *a)
{
    const int c[][3] = {{60, 50, 10}, {2, 3, 7}, {118, 97, 30}, {-4, 50, 6}, {60, 50, 0}};
    for (size_t i = 0; i < sizeof c / sizeof c[0]; ++i) {
        const int x = c[i][0], y = c[i][1], r = c[i][2];
        int       left, top, width, height;
        dgx_screen_progress_up(a);
        dgx_solid_circle(a, x, y, DGX_PEN_SOFT | r, 255);
        CHECK(dgx_screen_take_dirty(a, &left, &top, &width, &height));
        dgx_screen_progress_down(a);
        const int l = x - r < 0 ? 0 : x - r, t = y - r < 0 ? 0 : y - r;
        const int rr = x + r >= W ? W - 1 : x + r, bb = y + r >= H ? H - 1 : y + r;
        CHECK(left == l && top == t && width == rr - l + 1 && height == bb - t + 1);
    }
    /* wholly off the screen: nothing */
    int left, top, width, height;
    dgx_screen_progress_up(a);
    dgx_solid_circle(a, -20, 10, DGX_PEN_SOFT | 5, 255);
    CHECK(!dgx_screen_take_dirty(a, &left, &top, &width, &height));
    dgx_screen_progress_down(a);
}

/* the larger ink stays: a lighter circle leaves a darker paper as it is, and makes no pixel lighter */
static void test_keep_larger(dgx_screen_t *a, dgx_screen_t *b)
{
    paper(a, b, 200);
    dgx_solid_circle(a, 60, 50, DGX_PEN_SOFT | DGX_PEN_MAX | 20, 100);
    CHECK(differ(a, b) == 0);
    paper(a, b, -1);
    dgx_solid_circle(a, 60, 50, DGX_PEN_SOFT | DGX_PEN_MAX | 30, 120);
    int lighter = 0;
    for (size_t i = 0; i < size_of(a); ++i) lighter += memory(a)[i] < memory(b)[i];
    CHECK(lighter == 0);
    /* a dot of one pixel is the pixel */
    paper(a, b, 0);
    dgx_solid_circle(a, 7, 9, DGX_PEN_SOFT | DGX_PEN_MAX | 0, 77);
    CHECK(memory(a)[9 * W + 7] == 77 && differ(a, b) == 1);
    dgx_solid_circle(a, 17, 9, DGX_PEN_SOFT | 0, 77);
    CHECK(memory(a)[9 * W + 17] == 77 && differ(a, b) == 2);
    /* no radius, no circle, whatever bits it has */
    paper(a, b, 0);
    dgx_solid_circle(a, 7, 9, -1, 77);
    dgx_solid_circle(a, 7, 9, DGX_PEN_SOFT | DGX_PEN_MAX | -3, 77);
    dgx_solid_circle(a, 7, 9, DGX_PEN_SOFT | -3, 77);
    CHECK(differ(a, b) == 0);
}

/*
 * Covering: a pixel goes from what it had towards the ink by the share the circle covers, and that
 * share is the ink the soft circle with all the ink leaves on a clean paper, by the rule of the pen the
 * dot of the pen, see test_dot().
 */
static void test_cover(dgx_screen_t *a, dgx_screen_t *b, dgx_screen_t *share)
{
    const int      radii[] = {0, 1, 2, 3, 5, 8, 13, 40, 120, 800};
    const uint32_t inks[] = {255, 60, 0};
    for (size_t ri = 0; ri < sizeof radii / sizeof radii[0]; ++ri) {
        const int r = radii[ri];
        int       cs[8][2], n;
        centres(r, cs, &n);
        for (int ci = 0; ci < n; ++ci) {
            memset(memory(share), 0, size_of(share));
            dgx_solid_circle(share, cs[ci][0], cs[ci][1], DGX_PEN_SOFT | DGX_PEN_MAX | r, 255);
            for (size_t ii = 0; ii < sizeof inks / sizeof inks[0]; ++ii) {
                const int ink = (int)inks[ii];
                paper(a, b, -1);
                dgx_solid_circle(a, cs[ci][0], cs[ci][1], DGX_PEN_SOFT | r, (uint32_t)ink);
                int wrong = 0;
                for (size_t i = 0; i < size_of(a); ++i) {
                    const int s = memory(share)[i], was = memory(b)[i];
                    const int want = (was * (255 - s) + ink * s + 127) / 255;
                    wrong += memory(a)[i] != want;
                }
                if (wrong) printf("cover r %d at %d,%d ink %d: %d pixels wrong\n", r, cs[ci][0], cs[ci][1], ink, wrong);
                CHECK(wrong == 0);
            }
        }
    }
    /* inside it is the ink, darker or lighter than the paper */
    paper(a, b, 200);
    dgx_solid_circle(a, 60, 50, DGX_PEN_SOFT | 20, 30);
    CHECK(memory(a)[50 * W + 60] == 30 && memory(a)[50 * W + 79] == 30 && memory(a)[50 * W + 82] == 200);
}

/* where a pixel is not an amount of ink, or the radius is too large, the circle asked to be soft is the hard one */
static void test_hard_elsewhere(dgx_screen_t *a8, dgx_screen_t *b8, dgx_screen_t *a16, dgx_screen_t *b16)
{
    const int bits[] = {DGX_PEN_SOFT, DGX_PEN_SOFT | DGX_PEN_MAX, DGX_PEN_MAX, DGX_PEN_START_DISC | DGX_PEN_END_DISC};
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; ++i) {
        for (int r = 0; r < 40; r += 7) {
            paper(a16, b16, -1);
            dgx_solid_circle(a16, 50, 40, bits[i] | r, 0xa5c3);
            dgx_solid_circle(b16, 50, 40, r, 0xa5c3);
            CHECK(differ(a16, b16) == 0);
        }
    }
    /* on a screen of bytes the bits that are not DGX_PEN_SOFT change nothing of a hard circle */
    paper(a8, b8, -1);
    dgx_solid_circle(a8, 50, 40, DGX_PEN_MAX | DGX_PEN_START_DISC | 17, 99);
    dgx_solid_circle(b8, 50, 40, 17, 99);
    CHECK(differ(a8, b8) == 0);
    /* larger than the widest pen */
    paper(a8, b8, -1);
    dgx_solid_circle(a8, W / 2, H / 2 + 16384 - 20, DGX_PEN_SOFT | DGX_PEN_MAX | 16384, 99);
    dgx_solid_circle(b8, W / 2, H / 2 + 16384 - 20, 16384, 99);
    CHECK(differ(a8, b8) == 0);
}

/* a pen takes no notice of DGX_PEN_MAX */
static void test_pens_ignore_max(dgx_screen_t *a, dgx_screen_t *b)
{
    const int pens[] = {1, 2, 5, DGX_PEN_SOFT | 1, DGX_PEN_SOFT | 4, DGX_PEN_SOFT | DGX_PEN_START_DISC | 7};
    for (size_t i = 0; i < sizeof pens / sizeof pens[0]; ++i) {
        const dgx_point_2d_t curve[4] = {{10, 80}, {30, 5}, {90, 95}, {110, 20}};
        paper(a, b, 0);
        dgx_draw_thick_segment(a, 10.5f, 20.25f, 100.f, 70.f, DGX_PEN_MAX | pens[i], 180);
        dgx_draw_thick_segment(b, 10.5f, 20.25f, 100.f, 70.f, pens[i], 180);
        CHECK(differ(a, b) == 0);
        paper(a, b, 0);
        dgx_draw_bezier4(a, curve, 0, DGX_PEN_MAX | pens[i], 180);
        dgx_draw_bezier4(b, curve, 0, pens[i], 180);
        CHECK(differ(a, b) == 0);
        paper(a, b, 0);
        dgx_draw_arc(a, 60, 50, 40, 30, 0.3f, 4.0f, DGX_PEN_MAX | pens[i], 180);
        dgx_draw_arc(b, 60, 50, 40, 30, 0.3f, 4.0f, pens[i], 180);
        CHECK(differ(a, b) == 0);
    }
}

int main(void)
{
    srand(5);
    dgx_screen_t *a8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *b8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_screen_t *c8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_screen_t *a16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB), *b16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    CHECK(a8 && b8 && c8 && a16 && b16);
    test_dot(a8, b8);
    test_large(a8);
    test_dirty(a8);
    test_keep_larger(a8, b8);
    test_cover(a8, b8, c8);
    test_hard_elsewhere(a8, b8, a16, b16);
    test_pens_ignore_max(a8, b8);
    dgx_screen_destroy(&a8), dgx_screen_destroy(&b8), dgx_screen_destroy(&c8), dgx_screen_destroy(&a16), dgx_screen_destroy(&b16);
    CHECK_DONE();
}
