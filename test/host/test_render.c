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
    test_sprite();
    test_vscreen_clear();
    CHECK_DONE();
}
