/*
 * A test of the gap: the overlay layer of a screen mixed into a transfer
 * that goes to the screen through the halves of a bus buffer large enough
 * for whole rows -- as the QSPI bus of a board is. test_overlay.c's
 * transfers go through a buffer of 48 bytes, where no row fits a half and
 * every row is cut; here a half holds whole rows (a half of the board's
 * buffer holds seven rows of 320 pixels), and the rows that fill a half to
 * its very end are the ones that can go wrong.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_draw.h"
#include "dgx_overlay.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"

static uint32_t seed = 1234;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

#define DW 80
#define DH 40
#define LW 40
#define LH 30
#define MARK 0x5aa5

static uint16_t lut[256], layer_lut[256];

/* the panel the bus fills: a window and its pixels, two bytes each, the high one first */
static uint16_t panel[DW * DH];
static int      area_x, area_y, area_w, area_h, area_i;
static int      windows;

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    area_x = left, area_y = top, area_w = right - left + 1, area_h = bottom - top + 1, area_i = 0;
    ++windows;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    (void)scr;
    for (uint32_t i = 0; i + 1 < lenbits / 8u; i += 2) {
        if (area_i < area_w * area_h) {
            const int x = area_x + area_i % area_w, y = area_y + area_i / area_w;
            if (x < DW && y < DH) panel[x + y * DW] = (uint16_t)(data[i] << 8 | data[i + 1]);
        }
        ++area_i;
    }
}

static void panel_wait(dgx_screen_t *scr)
{
    (void)scr;
}

static dgx_screen_t *layer_made(void)
{
    dgx_screen_t *layer = dgx_vscreen_init(LW, LH, 8, DgxScreenRGB);
    dgx_fill_rectangle(layer, 0, 0, LW, LH, 0);
    const int density = rnd(1, 3);
    for (int y = 0; y < LH; ++y)
        for (int x = 0; x < LW; ++x)
            if (rnd(0, 3) < density) dgx_set_pixel(layer, x, y, (uint32_t)rnd(1, 255));
    return layer;
}

/* the mix of one pixel as dgx_overlay_pixel makes it */
static uint16_t mix(uint16_t color, uint16_t over, int weight /* of 32, of over */)
{
    const int shift[3] = {11, 5, 0}, mask[3] = {31, 63, 31};
    uint16_t  out      = 0;
    for (int c = 0; c < 3; ++c) {
        const int a = (over >> shift[c]) & mask[c], b = (color >> shift[c]) & mask[c];
        out |= (uint16_t)(((a * weight + b * (32 - weight)) >> 5) << shift[c]);
    }
    return out;
}

static int weight_of(uint8_t alpha)
{
    return (alpha * 32 + 127) / 255;
}

static long wrong_total, rounds_done, mixed_pixels;

int main(void)
{
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)rnd(0, 0xffff), layer_lut[i] = (uint16_t)rnd(0, 0xffff);

    /* buffers of whole-row halves: 8, 4 and 2 rows of DW pixels a half, and one that is the whole picture */
    static const int buffer_bytes[] = {2 * 8 * DW * 2, 2 * 4 * DW * 2, 2 * 2 * DW * 2, DW * DH * 2 + 4};
    static uint8_t   bus_buffer[DW * DH * 2 + 4];

    for (int round = 0; round < 4000; ++round) {
        dgx_screen_t          *src = dgx_vscreen_init(DW, DH, 8, DgxScreenRGB);
        dgx_screen_t          *plain = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB), *model = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
        dgx_screen_t          *layer = layer_made();
        const int              lx = rnd(-10, DW - 5), ly = rnd(-8, DH - 4);
        const uint8_t          alpha = (uint8_t)rnd(0, 255);
        dgx_bus_protocols_t    bus = {0};
        dgx_screen_with_bus_t  sbus = {0};
        bus.buffer = bus_buffer, bus.buffer_len = (uint32_t)buffer_bytes[round % 4];
        sbus.bus   = &bus;
        dgx_screen_t *lcd = &sbus.scr;
        lcd->width = DW, lcd->height = DH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
        lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;

        for (int y = 0; y < DH; ++y)
            for (int x = 0; x < DW; ++x) dgx_set_pixel(src, x, y, (uint32_t)rnd(0, 3));
        /* the model: the plain picture, the layer over it */
        dgx_vscreen8_region_to_screen16(plain, 0, 0, src, 0, 0, DW, DH, lut, 0);
        for (int y = 0; y < DH; ++y)
            for (int x = 0; x < DW; ++x) {
                const int lxx = x - lx, lyy = y - ly;
                uint16_t  c   = (uint16_t)dgx_get_pixel(plain, x, y);
                if (lxx >= 0 && lxx < LW && lyy >= 0 && lyy < LH) {
                    const uint8_t index = (uint8_t)dgx_get_pixel(layer, lxx, lyy);
                    if (index) c = mix(c, layer_lut[index], weight_of(alpha)), ++mixed_pixels;
                }
                dgx_set_pixel(model, x, y, c);
            }
        /* the bus: the same picture through the halves, the layer mixed in on the way; as a frame sends its
         * dirty rectangles, in parts that touch and parts that do not, one after another under the same layer */
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;
        if (!dgx_overlay_set(lcd, layer, lx, ly, layer_lut, alpha)) return 1;
        dgx_vscreen8_region_to_screen16(lcd, 0, 0, src, 0, 0, DW, DH, lut, 0);
        for (int part = 0; part < 5; ++part) {
            int px = rnd(0, DW - 1), py = rnd(0, DH - 1), pw = rnd(1, DW - px), ph = rnd(1, DH - py);
            if (part & 1) { /* one that touches the one before it: one dirty rectangle made of both */
                px = rnd(0, DW - 2), py = rnd(0, DH - 2), pw = rnd(1, DW - px), ph = rnd(1, DH - py);
            }
            dgx_vscreen8_region_to_screen16(lcd, px, py, src, px, py, pw, ph, lut, 0);
        }
        dgx_overlay_clear(lcd);
        dgx_overlay_forget(lcd);
        int wrong = 0;
        for (int y = 0; y < DH; ++y)
            for (int x = 0; x < DW; ++x) wrong += panel[x + y * DW] != dgx_get_pixel(model, x, y);
        if (wrong) {
            printf("whole-row halves: round %d buffer %u layer %d,%d alpha %u: %d pixels wrong\n", round, (unsigned)bus.buffer_len, lx, ly,
                   (unsigned)alpha, wrong);
            ++wrong_total;
        }
        ++rounds_done;
        dgx_screen_destroy(&src), dgx_screen_destroy(&plain), dgx_screen_destroy(&model), dgx_screen_destroy(&layer);
    }
    printf("%s: %ld rounds, %ld pixels mixed, %ld rounds wrong\n", wrong_total ? "FAIL" : "ok", rounds_done, mixed_pixels, wrong_total);
    return wrong_total != 0;
}
