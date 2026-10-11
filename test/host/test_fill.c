#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

/*
 * A rectangle filled by a virtual screen has to be what the same pixels
 * set one by one are: every depth, rows and columns of one pixel (they go a
 * short way of their own), rectangles cut by the edges or off the screen,
 * sizes of zero and below. And the area reported as changed is the part of
 * the rectangle that is on the screen.
 */

#define W 37
#define H 23

static uint32_t seed = 4711;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

static int calls, last_l, last_r, last_t, last_b;

static void record(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr;
    ++calls;
    last_l = left, last_r = right, last_t = top, last_b = bottom;
}

static void quiet(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

static void one(dgx_screen_t *filled, dgx_screen_t *model, int x, int y, int w, int h, uint32_t color)
{
    calls = 0;
    dgx_fill_rectangle(filled, x, y, w, h, color);
    int l = x < 0 ? 0 : x, t = y < 0 ? 0 : y, r = x + w - 1 >= W ? W - 1 : x + w - 1, b = y + h - 1 >= H ? H - 1 : y + h - 1;
    bool on_screen = w > 0 && h > 0 && l <= r && t <= b;
    if (on_screen) {
        for (int j = t; j <= b; ++j) {
            for (int i = l; i <= r; ++i) dgx_set_pixel(model, i, j, color);
        }
        CHECK(calls == 1 && last_l == l && last_r == r && last_t == t && last_b == b);
    } else {
        CHECK(calls == 0);
    }
    int differ = 0;
    for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) differ += dgx_get_pixel(filled, i, j) != dgx_get_pixel(model, i, j);
    }
    if (differ) {
        fprintf(stderr, "%d bits: rectangle %dx%d at (%d, %d) differs in %d pixels\n", filled->color_bits, w, h, x, y, differ);
        ++check_failures;
    }
}

static void screen(dgx_screen_t *filled, dgx_screen_t *model)
{
    filled->update_screen = record;
    model->update_screen  = quiet;
    uint32_t mask = filled->color_bits >= 24 ? 0xffffffu : filled->color_bits == 1 ? 1u : (1u << filled->color_bits) - 1u;
    if (filled->color_bits == 12) mask = 0xfff0u; /* a 12-bit color sits in the high bits of 16 */
    if (filled->color_bits == 18) mask = 0xfcfcfcu;
    for (int round = 0; round < 1500; ++round) {
        uint32_t color = ((uint32_t)rnd(0, 0xffff) << 8 | (uint32_t)rnd(0, 0xff)) & mask;
        int      kind = rnd(0, 5);
        int      x = rnd(-6, W + 4), y = rnd(-6, H + 4);
        int      w = kind == 0 ? 1 : rnd(-2, W + 8), h = kind == 1 ? 1 : rnd(-2, H + 8);
        if (kind == 2) w = 1, h = 1;
        one(filled, model, x, y, w, h, color);
    }
    /* the whole screen, and the whole screen cleared */
    one(filled, model, 0, 0, W, H, 0x00a5a5a5u & mask);
    one(filled, model, 0, 0, W, H, 0);
    one(filled, model, -3, -3, W + 6, 1, mask);
    one(filled, model, 5, -3, 1, H + 6, mask);
}

int main(void)
{
    static const uint8_t depths[] = {1, 8, 12, 16, 18, 24};
    for (size_t d = 0; d < sizeof(depths); ++d) {
        dgx_screen_t *filled = dgx_vscreen_init(W, H, depths[d], DgxScreenRGB), *model = dgx_vscreen_init(W, H, depths[d], DgxScreenRGB);
        screen(filled, model);
        /* byte for byte, not only as get_pixel() sees it */
        CHECK(memcmp(((dgx_vscreen_t *)filled)->v_array, ((dgx_vscreen_t *)model)->v_array, dgx_color_points_to_bytes(depths[d], W * H)) == 0);
        dgx_screen_destroy(&filled);
        dgx_screen_destroy(&model);
    }
    /* the monochrome paged screen */
    dgx_screen_t *filled = dgx_bw_init(W, H), *model = dgx_bw_init(W, H);
    screen(filled, model);
    dgx_screen_destroy(&filled);
    dgx_screen_destroy(&model);
    CHECK_DONE();
}
