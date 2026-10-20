#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bus/dgx_fb_area_priv.h"
#include "check.h"
#include "scale_ref.h"

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
        /* where the rectangle lies on the panel: the bounds of where its pixels went */
        uint16_t x0, y0, x1, y1;
        int      px0 = FW, py0 = FH, px1 = -1, py1 = -1;
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                int at = (int)(model_pixel(&area, x, y) - model) / pixel_bytes, px = at % FW, py = at / FW;
                if (px < px0) px0 = px;
                if (px > px1) px1 = px;
                if (py < py0) py0 = py;
                if (py > py1) py1 = py;
            }
        }
        dgx_fb_area_to_panel(&area, left, right, top, bottom, &x0, &y0, &x1, &y1);
        CHECK(x0 == px0 && y0 == py0 && x1 == px1 && y1 == py1);
        /* and one pixel of it */
        int at = (int)(model_pixel(&area, right, top) - model) / pixel_bytes;
        dgx_fb_area_point_to_panel(&area, right, top, &x0, &y0);
        CHECK(x0 == at % FW && y0 == at / FW);
    }
    CHECK(!dgx_fb_area_take_dirty(&area, &from, &length));
}

/*
 * Long rows of 16-bit pixels are moved by words. Every way the buffer and the
 * frame buffer can lie against the boundaries of a word, lengths around the
 * one words start from, buffers at odd addresses too, the screen mirrored.
 */
#define LW 83
#define LH 4
/* a rectangle moved to another place of the screen: every pixel of it is where it was that far away, the rest is as it was */
static void test_move(uint8_t pixel_bytes, bool swap_rb, bool mirror_x, bool mirror_y, bool swap_xy)
{
    static uint32_t before[FW > FH ? FW : FH][FW > FH ? FW : FH];
    dgx_fb_area_t   area;
    memset(fb, 0xa5, sizeof(fb));
    dgx_fb_area_init(&area, fb + 4, FW, FH, pixel_bytes, swap_rb);
    dgx_fb_area_orientation(&area, mirror_x, mirror_y, swap_xy);
    const int w = dgx_fb_area_width(&area), h = dgx_fb_area_height(&area);
    uint32_t  from, length;
    for (int round = 0; round < 300; ++round) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                dgx_fb_area_set_pixel(&area, (uint16_t)x, (uint16_t)y, ((uint32_t)rnd(0, 0xffff) << 8 | (uint32_t)rnd(0, 0xff)) & (pixel_bytes == 2 ? 0xffffu : 0xffffffu));
                before[y][x] = dgx_fb_area_get_pixel(&area, (uint16_t)x, (uint16_t)y);
            }
        }
        dgx_fb_area_take_dirty(&area, &from, &length);
        const int width = rnd(1, w), height = rnd(1, h), x = rnd(0, w - width), y = rnd(0, h - height);
        const int to_x = rnd(0, w - width), to_y = rnd(0, h - height);
        dgx_fb_area_move(&area, (uint16_t)x, (uint16_t)y, (uint16_t)width, (uint16_t)height, (uint16_t)to_x, (uint16_t)to_y);
        int      wrong = 0;
        uint32_t low = UINT32_MAX, high = 0;
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) {
                const bool moved = i >= to_x && i < to_x + width && j >= to_y && j < to_y + height;
                wrong += dgx_fb_area_get_pixel(&area, (uint16_t)i, (uint16_t)j) != (moved ? before[j - to_y + y][i - to_x + x] : before[j][i]);
                if (moved) {
                    /* the place of the pixel in the frame buffer, to tell what was written */
                    memcpy(model, fb + 4, FW * FH * 3);
                    const uint32_t at = (uint32_t)(model_pixel(&area, i, j) - model);
                    if (at < low) low = at;
                    if (at + pixel_bytes > high) high = at + pixel_bytes;
                }
            }
        }
        CHECK(wrong == 0);
        CHECK(fb[0] == 0xa5 && fb[3] == 0xa5 && fb[4 + FW * FH * pixel_bytes] == 0xa5);
        if (x == to_x && y == to_y) {
            CHECK(!dgx_fb_area_take_dirty(&area, &from, &length));
        } else {
            CHECK(dgx_fb_area_take_dirty(&area, &from, &length));
            CHECK(from == low && from + length == high);
        }
    }
}

static void test_long_rows(void)
{
    static _Alignas(8) uint8_t long_fb[LW * LH * 2 + 32], long_model[LW * LH * 2 + 32], pixels[LW * LH * 2 + 32];
    for (int mirror = 0; mirror < 2; ++mirror) {
        for (int fb_at = 0; fb_at < 4; ++fb_at) {
            for (int data_at = 0; data_at < 4; ++data_at) {
                dgx_fb_area_t area;
                memset(long_fb, 0xa5, sizeof(long_fb));
                dgx_fb_area_init(&area, long_fb + 8 + fb_at, LW, LH, 2, false);
                dgx_fb_area_orientation(&area, mirror, false, false);
                for (int round = 0; round < 300; ++round) {
                    int left = rnd(0, LW - 1), right = rnd(left, LW - 1), top = rnd(0, LH - 1), bottom = rnd(top, LH - 1);
                    int count = (right - left + 1) * (bottom - top + 1);
                    uint8_t *data = pixels + 8 + data_at, *lp = data;
                    memcpy(long_model, long_fb, sizeof(long_fb));
                    for (int y = top; y <= bottom; ++y) {
                        for (int x = left; x <= right; ++x) {
                            uint32_t color = (uint32_t)rnd(0, 0xffff);
                            lp[0] = (uint8_t)(color >> 8), lp[1] = (uint8_t)color, lp += 2;
                            int      px = mirror ? LW - 1 - x : x;
                            uint8_t *p  = long_model + 8 + fb_at + (y * LW + px) * 2;
                            p[0] = (uint8_t)color, p[1] = (uint8_t)(color >> 8);
                        }
                    }
                    set_window(&area, left, right, top, bottom, DGX_FB_AREA_WRITE);
                    for (int sent = 0; sent < count;) {
                        int piece = rnd(1, count - sent);
                        CHECK(dgx_fb_area_data(&area, data + sent * 2, (uint32_t)piece * 2));
                        sent += piece;
                    }
                    if (memcmp(long_fb, long_model, sizeof(long_fb)) != 0) {
                        fprintf(stderr, "long rows: frame buffer at +%d, buffer at +%d, mirror %d, window %d..%d x %d..%d differ\n", fb_at, data_at,
                                mirror, left, right, top, bottom);
                        ++check_failures;
                        memcpy(long_fb, long_model, sizeof(long_fb));
                    }
                }
            }
        }
    }
}

/*
 * Rows of the panel made from indices through a table: every index a square of `scale` pixels, or the
 * picture smoothed by Scale2x, Scale3x or their SFX kin, the picture turned on the screen in any of
 * its eight ways and the screen on the panel in any of its eight. Copied into the frame buffer as they
 * come, by bands of any height, they give the pixels of the screen the model puts one by one, from the
 * point-at-a-time shaders of scale_ref.h, and nothing outside the rectangle changes.
 */
#define IW 9 /* of the picture of indices */
#define IH 6

/* the point (u, v) of the picture made larger, `wide` by `high`, before it is turned, that its turned point (tx, ty) is */
static void unturned(unsigned turn, int wide, int high, int tx, int ty, int *u, int *v)
{
    if (!(turn & 4u)) {
        *u = turn & 1u ? wide - 1 - tx : tx;
        *v = turn & 2u ? high - 1 - ty : ty;
    } else {
        *u = turn & 1u ? wide - 1 - ty : ty;
        *v = turn & 2u ? high - 1 - tx : tx;
    }
}

static void test_pictures(bool mirror_x, bool mirror_y, bool swap_xy)
{
    static const uint8_t bus_smooth[3] = {0, DGX_BUS_SMOOTH, DGX_BUS_SMOOTH_SFX};
    static const int     ref_smooth_of[3] = {REF_BLOCKS, REF_SMOOTH, REF_SFX};
    static uint16_t      table[256];
    static uint32_t      values[IW * IH];
    static uint8_t       ink[IW * IH], band[FW * FH * 2];
    for (int i = 0; i < 256; ++i) table[i] = (uint16_t)rnd(0, 0xffff);
    dgx_fb_area_t area;
    dgx_fb_area_init(&area, fb, FW, FH, 2, false);
    dgx_fb_area_orientation(&area, mirror_x, mirror_y, swap_xy);
    const int           sw = dgx_fb_area_width(&area), sh = dgx_fb_area_height(&area);
    const ref_picture_t ref = {values, IW, IH};
    for (int round = 0; round < 1600; ++round) {
        if (round % 40 == 0) {
            /* many indices for squares, few for smoothing to find equal neighbours */
            const int kinds = round % 80 ? 3 : 256;
            for (int i = 0; i < IW * IH; ++i) ink[i] = (uint8_t)(values[i] = (uint32_t)rnd(0, kinds - 1));
        }
        const int      kind = round % 3, scale = kind ? rnd(2, 4) : rnd(1, 5);
        const unsigned turn = (unsigned)(round / 3) % 8u;
        const int      wide = scale * IW, high = scale * IH, tw = turn & 4u ? high : wide, th = turn & 4u ? wide : high;
        /* the rectangle no larger than the turned picture covers, and where in it it starts */
        const int left = rnd(0, sw - 1), top = rnd(0, sh - 1);
        const int width = rnd(1, sw - left < tw ? sw - left : tw), height = rnd(1, sh - top < th ? sh - top : th);
        const int px = rnd(0, tw - width), py = rnd(0, th - height);
        for (int i = 0; i < FW * FH * 2; ++i) fb[i] = model[i] = (uint8_t)rnd(0, 255);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int u, v;
                unturned(turn, wide, high, px + x, py + y, &u, &v);
                model_put(&area, left + x, top + y, table[ref_larger(&ref, scale, ref_smooth_of[kind], u, v)]);
            }
        }
        uint16_t x0, y0, x1, y1;
        dgx_fb_area_to_panel(&area, left, left + width - 1, top, top + height - 1, &x0, &y0, &x1, &y1);
        const int               columns = x1 - x0 + 1, rows = y1 - y0 + 1;
        const dgx_bus_indexed_t picture = {.pixels = ink, .pitch = IW, .width = IW, .height = IH, .x = (uint32_t)px, .y = (uint32_t)py,
                                           .scale = (uint8_t)scale, .smooth = bus_smooth[kind], .orientation = (uint8_t)turn, .lut = table};
        uint8_t          *scratch = malloc(dgx_fb_area_indexed_scratch(&picture, (uint32_t)columns));
        dgx_scale_lines_t lines;
        dgx_fb_area_indexed_lines(&area, &lines, (uint16_t)left, (uint16_t)top, (uint16_t)width, (uint16_t)height, &picture, scratch);
        for (int r = 0; r < rows;) {
            const int n = rnd(1, rows - r);
            dgx_fb_area_indexed_rows(&area, band, (uint16_t)left, (uint16_t)top, (uint16_t)width, (uint16_t)height, &picture, (uint16_t)r,
                                     (uint16_t)n, kind ? &lines : NULL);
            for (int i = 0; i < n; ++i) memcpy(fb + ((y0 + r + i) * FW + x0) * 2, band + i * columns * 2, (size_t)columns * 2);
            r += n;
        }
        free(scratch);
        if (memcmp(fb, model, FW * FH * 2) != 0) {
            fprintf(stderr, "picture rows: screen mirror %d %d swap %d, picture turned %u, %dx%d at %d,%d from %d,%d, scale %d smooth %d differ\n",
                    mirror_x, mirror_y, swap_xy, turn, width, height, left, top, px, py, scale, kind);
            ++check_failures;
        }
    }
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
        test_move(bits & 16 ? 3 : 2, bits & 8, bits & 4, bits & 2, bits & 1);
    }
    for (int bits = 0; bits < 8; ++bits) {
        test_pictures(bits & 4, bits & 2, bits & 1);
    }
    test_long_rows();
    test_bounds();
    CHECK_DONE();
}
