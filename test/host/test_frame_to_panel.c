/*
 * The way a frame goes to a panel behind a QSPI bus: a window set through
 * dgx_scr_set_area_sb (the CASET/RASET/RAMWR commands of the screen), then
 * the pixels of the halves of the bus buffer, all through the same
 * functions a board runs -- with a bus that holds a command back until its
 * data come and holds the chip select over the pixels of a window, as the
 * QSPI bus does, and counts where every pixel lands in the memory of the
 * panel. Whatever rectangles come, one after another under the same overlay
 * layer, the pixels land where the model says; pixels never come with no
 * window begun (the 0x3C the NV3041A does not take after the chip select
 * has risen), and the selection ends exactly at the last pixel of a window.
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
static int   ramwr_bare;     /* RAMWR came and no pixel followed it */
static int   ramwrc_used;    /* pixels came with no window begun: 0x3C after the chip select rose */
static int   ends_early;     /* the selection ended before the last pixel of its window */
static int   selections;     /* windows sent in one selection each */
static bool  selected;       /* the chip select is held over the pixels of a window */
static uint8_t pending_command;

static void bus_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    /* a command goes in a selection of its own: a window begun ends */
    selected = false;
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
    /* pixels: with RAMWR held back a selection begins at the window; in a selection they go on */
    if (pending_command == RAMWR) {
        bus_write_data(bus, data, lenbits);
        selected = true;
        ++selections;
        return;
    }
    if (!selected) ++ramwrc_used;
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

/* the screen says the window has all its pixels: the counter of the panel stands past its last one */
static void bus_end_pixels(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
    if (!selected || at_x != window_l || at_y != window_b + 1) ++ends_early;
    selected = false;
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
        bus.end_pixels = bus_end_pixels;
        selected = false;

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
    const bool bad = wrong_total || ramwr_bare || ramwrc_used || ends_early;
    printf("%s: %ld rounds, %ld wrong, %d windows in one selection each, %d bare RAMWRs flushed, %d pieces with no window begun, %d selections ended early\n",
           bad ? "FAIL" : "ok", rounds_done, wrong_total, selections, ramwr_bare, ramwrc_used, ends_early);
    return bad;
}
