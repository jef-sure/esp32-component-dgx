#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_matrix_morph.h"
#include "dgx_morph.h"
#include "dgx_morph_sources.h"
#include "fonts/CasusDotView.h"
#include "fonts/TerminusTTFMedium12.h"

typedef struct {
    int count;
    int last_x, last_y;
    uint8_t last_intensity;
} dots_t;

static void collect(void *user_data, const dgx_point_2d_t *p, uint8_t intensity)
{
    dots_t *d = user_data;
    ++d->count;
    d->last_x = p->x;
    d->last_y = p->y;
    d->last_intensity = intensity;
}

static dgx_bit_matrix_t *row(const char *cells)
{
    dgx_bit_matrix_t *m = dgx_matrix_init((uint16_t)strlen(cells), 1);
    for (int i = 0; cells[i]; ++i) dgx_matrix_set_point(m, i, 0, cells[i] == '#');
    return m;
}

static void test_classification(void)
{
    dgx_bit_matrix_t *from = row("##..");
    dgx_bit_matrix_t *to = row("#.#.");

    dgx_morph_t *m = dgx_morph_create(from, to, NULL, NULL);
    CHECK(m && m->width == 4 && m->height == 1);
    CHECK(m->number_of_static_points == 1);
    CHECK(m->number_of_segments == 1);
    CHECK(m->number_of_fading_points == 1);
    /* without sources a new cell appears from the center */
    CHECK(m->segments[0].start.x == 2 && m->segments[0].end.x == 2);
    CHECK(m->segments[0].start_intensity == 0 && m->segments[0].end_intensity == 255);
    dgx_morph_destroy(&m);
    CHECK(m == NULL);

    m = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
    CHECK(m && m->number_of_segments >= 1 && m->number_of_fading_points == 0);
    for (size_t i = 0; m && i < m->number_of_segments; ++i) {
        CHECK(dgx_matrix_get_point(from, m->segments[i].start.x, m->segments[i].start.y));
    }
    dgx_morph_destroy(&m);

    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
}

static void test_to_empty(void)
{
    dgx_bit_matrix_t *from = row("#.##.");
    dgx_morph_t *m = dgx_morph_create(from, NULL, dgx_morph_sources_cells, NULL);
    CHECK(m && m->number_of_segments == 3 && m->number_of_static_points == 0);
    for (size_t i = 0; m && i < m->number_of_segments; ++i) {
        CHECK(m->segments[i].end.x == 2 && m->segments[i].end.y == 0);
        CHECK(m->segments[i].start_intensity == 255 && m->segments[i].end_intensity == 0);
    }
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);

    m = dgx_morph_create(NULL, NULL, NULL, NULL);
    CHECK(m && m->width == 0 && m->number_of_segments == 0);
    dgx_morph_destroy(&m);
}

static int from_origin(const dgx_morph_ctx_t *ctx, int x, int y, int pass,
                       dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    (void)ctx, (void)x, (void)y, (void)pass, (void)user_data;
    out[0] = (dgx_point_2d_t){ .x = 0, .y = 0 };
    return 1;
}

static void test_draw(void)
{
    dgx_bit_matrix_t *from = row("#...");
    dgx_bit_matrix_t *to = row("...#");
    dgx_morph_t *m = dgx_morph_create(from, to, from_origin, NULL);
    CHECK(m && m->number_of_segments == 1 && m->number_of_fading_points == 0);

    dots_t d = { 0 };
    dgx_morph_draw(m, 1.0f, 10, 20, 4, false, collect, &d);
    CHECK(d.count == 1 && d.last_x == 10 + 2 + 3 * 4 && d.last_y == 20 + 2);

    memset(&d, 0, sizeof(d));
    dgx_morph_draw(m, 0.5f, 0, 0, 4, true, collect, &d);
    CHECK(d.count == 2);

    /* dots that do not fit int16_t are skipped */
    memset(&d, 0, sizeof(d));
    dgx_morph_draw(m, 1.0f, INT16_MAX, 0, 4, false, collect, &d);
    CHECK(d.count == 0);
    memset(&d, 0, sizeof(d));
    dgx_morph_draw(m, 0.0f, 0, 0, 20000, false, collect, &d);
    CHECK(d.count == 1);
    memset(&d, 0, sizeof(d));
    dgx_morph_draw(m, 1.0f, 0, 0, 20000, false, collect, &d);
    CHECK(d.count == 0);

    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
}

static void test_int16_guard(void)
{
    dgx_bit_matrix_t *wide = dgx_matrix_init(INT16_MAX + 1, 1);
    CHECK(wide != NULL);
    CHECK(dgx_morph_create(wide, NULL, NULL, NULL) == NULL);
    dgx_matrix_destroy(&wide);
}

static dgx_bit_matrix_t *grid(int w, int h, const int (*cells)[2], int n)
{
    dgx_bit_matrix_t *m = dgx_matrix_init((uint16_t)w, (uint16_t)h);
    for (int i = 0; i < n; ++i) dgx_matrix_set_point(m, cells[i][0], cells[i][1], true);
    return m;
}

static void test_life_blinker(void)
{
    const int v[][2] = { { 2, 1 }, { 2, 2 }, { 2, 3 } };
    const int h[][2] = { { 1, 2 }, { 2, 2 }, { 3, 2 } };
    dgx_bit_matrix_t *from = grid(5, 5, v, 3);
    dgx_bit_matrix_t *to = grid(5, 5, h, 3);
    dgx_morph_t *m = dgx_morph_create(from, to, dgx_morph_sources_life, NULL);
    CHECK(m && m->number_of_segments == 6);
    CHECK(m->number_of_static_points == 1 && m->number_of_fading_points == 0);
    for (size_t i = 0; m && i < m->number_of_segments; ++i) {
        CHECK(m->segments[i].start_intensity == 85 && m->segments[i].end_intensity == 85);
    }
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
}

static void expect_single_flight(const int (*f)[2], int nf, const int (*t)[2], int nt,
                                 int sx, int sy, int ex, int ey, size_t fading)
{
    dgx_bit_matrix_t *from = grid(10, 10, f, nf);
    dgx_bit_matrix_t *to = grid(10, 10, t, nt);
    dgx_morph_t *m = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
    CHECK(m && m->number_of_segments == 1 && m->number_of_fading_points == fading);
    if (m && m->number_of_segments == 1) {
        const dgx_morph_segment_t *s = &m->segments[0];
        CHECK(s->start.x == sx && s->start.y == sy && s->end.x == ex && s->end.y == ey);
        CHECK(s->start_intensity == 255 && s->end_intensity == 255);
    }
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
}

static void test_cells(void)
{
    /* far source found on a ring */
    expect_single_flight((const int[][2]){ { 7, 5 } }, 1, (const int[][2]){ { 5, 2 } }, 1, 7, 5, 5, 2, 0);
    /* static dot may be a source */
    expect_single_flight((const int[][2]){ { 2, 2 } }, 1, (const int[][2]){ { 2, 2 }, { 3, 2 } }, 2, 2, 2, 3, 2, 0);
    /* axis neighbor before diagonal; the unused diagonal fades */
    expect_single_flight((const int[][2]){ { 1, 1 }, { 2, 1 } }, 2, (const int[][2]){ { 2, 2 } }, 1, 2, 1, 2, 2, 1);
}

static int always_defer(const dgx_morph_ctx_t *ctx, int x, int y, int pass,
                        dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    (void)ctx, (void)x, (void)y, (void)pass, (void)out, (void)user_data;
    return DGX_MORPH_DEFER;
}

static void test_edge_cases(void)
{
    dgx_bit_matrix_t *a = row("#..");
    dgx_bit_matrix_t *b = row("..#");
    dgx_morph_t *m = dgx_morph_create(a, b, always_defer, NULL);
    CHECK(m && m->number_of_segments == 1 && m->segments[0].start_intensity == 0);
    dgx_morph_destroy(&m);

    /* empty from: everything appears from the center */
    m = dgx_morph_create(NULL, b, dgx_morph_sources_cells, NULL);
    CHECK(m && m->number_of_segments == 1 && m->segments[0].start.x == 1);
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&a);
    dgx_matrix_destroy(&b);

    /* different sizes: the morph grid is their union */
    a = dgx_matrix_init(3, 3);
    b = dgx_matrix_init(5, 2);
    m = dgx_morph_create(a, b, NULL, NULL);
    CHECK(m && m->width == 5 && m->height == 3);
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&a);
    dgx_matrix_destroy(&b);

    /* dense checkerboard: more flights than new cells */
    a = dgx_matrix_init(16, 16);
    b = dgx_matrix_init(16, 16);
    size_t n_new = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            dgx_matrix_set_point((x + y) % 2 ? a : b, x, y, true);
            n_new += (x + y) % 2 == 0;
        }
    }
    m = dgx_morph_create(a, b, dgx_morph_sources_life, NULL);
    CHECK(m && m->number_of_segments > n_new);
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&a);
    dgx_matrix_destroy(&b);
}

static void sum_at(void *user_data, const dgx_point_2d_t *p, uint8_t intensity)
{
    int *sum = user_data;
    if (p->x == 2 + 3 * 4 && p->y == 2) *sum += intensity;
}

static void test_trail_end(void)
{
    dgx_bit_matrix_t *from = row("#...");
    dgx_bit_matrix_t *to = row("...#");
    dgx_morph_t *m = dgx_morph_create(from, to, from_origin, NULL);
    int sum = 0;
    dgx_morph_draw(m, 1.0f, 0, 0, 4, true, sum_at, &sum);
    CHECK(sum == 254);
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
}

static void test_glyphs(void)
{
    dgx_font_t *font = CasusDotView();
    dgx_bit_matrix_t *a = dgx_morph_glyph_matrix(font, '0');
    dgx_bit_matrix_t *b = dgx_morph_glyph_matrix(font, '1');
    CHECK(a && b && a->width == b->width && a->height == b->height);
    CHECK(a->height == font->yBottomMax - font->yOffsetLowest);

    for (const glyph_array_t *r = font->glyph_ranges; r && r->number; ++r) {
        for (int i = 0; i < r->number; ++i) {
            dgx_bit_matrix_t *g = dgx_morph_glyph_matrix(font, r->first + (uint32_t)i);
            CHECK(g && g->number_of_set_cells == r->glyphs[i].number_of_dots);
            dgx_matrix_destroy(&g);
        }
    }

    dgx_bit_matrix_t *space = dgx_morph_glyph_matrix(font, ' ');
    dgx_morph_t *m = dgx_morph_create(a, space, dgx_morph_sources_cells, NULL);
    CHECK(m && m->number_of_segments == a->number_of_set_cells);
    for (size_t i = 0; m && i < m->number_of_segments; ++i) {
        CHECK(m->segments[i].end.x == a->width / 2 && m->segments[i].end.y == a->height / 2);
    }
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&space);
    dgx_matrix_destroy(&a);
    dgx_matrix_destroy(&b);
}

static void test_timing(void)
{
    CHECK(dgx_morph_progress(100, 50, 100) == 0.0f);
    CHECK(dgx_morph_progress(100, 150, 100) == 0.5f);
    CHECK(dgx_morph_progress(100, 500, 100) == 1.0f);
    CHECK(dgx_morph_progress(100, 0, 0) == 1.0f);
    CHECK(dgx_morph_ease(DGX_MORPH_SMOOTHSTEP, 0.5f) == 0.5f);
    CHECK(dgx_morph_ease(DGX_MORPH_EASE_IN_OUT, 1.0f) == 1.0f);
}

static void test_text(void)
{
    dgx_font_t *font = TerminusTTFMedium12();
    CHECK(dgx_morph_text_length("привет") == 6);
    CHECK(dgx_morph_text_length("") == 0 && dgx_morph_text_length(NULL) == 0);

    dgx_morph_text_t *t = dgx_morph_text_create(font, "кот", "кит", 0, dgx_morph_sources_cells, NULL);
    CHECK(t && t->length == 3 && t->changed == 2);
    CHECK(t->width == font->xRightMax - font->xOffsetLowest);
    CHECK(t->height == font->yBottomMax - font->yOffsetLowest);
    CHECK(dgx_morph_text_duration_us(t, 500, 250) == 250 + 500);
    for (size_t i = 0; t && i < t->length; ++i) CHECK(t->letters[i] != NULL);
    CHECK(t->letters[0]->number_of_segments == 0 && t->letters[0]->number_of_fading_points == 0);
    dgx_morph_text_destroy(&t);
    CHECK(t == NULL);

    t = dgx_morph_text_create(font, NULL, "ab", 5, NULL, NULL);
    CHECK(t && t->length == 5 && t->changed == 2);
    dgx_morph_text_destroy(&t);

    t = dgx_morph_text_create(font, "same", "same", 0, NULL, NULL);
    CHECK(t && t->changed == 0 && dgx_morph_text_duration_us(t, 500, 250) == 0);
    dgx_morph_text_destroy(&t);

    t = dgx_morph_text_create(font, "abcdef", "", 2, NULL, NULL);
    CHECK(t && t->length == 2 && t->changed == 2);
    dgx_morph_text_destroy(&t);

    CHECK(dgx_morph_text_create(NULL, "a", "b", 0, NULL, NULL) == NULL);
}

int main(void)
{
    test_classification();
    test_to_empty();
    test_draw();
    test_life_blinker();
    test_cells();
    test_edge_cases();
    test_trail_end();
    test_glyphs();
    test_int16_guard();
    test_timing();
    test_text();
    CHECK_DONE();
}
