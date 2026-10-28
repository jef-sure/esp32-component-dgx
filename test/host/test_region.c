#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "scale_ref.h"
#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"
#include "fonts/font0ss.h"

/*
 * A part of an 8-bit virtual screen sent to a 16-bit one, and the area a
 * screen has changed in: what a program that shows a virtual screen by parts
 * relies on.
 */

#define SW 37
#define SH 23
#define DW 50
#define DH 40
#define MARK 0x1234 /* what the destination holds before a transfer */

static uint32_t seed = 12345;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

static uint16_t lut[256];

/* the pixel of the source the pixel `d` of the destination shows, the source shown at `at` n times as large; -1 left of it */
static int source_of(int d, int at, int n)
{
    return d < at ? -1 : (d - at) / n;
}

/* the source of the round made larger by the shaders of scale_ref.h when it smooths: REF_SMOOTH or REF_SFX, else REF_BLOCKS */
static uint32_t      source_values[SW * SH];
static ref_picture_t source_ref = {source_values, SW, SH};
static int           smoothed;

/* what a transfer of the whole source to (x, y), n times as large, has to leave in the pixel (dx, dy) of the destination */
static uint16_t expected(dgx_screen_t *src, int x, int y, int n, int dx, int dy, bool transparent)
{
    int sx = source_of(dx, x, n), sy = source_of(dy, y, n);
    if (sx < 0 || sy < 0 || sx >= SW || sy >= SH) return MARK;
    uint8_t index = smoothed ? (uint8_t)ref_larger(&source_ref, n, smoothed, dx - x, dy - y) : (uint8_t)dgx_get_pixel(src, sx, sy);
    return transparent && !index ? MARK : lut[index];
}

/*
 * A display behind a bus: it takes an area and then the bytes of its pixels, two a pixel, row by row.
 * As a bus that sends by DMA, it reads the bytes of a piece only when it is done with it: when the
 * next piece, an area or a wait comes, or, for a piece queued behind another, when that one is
 * queued behind in turn. A piece written over before it has gone shows up as wrong pixels.
 */
static uint16_t       panel[DW * DH];
static int            area_l, area_r, area_t, area_b, area_x, area_y;
static int            sent_pixels, areas, pieces, queued_pieces;
static uint32_t       piece_first, piece_most;
static const uint8_t *pending[2];
static uint32_t       pending_bits[2];
static int            pendings;

/* the oldest piece on its way is done */
static void panel_take_oldest(void)
{
    const uint8_t *data = pending[0];
    const uint32_t bits = pending_bits[0];
    for (uint32_t i = 0; i + 1 < bits / 8; i += 2) {
        CHECK(area_y <= area_b && area_x < DW && area_y < DH);
        if (area_y <= area_b && area_x < DW && area_y < DH) panel[area_x + area_y * DW] = (uint16_t)(data[i] << 8 | data[i + 1]);
        ++sent_pixels;
        if (++area_x > area_r) area_x = area_l, ++area_y;
    }
    pending[0] = pending[1], pending_bits[0] = pending_bits[1];
    --pendings;
}

/* all the pieces on their way are done */
static void panel_take(void)
{
    while (pendings) panel_take_oldest();
}

static void panel_piece(const uint8_t *data, uint32_t lenbits)
{
    CHECK(pendings < 2);
    pending[pendings] = data, pending_bits[pendings] = lenbits;
    ++pendings;
    if (!pieces++) piece_first = lenbits / 8;
    if (lenbits / 8 > piece_most) piece_most = lenbits / 8;
}


static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    panel_take();
    area_l = left, area_r = right, area_t = top, area_b = bottom;
    area_x = left, area_y = top;
    ++areas;
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

/*
 * A bus that has a faster way than its window for indices: it makes the colors itself, a rectangle at a
 * time. As a real one does, it refuses what is too small to be worth it; this one also refuses a
 * rectangle now and then, and that has to come another way.
 */
static int indexed_calls, indexed_taken, indexed_refuse_every, indexed_least, indexed_scaled;

static bool panel_write_indexed(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                                const dgx_bus_indexed_t *picture)
{
    (void)bus;
    ++indexed_calls;
    if ((int)width * height * 2 < indexed_least) return false;
    if (indexed_refuse_every && indexed_calls % indexed_refuse_every == 0) return false;
    /* a smoothed picture comes the other way, for the test to see that one */
    if (picture->smooth) return false;
    const uint32_t scale = picture->scale;
    CHECK(left + width <= DW && top + height <= DH && picture->pitch == SW && picture->width == SW && picture->height == SH && scale >= 1 &&
          picture->lut == lut);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t index = picture->pixels[(size_t)((picture->y + y) / scale) * picture->pitch + (size_t)((picture->x + x) / scale)];
            if (left + x < DW && top + y < DH) panel[left + x + (top + y) * DW] = lut[index];
            ++sent_pixels;
        }
    }
    ++indexed_taken;
    indexed_scaled += scale > 1;
    return true;
}

static void panel_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr;
    panel_take();
    if (x >= 0 && y >= 0 && x < DW && y < DH) panel[x + y * DW] = (uint16_t)color;
}

static void panel_update(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

static uint16_t panel_or_screen(dgx_screen_t *dst, int x, int y)
{
    return dst ? (uint16_t)dgx_get_pixel(dst, x, y) : panel[x + y * DW];
}

static void test_region(void)
{
    dgx_screen_t *plain = dgx_vscreen_init(SW, SH, 8, DgxScreenRGB), *src = plain;
    dgx_screen_t *whole = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB), *part = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)(i * 257 + 3);
    for (int y = 0; y < SH; ++y) {
        for (int x = 0; x < SW; ++x) dgx_set_pixel(src, x, y, (uint32_t)(rnd(0, 3) ? rnd(1, 255) : 0));
    }
    /* a source of few indices beside it, for Scale2x to find equal neighbours */
    dgx_screen_t *few = dgx_vscreen_init(SW, SH, 8, DgxScreenRGB);
    for (int y = 0; y < SH; ++y) {
        for (int x = 0; x < SW; ++x) dgx_set_pixel(few, x, y, (uint32_t)rnd(0, 2));
    }
    for (int i = 0; i < SW * SH; ++i) source_values[i] = ((dgx_vscreen_t *)few)->v_array[i];
    int smoothed_rounds = 0;

    /* the display behind a bus, with a buffer small enough to be sent many times a transfer */
    static uint8_t        bus_buffer[64];
    dgx_bus_protocols_t   bus = {.buffer = bus_buffer, .buffer_len = sizeof(bus_buffer)};
    dgx_screen_with_bus_t sbus = {.bus = &bus};
    dgx_screen_t         *lcd = &sbus.scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;
    lcd->set_pixel = panel_set_pixel, lcd->update_screen = panel_update;

    /* rectangles the buffer takes whole, of rows that fit in a half of it, and of rows cut */
    int in_one = 0, rows_fit = 0, rows_cut = 0, leftover_first = 0;
    for (int round = 0; round < 12000; ++round) {
        bool transparent = round & 1, by_bus = (round >> 1) & 1;
        /* the bus of every third round makes the colors itself, of every sixth it refuses some of them */
        int  indexed_kind = by_bus ? (round >> 2) % 3 : 0;
        bus.write_indexed = indexed_kind ? panel_write_indexed : NULL;
        indexed_least = indexed_kind ? 60 : 0;
        indexed_refuse_every = indexed_kind == 2 ? 3 : 0;
        /* a bus that queues a piece behind the one on its way, in half the rounds */
        queueing = (round >> 3) & 1;
        /* the source as it is, and two, three and four times as large */
        const int      n = 1 + (round >> 4) % 4;
        /* every other block of rounds smooths, by Scale2x and Scale3x or by their SFX kin; at 1 that changes nothing */
        const bool     smooth = (round >> 6) & 1, sfx = (round >> 7) & 1;
        const uint32_t flags = (transparent ? DGX_LUT_TRANSPARENT : 0) | (n > 1 || round % 3 ? DGX_LUT_SCALE(n) : 0) |
                               (smooth ? (sfx ? DGX_VSCREEN_SMOOTH_SFX : DGX_LUT_SMOOTH) : 0);
        smoothed = smooth && n > 1 ? (sfx ? REF_SFX : REF_SMOOTH) : REF_BLOCKS;
        smoothed_rounds += smoothed != REF_BLOCKS;
        src = smooth ? few : plain;
        int  x = rnd(-n * SW - 3, DW + 3), y = rnd(-n * SH - 3, DH + 3); /* where the whole source is shown */
        int  left = rnd(-6, SW + 5), top = rnd(-6, SH + 5), width = rnd(-2, SW + 8), height = rnd(-2, SH + 8);
        if (round % 7 == 0) left = 0, top = 0, width = SW, height = SH;

        /* the whole source: what it gave before there was a transfer by parts */
        dgx_fill_rectangle(whole, 0, 0, DW, DH, MARK);
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;
        dgx_vscreen8_to_screen16(by_bus ? lcd : whole, x, y, src, lut, flags);
        int wrong = 0;
        for (int dy = 0; dy < DH; ++dy) {
            for (int dx = 0; dx < DW; ++dx) wrong += panel_or_screen(by_bus ? 0 : whole, dx, dy) != expected(src, x, y, n, dx, dy, transparent);
        }
        CHECK(wrong == 0);

        /* a part of it: the same pixels within the part, nothing outside it */
        dgx_fill_rectangle(part, 0, 0, DW, DH, MARK);
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;
        sent_pixels = areas = pieces = 0;
        piece_most = 0;
        dgx_vscreen8_region_to_screen16(by_bus ? lcd : part, x + n * left, y + n * top, src, left, top, width, height, lut, flags);
        wrong = 0;
        int inside = 0;
        for (int dy = 0; dy < DH; ++dy) {
            for (int dx = 0; dx < DW; ++dx) {
                int  sx = source_of(dx, x, n), sy = source_of(dy, y, n);
                bool in = sx >= left && sx < left + width && sy >= top && sy < top + height && sx >= 0 && sy >= 0 && sx < SW && sy < SH;
                inside += in;
                wrong += panel_or_screen(by_bus ? 0 : part, dx, dy) != (in ? expected(src, x, y, n, dx, dy, transparent) : MARK);
            }
        }
        CHECK(wrong == 0);
        /* over a bus only the pixels of the part are sent, in one area; nothing is when nothing is left of the part */
        if (by_bus && !transparent) CHECK(sent_pixels == inside);
        if (by_bus && !transparent && !indexed_kind) CHECK(areas == (inside ? 1 : 0));
        if (by_bus && !inside) CHECK(sent_pixels == 0 && areas == 0);
        if (by_bus && !transparent && !indexed_kind && inside) {
            /* the buffer in two halves of 32 bytes: whole rows in a half where a row fits in it,
             * and a multiple of 4 bytes with them, what the SPI DMA takes without a copy */
            const uint32_t all = 2u * (uint32_t)inside, row = 2u * (uint32_t)(area_r - area_l + 1), half = sizeof(bus_buffer) / 2;
            uint32_t      rows = 1;
            while (row * rows % 4u) ++rows;
            const uint32_t cut = row * rows;
            if (all <= sizeof(bus_buffer)) {
                CHECK(pieces == 1);
                ++in_one;
            } else {
                /* a half of whole rows where a row fits in it; what is left over after whole halves goes first */
                const uint32_t len = row > half ? half : cut <= half ? half / cut * cut : half / row * row;
                CHECK(piece_first == (all % len ? all % len : len) && piece_most == len);
                leftover_first += all % len != 0;
                if (row <= half) ++rows_fit;
                else ++rows_cut;
            }
        }
    }
    CHECK(in_one > 30 && rows_fit > 50 && rows_cut > 100 && leftover_first > 100);
    CHECK(queued_pieces > 1000 && smoothed_rounds > 2000);
    smoothed = REF_BLOCKS;
    dgx_screen_destroy(&few);
    /* the bus has made colors, scaled and not, and has refused some */
    CHECK(indexed_taken > 1000 && indexed_scaled > 500 && indexed_calls > indexed_taken + 300);
    dgx_screen_destroy(&plain);
    dgx_screen_destroy(&whole);
    dgx_screen_destroy(&part);
}

/*
 * A bus that mixes two colors by an amount of ink itself, as the accelerator of an ESP32-P4 does. This
 * one puts the colors of a table of its own, to be told from the table of the library, and refuses now
 * and then; what it refuses has to come another way, in the colors of the library.
 */
static int      ink_calls, ink_taken, ink_refuse_every;
static uint16_t ink_colors[256];

static bool panel_write_ink(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *ink,
                            uint32_t pitch, uint32_t paper, uint32_t color)
{
    (void)bus;
    ++ink_calls;
    if (ink_refuse_every && ink_calls % ink_refuse_every == 0) return false;
    CHECK(left + width <= DW && top + height <= DH && pitch >= width && paper == 0xffbd && color == 0x192f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (left + x < DW && top + y < DH) panel[left + x + (top + y) * DW] = ink_colors[ink[(size_t)y * pitch + x]];
        }
    }
    ++ink_taken;
    return true;
}

/* a table of two colors and what lies between them, rounded as a program would: each channel on its own, in 8 bits */
static void two_colors(uint16_t *table, const uint8_t *paper, const uint8_t *color)
{
    for (int a = 0; a < 256; ++a) {
        int rgb[3];
        for (int c = 0; c < 3; ++c) rgb[c] = paper[c] + (color[c] - paper[c]) * a / 255;
        table[a] = (uint16_t)((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3);
    }
}

static void test_ink(void)
{
    dgx_screen_t *src = dgx_vscreen_init(SW, SH, 8, DgxScreenRGB);
    dgx_screen_t *by_table = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
    for (int y = 0; y < SH; ++y) {
        for (int x = 0; x < SW; ++x) dgx_set_pixel(src, x, y, (uint32_t)(rnd(0, 3) ? rnd(0, 255) : rnd(0, 1) * 255));
    }
    /* the table of a page of ink, as examples/hw_soft_pen_demo makes it, and tables that are no two colors */
    static const uint8_t paper[3] = {250, 246, 232}, color[3] = {24, 36, 120};
    static uint16_t      straight[256], bent[256], stepped[256];
    two_colors(straight, paper, color);
    CHECK(straight[0] == 0xffbd && straight[255] == 0x192f);
    for (int a = 0; a < 256; ++a) {
        /* the same two colors with a bend in the way between them, and with two steps where one is let */
        bent[a] = straight[a * a / 255];
        stepped[a] = straight[a];
    }
    stepped[100] = (uint16_t)(stepped[100] ^ (2 << 5));
    /* the bus of the test puts colors of its own, to be told from the table */
    for (int a = 0; a < 256; ++a) ink_colors[a] = (uint16_t)(straight[a] ^ 0x0841);

    static uint8_t        bus_buffer[64];
    dgx_bus_protocols_t   bus = {.buffer = bus_buffer, .buffer_len = sizeof(bus_buffer)};
    dgx_screen_with_bus_t sbus = {.bus = &bus};
    dgx_screen_t         *lcd = &sbus.scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;
    lcd->set_pixel = panel_set_pixel, lcd->update_screen = panel_update;

    int asked_wrongly = 0;
    for (int round = 0; round < 8000; ++round) {
        /* a bus with its window alone, a bus that mixes, a bus that mixes and refuses every third time */
        const int       kind = round % 3;
        /* the table of two colors, the bent one, the stepped one, the two colors with a hole for 0, the two colors asked for to the last bit */
        const int       which = (round / 3) % 5;
        uint16_t       *table = which == 1 ? bent : which == 2 ? stepped : straight;
        const uint32_t  transparent = which == 3 ? DGX_LUT_TRANSPARENT : which == 4 ? DGX_LUT_EXACT : 0;
        int             x = rnd(-SW - 3, DW + 3), y = rnd(-SH - 3, DH + 3);
        int             left = rnd(-6, SW + 5), top = rnd(-6, SH + 5), width = rnd(-2, SW + 8), height = rnd(-2, SH + 8);
        if (round % 7 == 0) left = 0, top = 0, width = SW, height = SH;
        bus.write_ink = kind ? panel_write_ink : NULL;
        ink_refuse_every = kind == 2 ? 3 : 0;
        const int taken_before = ink_taken, calls_before = ink_calls;
        dgx_fill_rectangle(by_table, 0, 0, DW, DH, MARK);
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;
        dgx_vscreen8_region_to_screen16(by_table, x + left, y + top, src, left, top, width, height, table, transparent);
        dgx_vscreen8_region_to_screen16(lcd, x + left, y + top, src, left, top, width, height, table, transparent);
        const bool mixed = ink_taken != taken_before;
        /* only the table of two colors, with no hole in it and not asked for exactly, is offered to the bus that mixes */
        if (which) asked_wrongly += ink_calls != calls_before;
        int wrong = 0;
        for (int dy = 0; dy < DH; ++dy) {
            for (int dx = 0; dx < DW; ++dx) {
                uint16_t want = (uint16_t)dgx_get_pixel(by_table, dx, dy);
                int      sx = dx - x, sy = dy - y;
                bool     in = sx >= left && sx < left + width && sy >= top && sy < top + height && sx >= 0 && sy >= 0 && sx < SW && sy < SH;
                /* the bus that has mixed has put its own colors, in the same pixels */
                if (mixed && in) want = ink_colors[dgx_get_pixel(src, sx, sy)];
                wrong += panel[dx + dy * DW] != want;
            }
        }
        CHECK(wrong == 0);
    }
    /* the bus has mixed many times, has refused many, and was never asked for a table that is not two colors */
    CHECK(ink_taken > 250 && ink_calls > ink_taken + 50 && asked_wrongly == 0);
    dgx_screen_destroy(&src);
    dgx_screen_destroy(&by_table);
}

static int updates;

static void count_update(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
    ++updates;
}

static void test_take(void)
{
    dgx_screen_t *s = dgx_vscreen_init(40, 30, 8, DgxScreenRGB);
    int           l = -1, t = -1, w = -1, h = -1;
    /* a screen of dgx_vscreen_init() has an update_screen that does nothing, so closing a batch costs nothing and loses nothing */
    CHECK(s->update_screen != 0);
    dgx_screen_progress_up(s);
    dgx_fill_rectangle(s, 3, 4, 5, 6, 1);
    dgx_set_pixel(s, 20, 25, 1);
    CHECK(dgx_screen_progress_down(s) == 0 && s->dirty_right == 0 && s->in_progress == 0);

    s->update_screen = count_update;
    /* nothing has changed */
    dgx_screen_progress_up(s);
    CHECK(!dgx_screen_take_dirty(s, &l, &t, &w, &h) && l == 0 && t == 0 && w == 0 && h == 0);
    /* what has changed is taken once, and closing the batch sends nothing after that */
    dgx_fill_rectangle(s, 3, 4, 5, 6, 1);
    dgx_set_pixel(s, 20, 25, 1);
    CHECK(dgx_screen_take_dirty(s, &l, &t, &w, &h) && l == 3 && t == 4 && w == 18 && h == 22);
    CHECK(!dgx_screen_take_dirty(s, &l, &t, &w, &h) && w == 0 && h == 0);
    CHECK(dgx_screen_progress_down(s) == 0 && updates == 0);
    /* a batch kept open over many takes: each gives what was drawn since the one before */
    dgx_screen_progress_up(s);
    dgx_set_pixel(s, 1, 2, 1);
    CHECK(dgx_screen_take_dirty(s, &l, &t, &w, &h) && l == 1 && t == 2 && w == 1 && h == 1);
    dgx_fill_rectangle(s, 30, 20, 100, 100, 1);
    CHECK(dgx_screen_take_dirty(s, &l, &t, &w, &h) && l == 30 && t == 20 && w == 10 && h == 10);
    dgx_fill_rectangle(s, 50, 50, 5, 5, 1); /* off the screen */
    CHECK(!dgx_screen_take_dirty(s, &l, &t, &w, &h));
    dgx_screen_progress_down(s);
    CHECK(updates == 0);
    /* outside a batch every change is sent at once and there is nothing to take */
    dgx_set_pixel(s, 5, 5, 1);
    CHECK(updates == 1 && !dgx_screen_take_dirty(s, &l, &t, &w, &h));
    dgx_screen_destroy(&s);
}

#define W 200
#define H 120

static uint8_t before[W * H];

static void snapshot(dgx_screen_t *s)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) before[x + y * W] = (uint8_t)dgx_get_pixel(s, x, y);
    }
}

/* how many pixels have changed since the snapshot, and how many of them outside the area the screen gives */
static int changed_outside(dgx_screen_t *s, int *changed)
{
    int l, t, w, h, outside = 0;
    dgx_screen_take_dirty(s, &l, &t, &w, &h);
    *changed = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if ((uint8_t)dgx_get_pixel(s, x, y) == before[x + y * W]) continue;
            ++*changed;
            outside += x < l || x >= l + w || y < t || y >= t + h;
        }
    }
    return outside;
}

/* Whatever a clock draws with on an 8-bit virtual screen marks all it changes. */
static void test_marks(void)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_font_t   *font = font0ss();
    int           changed, drawn = 0;
    dgx_screen_progress_up(s); /* kept open: the areas are taken, not sent */

    for (int round = 0; round < 300; ++round) {
        uint32_t ink = (uint32_t)rnd(1, 255);
        snapshot(s);
        dgx_fill_rectangle(s, rnd(-20, W), rnd(-20, H), rnd(0, 60), rnd(0, 60), ink);
        CHECK(changed_outside(s, &changed) == 0);
        drawn += changed;

        snapshot(s);
        dgx_solid_circle(s, rnd(-10, W + 10), rnd(-10, H + 10), rnd(0, 25), ink ^ 0x55);
        CHECK(changed_outside(s, &changed) == 0);
        drawn += changed;

        /* a thick curve, whole and by parts */
        dgx_point_2d_t p[4];
        for (int i = 0; i < 4; ++i) p[i] = (dgx_point_2d_t){rnd(-20, W + 20), rnd(-20, H + 20)};
        dgx_bezier_t b;
        if (round & 1) dgx_bezier4_begin(&b, s, p, rnd(0, 12), rnd(1, 9), ink ^ 0xaa);
        else dgx_bezier3_begin(&b, s, p, rnd(0, 12), rnd(1, 9), ink ^ 0xaa);
        int steps = rnd(1, 9);
        for (int k = 1; k <= steps; ++k) {
            snapshot(s);
            dgx_bezier_draw_to(&b, (float)k / steps);
            CHECK(changed_outside(s, &changed) == 0);
            drawn += changed;
        }
    }
    CHECK(drawn > 100000);

    /* a text written by a pen: every step marks what it adds, at one size and at two */
    static const char *const texts[] = {"привет", "Часы 12:45", "−7 dBm"};
    for (int k = 0; k < 6; ++k) {
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_hw_pace_t    pace = dgx_hw_pace(font, 6);
        dgx_hw_writing_t writing;
        const char      *text = texts[k % 3];
        float            effort = dgx_hw_text_effort(font, text, true, &pace);
        if (k < 3) dgx_hw_writing_begin(&writing, s, 6, 70, font, text, 0.25f + 0.05f * k, 2 + k, 200, true, &pace);
        else dgx_hw_writing_begin_xy(&writing, s, 6, 70, font, text, 0.2f, 0.45f, 3, 200, k & 1, &pace);
        int  steps = 0, written = 0;
        bool finished = false;
        for (float at = 0; !finished && steps < 5000; at += effort / 173, ++steps) {
            snapshot(s);
            finished = dgx_hw_writing_draw_to(&writing, at);
            CHECK(changed_outside(s, &changed) == 0);
            written += changed;
        }
        CHECK(finished && written > 500);
    }

    /* a frame of a morph of handwritten text */
    dgx_hw_morph_t *m = dgx_hw_morph_create(font, "12:45", "12:46", false);
    CHECK(m != 0);
    for (int k = 0; m && k <= 20; ++k) {
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        int l, t, w, h;
        dgx_screen_take_dirty(s, &l, &t, &w, &h);
        snapshot(s);
        dgx_hw_morph_draw_xy(m, k / 20.0f, s, 4, 80, 0.3f, 0.4f, 3, 255);
        CHECK(changed_outside(s, &changed) == 0 && changed > 300);
    }
    dgx_hw_morph_destroy(&m);

    dgx_screen_progress_down(s);
    dgx_screen_destroy(&s);
}

int main(void)
{
    test_region();
    test_ink();
    test_take();
    test_marks();
    CHECK_DONE();
}
