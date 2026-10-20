/*
 * The lines of a picture made larger, against the shaders written out a
 * point at a time (scale_ref.h): every way of smoothing, every scale, rows
 * and columns, forward and back, asked in any order; lines of indices, of
 * values and of colors.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"
#include "scale2x_ref.h"
#include "scale_ref.h"
#include "vscreen/dgx_scale_priv.h"

static uint32_t seed = 4242;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

#define PW 13 /* the picture */
#define PH 9

static const int smooth_kinds[3] = {REF_BLOCKS, REF_SMOOTH, REF_SFX};
static const uint8_t bus_smooth[3] = {0, DGX_BUS_SMOOTH, DGX_BUS_SMOOTH_SFX};

/* a picture of few values, for many neighbours to be equal; now and then a thin slanted line, the trouble of Scale2x */
static void make_picture(uint32_t *values, int kinds)
{
    for (int i = 0; i < PW * PH; ++i) values[i] = (uint32_t)rnd(0, kinds - 1);
    if (rnd(0, 1)) {
        const int x = rnd(0, PW - 1), y = rnd(0, PH - 1), dx = rnd(0, 1) ? 1 : -1;
        for (int i = 0; i < 6; ++i) {
            const int px = x + i * dx, py = y + i;
            if (px >= 0 && px < PW && py < PH) values[py * PW + px] = 7;
        }
    }
}

/* Scale2x of the engine at 2 and 4 is what the point-at-a-time Scale2x gives */
static void test_reference_against_engine(void)
{
    static uint32_t values[PW * PH];
    static uint8_t  bytes[PW * PH], larger[16 * PW * PH];
    int             wrong = 0;
    for (int round = 0; round < 60; ++round) {
        make_picture(values, 2 + round % 3);
        for (int i = 0; i < PW * PH; ++i) bytes[i] = (uint8_t)values[i];
        const ref_picture_t p = {values, PW, PH};
        for (int n = 2; n <= 4; n += 2) {
            ref_smooth(bytes, PW, PH, n, larger);
            for (int y = 0; y < n * PH; ++y)
                for (int x = 0; x < n * PW; ++x) wrong += larger[y * n * PW + x] != ref_larger(&p, n, REF_SMOOTH, x, y);
        }
    }
    CHECK(wrong == 0);
}

/*
 * Every way of smoothing gives the same picture whether the picture is
 * mirrored or transposed before it or after it: a line of the larger
 * picture may be made along a row or along a column.
 */
static void test_reference_turns(void)
{
    static uint32_t values[PW * PH], mirrored[PW * PH], transposed[PW * PH];
    int             wrong = 0, smoothed = 0;
    for (int round = 0; round < 40; ++round) {
        make_picture(values, 2 + round % 3);
        for (int y = 0; y < PH; ++y) {
            for (int x = 0; x < PW; ++x) {
                mirrored[y * PW + PW - 1 - x] = values[y * PW + x];
                transposed[x * PH + y]        = values[y * PW + x];
            }
        }
        const ref_picture_t p = {values, PW, PH}, m = {mirrored, PW, PH}, t = {transposed, PH, PW};
        for (int n = 2; n <= 4; ++n) {
            for (int k = 1; k < 3; ++k) {
                const int smooth = smooth_kinds[k];
                for (int y = 0; y < n * PH; ++y) {
                    for (int x = 0; x < n * PW; ++x) {
                        const uint32_t v = ref_larger(&p, n, smooth, x, y);
                        wrong += v != ref_larger(&m, n, smooth, n * PW - 1 - x, y);
                        wrong += v != ref_larger(&t, n, smooth, y, x);
                        smoothed += v != ref_larger(&p, n, REF_BLOCKS, x, y);
                    }
                }
            }
        }
    }
    CHECK(wrong == 0);
    /* and the smoothing changes something */
    CHECK(smoothed > 1000);
}

/* the point `at` of the line `line` of the larger picture, a row or a column of it */
static uint32_t expected_point(const ref_picture_t *p, int n, int smooth, bool along_y, int32_t line, int32_t at)
{
    return along_y ? ref_larger(p, n, smooth, line, at) : ref_larger(p, n, smooth, at, line);
}

/* lines of indices and of colors, asked one after another forward, back, or anywhere */
static void test_indices(void)
{
    static uint32_t values[PW * PH];
    static uint8_t  indices[PW * PH];
    static uint16_t lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)(i * 2654435761u >> 16);
    int checked = 0;
    for (int round = 0; round < 3000; ++round) {
        if (round % 20 == 0) {
            make_picture(values, 2 + rnd(0, 3));
            for (int i = 0; i < PW * PH; ++i) indices[i] = (uint8_t)values[i];
        }
        const ref_picture_t     p = {values, PW, PH};
        const int               n = rnd(1, 5), kind = rnd(0, 2), smooth = smooth_kinds[kind];
        const bool              along_y = rnd(0, 1);
        const int               run = along_y ? n * PH : n * PW, lines_n = along_y ? n * PW : n * PH;
        const int32_t           from = rnd(0, run - 1);
        const uint32_t          count = (uint32_t)rnd(1, run - from);
        const dgx_bus_indexed_t picture = {.pixels = indices, .pitch = PW, .width = PW, .height = PH, .scale = (uint8_t)n, .smooth = bus_smooth[kind], .lut = lut};
        uint8_t                *scratch = malloc(dgx_scale_scratch((uint8_t)n, picture.smooth, count, 1));
        dgx_scale_lines_t       lines;
        dgx_scale_lines_begin(&lines, &picture, along_y, from, count, scratch);
        /* the lines in order, back, or anywhere */
        const int order = rnd(0, 2);
        for (int i = 0; i < 2 * lines_n; ++i) {
            const int32_t  at = order == 0 ? i % lines_n : order == 1 ? lines_n - 1 - i % lines_n : rnd(0, lines_n - 1);
            const uint8_t *line = dgx_scale_line(&lines, at);
            int            wrong = 0;
            for (uint32_t k = 0; k < count; ++k) wrong += line[k] != expected_point(&p, n, smooth, along_y, at, from + (int32_t)k);
            if (wrong) {
                fprintf(stderr, "indices: scale %d smooth %d along %s, line %d, points %d..%d: %d wrong\n", n, smooth, along_y ? "y" : "x", at, from,
                        from + (int)count - 1, wrong);
                ++check_failures;
                break;
            }
            ++checked;
        }
        /* colors of a smoothed picture: some lines of a block at a time, forward or back, rows made forward or from their ends */
        if (picture.smooth && (n == 2 || n == 3 || n == 4)) {
            const int       dir = rnd(0, 1) ? 1 : -1, step = rnd(0, 1) ? 1 : -1;
            static uint16_t rows[3 * 64];
            for (int i = 0; i < 8; ++i) {
                const int32_t at = rnd(0, lines_n - 1);
                uint32_t      asked = dgx_scale_lines_left(&lines, at, dir);
                CHECK(asked >= 1 && asked <= (uint32_t)(n == 3 ? 3 : 2));
                if (rnd(0, 1)) asked = (uint32_t)rnd(1, (int)asked);
                memset(rows, 0, sizeof(rows));
                dgx_scale_colors(&lines, at, dir, asked, rows + (step < 0 ? count - 1 : 0), 64, step);
                int wrong = 0;
                for (uint32_t r = 0; r < asked; ++r) {
                    for (uint32_t k = 0; k < count; ++k) {
                        const uint16_t got = rows[r * 64 + (step < 0 ? count - 1 - k : k)];
                        wrong += got != lut[expected_point(&p, n, smooth, along_y, at + (int32_t)r * dir, from + (int32_t)k)];
                    }
                }
                if (wrong) {
                    fprintf(stderr, "colors: scale %d smooth %d along %s, line %d dir %d asked %u step %d: %d wrong\n", n, smooth, along_y ? "y" : "x",
                            at, dir, asked, step, wrong);
                    ++check_failures;
                    break;
                }
                ++checked;
            }
        }
        free(scratch);
    }
    CHECK(checked > 50000);
}

/* lines of values of virtual screens of several depths, read from their memory and by get_pixel() */
static void test_values(void)
{
    static const uint8_t depths[] = {16, 24, 8, 32, 4, 1};
    static uint32_t      values[PW * PH];
    int                  checked = 0;
    for (int round = 0; round < 1200; ++round) {
        const uint8_t bits = depths[round % 6];
        dgx_screen_t *scr  = dgx_vscreen_init(PW, PH, bits, DgxScreenRGB);
        make_picture(values, 2 + rnd(0, 3));
        /* the few kinds spread over the depth: what set_pixel() takes and get_pixel() gives back */
        for (int i = 0; i < PW * PH; ++i) {
            uint32_t v = values[i] * 0x9e3779b1u;
            v          = bits == 1 ? values[i] & 1u : bits == 4 ? (values[i] * 0x30u) & 0xf0u : bits == 8 ? v & 0xffu : bits == 16 ? v & 0xffffu : bits == 24 ? v & 0xffffffu : v;
            dgx_set_pixel(scr, i % PW, i / PW, v);
            values[i] = dgx_get_pixel(scr, i % PW, i / PW);
        }
        const ref_picture_t p = {values, PW, PH};
        dgx_scale_source_t  source = {.screen = scr};
        if (bits >= 8 && rnd(0, 3)) {
            source.memory = ((dgx_vscreen_t *)scr)->v_array;
            source.bytes  = (uint8_t)(bits / 8u);
            source.pitch  = (uint32_t)PW * source.bytes;
        }
        const int      n = rnd(1, 5), kind = rnd(0, 2), smooth = smooth_kinds[kind];
        const bool     along_y = rnd(0, 1);
        const int      run = along_y ? n * PH : n * PW, lines_n = along_y ? n * PW : n * PH;
        const int32_t  from = rnd(0, run - 1);
        const uint32_t count = (uint32_t)rnd(1, run - from);
        uint8_t       *scratch = malloc(dgx_scale_scratch((uint8_t)n, bus_smooth[kind], count, 4));
        dgx_scale_lines_t lines;
        dgx_scale_values_begin(&lines, &source, (uint8_t)n, bus_smooth[kind], along_y, from, count, scratch);
        for (int i = 0; i < lines_n; ++i) {
            const int32_t   at = rnd(0, 1) ? i : rnd(0, lines_n - 1);
            const uint32_t *line = dgx_scale_values(&lines, at);
            int             wrong = 0;
            for (uint32_t k = 0; k < count; ++k) wrong += line[k] != expected_point(&p, n, smooth, along_y, at, from + (int32_t)k);
            if (wrong) {
                fprintf(stderr, "values: %d bits%s, scale %d smooth %d along %s, line %d: %d wrong\n", bits, source.memory ? " by memory" : "", n, smooth,
                        along_y ? "y" : "x", at, wrong);
                ++check_failures;
                break;
            }
            ++checked;
        }
        free(scratch);
        dgx_screen_destroy(&scr);
    }
    CHECK(checked > 10000);
}

int main(void)
{
    test_reference_against_engine();
    test_reference_turns();
    test_indices();
    test_values();
    CHECK_DONE();
}
