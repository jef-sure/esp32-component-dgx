/*
 * A picture kept in read-only memory, as a `const` array in flash is on a
 * chip, under a virtual screen of dgx_vscreen_from_array() that is only
 * read: the source of every kind of transfer, of a copy, a clone and a
 * texture. Each has to put what the same picture in RAM puts, and nothing
 * may write to the picture: its memory is read-only here too, so a write
 * stops the test. A display behind a bus must get it through the buffer of
 * the bus, never where it lies: a bus sends by DMA only from RAM.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus/dgx_fb_area_priv.h"
#include "check.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "dgx_vscreen.h"

#define DW 40
#define DH 30
#define SW 9
#define SH 7

/*
 * The pictures, SW by SH pixels as a virtual screen keeps them: rows one
 * after another, the high byte of a pixel first. Each lies after a byte that
 * is not of it, so it starts neither on a word nor on a whole pixel of two
 * bytes: read-only memory is read byte by byte, wherever it starts.
 */
static const uint8_t PICTURE_8[1 + 63] = {
    0xee, /* not of the picture */
    0x00, 0x02, 0xff, 0x01, 0xff, 0x00, 0xff, 0x02, 0xc8, 0x01, 0x00, 0xc8, 0xc8, 0xc8, 0x02, 0x02, 0x02, 0xff,
    0x01, 0x02, 0x03, 0x02, 0xc8, 0xff, 0x02, 0x00, 0x01, 0x03, 0x01, 0xff, 0xc8, 0x01, 0x00, 0x02, 0x01, 0x00,
    0x03, 0x01, 0x03, 0x01, 0xff, 0x02, 0x01, 0x01, 0xc8, 0x02, 0x01, 0x02, 0x01, 0x00, 0x01, 0xc8, 0xff, 0xc8,
    0x03, 0xc8, 0x01, 0x03, 0x03, 0x03, 0x02, 0xff, 0xff,
};

static const uint8_t PICTURE_16[1 + 126] = {
    0xee, /* not of the picture */
    0xf8, 0x00, 0x07, 0xe0, 0x00, 0x1f, 0xab, 0xcd, 0x00, 0x00, 0xab, 0xcd, 0x12, 0x34, 0xff, 0xff, 0x12, 0x34,
    0x12, 0x34, 0xab, 0xcd, 0x00, 0x1f, 0xf8, 0x00, 0xab, 0xcd, 0x00, 0x1f, 0xff, 0xff, 0x07, 0xe0, 0x07, 0xe0,
    0xf8, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x1f, 0x00, 0x1f, 0xff, 0xff, 0x12, 0x34, 0xf8, 0x00, 0x12, 0x34,
    0x00, 0x00, 0xab, 0xcd, 0xab, 0xcd, 0x12, 0x34, 0xab, 0xcd, 0x12, 0x34, 0x00, 0x1f, 0x07, 0xe0, 0xf8, 0x00,
    0x00, 0x1f, 0x12, 0x34, 0xab, 0xcd, 0x00, 0x1f, 0xf8, 0x00, 0xff, 0xff, 0x07, 0xe0, 0xff, 0xff, 0xab, 0xcd,
    0x07, 0xe0, 0x07, 0xe0, 0xf8, 0x00, 0xab, 0xcd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xab, 0xcd, 0xf8, 0x00,
    0x12, 0x34, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0xf8, 0x00, 0x00, 0x1f, 0xab, 0xcd,
};

static const uint8_t PICTURE_24[1 + 189] = {
    0xee, /* not of the picture */
    0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x12, 0x34, 0x56, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xfc, 0xb8, 0xa4, 0xfc, 0xb8, 0xa4, 0x12, 0x34, 0x56, 0x00, 0xff, 0x00, 0x12, 0x34, 0x56,
    0xfc, 0xb8, 0xa4, 0xfc, 0xb8, 0xa4, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0xfc, 0xb8, 0xa4, 0xff, 0x00, 0x00,
    0xfc, 0xb8, 0xa4, 0xfc, 0xb8, 0xa4, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xb8, 0xa4, 0xfc, 0xb8, 0xa4,
    0x00, 0x00, 0x00, 0xfc, 0xb8, 0xa4, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x12, 0x34, 0x56,
    0x00, 0x00, 0xff, 0x12, 0x34, 0x56, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x12, 0x34, 0x56,
    0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x00, 0xff, 0x00, 0x12, 0x34, 0x56, 0xff, 0xff, 0xff, 0x12, 0x34, 0x56,
    0x12, 0x34, 0x56, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56,
};

static const uint8_t PICTURE_1[1 + 8] = {
    0xee, /* not of the picture */
    0x8a, 0x94, 0x53, 0x59, 0x5a, 0xc1, 0x5d, 0x83,
};

static const uint8_t PICTURE_4[1 + 32] = {
    0xee, /* not of the picture */
    0xdd, 0xc5, 0x90, 0x3f, 0x1e, 0x7f, 0x9c, 0x2c, 0xe0, 0x3f, 0xe4, 0x32, 0xcc, 0x28, 0xea, 0xe9,
    0xb1, 0xa3, 0x7b, 0xf9, 0x4e, 0xb3, 0x5d, 0x7d, 0xaf, 0xe2, 0xaf, 0xeb, 0x92, 0x98, 0x71, 0xae,
};

static const uint8_t PICTURE_12[1 + 95] = {
    0xee, /* not of the picture */
    0x78, 0x39, 0xce, 0x0f, 0x0a, 0x80, 0x68, 0x99, 0x11, 0x81, 0x8b, 0xdd, 0x13, 0x56, 0x1b, 0xbe, 0xbd, 0xd2,
    0x8c, 0x99, 0x81, 0xcf, 0xc8, 0xbf, 0x51, 0x9b, 0xf7, 0xef, 0xc9, 0x56, 0xeb, 0xd8, 0xd4, 0x34, 0x58, 0x68,
    0x51, 0xe0, 0xc0, 0x03, 0x17, 0x25, 0xe4, 0x75, 0xef, 0x24, 0xad, 0x65, 0xb8, 0x0d, 0x72, 0x32, 0xf6, 0x3f,
    0x6d, 0x9e, 0x3e, 0x8c, 0x8b, 0x86, 0x52, 0xc5, 0x22, 0x31, 0xf1, 0xec, 0x38, 0x00, 0xd1, 0x86, 0xa1, 0x22,
    0xe1, 0xb2, 0xc6, 0x98, 0xf3, 0xec, 0x45, 0x5c, 0x82, 0x08, 0x74, 0x3d, 0x28, 0x20, 0x84, 0x24, 0xc5, 0x4b,
    0x2a, 0x66, 0xd0, 0xa1, 0x2e,
};

static uint32_t seed = 19;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

static const uint8_t *picture_of(uint8_t bits)
{
    switch (bits) {
    case 1: return PICTURE_1 + 1;
    case 4: return PICTURE_4 + 1;
    case 8: return PICTURE_8 + 1;
    case 12: return PICTURE_12 + 1;
    case 16: return PICTURE_16 + 1;
    default: return PICTURE_24 + 1;
    }
}

/* is the address in one of the pictures */
static bool in_picture(const void *p)
{
    static const struct {
        const uint8_t *at;
        size_t         size;
    } all[] = {
        {PICTURE_1, sizeof PICTURE_1},   {PICTURE_4, sizeof PICTURE_4},   {PICTURE_8, sizeof PICTURE_8},
        {PICTURE_12, sizeof PICTURE_12}, {PICTURE_16, sizeof PICTURE_16}, {PICTURE_24, sizeof PICTURE_24},
    };
    for (size_t i = 0; i < sizeof all / sizeof *all; ++i)
        if ((const uint8_t *)p >= all[i].at && (const uint8_t *)p < all[i].at + all[i].size) return true;
    return false;
}

/* is the address in memory the process cannot write to; true where that cannot be told */
static bool read_only(const void *p)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return true;
    char          line[256];
    unsigned long from, to;
    char          perms[8];
    bool          found = false, writable = false;
    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%lx-%lx %7s", &from, &to, perms) != 3) continue;
        if ((unsigned long)(uintptr_t)p >= from && (unsigned long)(uintptr_t)p < to) found = true, writable = perms[1] == 'w';
    }
    fclose(maps);
    return found && !writable;
}

/*
 * A display behind a bus: a window, then the bytes of its pixels, row by
 * row. As a bus that sends by DMA it reads the bytes of a piece only when it
 * is done with it. It counts the pieces it is handed where a picture lies,
 * read-only or in RAM.
 */
typedef struct {
    dgx_screen_with_bus_t sbus; /* first: the screen leads to its panel */
    dgx_bus_protocols_t   bus;  /* the bus leads to it too */
    uint8_t               buffer[64];
    uint16_t              frame[DW * DH]; /* of a display that keeps its picture in memory, as a DSI one */
    uint32_t              glass[DW * DH];
    int                   bytes;
    int                   l, r, b, x, y;
    const uint8_t        *pending;
    uint32_t              pending_bits;
    const uint8_t        *ram; /* the picture in RAM this display is shown, and its size */
    size_t                ram_size;
    long                  from_read_only, in_place, by_rows, by_bands, refused;
} panel_t;

static panel_t *panel_of_bus(struct _dgx_bus_protocols_t *bus)
{
    return (panel_t *)(void *)((uint8_t *)bus - offsetof(panel_t, bus));
}

static void panel_take(panel_t *p)
{
    if (!p->pending) return;
    for (uint32_t i = 0; i + (uint32_t)p->bytes <= p->pending_bits / 8u; i += (uint32_t)p->bytes) {
        uint32_t color = 0;
        for (int k = 0; k < p->bytes; ++k) color = color << 8 | p->pending[i + (uint32_t)k];
        CHECK(p->y <= p->b);
        if (p->y <= p->b) p->glass[p->x + p->y * DW] = color;
        if (++p->x > p->r) p->x = p->l, ++p->y;
    }
    p->pending = NULL;
}

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    panel_t *p = (panel_t *)scr;
    panel_take(p);
    CHECK(left <= right && top <= bottom && right < DW && bottom < DH);
    p->l = left, p->r = right, p->b = bottom, p->x = left, p->y = top;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    panel_t *p = (panel_t *)scr;
    panel_take(p);
    CHECK(lenbits % (8u * (uint32_t)p->bytes) == 0);
    if (in_picture(data)) ++p->from_read_only;
    if (p->ram && data >= p->ram && data < p->ram + p->ram_size) ++p->in_place;
    p->pending = data, p->pending_bits = lenbits;
}

static void panel_wait(dgx_screen_t *scr)
{
    panel_take((panel_t *)scr);
}

static uint32_t panel_get_pixel(dgx_screen_t *scr, int x, int y)
{
    panel_t *p = (panel_t *)scr;
    panel_take(p);
    return x >= 0 && y >= 0 && x < DW && y < DH ? p->glass[x + y * DW] : 0;
}

static void quiet(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

/* a bus that sends by DMA from RAM where the pixels lie, from a whole word: not from read-only memory */
static bool sends_from_ram(struct _dgx_bus_protocols_t *bus, const void *pixels)
{
    (void)bus;
    return !in_picture(pixels) && ((uintptr_t)pixels & 3u) == 0;
}

/*
 * The faster ways of a display that keeps its picture in memory, as the DSI
 * bus has them. Rows are moved by a DMA that reaches RAM only: read-only
 * memory is refused, as dgx_dsi_dma_takes() refuses it, and goes another
 * way. A picture of indices or of 16-bit colors is made into rows of the
 * panel by the processor, by the code the DSI bus makes its bands with.
 */
static bool panel_write_rows(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *rows,
                             uint32_t pitch)
{
    panel_t *p = panel_of_bus(bus);
    panel_take(p);
    if (in_picture(rows)) {
        ++p->refused;
        return false;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t color = 0;
            for (int k = 0; k < p->bytes; ++k) color = color << 8 | rows[(size_t)y * pitch + (size_t)x * (size_t)p->bytes + (size_t)k];
            p->glass[left + x + (top + y) * DW] = color;
        }
    }
    ++p->by_rows;
    return true;
}

static bool panel_write_indexed(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                                const dgx_bus_indexed_t *picture)
{
    panel_t *p = panel_of_bus(bus);
    panel_take(p);
    /* mixing with what the display has, and an overlay layer, are the DSI bus's own: they go another way here */
    if (p->bytes != 2 || picture->under || picture->unders || picture->overlay) return false;
    if (picture->bytes > 2 || (picture->bytes == 2 && picture->smooth)) return false;
    dgx_fb_area_t area;
    dgx_fb_area_init(&area, (uint8_t *)p->frame, DW, DH, 2, false);
    uint16_t *band    = malloc((size_t)width * height * 2u);
    uint32_t *scratch = malloc(dgx_fb_area_indexed_scratch(picture, width) + sizeof(uint32_t));
    CHECK(band && scratch);
    if (!band || !scratch) {
        free(band);
        free(scratch);
        return false;
    }
    dgx_scale_lines_t lines;
    dgx_fb_area_indexed_lines(&area, &lines, left, top, width, height, picture, (uint8_t *)scratch);
    /* in two bands, as the bus makes one while the one before it is moved */
    const uint16_t half = (uint16_t)(height / 2u);
    if (half) dgx_fb_area_indexed_rows(&area, (uint8_t *)band, left, top, width, height, picture, 0, half, &lines);
    dgx_fb_area_indexed_rows(&area, (uint8_t *)(band + (size_t)half * width), left, top, width, height, picture, half, (uint16_t)(height - half),
                             &lines);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) p->glass[left + x + (top + y) * DW] = band[(size_t)y * width + (size_t)x];
    free(band);
    free(scratch);
    ++p->by_bands;
    return true;
}

/* the displays a picture is put on */
enum {
    BUS_WINDOW,    /* behind a bus that has only its window and its buffer, as the parallel one */
    BUS_ASKED,     /* behind a bus that sends from RAM where the pixels lie, as SPI and QSPI */
    BUS_NO_BUFFER, /* behind a bus without a buffer: pixel by pixel */
    BUS_MEMORY,    /* one that keeps its picture in memory and is read back, as a DSI one */
    DISPLAYS
};

static dgx_screen_t *panel_screen(panel_t *p, int kind, uint8_t bits, const uint8_t *ram, size_t ram_size)
{
    memset(p, 0, sizeof(*p));
    p->ram = ram, p->ram_size = ram_size;
    if (kind != BUS_NO_BUFFER) p->bus.buffer = p->buffer, p->bus.buffer_len = sizeof(p->buffer);
    if (kind == BUS_ASKED) p->bus.sends_from = sends_from_ram;
    if (kind == BUS_MEMORY) p->bus.write_rows = panel_write_rows, p->bus.write_indexed = panel_write_indexed;
    p->sbus.bus       = &p->bus;
    p->bytes          = bits == 16 ? 2 : 3;
    dgx_screen_t *lcd = &p->sbus.scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = bits, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    dgx_scr_init_slow_bus_optimized_funcs(lcd);
    lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;
    lcd->update_screen = quiet;
    if (kind == BUS_MEMORY) lcd->get_pixel = panel_get_pixel;
    return lcd;
}

/* the picture in RAM, on a word, under a screen of its own: what the read-only one is compared with */
static dgx_screen_t *ram_twin(uint8_t bits)
{
    dgx_screen_t *twin = dgx_vscreen_init(SW, SH, bits, DgxScreenRGB);
    CHECK(twin != NULL);
    if (twin) memcpy(((dgx_vscreen_t *)twin)->v_array, picture_of(bits), dgx_color_points_to_bytes(bits, SW * SH));
    return twin;
}

static dgx_screen_t *read_only_screen(uint8_t bits)
{
    dgx_screen_t *scr = dgx_vscreen_from_array((void *)picture_of(bits), SW, SH, bits, DgxScreenRGB);
    CHECK(scr != NULL && dgx_vscreen_is_linear(scr));
    return scr;
}

static long transfers, in_place_total, rows_total, bands_total, refused_total;

/* the same transfer from the read-only picture and from its twin in RAM, onto two displays alike */
static void onto_displays(int kind, uint8_t bits, uint32_t flags, const uint16_t *lut, bool region)
{
    static panel_t a, b;
    dgx_screen_t  *fixed = read_only_screen(bits), *twin = ram_twin(bits);
    if (!fixed || !twin) return;
    const uint8_t dst_bits = lut ? 16 : bits;
    dgx_screen_t *da = panel_screen(&a, kind, dst_bits, NULL, 0);
    dgx_screen_t *db = panel_screen(&b, kind, dst_bits, ((dgx_vscreen_t *)twin)->v_array, dgx_color_points_to_bytes(bits, SW * SH));
    for (int i = 0; i < DW * DH; ++i) a.glass[i] = b.glass[i] = (uint32_t)rnd(0, 0xffff);
    const int x = rnd(-6, DW - 4), y = rnd(-5, DH - 3);
    const int rx = region ? rnd(-2, SW - 1) : 0, ry = region ? rnd(-2, SH - 1) : 0;
    const int rw = region ? rnd(1, SW + 2) : SW, rh = region ? rnd(1, SH + 2) : SH;
    if (lut) {
        dgx_vscreen8_region_to_screen16(da, x, y, fixed, rx, ry, rw, rh, lut, flags);
        dgx_vscreen8_region_to_screen16(db, x, y, twin, rx, ry, rw, rh, lut, flags);
    } else {
        dgx_vscreen_region_to_screen(da, x, y, fixed, rx, ry, rw, rh, flags);
        dgx_vscreen_region_to_screen(db, x, y, twin, rx, ry, rw, rh, flags);
    }
    panel_wait(da);
    panel_wait(db);
    CHECK(memcmp(a.glass, b.glass, sizeof a.glass) == 0);
    /* the bus was never handed the read-only picture where it lies; the one in RAM it may be */
    CHECK(a.from_read_only == 0 && b.from_read_only == 0 && a.by_rows == 0);
    ++transfers;
    in_place_total += b.in_place, rows_total += b.by_rows, bands_total += a.by_bands, refused_total += a.refused;
    dgx_screen_destroy(&fixed);
    dgx_screen_destroy(&twin);
}

/* and into the memory of a virtual screen */
static void into_memory(uint8_t bits, uint32_t flags, const uint16_t *lut, bool region)
{
    dgx_screen_t *fixed = read_only_screen(bits), *twin = ram_twin(bits);
    const uint8_t dst_bits = lut ? 16 : bits;
    dgx_screen_t *da = dgx_vscreen_init(DW, DH, dst_bits, DgxScreenRGB), *db = dgx_vscreen_init(DW, DH, dst_bits, DgxScreenRGB);
    CHECK(fixed && twin && da && db);
    if (fixed && twin && da && db) {
        const size_t size = dgx_color_points_to_bytes(dst_bits, DW * DH);
        uint8_t     *ma = ((dgx_vscreen_t *)da)->v_array, *mb = ((dgx_vscreen_t *)db)->v_array;
        for (size_t i = 0; i < size; ++i) ma[i] = mb[i] = (uint8_t)rnd(0, 255);
        const int x = rnd(-6, DW - 4), y = rnd(-5, DH - 3);
        const int rx = region ? rnd(-2, SW - 1) : 0, ry = region ? rnd(-2, SH - 1) : 0;
        const int rw = region ? rnd(1, SW + 2) : SW, rh = region ? rnd(1, SH + 2) : SH;
        if (lut) {
            dgx_vscreen8_region_to_screen16(da, x, y, fixed, rx, ry, rw, rh, lut, flags);
            dgx_vscreen8_region_to_screen16(db, x, y, twin, rx, ry, rw, rh, lut, flags);
        } else {
            dgx_vscreen_region_to_screen(da, x, y, fixed, rx, ry, rw, rh, flags);
            dgx_vscreen_region_to_screen(db, x, y, twin, rx, ry, rw, rh, flags);
        }
        CHECK(memcmp(ma, mb, size) == 0);
        ++transfers;
    }
    dgx_screen_destroy(&fixed);
    dgx_screen_destroy(&twin);
    dgx_screen_destroy(&da);
    dgx_screen_destroy(&db);
}

/* every way a picture is put: as it is, with holes, larger, smoothed, turned, mixed with what lies under it */
static void test_transfers(void)
{
    static const uint32_t ways[] = {
        0,
        DGX_VSCREEN_TRANSPARENT,
        DGX_VSCREEN_SCALE(2),
        DGX_VSCREEN_SCALE(3) | DGX_VSCREEN_TRANSPARENT,
        DGX_VSCREEN_SCALE(2) | DGX_VSCREEN_SMOOTH,
        DGX_VSCREEN_SCALE(3) | DGX_VSCREEN_SMOOTH_SFX,
        DGX_VSCREEN_SCALE(4) | DGX_VSCREEN_SMOOTH | DGX_VSCREEN_TRANSPARENT,
        DGX_VSCREEN_ALPHA(128),
        DGX_VSCREEN_ALPHA(77) | DGX_VSCREEN_TRANSPARENT,
        DGX_VSCREEN_ALPHA(200) | DGX_VSCREEN_SCALE(2),
        DGX_VSCREEN_ALPHA(128) | DGX_VSCREEN_EXACT,
    };
    static const uint8_t depths[] = {1, 4, 8, 12, 16, 24};
    static uint16_t      lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)rnd(0, 0xffff);
    for (size_t w = 0; w < sizeof ways / sizeof *ways; ++w) {
        for (int turn = 0; turn < 8; ++turn) {
            const uint32_t flags = ways[w] | DGX_VSCREEN_ORIENTED(turn);
            for (int region = 0; region < 2; ++region) {
                /* the same depth on both sides: a display takes pixels of whole bytes, memory every depth */
                for (size_t d = 0; d < sizeof depths; ++d) {
                    into_memory(depths[d], flags, NULL, region);
                    if (depths[d] < 16) continue;
                    for (int kind = 0; kind < DISPLAYS; ++kind) onto_displays(kind, depths[d], flags, NULL, region);
                }
                /* indices through a table of colors, as they are and as a mask */
                for (int mask = 0; mask < 2; ++mask) {
                    const uint32_t through = flags | (mask ? DGX_VSCREEN_MASK : 0u);
                    into_memory(8, through, lut, region);
                    for (int kind = 0; kind < DISPLAYS; ++kind) onto_displays(kind, 8, through, lut, region);
                }
            }
        }
    }
    /* as it is, whole or a part of it, in many places: where a bus has its shortest ways for a picture in RAM */
    for (int round = 0; round < 120; ++round) {
        for (int kind = 0; kind < DISPLAYS; ++kind) {
            onto_displays(kind, 16, 0, NULL, round & 1);
            onto_displays(kind, 24, 0, NULL, round & 1);
        }
    }
    /* the ways a read-only picture is kept from were open to the one in RAM, and the display of memory made its bands from it */
    CHECK(in_place_total > 10 && rows_total > 20 && refused_total > 20 && bands_total > 20);
}

/* read by a pixel, copied, cloned, read as ARGB, and drawn as a texture */
static void test_reading(void)
{
    static const uint8_t depths[] = {1, 4, 8, 12, 16, 24};
    for (size_t d = 0; d < sizeof depths; ++d) {
        const uint8_t bits  = depths[d];
        const size_t  size  = dgx_color_points_to_bytes(bits, SW * SH);
        dgx_screen_t *fixed = read_only_screen(bits), *twin = ram_twin(bits);
        if (!fixed || !twin) continue;
        for (int y = -1; y <= SH; ++y)
            for (int x = -1; x <= SW; ++x) CHECK(dgx_get_pixel(fixed, x, y) == dgx_get_pixel(twin, x, y));

        dgx_screen_t *copy = dgx_vscreen_init(SW, SH, bits, DgxScreenRGB);
        CHECK(copy && dgx_vscreen_copy(copy, fixed));
        if (copy) CHECK(memcmp(((dgx_vscreen_t *)copy)->v_array, picture_of(bits), size) == 0);
        dgx_screen_destroy(&copy);

        dgx_screen_t *clone = dgx_vscreen_clone(fixed);
        CHECK(clone != NULL);
        if (clone) {
            CHECK(((dgx_vscreen_t *)clone)->v_array != picture_of(bits));
            CHECK(memcmp(((dgx_vscreen_t *)clone)->v_array, picture_of(bits), size) == 0);
            /* the clone has memory of its own: it is drawn on, the picture is not */
            dgx_fill_rectangle(clone, 0, 0, SW, SH, 0);
        }
        dgx_screen_destroy(&clone);

        static uint32_t argb_a[SW * SH], argb_b[SW * SH];
        CHECK(dgx_vscreen_to_argb8888(fixed, 0, 0, SW, SH, NULL, argb_a, SW) && dgx_vscreen_to_argb8888(twin, 0, 0, SW, SH, NULL, argb_b, SW));
        CHECK(memcmp(argb_a, argb_b, sizeof argb_a) == 0);

        if (bits >= 8) {
            dgx_screen_t *da = dgx_vscreen_init(DW, DH, bits, DgxScreenRGB), *db = dgx_vscreen_init(DW, DH, bits, DgxScreenRGB);
            CHECK(da && db);
            if (da && db) {
                const dgx_point_2d_t quad[4] = {{3, 2}, {30, 5}, {35, 26}, {1, 20}};
                dgx_draw_texture_rect(da, 2, 3, 25, 17, fixed, 0, 0, SW, SH);
                dgx_draw_texture_rect(db, 2, 3, 25, 17, twin, 0, 0, SW, SH);
                dgx_draw_texture_quad(da, quad, fixed, 1, 1, SW - 2, SH - 2);
                dgx_draw_texture_quad(db, quad, twin, 1, 1, SW - 2, SH - 2);
                CHECK(memcmp(((dgx_vscreen_t *)da)->v_array, ((dgx_vscreen_t *)db)->v_array, dgx_color_points_to_bytes(bits, DW * DH)) == 0);
            }
            dgx_screen_destroy(&da);
            dgx_screen_destroy(&db);
        }
        /* the screen over the picture is gone; its memory is not freed, and is what it was */
        dgx_screen_destroy(&fixed);
        CHECK(memcmp(((dgx_vscreen_t *)twin)->v_array, picture_of(bits), size) == 0);
        dgx_screen_destroy(&twin);
    }
}

/* a logo on a 16-bit display behind SPI, as a program shows it */
static void test_logo(void)
{
    static panel_t panel;
    dgx_screen_t  *display = panel_screen(&panel, BUS_ASKED, 16, NULL, 0);
    const uint8_t *pixels  = picture_of(16);
    dgx_screen_t  *logo    = dgx_vscreen_from_array((void *)pixels, SW, SH, 16, DgxScreenRGB);
    CHECK(logo != NULL);
    if (!logo) return;
    dgx_vscreen_to_screen(display, 5, 4, logo, 0);
    panel_wait(display);
    for (int y = 0; y < SH; ++y)
        for (int x = 0; x < SW; ++x)
            CHECK(panel.glass[(4 + y) * DW + 5 + x] == ((uint32_t)pixels[2 * (y * SW + x)] << 8 | pixels[2 * (y * SW + x) + 1]));
    CHECK(panel.from_read_only == 0);
    dgx_screen_destroy(&logo);
}

int main(void)
{
    /* the pictures are where a write is a fault, so a transfer that wrote to its source would not pass */
    static const uint8_t depths[] = {1, 4, 8, 12, 16, 24};
    for (size_t d = 0; d < sizeof depths; ++d) CHECK(read_only(picture_of(depths[d])) && ((uintptr_t)picture_of(depths[d]) & 1u));
    test_transfers();
    test_reading();
    test_logo();
    CHECK(transfers > 2000);
    CHECK_DONE();
}
