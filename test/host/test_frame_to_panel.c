/*
 * The way a frame goes to a panel behind a QSPI bus: a window set through
 * dgx_scr_set_area_sb (the CASET/RASET/RAMWR commands of the screen), then
 * the pixels of the halves of the bus buffer, all through the same
 * functions a board runs -- with a bus that holds a command back until its
 * data come, as the QSPI bus does, and counts where every pixel lands in the
 * memory of the panel. Pixels with RAMWR held start at the window; pixels
 * with no command held go on where the ones before ended, as 0x3C does on
 * the panel. Whatever rectangles come, one after another under the same
 * overlay layer, the pixels land where the model says; RAMWR never goes
 * alone ahead of its pixels, and pixels never go on with no window begun
 * since the last command.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_draw.h"
#include "dgx_overlay.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"

static uint32_t seed = 4242;

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
#define CASET 0x2a
#define RASET 0x2b
#define RAMWR 0x2c
#define RAMWRC 0x3c

static uint16_t lut[256], layer_lut[256];
static uint16_t panel[DW * DH];

/* the panel's memory and window, filled the way its controller does */
static int window_l, window_r, window_t, window_b;
static int at_x, at_y;
static int   ramwr_bare;     /* RAMWR was sent alone, with no pixels */
static int   orphans;        /* pixels went on with no window begun since the last command */
static int   windows;        /* pieces that began a window, with RAMWR */
static int   continued;      /* pieces that went on where the one before ended, with 0x3C */
static bool  window_begun;   /* RAMWR has gone with pixels, and no command since */
static const uint8_t *in_place_from, *in_place_to; /* the pixels of the source: pieces from there came in place */
static int   in_place;       /* pieces that came straight from the source, not through the bus buffer */

/* the model's DMA reaches everything */
static bool bus_sends_from(struct _dgx_bus_protocols_t *bus, const void *pixels)
{
    (void)bus, (void)pixels;
    return true;
}
static uint8_t pending_command;

static void bus_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    window_begun = false;
    pending_command = cmd;
    if (cmd == RAMWR) {
        /* RAMWR starts at the window: the panel's counter goes back to its first pixel */
        at_x = window_l, at_y = window_t;
    }
}

static void bus_write_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t lenbits)
{
    (void)bus;
    const uint32_t bytes = lenbits / 8u;
    if (pending_command == CASET) {
        window_l = data[0] << 8 | data[1], window_r = data[2] << 8 | data[3];
    } else if (pending_command == RASET) {
        window_t = data[0] << 8 | data[1], window_b = data[2] << 8 | data[3];
    } else if (pending_command == RAMWR) {
        for (uint32_t i = 0; i + 1 < bytes; i += 2) {
            if (at_x < DW && at_y < DH) panel[at_x + at_y * DW] = (uint16_t)(data[i] << 8 | data[i + 1]);
            if (++at_x > window_r) at_x = window_l, ++at_y;
        }
    }
    pending_command = 0;
}

static void bus_write_data_async(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t lenbits)
{
    if (data >= in_place_from && data < in_place_to) ++in_place;
    /* pixels: with RAMWR held back they begin at the window, with no command they go on */
    if (pending_command == RAMWR) {
        bus_write_data(bus, data, lenbits);
        window_begun = true;
        ++windows;
        return;
    }
    if (!window_begun) ++orphans;
    ++continued;
    for (uint32_t i = 0; i + 1 < lenbits / 8u; i += 2) {
        if (at_x < DW && at_y < DH) panel[at_x + at_y * DW] = (uint16_t)(data[i] << 8 | data[i + 1]);
        if (++at_x > window_r) at_x = window_l, ++at_y;
    }
}

static void bus_sync_write(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
    if (pending_command == RAMWR) ++ramwr_bare;
    pending_command = 0;
}

static dgx_bus_protocols_t bus;

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

static uint16_t mix(uint16_t color, uint16_t over, int weight)
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

static long wrong_total, rounds_done;

int main(void)
{
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)rnd(0, 0xffff), layer_lut[i] = (uint16_t)rnd(0, 0xffff);
    static uint8_t bus_buffer[2 * 4 * DW * 2]; /* halves of four rows each */

    for (int round = 0; round < 20000; ++round) {
        dgx_screen_t          *src = dgx_vscreen_init(DW, DH, 8, DgxScreenRGB);
        dgx_screen_t          *model = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
        dgx_screen_t          *layer = layer_made();
        const int              lx = rnd(-10, DW - 5), ly = rnd(-8, DH - 4);
        const uint8_t          alpha = (uint8_t)rnd(0, 255);

        memset(&bus, 0, sizeof(bus));
        bus.bus_type = DGX_BUS_QSPI;
        bus.buffer = bus_buffer, bus.buffer_len = sizeof(bus_buffer);
        bus.write_command = bus_write_command;
        bus.write_data = bus_write_data;
        bus.write_data_async = bus_write_data_async;
        bus.sync_write = bus_sync_write;
        window_begun = false;

        dgx_screen_with_bus_t *sbus = (dgx_screen_with_bus_t *)calloc(1, sizeof(dgx_screen_with_bus_t));
        sbus->bus = &bus;
        dgx_screen_t *lcd = &sbus->scr;
        lcd->width = DW, lcd->height = DH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
        dgx_scr_init_slow_bus_optimized_funcs(lcd);
        dgx_screen_with_bus_init_area(sbus, CASET, RASET, RAMWR, 0, DGX_SCREEN_AREA_PROTO_STD16);

        for (int y = 0; y < DH; ++y)
            for (int x = 0; x < DW; ++x) dgx_set_pixel(src, x, y, (uint32_t)rnd(0, 3));
        /* the model */
        for (int y = 0; y < DH; ++y)
            for (int x = 0; x < DW; ++x) {
                const int lxx = x - lx, lyy = y - ly;
                uint16_t  c   = lut[(uint8_t)dgx_get_pixel(src, x, y)];
                if (lxx >= 0 && lxx < LW && lyy >= 0 && lyy < LH) {
                    const uint8_t index = (uint8_t)dgx_get_pixel(layer, lxx, lyy);
                    if (index) c = mix(c, layer_lut[index], weight_of(alpha));
                }
                dgx_set_pixel(model, x, y, c);
            }
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;

        if (!dgx_overlay_set(lcd, layer, lx, ly, layer_lut, alpha)) return 1;
        /* the frame: the whole picture once, then parts of it again, as dirty rectangles come */
        dgx_vscreen8_region_to_screen16(lcd, 0, 0, src, 0, 0, DW, DH, lut, 0);
        for (int part = 0; part < 5; ++part) {
            const int px = rnd(0, DW - 1), py = rnd(0, DH - 1);
            const int pw = rnd(1, DW - px), ph = rnd(1, DH - py);
            dgx_vscreen8_region_to_screen16(lcd, px, py, src, px, py, pw, ph, lut, 0);
        }
        dgx_overlay_clear(lcd);
        dgx_overlay_forget(lcd);

        int wrong = 0;
        for (int y = 0; y < DH; ++y)
            for (int x = 0; x < DW; ++x) wrong += panel[x + y * DW] != dgx_get_pixel(model, x, y);
        if (wrong) {
            printf("frame to panel: round %d layer %d,%d alpha %u: %d pixels wrong\n", round, lx, ly, (unsigned)alpha, wrong);
            ++wrong_total;
        }
        ++rounds_done;
        dgx_screen_destroy(&src);
        dgx_screen_destroy(&model);
        dgx_screen_destroy(&layer);
        free(sbus);
    }
    /*
     * A bus that sends a buffer where it lies: frames of a 16-bit virtual
     * screen with no overlay go in place when their rows lie one after
     * another, whole rows or a single one, and through the halves otherwise.
     */
    long in_place_wrong = 0, in_place_rounds = 0;
    int  in_place_expected = 0;
    for (int round = 0; round < 5000; ++round) {
        dgx_screen_t *src = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
        for (int y = 0; y < DH; ++y)
            for (int x = 0; x < DW; ++x) dgx_set_pixel(src, x, y, (uint32_t)rnd(0, 0xffff));
        memset(&bus, 0, sizeof(bus));
        bus.bus_type = DGX_BUS_QSPI;
        bus.buffer = bus_buffer, bus.buffer_len = sizeof(bus_buffer);
        bus.write_command = bus_write_command;
        bus.write_data = bus_write_data;
        bus.write_data_async = bus_write_data_async;
        bus.sync_write = bus_sync_write;
        bus.sends_from = bus_sends_from;
        window_begun = false;
        in_place_from = ((dgx_vscreen_t *)src)->v_array, in_place_to = in_place_from + DW * DH * 2;
        dgx_screen_with_bus_t *sbus = (dgx_screen_with_bus_t *)calloc(1, sizeof(dgx_screen_with_bus_t));
        sbus->bus = &bus;
        dgx_screen_t *lcd = &sbus->scr;
        lcd->width = DW, lcd->height = DH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
        dgx_scr_init_slow_bus_optimized_funcs(lcd);
        dgx_screen_with_bus_init_area(sbus, CASET, RASET, RAMWR, 0, DGX_SCREEN_AREA_PROTO_STD16);
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;
        /* the whole frame, then parts: whole rows, a single row, and any rectangle */
        dgx_vscreen_to_screen(lcd, 0, 0, src, 0u);
        ++in_place_expected;
        for (int part = 0; part < 6; ++part) {
            const int kind = part % 3;
            const int py = rnd(0, DH - 1), ph = kind == 1 ? 1 : rnd(1, DH - py);
            const int px = kind == 0 ? 0 : rnd(0, DW - 1), pw = kind == 0 ? DW : rnd(1, DW - px);
            for (int y = py; y < py + ph; ++y)
                for (int x = px; x < px + pw; ++x) panel[x + y * DW] = MARK;
            dgx_vscreen_region_to_screen(lcd, px, py, src, px, py, pw, ph, 0u);
            if (kind != 2 || pw == DW || ph == 1) ++in_place_expected;
        }
        int wrong = 0;
        for (int y = 0; y < DH; ++y)
            for (int x = 0; x < DW; ++x) wrong += panel[x + y * DW] != dgx_get_pixel(src, x, y);
        if (wrong) {
            printf("in place: round %d: %d pixels wrong\n", round, wrong);
            ++in_place_wrong;
        }
        ++in_place_rounds;
        dgx_screen_destroy(&src);
        free(sbus);
    }
    in_place_from = in_place_to = NULL;
    if (in_place != in_place_expected) printf("in place: %d pieces came from the source, %d rectangles were to come so\n", in_place, in_place_expected);
    printf("%s: in place, %ld rounds, %ld wrong, %d rectangles sent where they lie\n", in_place_wrong || in_place != in_place_expected ? "FAIL" : "ok",
           in_place_rounds, in_place_wrong, in_place);

    const bool bad = wrong_total || ramwr_bare || orphans || !continued || in_place_wrong || in_place != in_place_expected;
    printf("%s: %ld rounds, %ld wrong, %d windows begun with RAMWR, %d pieces going on, %d bare RAMWRs, %d pieces with no window begun\n",
           bad ? "FAIL" : "ok", rounds_done, wrong_total, windows, continued, ramwr_bare, orphans);
    return bad;
}
