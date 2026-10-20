/*
 * A turned virtual screen put to a display behind a bus goes through one
 * window, row after row of the display. It has to put the pixels that the
 * same call puts into a virtual screen, where they are set one by one.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"

#define DW 40
#define DH 30
#define SW 17
#define SH 11

static uint32_t panel[DW * DH];
static int      panel_bytes;
static int      area_l, area_r, area_t, area_b, area_x, area_y;
static long     windows, single_pixels, sent_pixels;

/*
 * The bytes of a piece are read only when the bus is done with it, as a bus that sends by DMA reads them:
 * when the next piece, a window or a wait comes, or, for a piece queued behind another, when that one is
 * queued behind in turn.
 */
static const uint8_t *pending[2];
static uint32_t       pending_bits[2];
static int            pendings;
static long           queued_pieces;

static void panel_take_oldest(void)
{
    const uint8_t *data = pending[0];
    const uint32_t bits = pending_bits[0];
    pending[0] = pending[1], pending_bits[0] = pending_bits[1];
    --pendings;
    for (uint32_t i = 0; i + panel_bytes <= bits / 8; i += panel_bytes) {
        uint32_t color = 0;
        for (int k = 0; k < panel_bytes; ++k) color = color << 8 | data[i + k];
        CHECK(area_y <= area_b);
        if (area_y <= area_b) panel[area_x + area_y * DW] = color;
        ++sent_pixels;
        if (++area_x > area_r) area_x = area_l, ++area_y;
    }
}

static void panel_take(void)
{
    while (pendings) panel_take_oldest();
}

static void panel_piece(const uint8_t *data, uint32_t lenbits)
{
    CHECK(lenbits % (8u * panel_bytes) == 0 && pendings < 2);
    pending[pendings] = data, pending_bits[pendings] = lenbits;
    ++pendings;
}


static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    panel_take();
    CHECK(left <= right && top <= bottom && right < DW && bottom < DH);
    area_l = left, area_r = right, area_t = top, area_b = bottom, area_x = left, area_y = top;
    ++windows;
}

/* a bus that waits for the piece on its way before it takes the next, or one that queues the next behind it */
static bool queueing;

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    (void)scr;
    if (!queueing) panel_take();
    panel_piece(data, lenbits);
    /* either way only this piece is on its way when it returns */
    while (pendings > 1) panel_take_oldest();
    queued_pieces += queueing;
}

static void panel_wait(dgx_screen_t *scr)
{
    (void)scr;
    panel_take();
}

static void panel_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr;
    panel_take();
    ++single_pixels;
    if (x >= 0 && y >= 0 && x < DW && y < DH) panel[x + y * DW] = color;
}

static void panel_update(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

static uint32_t seed = 777;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

int main(void)
{
    static const int depths[] = {16, 24, 18, 8};
    static const int buffers[] = {64, 37, 9, 4};
    static uint8_t   bus_buffer[64];
    for (int round = 0; round < 6000; ++round) {
        const int bits = depths[round % 4];
        panel_bytes = bits == 16 ? 2 : bits == 8 ? 1 : 3;
        const uint32_t mark = bits == 8 ? 0x5a : 0x1234;
        dgx_bus_protocols_t   bus = {.buffer = bus_buffer, .buffer_len = (size_t)buffers[rnd(0, 3)]};
        /* a bus that queues a piece behind the one on its way, in half the rounds */
        queueing = (round >> 2) & 1;
        dgx_screen_with_bus_t sbus = {.bus = &bus};
        dgx_screen_t         *lcd = &sbus.scr;
        lcd->width = DW, lcd->height = DH, lcd->color_bits = (uint8_t)bits, lcd->screen_subtype = DgxPhysicalScreenWithBus;
        lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;
        lcd->set_pixel = panel_set_pixel, lcd->update_screen = panel_update;
        dgx_screen_t *src = dgx_vscreen_init(SW, SH, (uint8_t)bits, DgxScreenRGB), *model = dgx_vscreen_init(DW, DH, (uint8_t)bits, DgxScreenRGB);
        CHECK(src && model);
        for (int y = 0; y < SH; ++y) {
            for (int x = 0; x < SW; ++x) dgx_set_pixel(src, x, y, bits == 8 ? (uint32_t)rnd(0, 255) : (uint32_t)rnd(1, bits == 16 ? 0xffff : 0xffffff));
        }
        dgx_fill_rectangle(model, 0, 0, DW, DH, mark);
        for (int i = 0; i < DW * DH; ++i) panel[i] = mark;

        const dgx_output_orientation_t turn = (dgx_output_orientation_t)(1 + round % 7);
        const int x = rnd(-SW - 3, DW + 3), y = rnd(-SW - 3, DH + 3);
        const bool whole = round % 5 == 0;
        const int left = whole ? 0 : rnd(-4, SW + 3), top = whole ? 0 : rnd(-4, SH + 3);
        const int width = whole ? SW : rnd(-1, SW + 6), height = whole ? SH : rnd(-1, SH + 6);
        windows = single_pixels = sent_pixels = 0;
        if (whole) {
            dgx_vscreen_to_screen(lcd, x, y, src, DGX_VSCREEN_ORIENTED(turn));
            dgx_vscreen_to_screen(model, x, y, src, DGX_VSCREEN_ORIENTED(turn));
        } else {
            dgx_vscreen_region_to_screen(lcd, x, y, src, left, top, width, height, DGX_VSCREEN_ORIENTED(turn));
            dgx_vscreen_region_to_screen(model, x, y, src, left, top, width, height, DGX_VSCREEN_ORIENTED(turn));
        }
        int wrong = 0, drawn = 0;
        for (int py = 0; py < DH; ++py) {
            for (int px = 0; px < DW; ++px) {
                const uint32_t want = dgx_get_pixel(model, px, py);
                wrong += panel[px + py * DW] != want;
                drawn += want != mark;
            }
        }
        CHECK(wrong == 0);
        /* through a window: no pixel is sent on its own, the window is set once, and no more pixels go than lie on the display */
        CHECK(single_pixels == 0);
        CHECK(windows <= 1 && (windows == 1) == (sent_pixels > 0));
        CHECK(sent_pixels >= drawn && sent_pixels <= DW * DH);
        dgx_screen_destroy(&src);
        dgx_screen_destroy(&model);
        if (wrong) break;
    }
    CHECK(queued_pieces > 1000);
    CHECK_DONE();
}
