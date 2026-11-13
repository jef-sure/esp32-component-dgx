/*
 * A transfer with DGX_VSCREEN_ALPHA(a): every pixel it puts is mixed with
 * what lies under it, (picture × w + under × (32 − w)) / 32 a channel. What
 * a transfer puts, and in which color, is found by putting it twice without
 * the flag, on two screens whose pixels all differ: a pixel put is the same
 * on both. The transfer with the flag must then give the mix of that color
 * with what was there, on a virtual screen of every depth and on a display
 * behind a bus, read back or not (black under it then), with every other
 * flag, cut at every edge, an overlay layer over it, and onto itself. The
 * faster ways of a bus, which put colors over what was there, must not be
 * taken.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_overlay.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"
#include "vscreen/dgx_alpha_priv.h"
#include "vscreen/dgx_overlay_priv.h"

static uint32_t seed = 4242;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

static uint32_t rnd32(void)
{
    return (uint32_t)rnd(0, 0xffff) << 16 | (uint32_t)rnd(0, 0xffff);
}

/* a value of a pixel of `bits`, every bit a pixel keeps random */
static uint32_t value_of(int bits, uint32_t r)
{
    switch (bits) {
    case 1:
        return r & 1u;
    case 4:
        return r & 0xf0u;
    case 8:
        return r & 0xffu;
    case 12:
        return r & 0xfff0u;
    case 16:
        return r & 0xffffu;
    case 18:
        return r & 0xfcfcfcu;
    case 24:
        return r & 0xffffffu;
    default:
        return r;
    }
}

/* the same value with one bit of a channel turned over: every pixel of the second screen differs from the first */
static uint32_t other_of(int bits, uint32_t v)
{
    return bits == 4 || bits == 12 ? v ^ 0x10u : bits == 18 ? v ^ 0x04u : v ^ 1u;
}

/* the mix of the model: each channel by itself */
static uint32_t model_mix(int bits, uint32_t under, uint32_t color, uint32_t w)
{
    if (bits == 1) return w >= 16 ? color : under;
    static const struct {
        int      bits, n;
        unsigned shift[4];
        uint32_t mask;
    } layouts[] = {{4, 1, {4}, 15},           {8, 1, {0}, 255},         {12, 3, {12, 8, 4}, 15},
                   {16, 0, {0}, 0},           {18, 3, {18, 10, 2}, 63}, {24, 3, {16, 8, 0}, 255},
                   {32, 4, {24, 16, 8, 0}, 255}};
    if (bits == 16) {
        const unsigned shift[3] = {11, 5, 0}, mask[3] = {31, 63, 31};
        uint32_t       out      = 0;
        for (int c = 0; c < 3; ++c) {
            const uint32_t u = (under >> shift[c]) & mask[c], v = (color >> shift[c]) & mask[c];
            out |= ((v * w + u * (32 - w)) >> 5) << shift[c];
        }
        return out;
    }
    for (size_t l = 0; l < sizeof(layouts) / sizeof(layouts[0]); ++l) {
        if (layouts[l].bits != bits) continue;
        uint32_t out = 0;
        for (int c = 0; c < layouts[l].n; ++c) {
            const uint32_t u = (under >> layouts[l].shift[c]) & layouts[l].mask, v = (color >> layouts[l].shift[c]) & layouts[l].mask;
            out |= ((v * w + u * (32 - w)) >> 5) << layouts[l].shift[c];
        }
        return out;
    }
    return 0;
}

static uint32_t weight_of(uint8_t alpha)
{
    return (alpha * 32u + 127u) / 255u;
}

/* ---- a display behind a bus: a window and its pixels of `panel_bytes` each, the high byte first ---- */

#define DW 26
#define DH 21
static uint32_t panel[DW * DH];
static int      panel_bits, panel_bytes, area_l, area_r, area_b, area_x, area_y;
static int      fast_taken;    /* rows of memory put as they lie, and two colors mixed: what a transfer that mixes must not take */
static int      indexed_taken; /* pictures of indices, which the bus mixes itself */

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    area_l = left, area_r = right, area_b = bottom, area_x = left, area_y = top;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    (void)scr;
    for (uint32_t i = 0; i + panel_bytes <= lenbits / 8; i += panel_bytes) {
        uint32_t v = 0;
        for (int b = 0; b < panel_bytes; ++b) v = v << 8 | data[i + b];
        if (area_y <= area_b && area_x < DW && area_y < DH) panel[area_x + area_y * DW] = v;
        if (++area_x > area_r) area_x = area_l, ++area_y;
    }
}

static void panel_wait(dgx_screen_t *scr)
{
    (void)scr;
}

static void panel_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr;
    if (x >= 0 && y >= 0 && x < DW && y < DH) panel[x + y * DW] = color;
}

static uint32_t panel_get_pixel(dgx_screen_t *scr, int x, int y)
{
    (void)scr;
    return x >= 0 && y >= 0 && x < DW && y < DH ? panel[x + y * DW] : 0;
}

static void panel_update(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

/*
 * The faster ways, put as a DSI bus puts them, and counted: rows of memory as they lie, and indices through the
 * table of upright squares. They put the colors over what was there, so a transfer that mixes with it must not take
 * them; one over black, through a table of colors made darker, may.
 */
static bool panel_write_rows(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *rows,
                             uint32_t pitch)
{
    (void)bus;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uint32_t v = 0;
            for (int b = 0; b < panel_bytes; ++b) v = v << 8 | rows[y * pitch + x * panel_bytes + b];
            panel[left + x + (top + y) * DW] = v;
        }
    ++fast_taken;
    return true;
}

static bool panel_write_indexed(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height,
                                const dgx_bus_indexed_t *picture)
{
    (void)bus;
    if (picture->smooth || picture->orientation) return false; /* those come the other way */
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            /* an index through the table, or a color of 16 bits kept the high byte first */
            const uint8_t *at = picture->pixels + (picture->y + y) / picture->scale * picture->pitch +
                                (picture->x + x) / picture->scale * (picture->bytes == 2 ? 2u : 1u);
            uint16_t color = picture->bytes == 2 ? (uint16_t)(at[0] << 8 | at[1]) : picture->lut[*at];
            /* mixed with what the display has, as a DSI bus mixes, before the layer; a mask puts its ink by the index */
            if (picture->under) color = (uint16_t)model_mix(16, panel[left + x + (top + y) * DW], color, 32u - picture->under);
            if (picture->unders) color = (uint16_t)model_mix(16, panel[left + x + (top + y) * DW], picture->ink, 32u - picture->unders[color & 0xffu]);
            if (picture->overlay) color = dgx_overlay_pixel(picture->overlay, left + x, top + y, color);
            panel[left + x + (top + y) * DW] = color;
        }
    ++indexed_taken;
    return true;
}

static bool panel_write_ink(struct _dgx_bus_protocols_t *bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *ink,
                            uint32_t pitch, uint32_t paper, uint32_t color)
{
    (void)bus, (void)left, (void)top, (void)width, (void)height, (void)ink, (void)pitch, (void)paper, (void)color;
    ++fast_taken;
    return false;
}

static uint8_t               bus_buffer[40];
static dgx_bus_protocols_t   bus;
static dgx_screen_with_bus_t sbus;

static dgx_screen_t *panel_screen(int bits, bool read)
{
    memset(&bus, 0, sizeof(bus));
    memset(&sbus, 0, sizeof(sbus));
    bus.buffer = bus_buffer, bus.buffer_len = sizeof(bus_buffer);
    bus.write_rows = panel_write_rows, bus.write_indexed = panel_write_indexed, bus.write_ink = panel_write_ink;
    sbus.bus           = &bus;
    dgx_screen_t *lcd  = &sbus.scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = (uint8_t)bits, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait_buffer = panel_wait;
    lcd->set_pixel = panel_set_pixel, lcd->update_screen = panel_update;
    lcd->get_pixel = read ? panel_get_pixel : NULL;
    panel_bits = bits, panel_bytes = bits == 16 ? 2 : 3;
    return lcd;
}

/* ---- the checks ---- */

static uint16_t layer_lut[256];

/* the layer of the test over the display at (lx, ly), alpha 160: what (x, y) gets from `color` */
static uint32_t over_of(dgx_screen_t *layer, int lx, int ly, int x, int y, uint32_t color)
{
    const int ix = x - lx, iy = y - ly;
    if (!layer || ix < 0 || iy < 0 || ix >= layer->width || iy >= layer->height) return color;
    const uint32_t index = dgx_get_pixel(layer, ix, iy);
    return index ? model_mix(16, color, layer_lut[index], weight_of(160)) : color;
}

static int transfers, mixed_pixels, table_fast;

/*
 * One transfer, the same depth or 8 bits through a table, onto a virtual screen of `bits` or a display behind a
 * bus, with random flags, place and alpha.
 */
static void one(int bits, bool through_table, int where, int round)
{
    /* where: 0 a virtual screen, 1 a display that cannot be read, 2 one read back as a DSI one is */
    const int     sbits = through_table ? 8 : bits;
    dgx_screen_t *src = dgx_vscreen_init(rnd(3, 11), rnd(3, 9), (uint8_t)sbits, DgxScreenRGB);
    dgx_screen_t *a = dgx_vscreen_init(DW, DH, (uint8_t)bits, DgxScreenRGB), *b = dgx_vscreen_init(DW, DH, (uint8_t)bits, DgxScreenRGB);
    CHECK(src && a && b);
    static uint16_t lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)rnd(0, 0xffff);
    /* few values, 0 among them, so that smoothing and holes have something to do */
    uint32_t palette[4];
    for (int i = 0; i < 4; ++i) palette[i] = i ? value_of(sbits, rnd32()) : 0;
    for (int y = 0; y < src->height; ++y)
        for (int x = 0; x < src->width; ++x) dgx_set_pixel(src, x, y, palette[rnd(0, 3)]);
    /* what lies under: a pattern, and on the second screen the same with a bit turned over */
    static uint32_t under[DW * DH];
    for (int i = 0; i < DW * DH; ++i) {
        under[i] = value_of(bits, rnd32());
        dgx_set_pixel(a, i % DW, i / DW, under[i]);
        dgx_set_pixel(b, i % DW, i / DW, other_of(bits, under[i]));
    }
    const int      n = rnd(1, 3), turn = rnd(0, 1) ? rnd(0, 7) : 0;
    const bool     transparent = rnd(0, 2) == 0;
    const uint32_t smooth      = n > 1 && rnd(0, 1) ? (rnd(0, 1) ? DGX_VSCREEN_SMOOTH_SFX : DGX_VSCREEN_SMOOTH) : 0;
    const uint32_t flags = (transparent ? DGX_VSCREEN_TRANSPARENT : 0) | DGX_VSCREEN_SCALE(n) | smooth | DGX_VSCREEN_ORIENTED(turn) |
                           (rnd(0, 3) == 0 ? DGX_VSCREEN_EXACT : 0);
    const int     left = rnd(-2, src->width - 1), top = rnd(-2, src->height - 1), w = rnd(0, src->width + 2), h = rnd(0, src->height + 2);
    const int     x = rnd(-8, DW), y = rnd(-8, DH);
    const uint8_t alpha = (uint8_t)(round % 7 == 0 ? 0 : round % 7 == 1 ? 255 : round % 7 == 2 ? 128 : rnd(1, 254));
    const uint32_t wgt  = weight_of(alpha);
#define PUT(dst, f)                                                                                                    \
    (through_table ? dgx_vscreen8_region_to_screen16(dst, x, y, src, left, top, w, h, lut, f)                          \
                   : dgx_vscreen_region_to_screen(dst, x, y, src, left, top, w, h, f))
    PUT(a, flags);
    PUT(b, flags);
    /* the destination: the pattern under it, or its own marks on a display that cannot be read */
    dgx_screen_t *dst = where == 0 ? dgx_vscreen_init(DW, DH, (uint8_t)bits, DgxScreenRGB) : panel_screen(bits, where == 2);
    CHECK(dst);
    for (int i = 0; i < DW * DH; ++i) {
        if (where == 0) dgx_set_pixel(dst, i % DW, i / DW, under[i]);
        else panel[i] = where == 2 ? under[i] : 0x5a5a5au & ((1u << panel_bits) - 1u);
    }
    /* an overlay layer over a 16-bit one now and then */
    dgx_screen_t *layer = NULL;
    int           lx = 0, ly = 0;
    if (bits == 16 && rnd(0, 2) == 0) {
        layer = dgx_vscreen_init(12, 9, 8, DgxScreenRGB);
        CHECK(layer);
        for (int i = 0; i < 12 * 9; ++i) dgx_set_pixel(layer, i % 12, i / 12, rnd(0, 2) ? 0u : (uint32_t)rnd(1, 255));
        lx = rnd(-4, DW - 4), ly = rnd(-4, DH - 4);
        CHECK(dgx_overlay_set(dst, layer, lx, ly, layer_lut, 160));
    }
    fast_taken = indexed_taken = 0;
    PUT(dst, flags | DGX_VSCREEN_ALPHA(alpha));
#undef PUT
    /*
     * Mixed with what was there: not as rows of memory, nor two colors put over it. Over black through a table made
     * darker the two colors are those of that table; pictures of indices the bus mixes itself. From 252 on the weight
     * is 32: no mixing.
     */
    if (wgt < 32 && !(through_table && where == 1)) CHECK(fast_taken == 0);
    if (wgt < 32 && through_table) table_fast += indexed_taken;
    int wrong = 0;
    for (int py = 0; py < DH; ++py)
        for (int px = 0; px < DW; ++px) {
            const uint32_t pa = dgx_get_pixel(a, px, py), pb = dgx_get_pixel(b, px, py);
            const bool     put = pa == pb;
            const uint32_t was = where == 1 ? panel[px + py * DW] : under[px + py * DW];
            uint32_t       want;
            if (where == 1) {
                /* black under it; the marks stay where nothing is put */
                want = put ? over_of(layer, lx, ly, px, py, model_mix(bits, 0, pa, wgt)) : 0x5a5a5au & ((1u << panel_bits) - 1u);
            } else {
                want = put ? over_of(layer, lx, ly, px, py, model_mix(bits, was, pa, wgt)) : was;
                /* nothing of the picture: the transfer leaves everything as it was, the layer too */
                if (put && wgt == 0) want = was;
            }
            const uint32_t got = where == 0 ? dgx_get_pixel(dst, px, py) : panel[px + py * DW];
            wrong += got != want;
            mixed_pixels += put && wgt > 0 && wgt < 32;
        }
    if (wrong) {
        fprintf(stderr, "alpha: round %d bits %d table %d where %d n %d turn %d transparent %d smooth %u alpha %u: %d pixels wrong\n", round, bits,
                through_table, where, n, turn, transparent, (unsigned)smooth, alpha, wrong);
        ++check_failures;
    }
    ++transfers;
    if (layer) dgx_overlay_clear(dst), dgx_screen_destroy(&layer);
    if (where == 0) dgx_screen_destroy(&dst);
    else if (dst->overlay) dgx_overlay_forget(dst);
    dgx_screen_destroy(&src), dgx_screen_destroy(&a), dgx_screen_destroy(&b);
}

/*
 * A mask, DGX_VSCREEN_MASK: where the transfer puts an index, lut[255] goes over what lies there by that index of 255,
 * times the alpha, in 32 steps; on a display that cannot be read the picture goes through the table as without the
 * flag. Which index lands where is found by putting the picture through a table that gives every index itself.
 */
static int masked_pixels;

static void one_mask(int where, int round)
{
    dgx_screen_t *src = dgx_vscreen_init(rnd(3, 11), rnd(3, 9), 8, DgxScreenRGB);
    dgx_screen_t *a = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB), *b = dgx_vscreen_init(DW, DH, 16, DgxScreenRGB);
    CHECK(src && a && b);
    static uint16_t lut[256], indices[256];
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)rnd(0, 0xffff), indices[i] = (uint16_t)i;
    /* a soft shape: amounts 0, a few between and 255 */
    const uint32_t amounts[5] = {0, (uint32_t)rnd(1, 254), (uint32_t)rnd(1, 254), 255, (uint32_t)rnd(1, 254)};
    for (int y = 0; y < src->height; ++y)
        for (int x = 0; x < src->width; ++x) dgx_set_pixel(src, x, y, amounts[rnd(0, 4)]);
    static uint32_t under[DW * DH];
    for (int i = 0; i < DW * DH; ++i) {
        under[i] = (uint32_t)rnd(0, 0xffff);
        dgx_set_pixel(a, i % DW, i / DW, 0x1000u + (uint32_t)i % 0xe000u);
        dgx_set_pixel(b, i % DW, i / DW, 0x1000u + (uint32_t)i % 0xe000u + 0x0800u);
    }
    const int      n = rnd(1, 3), turn = rnd(0, 1) ? rnd(0, 7) : 0;
    const bool     transparent = rnd(0, 3) == 0;
    const uint32_t smooth      = n > 1 && rnd(0, 1) ? (rnd(0, 1) ? DGX_VSCREEN_SMOOTH_SFX : DGX_VSCREEN_SMOOTH) : 0;
    const uint32_t flags = (transparent ? DGX_VSCREEN_TRANSPARENT : 0) | DGX_VSCREEN_SCALE(n) | smooth | DGX_VSCREEN_ORIENTED(turn);
    const int      left = rnd(-2, src->width - 1), top = rnd(-2, src->height - 1), w = rnd(0, src->width + 2), h = rnd(0, src->height + 2);
    const int      x = rnd(-8, DW), y = rnd(-8, DH);
    const uint8_t  alpha = (uint8_t)(round % 4 == 0 ? 255 : round % 4 == 1 ? 0 : rnd(1, 254));
    dgx_vscreen8_region_to_screen16(a, x, y, src, left, top, w, h, indices, flags);
    dgx_vscreen8_region_to_screen16(b, x, y, src, left, top, w, h, indices, flags);
    dgx_screen_t *dst = where == 0 ? dgx_vscreen_init(DW, DH, 16, DgxScreenRGB) : panel_screen(16, where == 2);
    CHECK(dst);
    for (int i = 0; i < DW * DH; ++i) {
        if (where == 0) dgx_set_pixel(dst, i % DW, i / DW, under[i]);
        else panel[i] = where == 2 ? under[i] : 0x5a5au;
    }
    dgx_screen_t *layer = NULL;
    int           lx = 0, ly = 0;
    if (rnd(0, 2) == 0) {
        layer = dgx_vscreen_init(12, 9, 8, DgxScreenRGB);
        CHECK(layer);
        for (int i = 0; i < 12 * 9; ++i) dgx_set_pixel(layer, i % 12, i / 12, rnd(0, 2) ? 0u : (uint32_t)rnd(1, 255));
        lx = rnd(-4, DW - 4), ly = rnd(-4, DH - 4);
        CHECK(dgx_overlay_set(dst, layer, lx, ly, layer_lut, 160));
    }
    fast_taken = indexed_taken = 0;
    dgx_vscreen8_region_to_screen16(dst, x, y, src, left, top, w, h, lut, flags | DGX_VSCREEN_MASK | DGX_VSCREEN_ALPHA(alpha));
    if (where != 1) CHECK(fast_taken == 0);
    int wrong = 0;
    for (int py = 0; py < DH; ++py)
        for (int px = 0; px < DW; ++px) {
            const uint32_t pa = dgx_get_pixel(a, px, py), pb = dgx_get_pixel(b, px, py);
            const bool     put = pa == pb;
            const uint32_t index = pa & 0xffu, was = under[px + py * DW];
            uint32_t       want;
            if (where == 1) {
                /* not read: the table as it is, made darker by the alpha, black under it */
                want = put ? over_of(layer, lx, ly, px, py, model_mix(16, 0, lut[index], weight_of(alpha))) : 0x5a5au;
            } else {
                const uint32_t share = (index * alpha + 127u) / 255u, wt = weight_of((uint8_t)share);
                want = put ? over_of(layer, lx, ly, px, py, model_mix(16, was, lut[255], wt)) : was;
                /* nothing of the ink anywhere, the weight of 255 too: the transfer leaves everything as it was, the layer too */
                if (!weight_of((uint8_t)((255u * alpha + 127u) / 255u))) want = was;
                masked_pixels += put && wt > 0 && wt < 32;
            }
            const uint32_t got = where == 0 ? dgx_get_pixel(dst, px, py) : panel[px + py * DW];
            wrong += got != want;
        }
    if (wrong) {
        fprintf(stderr, "mask: round %d where %d n %d turn %d transparent %d smooth %u alpha %u: %d pixels wrong\n", round, where, n, turn, transparent,
                (unsigned)smooth, alpha, wrong);
        ++check_failures;
    }
    if (layer) dgx_overlay_clear(dst), dgx_screen_destroy(&layer);
    if (where == 0) dgx_screen_destroy(&dst);
    else if (dst->overlay) dgx_overlay_forget(dst);
    dgx_screen_destroy(&src), dgx_screen_destroy(&a), dgx_screen_destroy(&b);
}

/* onto itself: what lies there and what is put are both read before anything changes */
static void onto_itself(void)
{
    for (int round = 0; round < 400; ++round) {
        const int     bits = round % 2 ? 16 : 24, n = rnd(1, 2);
        dgx_screen_t *scr = dgx_vscreen_init(20, 16, (uint8_t)bits, DgxScreenRGB), *copy = dgx_vscreen_init(20, 16, (uint8_t)bits, DgxScreenRGB);
        dgx_screen_t *a = dgx_vscreen_init(20, 16, (uint8_t)bits, DgxScreenRGB), *b = dgx_vscreen_init(20, 16, (uint8_t)bits, DgxScreenRGB);
        CHECK(scr && copy && a && b);
        for (int i = 0; i < 20 * 16; ++i) {
            const uint32_t v = value_of(bits, rnd32());
            dgx_set_pixel(scr, i % 20, i / 20, v), dgx_set_pixel(copy, i % 20, i / 20, v);
            dgx_set_pixel(a, i % 20, i / 20, v), dgx_set_pixel(b, i % 20, i / 20, other_of(bits, v));
        }
        const int      left = rnd(0, 10), top = rnd(0, 8), w = rnd(1, 10), h = rnd(1, 8), x = rnd(-3, 18), y = rnd(-3, 14);
        const uint32_t flags = DGX_VSCREEN_SCALE(n) | DGX_VSCREEN_ORIENTED(rnd(0, 1) ? rnd(0, 7) : 0);
        const uint8_t  alpha = (uint8_t)rnd(1, 254);
        /* what is put, from the copy as it was */
        dgx_vscreen_region_to_screen(a, x, y, copy, left, top, w, h, flags);
        dgx_vscreen_region_to_screen(b, x, y, copy, left, top, w, h, flags);
        dgx_vscreen_region_to_screen(scr, x, y, scr, left, top, w, h, flags | DGX_VSCREEN_ALPHA(alpha));
        int wrong = 0;
        for (int py = 0; py < 16; ++py)
            for (int px = 0; px < 20; ++px) {
                const uint32_t pa = dgx_get_pixel(a, px, py), was = dgx_get_pixel(copy, px, py);
                const uint32_t want = pa == dgx_get_pixel(b, px, py) ? model_mix(bits, was, pa, weight_of(alpha)) : was;
                wrong += dgx_get_pixel(scr, px, py) != want;
            }
        if (wrong) {
            fprintf(stderr, "alpha onto itself: round %d bits %d n %d: %d pixels wrong\n", round, bits, n, wrong);
            ++check_failures;
        }
        dgx_screen_destroy(&scr), dgx_screen_destroy(&copy), dgx_screen_destroy(&a), dgx_screen_destroy(&b);
    }
}

/* the flag: 255 is no flag at all, and the weights are those of the overlay layer */
static void test_flag(void)
{
    CHECK(DGX_VSCREEN_ALPHA(255) == 0);
    CHECK(dgx_alpha_weight(0) == 32 && dgx_alpha_weight(DGX_VSCREEN_ALPHA(0)) == 0 && dgx_alpha_weight(DGX_VSCREEN_ALPHA(128)) == 16);
    CHECK((DGX_VSCREEN_ALPHA(7) & 0x00ffffffu) == 0);
    for (int a = 0; a < 256; ++a) CHECK(dgx_alpha_weight(DGX_VSCREEN_ALPHA(a) | DGX_VSCREEN_TRANSPARENT | DGX_VSCREEN_SCALE(3)) == weight_of((uint8_t)a));
    /* the 16-bit mix of the rows is the model's */
    for (int i = 0; i < 20000; ++i) {
        const uint32_t u = (uint32_t)rnd(0, 0xffff), c = (uint32_t)rnd(0, 0xffff), w = (uint32_t)rnd(0, 32);
        CHECK(dgx_alpha_mix16((uint16_t)u, (uint16_t)c, w) == model_mix(16, u, c, w));
    }
}

int main(void)
{
    for (int i = 0; i < 256; ++i) layer_lut[i] = (uint16_t)(i * 0x0101u ^ 0x3c3c);
    test_flag();
    static const int depths[] = {1, 4, 8, 12, 16, 18, 24, 32};
    for (int round = 0; round < 1200; ++round) {
        for (size_t d = 0; d < sizeof(depths) / sizeof(depths[0]); ++d) one(depths[d], false, 0, round);
        one(16, true, 0, round);
        for (int where = 1; where <= 2; ++where) {
            one(16, false, where, round);
            one(24, false, where, round);
            one(16, true, where, round);
        }
    }
    for (int round = 0; round < 1500; ++round)
        for (int where = 0; where <= 2; ++where) one_mask(where, round);
    onto_itself();
    fprintf(stderr, "%d transfers, %d pixels mixed\n", transfers, mixed_pixels);
    fprintf(stderr, "%d pixels under a mask mixed\n", masked_pixels);
    CHECK(mixed_pixels > 100000 && table_fast > 50 && masked_pixels > 20000);
    CHECK_DONE();
}
