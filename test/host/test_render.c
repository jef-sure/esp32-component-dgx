#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_morph_render.h"
#include "dgx_screen_with_bus.h"
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

/* the same blur through a function of another name: the renderer does not know it and gives it a copy of the frame */
static void blur_on_a_copy(void *user_data, uint8_t *brightness, int width, int height)
{
    dgx_morph_glow_blur(user_data, brightness, width, height);
}

/* a display behind a bus: it takes an area and then the bytes of its pixels, row by row */
#define PANEL_W 24
#define PANEL_H 18
static uint8_t panel[PANEL_W * PANEL_H * 3];
static int     panel_bytes, panel_l, panel_r, panel_b, panel_x, panel_y, panel_sent;

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    CHECK(left <= right && top <= bottom && right < PANEL_W && bottom < PANEL_H);
    panel_l = left, panel_r = right, panel_b = bottom, panel_x = left, panel_y = top;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    (void)scr;
    CHECK(lenbits % (8u * (uint32_t)panel_bytes) == 0);
    for (uint32_t i = 0; i + panel_bytes <= lenbits / 8; i += (uint32_t)panel_bytes) {
        CHECK(panel_y <= panel_b);
        if (panel_y <= panel_b) memcpy(panel + (panel_x + panel_y * PANEL_W) * 3, data + i, (size_t)panel_bytes);
        ++panel_sent;
        if (++panel_x > panel_r) panel_x = panel_l, ++panel_y;
    }
}

static void panel_nothing(dgx_screen_t *scr)
{
    (void)scr;
}

static void panel_update(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

static uint32_t render_seed = 2026;

static int render_rnd(int lo, int hi)
{
    render_seed = render_seed * 1664525u + 1013904223u;
    return lo + (int)((render_seed >> 8) % (uint32_t)(hi - lo + 1));
}

/*
 * A glow keeps no frame of colors: brightness becomes colors on the way to
 * the screen, through the buffer a display sends from, and the ready blur in
 * one pass is made row by row there. What is shown must not depend on any of
 * that: not on the size of the buffer, not on the kind of the screen, not on
 * the blur having a copy of the frame or not.
 */
static void test_glow_rows(void)
{
    static const int sizes[][3] = { { 24, 18, 4 }, { 13, 9, 2 }, { 1, 1, 1 }, { 2, 7, 1 }, { 17, 1, 3 } };
    static const int places[][2] = { { 0, 0 }, { -3, -2 }, { 15, 12 }, { 40, 3 }, { 5, -30 } };
    static const int buffers[] = { 64, 37, 6, 3 };
    static uint8_t   bus_buffer[64];
    static const int one = 1;
    for (int round = 0; round < 300; ++round) {
        int bits = round % 3 == 0 ? 16 : round % 3 == 1 ? 18 : 24, size = render_rnd(0, 4), place = render_rnd(0, 4);
        int w = sizes[size][0], h = sizes[size][1];
        panel_bytes = bits == 16 ? 2 : 3;
        dgx_bus_protocols_t   bus = { .buffer = bus_buffer, .buffer_len = (uint32_t)buffers[render_rnd(0, 3)] };
        dgx_screen_with_bus_t sbus = { .bus = &bus };
        dgx_screen_t         *lcd = &sbus.scr;
        lcd->width = PANEL_W, lcd->height = PANEL_H, lcd->color_bits = (uint8_t)bits, lcd->screen_subtype = DgxPhysicalScreenWithBus;
        lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_nothing, lcd->update_screen = panel_update;
        lcd->draw_buffer = bus.buffer, lcd->draw_buffer_len = bus.buffer_len;
        dgx_screen_t *rows = dgx_vscreen_init(PANEL_W, PANEL_H, (uint8_t)bits, DgxScreenRGB), *copy = dgx_vscreen_init(PANEL_W, PANEL_H, (uint8_t)bits, DgxScreenRGB);
        dgx_morph_glow_t *by_rows = dgx_morph_glow_create(w, h, sizes[size][2], (uint8_t)bits), *by_copy = dgx_morph_glow_create(w, h, sizes[size][2], (uint8_t)bits);
        dgx_morph_glow_t *to_panel = dgx_morph_glow_create(w, h, sizes[size][2], (uint8_t)bits);
        CHECK(rows && copy && by_rows && by_copy && to_panel);
        bool blurred = round % 4 != 3;
        if (blurred) {
            CHECK(dgx_morph_glow_set_filter(by_rows, dgx_morph_glow_blur, round & 1 ? (void *)&one : NULL));
            CHECK(dgx_morph_glow_set_filter(by_copy, blur_on_a_copy, NULL));
            CHECK(dgx_morph_glow_set_filter(to_panel, dgx_morph_glow_blur, NULL));
        }
        memset(panel, 0, sizeof(panel));
        for (int frame = 0; frame < 4; ++frame) {
            for (int dots = render_rnd(1, 8); dots > 0; --dots) {
                dgx_point_2d_t at = { .x = (int16_t)render_rnd(-2, w + 1), .y = (int16_t)render_rnd(-2, h + 1) };
                uint8_t        intensity = (uint8_t)render_rnd(1, 255);
                dgx_morph_glow_dot(by_rows, &at, intensity), dgx_morph_glow_dot(by_copy, &at, intensity), dgx_morph_glow_dot(to_panel, &at, intensity);
            }
            float t = (float)render_rnd(0, 4) / 4;
            panel_sent = 0;
            dgx_morph_glow_present(by_rows, t, rows, places[place][0], places[place][1]);
            dgx_morph_glow_present(by_copy, t, copy, places[place][0], places[place][1]);
            dgx_morph_glow_present(to_panel, t, lcd, places[place][0], places[place][1]);
            int differ = 0, lit = 0, inside = 0;
            for (int y = 0; y < PANEL_H; ++y) {
                for (int x = 0; x < PANEL_W; ++x) {
                    uint32_t shown = dgx_get_pixel(rows, x, y);
                    const uint8_t *sent = panel + (x + y * PANEL_W) * 3;
                    uint32_t on_panel = bits == 16 ? (uint32_t)(sent[0] << 8 | sent[1]) : (uint32_t)(sent[0] << 16 | sent[1] << 8 | sent[2]);
                    differ += shown != dgx_get_pixel(copy, x, y) || shown != on_panel;
                    lit += shown != 0;
                    inside += x >= places[place][0] && x < places[place][0] + w && y >= places[place][1] && y < places[place][1] + h;
                }
            }
            CHECK(differ == 0);
            /* only the part of the frame that is on the display is sent, and nothing lights up beside it */
            CHECK(panel_sent == inside && lit <= inside);
        }
        dgx_morph_glow_destroy(&by_rows), dgx_morph_glow_destroy(&by_copy), dgx_morph_glow_destroy(&to_panel);
        dgx_screen_destroy(&rows), dgx_screen_destroy(&copy);
    }
    /* a glow of other colors than the screen shows nothing */
    dgx_morph_glow_t *glow = dgx_morph_glow_create(4, 4, 1, 24);
    dgx_screen_t     *target = dgx_vscreen_init(4, 4, 16, DgxScreenRGB);
    dgx_morph_glow_dot(glow, &(dgx_point_2d_t){ .x = 1, .y = 1 }, 255);
    dgx_morph_glow_present(glow, 1.0f, target, 0, 0);
    CHECK(dgx_get_pixel(target, 1, 1) == 0);
    dgx_morph_glow_present(glow, 1.0f, NULL, 0, 0);
    dgx_screen_destroy(&target);
    dgx_morph_glow_destroy(&glow);
}

int main(void)
{
    test_glow();
    test_glow_blend();
    test_glow_outside();
    test_glow_filter();
    test_glow_blur();
    test_glow_rows();
    test_sprite();
    test_vscreen_clear();
    CHECK_DONE();
}
