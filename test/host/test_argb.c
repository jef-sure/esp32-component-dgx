/*
 * A rectangle of a screen read as ARGB8888 colors, as a window on a PC takes
 * them: every depth of a virtual screen, an 8-bit screen through a table and
 * as grey, a page screen read pixel by pixel, the stride of the destination,
 * and rectangles that do not lie within the screen.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

#define W 13
#define H 5

/* a colored rectangle on a black screen of the depth, read back whole */
static void check_depth(uint8_t bits, uint32_t color, uint32_t want)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, bits, DgxScreenRGB);
    CHECK(s != NULL);
    if (!s) return;
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_fill_rectangle(s, 3, 1, 5, 3, color);
    uint32_t out[W * H];
    CHECK(dgx_vscreen_to_argb8888(s, 0, 0, W, H, NULL, out, W));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int      in = x >= 3 && x < 8 && y >= 1 && y < 4;
            uint32_t v  = out[y * W + x];
            if (v != (in ? want : 0xff000000u)) {
                fprintf(stderr, "depth %u at %d,%d: %08x\n", bits, x, y, (unsigned)v);
                CHECK(0);
                goto done;
            }
        }
done:
    dgx_screen_destroy(&s);
}

int main(void)
{
    /* white is white at every depth, and the channels land where they belong */
    check_depth(16, dgx_rgb_to_16(255, 255, 255), 0xffffffffu);
    check_depth(16, dgx_rgb_to_16(255, 0, 0), 0xffff0000u);
    check_depth(16, dgx_rgb_to_16(0, 255, 0), 0xff00ff00u);
    check_depth(16, dgx_rgb_to_16(0, 0, 255), 0xff0000ffu);
    check_depth(16, dgx_rgb_to_16(0x84, 0x82, 0x08), 0xff848208u); /* 10000 100000 00001 widened */
    check_depth(12, dgx_rgb_to_12(0xf0, 0x50, 0xa0), 0xffff55aau);
    check_depth(18, dgx_rgb_to_18(255, 255, 255), 0xffffffffu);
    check_depth(18, dgx_rgb_to_18(0x80, 0x40, 0x04), 0xff824104u);
    check_depth(24, dgx_rgb_to_24(0x12, 0x34, 0x56), 0xff123456u);
    check_depth(8, 0x7f, 0xff7f7f7fu);
    check_depth(1, 1, 0xffffffffu);

    /* 8 bits through a table; a part of the screen into a wider destination */
    {
        uint16_t lut[256];
        for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)dgx_rgb_to_16(0, 0, 0);
        lut[9] = (uint16_t)dgx_rgb_to_16(255, 0, 255);
        dgx_screen_t *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_set_pixel(s, 6, 2, 9);
        uint32_t out[4 * 8];
        for (int i = 0; i < 4 * 8; ++i) out[i] = 0x12345678u;
        CHECK(dgx_vscreen_to_argb8888(s, 5, 1, 3, 4, lut, out, 8));
        CHECK(out[1 * 8 + 1] == 0xffff00ffu);
        CHECK(out[0] == 0xff000000u && out[3 * 8 + 2] == 0xff000000u);
        CHECK(out[3] == 0x12345678u && out[3 * 8 + 7] == 0x12345678u); /* past the width of each row: untouched */
        /* rectangles not within the screen, a stride too short: nothing written */
        CHECK(!dgx_vscreen_to_argb8888(s, -1, 0, 3, 3, lut, out, 8));
        CHECK(!dgx_vscreen_to_argb8888(s, W - 2, 0, 3, 3, lut, out, 8));
        CHECK(!dgx_vscreen_to_argb8888(s, 0, H - 1, 3, 2, lut, out, 8));
        CHECK(!dgx_vscreen_to_argb8888(s, 0, 0, 3, 3, lut, out, 2));
        CHECK(out[3] == 0x12345678u);
        CHECK(dgx_vscreen_to_argb8888(s, 0, 0, 0, 0, lut, out, 0)); /* an empty rectangle is no error */
        dgx_screen_destroy(&s);
    }

    /* 1 bit on odd offsets, where a row starts inside a byte */
    {
        dgx_screen_t *s = dgx_vscreen_init(W, H, 1, DgxScreenRGB);
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_set_pixel(s, 0, 3, 1);
        dgx_set_pixel(s, 12, 2, 1);
        uint32_t out[W * H];
        CHECK(dgx_vscreen_to_argb8888(s, 0, 0, W, H, NULL, out, W));
        int on = 0;
        for (int i = 0; i < W * H; ++i) on += out[i] == 0xffffffffu;
        CHECK(on == 2 && out[3 * W] == 0xffffffffu && out[2 * W + 12] == 0xffffffffu);
        dgx_screen_destroy(&s);
    }

    /* a page screen is not linear: read pixel by pixel */
    {
        dgx_screen_t *s = dgx_bw_init(16, 16);
        CHECK(s != NULL && !dgx_vscreen_is_linear(s));
        dgx_fill_rectangle(s, 0, 0, 16, 16, 0);
        dgx_set_pixel(s, 5, 9, 1);
        uint32_t out[16 * 16];
        CHECK(dgx_vscreen_to_argb8888(s, 0, 0, 16, 16, NULL, out, 16));
        CHECK(out[9 * 16 + 5] == 0xffffffffu && out[9 * 16 + 4] == 0xff000000u);
        dgx_screen_destroy(&s);
    }
    CHECK_DONE();
}
