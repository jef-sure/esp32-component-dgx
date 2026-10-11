#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bus/dgx_fb_area_priv.h"
#include "check.h"

/*
 * A frame buffer written as a display behind a bus is: commands set a
 * window, then pixels follow, the high byte first. What lands in the frame
 * buffer is checked against a model that places every pixel on its own.
 */

#define FW 13 /* of the panel */
#define FH 7

static uint32_t seed = 2026;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

static uint8_t fb[FW * FH * 3 + 8], model[FW * FH * 3 + 8];

/* where the pixel (x, y) of the screen lies in the frame buffer */
static uint8_t *model_pixel(const dgx_fb_area_t *area, int x, int y)
{
    int px = area->swap_xy ? y : x, py = area->swap_xy ? x : y;
    if (area->mirror_x) px = FW - 1 - px;
    if (area->mirror_y) py = FH - 1 - py;
    return model + (py * FW + px) * area->pixel_bytes;
}

/* a color of the screen as the frame buffer holds it */
static void model_put(const dgx_fb_area_t *area, int x, int y, uint32_t color)
{
    uint8_t *p = model_pixel(area, x, y);
    if (area->pixel_bytes == 2) {
        if (area->swap_rb) color = (color & 0x07e0u) | (color >> 11) | ((color & 0x1fu) << 11);
        p[0] = (uint8_t)color;
        p[1] = (uint8_t)(color >> 8);
    } else {
        uint8_t r = (uint8_t)(color >> 16), g = (uint8_t)(color >> 8), b = (uint8_t)color;
        p[0] = area->swap_rb ? r : b;
        p[1] = g;
        p[2] = area->swap_rb ? b : r;
    }
}

/* a color as DGX keeps it in a buffer */
static uint8_t *buffer_put(uint8_t *lp, uint8_t pixel_bytes, uint32_t color)
{
    if (pixel_bytes == 3) *lp++ = (uint8_t)(color >> 16);
    *lp++ = (uint8_t)(color >> 8);
    *lp++ = (uint8_t)color;
    return lp;
}

/* the window, set the way dgx_screen_with_bus_set_area_window() does it */
static void set_window(dgx_fb_area_t *area, int left, int right, int top, int bottom, uint8_t memory_command)
{
    uint8_t columns[4] = {(uint8_t)(left >> 8), (uint8_t)left, (uint8_t)(right >> 8), (uint8_t)right};
    uint8_t rows[4]    = {(uint8_t)(top >> 8), (uint8_t)top, (uint8_t)(bottom >> 8), (uint8_t)bottom};
    CHECK(dgx_fb_area_command(area, DGX_FB_AREA_SET_COLUMNS));
    CHECK(dgx_fb_area_data(area, columns, 4));
    CHECK(dgx_fb_area_command(area, DGX_FB_AREA_SET_ROWS));
    CHECK(dgx_fb_area_data(area, rows, 4));
    CHECK(dgx_fb_area_command(area, memory_command));
}

static void test_windows(uint8_t pixel_bytes, bool swap_rb, bool mirror_x, bool mirror_y, bool swap_xy)
{
    dgx_fb_area_t area;
    memset(fb, 0xa5, sizeof(fb));
    memset(model, 0xa5, sizeof(model));
    dgx_fb_area_init(&area, fb + 4, FW, FH, pixel_bytes, swap_rb);
    dgx_fb_area_orientation(&area, mirror_x, mirror_y, swap_xy);
    int w = dgx_fb_area_width(&area), h = dgx_fb_area_height(&area);
    CHECK(w == (swap_xy ? FH : FW) && h == (swap_xy ? FW : FH));
    uint32_t color_mask = pixel_bytes == 2 ? 0xffffu : 0xffffffu;
    for (int round = 0; round < 200; ++round) {
        int left = rnd(0, w - 1), right = rnd(left, w - 1), top = rnd(0, h - 1), bottom = rnd(top, h - 1);
        int count = (right - left + 1) * (bottom - top + 1);
        uint8_t  data[FW * FH * 3], back[FW * FH * 3];
        uint8_t *lp = data;
        memcpy(model, fb + 4, FW * FH * 3); /* the model goes on from what the frame buffer holds */
        uint32_t low = UINT32_MAX, high = 0;
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                uint32_t color = ((uint32_t)rnd(0, 0xffff) << 8 | (uint32_t)rnd(0, 0xff)) & color_mask;
                lp             = buffer_put(lp, pixel_bytes, color);
                model_put(&area, x, y, color);
                uint32_t at = (uint32_t)(model_pixel(&area, x, y) - model);
                if (at < low) low = at;
                if (at + pixel_bytes > high) high = at + pixel_bytes;
            }
        }
        /* sent in pieces of any length, as a draw buffer is */
        set_window(&area, left, right, top, bottom, DGX_FB_AREA_WRITE);
        for (int sent = 0; sent < count;) {
            int piece = rnd(1, count - sent);
            CHECK(dgx_fb_area_data(&area, data + sent * pixel_bytes, (uint32_t)piece * pixel_bytes));
            sent += piece;
        }
        CHECK(memcmp(fb + 4, model, FW * FH * 3) == 0);
        /* nothing outside the frame buffer is touched */
        CHECK(fb[0] == 0xa5 && fb[3] == 0xa5 && fb[4 + FW * FH * pixel_bytes] == 0xa5);
        /* after the last pixel of the window comes its first */
        CHECK(area.x == left && area.y == top);
        uint32_t from, length;
        CHECK(dgx_fb_area_take_dirty(&area, &from, &length));
        CHECK(from == low && from + length == high);
        CHECK(!dgx_fb_area_take_dirty(&area, &from, &length));
        /* read back, it is what was sent */
        set_window(&area, left, right, top, bottom, DGX_FB_AREA_READ);
        dgx_fb_area_read(&area, back, (uint32_t)count);
        CHECK(memcmp(back, data, (size_t)count * pixel_bytes) == 0);
        CHECK(!dgx_fb_area_take_dirty(&area, &from, &length));
    }
}

/* a pixel and a rectangle put past the window: the same pixels as through it, and the window is left alone */
static void test_direct(uint8_t pixel_bytes, bool swap_rb, bool mirror_x, bool mirror_y, bool swap_xy)
{
    dgx_fb_area_t area;
    memset(fb, 0xa5, sizeof(fb));
    dgx_fb_area_init(&area, fb + 4, FW, FH, pixel_bytes, swap_rb);
    dgx_fb_area_orientation(&area, mirror_x, mirror_y, swap_xy);
    int      w = dgx_fb_area_width(&area), h = dgx_fb_area_height(&area);
    uint32_t color_mask = pixel_bytes == 2 ? 0xffffu : 0xffffffu;
    set_window(&area, 1, 3, 2, 4, DGX_FB_AREA_WRITE);
    uint32_t from, length;
    for (int round = 0; round < 200; ++round) {
        int      left = rnd(0, w - 1), right = rnd(left, w - 1), top = rnd(0, h - 1), bottom = rnd(top, h - 1);
        uint32_t color = ((uint32_t)rnd(0, 0xffff) << 8 | (uint32_t)rnd(0, 0xff)) & color_mask;
        memcpy(model, fb + 4, FW * FH * 3);
        uint32_t low = UINT32_MAX, high = 0;
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                model_put(&area, x, y, color);
                uint32_t at = (uint32_t)(model_pixel(&area, x, y) - model);
                if (at < low) low = at;
                if (at + pixel_bytes > high) high = at + pixel_bytes;
            }
        }
        if (round & 1) {
            dgx_fb_area_fill(&area, (uint16_t)left, (uint16_t)top, (uint16_t)(right - left + 1), (uint16_t)(bottom - top + 1), color);
        } else {
            /* the same rectangle pixel by pixel */
            for (int y = top; y <= bottom; ++y) {
                for (int x = left; x <= right; ++x) dgx_fb_area_set_pixel(&area, (uint16_t)x, (uint16_t)y, color);
            }
        }
        CHECK(memcmp(fb + 4, model, FW * FH * 3) == 0);
        CHECK(fb[0] == 0xa5 && fb[3] == 0xa5 && fb[4 + FW * FH * pixel_bytes] == 0xa5);
        CHECK(dgx_fb_area_take_dirty(&area, &from, &length));
        CHECK(from == low && from + length == high);
        CHECK(dgx_fb_area_get_pixel(&area, (uint16_t)left, (uint16_t)top) == color);
        CHECK(dgx_fb_area_get_pixel(&area, (uint16_t)right, (uint16_t)bottom) == color);
        CHECK(area.left == 1 && area.right == 3 && area.top == 2 && area.bottom == 4 && area.x == 1 && area.y == 2);
    }
    CHECK(!dgx_fb_area_take_dirty(&area, &from, &length));
}

/* a window larger than the screen is cut, a command of the display is left to it */
static void test_bounds(void)
{
    dgx_fb_area_t area;
    memset(fb, 0, sizeof(fb));
    dgx_fb_area_init(&area, fb, FW, FH, 2, false);
    set_window(&area, 5, 500, 3, 400, DGX_FB_AREA_WRITE);
    CHECK(area.left == 5 && area.right == FW - 1 && area.top == 3 && area.bottom == FH - 1);
    set_window(&area, 9, 2, 6, 1, DGX_FB_AREA_WRITE);
    CHECK(area.left == 2 && area.right == 9 && area.top == 1 && area.bottom == 6);
    uint8_t one[2] = {0x12, 0x34};
    CHECK(dgx_fb_area_data(&area, one, 2));
    CHECK(fb[(1 * FW + 2) * 2] == 0x34 && fb[(1 * FW + 2) * 2 + 1] == 0x12);
    /* the rest of a pixel cut short is dropped */
    CHECK(dgx_fb_area_data(&area, one, 1));
    CHECK(area.x == 3 && area.y == 1);
    /* writing goes on where it stopped */
    CHECK(dgx_fb_area_command(&area, DGX_FB_AREA_WRITE_CONTINUE));
    CHECK(dgx_fb_area_data(&area, one, 2));
    CHECK(fb[(1 * FW + 3) * 2] == 0x34 && area.x == 4);
    CHECK(!dgx_fb_area_command(&area, 0x11));
    CHECK(!dgx_fb_area_data(&area, one, 2));
    CHECK(area.x == 4);
}

int main(void)
{
    for (int bits = 0; bits < 32; ++bits) {
        test_windows(bits & 16 ? 3 : 2, bits & 8, bits & 4, bits & 2, bits & 1);
        test_direct(bits & 16 ? 3 : 2, bits & 8, bits & 4, bits & 2, bits & 1);
    }
    test_bounds();
    CHECK_DONE();
}
