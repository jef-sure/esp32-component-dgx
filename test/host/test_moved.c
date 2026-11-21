/*
 * The hard pen in a screen moved by whole pixels: a segment, a curve and a
 * handwritten line drawn at (x - dx, y - dy) in one screen are the pixels
 * drawn at (x, y) in another, wherever both screens have the pixel, the
 * edge of a screen cutting the drawing as a clip does. A screen of a strip
 * of rows draws so, the part of the picture above it at places below 0.
 */
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "dgx_vscreen.h"
#include "fonts/font0ss.h"

#define W 48
#define H 40

static float between(float lo, float hi)
{
    return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

/* a place that is a half now and then, where rounding decides */
static float place(int lo, int hi)
{
    return rand() % 3 ? between((float)lo, (float)hi) : (float)(lo + rand() % (hi - lo)) + 0.5f;
}

/* the pixels of `a` at (x + dx, y + dy) and of `b` at (x, y) that differ */
static int differ(dgx_screen_t *a, dgx_screen_t *b, int dx, int dy)
{
    int off = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int ax = x + dx, ay = y + dy;
            if (ax >= 0 && ay >= 0 && ax < W && ay < H && dgx_get_pixel(a, ax, ay) != dgx_get_pixel(b, x, y)) ++off;
        }
    return off;
}

static void clear(dgx_screen_t *s)
{
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
}

static void test_segments(dgx_screen_t *a, dgx_screen_t *b, uint32_t color)
{
    int wrong = 0;
    for (int i = 0; i < 3000; ++i) {
        const int   discs = rand() % 4, dx = rand() % 61 - 30, dy = rand() % 61 - 30;
        const int   pen   = (1 + rand() % 6) | (discs & 1 ? DGX_PEN_START_DISC : 0) | (discs & 2 ? DGX_PEN_END_DISC : 0);
        const float x1 = place(-10, 58), y1 = place(-10, 50), x2 = place(-10, 58), y2 = place(-10, 50);
        clear(a), clear(b);
        dgx_draw_thick_segment(a, x1, y1, x2, y2, pen, color);
        dgx_draw_thick_segment(b, x1 - dx, y1 - dy, x2 - dx, y2 - dy, pen, color);
        wrong += differ(a, b, dx, dy) > 0;
    }
    CHECK(wrong == 0);
}

static void test_curves(dgx_screen_t *a, dgx_screen_t *b, uint32_t color)
{
    int wrong = 0;
    for (int i = 0; i < 2000; ++i) {
        const int      pen = 1 + rand() % 6, dx = rand() % 61 - 30, dy = rand() % 61 - 30;
        dgx_point_2d_t p[4], q[4];
        for (int k = 0; k < 4; ++k) {
            p[k].x = (int16_t)(rand() % 70 - 11), p[k].y = (int16_t)(rand() % 60 - 10);
            q[k].x = (int16_t)(p[k].x - dx), q[k].y = (int16_t)(p[k].y - dy);
        }
        clear(a), clear(b);
        if (i & 1) dgx_draw_bezier3(a, p, 0, pen, color), dgx_draw_bezier3(b, q, 0, pen, color);
        else dgx_draw_bezier4(a, p, 0, pen, color), dgx_draw_bezier4(b, q, 0, pen, color);
        wrong += differ(a, b, dx, dy) > 0;
    }
    CHECK(wrong == 0);
}

/* the handwriting of a strip: rows of 8, the line at its place less the strip's top */
static void test_handwriting(dgx_screen_t *a, dgx_screen_t *b, uint32_t color)
{
    const dgx_font_t *font  = &font0ss;
    const float scale = 16.0f / font->hw->symbol_size_y;
    int         wrong = 0;
    for (int base = 10; base <= 30; ++base)
        for (int top = 0; top < H; top += 8) {
            dgx_hw_context_t pen = {.scr = a, .font = font, .scale_x = scale, .scale_y = scale, .width = 2, .color = color, .joined = true};
            clear(a), clear(b);
            dgx_hw_draw(&pen, 2, base, "four five");
            pen.scr = b;
            dgx_hw_draw(&pen, 2, base - top, "four five");
            wrong += differ(a, b, 0, top) > 0;
        }
    CHECK(wrong == 0);
}

int main(void)
{
    dgx_screen_t *a16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB), *b16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    dgx_screen_t *a8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *b8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    CHECK(a16 && b16 && a8 && b8);
    srand(11);
    test_segments(a16, b16, 0xffff);
    test_segments(a8, b8, 0xff);
    test_curves(a16, b16, 0xffff);
    test_curves(a8, b8, 0xff);
    test_handwriting(a16, b16, 0xffff);
    dgx_screen_destroy(&a16), dgx_screen_destroy(&b16), dgx_screen_destroy(&a8), dgx_screen_destroy(&b8);
    CHECK_DONE();
}
