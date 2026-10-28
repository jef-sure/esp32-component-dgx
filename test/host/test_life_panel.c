/*
 * The life morph demo of the JC4827W543 run on the host: the same patterns,
 * the same glow and the same screen 480x272 with the bus buffer the QSPI bus
 * has there (10424 bytes), every frame sent to a model of the panel that
 * keeps its memory the way its commands fill it, and every frame compared
 * with the same glow put into a virtual screen. A pixel of the panel that
 * differs is one the screen and the bus did not deliver where they should.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_matrix_morph.h"
#include "dgx_morph.h"
#include "dgx_morph_render.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"

#define DW 480
#define DH 272
#define CASET 0x2a
#define RASET 0x2b
#define RAMWR 0x2c

static uint8_t panel[DW * DH * 2];
static int     window_l, window_r, window_t, window_b, at_x, at_y;
static bool    window_begun; /* RAMWR has gone with pixels, and no command since */
static uint8_t pending_command;
static long    orphans;

static void put(const uint8_t *data, uint32_t bytes)
{
    for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        if (at_x < DW && at_y < DH) panel[2 * (at_x + at_y * DW)] = data[i], panel[2 * (at_x + at_y * DW) + 1] = data[i + 1];
        if (++at_x > window_r) at_x = window_l, ++at_y;
    }
}
static void bus_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    window_begun = false;
    pending_command = cmd;
    if (cmd == RAMWR) at_x = window_l, at_y = window_t;
}
static void bus_write_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t lenbits)
{
    if (pending_command == CASET) window_l = data[0] << 8 | data[1], window_r = data[2] << 8 | data[3];
    else if (pending_command == RASET) window_t = data[0] << 8 | data[1], window_b = data[2] << 8 | data[3];
    else if (pending_command == RAMWR) put(data, lenbits / 8u);
    pending_command = 0;
}
static void bus_write_data_async(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t lenbits)
{
    /* with RAMWR held the pixels begin at the window, with no command they go on, as 0x3C does */
    if (pending_command == RAMWR) {
        pending_command = 0;
        window_begun = true;
    } else if (!window_begun) {
        ++orphans;
    }
    put(data, lenbits / 8u);
}
static void bus_sync_write(struct _dgx_bus_protocols_t *bus) { pending_command = 0; }

typedef dgx_bit_matrix_t *(*seed_func_t)(void);

static dgx_bit_matrix_t *seed_from_cells(int width, int height, const int8_t cells[][2], size_t count)
{
    dgx_bit_matrix_t *life = dgx_matrix_init((uint16_t)width, (uint16_t)height);
    if (!life) return NULL;
    for (size_t i = 0; i < count; ++i) {
        dgx_matrix_set_point(life, cells[i][0], cells[i][1], true);
    }
    return life;
}

#define SEED(width, height, cells) seed_from_cells(width, height, cells, sizeof(cells) / sizeof((cells)[0]))

/* Four gliders fly in from the corners and collide into four blocks. */
static dgx_bit_matrix_t *seed_gliders(void)
{
    static const int8_t cells[][2] = {
        {1, 0}, {2, 1}, {0, 2}, {1, 2},  {2, 2}, {8, 0}, {7, 1}, {7, 2}, {8, 2}, {9, 2},
        {1, 10}, {2, 9}, {0, 8}, {1, 8}, {2, 8}, {7, 8}, {8, 8}, {9, 8}, {7, 9}, {8, 10},
    };
    return SEED(10, 11, cells);
}

/* T-tetromino: grows, oscillates and settles. */
static dgx_bit_matrix_t *seed_navy(void)
{
    static const int8_t cells[][2] = {{4, 3}, {3, 4}, {4, 4}, {5, 4}};
    return SEED(9, 9, cells);
}

/* Period 2: two blocks blinking at their touching corners. */
static dgx_bit_matrix_t *seed_beacon(void)
{
    static const int8_t cells[][2] = {{1, 1}, {2, 1}, {1, 2}, {4, 3}, {3, 4}, {4, 4}};
    return SEED(6, 6, cells);
}

/* Period 2: two shifted rows of three. */
static dgx_bit_matrix_t *seed_toad(void)
{
    static const int8_t cells[][2] = {{2, 1}, {3, 1}, {4, 1}, {1, 2}, {2, 2}, {3, 2}};
    return SEED(6, 4, cells);
}

/* Period 3. Two 3-cell arm segments, four rotations and a diagonal mirror give the whole shape. */
static dgx_bit_matrix_t *seed_pulsar(void)
{
    static const int8_t arms[2][3][2] = {
        {{-4, -6}, {-3, -6}, {-2, -6}},
        {{-1, -4}, {-1, -3}, {-1, -2}},
    };
    const int side = 15;
    const int center = side / 2;
    dgx_bit_matrix_t *life = dgx_matrix_init(side, side);
    if (!life) return NULL;
    for (int arm = 0; arm < 2; ++arm) {
        for (int i = 0; i < 3; ++i) {
            int x = arms[arm][i][0];
            int y = arms[arm][i][1];
            for (int rotation = 0; rotation < 4; ++rotation) {
                dgx_matrix_set_point(life, center + x, center + y, true);
                dgx_matrix_set_point(life, center + y, center + x, true);
                int t = x;
                x = -y;
                y = t;
            }
        }
    }
    return life;
}

/* Five cells that take a long time to settle. */
static dgx_bit_matrix_t *seed_rpentomino(void)
{
    static const int8_t cells[][2] = {{13, 9}, {14, 9}, {12, 10}, {13, 10}, {13, 11}};
    return SEED(30, 25, cells);
}

static const struct {
    const char *name;
    seed_func_t seed;
} patterns[] = {
    {"gliders", seed_gliders},
    {"navy", seed_navy},
    {"beacon", seed_beacon},
    {"toad", seed_toad},
    {"pulsar", seed_pulsar},
    {"r-pentomino", seed_rpentomino},
};

#define PATTERN_COUNT (sizeof(patterns) / sizeof(patterns[0]))
static dgx_bit_matrix_t *next_generation(const dgx_bit_matrix_t *now)
{
    dgx_bit_matrix_t *next = dgx_matrix_init(now->width, now->height);
    if (!next) return NULL;
    for (int y = 0; y < now->height; ++y) {
        for (int x = 0; x < now->width; ++x) {
            int neighbors = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    /* dgx_matrix_get_point() returns false outside the matrix */
                    if ((dx || dy) && dgx_matrix_get_point(now, x + dx, y + dy)) ++neighbors;
                }
            }
            bool alive = dgx_matrix_get_point(now, x, y);
            dgx_matrix_set_point(next, x, y, neighbors == 3 || (alive && neighbors == 2));
        }
    }
    return next;
}

static long compare(const dgx_screen_t *ref, const char *what, int frame)
{
    const uint8_t *v = ((const dgx_vscreen_t *)ref)->v_array;
    long wrong = 0;
    int  fx = -1, fy = -1, lx = 0, rx = 0;
    for (int y = 0; y < DH; ++y)
        for (int x = 0; x < DW; ++x)
            if (memcmp(panel + 2 * (x + y * DW), v + 2 * (x + y * DW), 2)) {
                if (!wrong) fx = x, fy = y, lx = rx = x;
                if (x < lx) lx = x;
                if (x > rx) rx = x;
                ++wrong;
            }
    if (wrong) printf("  %s frame %d: %ld pixels differ, first at %d,%d, columns %d..%d\n", what, frame, wrong, fx, fy, lx, rx);
    return wrong;
}

int main(int argc, char **argv)
{
    const size_t buffer_len = argc > 1 ? (size_t)atoi(argv[1]) : 10424;
    static uint8_t bus_buffer[32768];
    dgx_bus_protocols_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.bus_type = DGX_BUS_QSPI;
    bus.buffer = bus_buffer, bus.buffer_len = buffer_len;
    bus.write_command = bus_write_command, bus.write_data = bus_write_data, bus.write_data_async = bus_write_data_async;
    bus.sync_write = bus_sync_write;
    dgx_screen_with_bus_t *sbus = calloc(1, sizeof(*sbus));
    sbus->bus = &bus;
    dgx_screen_t *lcd = &sbus->scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    dgx_scr_init_slow_bus_optimized_funcs(lcd);
    dgx_screen_with_bus_init_area(sbus, CASET, RASET, RAMWR, 0, DGX_SCREEN_AREA_PROTO_STD16);
    dgx_screen_t *ref = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
    memset(panel, 0x55, sizeof(panel));

    long total = 0;
    for (size_t pattern = 0; pattern < PATTERN_COUNT; ++pattern) {
        dgx_bit_matrix_t *now = patterns[pattern].seed();
        int cell = DGX_MIN(DW / now->width, DH / now->height);
        if (cell % 2 == 0) --cell;
        dgx_morph_glow_t *ga = dgx_morph_glow_create(now->width * cell, now->height * cell, cell, 16);
        dgx_morph_glow_t *gb = dgx_morph_glow_create(now->width * cell, now->height * cell, cell, 16);
        const int ox = (DW - now->width * cell) / 2, oy = (DH - now->height * cell) / 2;
        dgx_fill_rectangle(lcd, 0, 0, DW, DH, DGX_BLACK(dgx_rgb_to_16));
        dgx_fill_rectangle(ref, 0, 0, DW, DH, DGX_BLACK(dgx_rgb_to_16));
        long wrong = compare(ref, "fill", 0);
        printf("%s: %dx%d cells of %d px, window %dx%d at %d,%d\n", patterns[pattern].name, now->width, now->height, cell, now->width * cell,
               now->height * cell, ox, oy);
        int frame = 0;
        for (int generation = 0; generation < 20; ++generation) {
            dgx_bit_matrix_t *next = next_generation(now);
            dgx_morph_t *ma = dgx_morph_create(now, next, dgx_morph_sources_life, NULL);
            dgx_morph_t *mb = dgx_morph_create(now, next, dgx_morph_sources_life, NULL);
            for (int f = 0; f <= 16; ++f, ++frame) {
                const float t = f / 16.0f;
                dgx_morph_draw(ma, t, 0, 0, cell, true, dgx_morph_glow_dot, ga);
                dgx_morph_glow_present(ga, t, lcd, ox, oy);
                dgx_morph_draw(mb, t, 0, 0, cell, true, dgx_morph_glow_dot, gb);
                dgx_morph_glow_present(gb, t, ref, ox, oy);
                wrong += compare(ref, patterns[pattern].name, frame);
            }
            dgx_morph_destroy(&ma);
            dgx_morph_destroy(&mb);
            dgx_matrix_destroy(&now);
            now = next;
        }
        total += wrong;
        dgx_matrix_destroy(&now);
        dgx_morph_glow_destroy(&ga);
        dgx_morph_glow_destroy(&gb);
    }
    printf("%s: buffer %u, %ld pixels wrong in all, %ld pieces with no window begun\n", total || orphans ? "FAIL" : "ok", (unsigned)buffer_len, total, orphans);
    dgx_screen_destroy(&ref);
    free(sbus);
    return total != 0 || orphans != 0;
}
