/*
 * A virtual screen over memory of the program: it draws as a screen of
 * dgx_vscreen_init() does, leaves the memory to its owner, and can be pointed
 * at other memory, where nothing of the old pixels stays with it.
 *
 * A wrong free(), a write outside the memory and a lost screen are found by
 * the sanitizers the tests run under; so the memory here is as large as it is
 * said to be, with no room to spare.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_overlay.h"
#include "drivers/vscreen.h"

#define W 23
#define H 17

static uint32_t seed = 2654435769u;

static uint32_t next(uint32_t limit)
{
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % limit;
}

static void draw_something(dgx_screen_t *s)
{
    const uint32_t mask = s->color_bits >= 32 ? 0xffffffffu : (1u << s->color_bits) - 1u;
    dgx_fill_rectangle(s, 2, 3, 15, 9, 0x5a5a5a5au & mask);
    dgx_fill_rectangle(s, -4, 12, 40, 1, 0xa5a5a5a5u & mask);
    dgx_fill_rectangle(s, 20, -3, 1, 40, 0x3c3c3c3cu & mask);
    dgx_draw_line(s, 0, 0, W - 1, H - 1, 0xc3c3c3c3u & mask);
    dgx_draw_circle(s, 11, 8, 6, 0x69696969u & mask);
    dgx_set_pixel(s, 1, 15, 0x96969696u & mask);
    dgx_set_pixel(s, W, H, 0xffffffffu & mask);
}

/* the same pixels as a screen with memory of its own, in exactly the memory it was given, wherever that starts */
static void test_draws_as_its_own(uint8_t bits, size_t shift)
{
    const uint32_t size   = dgx_color_points_to_bytes(bits, W * H);
    uint8_t       *block  = calloc(shift + size, 1);
    uint8_t       *memory = block + shift;
    dgx_screen_t  *own    = dgx_vscreen_init(W, H, bits, DgxScreenRGB);
    dgx_screen_t  *over   = dgx_vscreen_from_array(memory, W, H, bits, DgxScreenRGB);
    CHECK(block && own && over);
    if (block && own && over) {
        CHECK(over->width == W && over->height == H && over->color_bits == bits);
        CHECK(over->screen_subtype == DgxVirtualScreen);
        CHECK(dgx_vscreen_is_linear(over));
        CHECK(((dgx_vscreen_t *)over)->v_array == memory);
        draw_something(own);
        draw_something(over);
        CHECK(!memcmp(memory, ((dgx_vscreen_t *)own)->v_array, size));
        CHECK(over->get_pixel(over, 11, 2) == own->get_pixel(own, 11, 2));
    }
    dgx_screen_destroy(&over);
    dgx_screen_destroy(&own);
    free(block);
}

/* memory not from the heap, so that a free() of it stops the test */
static void test_destroy_leaves_the_memory(void)
{
    uint16_t      memory[4 * 3];
    uint16_t      copy[4 * 3];
    dgx_screen_t *s = dgx_vscreen_from_array(memory, 4, 3, 16, DgxScreenRGB);
    CHECK(s != NULL);
    dgx_fill_rectangle(s, 0, 0, 4, 3, 0x1234);
    memcpy(copy, memory, sizeof(memory));
    dgx_screen_destroy(&s);
    CHECK(s == NULL);
    CHECK(!memcmp(copy, memory, sizeof(memory)));
}

static void test_refused_at_the_start(void)
{
    uint8_t memory[4];
    CHECK(dgx_vscreen_from_array(NULL, 2, 2, 8, DgxScreenRGB) == NULL);
    CHECK(dgx_vscreen_from_array(memory, 0, 2, 8, DgxScreenRGB) == NULL);
    CHECK(dgx_vscreen_from_array(memory, 2, 0, 8, DgxScreenRGB) == NULL);
    CHECK(dgx_vscreen_from_array(memory, -2, 2, 8, DgxScreenRGB) == NULL);
    CHECK(dgx_vscreen_from_array(memory, 2, -2, 8, DgxScreenRGB) == NULL);
}

static void test_other_memory(void)
{
    const uint32_t size_a = 2u * 10u * 6u, size_b = 2u * 4u * 3u;
    uint8_t       *a = calloc(size_a, 1), *b = calloc(size_b, 1), *expected = calloc(size_a, 1);
    dgx_screen_t  *s = dgx_vscreen_from_array(a, 10, 6, 16, DgxScreenBGR);
    CHECK(a && b && expected && s);
    if (!a || !b || !expected || !s) goto done;

    dgx_vscreen_array_set(s, b, 4, 3);
    CHECK(((dgx_vscreen_t *)s)->v_array == b);
    CHECK(s->width == 4 && s->height == 3);
    CHECK(s->color_bits == 16 && s->rgb_order == DgxScreenBGR);
    CHECK(dgx_vscreen_is_linear(s));

    /* the whole of the smaller memory, and past it nothing: the sanitizer sees to that */
    dgx_fill_rectangle(s, -2, -2, 20, 20, 0xbeef);
    dgx_set_pixel(s, 4, 0, 0x1111);
    dgx_set_pixel(s, 0, 3, 0x1111);
    CHECK(!memcmp(a, expected, size_a));
    for (uint32_t i = 0; i < size_b; i += 2) CHECK(b[i] == 0xbe && b[i + 1] == 0xef);

    /* and back to a larger one: the first one has all of it to come */
    dgx_vscreen_array_set(s, a, 10, 6);
    CHECK(s->width == 10 && s->height == 6);
    dgx_set_pixel(s, 9, 5, 0x1234);
    CHECK(a[size_a - 2] == 0x12 && a[size_a - 1] == 0x34);
    CHECK(s->get_pixel(s, 9, 5) == 0x1234);
done:
    dgx_screen_destroy(&s);
    free(a);
    free(b);
    free(expected);
}

/*
 * Rows of the memory longer than the picture, the width of the screen being
 * their length, as a renderer gives its bands; every band of another size,
 * where a pixel past the picture must not go out.
 */
static void test_bands_of_another_stride(void)
{
    dgx_screen_t *display = dgx_vscreen_init(40, 30, 16, DgxScreenRGB);
    dgx_screen_t *band    = NULL;
    CHECK(display != NULL);
    for (int round = 0; round < 500 && display; ++round) {
        const int      width = 1 + (int)next(12), height = 1 + (int)next(8), stride = width + (int)next(5);
        const int      x = (int)next(28), y = (int)next(22);
        const size_t   size   = (size_t)stride * (size_t)height * 2u;
        uint8_t       *memory = malloc(size);
        CHECK(memory != NULL);
        if (!memory) break;
        for (size_t i = 0; i < size; ++i) memory[i] = (uint8_t)next(256);
        if (band) dgx_vscreen_array_set(band, memory, stride, height);
        else band = dgx_vscreen_from_array(memory, stride, height, 16, DgxScreenRGB);
        CHECK(band != NULL);
        dgx_fill_rectangle(display, 0, 0, 40, 30, 0);
        dgx_vscreen_region_to_screen(display, x, y, band, 0, 0, width, height, 0u);
        int wrong = 0;
        for (int py = 0; py < 30; ++py) {
            for (int px = 0; px < 40; ++px) {
                uint32_t want = 0;
                if (px >= x && px < x + width && py >= y && py < y + height) {
                    const uint8_t *p = memory + ((size_t)(py - y) * (size_t)stride + (size_t)(px - x)) * 2u;
                    want             = (uint32_t)p[0] << 8 | p[1];
                }
                wrong += display->get_pixel(display, px, py) != want;
            }
        }
        CHECK(wrong == 0);
        free(memory);
    }
    dgx_screen_destroy(&band);
    dgx_screen_destroy(&display);
}

/* the window of the old pixels must not be written through */
static void test_window_starts_anew(void)
{
    static uint8_t large[2 * 40 * 25];
    uint8_t       *small = calloc(2u * 4u * 3u, 1);
    dgx_screen_t  *s     = dgx_vscreen_from_array(large, 40, 25, 16, DgxScreenRGB);
    CHECK(small && s);
    if (small && s) {
        uint8_t pixel[2] = {0xab, 0xcd};
        s->set_area(s, 30, 39, 20, 24);
        s->write_area(s, pixel, 16);
        dgx_vscreen_array_set(s, small, 4, 3);
        s->write_area(s, pixel, 16);
        CHECK(small[0] == 0xab && small[1] == 0xcd);
        s->set_area(s, 1, 3, 2, 2);
        s->write_area(s, pixel, 16);
        CHECK(small[2u * (2u * 4u + 1u)] == 0xab);
    }
    dgx_screen_destroy(&s);
    free(small);
}

/* what was dirty in the old pixels is not to be taken for a change of the new ones */
static void test_nothing_left_dirty(void)
{
    static uint8_t a[20 * 10], b[3 * 2];
    dgx_screen_t  *s = dgx_vscreen_from_array(a, 20, 10, 8, DgxScreenRGB);
    int            left, top, width, height;
    CHECK(s != NULL);
    dgx_screen_progress_up(s);
    dgx_fill_rectangle(s, 5, 5, 10, 4, 7);
    dgx_vscreen_array_set(s, b, 3, 2);
    CHECK(!dgx_screen_take_dirty(s, &left, &top, &width, &height));
    dgx_set_pixel(s, 2, 1, 9);
    CHECK(dgx_screen_take_dirty(s, &left, &top, &width, &height));
    CHECK(left == 2 && top == 1 && width == 1 && height == 1);
    dgx_screen_progress_down(s);
    dgx_screen_destroy(&s);
}

static void test_refused_later(void)
{
    static uint8_t mine[8 * 4], other[4 * 2];
    dgx_screen_t  *own  = dgx_vscreen_init(8, 4, 8, DgxScreenRGB);
    dgx_screen_t  *bw   = dgx_bw_init(16, 8);
    dgx_screen_t  *over = dgx_vscreen_from_array(mine, 8, 4, 8, DgxScreenRGB);
    CHECK(own && bw && over);
    if (!own || !bw || !over) goto done;

    /* its memory would be lost, and that of the program would be given to free() */
    uint8_t *own_array = ((dgx_vscreen_t *)own)->v_array;
    dgx_vscreen_array_set(own, other, 4, 2);
    CHECK(((dgx_vscreen_t *)own)->v_array == own_array);
    CHECK(own->width == 8 && own->height == 4);
    dgx_vscreen_array_set(bw, other, 4, 2);
    CHECK(bw->width == 16 && bw->height == 8);

    dgx_vscreen_array_set(over, NULL, 4, 2);
    dgx_vscreen_array_set(over, other, 0, 2);
    dgx_vscreen_array_set(over, other, 4, 0);
    dgx_vscreen_array_set(over, other, -4, 2);
    dgx_vscreen_array_set(over, other, 4, -2);
    CHECK(((dgx_vscreen_t *)over)->v_array == mine);
    CHECK(over->width == 8 && over->height == 4);
done:
    dgx_screen_destroy(&over);
    dgx_screen_destroy(&bw);
    dgx_screen_destroy(&own);
}

/* an overlay keeps where the pixels of its layer lie, and the size of both */
static void test_overlay_keeps_its_hold(void)
{
    static uint8_t  layer_memory[8 * 4], other[4 * 2];
    static uint8_t  display_memory[2 * 8 * 4], display_other[2 * 4 * 2];
    static uint16_t lut[256];
    dgx_screen_t   *display = dgx_vscreen_from_array(display_memory, 8, 4, 16, DgxScreenRGB);
    dgx_screen_t   *layer   = dgx_vscreen_from_array(layer_memory, 8, 4, 8, DgxScreenRGB);
    CHECK(display && layer);
    if (display && layer) {
        CHECK(dgx_overlay_set(display, layer, 0, 0, lut, 255));
        dgx_vscreen_array_set(layer, other, 4, 2);
        CHECK(((dgx_vscreen_t *)layer)->v_array == layer_memory && layer->width == 8);
        dgx_vscreen_array_set(display, display_other, 4, 2);
        CHECK(((dgx_vscreen_t *)display)->v_array == display_memory && display->width == 8);

        /* taken away, the layer is a screen as any other; the display forgets the place it changed with its old memory */
        dgx_overlay_clear(display);
        dgx_vscreen_array_set(layer, other, 4, 2);
        CHECK(((dgx_vscreen_t *)layer)->v_array == other && layer->width == 4);
        dgx_vscreen_array_set(display, display_other, 4, 2);
        CHECK(((dgx_vscreen_t *)display)->v_array == display_other && display->width == 4);
        int x, y, width, height;
        CHECK(!dgx_overlay_take_dirty(display, &x, &y, &width, &height) && !display->overlay);

        /* and one may be let go of while it lies over a screen */
        dgx_vscreen_array_set(layer, layer_memory, 8, 4);
        dgx_vscreen_array_set(display, display_memory, 8, 4);
        CHECK(dgx_overlay_set(display, layer, 0, 0, lut, 255));
    }
    dgx_screen_destroy(&layer);
    dgx_screen_destroy(&display);
}

/* a copy has memory of its own: it stays when the screen and the memory are gone */
static void test_clone_owns_its_memory(void)
{
    uint8_t      *memory = calloc(2u * 5u * 4u, 1);
    dgx_screen_t *s      = dgx_vscreen_from_array(memory, 5, 4, 16, DgxScreenRGB);
    CHECK(memory && s);
    if (!memory || !s) return;
    dgx_set_pixel(s, 3, 2, 0x4321);
    dgx_screen_t *copy = dgx_vscreen_clone(s);
    CHECK(copy != NULL);
    CHECK(((dgx_vscreen_t *)copy)->v_array != memory);
    dgx_screen_destroy(&s);
    free(memory);
    if (copy) {
        CHECK(copy->get_pixel(copy, 3, 2) == 0x4321);
        dgx_set_pixel(copy, 4, 3, 1);
    }
    dgx_screen_destroy(&copy);
}

/* an 8-bit picture over memory of the program goes through a table as any other */
static void test_source_through_a_table(void)
{
    static uint8_t  memory[6 * 2] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static uint16_t lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)(i * 257);
    dgx_screen_t *picture = dgx_vscreen_from_array(memory, 6, 2, 8, DgxScreenRGB);
    dgx_screen_t *display = dgx_vscreen_init(6, 2, 16, DgxScreenRGB);
    CHECK(picture && display);
    if (picture && display) {
        dgx_vscreen8_to_screen16(display, 0, 0, picture, lut, 0u);
        for (int i = 0; i < 12; ++i) CHECK(display->get_pixel(display, i % 6, i / 6) == lut[i]);
    }
    dgx_screen_destroy(&display);
    dgx_screen_destroy(&picture);
}

int main(void)
{
    static const uint8_t depths[] = {1, 4, 8, 12, 16, 18, 24, 32};
    for (size_t i = 0; i < sizeof(depths); ++i) {
        for (size_t shift = 0; shift < 4; ++shift) test_draws_as_its_own(depths[i], shift);
    }
    test_destroy_leaves_the_memory();
    test_refused_at_the_start();
    test_other_memory();
    test_bands_of_another_stride();
    test_window_starts_anew();
    test_nothing_left_dirty();
    test_refused_later();
    test_overlay_keeps_its_hold();
    test_clone_owns_its_memory();
    test_source_through_a_table();
    CHECK_DONE();
}
