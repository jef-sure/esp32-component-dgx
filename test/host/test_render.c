#include <stdint.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_morph_render.h"
#include "drivers/vscreen.h"

static int lit_radius_x(int cell)
{
    int size = 41, c = size / 2;
    dgx_morph_glow_t *glow = dgx_morph_glow_create(size, size, cell, 24);
    dgx_screen_t *target = dgx_vscreen_init(size, size, 24, DgxScreenRGB);
    CHECK(glow && target);
    dgx_morph_glow_dot(glow, &(dgx_point_2d_t){ .x = (int16_t)c, .y = (int16_t)c }, 255);
    dgx_morph_glow_present(glow, 1.0f, target, 0, 0);
    CHECK(dgx_get_pixel(target, c, c) == DGX_RGB_24(255, 255, 255));
    int r = 0;
    while (c + r + 1 < size && dgx_get_pixel(target, c + r + 1, c) != 0) ++r;
    dgx_screen_destroy(&target);
    dgx_morph_glow_destroy(&glow);
    return r;
}

static void test_glow(void)
{
    CHECK(dgx_morph_glow_create(10, 10, 3, 8) == NULL);
    CHECK(dgx_morph_glow_create(0, 10, 3, 16) == NULL);
    /* radius max(cell/2 + cell/4, 2); the disk is d^2 < R^2 */
    CHECK(lit_radius_x(1) == 1);
    CHECK(lit_radius_x(3) == 1);
    CHECK(lit_radius_x(8) == 5);
}

static void test_glow_blend(void)
{
    dgx_morph_glow_t *glow = dgx_morph_glow_create(5, 5, 1, 16);
    dgx_screen_t *target = dgx_vscreen_init(5, 5, 16, DgxScreenRGB);
    dgx_morph_glow_dot(glow, &(dgx_point_2d_t){ .x = 2, .y = 2 }, 255);
    dgx_morph_glow_present(glow, 0.0f, target, 0, 0);
    CHECK(dgx_get_pixel(target, 2, 2) == DGX_RGB_16(255, 255, 255));
    /* next frame at t = 0 keeps the previous one */
    dgx_morph_glow_present(glow, 0.0f, target, 0, 0);
    CHECK(dgx_get_pixel(target, 2, 2) == DGX_RGB_16(255, 255, 255));
    dgx_morph_glow_present(glow, 1.0f, target, 0, 0);
    CHECK(dgx_get_pixel(target, 2, 2) == 0);
    dgx_screen_destroy(&target);
    dgx_morph_glow_destroy(&glow);
}

static int filter_calls, filter_width, filter_height;

static void filter_black(void *user_data, uint8_t *brightness, int width, int height)
{
    ++filter_calls;
    filter_width = width, filter_height = height;
    if (user_data) *(int *)user_data = brightness[2 * width + 2];
    for (int i = 0; i < width * height; ++i) brightness[i] = 0;
}

/* a filter gets a copy of the finished frame: what it does is shown and does not get into the glow */
static void test_glow_filter(void)
{
    for (int bits = 16; bits <= 24; bits += 8) {
        dgx_morph_glow_t *glow = dgx_morph_glow_create(5, 5, 1, (uint8_t)bits);
        dgx_screen_t     *target = dgx_vscreen_init(5, 5, (uint8_t)bits, DgxScreenRGB);
        uint32_t          white = bits == 16 ? DGX_RGB_16(255, 255, 255) : DGX_RGB_24(255, 255, 255);
        int               seen = -1;
        filter_calls = 0;
        CHECK(dgx_morph_glow_set_filter(glow, filter_black, &seen));
        dgx_morph_glow_dot(glow, &(dgx_point_2d_t){ .x = 2, .y = 2 }, 255);
        dgx_morph_glow_present(glow, 1.0f, target, 0, 0);
        /* called once with the whole frame, the dot was in it, and the black it left is what is shown */
        CHECK(filter_calls == 1 && filter_width == 5 && filter_height == 5 && seen == 255);
        CHECK(dgx_get_pixel(target, 2, 2) == 0);
        /* the glow still has the dot: without the filter the frame kept by the phosphor is shown */
        CHECK(dgx_morph_glow_set_filter(glow, NULL, NULL));
        dgx_morph_glow_present(glow, 0.0f, target, 0, 0);
        CHECK(filter_calls == 1);
        CHECK(dgx_get_pixel(target, 2, 2) == white);
        dgx_screen_destroy(&target);
        dgx_morph_glow_destroy(&glow);
    }
    CHECK(!dgx_morph_glow_set_filter(NULL, filter_black, NULL));
}

/* the ready blur spreads a dot over its neighbours and takes nothing from the next frame */
static void test_glow_blur(void)
{
    uint8_t map[7 * 7] = {0};
    map[3 * 7 + 3] = 200;
    dgx_morph_glow_blur(NULL, map, 7, 7);
    CHECK(map[3 * 7 + 3] == 50 && map[3 * 7 + 2] == 25 && map[2 * 7 + 3] == 25 && map[2 * 7 + 2] == 13);
    CHECK(map[3 * 7 + 5] == 0 && map[0] == 0);
    int sum = 0;
    for (int i = 0; i < 49; ++i) sum += map[i];
    CHECK(sum >= 195 && sum <= 205);
    /* more passes spread it further; an even map stays even, edges too */
    static const int passes = 3;
    uint8_t          wide[7 * 7] = {0}, even[4 * 3];
    wide[3 * 7 + 3] = 200;
    dgx_morph_glow_blur((void *)&passes, wide, 7, 7);
    CHECK(wide[3 * 7 + 5] > 0 && wide[3 * 7 + 3] < 50);
    for (int i = 0; i < 12; ++i) even[i] = 77;
    dgx_morph_glow_blur((void *)&passes, even, 4, 3);
    for (int i = 0; i < 12; ++i) CHECK(even[i] == 77);
    uint8_t one = 9;
    dgx_morph_glow_blur(NULL, &one, 1, 1);
    CHECK(one == 9);
    dgx_morph_glow_blur(NULL, NULL, 3, 3);

    /* in a renderer: the shown dot is softer, the frames after it are as without the filter */
    dgx_morph_glow_t *plain = dgx_morph_glow_create(9, 9, 1, 16), *soft = dgx_morph_glow_create(9, 9, 1, 16);
    dgx_screen_t     *a = dgx_vscreen_init(9, 9, 16, DgxScreenRGB), *b = dgx_vscreen_init(9, 9, 16, DgxScreenRGB);
    CHECK(dgx_morph_glow_set_filter(soft, dgx_morph_glow_blur, NULL));
    dgx_morph_glow_dot(plain, &(dgx_point_2d_t){ .x = 4, .y = 4 }, 255);
    dgx_morph_glow_dot(soft, &(dgx_point_2d_t){ .x = 4, .y = 4 }, 255);
    dgx_morph_glow_present(plain, 1.0f, a, 0, 0);
    dgx_morph_glow_present(soft, 1.0f, b, 0, 0);
    CHECK(dgx_get_pixel(b, 4, 4) != 0 && dgx_get_pixel(b, 4, 4) != dgx_get_pixel(a, 4, 4));
    CHECK(dgx_get_pixel(b, 4, 7) == 0);
    dgx_morph_glow_set_filter(soft, NULL, NULL);
    dgx_morph_glow_present(plain, 0.0f, a, 0, 0);
    dgx_morph_glow_present(soft, 0.0f, b, 0, 0);
    int differ = 0;
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 9; ++x) differ += dgx_get_pixel(a, x, y) != dgx_get_pixel(b, x, y);
    }
    CHECK(differ == 0);
    dgx_screen_destroy(&a);
    dgx_screen_destroy(&b);
    dgx_morph_glow_destroy(&plain);
    dgx_morph_glow_destroy(&soft);
}

static void test_glow_outside(void)
{
    dgx_morph_glow_t *glow = dgx_morph_glow_create(6, 4, 8, 16);
    const dgx_point_2d_t points[] = { { -3, 0 }, { 0, -3 }, { 8, 2 }, { 2, 6 }, { -100, -100 }, { 100, 100 } };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        dgx_morph_glow_dot(glow, &points[i], 255);
    }
    dgx_morph_glow_present(glow, 1.0f, NULL, 0, 0);
    dgx_morph_glow_destroy(&glow);
}

static void test_sprite(void)
{
    dgx_screen_t *target = dgx_vscreen_init(9, 9, 16, DgxScreenRGB);
    dgx_morph_sprite_t *sprite = dgx_morph_sprite_create(3);
    CHECK(sprite && target);
    dgx_morph_sprite_set_target(sprite, target);
    dgx_morph_sprite_dot(sprite, &(dgx_point_2d_t){ .x = 4, .y = 4 }, 255);
    CHECK(dgx_get_pixel(target, 4, 4) != 0);
    CHECK(dgx_get_pixel(target, 0, 0) == 0);
    dgx_morph_sprite_destroy(&sprite);
    dgx_screen_destroy(&target);
}

static void test_vscreen_clear(void)
{
    dgx_screen_t *s = dgx_vscreen_init(7, 5, 16, DgxScreenRGB);
    dgx_fill_rectangle(s, 0, 0, 7, 5, 0xffff);
    dgx_fill_rectangle(s, 0, 0, 7, 3, 0);
    CHECK(dgx_get_pixel(s, 6, 2) == 0 && dgx_get_pixel(s, 6, 3) == 0xffff);
    /* full-screen black takes the memset path */
    dgx_fill_rectangle(s, 0, 0, 7, 5, 0);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 7; ++x) CHECK(dgx_get_pixel(s, x, y) == 0);
    dgx_screen_destroy(&s);
}

int main(void)
{
    test_glow();
    test_glow_blend();
    test_glow_outside();
    test_glow_filter();
    test_glow_blur();
    test_sprite();
    test_vscreen_clear();
    CHECK_DONE();
}
