/*
 * Handwritten text drawn from a context, dgx_hw_context_t: the calls that
 * take it draw what the calls with every setting as an argument draw, the
 * defaults of its members, and the boxes of pixels, dgx_hw_bounds() and
 * dgx_hw_morph_bounds(), against what is really painted.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"
#include "fonts/font0ss.h"

#define W 640
#define H 440

/*
 * The screens are virtual ones of 8 and 16 bits a pixel, and the checks are
 * many whole screens: they read the memory of a screen, a row at a time.
 */
static uint8_t *row_of(dgx_screen_t *s, int y)
{
    return ((dgx_vscreen_t *)s)->v_array + (size_t)y * W * (s->color_bits / 8);
}

static const uint8_t clean[W * 2];

static bool row_is_clean(dgx_screen_t *s, int y)
{
    return memcmp(row_of(s, y), clean, (size_t)W * (s->color_bits / 8)) == 0;
}

/* how many rows of two screens differ */
static int same(dgx_screen_t *a, dgx_screen_t *b)
{
    int differ = 0;
    for (int y = 0; y < H; ++y)
        differ += memcmp(row_of(a, y), row_of(b, y), (size_t)W * (a->color_bits / 8)) != 0;
    return differ;
}

static void wipe(dgx_screen_t *s)
{
    memset(row_of(s, 0), 0, (size_t)W * H * (s->color_bits / 8));
}

/* the box of what is painted: false for a clean screen */
static bool ink_box(dgx_screen_t *s, int *l, int *t, int *r, int *b)
{
    const int bytes = s->color_bits / 8;
    bool      any   = false;
    for (int y = 0; y < H; ++y) {
        if (row_is_clean(s, y)) continue;
        const uint8_t *row   = row_of(s, y);
        int            first = 0, last = W * bytes - 1;
        while (!row[first])
            ++first;
        while (!row[last])
            --last;
        first /= bytes, last /= bytes;
        if (!any || first < *l) *l = first;
        if (!any || last > *r) *r = last;
        if (!any) *t = y;
        *b  = y;
        any = true;
    }
    return any;
}

/* how many pixels are painted */
static int ink(dgx_screen_t *s)
{
    const int bytes = s->color_bits / 8;
    int       n     = 0;
    for (int y = 0; y < H; ++y) {
        if (row_is_clean(s, y)) continue;
        const uint8_t *row = row_of(s, y);
        for (int x = 0; x < W; ++x)
            n += row[x * bytes] || row[x * bytes + bytes - 1];
    }
    return n;
}

static int updates, up_left, up_right, up_top, up_bottom;

static void record(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr;
    ++updates;
    up_left = left, up_right = right, up_top = top, up_bottom = bottom;
}

static const char *const texts[] = {"Ёжик, 15!", "соревнований", "привет\nёж", "у", "08", "её ёж"};
#define TEXTS ((int)(sizeof(texts) / sizeof(*texts)))

/* a size across, a size down as the context takes them (0: as across), and the two as the calls with arguments take them */
static const struct
{
    float cx, cy, sx, sy;
} sizes[] = {
    {0.2f,  0,     0.2f,  0.2f },
    {0.4f,  0,     0.4f,  0.4f },
    {0.5f,  0.5f,  0.5f,  0.5f },
    {0.33f, 0.7f,  0.33f, 0.7f },
    {0.6f,  0.25f, 0.6f,  0.25f},
    {0.71f, 0,     0.71f, 0.71f},
    {1.5f,  0.5f,  1.5f,  0.5f },
};
#define SIZES ((int)(sizeof(sizes) / sizeof(*sizes)))

static const int pens[] = {1, 2, 3, 4, 7, 10, DGX_PEN_SOFT | 1, DGX_PEN_SOFT | 2, DGX_PEN_SOFT | 3, DGX_PEN_SOFT | 6, DGX_PEN_SOFT | 9};
#define PENS ((int)(sizeof(pens) / sizeof(*pens)))

/* dgx_hw_draw() against dgx_hw_draw_text_xy(), on a screen of ink (a soft pen is soft there) and on one of colors */
static void draws_the_same(void)
{
    dgx_font_t *font = font0ss();
    for (int bits = 8; bits <= 16; bits += 8) {
        dgx_screen_t  *a = dgx_vscreen_init(W, H, bits, DgxScreenRGB), *b = dgx_vscreen_init(W, H, bits, DgxScreenRGB);
        const uint32_t color   = bits == 8 ? 0xe0 : 0xffff;
        int            painted = 0;
        for (int k = 0; k < TEXTS; ++k)
            for (int z = 0; z < SIZES; ++z)
                for (int p = 0; p < PENS; ++p)
                    for (int joined = 0; joined < 2; ++joined) {
                        const dgx_hw_context_t ctx = {.scr     = a,
                                                      .font    = font,
                                                      .scale_x = sizes[z].cx,
                                                      .scale_y = sizes[z].cy,
                                                      .width   = pens[p],
                                                      .color   = color,
                                                      .joined  = joined != 0};
                        wipe(a), wipe(b);
                        dgx_hw_draw(&ctx, 30, 150, texts[k]);
                        dgx_hw_draw_text_xy(b, 30, 150, font, texts[k], sizes[z].sx, sizes[z].sy, pens[p], color, joined != 0);
                        const int differ = same(a, b);
                        if (differ)
                            fprintf(stderr, "%d bits, text %d size %d pen %#x joined %d: %d rows differ\n", bits, k, z, pens[p], joined, differ);
                        CHECK(differ == 0);
                        painted += ink(a) != 0;
                    }
        CHECK(painted == TEXTS * SIZES * PENS * 2);

        /* the defaults: a size of 0 across is a pixel a cell, a width of 0 a pen of 1, with the bits it has */
        {
            const dgx_hw_context_t zero = {.scr = a, .font = font, .color = color};
            wipe(a), wipe(b);
            dgx_hw_draw(&zero, 20, 200, "ёж");
            dgx_hw_draw_text(b, 20, 200, font, "ёж", 1.0f, 1, color, false);
            CHECK(same(a, b) == 0 && ink(a) > 200);
            const dgx_hw_context_t soft = {.scr = a, .font = font, .scale_y = 0.5f, .width = DGX_PEN_SOFT, .color = color, .joined = true};
            wipe(a), wipe(b);
            dgx_hw_draw(&soft, 20, 200, "ёж");
            dgx_hw_draw_text_xy(b, 20, 200, font, "ёж", 1.0f, 0.5f, DGX_PEN_SOFT | 1, color, true);
            CHECK(same(a, b) == 0 && ink(a) > 200);
            /* what is below nothing stays nothing, as in the calls with arguments */
            const dgx_hw_context_t none = {.scr = a, .font = font, .scale_x = 0.4f, .width = -3, .color = color};
            wipe(a);
            dgx_hw_draw(&none, 20, 200, "ёж");
            CHECK(ink(a) == 0);
        }
        dgx_screen_destroy(&a);
        dgx_screen_destroy(&b);
    }
}

/* a text written from a context goes as one begun with arguments, step for step; the context may go once it is begun */
static void writes_the_same(void)
{
    dgx_font_t         *font = font0ss();
    dgx_screen_t       *a = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *b = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    const char         *text     = "Ёжик, 15";
    const dgx_hw_pace_t pace     = dgx_hw_pace(font, 0.5f);
    static const int    widths[] = {3, DGX_PEN_SOFT | 4};
    for (int k = 0; k < 2; ++k) {
        for (int paced = 0; paced < 2; ++paced) {
            dgx_hw_writing_t wa, wb;
            dgx_hw_context_t ctx = {
                .scr = a, .font = font, .scale_x = 0.45f, .scale_y = k ? 0.3f : 0, .width = widths[k], .color = 0xff, .joined = true};
            if (paced) ctx.pace = pace;
            const float all = dgx_hw_text_effort(font, text, true, paced ? &pace : NULL);
            wipe(a), wipe(b);
            dgx_hw_writing_begin_with(&wa, &ctx, 12, 160, text);
            dgx_hw_writing_begin_xy(&wb, b, 12, 160, font, text, 0.45f, k ? 0.3f : 0.45f, widths[k], 0xff, true, paced ? &pace : NULL);
            memset(&ctx, 0x55, sizeof(ctx)); /* nothing of it is kept */
            CHECK(ink(a) == 0);              /* begun, nothing drawn */
            int  differ = 0, steps = 0;
            bool done_a = false, done_b = false;
            for (int i = 1; i <= 41 && !done_a; ++i) {
                const float effort = i == 41 ? INFINITY : all * i / 40;
                done_a             = dgx_hw_writing_draw_to(&wa, effort);
                done_b             = dgx_hw_writing_draw_to(&wb, effort);
                differ += same(a, b) + (done_a != done_b);
                ++steps;
            }
            CHECK(differ == 0 && done_a && done_b && steps > 30 && ink(a) > 500);
        }
    }
    dgx_screen_destroy(&a);
    dgx_screen_destroy(&b);
}

/* a frame of a morph from a context is the frame drawn with arguments */
static void morphs_the_same(void)
{
    dgx_font_t     *font = font0ss();
    dgx_screen_t   *a = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *b = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_hw_morph_t *m = dgx_hw_morph_create(font, "15", "08", false);
    CHECK(m != NULL);
    if (m) {
        static const float ts[] = {0, 0.3f, 0.5f, 1, -2, 7};
        for (int k = 0; k < 6; ++k) {
            const dgx_hw_context_t ctx = {
                .scr = a, .scale_x = 0.5f, .scale_y = k & 1 ? 0.8f : 0, .width = k & 2 ? DGX_PEN_SOFT | 5 : 3, .color = 0xff};
            wipe(a), wipe(b);
            dgx_hw_morph_draw_with(m, ts[k], &ctx, 40, 200);
            dgx_hw_morph_draw_xy(m, ts[k], b, 40, 200, 0.5f, k & 1 ? 0.8f : 0.5f, k & 2 ? DGX_PEN_SOFT | 5 : 3, 0xff);
            CHECK(same(a, b) == 0 && ink(a) > 100);
        }
        /* the defaults are those of a text */
        const dgx_hw_context_t zero = {.scr = a, .color = 0xff};
        wipe(a), wipe(b);
        dgx_hw_morph_draw_with(m, 0.5f, &zero, 20, 200);
        dgx_hw_morph_draw(m, 0.5f, b, 20, 200, 1.0f, 1, 0xff);
        CHECK(same(a, b) == 0 && ink(a) > 100);
        /* no morph, nothing drawn */
        wipe(a);
        dgx_hw_morph_draw_with(NULL, 0.5f, &zero, 20, 200);
        CHECK(ink(a) == 0);
    }
    dgx_hw_morph_destroy(&m);
    dgx_screen_destroy(&a);
    dgx_screen_destroy(&b);
}

/*
 * The box of a text against its ink, at several places: where a half rounds
 * to depends on the place. The box holds every pixel painted. It is not
 * always tight: a curve is drawn as straight pieces that stop short of where
 * it turns back, the spare pixel of an even width falls on either side, and
 * a half is taken both ways. So a side is exact as a rule, one pixel larger
 * often, and seldom more: three is the most seen over 60 000 texts at
 * random. A hard pen of an odd width has most of its boxes exact on all four
 * sides. On a screen of colors a soft pen draws as a hard one, and its box
 * is still that of both.
 */
static void bounds_hold_the_ink(void)
{
    dgx_font_t *font = font0ss();
    for (int bits = 8; bits <= 16; bits += 8) {
        dgx_screen_t  *s     = dgx_vscreen_init(W, H, bits, DgxScreenRGB);
        const uint32_t color = bits == 8 ? 0xff : 0xffff;
        for (int p = 0; p < PENS; ++p) {
            const bool soft = (pens[p] & DGX_PEN_SOFT) != 0, odd = (pens[p] & 1) != 0;
            int        cases = 0, outside = 0, exact = 0, most = 0, sides[2] = {0, 0}; /* sides looser than a pixel, and all */
            for (int k = 0; k < TEXTS; ++k)
                for (int z = 0; z < SIZES; ++z)
                    for (int joined = 0; joined < 2; ++joined) {
                        const dgx_hw_context_t ctx = {.scr     = s,
                                                      .font    = font,
                                                      .scale_x = sizes[z].cx,
                                                      .scale_y = sizes[z].cy,
                                                      .width   = pens[p],
                                                      .color   = color,
                                                      .joined  = joined != 0};
                        int                    l, t, w, h;
                        if (!dgx_hw_bounds(&ctx, texts[k], &l, &t, &w, &h)) {
                            CHECK(!"a text of the font has a box");
                            continue;
                        }
                        if (w + 24 > W || h + 24 > H) continue; /* too large for this screen at this size */
                        for (int place = 0; place < 3; ++place) {
                            const int x = 10 + place - l, y = 10 + 2 * place - t;
                            int       il = 0, it = 0, ir = 0, ib = 0;
                            wipe(s);
                            dgx_hw_draw(&ctx, x, y, texts[k]);
                            if (!ink_box(s, &il, &it, &ir, &ib)) {
                                CHECK(!"a text of the font leaves ink");
                                continue;
                            }
                            ++cases;
                            const int d[4] = {il - (x + l), it - (y + t), (x + l + w - 1) - ir, (y + t + h - 1) - ib};
                            bool      out  = false;
                            for (int side = 0; side < 4; ++side) {
                                out = out || d[side] < 0;
                                if (d[side] > most) most = d[side];
                                sides[0] += d[side] > 1, ++sides[1];
                            }
                            if (out && ++outside <= 3)
                                fprintf(stderr, "%d bits, text %d size %d pen %#x joined %d place %d: ink outside the box by %d %d %d %d\n", bits, k,
                                        z, pens[p], joined, place, -d[0], -d[1], -d[2], -d[3]);
                            exact += d[0] + d[1] + d[2] + d[3] == 0;
                        }
                    }
            const bool holds = cases > 150 && outside == 0 && most <= 3 && sides[0] * 8 < sides[1] && (!odd || soft || exact * 2 > cases);
            if (!holds)
                fprintf(
                    stderr,
                    "%d bits, %s pen %d: %d texts placed, %d exact, %d with ink outside, the loosest side %d, %d sides of %d looser than a pixel\n",
                    bits, soft ? "soft" : "hard", pens[p] & 0xffff, cases, exact, outside, most, sides[0], sides[1]);
            CHECK(holds);
        }
        dgx_screen_destroy(&s);
    }
}

static void bounds_of_nothing_and_defaults(void)
{
    dgx_font_t *font = font0ss();
    int         l = 7, t = 7, w = 7, h = 7;

    /* no symbol of the font: nothing painted, zeros */
    const dgx_hw_context_t ctx = {.font = font, .scale_x = 0.4f, .width = 3};
    CHECK(!dgx_hw_bounds(&ctx, "", &l, &t, &w, &h) && l == 0 && t == 0 && w == 0 && h == 0);
    l = t = w = h = 7;
    CHECK(!dgx_hw_bounds(&ctx, " @\n ", &l, &t, &w, &h) && l == 0 && t == 0 && w == 0 && h == 0);
    /* a font of another kind, no font, a pen below nothing */
    const dgx_hw_context_t other    = {.font = TerminusTTFMedium12(), .scale_x = 0.4f, .width = 3};
    const dgx_hw_context_t fontless = {.scale_x = 0.4f, .width = 3};
    const dgx_hw_context_t penless  = {.font = font, .scale_x = 0.4f, .width = -1};
    l                               = 7;
    CHECK(!dgx_hw_bounds(&other, "abc", &l, &t, &w, &h) && l == 0);
    CHECK(!dgx_hw_bounds(&fontless, "ёж", &l, &t, &w, &h));
    CHECK(!dgx_hw_bounds(&penless, "ёж", &l, &t, &w, &h) && w == 0 && h == 0);
    /* any output may be left out */
    CHECK(dgx_hw_bounds(&ctx, "ёж", NULL, NULL, NULL, NULL));
    CHECK(dgx_hw_bounds(&ctx, "ёж", NULL, &t, NULL, &h) && t < 0 && h > 0);

    /* a size down of 0 is the size across; a size across of 0 is 1; a width of 0 is 1 */
    int                    l2, t2, w2, h2;
    const dgx_hw_context_t both = {.font = font, .scale_x = 0.4f, .scale_y = 0.4f, .width = 3};
    CHECK(dgx_hw_bounds(&ctx, "Ёжик", &l, &t, &w, &h) && dgx_hw_bounds(&both, "Ёжик", &l2, &t2, &w2, &h2));
    CHECK(l == l2 && t == t2 && w == w2 && h == h2);
    const dgx_hw_context_t zero = {.font = font}, one = {.font = font, .scale_x = 1, .scale_y = 1, .width = 1};
    CHECK(dgx_hw_bounds(&zero, "Ёжик", &l, &t, &w, &h) && dgx_hw_bounds(&one, "Ёжик", &l2, &t2, &w2, &h2));
    CHECK(l == l2 && t == t2 && w == w2 && h == h2);

    /* a thicker pen takes its thickness more, half of it on either side; twice the size is about twice the box */
    const dgx_hw_context_t thin = {.font = font, .scale_x = 0.5f, .width = 1}, thick = {.font = font, .scale_x = 0.5f, .width = 9};
    const dgx_hw_context_t large = {.font = font, .scale_x = 1.0f, .width = 1};
    CHECK(dgx_hw_bounds(&thin, "Ёжик", &l, &t, &w, &h) && dgx_hw_bounds(&thick, "Ёжик", &l2, &t2, &w2, &h2));
    CHECK(l2 == l - 4 && t2 == t - 4 && w2 == w + 8 && h2 == h + 8);
    CHECK(dgx_hw_bounds(&large, "Ёжик", &l2, &t2, &w2, &h2));
    CHECK(abs(w2 - 2 * w) <= 6 && abs(h2 - 2 * h) <= 6);
    /* the second line is a line lower */
    CHECK(dgx_hw_bounds(&thin, "о", &l, &t, &w, &h) && dgx_hw_bounds(&thin, "о\nо", &l2, &t2, &w2, &h2));
    CHECK(l == l2 && t == t2 && w == w2 && h2 == h + font->hw->symbol_size_y / 2);
    /* joined, the curves between the letters are in the box; apart, the box is no larger */
    const dgx_hw_context_t joined = {.font = font, .scale_x = 0.5f, .width = 1, .joined = true};
    CHECK(dgx_hw_bounds(&joined, "соревнований", &l2, &t2, &w2, &h2) && dgx_hw_bounds(&thin, "соревнований", &l, &t, &w, &h));
    CHECK(w2 >= w && h2 >= h);
}

/* the box of a morph holds every frame of it, and the frames of the letters of a text morphed letter by letter */
static void morph_bounds_hold_the_frames(void)
{
    dgx_font_t   *font = font0ss();
    dgx_screen_t *s    = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    static const struct
    {
        const char *from, *to;
        bool        joined;
    } pairs[] = {
        {"15",     "08", false},
        {"ёж",     "уж", true },
        {"",       "ёж", false},
        {"привет", "ёж", true }
    };
    int outside = 0, frames = 0;
    for (int k = 0; k < 4; ++k) {
        dgx_hw_morph_t *m = dgx_hw_morph_create(font, pairs[k].from, pairs[k].to, pairs[k].joined);
        CHECK(m != NULL);
        if (!m) continue;
        dgx_hw_morph_shift(m, 3, -5, -4, 6);
        for (int z = 0; z < SIZES; ++z)
            for (int p = 0; p < PENS; p += 2) {
                const dgx_hw_context_t ctx = {.scr = s, .scale_x = sizes[z].cx, .scale_y = sizes[z].cy, .width = pens[p], .color = 0xff};
                int                    l, t, w, h;
                CHECK(dgx_hw_morph_bounds(m, &ctx, &l, &t, &w, &h) && w > 0 && h > 0);
                if (w + 24 > W || h + 24 > H) continue;
                const int x = 11 - l, y = 12 - t;
                for (int i = 0; i <= 8; ++i) {
                    int il = 0, it = 0, ir = 0, ib = 0;
                    wipe(s);
                    dgx_hw_morph_draw_with(m, i / 8.0f, &ctx, x, y);
                    if (!ink_box(s, &il, &it, &ir, &ib)) continue; /* a text out of nothing has no first frame */
                    ++frames;
                    if (il < x + l || it < y + t || ir > x + l + w - 1 || ib > y + t + h - 1) {
                        if (++outside <= 10)
                            fprintf(stderr, "morph %d size %d pen %#x t %d/8: ink %d..%d x %d..%d outside the box %d..%d x %d..%d\n", k, z, pens[p],
                                    i, il, ir, it, ib, x + l, x + l + w - 1, y + t, y + t + h - 1);
                    }
                }
            }
        dgx_hw_morph_destroy(&m);
    }
    if (frames <= 500 || outside) fprintf(stderr, "morphs: %d frames, %d with ink outside\n", frames, outside);
    CHECK(frames > 500 && outside == 0);

    /* at its two ends a morph is its two texts: its box holds the boxes of both */
    dgx_hw_morph_t *m = dgx_hw_morph_create(font, "15", "08", false);
    CHECK(m != NULL);
    if (m) {
        const dgx_hw_context_t ctx = {.font = font, .scale_x = 0.5f, .width = 3};
        int                    l, t, w, h, l1, t1, w1, h1, l2, t2, w2, h2;
        CHECK(dgx_hw_morph_bounds(m, &ctx, &l, &t, &w, &h));
        CHECK(dgx_hw_bounds(&ctx, "15", &l1, &t1, &w1, &h1) && dgx_hw_bounds(&ctx, "08", &l2, &t2, &w2, &h2));
        CHECK(l <= l1 && t <= t1 && l + w >= l1 + w1 && t + h >= t1 + h1);
        CHECK(l <= l2 && t <= t2 && l + w >= l2 + w2 && t + h >= t2 + h2);
        /* and is not far larger than the two together */
        const int ul = l1 < l2 ? l1 : l2, ut = t1 < t2 ? t1 : t2;
        const int ur = l1 + w1 > l2 + w2 ? l1 + w1 : l2 + w2, ub = t1 + h1 > t2 + h2 ? t1 + h1 : t2 + h2;
        CHECK(w <= (ur - ul) * 2 && h <= (ub - ut) * 2);
        /* any output may be left out; a pen below nothing paints nothing */
        CHECK(dgx_hw_morph_bounds(m, &ctx, NULL, NULL, &w1, NULL) && w1 == w);
        const dgx_hw_context_t penless = {.scale_x = 0.5f, .width = -2};
        CHECK(!dgx_hw_morph_bounds(m, &penless, &l, &t, &w, &h) && l == 0 && t == 0 && w == 0 && h == 0);
        /* the defaults are those of a text */
        const dgx_hw_context_t zero = {0}, one = {.scale_x = 1, .scale_y = 1, .width = 1};
        CHECK(dgx_hw_morph_bounds(m, &zero, &l, &t, &w, &h) && dgx_hw_morph_bounds(m, &one, &l1, &t1, &w1, &h1));
        CHECK(l == l1 && t == t1 && w == w1 && h == h1);
    }
    dgx_hw_morph_destroy(&m);
    int                    l = 7, t = 7, w = 7, h = 7;
    const dgx_hw_context_t ctx = {.scale_x = 0.5f, .width = 3};
    CHECK(!dgx_hw_morph_bounds(NULL, &ctx, &l, &t, &w, &h) && l == 0 && t == 0 && w == 0 && h == 0);

    /* letter by letter: every letter is a morph, drawn from the same context at the same place */
    dgx_hw_morph_text_t *text = dgx_hw_morph_text_create(font, "15:08", "23:59", false);
    CHECK(text != NULL);
    if (text) {
        const dgx_hw_context_t c  = {.scr = s, .scale_x = 0.4f, .width = DGX_PEN_SOFT | 4, .color = 0xff};
        int                    bl = 0, bt = 0, br = 0, bb = 0, n = 0;
        for (size_t i = 0; i < text->length; ++i) {
            if (!dgx_hw_morph_bounds(text->letters[i], &c, &l, &t, &w, &h)) continue;
            if (!n || l < bl) bl = l;
            if (!n || t < bt) bt = t;
            if (!n || l + w - 1 > br) br = l + w - 1;
            if (!n || t + h - 1 > bb) bb = t + h - 1;
            ++n;
        }
        CHECK(n == 5);
        const int x = 10 - bl, y = 10 - bt;
        int       wrong = 0;
        for (int step = 0; step <= 6; ++step) {
            int il = 0, it = 0, ir = 0, ib = 0;
            wipe(s);
            for (size_t i = 0; i < text->length; ++i)
                dgx_hw_morph_draw_with(text->letters[i], (step + (int)i) % 7 / 6.0f, &c, x, y);
            if (!ink_box(s, &il, &it, &ir, &ib)) ++wrong;
            else wrong += il < x + bl || it < y + bt || ir > x + br || ib > y + bb;
        }
        CHECK(wrong == 0);
    }
    dgx_hw_morph_text_destroy(&text);
    dgx_screen_destroy(&s);
}

/* a text and a frame reach the screen as one update each, and the update lies within the box */
static void one_update_a_call(void)
{
    dgx_font_t   *font         = font0ss();
    dgx_screen_t *s            = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    s->update_screen           = record;
    const dgx_hw_context_t ctx = {.scr = s, .font = font, .scale_x = 0.4f, .width = 3, .color = 0xff, .joined = true};
    int                    l, t, w, h;
    CHECK(dgx_hw_bounds(&ctx, "Ёжик, 15", &l, &t, &w, &h));
    updates = 0;
    dgx_hw_draw(&ctx, 20, 150, "Ёжик, 15");
    CHECK(updates == 1);
    CHECK(up_left >= 20 + l && up_top >= 150 + t && up_right <= 20 + l + w - 1 && up_bottom <= 150 + t + h - 1);
    /* a soft pen: one update too; the area it marks as changed is a pixel wider than its ink, which the box is of */
    const dgx_hw_context_t soft = {.scr = s, .font = font, .scale_x = 0.4f, .width = DGX_PEN_SOFT | 4, .color = 0xff, .joined = true};
    updates                     = 0;
    dgx_hw_draw(&soft, 20, 150, "Ёжик, 15");
    CHECK(updates == 1);
    /* a text with nothing of the font: no update at all */
    updates = 0;
    dgx_hw_draw(&ctx, 20, 150, " \n ");
    CHECK(updates == 0);

    dgx_hw_morph_t *m = dgx_hw_morph_create(font, "15", "08", false);
    CHECK(m != NULL);
    if (m) {
        CHECK(dgx_hw_morph_bounds(m, &ctx, &l, &t, &w, &h));
        updates = 0;
        dgx_hw_morph_draw_with(m, 0.5f, &ctx, 20, 150);
        CHECK(updates == 1);
        CHECK(up_left >= 20 + l && up_top >= 150 + t && up_right <= 20 + l + w - 1 && up_bottom <= 150 + t + h - 1);
    }
    dgx_hw_morph_destroy(&m);

    /* writing: an update a step that draws, none for a step that does not */
    dgx_hw_writing_t wr;
    dgx_hw_writing_begin_with(&wr, &ctx, 20, 150, "ёж");
    CHECK(updates == 1); /* begun: nothing sent */
    updates         = 0;
    const float all = dgx_hw_text_effort(font, "ёж", true, NULL);
    CHECK(!dgx_hw_writing_draw_to(&wr, all / 2) && updates == 1);
    CHECK(dgx_hw_writing_draw_to(&wr, INFINITY) && updates == 2);
    CHECK(dgx_hw_writing_draw_to(&wr, INFINITY) && updates == 2);
    dgx_screen_destroy(&s);
}

int main(void)
{
    draws_the_same();
    writes_the_same();
    morphs_the_same();
    bounds_hold_the_ink();
    bounds_of_nothing_and_defaults();
    morph_bounds_hold_the_frames();
    one_update_a_call();
    CHECK_DONE();
}
