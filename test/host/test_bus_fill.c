#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"

/*
 * A rectangle filled on a display behind a bus: the buffer of the bus is
 * filled once and sent as many times as the rectangle needs, what is left
 * over after whole buffers first. Every depth a bus screen fills, buffers of
 * many lengths, rectangles cut by the edges. The bytes the display gets in
 * its window have to be those of the pixels one after another, the pieces no
 * more than whole buffers and the leftover take, and the leftover first
 * where it ends on a whole byte.
 */

#define W 61
#define H 37

static uint32_t seed = 2718;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

/*
 * The display behind the bus. As a bus that sends by DMA, it reads the bytes
 * of a piece only when it is done with it: when the next piece, a window or a
 * wait comes, or, for a piece queued behind another, when that one is queued
 * behind in turn. A piece written over before it has gone shows up as wrong
 * bytes.
 */
static uint8_t        got[W * H * 4 + 4];
static uint32_t       got_len;
static int            windows, pieces;
static uint32_t       first_piece;
static const uint8_t *pending[2];
static uint32_t       pending_bits[2];
static int            pendings;
static bool           queueing;

static void take_oldest(void)
{
    const uint32_t len = pending_bits[0] / 8u;
    CHECK(pending_bits[0] % 8u == 0 && got_len + len <= sizeof(got));
    if (got_len + len <= sizeof(got)) memcpy(got + got_len, pending[0], len);
    got_len += len;
    pending[0] = pending[1], pending_bits[0] = pending_bits[1];
    --pendings;
}

static void take(void)
{
    while (pendings) take_oldest();
}

static void model_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
    take();
    ++windows;
}

static void model_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    (void)scr;
    if (!queueing) take();
    pending[pendings] = data, pending_bits[pendings] = lenbits;
    ++pendings;
    if (!pieces++) first_piece = lenbits / 8u;
    while (pendings > 1) take_oldest();
}

static void model_wait(dgx_screen_t *scr)
{
    (void)scr;
    take();
}

static void quiet(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

static int clip(int at, int len, int size, int *from)
{
    int to = at + len;
    if (at < 0) at = 0;
    if (to > size) to = size;
    *from = at;
    return to > at ? to - at : 0;
}

int main(void)
{
    static const uint8_t depths[] = {1, 4, 8, 12, 16, 18, 24, 32};
    static uint8_t       buffer[600], want[W * H * 4 + 4];
    int                  leftover_first = 0, leftover_last = 0;
    for (int round = 0; round < 20000; ++round) {
        const uint8_t bits = depths[round % sizeof(depths)];
        /* any length the buffer of a bus may have, of whole bytes */
        const size_t len = (size_t)rnd(8, sizeof(buffer));
        queueing         = (round >> 3) & 1;

        dgx_bus_protocols_t   bus  = {.buffer = buffer, .buffer_len = len};
        dgx_screen_with_bus_t sbus = {.bus = &bus};
        dgx_screen_t         *lcd  = &sbus.scr;
        lcd->width = W, lcd->height = H, lcd->color_bits = bits, lcd->screen_subtype = DgxPhysicalScreenWithBus;
        dgx_scr_init_slow_bus_optimized_funcs(lcd);
        lcd->set_area = model_set_area, lcd->write_area = model_write_area, lcd->wait_buffer = model_wait;
        lcd->update_screen = quiet;

        const int      x = rnd(-10, W), y = rnd(-10, H), w = rnd(0, W + 10), h = rnd(0, H + 10);
        const uint32_t color = (seed = seed * 1664525u + 1013904223u) & (bits == 32 ? 0xffffffffu : (1u << bits) - 1u);
        int            cx, cy;
        const uint32_t n = (uint32_t)clip(x, w, W, &cx) * (uint32_t)clip(y, h, H, &cy);

        memset(buffer, 0xA5, sizeof(buffer));
        got_len = 0, windows = pieces = 0, pendings = 0;
        dgx_fill_rectangle(lcd, x, y, w, h, color);
        take();
        if (n == 0) {
            CHECK(pieces == 0 && windows == 0);
            continue;
        }

        /* the bytes of n pixels one after another, a last one cut short only at the very end */
        memset(want, 0, sizeof(want));
        dgx_fill_pixels(bits, want, 0, n, color);
        const uint32_t whole = n * dgx_color_points_to_bytes(bits, 8u) / 8u;
        CHECK(windows == 1);
        CHECK(got_len == dgx_color_points_to_bytes(bits, n));
        CHECK(memcmp(got, want, whole) == 0);

        /* as many pieces as whole buffers and the leftover, the leftover first if it ends on a byte */
        uint32_t pic = dgx_bytes_to_color_points(bits, (uint32_t)len);
        if (pic > n) pic = n;
        /* a buffer sent more than once ends on a whole byte: 12-bit pixels go by pairs */
        if (pic < n && bits == 12) pic &= ~1u;
        const uint32_t rest = n % pic;
        CHECK(pieces == (int)((n + pic - 1) / pic));
        if (rest && rest * dgx_color_points_to_bytes(bits, 8u) % 8u == 0) {
            CHECK(first_piece == dgx_color_points_to_bytes(bits, rest));
            ++leftover_first;
        } else {
            CHECK(first_piece == dgx_color_points_to_bytes(bits, pic));
            leftover_last += rest != 0;
        }
    }
    /* both ways came up often enough to mean something */
    CHECK(leftover_first > 2000 && leftover_last > 200);
    CHECK_DONE();
}
