/*
 * The calls that only wrote a refusal into the log before DGX 0.12 and now
 * return it as well: the transfers of a virtual screen, dgx_vscreen_array_set(),
 * the textures and the glow of a morph. ESP_OK for what is done, also when
 * nothing of it falls on the screen; the error of the log for what is refused,
 * with nothing drawn.
 */
#include <stdint.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_morph_render.h"
#include "dgx_vscreen.h"

static uint16_t lut[256];

static void test_transfers(void)
{
    dgx_screen_t *dst = dgx_vscreen_init(20, 10, 16, DgxScreenRGB), *src = dgx_vscreen_init(5, 4, 16, DgxScreenRGB);
    dgx_screen_t *src8 = dgx_vscreen_init(5, 4, 8, DgxScreenRGB), *dst8 = dgx_vscreen_init(20, 10, 8, DgxScreenRGB);
    CHECK(dst && src && src8 && dst8);
    dgx_fill_rectangle(src, 0, 0, 5, 4, 0xf800);
    CHECK(dgx_vscreen_to_screen(dst, 2, 3, src, 0) == ESP_OK);
    CHECK(dgx_get_pixel(dst, 2, 3) == 0xf800);
    CHECK(dgx_vscreen_region_to_screen(dst, 0, 0, src, 1, 1, 2, 2, DGX_VSCREEN_SCALE(2)) == ESP_OK);
    /* nothing of it on the screen: done, with nothing to do */
    CHECK(dgx_vscreen_to_screen(dst, 100, 100, src, 0) == ESP_OK);
    /* another depth: refused, nothing drawn */
    dgx_fill_rectangle(dst, 0, 0, 20, 10, 0);
    CHECK(dgx_vscreen_to_screen(dst, 0, 0, src8, 0) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_vscreen_region_to_screen(dst8, 0, 0, src, 0, 0, 5, 4, 0) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_get_pixel(dst, 0, 0) == 0);
    /* through a table: 8 bits onto 16, and nothing else */
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)(i * 257);
    CHECK(dgx_vscreen8_to_screen16(dst, 0, 0, src8, lut, 0) == ESP_OK);
    CHECK(dgx_vscreen8_region_to_screen16(dst, 0, 0, src8, 0, 0, 2, 2, lut, DGX_VSCREEN_SCALE(3)) == ESP_OK);
    CHECK(dgx_vscreen8_to_screen16(dst8, 0, 0, src8, lut, 0) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_vscreen8_region_to_screen16(dst, 0, 0, src, 0, 0, 2, 2, lut, 0) == ESP_ERR_INVALID_ARG);
    dgx_screen_destroy(&src), dgx_screen_destroy(&src8), dgx_screen_destroy(&dst), dgx_screen_destroy(&dst8);
}

static void test_array_set(void)
{
    static uint16_t a[4 * 3], b[6 * 2];
    dgx_screen_t   *over = dgx_vscreen_from_array(a, 4, 3, 16, DgxScreenRGB), *own = dgx_vscreen_init(4, 3, 16, DgxScreenRGB);
    CHECK(over && own);
    CHECK(dgx_vscreen_array_set(over, b, 6, 2) == ESP_OK && over->width == 6 && over->height == 2);
    /* a screen with pixels of its own, no pixels, a size of none: refused, the screen as it was */
    CHECK(dgx_vscreen_array_set(own, b, 6, 2) == ESP_ERR_INVALID_ARG && own->width == 4);
    CHECK(dgx_vscreen_array_set(over, NULL, 6, 2) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_vscreen_array_set(over, a, 0, 3) == ESP_ERR_INVALID_ARG && over->width == 6);
    dgx_screen_destroy(&over), dgx_screen_destroy(&own);
}

static void test_textures(void)
{
    dgx_screen_t *scr = dgx_vscreen_init(20, 20, 16, DgxScreenRGB), *tex = dgx_vscreen_init(8, 8, 16, DgxScreenRGB);
    dgx_screen_t *tex8 = dgx_vscreen_init(8, 8, 8, DgxScreenRGB);
    CHECK(scr && tex && tex8);
    dgx_fill_rectangle(tex, 0, 0, 8, 8, 0x07e0);
    CHECK(dgx_draw_texture_rect(scr, 2, 2, 10, 10, tex, 0, 0, 8, 8) == ESP_OK);
    CHECK(dgx_get_pixel(scr, 5, 5) == 0x07e0);
    const dgx_point_2d_t quad[4] = {{0, 0}, {15, 2}, {13, 15}, {1, 12}};
    CHECK(dgx_draw_texture_quad(scr, quad, tex, 1, 1, 6, 6) == ESP_OK);
    /* nothing of it on the screen, and a region of no pixels: done */
    CHECK(dgx_draw_texture_rect(scr, 50, 50, 10, 10, tex, 0, 0, 8, 8) == ESP_OK);
    CHECK(dgx_draw_texture_rect(scr, 0, 0, 10, 10, tex, 0, 0, 0, 8) == ESP_OK);
    /* a texture of another depth, a region outside the texture, no texture */
    CHECK(dgx_draw_texture_rect(scr, 0, 0, 10, 10, tex8, 0, 0, 8, 8) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_draw_texture_rect(scr, 0, 0, 10, 10, tex, 4, 4, 8, 8) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_draw_texture_quad(scr, quad, NULL, 0, 0, 8, 8) == ESP_ERR_INVALID_ARG);
    dgx_screen_destroy(&scr), dgx_screen_destroy(&tex), dgx_screen_destroy(&tex8);
}

static void test_glow(void)
{
    dgx_morph_glow_t *glow = dgx_morph_glow_create(8, 8, 2, 16);
    dgx_screen_t     *s16 = dgx_vscreen_init(8, 8, 16, DgxScreenRGB), *s24 = dgx_vscreen_init(8, 8, 24, DgxScreenRGB);
    CHECK(glow && s16 && s24);
    const dgx_point_2d_t at = {2, 2};
    dgx_morph_glow_dot(glow, &at, 255);
    CHECK(dgx_morph_glow_present(glow, 1.0f, s16, 0, 0) == ESP_OK);
    CHECK(dgx_morph_glow_present(glow, 1.0f, NULL, 0, 0) == ESP_OK);
    CHECK(dgx_morph_glow_present(glow, 1.0f, s24, 0, 0) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_morph_glow_present(NULL, 1.0f, s16, 0, 0) == ESP_ERR_INVALID_ARG);
    dgx_morph_glow_destroy(&glow);
    dgx_screen_destroy(&s16), dgx_screen_destroy(&s24);
}

int main(void)
{
    test_transfers();
    test_array_set();
    test_textures();
    test_glow();
    CHECK_DONE();
}
