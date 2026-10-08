#include <math.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "drivers/vscreen.h"

/* font0-ss: the numbers below are of the font as it is in src/fonts */
#include "fonts/font0ss.h"

#define W 330
#define H 110
#define X 8
#define Y 70
#define SCALE 0.4f

static dgx_screen_t *screen(void)
{
    return dgx_vscreen_init(W, H, 8, DgxScreenRGB);
}

static int ink(dgx_screen_t *s)
{
    int n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) n += dgx_get_pixel(s, x, y) != 0;
    }
    return n;
}

/* pixels of a which have no pixel of b within a pixel around */
static int apart(dgx_screen_t *a, dgx_screen_t *b)
{
    int n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!dgx_get_pixel(a, x, y)) continue;
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; ++dy) {
                for (int dx = -1; dx <= 1 && !near; ++dx) {
                    int px = x + dx, py = y + dy;
                    near = px >= 0 && py >= 0 && px < W && py < H && dgx_get_pixel(b, px, py);
                }
            }
            n += !near;
        }
    }
    return n;
}

/* the same picture but for a pixel here and there on the edge of a line */
static bool same(dgx_screen_t *a, dgx_screen_t *b)
{
    int da = apart(a, b), db = apart(b, a);
    if (da || db) fprintf(stderr, "pictures differ: %d and %d pixels apart\n", da, db);
    return !da && !db && ink(a) > 0;
}

static int strokes_of(dgx_font_t *font, const char *text, bool joined)
{
    dgx_hw_writer_t w;
    dgx_hw_stroke_t s;
    int             n = 0;
    dgx_hw_writer_begin(&w, font, text, joined);
    while (dgx_hw_writer_next(&w, &s)) ++n;
    return n;
}

/* the way all the points of a morph go */
static float way_of(const dgx_hw_morph_t *m)
{
    float way = 0;
    for (int i = 0; i < m->number; ++i) {
        for (int k = 0; k < 4; ++k) way += hypotf(m->to[i].x[k] - m->from[i].x[k], m->to[i].y[k] - m->from[i].y[k]);
    }
    return way;
}

/* the same for the strokes of two symbols paired in the order of writing, what is left over going into the last point */
static float way_in_order(dgx_font_t *font, const char *a, const char *b)
{
    dgx_hw_writer_t wa, wb;
    dgx_hw_stroke_t sa, sb;
    float           way = 0, ax = 0, ay = 0, bx = 0, by = 0;
    dgx_hw_writer_begin(&wa, font, a, false);
    dgx_hw_writer_begin(&wb, font, b, false);
    for (;;) {
        bool ha = dgx_hw_writer_next(&wa, &sa), hb = dgx_hw_writer_next(&wb, &sb);
        if (!ha && !hb) break;
        for (int k = 0; k < 4; ++k) {
            /* the strokes of the digits used here are cubic curves, lines and dots: a dot is its point four times */
            float pax = ha ? sa.x[sa.type == 4 ? k : (k < 2 ? 0 : sa.type - 1)] : ax, pay = ha ? sa.y[sa.type == 4 ? k : (k < 2 ? 0 : sa.type - 1)] : ay;
            float pbx = hb ? sb.x[sb.type == 4 ? k : (k < 2 ? 0 : sb.type - 1)] : bx, pby = hb ? sb.y[sb.type == 4 ? k : (k < 2 ? 0 : sb.type - 1)] : by;
            way += hypotf(pbx - pax, pby - pay);
        }
        if (ha) ax = sa.x[sa.type - 1], ay = sa.y[sa.type - 1];
        if (hb) bx = sb.x[sb.type - 1], by = sb.y[sb.type - 1];
    }
    return way;
}

/* no frame leaves the box of the morph, but for half the pen */
static bool in_box(const dgx_hw_morph_t *m, dgx_screen_t *s, int width)
{
    int l, t, r, b;
    if (!dgx_hw_morph_box(m, &l, &t, &r, &b)) return ink(s) == 0;
    int x0 = X + (int)floorf(l * SCALE) - width, x1 = X + (int)ceilf(r * SCALE) + width;
    int y0 = Y + (int)floorf(t * SCALE) - width, y1 = Y + (int)ceilf(b * SCALE) + width;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (dgx_get_pixel(s, x, y) && (x < x0 || x > x1 || y < y0 || y > y1)) return false;
        }
    }
    return true;
}

int main(void)
{
    dgx_font_t  *font = font0ss();
    const char  *from = "привет", *to = "участникам";
    dgx_screen_t *a = screen(), *b = screen();
    dgx_hw_draw_text(a, X, Y, font, from, SCALE, 3, 0xff, true);
    dgx_hw_draw_text(b, X, Y, font, to, SCALE, 3, 0xff, true);

    /* a line as a whole: as many pairs as the longer way has strokes, the ends are the two texts */
    dgx_hw_morph_t *m = dgx_hw_morph_create(font, from, to, true);
    int             na = strokes_of(font, from, true), nb = strokes_of(font, to, true);
    CHECK(m && m->number == (na > nb ? na : nb) && m->changes);
    dgx_screen_t *s = screen();
    dgx_hw_morph_draw(m, 0, s, X, Y, SCALE, 3, 0xff);
    CHECK(same(s, a) && in_box(m, s, 3));
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_hw_morph_draw(m, 1, s, X, Y, SCALE, 3, 0xff);
    CHECK(same(s, b) && in_box(m, s, 3));
    /* the curves follow one another as the pen goes: every pair begins where the one before ended, or after a lift */
    for (int i = 0; i < m->number; ++i) CHECK(m->nothing[i] == 0);
    /* between the texts: a line of its own, of about as much ink, inside the box, and t outside 0 .. 1 is an end */
    for (int k = 1; k < 10; ++k) {
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_hw_morph_draw(m, k / 10.0f, s, X, Y, SCALE, 3, 0xff);
        CHECK(in_box(m, s, 3));
        CHECK(ink(s) > ink(a) / 3 && ink(s) < ink(a) + ink(b));
    }
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_hw_morph_draw(m, 0.5f, s, X, Y, SCALE, 3, 0xff);
    CHECK(apart(s, a) > 50 && apart(s, b) > 50);
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_hw_morph_draw(m, -3, s, X, Y, SCALE, 3, 0xff);
    CHECK(same(s, a));
    dgx_hw_morph_destroy(&m);
    CHECK(m == NULL);
    dgx_hw_morph_destroy(&m);

    /* the other way round the same pairs; a text into itself changes nothing */
    m = dgx_hw_morph_create(font, to, from, true);
    CHECK(m && m->number == (na > nb ? na : nb));
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_hw_morph_draw(m, 1, s, X, Y, SCALE, 3, 0xff);
    CHECK(same(s, a));
    dgx_hw_morph_destroy(&m);
    m = dgx_hw_morph_create(font, from, from, true);
    CHECK(m && m->number == na && !m->changes);
    dgx_hw_morph_destroy(&m);

    /* out of nothing and into nothing: no dots where a stroke is not there yet */
    m = dgx_hw_morph_create(font, "", to, true);
    CHECK(m && m->number == nb && m->changes && m->nothing[0] == DGX_HW_MORPH_FROM_NOTHING);
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_hw_morph_draw(m, 0, s, X, Y, SCALE, 3, 0xff);
    CHECK(ink(s) == 0);
    dgx_hw_morph_draw(m, 0.5f, s, X, Y, SCALE, 3, 0xff);
    CHECK(ink(s) > 0 && ink(s) < ink(b));
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_hw_morph_draw(m, 1, s, X, Y, SCALE, 3, 0xff);
    CHECK(same(s, b));
    dgx_hw_morph_destroy(&m);
    m = dgx_hw_morph_create(font, to, " ", true);
    CHECK(m && m->nothing[0] == DGX_HW_MORPH_TO_NOTHING);
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    dgx_hw_morph_draw(m, 1, s, X, Y, SCALE, 3, 0xff);
    CHECK(ink(s) == 0);
    dgx_hw_morph_destroy(&m);
    CHECK(dgx_hw_morph_create(font, "", " @", true) == NULL);
    dgx_hw_morph_draw(NULL, 0.5f, s, X, Y, SCALE, 3, 0xff);
    int l, t, r, bt;
    CHECK(!dgx_hw_morph_box(NULL, &l, &t, &r, &bt));

    /* letter by letter: a morph for every position, all at 0 is the first text and all at 1 the second one */
    dgx_hw_morph_text_t *text = dgx_hw_morph_text_create(font, from, to, true);
    CHECK(text && text->length == 10 && text->changed == 10);
    int pairs = 0;
    for (size_t i = 0; i < text->length; ++i) {
        CHECK(text->letters[i] && text->letters[i]->changes);
        pairs += text->letters[i]->number;
    }
    CHECK(pairs >= nb && pairs <= na + nb);
    /* "привет" has six letters: the other four grow from where its pen ended */
    for (size_t i = 6; i < text->length; ++i) {
        const dgx_hw_morph_t *letter = text->letters[i];
        for (int k = 0; k < letter->number; ++k) {
            CHECK(letter->nothing[k] == DGX_HW_MORPH_FROM_NOTHING);
            CHECK(letter->from[k].x[0] == text->letters[6]->from[0].x[0] && letter->from[k].x[3] == letter->from[k].x[0]);
        }
    }
    for (int end = 0; end < 2; ++end) {
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        for (size_t i = 0; i < text->length; ++i) dgx_hw_morph_draw(text->letters[i], end, s, X, Y, SCALE, 3, 0xff);
        CHECK(same(s, end ? b : a));
    }
    /* one after another: the first letters are already of the second text while the last ones are still of the first */
    dgx_fill_rectangle(s, 0, 0, W, H, 0);
    for (size_t i = 0; i < text->length; ++i) dgx_hw_morph_draw(text->letters[i], i < 3 ? 1 : 0, s, X, Y, SCALE, 3, 0xff);
    CHECK(ink(s) > 0 && apart(s, a) > 20 && apart(s, b) > 20);
    CHECK(dgx_hw_morph_text_duration_us(text, 1000, 250) == 9 * 250 + 1000);
    dgx_hw_morph_text_destroy(&text);
    CHECK(text == NULL);

    /* each text moved by its own shift: the ends are the texts in their new places */
    m = dgx_hw_morph_create(font, from, to, true);
    dgx_hw_morph_shift(m, 40, -10, -5, 20);
    {
        dgx_screen_t *moved = screen();
        dgx_hw_draw_text(moved, X + 16, Y - 4, font, from, SCALE, 3, 0xff, true);
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_hw_morph_draw(m, 0, s, X, Y, SCALE, 3, 0xff);
        CHECK(same(s, moved) && in_box(m, s, 3));
        dgx_fill_rectangle(moved, 0, 0, W, H, 0);
        dgx_hw_draw_text(moved, X - 2, Y + 8, font, to, SCALE, 3, 0xff, true);
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_hw_morph_draw(m, 1, s, X, Y, SCALE, 3, 0xff);
        CHECK(same(s, moved));
        dgx_screen_destroy(&moved);
    }
    dgx_hw_morph_destroy(&m);
    /* the same text in another place does change */
    text = dgx_hw_morph_text_create(font, "12", "12", false);
    CHECK(text->changed == 0);
    dgx_hw_morph_text_shift(text, 0, 0, 30, 0);
    CHECK(text->changed == 2 && text->letters[0]->changes);
    dgx_hw_morph_text_destroy(&text);
    dgx_hw_morph_shift(NULL, 1, 1, 1, 1);
    dgx_hw_morph_text_shift(NULL, 1, 1, 1, 1);

    /* a size of its own along each axis: the ends are the two texts drawn with those sizes */
    m = dgx_hw_morph_create(font, "3", "8", false);
    {
        dgx_screen_t *digit = screen();
        for (int end = 0; end < 2; ++end) {
            dgx_fill_rectangle(digit, 0, 0, W, H, 0);
            dgx_hw_draw_text_xy(digit, 30, 100, font, end ? "8" : "3", 0.33f, 0.7f, 3, 0xff, false);
            dgx_fill_rectangle(s, 0, 0, W, H, 0);
            dgx_hw_morph_draw_xy(m, end, s, 30, 100, 0.33f, 0.7f, 3, 0xff);
            CHECK(same(s, digit));
        }
        dgx_fill_rectangle(digit, 0, 0, W, H, 0);
        dgx_hw_morph_draw(m, 0.4f, digit, 30, 100, 0.5f, 3, 0xff);
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_hw_morph_draw_xy(m, 0.4f, s, 30, 100, 0.5f, 0.5f, 3, 0xff);
        CHECK(apart(s, digit) == 0 && apart(digit, s) == 0 && ink(s) > 0);
        dgx_screen_destroy(&digit);
    }
    dgx_hw_morph_destroy(&m);

    /*
     * Strokes are paired by the shortest way of their points, not by the order
     * of writing: "9" is written oval first and tail last, "7" top first. The
     * ends of a morph are still the two symbols, and no pair of digits goes a
     * longer way than in the order of writing; those with "9" go a much shorter one.
     */
    {
        char   a[2] = "0", b[2] = "0";
        float  paired = 0, ordered = 0;
        for (a[0] = '0'; a[0] <= '9'; ++a[0]) {
            for (b[0] = '0'; b[0] <= '9'; ++b[0]) {
                text = dgx_hw_morph_text_create(font, a, b, false);
                CHECK(text && text->length == 1 && text->letters[0]);
                CHECK(text->letters[0]->changes == (a[0] != b[0]));
                float w = way_of(text->letters[0]), o = way_in_order(font, a, b);
                /* a line is a curve with its points on thirds, which way_in_order has at the ends: a little slack */
                CHECK(w <= o + 60);
                if (a[0] == b[0]) CHECK(w == 0);
                paired += w, ordered += o;
                dgx_hw_morph_text_destroy(&text);
            }
        }
        CHECK(paired < ordered * 0.8f);
        text = dgx_hw_morph_text_create(font, "2", "9", false);
        CHECK(way_of(text->letters[0]) < way_in_order(font, "2", "9") * 0.5f);
        /* the ends are the digits themselves, whichever way their curves were taken */
        dgx_screen_t *two = screen(), *nine = screen();
        dgx_hw_draw_text(two, X, Y, font, "2", SCALE, 3, 0xff, false);
        dgx_hw_draw_text(nine, X, Y, font, "9", SCALE, 3, 0xff, false);
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_hw_morph_draw(text->letters[0], 0, s, X, Y, SCALE, 3, 0xff);
        CHECK(same(s, two));
        dgx_fill_rectangle(s, 0, 0, W, H, 0);
        dgx_hw_morph_draw(text->letters[0], 1, s, X, Y, SCALE, 3, 0xff);
        CHECK(same(s, nine));
        dgx_screen_destroy(&two);
        dgx_screen_destroy(&nine);
        dgx_hw_morph_text_destroy(&text);
    }

    /* a clock: the positions that keep their symbol do not change, the others do */
    text = dgx_hw_morph_text_create(font, "12:34", "12:35", false);
    CHECK(text && text->length == 5 && text->changed == 5);
    for (size_t i = 0; i < 4; ++i) CHECK(text->letters[i] && !text->letters[i]->changes);
    CHECK(text->letters[4]->changes);
    CHECK(dgx_hw_morph_text_duration_us(text, 1000, 0) == 1000);
    dgx_hw_morph_text_destroy(&text);
    text = dgx_hw_morph_text_create(font, "1 1", "1 1", false);
    CHECK(text && text->length == 3 && text->changed == 0 && text->letters[1] == NULL);
    CHECK(dgx_hw_morph_text_duration_us(text, 1000, 250) == 0);
    dgx_hw_morph_text_destroy(&text);
    /* a stroke more in a symbol grows from the end of the other symbol: "0" has a bar, "о" has none */
    text = dgx_hw_morph_text_create(font, "о", "ё", false);
    CHECK(text && text->length == 1);
    {
        const dgx_hw_morph_t *letter = text->letters[0];
        int                   grown = 0;
        for (int k = 0; k < letter->number; ++k) grown += letter->nothing[k] == DGX_HW_MORPH_FROM_NOTHING;
        CHECK(letter->number == 4 && grown == 1 && letter->nothing[3] == DGX_HW_MORPH_FROM_NOTHING);
        CHECK(letter->from[3].x[0] == letter->from[2].x[3] && letter->from[3].y[0] == letter->from[2].y[3]);
    }
    dgx_hw_morph_text_destroy(&text);
    text = dgx_hw_morph_text_create(font, "", "", true);
    CHECK(text && text->length == 0 && text->changed == 0);
    dgx_hw_morph_text_destroy(&text);

    dgx_screen_destroy(&s);
    dgx_screen_destroy(&a);
    dgx_screen_destroy(&b);
    CHECK_DONE();
}
