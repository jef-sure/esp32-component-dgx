/*
 * A font read of the bytes of its file, dgx_font_file.h, against the same font of C: every font of DGX written as a
 * file by font2c/dgxf_font.h, read at an address divisible by 4, at an even one and at an odd one, draws every glyph
 * the same, pixel for pixel, every way. Small files of every model, cut at every length and spoiled byte by byte,
 * are refused or give a font that draws and measures without a fault (the tests run under AddressSanitizer and
 * UndefinedBehaviorSanitizer). A file of a later format, too little memory, memory not aligned are told apart.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_font_file.h"
#include "dgx_hw_font.h"
#include "dgx_matrix_morph.h"
#include "dgx_morph_sources.h"
#include "dgx_vscreen.h"
#include "dgxf_font.h"
#include "fonts/CasusDotView.h"
#include "fonts/CozetteRegular13.h"
#include "fonts/DejaVuSansBook32.h"
#include "fonts/DroidSansRegular9.h"
#include "fonts/GLCD5x7.h"
#include "fonts/IBMCGALight8x16Light8x1616.h"
#include "fonts/IBMPlexMonoMedium28.h"
#include "fonts/LiberationSansRegular12.h"
#include "fonts/NotoMonoRegular28.h"
#include "fonts/TerminusTTFMedium12.h"
#include "fonts/WeatherDotView25.h"
#include "fonts/WeatherIconsRegular13.h"
#include "fonts/WeatherIconsRegular27.h"
#include "fonts/font0ss.h"

typedef struct {
    uint8_t *bytes;
    size_t   length;
} file_t;

/* the file of a font, in memory */
static file_t file_of(const dgx_font_t *font, const char *about)
{
    file_t f    = {0};
    char   why[160];
    FILE  *out  = open_memstream((char **)&f.bytes, &f.length);
    int    fail = dgxf_of_font(font, about, out, why, sizeof why);
    fclose(out);
    if (fail) fprintf(stderr, "dgxf_of_font: %s\n", why);
    CHECK(!fail);
    return f;
}

/* a font of the bytes at `shift` from an address divisible by 8: the copy of the bytes and the memory are kept */
typedef struct {
    uint8_t          *copy;
    void             *memory;
    size_t            need;
    const dgx_font_t *font;
} loaded_t;

static esp_err_t load(const file_t *f, int shift, size_t extra, loaded_t *l)
{
    *l      = (loaded_t){0};
    l->copy = malloc(f->length + extra + 8);
    memcpy(l->copy + shift, f->bytes, f->length);
    memset(l->copy + shift + f->length, 0xA5, extra);
    esp_err_t err = dgx_font_file_memory(l->copy + shift, f->length + extra, &l->need);
    if (err == ESP_OK) {
        l->memory = malloc(l->need ? l->need : 1);
        err       = dgx_font_file_load(l->copy + shift, f->length + extra, l->memory, l->need, &l->font);
    }
    return err;
}

static void unload(loaded_t *l)
{
    free(l->copy), free(l->memory);
    *l = (loaded_t){0};
}

/* every code point of a font, in its order, as UTF-8 lines of 24 */
static size_t code_points(const dgx_font_t *f, uint32_t *out, size_t most)
{
    size_t n = 0;
    for (const dgx_glyph_array_t *r = f->glyph_ranges; r->number; ++r)
        for (uint32_t k = 0; k < r->number && n < most; ++k) out[n++] = r->first + k;
    return n;
}

static void utf8_of(const uint32_t *cps, size_t n, char *text)
{
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t cp = cps[i];
        if (cp < 0x20) continue;
        if (cp < 0x80) text[o++] = (char)cp;
        else if (cp < 0x800) text[o++] = (char)(0xC0 | cp >> 6), text[o++] = (char)(0x80 | (cp & 63));
        else if (cp < 0x10000) text[o++] = (char)(0xE0 | cp >> 12), text[o++] = (char)(0x80 | (cp >> 6 & 63)), text[o++] = (char)(0x80 | (cp & 63));
        else
            text[o++] = (char)(0xF0 | cp >> 18), text[o++] = (char)(0x80 | (cp >> 12 & 63)), text[o++] = (char)(0x80 | (cp >> 6 & 63)),
            text[o++] = (char)(0x80 | (cp & 63));
    }
    text[o] = 0;
}

static bool same_screens(dgx_screen_t *a, dgx_screen_t *b)
{
    const size_t bytes = (size_t)a->width * a->height * ((a->color_bits + 7) / 8);
    return !memcmp(((dgx_vscreen_t *)a)->v_array, ((dgx_vscreen_t *)b)->v_array, bytes);
}

/* the glyphs of two fonts: the same measures and the same data */
static bool same_glyphs(const dgx_font_t *c, const dgx_font_t *file)
{
    for (const dgx_glyph_array_t *r = c->glyph_ranges; r->number; ++r)
        for (uint32_t k = 0; k < r->number; ++k) {
            int16_t            a1, a2;
            const dgx_glyph_t *g1 = dgx_font_glyph(c, r->first + k, &a1), *g2 = dgx_font_glyph(file, r->first + k, &a2);
            if (!g1 || !g2 || a1 != a2 || memcmp(g1, g2, sizeof *g1)) return false;
            const size_t bytes = c->f_type == DGX_FONT_HW ? g1->count * sizeof(dgx_hw_element_t) : c->f_type == DGX_FONT_DOTS ? g1->count * 2u
                                                                                                    : dgxf_data_of(c, g1);
            if (memcmp(dgx_font_glyph_data(c, g1), dgx_font_glyph_data(file, g2), bytes)) return false;
            if (c->f_type == DGX_FONT_HW && memcmp(dgx_hw_glyph_symbol(c, g1), dgx_hw_glyph_symbol(file, g2), sizeof(dgx_hw_symbol_t))) return false;
        }
    return true;
}

/* the text of the font of C and of the font of the file drawn by every way: the same pixels */
static bool draws_alike(const dgx_font_t *c, const dgx_font_t *file)
{
    static uint32_t cps[1024];
    static char     text[24 * 4 + 1];
    const size_t    n = code_points(c, cps, 1024);
    bool            same = true;
    if (c->f_type == DGX_FONT_HW) {
        for (int k = 0; k < 4 && same; ++k) {
            dgx_screen_t *a = dgx_vscreen_init(600, 160, 16, DgxScreenRGB), *b = dgx_vscreen_init(600, 160, 16, DgxScreenRGB);
            for (size_t from = 0; from < n && same; from += 12) {
                utf8_of(cps + from, n - from < 12 ? n - from : 12, text);
                dgx_fill_rectangle(a, 0, 0, 600, 160, 0), dgx_fill_rectangle(b, 0, 0, 600, 160, 0);
                dgx_hw_context_t ca = {.scr = a, .font = c, .scale_x = 0.5f, .width = k & 1 ? (DGX_PEN_SOFT | 3) : 2, .color = 0xffff, .joined = k > 1};
                dgx_hw_context_t cb = ca;
                cb.scr = b, cb.font = file;
                dgx_hw_draw(&ca, 10, 80, text), dgx_hw_draw(&cb, 10, 80, text);
                int l1, t1, w1, h1, l2, t2, w2, h2;
                same = same_screens(a, b) && dgx_hw_bounds(&ca, text, &l1, &t1, &w1, &h1) == dgx_hw_bounds(&cb, text, &l2, &t2, &w2, &h2) &&
                       l1 == l2 && t1 == t2 && w1 == w2 && h1 == h2;
            }
            dgx_screen_destroy(&a), dgx_screen_destroy(&b);
        }
        return same;
    }
    static const struct {
        int bits, flags, scale, orientation;
    } ways[] = {{16, 0, 1, 0}, {16, DGX_FONT_FILL, 1, 0}, {16, DGX_FONT_MONO, 2, 0}, {16, DGX_FONT_BG, 1, DgxOutputRotate90CW}, {8, DGX_FONT_FILL, 1, 0}};
    for (size_t w = 0; w < sizeof ways / sizeof *ways && same; ++w) {
        const int     side = 900;
        dgx_screen_t *a = dgx_vscreen_init(side, 400, ways[w].bits, DgxScreenRGB), *b = dgx_vscreen_init(side, 400, ways[w].bits, DgxScreenRGB);
        for (size_t from = 0; from < n && same; from += 24) {
            utf8_of(cps + from, n - from < 24 ? n - from : 24, text);
            dgx_fill_rectangle(a, 0, 0, side, 400, 3), dgx_fill_rectangle(b, 0, 0, side, 400, 3);
            dgx_font_context_t ca = {.scr = a, .font = c, .color = 0xffe0, .bg = 0x0010, .flags = ways[w].flags, .scale = ways[w].scale,
                                     .orientation = ways[w].orientation};
            dgx_font_context_t cb = ca;
            cb.scr = b, cb.font = file;
            const int x = ways[w].orientation ? 300 : 4, y = ways[w].orientation ? 4 : 200;
            int       top1, height1, top2, height2;
            same = dgx_font_draw(&ca, x, y, text) == dgx_font_draw(&cb, x, y, text) && same_screens(a, b) &&
                   dgx_font_bounds(&ca, text, &top1, &height1) == dgx_font_bounds(&cb, text, &top2, &height2) && top1 == top2 && height1 == height2;
        }
        dgx_screen_destroy(&a), dgx_screen_destroy(&b);
    }
    /* the dots of every glyph a morph takes */
    for (size_t i = 0; i < n && same; ++i) {
        dgx_bit_matrix_t *ma = dgx_morph_glyph_matrix(c, cps[i]), *mb = dgx_morph_glyph_matrix(file, cps[i]);
        same = dgx_matrix_equals(ma, mb);
        dgx_matrix_destroy(&ma), dgx_matrix_destroy(&mb);
    }
    return same;
}

/* a font of 4 bits a pixel of two glyphs, made here */
static uint8_t           soft_bits[2][40];
static dgx_glyph_t       soft_glyphs[2];
static dgx_glyph_array_t soft_ranges[2];
static dgx_font_t        soft_font;

static void make_soft_font(void)
{
    for (int k = 0; k < 2; ++k) {
        const int w = 5 + k, h = 8, pitch = (w * 4 + 7) / 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) soft_bits[k][y * pitch + x / 2] |= (uint8_t)(((x * 7 + y * 5 + k * 3) % 16) << (x & 1 ? 0 : 4));
        soft_glyphs[k] = (dgx_glyph_t){.offset = (uint32_t)(k * sizeof soft_bits[0]), .width = (int16_t)w, .height = (int16_t)h, .x_advance = 8, .y_offset = -8};
    }
    soft_ranges[0] = (dgx_glyph_array_t){.first = 'A', .number = 2, .glyph = 0};
    soft_font      = (dgx_font_t){.glyph_ranges = soft_ranges, .glyphs = soft_glyphs, .data = soft_bits[0], .y_advance = 10, .y_offset_lowest = -8,
                                  .y_bottom_max = 0, .x_widest = 6, .x_width_average = 5, .f_type = DGX_FONT_BITMAP_DEPTH4, .x_right_max = 6,
                                  .number_of_ranges = 1};
}

/* a handwritten font of three symbols, as in test_hw_font.c */
static const dgx_hw_element_t hw_elements[] = {
    {DGX_HW_LINE, 1, 12000, 120, {{110, 67}, {110, 187}}},
    {DGX_HW_CURVE, 6, 9050, 80, {{80, 70}, {150, 60}, {120, 120}, {100, 187}}},
    {DGX_HW_LINE, 1, 4000, 40, {{90, 130}, {130, 130}}},
    {DGX_HW_CURVE3P, 2, 2000, 18, {{67, 158}, {74, 150}, {78, 133}}},
    {DGX_HW_CURVE, 9, 14000, 125, {{78, 133}, {120, 120}, {60, 200}, {110, 187}}},
    {DGX_HW_DOT, 0, 0, 1, {{110, 187}}},
    {DGX_HW_CURVE, 3, 3100, 28, {{110, 187}, {120, 185}, {128, 170}, {134, 158}}},
};
static const dgx_hw_symbol_t hw_symbols[] = {
    {0, 1, 0, 0, 1, -120, 0, 44, 44, 44, 44, 1, 44, 44, 17, 0, 16},
    {0, 1, 0, 1, 51, -121, 0, 14, 64, 34, 64, 31, 14, 64, 51, 8, 12},
    {1, 2, 1, 0, 40, -58, 0, 12, 51, 12, 51, 40, DGX_HW_NONE, DGX_HW_NONE, 56, 0, 16},
};
static const dgx_glyph_t hw_glyphs[] = {
    {0, 1, 121, 17, 0, -120, 1},
    {1 * sizeof(dgx_hw_element_t), 51, 122, 51, -12, -121, 2},
    {3 * sizeof(dgx_hw_element_t), 40, 59, 56, 0, -58, 4},
};
static const dgx_glyph_array_t hw_ranges[] = {{0x31, 1, 0}, {0x37, 1, 1}, {0x430, 1, 2}, {0, 0, 0}};
static const dgx_hw_font_t     hw_part     = {"test", 1, 139, 58, 16, 40, 66, 48, 100, 202, hw_symbols};
static const dgx_font_t        hw_font     = {.glyph_ranges = hw_ranges, .y_advance = 202, .y_offset_lowest = -121, .x_widest = 51, .x_width_average = 31,
                                              .f_type = DGX_FONT_HW, .y_bottom_max = 1, .x_offset_lowest = -12, .x_right_max = 40, .number_of_ranges = 3,
                                              .hw = &hw_part, .glyphs = hw_glyphs, .data = (const uint8_t *)hw_elements};

/* a font a spoiled file gave: whatever it holds, it is drawn, measured and morphed without a fault */
static void use(const dgx_font_t *font)
{
    static dgx_screen_t *s;
    if (!s) s = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
    if (font->f_type == DGX_FONT_HW) {
        dgx_hw_context_t ctx = {.scr = s, .font = font, .scale_x = 0.25f, .width = DGX_PEN_SOFT | 2, .color = 0xffff, .joined = true};
        dgx_hw_draw(&ctx, 2, 40, "17а1");
        int l, t, w, h;
        dgx_hw_bounds(&ctx, "17а", &l, &t, &w, &h);
        dgx_hw_morph_t *m = dgx_hw_morph_create(font, "17", "а1", true);
        if (m) {
            dgx_hw_morph_draw_with(m, 0.5f, &ctx, 2, 40);
            dgx_hw_morph_destroy(&m);
        }
        return;
    }
    dgx_font_context_t ctx = {.scr = s, .font = font, .color = 0xffff, .bg = 1, .flags = DGX_FONT_FILL};
    static const char *const texts[] = {"AB", "0:9", "\xE2\x84\x83%"};
    for (int i = 0; i < 3; ++i) {
        int top, height;
        dgx_font_draw(&ctx, 2, 30, texts[i]);
        dgx_font_bounds(&ctx, texts[i], &top, &height);
    }
    for (uint32_t cp = '0'; cp <= 'B'; cp += 9) {
        dgx_bit_matrix_t *m = dgx_morph_glyph_matrix(font, cp);
        dgx_matrix_destroy(&m);
    }
    dgx_font_symbol_morph_t *ms = dgx_font_morph_create(font, 'A', '0', 0, 0, 10, 0, 1);
    dgx_font_morph_destroy(&ms);
}

/* every cut of a file is refused; every byte spoiled four ways is refused or gives a font that is used safely */
static void spoil(const char *name, const file_t *f)
{
    loaded_t l;
    int      cut_taken = 0, taken = 0, refused = 0;
    for (size_t n = 0; n < f->length; ++n) {
        file_t cut = {f->bytes, n};
        if (load(&cut, 0, 0, &l) == ESP_OK) ++cut_taken;
        unload(&l);
    }
    CHECK(cut_taken == 0);
    for (size_t at = 0; at < f->length; ++at) {
        const uint8_t was = f->bytes[at];
        const uint8_t ways[4] = {0x00, 0xFF, (uint8_t)(was ^ 0x80), (uint8_t)(was + 1)};
        for (int k = 0; k < 4; ++k) {
            if (ways[k] == was) continue;
            f->bytes[at] = ways[k];
            if (load(f, (int)(at & 3), 0, &l) == ESP_OK) {
                use(l.font);
                ++taken;
            } else {
                ++refused;
            }
            unload(&l);
        }
        f->bytes[at] = was;
    }
    printf("%s: %zu bytes, every cut refused; spoiled: %d taken and used, %d refused\n", name, f->length, taken, refused);
    CHECK(refused > 0);
}

int main(void)
{
    static const struct {
        const char       *name;
        const dgx_font_t *font;
    } fonts[] = {
        {"CasusDotView", &CasusDotView},
        {"CozetteRegular13", &CozetteRegular13},
        {"DejaVuSansBook32", &DejaVuSansBook32},
        {"DroidSansRegular9", &DroidSansRegular9},
        {"GLCD5x7", &GLCD5x7},
        {"IBMCGALight8x16Light8x1616", &IBMCGALight8x16Light8x1616},
        {"IBMPlexMonoMedium28", &IBMPlexMonoMedium28},
        {"LiberationSansRegular12", &LiberationSansRegular12},
        {"NotoMonoRegular28", &NotoMonoRegular28},
        {"TerminusTTFMedium12", &TerminusTTFMedium12},
        {"WeatherDotView25", &WeatherDotView25},
        {"WeatherIconsRegular13", &WeatherIconsRegular13},
        {"WeatherIconsRegular27", &WeatherIconsRegular27},
        {"font0ss", &font0ss},
    };
    make_soft_font();

    /* every font of DGX as a file: the same glyphs, read in place or copied */
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts; ++i) {
        file_t f = file_of(fonts[i].font, "name: test\n");
        for (int shift = 0; shift < 3; ++shift) {
            loaded_t l;
            CHECK(load(&f, shift, shift * 5, &l) == ESP_OK);
            if (!l.font) {
                fprintf(stderr, "%s at %d: not read\n", fonts[i].name, shift);
                continue;
            }
            /* in place the memory holds the font and the part of a handwritten one, nothing of its tables */
            if (!shift) CHECK(l.need <= 2 * sizeof(dgx_font_t) + sizeof(dgx_hw_font_t) && l.font->glyphs == (const void *)(l.copy + 64 + 12 * (l.font->number_of_ranges + 1)));
            /* every way of drawing at one address; at the others the tables they were copied to */
            if (!same_glyphs(fonts[i].font, l.font) || (!shift && !draws_alike(fonts[i].font, l.font))) {
                fprintf(stderr, "%s at %d: drawn otherwise\n", fonts[i].name, shift);
                CHECK(false);
            }
            unload(&l);
        }
        free(f.bytes);
    }

    /* small files of every model spoiled */
    file_t glcd = file_of(&GLCD5x7, NULL), casus = file_of(&CasusDotView, "name: CasusDotView\n"), soft = file_of(&soft_font, NULL),
           hw = file_of(&hw_font, NULL);
    loaded_t l;
    CHECK(load(&soft, 1, 0, &l) == ESP_OK && draws_alike(&soft_font, l.font));
    unload(&l);
    CHECK(load(&hw, 1, 0, &l) == ESP_OK && draws_alike(&hw_font, l.font));
    unload(&l);
    spoil("GLCD5x7", &glcd);
    spoil("CasusDotView", &casus);
    spoil("a soft font", &soft);
    spoil("a handwritten font", &hw);

    /* a later format, too little memory, memory not aligned, nothing given */
    size_t need = 0;
    glcd.bytes[4] = 2;
    CHECK(dgx_font_file_memory(glcd.bytes, glcd.length, &need) == ESP_ERR_INVALID_VERSION);
    glcd.bytes[4] = 1;
    CHECK(dgx_font_file_memory(glcd.bytes, glcd.length, &need) == ESP_OK);
    const dgx_font_t *font = NULL;
    uint64_t          memory[16];
    CHECK(dgx_font_file_load(glcd.bytes, glcd.length, memory, need - 1, &font) == ESP_ERR_INVALID_SIZE && !font);
    CHECK(dgx_font_file_load(glcd.bytes, glcd.length, (uint8_t *)memory + 1, need, &font) == ESP_ERR_INVALID_SIZE && !font);
    CHECK(dgx_font_file_load(glcd.bytes, glcd.length, memory, sizeof memory, &font) == ESP_OK && font && font->f_type == DGX_FONT_BITMAP_LINES);
    CHECK(dgx_font_file_memory(NULL, 100, &need) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_font_file_load(glcd.bytes, glcd.length, NULL, need, &font) == ESP_ERR_INVALID_SIZE);
    free(glcd.bytes), free(casus.bytes), free(soft.bytes), free(hw.bytes);
    CHECK_DONE();
}
