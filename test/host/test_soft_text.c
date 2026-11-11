/*
 * Text by a context, dgx_font_draw(), and fonts of 2 and 4 bits a pixel (DGX_FONT_BITMAP_DEPTH2/4).
 *
 * The fonts are made here: two glyphs with levels of every kind, one sticking out of its cell on both sides, packed
 * as font2c packs them, and fonts of one bit with the same shapes. A soft font drawn hard has to put the pixels the
 * font of one bit of its shape puts, in all eight orientations and at three scales; its levels have to mix with the
 * screen by the curve, channel by channel, on screens of 8, 12, 16, 18 and 24 bits; with DGX_FONT_BG with the paper
 * and not with the screen, which a screen that cannot be read cannot give; with DGX_FONT_FILL over the cell of the
 * text; cut as font2c cuts with DGX_FONT_MONO, on a screen of one bit and on one that cannot be read. Dot fonts take
 * their dots from a dot function of the context as from the old parameters, and the measures are scaled.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_matrix_morph.h"
#include "dgx_morph_sources.h"
#include "drivers/vscreen.h"
#include "fonts/CasusDotView.h"

/* ---- the fonts of the test ---- */

enum { AW = 5, AH = 4, BW = 6, BH = 5 };
static const uint8_t levels_a[AH][AW] = {{0, 3, 15, 3, 0}, {2, 15, 0, 15, 2}, {15, 15, 15, 15, 15}, {15, 7, 0, 6, 15}};
static const uint8_t levels_b[BH][BW] = {{1, 0, 0, 0, 0, 14}, {0, 8, 15, 15, 8, 0}, {0, 15, 0, 0, 15, 0}, {0, 8, 15, 15, 8, 0}, {14, 0, 0, 0, 0, 1}};

typedef enum { AS_IS, TO_2, HARD_1, MONO_1, HARD_2 } kind_t;

static int level_of(int glyph, int x, int y) { return glyph ? levels_b[y][x] : levels_a[y][x]; }

/* the level a pixel of a font of this kind has, from the levels of 4 bits */
static int kind_level(kind_t kind, int level)
{
    switch (kind) {
    case TO_2:
        return (level * 3 + 7) / 15;
    case HARD_1:
        return level > 0;
    case MONO_1:
        return level * 17 >= 110;
    case HARD_2:
        return (level * 3 + 7) / 15 > 0;
    default:
        return level;
    }
}

typedef struct {
    uint8_t       bits[2][64];
    glyph_t       glyphs[2];
    glyph_array_t ranges[2];
    dgx_font_t    font;
} test_font_t;

static void make_font(test_font_t *f, kind_t kind)
{
    const int depth = kind == AS_IS ? 4 : kind == TO_2 ? 2 : 1;
    memset(f, 0, sizeof(*f));
    for (int k = 0; k < 2; ++k) {
        const int w = k ? BW : AW, h = k ? BH : AH, pitch = (w * depth + 7) / 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const unsigned bit = (unsigned)x * depth;
                f->bits[k][y * pitch + bit / 8] |= (uint8_t)(kind_level(kind, level_of(k, x, y)) << (8 - depth - bit % 8));
            }
        f->glyphs[k].bitmap   = f->bits[k];
        f->glyphs[k].width    = (int16_t)w;
        f->glyphs[k].height   = (int16_t)h;
        f->glyphs[k].xAdvance = k ? 3 : 7;
        f->glyphs[k].xOffset  = k ? -1 : 1;
        f->glyphs[k].yOffset  = k ? -3 : -4;
    }
    f->ranges[0] = (glyph_array_t){.first = 'A', .number = 2, .glyphs = f->glyphs};
    f->font      = (dgx_font_t){.glyph_ranges     = f->ranges,
                                .yAdvance         = 8,
                                .yOffsetLowest    = -4,
                                .xWidest          = BW,
                                .xWidthAverage    = 5,
                                .f_type           = depth == 4 ? DGX_FONT_BITMAP_DEPTH4 : depth == 2 ? DGX_FONT_BITMAP_DEPTH2 : DGX_FONT_BITMAP_LINES,
                                .yBottomMax       = 2,
                                .xOffsetLowest    = -1,
                                .xRightMax        = 5,
                                .number_of_ranges = 1};
}

/* ---- what the pixels should be ---- */

/* the curve the plan settled on, computed here and not taken from the library */
static unsigned alpha_of(int level, int top) { return (unsigned)lround(255.0 * pow((double)level / top, 1 / 1.45)); }

static uint32_t mix_ch(uint32_t u, uint32_t c, unsigned a, unsigned shift, uint32_t mask)
{
    u = u >> shift & mask, c = c >> shift & mask;
    return (u * (255 - a) + c * a + 127) / 255 << shift;
}

static uint32_t mix(int bits, uint32_t under, uint32_t color, unsigned a)
{
    switch (bits) {
    case 8:
        return mix_ch(under, color, a, 0, 255);
    case 12:
        return mix_ch(under, color, a, 12, 15) | mix_ch(under, color, a, 8, 15) | mix_ch(under, color, a, 4, 15);
    case 16:
        return mix_ch(under, color, a, 11, 31) | mix_ch(under, color, a, 5, 63) | mix_ch(under, color, a, 0, 31);
    case 18:
        return mix_ch(under, color, a, 18, 63) | mix_ch(under, color, a, 10, 63) | mix_ch(under, color, a, 2, 63);
    default:
        return mix_ch(under, color, a, 16, 255) | mix_ch(under, color, a, 8, 255) | mix_ch(under, color, a, 0, 255);
    }
}

/* a color of the depth with every channel in use: the pattern under the text */
static uint32_t pattern(int bits, int x, int y)
{
    const uint32_t v = (uint32_t)(x * 2654435761u ^ y * 40503u ^ (x * y * 97u)) * 2246822519u >> 7;
    switch (bits) {
    case 8:
        return v & 0xff;
    case 12:
        return v & 0xfff0;
    case 16:
        return v & 0xffff;
    case 18:
        return v & 0xfcfcfc;
    default:
        return v & 0xffffff;
    }
}

static void paint(dgx_screen_t *scr)
{
    for (int y = 0; y < scr->height; ++y)
        for (int x = 0; x < scr->width; ++x) dgx_set_pixel(scr, x, y, pattern(scr->color_bits, x, y));
}

/* the level of the font of `kind` at a screen pixel of the text "AB" upright at x, y, scale s; -1 outside the glyphs */
static int level_at(kind_t kind, int x0, int y0, int s, int x, int y)
{
    static const int pens[2] = {0, 7}, xo[2] = {1, -1}, yo[2] = {-4, -3}, ws[2] = {AW, BW}, hs[2] = {AH, BH};
    for (int k = 0; k < 2; ++k) {
        const int gx = x - (x0 + pens[k] * s + xo[k] * s), gy = y - (y0 + yo[k] * s);
        if (gx >= 0 && gy >= 0 && gx < ws[k] * s && gy < hs[k] * s) return kind_level(kind, level_of(k, gx / s, gy / s));
    }
    return -1;
}

/* ---- the checks ---- */

/* drawn hard and cut, a soft font puts the pixels of the fonts of one bit of its shapes, turned and scaled alike */
static void test_placement(void)
{
    test_font_t soft, two, hard, mono, hard2;
    make_font(&soft, AS_IS), make_font(&two, TO_2), make_font(&hard, HARD_1), make_font(&mono, MONO_1), make_font(&hard2, HARD_2);
    dgx_screen_t *a = dgx_vscreen_init(80, 80, 16, DgxScreenRGB), *b = dgx_vscreen_init(80, 80, 16, DgxScreenRGB);
    CHECK(a && b);
    for (int o = 0; o < 8; ++o)
        for (int s = 1; s <= 3; ++s)
            for (int how = 0; how < 3; ++how) {
                const dgx_output_orientation_t turn = (dgx_output_orientation_t)o;
                dgx_fill_rectangle(a, 0, 0, 80, 80, 0), dgx_fill_rectangle(b, 0, 0, 80, 80, 0);
                const dgx_font_context_t ctx = {.scr         = a,
                                                .font        = how == 2 ? &two.font : &soft.font,
                                                .color       = 0xf81f,
                                                .flags       = how == 1 ? DGX_FONT_MONO : DGX_FONT_HARD,
                                                .orientation = turn,
                                                .scale       = s};
                const int advance = dgx_font_draw(&ctx, 40, 40, "AB");
                dgx_font_string_utf8_screen(b, 40, 40, "AB", 0xf81f, turn, s, how == 1 ? &mono.font : how == 2 ? &hard2.font : &hard.font, NULL, NULL);
                CHECK(advance == 10 * s);
                int differ = 0, drawn = 0;
                for (int y = 0; y < 80; ++y)
                    for (int x = 0; x < 80; ++x) {
                        differ += dgx_get_pixel(a, x, y) != dgx_get_pixel(b, x, y);
                        drawn += dgx_get_pixel(a, x, y) != 0;
                    }
                CHECK(differ == 0 && drawn > 0);
                /* the new call puts a font of one bit as the old one does */
                dgx_fill_rectangle(a, 0, 0, 80, 80, 0);
                const dgx_font_context_t one = {.scr = a, .font = how == 2 ? &hard2.font : &hard.font, .color = 0xf81f, .orientation = turn, .scale = s};
                dgx_font_draw(&one, 40, 40, "AB");
                differ = 0;
                for (int y = 0; y < 80; ++y)
                    for (int x = 0; x < 80; ++x) differ += dgx_get_pixel(a, x, y) != dgx_get_pixel(b, x, y);
                CHECK(how == 1 || differ == 0);
            }
    dgx_screen_destroy(&a), dgx_screen_destroy(&b);
}

/* mixed with the screen, upright at scales 1 and 2, on every depth that mixes, for both soft depths */
static void test_mix(void)
{
    static const int depths[] = {8, 12, 16, 18, 24};
    for (int d = 0; d < 5; ++d)
        for (int depth = 2; depth <= 4; depth += 2)
            for (int s = 1; s <= 2; ++s) {
                const int bits = depths[d];
                test_font_t f;
                make_font(&f, depth == 4 ? AS_IS : TO_2);
                dgx_screen_t *scr = dgx_vscreen_init(40, 24, (uint8_t)bits, DgxScreenRGB);
                CHECK(scr);
                paint(scr);
                const uint32_t color = pattern(bits, 77, 13);
                const dgx_font_context_t ctx = {.scr = scr, .font = &f.font, .color = color, .scale = s};
                dgx_font_draw(&ctx, 3, 12, "AB");
                int wrong = 0, mixed = 0;
                for (int y = 0; y < 24; ++y)
                    for (int x = 0; x < 40; ++x) {
                        const int level = level_at(depth == 4 ? AS_IS : TO_2, 3, 12, s, x, y), top = (1 << depth) - 1;
                        const uint32_t under = pattern(bits, x, y);
                        const uint32_t want = level <= 0 ? under : level == top ? color : mix(bits, under, color, alpha_of(level, top));
                        wrong += dgx_get_pixel(scr, x, y) != want;
                        mixed += level > 0 && level < top;
                    }
                CHECK(wrong == 0 && mixed > 0);
                dgx_screen_destroy(&scr);
            }
}

/*
 * With DGX_FONT_BG the paper is the context's, not the screen's: on a screen that is read and on one that is not
 * the same pixels. A screen that is not read is a virtual one without get_pixel, which the test reads by itself.
 * Without the flag a screen that is not read cuts as DGX_FONT_MONO does, and so does a screen of one bit.
 */
static void test_paper(void)
{
    test_font_t f, mono;
    make_font(&f, AS_IS), make_font(&mono, MONO_1);
    for (int unread = 0; unread < 2; ++unread)
        for (int flag = 0; flag < 2; ++flag) {
            dgx_screen_t *scr = dgx_vscreen_init(40, 24, 16, DgxScreenRGB);
            CHECK(scr);
            paint(scr);
            uint32_t (*get)(dgx_screen_t *, int, int) = scr->get_pixel;
            if (unread) scr->get_pixel = NULL;
            CHECK(dgx_screen_is_readable(scr) == !unread);
            const uint32_t color = 0xffe0, bg = 0x18e7;
            const dgx_font_context_t ctx = {.scr = scr, .font = &f.font, .color = color, .bg = bg, .flags = flag ? DGX_FONT_BG : 0};
            dgx_font_draw(&ctx, 3, 12, "AB");
            int wrong = 0;
            for (int y = 0; y < 24; ++y)
                for (int x = 0; x < 40; ++x) {
                    const int      level = level_at(AS_IS, 3, 12, 1, x, y);
                    const uint32_t under = pattern(16, x, y);
                    uint32_t       want  = under;
                    if (flag && level > 0) want = level == 15 ? color : mix(16, bg, color, alpha_of(level, 15));
                    else if (!flag && level > 0)
                        want = unread ? (level * 17 >= 110 ? color : under) : level == 15 ? color : mix(16, under, color, alpha_of(level, 15));
                    wrong += get(scr, x, y) != want;
                }
            CHECK(wrong == 0);
            scr->get_pixel = get;
            dgx_screen_destroy(&scr);
        }
    /* a screen of one bit: as the font of one bit cut from it */
    dgx_screen_t *one = dgx_vscreen_init(40, 24, 1, DgxScreenRGB), *cut = dgx_vscreen_init(40, 24, 1, DgxScreenRGB);
    CHECK(one && cut);
    const dgx_font_context_t ctx = {.scr = one, .font = &f.font, .color = 1};
    dgx_font_draw(&ctx, 3, 12, "AB");
    dgx_font_string_utf8_screen(cut, 3, 12, "AB", 1, DgxOutputNormal, 1, &mono.font, NULL, NULL);
    int differ = 0;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 40; ++x) differ += dgx_get_pixel(one, x, y) != dgx_get_pixel(cut, x, y);
    CHECK(differ == 0);
    dgx_screen_destroy(&one), dgx_screen_destroy(&cut);
}

/*
 * DGX_FONT_FILL: the cell is the paper but for the letters, as high as the font and from the start to the far ends of
 * the letters: here 0..12 along the line (B sticks out past the advance of 10) and -4..2 across; turned, it is that
 * rectangle turned, and it holds every pixel of the letters. dgx_font_draw_char() fills the cell of its one letter.
 */
static void test_fill(void)
{
    test_font_t f;
    make_font(&f, AS_IS);
    for (int o = 0; o < 8; ++o)
        for (int s = 1; s <= 2; ++s) {
            const dgx_output_orientation_t turn = (dgx_output_orientation_t)o;
            dgx_screen_t *scr = dgx_vscreen_init(80, 80, 16, DgxScreenRGB), *hard = dgx_vscreen_init(80, 80, 16, DgxScreenRGB);
            CHECK(scr && hard);
            paint(scr), dgx_fill_rectangle(hard, 0, 0, 80, 80, 0);
            const uint32_t color = 0x07e0, bg = 0x2104;
            const dgx_font_context_t ctx = {.scr = scr, .font = &f.font, .color = color, .bg = bg, .flags = DGX_FONT_FILL, .orientation = turn, .scale = s};
            dgx_font_draw(&ctx, 40, 40, "AB");
            const dgx_font_context_t mark = {.scr = hard, .font = &f.font, .color = 0xffff, .flags = DGX_FONT_HARD, .orientation = turn, .scale = s};
            dgx_font_draw(&mark, 40, 40, "AB");
            int l = 80, r = -1, t = 80, b = -1, outside = 0, paper = 0, letters_out = 0;
            for (int y = 0; y < 80; ++y)
                for (int x = 0; x < 80; ++x) {
                    if (dgx_get_pixel(scr, x, y) == pattern(16, x, y)) continue;
                    if (x < l) l = x;
                    if (x > r) r = x;
                    if (y < t) t = y;
                    if (y > b) b = y;
                }
            for (int y = 0; y < 80; ++y)
                for (int x = 0; x < 80; ++x) {
                    const bool in = x >= l && x <= r && y >= t && y <= b;
                    const uint32_t got = dgx_get_pixel(scr, x, y);
                    outside += !in && got != pattern(16, x, y);
                    paper += in && dgx_get_pixel(hard, x, y) == 0 && got == bg;
                    letters_out += !in && dgx_get_pixel(hard, x, y) != 0;
                }
            const bool swap = dgx_output_orientation_swap_xy(turn);
            const int  along = 12 * s, across = 6 * s;
            CHECK(r - l + 1 == (swap ? across : along) && b - t + 1 == (swap ? along : across));
            CHECK(outside == 0 && letters_out == 0);
            /* the paper of the cell: everything there the letters do not touch */
            int letters = 0;
            for (int y = t; y <= b; ++y)
                for (int x = l; x <= r; ++x) letters += dgx_get_pixel(hard, x, y) != 0;
            CHECK(paper == along * across - letters);
            if (turn == DgxOutputNormal) CHECK(l == 40 && t == 40 - 4 * s);
            dgx_screen_destroy(&scr), dgx_screen_destroy(&hard);
        }
    /* one letter: the cell of A alone, 0..7 along (its advance; its ink is 1..6) */
    dgx_screen_t *scr = dgx_vscreen_init(40, 24, 16, DgxScreenRGB);
    CHECK(scr);
    paint(scr);
    const dgx_font_context_t ctx = {.scr = scr, .font = &f.font, .color = 0xffff, .bg = 0x0841, .flags = DGX_FONT_FILL};
    CHECK(dgx_font_draw_char(&ctx, 3, 12, 'A') == 7);
    int changed = 0, out = 0;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 40; ++x) {
            const bool in = x >= 3 && x < 10 && y >= 8 && y < 14;
            changed += in && dgx_get_pixel(scr, x, y) != pattern(16, x, y);
            out += !in && dgx_get_pixel(scr, x, y) != pattern(16, x, y);
        }
    CHECK(changed == 7 * 6 && out == 0);
    dgx_screen_destroy(&scr);
}

/*
 * A display behind a bus: windows and the pixels written into them, rectangles filled, nothing read. The bus buffer
 * is the screen's draw_buffer; a piece is taken when it is written, as by a bus that waits for it.
 */
enum { PW = 80, PH = 80 };
static uint32_t panel[PW * PH];
static int      win_l, win_r, win_b, win_x, win_y, panel_bytes;
static long     panel_windows, panel_rects;

static void bus_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)scr;
    CHECK(left <= right && top <= bottom && right < PW && bottom < PH);
    win_l = left, win_r = right, win_b = bottom, win_x = left, win_y = top;
    ++panel_windows;
}

static void bus_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    CHECK(data == scr->draw_buffer && lenbits % (8u * panel_bytes) == 0 && lenbits / 8 <= scr->draw_buffer_len);
    for (uint32_t i = 0; i + panel_bytes <= lenbits / 8; i += panel_bytes) {
        uint32_t c = 0;
        for (int k = 0; k < panel_bytes; ++k) c = c << 8 | data[i + k];
        CHECK(win_y <= win_b);
        if (win_y <= win_b) panel[win_x + win_y * PW] = c;
        if (++win_x > win_r) win_x = win_l, ++win_y;
    }
}

static void bus_fill(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    (void)scr;
    ++panel_rects;
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            if (xx >= 0 && yy >= 0 && xx < PW && yy < PH) panel[xx + yy * PW] = color;
}

static void bus_pixel(dgx_screen_t *scr, int x, int y, uint32_t color) { bus_fill(scr, x, y, 1, 1, color); }

static void bus_update(dgx_screen_t *scr, int left, int right, int top, int bottom) { (void)scr, (void)left, (void)right, (void)top, (void)bottom; }

/*
 * DGX_FONT_BG and DGX_FONT_FILL on a display behind a bus put the pixels they put on a virtual screen holding the same
 * picture, with a run of touched pixels of a row as one window, and as rectangles when a row does not fit the buffer
 * of the bus; turned, scaled, cut by the edges of the display.
 */
static void test_bus_spans(void)
{
    test_font_t f;
    make_font(&f, AS_IS);
    static uint8_t buffer[512];
    for (int round = 0; round < 8 * 3 * 3 * 2; ++round) {
        const dgx_output_orientation_t turn = (dgx_output_orientation_t)(round % 8);
        const int s = 1 + round / 8 % 3, place = round / 24 % 3, bits = round / 72 % 2 ? 24 : 16;
        const bool tiny = place == 2, fill = round % 3 == 1;
        panel_bytes = bits / 8;
        dgx_screen_t lcd = {.width = PW, .height = PH, .color_bits = (uint8_t)bits, .screen_subtype = DgxPhysicalScreenWithBus};
        lcd.set_area = bus_set_area, lcd.write_area = bus_write_area, lcd.fill_rectangle = bus_fill, lcd.set_pixel = bus_pixel;
        lcd.update_screen = bus_update, lcd.draw_buffer = buffer, lcd.draw_buffer_len = tiny ? 4 : sizeof(buffer);
        CHECK(!dgx_screen_is_readable(&lcd));
        dgx_screen_t *model = dgx_vscreen_init(PW, PH, (uint8_t)bits, DgxScreenRGB);
        CHECK(model);
        paint(model);
        for (int y = 0; y < PH; ++y)
            for (int x = 0; x < PW; ++x) panel[x + y * PW] = pattern(bits, x, y);
        /* in the middle, or half off the left top corner */
        const int x = place == 1 ? 2 : 40, y = place == 1 ? 3 : 40;
        const uint32_t color = pattern(bits, 5, 9), bg = pattern(bits, 31, 2);
        dgx_font_context_t ctx = {.scr = &lcd, .font = &f.font, .color = color, .bg = bg, .flags = fill ? DGX_FONT_FILL : DGX_FONT_BG,
                                  .orientation = turn, .scale = s};
        panel_windows = panel_rects = 0;
        dgx_font_draw(&ctx, x, y, "AB");
        ctx.scr = model;
        dgx_font_draw(&ctx, x, y, "AB");
        int differ = 0;
        for (int yy = 0; yy < PH; ++yy)
            for (int xx = 0; xx < PW; ++xx) differ += panel[xx + yy * PW] != dgx_get_pixel(model, xx, yy);
        CHECK(differ == 0);
        /* a span a run of touched pixels: as many windows as runs of a row, AB has 4 + 5 rows of them, 2 at most each */
        if (!tiny && place == 0) CHECK(panel_windows >= 9 && panel_windows <= 18 && panel_rects == fill);
        /* a buffer of 4 bytes takes a row of a window of a pixel or two: an upright span longer than that goes as rectangles */
        if (tiny && !dgx_output_orientation_swap_xy(turn)) CHECK(panel_rects > fill);
        dgx_screen_destroy(&model);
    }
}

/* the dots of a dot font from the dot function of a context, where the old call takes them from its parameters */
typedef struct {
    int n;
    int xy[4096][2];
} dots_t;
static dots_t old_dots, new_dots;

static void old_dot(dgx_screen_t *scr, int x, int y, dgx_font_sym8_params_t *p)
{
    (void)scr, (void)p;
    if (old_dots.n < 4096) old_dots.xy[old_dots.n][0] = x, old_dots.xy[old_dots.n++][1] = y;
}

static void new_dot(const dgx_font_context_t *ctx, int x, int y)
{
    dots_t *d = (dots_t *)ctx->user_data;
    if (d->n < 4096) d->xy[d->n][0] = x, d->xy[d->n++][1] = y;
}

static void test_dots(void)
{
    dgx_screen_t *scr = dgx_vscreen_init(64, 64, 16, DgxScreenRGB), *a = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
    CHECK(scr && a);
    for (int o = 0; o < 8; ++o) {
        const dgx_output_orientation_t turn = (dgx_output_orientation_t)o;
        old_dots.n = new_dots.n = 0;
        dgx_font_sym8_params_t params = {.dot_func = old_dot};
        dgx_font_string_utf8_screen(scr, 30, 30, "12", 0, turn, 2, CasusDotView(), NULL, &params);
        const dgx_font_context_t ctx = {.scr = scr, .font = CasusDotView(), .orientation = turn, .scale = 2, .dot_func = new_dot, .user_data = &new_dots};
        dgx_font_draw(&ctx, 30, 30, "12");
        CHECK(old_dots.n > 0 && old_dots.n == new_dots.n && !memcmp(old_dots.xy, new_dots.xy, sizeof(old_dots.xy[0]) * old_dots.n));
        /* without a dot function: squares of the color, as the old call without parameters */
        dgx_fill_rectangle(scr, 0, 0, 64, 64, 0), dgx_fill_rectangle(a, 0, 0, 64, 64, 0);
        dgx_font_string_utf8_screen(scr, 30, 30, "12", 0xffff, turn, 2, CasusDotView(), NULL, NULL);
        const dgx_font_context_t plain = {.scr = a, .font = CasusDotView(), .color = 0xffff, .orientation = turn, .scale = 2};
        dgx_font_draw(&plain, 30, 30, "12");
        int differ = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) differ += dgx_get_pixel(scr, x, y) != dgx_get_pixel(a, x, y);
        CHECK(differ == 0);
    }
    dgx_screen_destroy(&scr), dgx_screen_destroy(&a);
}

/* the morphs take of a soft font the pixels DGX_FONT_MONO draws: as of the font of one bit cut from it */
static void test_morph_sources(void)
{
    test_font_t f, mono;
    make_font(&f, AS_IS), make_font(&mono, MONO_1);
    for (uint32_t cp = 'A'; cp <= 'B'; ++cp) {
        dgx_bit_matrix_t *a = dgx_morph_glyph_matrix(&f.font, cp), *b = dgx_morph_glyph_matrix(&mono.font, cp);
        CHECK(a && b && dgx_matrix_equals(a, b) && a->number_of_set_cells > 0);
        dgx_matrix_destroy(&a), dgx_matrix_destroy(&b);
    }
    dgx_font_symbol_morph_t *a = dgx_font_make_morph_struct(&f.font, 'A', 'B', 0, 0, 10, 0, 2);
    dgx_font_symbol_morph_t *b = dgx_font_make_morph_struct(&mono.font, 'A', 'B', 0, 0, 10, 0, 2);
    CHECK(a && b && a->number_of_dots == b->number_of_dots && a->number_of_dots > 0);
    if (a && b && a->number_of_dots == b->number_of_dots) {
        CHECK(!memcmp(a->m_start, b->m_start, sizeof(*a->m_start) * a->number_of_dots));
        CHECK(!memcmp(a->m_end, b->m_end, sizeof(*a->m_end) * a->number_of_dots));
    }
    dgx_font_make_morph_struct_destroy(&a), dgx_font_make_morph_struct_destroy(&b);
}

/* the measures of a text by a context: those of the old call, times the scale */
static void test_bounds(void)
{
    test_font_t f;
    make_font(&f, AS_IS);
    int16_t top16, height16;
    const int width16 = dgx_font_string_bounds("ABA?", &f.font, &top16, &height16);
    for (int s = 1; s <= 3; ++s) {
        const dgx_font_context_t ctx = {.font = &f.font, .scale = s};
        int top, height;
        CHECK(dgx_font_bounds(&ctx, "ABA?", &top, &height) == width16 * s && top == top16 * s && height == height16 * s);
        CHECK(dgx_font_bounds(&ctx, "", NULL, NULL) == 0);
    }
    CHECK(width16 == 7 + 3 + 7 + 7); /* the missing '?' advances as the first glyph of the font */
}

int main(void)
{
    test_placement();
    test_mix();
    test_paper();
    test_fill();
    test_bus_spans();
    test_dots();
    test_bounds();
    test_morph_sources();
    CHECK_DONE();
}
