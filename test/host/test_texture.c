#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

#define TEX_W 8
#define TEX_H 6

static int calls;

static void record(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
    ++calls;
}

/* texel (x, y) holds x + 16 * y + 1, never 0 */
static uint32_t texel(int x, int y)
{
    return (uint32_t)(x + 16 * y + 1);
}

static dgx_screen_t *make_texture(uint8_t bits)
{
    dgx_screen_t *t = dgx_vscreen_init(TEX_W, TEX_H, bits, DgxScreenRGB);
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x) dgx_set_pixel(t, x, y, texel(x, y));
    return t;
}

static int count_set(dgx_screen_t *s)
{
    int n = 0;
    for (int y = 0; y < s->height; ++y)
        for (int x = 0; x < s->width; ++x) n += s->get_pixel(s, x, y) != 0;
    return n;
}

/* The 8-bit path goes through get_pixel, the 16-bit one reads the buffer directly. */
static void test_copy_and_scale(uint8_t bits)
{
    dgx_screen_t *tex = make_texture(bits);
    dgx_screen_t *dst = dgx_vscreen_init(40, 30, bits, DgxScreenRGB);
    dst->update_screen = record;

    /* same size: an exact copy of the region, nothing around it */
    calls = 0;
    dgx_draw_texture_rect(dst, 5, 4, 6, 4, tex, 1, 2, 6, 4);
    CHECK(calls == 1);
    for (int y = 0; y < dst->height; ++y) {
        for (int x = 0; x < dst->width; ++x) {
            bool     inside = x >= 5 && x < 11 && y >= 4 && y < 8;
            uint32_t want   = inside ? texel(x - 5 + 1, y - 4 + 2) : 0;
            CHECK(dst->get_pixel(dst, x, y) == want);
        }
    }

    /* three times larger: every texel becomes a 3x3 block */
    dgx_fill_rectangle(dst, 0, 0, 40, 30, 0);
    dgx_draw_texture_rect(dst, 2, 3, TEX_W * 3, TEX_H * 3, tex, 0, 0, TEX_W, TEX_H);
    CHECK(count_set(dst) == TEX_W * 3 * TEX_H * 3);
    CHECK(dst->get_pixel(dst, 2, 3) == texel(0, 0));
    CHECK(dst->get_pixel(dst, 2 + TEX_W * 3 - 1, 3) == texel(TEX_W - 1, 0));
    CHECK(dst->get_pixel(dst, 2, 3 + TEX_H * 3 - 1) == texel(0, TEX_H - 1));
    CHECK(dst->get_pixel(dst, 2 + TEX_W * 3 - 1, 3 + TEX_H * 3 - 1) == texel(TEX_W - 1, TEX_H - 1));
    for (int y = 0; y < TEX_H * 3; ++y) {
        for (int x = 0; x < TEX_W * 3; ++x) {
            CHECK(dst->get_pixel(dst, 2 + x, 3 + y) == texel(x / 3, y / 3));
        }
    }

    /* mirrored: quad vertices in the opposite horizontal order */
    dgx_fill_rectangle(dst, 0, 0, 40, 30, 0);
    const dgx_point_2d_t mirrored[4] = {{12, 4}, {5, 4}, {5, 9}, {12, 9}};
    dgx_draw_texture_quad(dst, mirrored, tex, 0, 0, TEX_W, TEX_H);
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x) CHECK(dst->get_pixel(dst, 12 - x, 4 + y) == texel(x, y));

    /* upside down */
    dgx_fill_rectangle(dst, 0, 0, 40, 30, 0);
    const dgx_point_2d_t flipped[4] = {{5, 9}, {12, 9}, {12, 4}, {5, 4}};
    dgx_draw_texture_quad(dst, flipped, tex, 0, 0, TEX_W, TEX_H);
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x) CHECK(dst->get_pixel(dst, 5 + x, 9 - y) == texel(x, y));

    dgx_screen_destroy(&dst);
    dgx_screen_destroy(&tex);
}

static void test_shapes_and_clipping(void)
{
    dgx_screen_t *tex = make_texture(16);
    dgx_screen_t *dst = dgx_vscreen_init(40, 30, 16, DgxScreenRGB);

    /* trapezoid: wide top, narrow bottom; stays inside its outline and has no holes in a row */
    const dgx_point_2d_t trapezoid[4] = {{4, 5}, {35, 5}, {25, 24}, {14, 24}};
    dgx_draw_texture_quad(dst, trapezoid, tex, 0, 0, TEX_W, TEX_H);
    for (int y = 0; y < dst->height; ++y) {
        int first = -1, last = -1, n = 0;
        for (int x = 0; x < dst->width; ++x) {
            if (!dst->get_pixel(dst, x, y)) continue;
            if (first < 0) first = x;
            last = x;
            ++n;
        }
        if (y < 5 || y > 24) {
            CHECK(n == 0);
        } else {
            CHECK(n > 0 && n == last - first + 1);
            CHECK(first >= 4 && last <= 35);
        }
    }
    CHECK(dst->get_pixel(dst, 4, 5) == texel(0, 0) && dst->get_pixel(dst, 35, 5) == texel(TEX_W - 1, 0));
    CHECK(dst->get_pixel(dst, 14, 24) == texel(0, TEX_H - 1) && dst->get_pixel(dst, 25, 24) == texel(TEX_W - 1, TEX_H - 1));

    /* rotated by 45 degrees: a diamond with a single top vertex */
    dgx_fill_rectangle(dst, 0, 0, 40, 30, 0);
    const dgx_point_2d_t diamond[4] = {{20, 2}, {32, 14}, {20, 26}, {8, 14}};
    dgx_draw_texture_quad(dst, diamond, tex, 0, 0, TEX_W, TEX_H);
    CHECK(dst->get_pixel(dst, 20, 2) == texel(0, 0));
    CHECK(dst->get_pixel(dst, 32, 14) == texel(TEX_W - 1, 0));
    CHECK(dst->get_pixel(dst, 20, 26) == texel(TEX_W - 1, TEX_H - 1));
    CHECK(dst->get_pixel(dst, 8, 14) == texel(0, TEX_H - 1));
    CHECK(dst->get_pixel(dst, 20, 14) != 0 && dst->get_pixel(dst, 8, 2) == 0);

    /* hanging over every edge: the visible part matches the unclipped picture (ASan guards the rest) */
    dgx_screen_t *big = dgx_vscreen_init(120, 110, 16, DgxScreenRGB);
    const dgx_point_2d_t over[4]    = {{-20, -15}, {70, -25}, {60, 50}, {-10, 45}};
    const dgx_point_2d_t shifted[4] = {{20, 25}, {110, 15}, {100, 90}, {30, 85}};
    dgx_fill_rectangle(dst, 0, 0, 40, 30, 0);
    dgx_draw_texture_quad(dst, over, tex, 0, 0, TEX_W, TEX_H);
    dgx_draw_texture_quad(big, shifted, tex, 0, 0, TEX_W, TEX_H);
    for (int y = 0; y < dst->height; ++y)
        for (int x = 0; x < dst->width; ++x) CHECK(dst->get_pixel(dst, x, y) == big->get_pixel(big, x + 40, y + 40));
    dgx_screen_destroy(&big);

    /* fully outside, degenerate and invalid input draw nothing */
    dgx_fill_rectangle(dst, 0, 0, 40, 30, 0);
    const dgx_point_2d_t outside[4] = {{50, 5}, {60, 5}, {60, 15}, {50, 15}};
    dgx_draw_texture_quad(dst, outside, tex, 0, 0, TEX_W, TEX_H);
    dgx_draw_texture_rect(dst, 5, 5, 0, 4, tex, 0, 0, TEX_W, TEX_H);
    dgx_draw_texture_rect(dst, 5, 5, 4, 4, tex, 0, 0, 0, TEX_H);
    dgx_draw_texture_rect(dst, 5, 5, 4, 4, tex, 4, 0, TEX_W, TEX_H);
    dgx_draw_texture_rect(dst, 5, 5, 4, 4, tex, -1, 0, 4, 4);
    /* wider than 16.16 fixed point can step over in one scanline: refused, not drawn wrong */
    const dgx_point_2d_t huge[4] = {{-30000, 5}, {30000, 6}, {30000, 15}, {-30000, 15}};
    dgx_draw_texture_quad(dst, huge, tex, 0, 0, TEX_W, TEX_H);
    dgx_screen_t *other = dgx_vscreen_init(4, 4, 8, DgxScreenRGB);
    dgx_draw_texture_rect(dst, 5, 5, 4, 4, other, 0, 0, 4, 4);
    dgx_screen_destroy(&other);
    CHECK(count_set(dst) == 0);
    /* a single point and a single row */
    dgx_draw_texture_rect(dst, 7, 7, 1, 1, tex, 3, 2, 1, 1);
    CHECK(count_set(dst) == 1 && dst->get_pixel(dst, 7, 7) == texel(3, 2));
    dgx_draw_texture_rect(dst, 0, 0, 40, 1, tex, 0, 0, TEX_W, 1);
    CHECK(dst->get_pixel(dst, 0, 0) == texel(0, 0) && dst->get_pixel(dst, 39, 0) == texel(TEX_W - 1, 0));

    dgx_screen_destroy(&dst);
    dgx_screen_destroy(&tex);
}

/* Packed depths: the same picture on a page screen, a 1-bit and a 12-bit virtual screen. */
static void test_packed_depths(void)
{
    dgx_screen_t *tex1 = dgx_vscreen_init(TEX_W, TEX_H, 1, DgxScreenRGB);
    dgx_screen_t *tex8 = dgx_vscreen_init(TEX_W, TEX_H, 8, DgxScreenRGB);
    dgx_screen_t *tex12 = dgx_vscreen_init(TEX_W, TEX_H, 12, DgxScreenRGB);
    for (int y = 0; y < TEX_H; ++y) {
        for (int x = 0; x < TEX_W; ++x) {
            bool on = (x ^ y) & 1 || x == 3;
            dgx_set_pixel(tex1, x, y, on);
            dgx_set_pixel(tex8, x, y, on);
            dgx_set_pixel(tex12, x, y, on ? DGX_RGB_12(0xf0, 0x80, 0x10) : 0);
        }
    }
    static const dgx_point_2d_t quads[][4] = {
        {{3, 2}, {27, 4}, {22, 21}, {6, 17}},
        {{-4, -3}, {35, 1}, {30, 26}, {2, 20}},
        {{5, 5}, {11, 5}, {11, 9}, {5, 9}},
    };
    for (size_t q = 0; q < sizeof(quads) / sizeof(quads[0]); ++q) {
        dgx_screen_t *ref = dgx_vscreen_init(31, 24, 8, DgxScreenRGB);
        dgx_screen_t *lin = dgx_vscreen_init(31, 24, 1, DgxScreenRGB);
        dgx_screen_t *bw  = dgx_bw_init(31, 24);
        dgx_screen_t *d12 = dgx_vscreen_init(31, 24, 12, DgxScreenRGB);
        dgx_draw_texture_quad(ref, quads[q], tex8, 0, 0, TEX_W, TEX_H);
        dgx_draw_texture_quad(lin, quads[q], tex1, 0, 0, TEX_W, TEX_H);
        dgx_draw_texture_quad(bw, quads[q], tex1, 0, 0, TEX_W, TEX_H);
        dgx_draw_texture_quad(d12, quads[q], tex12, 0, 0, TEX_W, TEX_H);
        CHECK(count_set(ref) > 10);
        for (int y = 0; y < ref->height; ++y) {
            for (int x = 0; x < ref->width; ++x) {
                bool on = ref->get_pixel(ref, x, y) != 0;
                CHECK(on == (lin->get_pixel(lin, x, y) != 0));
                CHECK(on == (bw->get_pixel(bw, x, y) != 0));
                CHECK(d12->get_pixel(d12, x, y) == (on ? DGX_RGB_12(0xf0, 0x80, 0x10) : 0));
            }
        }
        dgx_screen_destroy(&ref);
        dgx_screen_destroy(&lin);
        dgx_screen_destroy(&bw);
        dgx_screen_destroy(&d12);
    }
    dgx_screen_destroy(&tex1);
    dgx_screen_destroy(&tex8);
    dgx_screen_destroy(&tex12);
}

int main(void)
{
    test_copy_and_scale(8);
    test_copy_and_scale(16);
    test_shapes_and_clipping();
    test_packed_depths();
    CHECK_DONE();
}
