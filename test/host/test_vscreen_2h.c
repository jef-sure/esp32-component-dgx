/*
 * The two-panel screen: a virtual screen as wide as two displays side by
 * side, each behind a bus of its own. What is drawn into it has to come out
 * on the panels as it lies in it, whatever the sizes of the two: rows that
 * cross both go through the buses side by side when the panels are alike,
 * and every rectangle to each panel by itself otherwise. Two panels on one
 * SPI host cannot go side by side: a bus held for one panel keeps the other
 * waiting for ever, in the same task.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen_2h.h"
#include "vscreen/vscreen_2h.c"

#define CASET 0x2a
#define RASET 0x2b
#define RAMWR 0x2c

static uint32_t seed = 1729;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

/* a panel and its bus, the bus first: the screen of the panel is given the bus, and the bus is the panel */
typedef struct {
    dgx_bus_protocols_t bus;
    int                 width, height;
    uint16_t           *memory;
    int                 left, right, top, bottom, x, y; /* the window, and where the next pixel goes */
    uint8_t             command;                        /* sent, its parameters not yet */
    uint8_t buffer[64] __attribute__((aligned(4)));
} panel_t;

/* what the two buses share when they lie on one host, the panel that holds it and how many times the other waited for ever */
static const char one_host = 0;
static const panel_t *holder;
static int            hangs;

static void panel_hold(dgx_bus_protocols_t *bus)
{
    const panel_t *p = (const panel_t *)bus;
    if (bus->wire != &one_host) return;
    if (holder && holder != p) ++hangs;
    holder = p;
}

static void panel_release(dgx_bus_protocols_t *bus)
{
    if (holder == (const panel_t *)bus) holder = NULL;
}

/* pixels into the window, as a controller takes them: at its end it starts again at its top */
static void panel_pixels(panel_t *p, const uint8_t *data, uint32_t bytes)
{
    for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        if (p->x < p->width && p->y < p->height) p->memory[p->x + p->y * p->width] = (uint16_t)(data[i] << 8 | data[i + 1]);
        if (++p->x > p->right) {
            p->x = p->left;
            if (++p->y > p->bottom) p->y = p->top;
        }
    }
}

static void panel_command(dgx_bus_protocols_t *bus, uint8_t command)
{
    panel_t *p  = (panel_t *)bus;
    p->command  = command;
    if (command == RAMWR) p->x = p->left, p->y = p->top;
}

static void panel_data(dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t lenbits)
{
    panel_t *p = (panel_t *)bus;
    if (p->command == CASET) p->left = data[0] << 8 | data[1], p->right = data[2] << 8 | data[3];
    else if (p->command == RASET) p->top = data[0] << 8 | data[1], p->bottom = data[2] << 8 | data[3];
    else panel_pixels(p, data, lenbits / 8u);
    p->command = 0;
}

static void panel_data_async(dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t lenbits)
{
    panel_pixels((panel_t *)bus, data, lenbits / 8u);
}

static void panel_sync(dgx_bus_protocols_t *bus)
{
    (void)bus;
}

static dgx_screen_t *panel_screen(panel_t *p, int width, int height)
{
    memset(p, 0, sizeof(*p));
    p->width = width, p->height = height;
    p->memory           = calloc((size_t)width * (size_t)height, sizeof(uint16_t));
    p->bus.bus_type     = DGX_BUS_QSPI;
    p->bus.buffer       = p->buffer, p->bus.buffer_len = sizeof(p->buffer);
    p->bus.write_command = panel_command, p->bus.write_data = panel_data, p->bus.write_data_async = panel_data_async;
    p->bus.sync_write = panel_sync;
    dgx_screen_with_bus_t *sbus = calloc(1, sizeof(*sbus));
    sbus->bus                   = &p->bus;
    sbus->scr.width = width, sbus->scr.height = height, sbus->scr.color_bits = 16, sbus->scr.screen_subtype = DgxPhysicalScreenWithBus;
    dgx_scr_init_slow_bus_optimized_funcs(&sbus->scr);
    dgx_screen_with_bus_init_area(sbus, CASET, RASET, RAMWR, 0, DGX_SCREEN_AREA_PROTO_STD16);
    return &sbus->scr;
}

/* the pixels of the panel that are not those of the two-panel screen at its place, `x0` its first column */
static int panel_wrong(const panel_t *p, dgx_screen_t *both, int x0)
{
    int wrong = 0;
    for (int y = 0; y < p->height; ++y)
        for (int x = 0; x < p->width; ++x) wrong += p->memory[x + y * p->width] != (uint16_t)dgx_get_pixel(both, x0 + x, y);
    return wrong;
}

int main(void)
{
    /* of one size, of one width and two heights the one way and the other, and of two widths */
    static const int sizes[][4] = {{40, 30, 40, 30}, {40, 30, 40, 40}, {40, 40, 40, 30}, {30, 30, 40, 30}};
    for (int run = 0; run < 8; ++run) {
        const int     kind = run % 4;
        panel_t       lm, rm;
        dgx_screen_t *left = panel_screen(&lm, sizes[kind][0], sizes[kind][1]), *right = panel_screen(&rm, sizes[kind][2], sizes[kind][3]);
        dgx_screen_t *both = dgx_vscreen_2h_init(left, right);
        CHECK(both && both->width == left->width + right->width);
        lm.bus.hold = rm.bus.hold = panel_hold, lm.bus.release = rm.bus.release = panel_release;
        /* a host each, then one host */
        lm.bus.wire = run < 4 ? (const void *)&lm : &one_host, rm.bus.wire = run < 4 ? (const void *)&rm : &one_host;
        int wrong = 0;
        for (int round = 0; round < 300; ++round) {
            /* bands across both panels, every row of a color of its own, and rectangles anywhere */
            dgx_screen_progress_up(both);
            if (round % 2) {
                const int top = rnd(0, both->height - 1), bottom = rnd(top, both->height - 1);
                for (int y = top; y <= bottom; ++y) dgx_fill_rectangle(both, 0, y, both->width, 1, (uint32_t)rnd(0, 0xffff));
            } else {
                const int x = rnd(-5, both->width), y = rnd(-5, both->height);
                dgx_fill_rectangle(both, x, y, rnd(1, both->width), rnd(1, both->height), (uint32_t)rnd(0, 0xffff));
            }
            dgx_screen_progress_down(both);
            wrong += panel_wrong(&lm, both, 0) + panel_wrong(&rm, both, left->width);
        }
        if (wrong) fprintf(stderr, "two panels of %dx%d and %dx%d: %d pixels wrong\n", sizes[kind][0], sizes[kind][1], sizes[kind][2], sizes[kind][3], wrong);
        CHECK(wrong == 0);
        if (hangs) fprintf(stderr, "two panels of %dx%d and %dx%d on one host: %d waits for ever\n", sizes[kind][0], sizes[kind][1], sizes[kind][2], sizes[kind][3], hangs);
        CHECK(hangs == 0);
        hangs = 0, holder = NULL;
        dgx_screen_destroy(&both);
        free(left), free(right), free(lm.memory), free(rm.memory);
    }
    CHECK_DONE();
}
