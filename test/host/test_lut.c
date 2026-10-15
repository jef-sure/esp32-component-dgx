/*
 * Tables of RGB565 colors for an 8-bit screen: from a palette, a ramp between
 * two colors, a mix of two palettes or of a palette with one color. The ends
 * are exact, the rest rounds to the nearest, and nothing outside the entries
 * asked for is touched.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_lut.h"

#define UNTOUCHED 0xa5a5u

static uint32_t seed = 99;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static uint16_t rgb16(const uint8_t c[3])
{
    return (uint16_t)dgx_rgb_to_16(c[0], c[1], c[2]);
}

static uint16_t packed16(uint32_t rgb)
{
    return (uint16_t)dgx_rgb_to_16((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

static void clear(uint16_t *lut)
{
    for (int i = 0; i < 256; ++i) lut[i] = UNTOUCHED;
}

/* entries outside first .. first + count - 1 still hold what they held */
static int touched_outside(const uint16_t *lut, int first, int count)
{
    int n = 0;
    for (int i = 0; i < 256; ++i) n += (i < first || i >= first + count) && lut[i] != UNTOUCHED;
    return n;
}

static uint8_t palette_a[256][3], palette_b[256][3];

static void test_from_rgb(void)
{
    uint16_t lut[256];
    static const int cases[][2] = {{0, 256}, {10, 20}, {250, 10}, {-3, 5}, {-300, 10}, {256, 4}, {7, 0}, {7, -2}, {0, 1}, {255, 1}};
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const int first = cases[c][0], count = cases[c][1];
        clear(lut);
        dgx_lut_from_rgb(lut, first, count, palette_a);
        int wrong = 0;
        for (int k = 0; k < count; ++k) {
            if (first + k >= 0 && first + k < 256) wrong += lut[first + k] != rgb16(palette_a[k]);
        }
        CHECK(wrong == 0);
        CHECK(touched_outside(lut, first, count) == 0);
    }
}

/* the channels of an RGB565 color, as 0..31 or 0..63 */
static void channels(uint16_t c, int *r, int *g, int *b)
{
    *r = c >> 11, *g = (c >> 5) & 63, *b = c & 31;
}

static void test_gradient(void)
{
    uint16_t lut[256];
    for (int round = 0; round < 2000; ++round) {
        const uint32_t from = (uint32_t)rnd(0, 0xffffff), to = (uint32_t)rnd(0, 0xffffff);
        const int      first = round % 5 == 0 ? rnd(-40, 250) : rnd(0, 128), count = round % 7 == 0 ? rnd(-2, 3) : rnd(2, 300);
        clear(lut);
        dgx_lut_gradient(lut, first, count, from, to);
        CHECK(touched_outside(lut, first, count) == 0);
        if (count <= 0) continue;
        /* the ends exactly */
        if (first >= 0 && first < 256) CHECK(lut[first] == packed16(from));
        if (count > 1 && first + count - 1 >= 0 && first + count - 1 < 256) CHECK(lut[first + count - 1] == packed16(to));
        /* every entry the nearest of the straight way, and every channel goes one way */
        int wrong = 0, back = 0, pr = -1, pg = -1, pb = -1;
        for (int k = 0; k < count; ++k) {
            if (first + k < 0 || first + k > 255) continue;
            uint8_t c[3];
            for (int i = 0; i < 3; ++i) {
                const double a = (from >> (16 - 8 * i)) & 0xff, b = (to >> (16 - 8 * i)) & 0xff;
                c[i] = (uint8_t)floor(count > 1 ? a + (b - a) * k / (count - 1) + 0.5 : a);
            }
            const uint16_t want = rgb16(c);
            wrong += lut[first + k] != want;
            int r, g, b;
            channels(lut[first + k], &r, &g, &b);
            if (pr >= 0) {
                back += ((from >> 16 & 0xff) <= (to >> 16 & 0xff) ? r < pr : r > pr);
                back += ((from >> 8 & 0xff) <= (to >> 8 & 0xff) ? g < pg : g > pg);
                back += ((from & 0xff) <= (to & 0xff) ? b < pb : b > pb);
            }
            pr = r, pg = g, pb = b;
        }
        CHECK(wrong == 0);
        CHECK(back == 0);
    }

    /*
     * The loop the examples had, from the paper to the ink over 256 entries:
     * it truncates, the gradient rounds, so they differ by one step of a
     * channel of 8 bits at most, and in RGB565 by one step at most.
     */
    dgx_lut_gradient(lut, 0, 256, dgx_rgb_to_24(250, 246, 232), dgx_rgb_to_24(24, 36, 120));
    int same = 0, far = 0;
    for (int i = 0; i < 256; ++i) {
        const uint16_t old = (uint16_t)dgx_rgb_to_16((uint8_t)(250 - (250 - 24) * i / 255), (uint8_t)(246 - (246 - 36) * i / 255),
                                                     (uint8_t)(232 - (232 - 120) * i / 255));
        int r0, g0, b0, r1, g1, b1;
        channels(old, &r0, &g0, &b0);
        channels(lut[i], &r1, &g1, &b1);
        same += old == lut[i];
        far += abs(r0 - r1) > 1 || abs(g0 - g1) > 1 || abs(b0 - b1) > 1;
    }
    CHECK(far == 0);
    CHECK(same > 200);
}

static void test_mix(void)
{
    uint16_t lut[256], other[256], expected[256];
    for (int t = 0; t < 256; ++t) {
        clear(lut);
        dgx_lut_mix(lut, 0, 256, palette_a, palette_b, (uint8_t)t);
        /* the same mix the other way round */
        dgx_lut_mix(other, 0, 256, palette_b, palette_a, (uint8_t)(255 - t));
        int wrong = 0, asym = 0;
        for (int k = 0; k < 256; ++k) {
            uint8_t c[3];
            for (int i = 0; i < 3; ++i) c[i] = (uint8_t)floor((palette_a[k][i] * (255.0 - t) + palette_b[k][i] * (double)t) / 255.0 + 0.5);
            wrong += lut[k] != rgb16(c);
            asym += lut[k] != other[k];
        }
        CHECK(wrong == 0);
        CHECK(asym == 0);
    }
    /* the ends exactly */
    dgx_lut_from_rgb(expected, 0, 256, palette_a);
    dgx_lut_mix(lut, 0, 256, palette_a, palette_b, 0);
    int wrong = 0;
    for (int k = 0; k < 256; ++k) wrong += lut[k] != expected[k];
    dgx_lut_from_rgb(expected, 0, 256, palette_b);
    dgx_lut_mix(lut, 0, 256, palette_a, palette_b, 255);
    for (int k = 0; k < 256; ++k) wrong += lut[k] != expected[k];
    CHECK(wrong == 0);
    /* a part of the table: entry k of the palettes goes to first + k */
    clear(lut);
    dgx_lut_mix(lut, 200, 80, palette_a, palette_b, 0);
    for (int k = 0; k < 56; ++k) wrong += lut[200 + k] != rgb16(palette_a[k]);
    CHECK(wrong == 0);
    CHECK(touched_outside(lut, 200, 80) == 0);
}

static void test_mix_color(void)
{
    uint16_t lut[256], via_mix[256];
    static uint8_t one[256][3];
    for (int round = 0; round < 300; ++round) {
        const uint32_t rgb = round == 0 ? 0 : round == 1 ? 0xffffff : (uint32_t)rnd(0, 0xffffff);
        const int      t = round < 2 ? 128 : rnd(0, 255), first = rnd(-20, 200), count = rnd(-1, 120);
        for (int k = 0; k < 256; ++k) one[k][0] = (uint8_t)(rgb >> 16), one[k][1] = (uint8_t)(rgb >> 8), one[k][2] = (uint8_t)rgb;
        /* the same as a mix with a palette of that one color */
        clear(lut);
        clear(via_mix);
        dgx_lut_mix_color(lut, first, count, palette_a, rgb, (uint8_t)t);
        dgx_lut_mix(via_mix, first, count, palette_a, one, (uint8_t)t);
        int wrong = 0;
        for (int i = 0; i < 256; ++i) wrong += lut[i] != via_mix[i];
        CHECK(wrong == 0);
        CHECK(touched_outside(lut, first, count) == 0);
    }
    /* a fade to black ends in black, a flash to white in white, and both start from the palette */
    uint16_t palette[256];
    dgx_lut_from_rgb(palette, 0, 256, palette_a);
    dgx_lut_mix_color(lut, 0, 256, palette_a, 0x000000, 255);
    int wrong = 0;
    for (int k = 0; k < 256; ++k) wrong += lut[k] != 0;
    dgx_lut_mix_color(lut, 0, 256, palette_a, 0xffffff, 255);
    for (int k = 0; k < 256; ++k) wrong += lut[k] != 0xffff;
    dgx_lut_mix_color(lut, 0, 256, palette_a, 0xffffff, 0);
    for (int k = 0; k < 256; ++k) wrong += lut[k] != palette[k];
    CHECK(wrong == 0);
}

int main(void)
{
    for (int k = 0; k < 256; ++k) {
        for (int i = 0; i < 3; ++i) palette_a[k][i] = (uint8_t)rnd(0, 255), palette_b[k][i] = (uint8_t)rnd(0, 255);
    }
    palette_a[0][0] = palette_a[0][1] = palette_a[0][2] = 0;
    palette_b[1][0] = palette_b[1][1] = palette_b[1][2] = 255;
    test_from_rgb();
    test_gradient();
    test_mix();
    test_mix_color();
    CHECK_DONE();
}
