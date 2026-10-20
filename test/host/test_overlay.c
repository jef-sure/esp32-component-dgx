#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bus/dgx_fb_area_priv.h"
#include "check.h"
#include "dgx_draw.h"
#include "dgx_overlay.h"
#include "vscreen/dgx_overlay_priv.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"

/*
 * An overlay layer of a screen: its colors mixed into what goes to the
 * screen from virtual screens, in every way a transfer can go, and the
 * changed places it keeps for whoever puts the picture on the screen.
 */

static uint32_t seed = 77;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

/* the mix of two RGB565 colors, channel by channel, `weight` of 32 of the first */
static uint16_t model_mix(uint16_t color, uint16_t under, int weight)
{
    const int shift[3] = {11, 5, 0}, mask[3] = {31, 63, 31};
    uint16_t  out      = 0;
    for (int c = 0; c < 3; ++c) {
        const int a = (color >> shift[c]) & mask[c], b = (under >> shift[c]) & mask[c];
        out |= (uint16_t)(((a * weight + b * (32 - weight)) >> 5) << shift[c]);
    }
    return out;
}

/* 0, half and all of the color: the mix rounds down channel by channel, 255 gives the color itself, 0 leaves the pixel */
static void test_mix(void)
{
    for (int round = 0; round < 20000; ++round) {
        const uint16_t color = (uint16_t)rnd(0, 0xffff), under = (uint16_t)rnd(0, 0xffff);
        const int      weight = rnd(0, 32);
        const uint32_t pre    = ((color | (uint32_t)color << 16) & 0x07E0F81Fu) * (uint32_t)weight;
        CHECK(dgx_bus_overlay_mix(under, pre, 32u - (uint32_t)weight) == model_mix(color, under, weight));
    }
}

#define DW 40
#define DH 30
#define LW 23
#define LH 17
#define MARK 0x5aa5

static uint16_t lut[256], layer_lut[256];

static int weight_of(uint8_t alpha)
{
    return (alpha * 32 + 127) / 255;
}

/* the layer of the test over the display at (lx, ly): what the pixel (x, y) of the display gets from `color` */
static uint16_t expected_over(dgx_screen_t *layer, int lx, int ly, uint8_t alpha, int x, int y, uint16_t color)
{
    const int ix = x - lx, iy = y - ly;
    if (ix < 0 || iy < 0 || ix >= LW || iy >= LH) return color;
    const uint8_t index = (uint8_t)dgx_get_pixel(layer, ix, iy);
    return index ? model_mix(layer_lut[index], color, weight_of(alpha)) : color;
}

/* a display behind a bus: a window and its pixels, two bytes each, the high one first */
static uint16_t panel[DW * DH];
static int      area_l, area_r, area_b, area_x, area_y;

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    area_l = left, area_r = right, area_b = bottom, area_x = left, area_y = top;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    (void)scr;
    for (uint32_t i = 0; i + 1 < lenbits / 8; i += 2) {
        if (area_y <= area_b && area_x < DW && area_y < DH) panel[area_x + area_y * DW] = (uint16_t)(data[i] << 8 | data[i + 1]);
        if (++area_x > area_r) area_x = area_l, ++area_y;
    }
}

static void panel_wait(dgx_screen_t *scr)
{
    (void)scr;
}

static void panel_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr;
    if (x >= 0 && y >= 0 && x < DW && y < DH) panel[x + y * DW] = (uint16_t)color;
}

/* a display read back, as a DSI one is: the transparent ways go pixel by pixel to it */
static uint32_t panel_get_pixel(dgx_screen_t *scr, int x, int y)
{
    (void)scr;
    return x >= 0 && y >= 0 && x < DW && y < DH ? panel[x + y * DW] : 0;
}

static void panel_update(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

/* a bus with faster ways, as a DSI one: rows put as they lie, and indices through a table with the layer it is given */
static int rows_taken, indexed_taken, indexed_mixed;

static bool panel_write_rows(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *rows,
                             uint32_t pitch)
{
    (void)bus;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) panel[left + x + (top + y) * DW] = (uint16_t)(rows[y * pitch + 2 * x] << 8 | rows[y * pitch + 2 * x + 1]);
    }
    ++rows_taken;
    return true;
}

static bool panel_write_indexed(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                                const dgx_bus_indexed_t *picture)
{
    (void)bus;
    if (picture->smooth || picture->orientation) return false; /* those come the other way */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t index = picture->pixels[(picture->y + y) / picture->scale * picture->pitch + (picture->x + x) / picture->scale];
            uint16_t      color = picture->lut[index];
            if (picture->overlay) color = dgx_overlay_pixel(picture->overlay, left + x, top + y, color);
            panel[left + x + (top + y) * DW] = color;
        }
    }
    ++indexed_taken;
    indexed_mixed += picture->overlay != NULL;
    return true;
}

/*
 * Every transfer from a virtual screen, to a virtual display and to one behind a bus, with and without
 * the layer: with it every pixel put is the one put without it mixed with the layer, the others stay.
 */
static void test_transfers(void)
{
    dgx_screen_t *src8 = dgx_vscreen_init(13, 11, 8, DgxScreenRGB), *src16 = dgx_vscreen_init(19, 14, 16, DgxScreenRGB);
    dgx_screen_t *layer = dgx_vscreen_init(LW, LH, 8, DgxScreenRGB);
    dgx_screen_t *plain = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB), *mixed = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)rnd(0, 0xffff), layer_lut[i] = (uint16_t)rnd(0, 0xffff);
    for (int y = 0; y < 11; ++y) {
        for (int x = 0; x < 13; ++x) dgx_set_pixel(src8, x, y, (uint32_t)rnd(0, 3));
    }
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < 19; ++x) dgx_set_pixel(src16, x, y, (uint32_t)rnd(0, 3) * 0x4321u); /* few colors, 0 among them, for smoothing and holes */
    }
    static uint8_t        bus_buffer[48];
    dgx_bus_protocols_t   bus  = {.buffer = bus_buffer, .buffer_len = sizeof(bus_buffer)};
    dgx_screen_with_bus_t sbus = {.bus = &bus};
    dgx_screen_t         *lcd  = &sbus.scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;
    lcd->set_pixel = panel_set_pixel, lcd->update_screen = panel_update;
    int mixed_pixels = 0;
    for (int round = 0; round < 3000; ++round) {
        /* a new picture in the layer now and then: few pixels, many, none */
        if (round % 40 == 0) {
            dgx_fill_rectangle(layer, 0, 0, LW, LH, 0);
            const int density = rnd(0, 3);
            for (int y = 0; y < LH; ++y) {
                for (int x = 0; x < LW; ++x) {
                    if (rnd(0, 3) < density) dgx_set_pixel(layer, x, y, (uint32_t)rnd(1, 255));
                }
            }
        }
        const int     lx = rnd(-10, DW - 5), ly = rnd(-8, DH - 4);
        const uint8_t alpha = (uint8_t)(round % 5 == 0 ? 255 : round % 5 == 1 ? 0 : rnd(1, 254));
        const int     kind  = round % 4; /* 8 to 16 plain, scaled, smoothed, 16 to 16 */
        const bool    by_bus = (round >> 2) & 1, transparent = (round >> 3) % 3 == 0;
        const int     n      = kind == 0 ? 1 : kind == 1 ? rnd(2, 3) : kind == 2 ? rnd(2, 4) : rnd(1, 3);
        /* every other round turned; the smoothing of 16 to 16 too, now and then */
        const unsigned turn  = (round >> 6) & 1 ? (unsigned)rnd(1, 7) : 0;
        const uint32_t smooth = kind == 2 || (kind == 3 && rnd(0, 1)) ? (rnd(0, 1) ? DGX_VSCREEN_SMOOTH_SFX : DGX_VSCREEN_SMOOTH) : 0;
        const uint32_t flags = (transparent ? DGX_VSCREEN_TRANSPARENT : 0) | DGX_VSCREEN_SCALE(n) | smooth | DGX_VSCREEN_ORIENTED(turn);
        const int      x = rnd(-20, DW), y = rnd(-20, DH), left = rnd(-2, 12), top = rnd(-2, 10), w = rnd(0, 20), h = rnd(0, 16);
        dgx_screen_t  *dst = by_bus ? lcd : mixed;
        /* every other bus has the faster ways; every other display is read back */
        const bool fast   = (round >> 4) & 1;
        bus.write_rows    = fast ? panel_write_rows : NULL;
        bus.write_indexed = fast ? panel_write_indexed : NULL;
        lcd->get_pixel    = (round >> 5) & 1 ? panel_get_pixel : NULL;
        /* without the layer, then with it, from the same start */
        dgx_fill_rectangle(plain, 0, 0, DW, DH, MARK);
        if (kind < 3) dgx_vscreen8_region_to_screen16(plain, x + n * left, y + n * top, src8, left, top, w, h, lut, flags);
        else dgx_vscreen_region_to_screen(plain, x + left, y + top, src16, left, top, w, h, flags);
        dgx_fill_rectangle(mixed, 0, 0, DW, DH, MARK);
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;
        CHECK(dgx_overlay_set(dst, layer, lx, ly, layer_lut, alpha));
        if (kind < 3) dgx_vscreen8_region_to_screen16(dst, x + n * left, y + n * top, src8, left, top, w, h, lut, flags);
        else dgx_vscreen_region_to_screen(dst, x + left, y + top, src16, left, top, w, h, flags);
        dgx_overlay_clear(dst);
        int ox, oy, ow, oh;
        dgx_overlay_take_dirty(dst, &ox, &oy, &ow, &oh);
        CHECK(dst->overlay == NULL && layer->overlay == NULL);
        int wrong = 0;
        for (int py = 0; py < DH; ++py) {
            for (int px = 0; px < DW; ++px) {
                const uint16_t was = (uint16_t)dgx_get_pixel(plain, px, py);
                const uint16_t got = by_bus ? panel[px + py * DW] : (uint16_t)dgx_get_pixel(mixed, px, py);
                const uint16_t want = was == MARK ? MARK : expected_over(layer, lx, ly, alpha, px, py, was);
                wrong += got != want;
                mixed_pixels += was != MARK && want != was;
            }
        }
        if (wrong) {
            fprintf(stderr, "overlay transfer: round %d kind %d bus %d transparent %d n %d: %d pixels wrong\n", round, kind, by_bus, transparent, n, wrong);
            ++check_failures;
        }
    }
    CHECK(mixed_pixels > 3000);
    /* the bus took rows where no layer was over them, and indices with the layer and without it */
    CHECK(rows_taken > 5 && indexed_taken > 40 && indexed_mixed > 10 && indexed_taken > indexed_mixed + 10);
    dgx_screen_destroy(&src8);
    dgx_screen_destroy(&src16);
    dgx_screen_destroy(&layer);
    dgx_screen_destroy(&plain);
    dgx_screen_destroy(&mixed);
}

/* the changed places: what is drawn into the layer, where it was and is, where it was when taken away */
static void test_changes(void)
{
    dgx_screen_t *display = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB), *layer = dgx_vscreen_init(LW, LH, 8, DgxScreenRGB);
    int           x, y, w, h;
    for (int i = 0; i < 256; ++i) layer_lut[i] = (uint16_t)i;
    /* nothing yet, and an empty layer changes nothing when laid */
    CHECK(!dgx_overlay_take_dirty(display, &x, &y, &w, &h) && x == 0 && w == 0);
    CHECK(dgx_overlay_set(display, layer, 5, 6, layer_lut, 128));
    CHECK(!dgx_overlay_take_dirty(display, &x, &y, &w, &h));
    /* drawn into: that place of the display, outside a batch and inside one */
    dgx_fill_rectangle(layer, 2, 3, 4, 2, 7);
    CHECK(dgx_overlay_take_dirty(display, &x, &y, &w, &h) && x == 7 && y == 9 && w == 4 && h == 2);
    CHECK(!dgx_overlay_take_dirty(display, &x, &y, &w, &h));
    dgx_screen_progress_up(layer);
    dgx_set_pixel(layer, 0, 0, 9);
    dgx_set_pixel(layer, 10, 12, 9);
    dgx_screen_progress_down(layer);
    CHECK(dgx_overlay_take_dirty(display, &x, &y, &w, &h) && x == 5 && y == 6 && w == 11 && h == 13);
    /* moved: where it has something, before and after */
    CHECK(dgx_overlay_set(display, layer, 20, 6, layer_lut, 128));
    CHECK(dgx_overlay_take_dirty(display, &x, &y, &w, &h) && x == 5 && y == 6 && w == 26 && h == 13);
    /* a part outside the display is cut off */
    CHECK(dgx_overlay_set(display, layer, 35, 25, layer_lut, 128));
    dgx_overlay_take_dirty(display, &x, &y, &w, &h);
    dgx_fill_rectangle(layer, 0, 0, LW, LH, 3);
    CHECK(dgx_overlay_take_dirty(display, &x, &y, &w, &h) && x == 35 && y == 25 && w == DW - 35 && h == DH - 25);
    /* drawn after it was laid, into a part that was empty: mixed in like the rest */
    {
        dgx_screen_t *big = dgx_vscreen_init(100, 70, 8, DgxScreenRGB), *src = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
        dgx_fill_rectangle(src, 0, 0, DW, DH, 0x1234);
        CHECK(dgx_overlay_set(display, big, -50, -40, layer_lut, 255));
        dgx_set_pixel(big, 75, 55, 9); /* far from everything, in a tile that had nothing */
        /* by the window of the display, which looks at the layer's tiles; and pixel by pixel, which does not */
        dgx_vscreen_region_to_screen(display, 0, 0, src, 0, 0, DW, DH, 0);
        CHECK(dgx_get_pixel(display, 25, 15) == layer_lut[9] && dgx_get_pixel(display, 24, 15) == 0x1234);
        dgx_fill_rectangle(display, 0, 0, DW, DH, 0);
        dgx_vscreen_to_screen(display, 0, 0, src, 0);
        CHECK(dgx_get_pixel(display, 25, 15) == layer_lut[9] && dgx_get_pixel(display, 24, 15) == 0x1234);
        dgx_overlay_clear(display);
        dgx_overlay_take_dirty(display, &x, &y, &w, &h);
        dgx_screen_destroy(&big);
        dgx_screen_destroy(&src);
    }
    /* taken away: its place is kept until taken, then the display has nothing of it */
    CHECK(dgx_overlay_set(display, layer, 1, 2, layer_lut, 200));
    dgx_overlay_take_dirty(display, &x, &y, &w, &h);
    dgx_overlay_clear(display);
    CHECK(display->overlay != NULL && layer->overlay == NULL);
    dgx_set_pixel(layer, 0, 0, 1); /* drawn after it was taken away: nothing of the display */
    CHECK(dgx_overlay_take_dirty(display, &x, &y, &w, &h) && x == 1 && y == 2 && w == LW && h == LH);
    CHECK(display->overlay == NULL && !dgx_overlay_take_dirty(display, &x, &y, &w, &h));
    /* one layer a display, a display a layer; not an 8-bit layer or a 16-bit display: refused */
    dgx_screen_t *other = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB), *deep = dgx_vscreen_init(8, 8, 16, DgxScreenRGB);
    CHECK(dgx_overlay_set(display, layer, 0, 0, layer_lut, 255));
    CHECK(!dgx_overlay_set(other, layer, 0, 0, layer_lut, 255));
    CHECK(!dgx_overlay_set(other, deep, 0, 0, layer_lut, 255));
    CHECK(!dgx_overlay_set(layer, deep, 0, 0, layer_lut, 255));
    /* another layer in its place: the first one's place is changed, the first one is free */
    dgx_screen_t *second = dgx_vscreen_init(4, 4, 8, DgxScreenRGB);
    dgx_fill_rectangle(second, 0, 0, 4, 4, 1);
    dgx_overlay_take_dirty(display, &x, &y, &w, &h);
    CHECK(dgx_overlay_set(display, second, 30, 20, layer_lut, 255));
    CHECK(layer->overlay == NULL && second->overlay == display->overlay);
    CHECK(dgx_overlay_take_dirty(display, &x, &y, &w, &h) && x == 0 && y == 0 && w == 34 && h == 24);
    /* the layer destroyed while it lies over the display: its place is changed, the display goes on */
    dgx_screen_destroy(&second);
    CHECK(dgx_overlay_take_dirty(display, &x, &y, &w, &h) && x == 30 && y == 20 && w == 4 && h == 4);
    /* the display destroyed while it has a layer: the layer draws as before */
    CHECK(dgx_overlay_set(display, layer, 0, 0, layer_lut, 255));
    dgx_screen_destroy(&display);
    CHECK(layer->overlay == NULL);
    dgx_set_pixel(layer, 1, 1, 2);
    dgx_screen_destroy(&layer);
    dgx_screen_destroy(&other);
    dgx_screen_destroy(&deep);
}

/*
 * Rows of the panel with the layer mixed in, in every orientation: the pixels of the screen are the
 * colors of the picture mixed with the layer where it has something.
 */
#define FW 13
#define FH 9

static void test_panel(bool mirror_x, bool mirror_y, bool swap_xy)
{
    /* a layer of 3 by 2 tiles of 32, some of them empty, and its map */
    enum { PW = 70, PH = 40 };
    static uint8_t  fb[FW * FH * 2], band[FW * FH * 2], ink[7 * 6], pixels[PW * PH], tiles[3 * 2];
    static uint32_t pre[256];
    dgx_fb_area_t   area;
    dgx_fb_area_init(&area, fb, FW, FH, 2, false);
    dgx_fb_area_orientation(&area, mirror_x, mirror_y, swap_xy);
    const int sw = dgx_fb_area_width(&area), sh = dgx_fb_area_height(&area);
    for (int round = 0; round < 300; ++round) {
        for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)rnd(0, 0xffff), layer_lut[i] = (uint16_t)rnd(0, 0xffff);
        for (int i = 0; i < 7 * 6; ++i) ink[i] = (uint8_t)rnd(0, 255);
        for (int t = 0; t < 6; ++t) tiles[t] = (uint8_t)rnd(0, 1);
        for (int i = 0; i < PW * PH; ++i) pixels[i] = (uint8_t)(tiles[(i / PW / 32) * 3 + i % PW / 32] && !rnd(0, 2) ? rnd(1, 255) : 0);
        const int weight = rnd(0, 32);
        for (int i = 0; i < 256; ++i) pre[i] = ((layer_lut[i] | (uint32_t)layer_lut[i] << 16) & 0x07E0F81Fu) * (uint32_t)weight;
        /* the layer somewhere over the screen, and where it may have something within it and the screen */
        dgx_bus_overlay_t over = {.pixels = pixels, .pitch = PW, .x = rnd(-PW + 2, sw - 2), .y = rnd(-PH + 2, sh - 2), .pre = pre,
                                  .under = 32u - (uint32_t)weight, .tiles = round & 1 ? tiles : NULL, .tiles_across = 3};
        over.left   = rnd(over.x, over.x + PW - 1), over.top = rnd(over.y, over.y + PH - 1);
        over.right  = rnd(over.left, over.x + PW - 1), over.bottom = rnd(over.top, over.y + PH - 1);
        over.left   = over.left < 0 ? 0 : over.left, over.top = over.top < 0 ? 0 : over.top;
        over.right  = over.right >= sw ? sw - 1 : over.right, over.bottom = over.bottom >= sh ? sh - 1 : over.bottom;
        if (over.left > over.right || over.top > over.bottom) continue;
        const int left = rnd(0, sw - 1), top = rnd(0, sh - 1), width = rnd(1, sw - left < 7 ? sw - left : 7), height = rnd(1, sh - top < 6 ? sh - top : 6);
        const dgx_bus_indexed_t picture = {.pixels = ink, .pitch = 7, .width = 7, .height = 6, .scale = 1, .lut = lut};
        uint16_t x0, y0, x1, y1;
        dgx_fb_area_to_panel(&area, left, left + width - 1, top, top + height - 1, &x0, &y0, &x1, &y1);
        const int columns = x1 - x0 + 1, rows = y1 - y0 + 1;
        memset(fb, 0, sizeof(fb));
        for (int r = 0; r < rows;) {
            const int n = rnd(1, rows - r);
            dgx_fb_area_indexed_rows(&area, band, (uint16_t)left, (uint16_t)top, (uint16_t)width, (uint16_t)height, &picture, (uint16_t)r, (uint16_t)n, NULL);
            dgx_fb_area_overlay_rows(&area, band, (uint16_t)left, (uint16_t)top, (uint16_t)width, (uint16_t)height, &over, (uint16_t)r, (uint16_t)n);
            for (int i = 0; i < n; ++i) memcpy(fb + ((y0 + r + i) * FW + x0) * 2, band + i * columns * 2, (size_t)columns * 2);
            r += n;
        }
        int wrong = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int sx = left + x, sy = top + y;
                uint16_t  want = lut[ink[y * 7 + x]];
                if (sx >= over.left && sx <= over.right && sy >= over.top && sy <= over.bottom) {
                    const uint8_t index = pixels[(sy - over.y) * PW + sx - over.x];
                    if (index) want = model_mix(layer_lut[index], want, weight);
                }
                wrong += (uint16_t)dgx_fb_area_get_pixel(&area, (uint16_t)sx, (uint16_t)sy) != want;
            }
        }
        if (wrong) {
            fprintf(stderr, "overlay rows: mirror %d %d swap %d round %d: %d pixels wrong\n", mirror_x, mirror_y, swap_xy, round, wrong);
            ++check_failures;
        }
    }
}

int main(void)
{
    test_mix();
    test_transfers();
    test_changes();
    for (int bits = 0; bits < 8; ++bits) test_panel(bits & 4, bits & 2, bits & 1);
    CHECK_DONE();
}
