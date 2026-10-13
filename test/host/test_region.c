#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
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

/* what a transfer of the whole source to (x, y) has to leave in the pixel (dx, dy) of the destination */
static uint16_t expected(dgx_screen_t *src, int x, int y, int dx, int dy, bool transparent)
{
    int sx = dx - x, sy = dy - y;
    if (sx < 0 || sy < 0 || sx >= SW || sy >= SH) return MARK;
    uint8_t index = (uint8_t)dgx_get_pixel(src, sx, sy);
    return transparent && !index ? MARK : lut[index];
}

/* a display behind a bus: it takes an area and then the bytes of its pixels, two a pixel, row by row */
static uint16_t panel[DW * DH];
static int      area_l, area_r, area_t, area_b, area_x, area_y;
static int      sent_pixels, areas;

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    area_l = left, area_r = right, area_t = top, area_b = bottom;
    area_x = left, area_y = top;
    ++areas;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    (void)scr;
    for (uint32_t i = 0; i + 1 < lenbits / 8; i += 2) {
        CHECK(area_y <= area_b && area_x < DW && area_y < DH);
        if (area_y <= area_b && area_x < DW && area_y < DH) panel[area_x + area_y * DW] = (uint16_t)(data[i] << 8 | data[i + 1]);
        ++sent_pixels;
        if (++area_x > area_r) area_x = area_l, ++area_y;
    }
}

static void panel_wait(dgx_screen_t *scr)
{
    (void)scr;
}

/*
 * A bus that has a faster way than its window: it takes rows as they lie in memory, a rectangle at a
 * time. As a real one does, it refuses what is too small to be worth it; this one also refuses a
 * rectangle now and then, and that has to go through the window.
 */
static int rows_calls, rows_taken, rows_refuse_every, rows_least, bands_given;

static bool panel_write_rows(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *rows,
                             uint32_t pitch)
{
    (void)bus;
    ++rows_calls;
    if ((int)width * height * 2 < rows_least) return false;
    if (rows_refuse_every && rows_calls % rows_refuse_every == 0) return false;
    CHECK(left + width <= DW && top + height <= DH && pitch >= 2u * width);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t *p = rows + (size_t)y * pitch + 2u * (size_t)x;
            if (left + x < DW && top + y < DH) panel[left + x + (top + y) * DW] = (uint16_t)(p[0] << 8 | p[1]);
            ++sent_pixels;
        }
    }
    ++rows_taken;
    return true;
}

static uint8_t *panel_memory(uint32_t len)
{
    ++bands_given;
    return (uint8_t *)malloc(len);
}

static void panel_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr;
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
    dgx_screen_t *src = dgx_vscreen_init(SW, SH, 8, DgxScreenRGB);
    dgx_screen_t *whole = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB), *part = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)(i * 257 + 3);
    for (int y = 0; y < SH; ++y) {
        for (int x = 0; x < SW; ++x) dgx_set_pixel(src, x, y, (uint32_t)(rnd(0, 3) ? rnd(1, 255) : 0));
    }

    /* the display behind a bus, with a buffer small enough to be sent many times a transfer */
    static uint8_t        bus_buffer[64];
    dgx_bus_protocols_t   bus = {.buffer = bus_buffer, .buffer_len = sizeof(bus_buffer)};
    dgx_screen_with_bus_t sbus = {.bus = &bus};
    dgx_screen_t         *lcd = &sbus.scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;
    lcd->set_pixel = panel_set_pixel, lcd->update_screen = panel_update;

    for (int round = 0; round < 12000; ++round) {
        bool transparent = round & 1, by_bus = (round >> 1) & 1;
        /* the bus of every third round takes rows, of every sixth it refuses some of them */
        int  rows_kind = by_bus ? (round >> 2) % 3 : 0;
        bus.write_rows = rows_kind ? panel_write_rows : NULL;
        bus.memory_allocator = rows_kind ? panel_memory : NULL;
        rows_least = rows_kind ? 60 : 0;
        rows_refuse_every = rows_kind == 2 ? 3 : 0;
        int  x = rnd(-SW - 3, DW + 3), y = rnd(-SH - 3, DH + 3); /* where the whole source is shown */
        int  left = rnd(-6, SW + 5), top = rnd(-6, SH + 5), width = rnd(-2, SW + 8), height = rnd(-2, SH + 8);
        if (round % 7 == 0) left = 0, top = 0, width = SW, height = SH;

        /* the whole source: what it gave before there was a transfer by parts */
        dgx_fill_rectangle(whole, 0, 0, DW, DH, MARK);
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;
        dgx_vscreen8_to_screen16(by_bus ? lcd : whole, x, y, src, lut, transparent);
        int wrong = 0;
        for (int dy = 0; dy < DH; ++dy) {
            for (int dx = 0; dx < DW; ++dx) wrong += panel_or_screen(by_bus ? 0 : whole, dx, dy) != expected(src, x, y, dx, dy, transparent);
        }
        CHECK(wrong == 0);

        /* a part of it: the same pixels within the part, nothing outside it */
        dgx_fill_rectangle(part, 0, 0, DW, DH, MARK);
        for (int i = 0; i < DW * DH; ++i) panel[i] = MARK;
        sent_pixels = areas = 0;
        dgx_vscreen8_region_to_screen16(by_bus ? lcd : part, x + left, y + top, src, left, top, width, height, lut, transparent);
        wrong = 0;
        int inside = 0;
        for (int dy = 0; dy < DH; ++dy) {
            for (int dx = 0; dx < DW; ++dx) {
                int  sx = dx - x, sy = dy - y;
                bool in = sx >= left && sx < left + width && sy >= top && sy < top + height && sx >= 0 && sy >= 0 && sx < SW && sy < SH;
                inside += in;
                wrong += panel_or_screen(by_bus ? 0 : part, dx, dy) != (in ? expected(src, x, y, dx, dy, transparent) : MARK);
            }
        }
        CHECK(wrong == 0);
        /* over a bus only the pixels of the part are sent, in one area; nothing is when nothing is left of the part */
        if (by_bus && !transparent) CHECK(sent_pixels == inside);
        if (by_bus && !transparent && !rows_kind) CHECK(areas == (inside ? 1 : 0));
        if (by_bus && !inside) CHECK(sent_pixels == 0 && areas == 0);
    }
    /* the bus has taken rows, has refused some, and bands of several rows have gone both ways */
    CHECK(rows_taken > 1000 && rows_calls > rows_taken + 500 && bands_given > 1000);
    dgx_screen_destroy(&src);
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
    CHECK(ink_taken > 300 && ink_calls > ink_taken + 50 && asked_wrongly == 0);
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
