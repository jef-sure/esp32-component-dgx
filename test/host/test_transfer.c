/*
 * A virtual screen put on a screen with every flag: transparent, made larger
 * by squares or smoothed, turned in its eight ways, a region or the whole of
 * it, anywhere, to a display behind a bus (with its window only, or with the
 * faster ways of a DSI display) and to virtual screens. The model is worked
 * out the other way round from the library: every point of the larger
 * picture is carried forward to the pixel of the destination it lands on,
 * where the library asks of every pixel of the destination which point it
 * shows.
 */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_overlay.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"
#include "scale_ref.h"

#define DW 37
#define DH 29
#define SW 9
#define SH 7
#define MARK 0x5a5a5a5au

static uint32_t seed = 31337;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

/* what the destination is expected to hold */
static uint32_t model[DW * DH];

/*
 * The source of SW by SH values, shown at (x, y) by its region (rx, ry, rw, rh), made n times as large with
 * `smooth` and turned by `turn`; through `lut` when there is one; values of 0 left out when transparent.
 */
static void model_transfer(const uint32_t *values, int x, int y, int rx, int ry, int rw, int rh, int n, int smooth, unsigned turn, bool transparent,
                           const uint16_t *lut)
{
    const ref_picture_t p = {values, SW, SH};
    const int           wide = n * SW, high = n * SH;
    /* a point of the larger picture carried by the turn */
#define FORWARD(U, V, u, v)                                                 \
    do {                                                                    \
        if (!(turn & 4u)) {                                                 \
            u = turn & 1u ? wide - 1 - (U) : (U);                           \
            v = turn & 2u ? high - 1 - (V) : (V);                           \
        } else {                                                            \
            u = turn & 2u ? high - 1 - (V) : (V);                           \
            v = turn & 1u ? wide - 1 - (U) : (U);                           \
        }                                                                   \
    } while (0)
    /* the corner of the region asked for, turned: the least of its corners */
    int bu = 1 << 30, bv = 1 << 30;
    for (int c = 0; c < 4; ++c) {
        const int U = c & 1 ? (rx + rw) * n - 1 : rx * n, V = c & 2 ? (ry + rh) * n - 1 : ry * n;
        int       u, v;
        FORWARD(U, V, u, v);
        if (u < bu) bu = u;
        if (v < bv) bv = v;
    }
    for (int V = (ry > 0 ? ry : 0) * n; V < (ry + rh < SH ? ry + rh : SH) * n; ++V) {
        for (int U = (rx > 0 ? rx : 0) * n; U < (rx + rw < SW ? rx + rw : SW) * n; ++U) {
            int u, v;
            FORWARD(U, V, u, v);
            const int dx = x + u - bu, dy = y + v - bv;
            if (dx < 0 || dy < 0 || dx >= DW || dy >= DH) continue;
            const uint32_t value = ref_larger(&p, n, smooth, U, V);
            if (transparent && !value) continue;
            model[dy * DW + dx] = lut ? lut[value] : value;
        }
    }
#undef FORWARD
}

/*
 * A display behind a bus: a window, then the bytes of its pixels, `pixel_bytes` a pixel, row by row. As a bus
 * that sends by DMA it reads the bytes of a piece only when it is done with it, so a piece written over before
 * it has gone shows up as wrong pixels.
 */
static uint32_t       panel[DW * DH];
static int            pixel_bytes;
static int            area_l, area_r, area_b, area_x, area_y;
static long           windows, single_pixels, sent_pixels;
static const uint8_t *pending;
static uint32_t       pending_bits;

static void panel_take(void)
{
    if (!pending) return;
    for (uint32_t i = 0; i + pixel_bytes <= pending_bits / 8; i += pixel_bytes) {
        uint32_t color = 0;
        for (int k = 0; k < pixel_bytes; ++k) color = color << 8 | pending[i + k];
        CHECK(area_y <= area_b);
        if (area_y <= area_b && area_x < DW && area_y < DH) panel[area_x + area_y * DW] = color;
        ++sent_pixels;
        if (++area_x > area_r) area_x = area_l, ++area_y;
    }
    pending = NULL;
}

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    panel_take();
    CHECK(left <= right && top <= bottom && right < DW && bottom < DH);
    area_l = left, area_r = right, area_b = bottom, area_x = left, area_y = top;
    ++windows;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    (void)scr;
    panel_take();
    CHECK(lenbits % (8u * (uint32_t)pixel_bytes) == 0);
    pending = data, pending_bits = lenbits;
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

/* a display whose pixels are memory, as a DSI one: they are read back, and a pixel set alone costs no window */
static uint32_t panel_get_pixel(dgx_screen_t *scr, int x, int y)
{
    (void)scr;
    panel_take();
    return x >= 0 && y >= 0 && x < DW && y < DH ? panel[x + y * DW] : 0;
}

/* the faster ways of a DSI display: rows put as they lie, and indices through a table; each refuses now and then */
static int rows_taken, indexed_taken, indexed_turned, indexed_smoothed, refuse_every, calls;

static bool panel_write_rows(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *rows,
                             uint32_t pitch)
{
    (void)bus;
    if (refuse_every && ++calls % refuse_every == 0) return false;
    CHECK(left + width <= DW && top + height <= DH);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t color = 0;
            for (int k = 0; k < pixel_bytes; ++k) color = color << 8 | rows[y * pitch + x * pixel_bytes + k];
            panel[left + x + (top + y) * DW] = color;
        }
    }
    ++rows_taken;
    return true;
}

static uint32_t source_values[SW * SH];

/* the pixels of the rectangle as the picture tells them: its point (x, y) of the turned larger picture at the corner */
static bool panel_write_indexed(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                                const dgx_bus_indexed_t *picture)
{
    (void)bus;
    if (refuse_every && ++calls % refuse_every == 0) return false;
    CHECK(left + width <= DW && top + height <= DH && picture->width == SW && picture->height == SH && picture->pitch == SW);
    const ref_picture_t p     = {source_values, SW, SH};
    const int           n     = picture->scale, wide = n * SW, high = n * SH;
    const unsigned      turn  = picture->orientation;
    const int           smooth = picture->smooth == DGX_BUS_SMOOTH_SFX ? REF_SFX : picture->smooth ? REF_SMOOTH : REF_BLOCKS;
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            const int tx = (int)picture->x + i, ty = (int)picture->y + j;
            int       u, v;
            if (!(turn & 4u)) {
                u = turn & 1u ? wide - 1 - tx : tx;
                v = turn & 2u ? high - 1 - ty : ty;
            } else {
                u = turn & 1u ? wide - 1 - ty : ty;
                v = turn & 2u ? high - 1 - tx : tx;
            }
            CHECK(u >= 0 && v >= 0 && u < wide && v < high);
            panel[left + i + (top + j) * DW] = picture->lut[ref_larger(&p, n, smooth, u, v)];
        }
    }
    ++indexed_taken;
    indexed_turned += turn != 0;
    indexed_smoothed += picture->smooth != 0;
    return true;
}

static uint8_t       bus_buffer[64];
static dgx_bus_protocols_t   bus;
static dgx_screen_with_bus_t sbus;

static dgx_screen_t *panel_screen(int bits, size_t buffer_len, bool fast, bool readable)
{
    memset(&bus, 0, sizeof(bus));
    memset(&sbus, 0, sizeof(sbus));
    bus.buffer = bus_buffer, bus.buffer_len = buffer_len;
    bus.write_rows    = fast ? panel_write_rows : NULL;
    bus.write_indexed = fast ? panel_write_indexed : NULL;
    sbus.bus          = &bus;
    dgx_screen_t *lcd = &sbus.scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = (uint8_t)bits, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;
    lcd->set_pixel = panel_set_pixel, lcd->update_screen = panel_update;
    lcd->get_pixel = readable ? panel_get_pixel : NULL;
    return lcd;
}

/* a value of a few kinds spread over the depth, 0 among them, as set_pixel() takes it and get_pixel() gives it */
static uint32_t spread(int kind, int bits)
{
    if (!kind) return 0;
    const uint32_t v = (uint32_t)kind * 0x9e3779b1u;
    switch (bits) {
    case 1:
        return 1;
    case 4:
        return (uint32_t)(kind * 0x30) & 0xf0u;
    case 8:
        return (v >> 24) | 1u;
    case 16:
        return (v >> 16) | 1u;
    case 18:
        return ((v >> 8) & 0xfcfcfcu) | 4u;
    case 24:
        return (v >> 8) | 1u;
    default:
        return v | 1u;
    }
}

static const int     ref_smooth_of[3] = {REF_BLOCKS, REF_SMOOTH, REF_SFX};
static const uint32_t smooth_flag[3]  = {0, DGX_VSCREEN_SMOOTH, DGX_VSCREEN_SMOOTH_SFX};

static void test_same_depth(void)
{
    static const int depths[] = {16, 8, 24, 18, 32, 4, 1};
    static const int buffers[] = {64, 37, 9, 4};
    long             transparent_rounds = 0, turned = 0, scaled = 0, smoothed = 0, to_bus = 0;
    for (int round = 0; round < 6000; ++round) {
        const int bits = depths[round % 7];
        pixel_bytes    = bits == 16 ? 2 : bits == 8 ? 1 : bits == 32 ? 4 : 3;
        dgx_screen_t *src = dgx_vscreen_init(SW, SH, (uint8_t)bits, DgxScreenRGB);
        const int     kinds = rnd(2, 4);
        for (int i = 0; i < SW * SH; ++i) {
            const uint32_t value = spread(rnd(0, kinds - 1), bits);
            dgx_set_pixel(src, i % SW, i / SW, value);
            source_values[i] = dgx_get_pixel(src, i % SW, i / SW);
        }
        /* the destination: a display behind a bus, plain, with the ways of a DSI display, or read back as one is; or a virtual screen of the depth */
        const int     where = bits % 8 && bits != 18 ? 2 : rnd(0, 3);
        const bool    fast = where == 1, display = where != 2;
        refuse_every        = fast ? rnd(0, 3) : 0;
        dgx_screen_t *dst   = display ? panel_screen(bits, (size_t)buffers[rnd(0, 3)], fast, where == 3) : dgx_vscreen_init(DW, DH, (uint8_t)bits, DgxScreenRGB);
        const uint32_t mark = display ? MARK : dgx_get_pixel(dst, 0, 0);
        for (int i = 0; i < DW * DH; ++i) model[i] = panel[i] = mark;
        const bool     transparent = rnd(0, 2) == 0;
        const int      n = rnd(0, 2) ? 1 : rnd(2, 4), kind = n > 1 ? rnd(0, 2) : 0;
        const unsigned turn = (unsigned)rnd(0, 7);
        const bool     whole = rnd(0, 4) == 0;
        const int      rx = whole ? 0 : rnd(-3, SW), ry = whole ? 0 : rnd(-3, SH), rw = whole ? SW : rnd(0, SW + 4), rh = whole ? SH : rnd(0, SH + 4);
        const int      x = rnd(-n * SW, DW), y = rnd(-n * SH, DH);
        const uint32_t flags = (transparent ? DGX_VSCREEN_TRANSPARENT : 0) | DGX_VSCREEN_SCALE(n) | smooth_flag[kind] | DGX_VSCREEN_ORIENTED(turn);
        windows = single_pixels = sent_pixels = 0;
        if (whole) dgx_vscreen_to_screen(dst, x, y, src, flags);
        else dgx_vscreen_region_to_screen(dst, x, y, src, rx, ry, rw, rh, flags);
        model_transfer(source_values, x, y, rx, ry, rw, rh, n, ref_smooth_of[kind], turn, transparent, NULL);
        int wrong = 0, drawn = 0;
        for (int i = 0; i < DW * DH; ++i) {
            const uint32_t got = display ? panel[i] : dgx_get_pixel(dst, i % DW, i / DW);
            wrong += got != model[i];
            drawn += model[i] != mark;
        }
        if (wrong) {
            fprintf(stderr, "same depth: %d bits to %s, %s n %d smooth %d turn %u, region %d,%d %dx%d at %d,%d: %d wrong\n", bits,
                    where == 0 ? "a bus" : where == 1 ? "a DSI bus" : where == 3 ? "a display read back" : "a virtual screen",
                    transparent ? "transparent," : "", n, kind, turn, rx, ry, rw, rh, x, y, wrong);
            ++check_failures;
        }
        /* to a display a picture goes through windows, never a pixel on its own, but for one with holes onto a display read back; one window when nothing is left out */
        if (display && pixel_bytes) {
            if (!(transparent && where == 3)) CHECK(single_pixels == 0);
            if (!transparent && !fast) CHECK(windows <= 1 && sent_pixels == drawn);
            to_bus += drawn > 0;
        }
        transparent_rounds += transparent && drawn;
        turned += turn && drawn;
        scaled += n > 1 && drawn;
        smoothed += kind && drawn;
        dgx_screen_destroy(&src);
        if (!display) dgx_screen_destroy(&dst);
    }
    CHECK(transparent_rounds > 500 && turned > 2000 && scaled > 700 && smoothed > 400 && to_bus > 1500);
}

static void test_lut(void)
{
    static const int buffers[] = {64, 37, 9, 4};
    static uint16_t  lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)(i * 2654435761u >> 16);
    pixel_bytes = 2;
    long indexed_rounds = 0;
    for (int round = 0; round < 6000; ++round) {
        dgx_screen_t *src   = dgx_vscreen_init(SW, SH, 8, DgxScreenRGB);
        const int     kinds = rnd(0, 3) ? rnd(2, 4) : 256;
        for (int i = 0; i < SW * SH; ++i) {
            source_values[i] = (uint32_t)rnd(0, kinds - 1);
            dgx_set_pixel(src, i % SW, i / SW, source_values[i]);
        }
        const int     where = rnd(0, 3);
        const bool    fast = where == 1, display = where != 2;
        refuse_every        = fast ? rnd(0, 3) : 0;
        dgx_screen_t *dst   = display ? panel_screen(16, (size_t)buffers[rnd(0, 3)], fast, where == 3) : dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
        const uint32_t mark = display ? MARK : 0;
        for (int i = 0; i < DW * DH; ++i) model[i] = panel[i] = mark;
        const bool     transparent = rnd(0, 2) == 0;
        const int      n = rnd(0, 2) ? 1 : rnd(2, 4), kind = n > 1 ? rnd(0, 2) : 0;
        const unsigned turn = (unsigned)rnd(0, 7);
        const bool     whole = rnd(0, 4) == 0;
        const int      rx = whole ? 0 : rnd(-3, SW), ry = whole ? 0 : rnd(-3, SH), rw = whole ? SW : rnd(0, SW + 4), rh = whole ? SH : rnd(0, SH + 4);
        const int      x = rnd(-n * SW, DW), y = rnd(-n * SH, DH);
        const uint32_t flags = (transparent ? DGX_VSCREEN_TRANSPARENT : 0) | DGX_VSCREEN_SCALE(n) | smooth_flag[kind] | DGX_VSCREEN_ORIENTED(turn);
        const int      taken_before = indexed_taken;
        windows = single_pixels = sent_pixels = 0;
        if (whole) dgx_vscreen8_to_screen16(dst, x, y, src, lut, flags);
        else dgx_vscreen8_region_to_screen16(dst, x, y, src, rx, ry, rw, rh, lut, flags);
        indexed_rounds += indexed_taken != taken_before;
        model_transfer(source_values, x, y, rx, ry, rw, rh, n, ref_smooth_of[kind], turn, transparent, lut);
        int wrong = 0, drawn = 0;
        for (int i = 0; i < DW * DH; ++i) {
            const uint32_t got = display ? panel[i] : dgx_get_pixel(dst, i % DW, i / DW);
            wrong += got != model[i];
            drawn += model[i] != mark;
        }
        if (wrong) {
            fprintf(stderr, "through a table to %s, %s n %d smooth %d turn %u, region %d,%d %dx%d at %d,%d: %d wrong\n",
                    where == 0 ? "a bus" : where == 1 ? "a DSI bus" : where == 3 ? "a display read back" : "a virtual screen",
                    transparent ? "transparent," : "", n, kind, turn, rx, ry, rw, rh, x, y, wrong);
            ++check_failures;
        }
        if (display) {
            if (!(transparent && where == 3)) CHECK(single_pixels == 0);
            if (!transparent && !fast) CHECK(windows <= 1 && sent_pixels == drawn);
        }
        dgx_screen_destroy(&src);
        if (!display) dgx_screen_destroy(&dst);
    }
    /* the faster way took turned and smoothed pictures too */
    CHECK(indexed_rounds > 250 && indexed_turned > 200 && indexed_smoothed > 50);
}

/* a 1-bit picture onto a screen of pages, as an SSD1306 keeps it: pixel by pixel, every flag */
static void test_pages(void)
{
    for (int round = 0; round < 800; ++round) {
        dgx_screen_t *src = dgx_vscreen_init(SW, SH, 1, DgxScreenRGB), *dst = dgx_bw_init(DW, 32);
        for (int i = 0; i < SW * SH; ++i) {
            source_values[i] = (uint32_t)rnd(0, 1);
            dgx_set_pixel(src, i % SW, i / SW, source_values[i]);
        }
        for (int i = 0; i < DW * DH; ++i) model[i] = 0;
        const bool     transparent = rnd(0, 1);
        const int      n = rnd(1, 4), kind = n > 1 ? rnd(0, 2) : 0;
        const unsigned turn = (unsigned)rnd(0, 7);
        const int      x = rnd(-n * SW, DW), y = rnd(-n * SH, DH);
        if (transparent) {
            /* what is not put stays: the destination is all set first */
            dgx_fill_rectangle(dst, 0, 0, DW, 32, 1);
            for (int i = 0; i < DW * DH; ++i) model[i] = 1;
        }
        dgx_vscreen_to_screen(dst, x, y, src, (transparent ? DGX_VSCREEN_TRANSPARENT : 0) | DGX_VSCREEN_SCALE(n) | smooth_flag[kind] | DGX_VSCREEN_ORIENTED(turn));
        model_transfer(source_values, x, y, 0, 0, SW, SH, n, ref_smooth_of[kind], turn, transparent, NULL);
        int wrong = 0;
        for (int i = 0; i < DW * 32 && i < DW * DH; ++i) wrong += dgx_get_pixel(dst, i % DW, i / DW) != model[i];
        if (wrong) {
            fprintf(stderr, "pages: n %d smooth %d turn %u at %d,%d: %d wrong\n", n, kind, turn, x, y, wrong);
            ++check_failures;
        }
        dgx_screen_destroy(&src);
        dgx_screen_destroy(&dst);
    }
}

/* the calls of release 0.6 the header keeps: the pixels of the calls they stand for, and the names still functions */
static void test_old_calls(void)
{
    void (*const whole)(dgx_screen_t *, int, int, dgx_screen_t *, uint32_t) = dgx_vscreen_to_screen;
    void (*const region)(dgx_screen_t *, int, int, dgx_screen_t *, int, int, int, int, uint32_t) = dgx_vscreen_region_to_screen;
    dgx_screen_t *src = dgx_vscreen_init(SW, SH, 16, DgxScreenRGB);
    dgx_screen_t *old = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB), *now = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < SW * SH; ++i) dgx_set_pixel(src, i % SW, i / SW, (uint32_t)rnd(0, 2) * 0x1234u);
        dgx_fill_rectangle(old, 0, 0, DW, DH, 0x5555), dgx_fill_rectangle(now, 0, 0, DW, DH, 0x5555);
        const int  x = rnd(-SW, DW), y = rnd(-SH, DH), rx = rnd(0, SW - 1), ry = rnd(0, SH - 1), rw = rnd(1, SW), rh = rnd(1, SH);
        const bool holes = rnd(0, 1);
        switch (round % 3) {
        case 0:
            dgx_vscreen_to_screen(old, x, y, src);
            whole(now, x, y, src, 0);
            break;
        case 1:
            dgx_vscreen_region_to_screen(old, x, y, src, rx, ry, rw, rh);
            region(now, x, y, src, rx, ry, rw, rh, 0);
            break;
        default:
            dgx_vscreen_to_vscreen(old, x, y, src, holes);
            whole(now, x, y, src, holes ? DGX_VSCREEN_TRANSPARENT : 0);
            break;
        }
        int wrong = 0;
        for (int i = 0; i < DW * DH; ++i) wrong += dgx_get_pixel(old, i % DW, i / DW) != dgx_get_pixel(now, i % DW, i / DW);
        CHECK(wrong == 0);
    }
    dgx_screen_destroy(&src), dgx_screen_destroy(&old), dgx_screen_destroy(&now);
}

/* regions and places far off any screen, at every turn and the largest scale: no int overflows under UBSan, and nothing is drawn */
static void test_far(void)
{
    static const int far[] = {INT_MIN, -(1 << 30), -(1 << 23) - 5, -(1 << 21) - 1, -100000, 100000, (1 << 21) + 1, (1 << 23) + 5, (1 << 30), INT_MAX};
    const int        n     = (int)(sizeof(far) / sizeof(far[0]));
    dgx_screen_t    *src = dgx_vscreen_init(SW, SH, 16, DgxScreenRGB), *dst = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
    for (int i = 0; i < SW * SH; ++i) dgx_set_pixel(src, i % SW, i / SW, 0x1234u);
    dgx_fill_rectangle(dst, 0, 0, DW, DH, 0);
    for (unsigned turn = 0; turn < 8; ++turn) {
        for (int a = 0; a < n; ++a) {
            for (int b = 0; b < n; ++b) {
                const uint32_t flags = DGX_VSCREEN_SCALE(255) | DGX_VSCREEN_ORIENTED(turn);
                /* the place far off, the region whole */
                dgx_vscreen_region_to_screen(dst, far[a], far[b], src, 0, 0, SW, SH, flags);
                /* the region far off and as wide as can be, the place on the screen */
                dgx_vscreen_region_to_screen(dst, 0, 0, src, far[a], far[b], INT_MAX, INT_MAX, flags);
            }
        }
    }
    /* around the bound of the place worked out in an int: below it nothing overflows, above it the place is worked out in 64 bits */
    static const int edge[] = {(1 << 21) - 1, 1 << 21, (1 << 21) + 1, (3 << 21) - 1, 3 << 21};
    for (unsigned turn = 0; turn < 8; ++turn) {
        for (int a = 0; a < (int)(sizeof(edge) / sizeof(edge[0])); ++a) {
            const uint32_t flags = DGX_VSCREEN_SCALE(255) | DGX_VSCREEN_ORIENTED(turn);
            dgx_vscreen_region_to_screen(dst, 0, 0, src, edge[a], edge[a], edge[a], edge[a], flags);
            dgx_vscreen_region_to_screen(dst, 0, 0, src, -edge[a], -edge[a], edge[a], edge[a], flags);
            dgx_vscreen_region_to_screen(dst, -edge[a], -edge[a], src, 0, 0, edge[a], edge[a], flags);
        }
    }
    int drawn = 0;
    for (int i = 0; i < DW * DH; ++i) drawn += dgx_get_pixel(dst, i % DW, i / DW) != 0;
    CHECK(drawn == 0);
    /* a region from far off that reaches into the source: what of it the source has goes where the whole region puts it */
    dgx_vscreen_region_to_screen(dst, -(1 << 30), 0, src, -(1 << 30), 0, (1 << 30) + 5, SH, 0);
    for (int i = 0; i < DW * DH; ++i) CHECK((dgx_get_pixel(dst, i % DW, i / DW) != 0) == (i % DW < 5 && i / DW < SH));
    dgx_screen_destroy(&src), dgx_screen_destroy(&dst);
}

/*
 * A display whose bus has no buffer: the pixels go to it one by one, through
 * a table or as they are, and none is written through the buffer it lacks.
 * The scale macro keeps 255 for anything larger.
 */
static void test_no_buffer(void)
{
    static uint16_t lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)(i * 40503u);
    pixel_bytes = 2;
    for (int round = 0; round < 200; ++round) {
        dgx_screen_t *src8 = dgx_vscreen_init(SW, SH, 8, DgxScreenRGB), *src16 = dgx_vscreen_init(SW, SH, 16, DgxScreenRGB);
        dgx_screen_t *lcd  = panel_screen(16, 0, false, false);
        bus.buffer         = NULL;
        const bool table = rnd(0, 1);
        for (int i = 0; i < SW * SH; ++i) {
            source_values[i] = (uint32_t)rnd(0, table ? 255 : 0xffff);
            dgx_set_pixel(table ? src8 : src16, i % SW, i / SW, source_values[i]);
        }
        for (int i = 0; i < DW * DH; ++i) model[i] = panel[i] = MARK;
        const int      n = rnd(1, 3), x = rnd(-n * SW, DW), y = rnd(-n * SH, DH);
        const uint32_t flags = DGX_VSCREEN_SCALE(n);
        single_pixels        = 0;
        if (table) dgx_vscreen8_to_screen16(lcd, x, y, src8, lut, flags);
        else dgx_vscreen_to_screen(lcd, x, y, src16, flags);
        model_transfer(source_values, x, y, 0, 0, SW, SH, n, REF_BLOCKS, 0, false, table ? lut : NULL);
        int wrong = 0, drawn = 0;
        for (int i = 0; i < DW * DH; ++i) wrong += panel[i] != model[i], drawn += model[i] != MARK;
        CHECK(wrong == 0 && single_pixels == drawn);
        dgx_screen_destroy(&src8), dgx_screen_destroy(&src16);
    }
    CHECK(DGX_VSCREEN_SCALE(4) == 0x400u && DGX_VSCREEN_SCALE(255) == 0xff00u && DGX_VSCREEN_SCALE(256) == 0xff00u && DGX_VSCREEN_SCALE(300) == 0xff00u);
}

/* a screen of every depth, or of pages of eight rows, with values of 0 among the others */
static dgx_screen_t *onto_screen(int kind, int width, int height, const dgx_screen_t *like)
{
    static const uint8_t bits[] = {1, 4, 8, 12, 16, 24};
    dgx_screen_t        *scr    = kind < 6 ? dgx_vscreen_init(width, height, bits[kind], DgxScreenRGB) : dgx_bw_init(width, height);
    for (int i = 0; i < width * height; ++i) {
        const int x = i % width, y = i / width;
        if (like) dgx_set_pixel(scr, x, y, dgx_get_pixel((dgx_screen_t *)like, x, y));
        else dgx_set_pixel(scr, x, y, rnd(0, 2) ? (uint32_t)rnd(1, 0x7fffffff) & (kind < 6 ? (1u << bits[kind]) - 1u : 1u) : 0u);
    }
    return scr;
}

/*
 * A screen put onto itself, the place a few pixels off the region so that
 * it covers what is read: the pixels of a transfer from a copy of the screen
 * as it was, at every depth, with holes, under a layer, made larger and
 * turned.
 */
static void test_onto_itself(void)
{
    static uint16_t layer_lut[256];
    for (int i = 0; i < 256; ++i) layer_lut[i] = (uint16_t)rnd(0, 0xffff);
    int plain = 0, moved = 0, layered = 0, larger = 0;
    for (int round = 0; round < 6000; ++round) {
        /* rows longer than the pieces a row moving along itself goes by, too */
        const int      kind = rnd(0, 6), width = rnd(0, 3) ? DW : 150, height = kind == 6 ? 32 : DH;
        dgx_screen_t  *self = onto_screen(kind, width, height, NULL), *dst = onto_screen(kind, width, height, self);
        dgx_screen_t  *copy = onto_screen(kind, width, height, self);
        dgx_screen_t  *layer = NULL, *layer_too = NULL;
        const bool     transparent = rnd(0, 1), upright = rnd(0, 3);
        const int      n = upright ? 1 : rnd(1, 3), smooth = n > 1 ? rnd(0, 2) : 0, reach = upright ? 4 : 10;
        const unsigned turn = upright ? 0u : (unsigned)rnd(0, 7);
        const int      rx = rnd(-2, width - 1), ry = rnd(-2, height - 1), rw = rnd(1, width), rh = rnd(1, height);
        const int      x = rx + rnd(-reach, reach), y = ry + rnd(-reach, reach);
        if (kind == 4 && rnd(0, 2) == 0) {
            /* the same layer over both screens, each a layer of its own */
            layer = dgx_vscreen_init(20, 15, 8, DgxScreenRGB);
            for (int i = 0; i < 20 * 15; ++i) dgx_set_pixel(layer, i % 20, i / 20, rnd(0, 1) ? (uint32_t)rnd(1, 255) : 0u);
            layer_too        = dgx_vscreen_clone(layer);
            const int lx = rnd(-5, width - 10), ly = rnd(-5, height - 8), alpha = rnd(0, 255);
            CHECK(dgx_overlay_set(self, layer, lx, ly, layer_lut, (uint8_t)alpha) && dgx_overlay_set(dst, layer_too, lx, ly, layer_lut, (uint8_t)alpha));
            ++layered;
        }
        const uint32_t flags = (transparent ? DGX_VSCREEN_TRANSPARENT : 0) | DGX_VSCREEN_SCALE(n) | smooth_flag[smooth] | DGX_VSCREEN_ORIENTED(turn);
        dgx_vscreen_region_to_screen(self, x, y, self, rx, ry, rw, rh, flags);
        dgx_vscreen_region_to_screen(dst, x, y, copy, rx, ry, rw, rh, flags);
        int wrong = 0, changed = 0;
        for (int i = 0; i < width * height; ++i) {
            const uint32_t got = dgx_get_pixel(self, i % width, i / width);
            wrong += got != dgx_get_pixel(dst, i % width, i / width);
            changed += got != dgx_get_pixel(copy, i % width, i / width);
        }
        if (wrong) {
            fprintf(stderr, "onto itself: kind %d width %d transparent %d layer %d n %d smooth %d turn %u region %d,%d %dx%d to %d,%d: %d wrong\n", kind, width,
                    transparent, layer != NULL, n, smooth, turn, rx, ry, rw, rh, x, y, wrong);
            ++check_failures;
        }
        if (changed) ++*(n > 1 || turn ? &larger : x != rx || y != ry ? &moved : &plain);
        dgx_screen_destroy(&self), dgx_screen_destroy(&dst), dgx_screen_destroy(&copy);
        dgx_screen_destroy(&layer), dgx_screen_destroy(&layer_too);
    }
    CHECK(moved > 3000 && larger > 600 && layered > 200);
}

int main(void)
{
    test_same_depth();
    test_lut();
    test_pages();
    test_old_calls();
    test_far();
    test_no_buffer();
    test_onto_itself();
    CHECK_DONE();
}
