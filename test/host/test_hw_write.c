#include <math.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

/* font0-ss: the numbers below are of the font as it is in src/fonts */
#include "fonts/font0ss.h"

#define W 300
#define H 160

static int strokes_of(dgx_font_t *font, const char *text, bool joined, dgx_hw_stroke_t *out, int max)
{
    dgx_hw_writer_t w;
    int             n = 0;
    dgx_hw_writer_begin(&w, font, text, joined);
    while (n < max && dgx_hw_writer_next(&w, &out[n])) ++n;
    return n;
}

static const dgx_hw_symbol_t *symbol(uint32_t cp)
{
    int16_t advance;
    return dgx_font_find_glyph(cp, font0ss(), &advance)->hw;
}

/* a small font of its own for what font0-ss has not: a letter with a begin connection and no end one */
static const dgx_hw_element_t tiny_elements[] = {
    {DGX_HW_LINE, 1, 1000, 10, {{70, 180}, {80, 180}}},  /* a: begin */
    {DGX_HW_LINE, 1, 4000, 40, {{80, 180}, {80, 140}}},  /* a: main */
    {DGX_HW_LINE, 1, 1000, 10, {{70, 180}, {80, 180}}},  /* b: begin */
    {DGX_HW_LINE, 1, 4000, 40, {{80, 180}, {100, 140}}}, /* b: main */
    {DGX_HW_LINE, 1, 1000, 10, {{100, 140}, {110, 140}}}, /* b: end */
};
static const dgx_hw_symbol_t tiny_symbols[] = {
    {1, 1, 0, 0, 1, -47, -7, 14, 14, 14, 14, 1, DGX_HW_NONE, DGX_HW_NONE, 17, 0, 16},
    {1, 1, 1, 0, 21, -47, -7, 14, 34, 14, 34, 21, DGX_HW_NONE, DGX_HW_NONE, 37, 0, 16},
};
static const glyph_t tiny_glyphs[] = {
    {{.elements = tiny_elements + 0, .hw = tiny_symbols + 0}, 1, 41, 17, 0, -47},
    {{.elements = tiny_elements + 2, .hw = tiny_symbols + 1}, 21, 41, 37, 0, -47},
};
static const glyph_array_t tiny_ranges[] = {{'a', 2, tiny_glyphs}, {0, 0, 0}};
static const dgx_hw_font_t tiny_hw = {"tiny", 1, 139, 58, 16, 40, 66, 48, 100, 202};
static dgx_font_t          tiny = {.glyph_ranges = tiny_ranges, .yAdvance = 202, .f_type = DGX_FONT_HW, .number_of_ranges = 1, .hw = &tiny_hw};

static int ink(dgx_screen_t *s)
{
    int n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) n += dgx_get_pixel(s, x, y) != 0;
    }
    return n;
}

int main(void)
{
    dgx_font_t     *font = font0ss();
    dgx_hw_stroke_t s[64];
    int             n;

    /* apart: every symbol is its main part and then its postponed strokes, no connections */
    const dgx_hw_symbol_t *yo = symbol(0x451), *zh = symbol(0x436);
    CHECK(yo->post_segments == 2 && yo->begin_connection && yo->end_connection && zh->begin_connection);
    n = strokes_of(font, "ёж", false, s, 64);
    CHECK(n == yo->main_segments + yo->post_segments + zh->main_segments + zh->post_segments);
    CHECK(s[yo->main_segments].type == DGX_HW_DOT && s[yo->main_segments + 1].type == DGX_HW_DOT);
    CHECK(!s[0].lift && s[yo->main_segments].lift);

    /*
     * Joined: "ё" without its begin connection, its end connection but the last
     * element, one curve to "ж", the rest of the begin connection of "ж", "ж",
     * and only then the dots of "ё".
     */
    n = strokes_of(font, "ёж", true, s, 64);
    int joint = yo->main_segments + yo->end_connection - 1;
    CHECK(n == joint + 1 + zh->begin_connection - 1 + zh->main_segments + yo->post_segments + zh->post_segments);
    CHECK(s[n - 2].type == DGX_HW_DOT && s[n - 1].type == DGX_HW_DOT && s[n - 2].lift);
    /* the pen goes into the joint and out of it without leaving the paper */
    CHECK(s[joint].type == DGX_HW_CURVE && !s[joint].lift && !s[joint + 1].lift);
    CHECK(s[joint].pieces >= 1 && s[joint].length > hypotf(s[joint].x[3] - s[joint].x[0], s[joint].y[3] - s[joint].y[0]) - 0.01f);
    /* it leaves and comes as the connections are drawn: not backwards */
    CHECK(s[joint].x[3] > s[joint].x[0]);
    /* symbols stand where the line places them: "ж" is as far from "ё" joined or not */
    dgx_hw_stroke_t a[64];
    strokes_of(font, "ёж", false, a, 64);
    CHECK(s[joint + zh->begin_connection].x[0] == a[yo->main_segments + yo->post_segments].x[0]);
    CHECK(s[0].x[0] == a[0].x[0] && s[0].y[0] == a[0].y[0]);

    /* a space takes the pen off: the dots of "её" come before "ёж" */
    n = strokes_of(font, "её ёж", true, s, 64);
    const dgx_hw_symbol_t *ye = symbol(0x435);
    int                    word = ye->main_segments + ye->end_connection - 1 + 1 + yo->begin_connection - 1 + yo->main_segments;
    CHECK(s[word].type == DGX_HW_DOT && s[word + 1].type == DGX_HW_DOT && s[word + 2].type != DGX_HW_DOT && s[word + 2].lift);
    /* a capital letter goes on into a lowercase one but is not joined to the one before */
    const dgx_hw_symbol_t *cap = symbol(0x416);
    CHECK(!cap->begin_connection && cap->end_connection);
    n = strokes_of(font, "жЖж", true, s, 64);
    CHECK(n == zh->main_segments + cap->main_segments + cap->end_connection - 1 + 1 + zh->begin_connection - 1 + zh->main_segments);
    /* digits are written apart */
    n = strokes_of(font, "12", true, s, 64);
    CHECK(n == symbol('1')->main_segments + symbol('1')->post_segments + symbol('2')->main_segments + symbol('2')->post_segments);

    /* the next line is symbol_size_y lower and starts from the left again */
    n = strokes_of(font, "о\nо", true, s, 64);
    CHECK(n == 2 * symbol(0x43e)->main_segments);
    CHECK(s[n / 2].x[0] == s[0].x[0] && s[n / 2].y[0] == s[0].y[0] + font->hw->symbol_size_y);
    CHECK(strokes_of(font, "", true, s, 64) == 0 && strokes_of(font, " @\n ", true, s, 64) == 0);

    /* a letter after one that has a begin connection but no end one starts with its whole begin connection */
    n = strokes_of(&tiny, "ab", true, s, 64);
    CHECK(n == 3 && s[1].lift && s[1].x[1] == s[2].x[0] && s[1].length == 10 && !s[2].lift);
    n = strokes_of(&tiny, "bb", true, s, 64);
    CHECK(n == 3 && s[1].type == DGX_HW_CURVE && !s[1].lift && !s[2].lift);
    n = strokes_of(&tiny, "ba", true, s, 64);
    CHECK(n == 3 && s[1].type == DGX_HW_CURVE && s[2].length == 40);
    n = strokes_of(&tiny, "ab", false, s, 64);
    CHECK(n == 2);

    /* the effort of a text is that of its strokes and the least one for every move of the pen in the air */
    dgx_hw_pace_t pace = dgx_hw_pace(font, 5);
    const dgx_hw_symbol_t *hyphen = symbol('-');
    CHECK(hyphen->main_segments == 1 && pace.piece == 5);
    {
        int16_t        adv;
        const glyph_t *g = dgx_font_find_glyph('-', font, &adv);
        CHECK(fabsf(pace.least - (g->elements[0].length * 0.01f + 5 * g->elements[0].pieces)) < 0.001f);
    }
    CHECK(dgx_hw_pace(&tiny, 0).least == 58 * 2 / 3.0f);
    const char *text = "Ёжик, 15";
    n = strokes_of(font, text, true, s, 64);
    float sum = 0;
    int   lifts = 0;
    for (int i = 0; i < n; ++i) {
        sum += dgx_hw_stroke_effort(&s[i], &pace);
        lifts += s[i].lift;
        CHECK(dgx_hw_stroke_effort(&s[i], &pace) >= pace.least);
    }
    float total = dgx_hw_text_effort(font, text, true, &pace);
    CHECK(lifts > 0 && fabsf(total - (sum + lifts * pace.least)) < 0.01f);
    CHECK(dgx_hw_text_effort(font, text, true, NULL) < total);

    /* written by the effort in any number of calls: ink is only added and the end is what the whole text gives */
    static const int steps[] = {1, 3, 50, 700};
    for (int width = 1; width <= 4; ++width) {
        dgx_screen_t *whole = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
        dgx_hw_draw_text(whole, 5, 100, font, text, 0.4f, width, 0xff, true);
        CHECK(ink(whole) > 500);
        for (unsigned k = 0; k < sizeof(steps) / sizeof(steps[0]); ++k) {
            dgx_screen_t    *scr = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
            dgx_hw_writing_t w;
            dgx_hw_writing_begin(&w, scr, 5, 100, font, text, 0.4f, width, 0xff, true, &pace);
            CHECK(!dgx_hw_writing_draw_to(&w, 0) && ink(scr) == 0);
            int before = 0, outside = 0;
            for (int i = 1; i <= steps[k]; ++i) {
                bool finished = dgx_hw_writing_draw_to(&w, total * i / steps[k] + (i == steps[k] ? 0.5f : 0));
                CHECK(finished == (i == steps[k]));
                int now = ink(scr);
                CHECK(now >= before);
                before = now;
            }
            int differ = 0;
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    differ += dgx_get_pixel(scr, x, y) != dgx_get_pixel(whole, x, y);
                    outside += dgx_get_pixel(scr, x, y) && !dgx_get_pixel(whole, x, y);
                }
            }
            if (differ) fprintf(stderr, "width %d, %d steps: %d pixels differ, %d outside\n", width, steps[k], differ, outside);
            CHECK(differ == 0);
            CHECK(dgx_hw_writing_draw_to(&w, 0));
            dgx_screen_destroy(&scr);
        }
        /* at half the effort about half the ink is there, and the pen is in the middle of the text */
        dgx_screen_t    *half = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
        dgx_hw_writing_t w;
        dgx_hw_writing_begin(&w, half, 5, 100, font, text, 0.4f, width, 0xff, true, &pace);
        CHECK(!dgx_hw_writing_draw_to(&w, total / 2));
        CHECK(ink(half) > ink(whole) / 4 && ink(half) < ink(whole) * 3 / 4);
        dgx_screen_destroy(&half);
        dgx_screen_destroy(&whole);
    }

    /* the box of a text is where its ink is: nothing outside it but half the pen, and it is filled to its sides */
    {
        static const char *const texts[] = {"соревнований", "привет\nучастникам\nсоревнований", "Ёжик, 15!", "у"};
        for (unsigned k = 0; k < sizeof(texts) / sizeof(texts[0]); ++k) {
            for (int joined = 0; joined < 2; ++joined) {
                int left, top, right, bottom;
                CHECK(dgx_hw_text_box(font, texts[k], &left, &top, &right, &bottom));
                int           bw = right - left + 1, bh = bottom - top + 1;
                dgx_screen_t *scr = dgx_vscreen_init(bw + 20, bh + 20, 8, DgxScreenRGB);
                dgx_hw_draw_text(scr, 10 - left, 10 - top, font, texts[k], 1.0f, 1, 0xff, joined);
                int il = bw + 20, it = bh + 20, ir = -1, ib = -1;
                for (int y = 0; y < bh + 20; ++y) {
                    for (int x = 0; x < bw + 20; ++x) {
                        if (!dgx_get_pixel(scr, x, y)) continue;
                        if (x < il) il = x;
                        if (x > ir) ir = x;
                        if (y < it) it = y;
                        if (y > ib) ib = y;
                    }
                }
                if (il < 9 || it < 9 || ir > bw + 10 || ib > bh + 10 || il > 12 || it > 12 || ir < bw + 7 || ib < bh + 7) {
                    fprintf(stderr, "text %u joined %d: box %d..%d x %d..%d, ink %d..%d x %d..%d\n", k, joined, 10, bw + 9, 10, bh + 9, il,
                            ir, it, ib);
                }
                CHECK(il >= 9 && it >= 9 && ir <= bw + 10 && ib <= bh + 10);
                CHECK(il <= 12 && it <= 12 && ir >= bw + 7 && ib >= bh + 7);
                dgx_screen_destroy(&scr);
            }
        }
        int left = 7, top = 7, right = 7, bottom = 7;
        CHECK(!dgx_hw_text_box(font, " @\n", &left, &top, &right, &bottom) && left == 7 && bottom == 7);
        /* lines are symbol_size_y apart */
        int l1, t1, r1, b1, l2, t2, r2, b2;
        CHECK(dgx_hw_text_box(font, "о", &l1, &t1, &r1, &b1) && dgx_hw_text_box(font, "о\nо", &l2, &t2, &r2, &b2));
        CHECK(l1 == l2 && t1 == t2 && r1 == r2 && b2 == b1 + font->hw->symbol_size_y);
    }

    /* a font of another kind, any size and any place: nothing is written and nothing breaks */
    {
        dgx_screen_t    *scr = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
        dgx_font_t      *other = TerminusTTFMedium12();
        dgx_hw_writing_t w;
        int              left = 7, top, right, bottom;
        CHECK(!dgx_hw_text_box(other, "abc", &left, &top, &right, &bottom) && left == 7);
        CHECK(dgx_hw_text_effort(other, "abc", true, NULL) == 0 && strokes_of(other, "abc", true, s, 64) == 0);
        dgx_hw_draw_text(scr, 5, 100, other, "abc", 0.4f, 3, 0xff, true);
        dgx_hw_writing_begin(&w, scr, 5, 100, other, "abc", 0.4f, 3, 0xff, true, NULL);
        CHECK(dgx_hw_writing_draw_to(&w, 1));
        CHECK(ink(scr) == 0);
        static const float sizes[] = {0, -1, 1e9f, INFINITY, NAN};
        for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
            dgx_hw_draw_text(scr, 5, 100, font, text, sizes[k], 3, 0xff, true);
            dgx_hw_draw_text(scr, 2000000000, -2000000000, font, text, 0.4f, 3, 0xff, true);
        }
        dgx_hw_writing_begin(&w, scr, 5, 100, font, text, 0.4f, 3, 0xff, true, &pace);
        CHECK(!dgx_hw_writing_draw_to(&w, NAN) && !dgx_hw_writing_draw_to(&w, -5));
        CHECK(dgx_hw_writing_draw_to(&w, INFINITY));
        dgx_screen_destroy(&scr);
    }

    /* one flush for a call, however many strokes it writes */
    {
        dgx_screen_t *scr = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
        CHECK(scr->in_progress == 0);
        dgx_hw_draw_text(scr, 5, 100, font, text, 0.4f, 3, 0xff, false);
        CHECK(scr->in_progress == 0 && ink(scr) > 500);
        dgx_screen_destroy(&scr);
    }
    CHECK_DONE();
}
