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

/* count the cells of a morph that fly in from a given cell */
static int flights_from(const dgx_morph_t *m, int x, int y, int *to_x)
{
    int n = 0;
    for (size_t i = 0; i < m->number_of_segments; ++i) {
        if (m->segments[i].start.x == x && m->segments[i].start.y == y && m->segments[i].start_intensity) {
            ++n;
            if (to_x) *to_x = m->segments[i].end.x;
        }
    }
    return n;
}

/*
 * All the new cells look at one radius before any of them looks at the next:
 * a cell earlier in the scan does not take a source four cells away from a
 * cell that has it three cells away.
 */
static void test_cells_by_radius(void)
{
    dgx_bit_matrix_t *from = dgx_matrix_init(12, 1), *to = dgx_matrix_init(12, 1);
    dgx_matrix_set_point(from, 5, 0, true);
    dgx_matrix_set_point(to, 1, 0, true); /* four cells from the source, first in the scan */
    dgx_matrix_set_point(to, 8, 0, true); /* three cells from it */
    dgx_morph_t *m = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
    int          to_x = -1;
    CHECK(m && m->number_of_segments == 2 && m->number_of_fading_points == 0);
    CHECK(flights_from(m, 5, 0, &to_x) == 1 && to_x == 8);
    /* the other one has nothing left and appears from the center */
    for (size_t i = 0; i < m->number_of_segments; ++i) {
        if (m->segments[i].end.x == 1) CHECK(m->segments[i].start_intensity == 0 && m->segments[i].start.x == 6);
    }
    dgx_morph_destroy(&m);

    /* two sources, two cells: each gets the nearer one whatever the scan order is */
    dgx_matrix_set_point(from, 0, 0, true);
    m = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
    CHECK(m && m->number_of_segments == 2);
    CHECK(flights_from(m, 0, 0, &to_x) == 1 && to_x == 1);
    CHECK(flights_from(m, 5, 0, &to_x) == 1 && to_x == 8);
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);

    /* a source farther than eight rings is still found: a grid 40 cells across */
    from = dgx_matrix_init(40, 3), to = dgx_matrix_init(40, 3);
    dgx_matrix_set_point(from, 0, 1, true);
    dgx_matrix_set_point(to, 39, 1, true);
    m = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
    CHECK(m && m->number_of_segments == 1 && flights_from(m, 0, 1, &to_x) == 1 && to_x == 39);
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
}

/* the vectors of the search: ring by ring, the axes first, then from each axis toward the corners */
static void test_scan_vector(void)
{
    static const int ring1[8][2] = {{0, -1}, {0, 1}, {1, 0}, {-1, 0}, {1, -1}, {-1, -1}, {1, 1}, {-1, 1}};
    static const int ring2[16][2] = {{0, -2}, {0, 2}, {2, 0}, {-2, 0}, {1, -2}, {-1, -2}, {1, 2}, {-1, 2},
                                     {2, -1}, {2, 1}, {-2, -1}, {-2, 1}, {2, -2}, {-2, -2}, {2, 2}, {-2, 2}};
    int              dx, dy;
    for (int i = 0; i < 8; ++i) CHECK(dgx_morph_scan_vector(i, &dx, &dy) && dx == ring1[i][0] && dy == ring1[i][1]);
    for (int i = 0; i < 16; ++i) CHECK(dgx_morph_scan_vector(8 + i, &dx, &dy) && dx == ring2[i][0] && dy == ring2[i][1]);
    CHECK(!dgx_morph_scan_vector(-1, &dx, &dy) && dgx_morph_scan_vector(3, NULL, NULL));
    /* every ring has each of its cells once, and the nearer ones come first */
    int pass = 0;
    for (int r = 1; r <= 9; ++r) {
        static uint8_t seen[19][19];
        memset(seen, 0, sizeof(seen));
        int side = 0;
        for (int i = 0; i < 8 * r; ++i, ++pass) {
            CHECK(dgx_morph_scan_vector(pass, &dx, &dy));
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            CHECK((ax > ay ? ax : ay) == r);
            CHECK(!seen[dy + 9][dx + 9]);
            seen[dy + 9][dx + 9] = 1;
            int off = ax < ay ? ax : ay;
            CHECK(off >= side);
            side = off;
        }
    }
}

static int same_as_cells(const dgx_morph_ctx_t *ctx, int x, int y, int pass, dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    return dgx_morph_sources_cells(ctx, x, y, pass, out, user_data);
}

static int segment_cmp(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(dgx_morph_segment_t));
}

/* the planner's own quick search by vectors gives what asking the callback cell by cell, pass by pass, gives */
static void check_same_plans(dgx_morph_t *quick, dgx_morph_t *asked)
{
    CHECK(quick && asked && quick->number_of_segments == asked->number_of_segments);
    CHECK(quick->number_of_fading_points == asked->number_of_fading_points);
    if (quick && asked && quick->number_of_segments == asked->number_of_segments) {
        /* zeroed first: a segment has padding */
        dgx_morph_segment_t *q = calloc(quick->number_of_segments + 1, sizeof(*q)), *c = calloc(quick->number_of_segments + 1, sizeof(*c));
        for (size_t i = 0; i < quick->number_of_segments; ++i) {
            q[i].start = quick->segments[i].start, q[i].end = quick->segments[i].end;
            q[i].start_intensity = quick->segments[i].start_intensity, q[i].end_intensity = quick->segments[i].end_intensity;
            c[i].start = asked->segments[i].start, c[i].end = asked->segments[i].end;
            c[i].start_intensity = asked->segments[i].start_intensity, c[i].end_intensity = asked->segments[i].end_intensity;
        }
        qsort(q, quick->number_of_segments, sizeof(*q), segment_cmp);
        qsort(c, quick->number_of_segments, sizeof(*c), segment_cmp);
        CHECK(memcmp(q, c, quick->number_of_segments * sizeof(*q)) == 0);
        free(q);
        free(c);
    }
    dgx_morph_destroy(&quick);
    dgx_morph_destroy(&asked);
}

static int same_as_within(const dgx_morph_ctx_t *ctx, int x, int y, int pass, dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    return dgx_morph_sources_cells_within(ctx, x, y, pass, out, user_data);
}

static void check_same_plan(const dgx_bit_matrix_t *a, const dgx_bit_matrix_t *b)
{
    check_same_plans(dgx_morph_create(a, b, dgx_morph_sources_cells, NULL), dgx_morph_create(a, b, same_as_cells, NULL));
    /* and kept within a radius: no dot flies further, the planner and the callback agree again */
    static const int radii[] = {1, 2, 3, 5, 9, 40};
    for (unsigned k = 0; k < sizeof(radii) / sizeof(radii[0]); ++k) {
        int          radius = radii[k];
        dgx_morph_t *m = dgx_morph_create(a, b, dgx_morph_sources_cells_within, &radius);
        CHECK(m != NULL);
        for (size_t i = 0; m && i < m->number_of_segments; ++i) {
            const dgx_morph_segment_t *g = &m->segments[i];
            if (!g->start_intensity || !g->end_intensity) continue; /* from the center, or into it */
            CHECK(abs(g->end.x - g->start.x) <= radius && abs(g->end.y - g->start.y) <= radius);
        }
        check_same_plans(m, dgx_morph_create(a, b, same_as_within, &radius));
    }
    /* a radius of the whole grid, none, or one that is not a radius: the search without a limit */
    int all = 1000, none = 0, minus = -5;
    check_same_plans(dgx_morph_create(a, b, dgx_morph_sources_cells_within, &all), dgx_morph_create(a, b, dgx_morph_sources_cells, NULL));
    check_same_plans(dgx_morph_create(a, b, dgx_morph_sources_cells_within, &none), dgx_morph_create(a, b, dgx_morph_sources_cells, NULL));
    check_same_plans(dgx_morph_create(a, b, dgx_morph_sources_cells_within, &minus), dgx_morph_create(a, b, same_as_cells, NULL));
    check_same_plans(dgx_morph_create(a, b, dgx_morph_sources_cells_within, NULL), dgx_morph_create(a, b, same_as_within, NULL));
}

static void test_cells_by_vector(void)
{
    /* a line moved aside by a vector is found by that vector as a whole: every cell flies the same way */
    static const struct {
        int step_x, step_y; /* how the line goes */
        int move_x, move_y; /* how it is moved */
    } lines[] = {{1, 0, 0, 2}, {1, 0, 0, -3}, {0, 1, 3, 0}, {0, 1, -2, 0}, {1, 1, 4, -4}, {1, -1, 2, 2}};
    for (unsigned k = 0; k < sizeof(lines) / sizeof(lines[0]); ++k) {
        dgx_bit_matrix_t *from = dgx_matrix_init(28, 28), *to = dgx_matrix_init(28, 28);
        for (int i = 0; i < 9; ++i) {
            int x = 10 + i * lines[k].step_x, y = 14 + i * lines[k].step_y;
            dgx_matrix_set_point(from, x, y, true);
            dgx_matrix_set_point(to, x + lines[k].move_x, y + lines[k].move_y, true);
        }
        dgx_morph_t *m = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
        CHECK(m && m->number_of_segments == 9 && m->number_of_static_points == 0 && m->number_of_fading_points == 0);
        for (size_t i = 0; m && i < m->number_of_segments; ++i) {
            const dgx_morph_segment_t *g = &m->segments[i];
            CHECK(g->start_intensity == 255);
            CHECK(g->end.x - g->start.x == lines[k].move_x && g->end.y - g->start.y == lines[k].move_y);
        }
        check_same_plan(from, to);
        dgx_morph_destroy(&m);
        dgx_matrix_destroy(&from);
        dgx_matrix_destroy(&to);
    }

    /* random grids, wider than a word of bits too: the quick search and the callback agree */
    unsigned seed = 7;
    for (int round = 0; round < 60; ++round) {
        int               w = round % 3 == 0 ? 37 : 9 + round % 7, h = 5 + round % 6;
        dgx_bit_matrix_t *a = dgx_matrix_init((uint16_t)w, (uint16_t)h), *b = dgx_matrix_init((uint16_t)w, (uint16_t)h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                seed = seed * 1103515245u + 12345u;
                if ((seed >> 16) % 5 == 0) dgx_matrix_set_point(a, x, y, true);
                if ((seed >> 20) % (2 + round % 5) == 0) dgx_matrix_set_point(b, x, y, true);
            }
        }
        check_same_plan(a, b);
        check_same_plan(b, a);
        dgx_matrix_destroy(&a);
        dgx_matrix_destroy(&b);
    }
    /* and on glyphs */
    dgx_font_t *font = TerminusTTFMedium12();
    static const uint32_t pairs[][2] = {{'1', '8'}, {'A', 'W'}, {'.', 'M'}, {'i', ' '}, {' ', '%'}, {0x416, 0x449}};
    for (unsigned k = 0; k < sizeof(pairs) / sizeof(pairs[0]); ++k) {
        dgx_bit_matrix_t *a = dgx_morph_glyph_matrix(font, pairs[k][0]), *b = dgx_morph_glyph_matrix(font, pairs[k][1]);
        check_same_plan(a, b);
        dgx_matrix_destroy(&a);
        dgx_matrix_destroy(&b);
    }
}

static int ring_probe_radius, ring_probe_found;
static dgx_point_2d_t ring_probe_cell;

static int ring_probe(const dgx_morph_ctx_t *ctx, int x, int y, int pass, dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    (void)pass, (void)user_data;
    dgx_point_2d_t cell[DGX_MORPH_MAX_SOURCES];
    ring_probe_found = dgx_morph_ring_at(ctx, x, y, ring_probe_radius, cell);
    ring_probe_cell = cell[0];
    (void)out;
    return 0;
}

/* the ring of exactly one radius: nothing nearer or farther is taken, the cells nearest to the axes come first */
/* the rings of cells that are not on the grid: asked about from a callback of one's own */
static int off_grid_found;

static int off_grid_probe(const dgx_morph_ctx_t *ctx, int x, int y, int pass, dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    (void)x, (void)y, (void)pass, (void)user_data;
    static const int far[] = {-2000000000, -70000, -40, -3, 0, 5, 10, 11, 14, 60, 70000, 2000000000};
    static const int radii[] = {1, 2, 3, 9, 12, 50, 100000, 2000000000};
    dgx_point_2d_t   cell[DGX_MORPH_MAX_SOURCES];
    for (unsigned i = 0; i < sizeof(far) / sizeof(far[0]); ++i) {
        for (unsigned j = 0; j < sizeof(far) / sizeof(far[0]); ++j) {
            for (unsigned k = 0; k < sizeof(radii) / sizeof(radii[0]); ++k) {
                if (dgx_morph_ring_at(ctx, far[i], far[j], radii[k], cell)) {
                    CHECK(cell[0].x == 6 && cell[0].y == 5);
                    ++off_grid_found;
                }
            }
            /* the source is found from anywhere but its own cell */
            if (far[i] > -100 && far[i] < 100 && far[j] > -100 && far[j] < 100) {
                CHECK(dgx_morph_ring_find(ctx, far[i], far[j], 1, cell) == !(far[i] == 6 && far[j] == 5));
            }
        }
    }
    /* the only source is at (6, 5): three cells left of the grid on its row it is on the ring of radius 9 */
    CHECK(dgx_morph_ring_at(ctx, -3, 5, 9, out) == 1 && out[0].x == 6 && out[0].y == 5);
    CHECK(dgx_morph_ring_at(ctx, -3, 5, 8, out) == 0);
    CHECK(dgx_morph_ring_at(ctx, 6, 14, 9, out) == 1 && dgx_morph_ring_at(ctx, 6, -40, 45, out) == 1);
    CHECK(dgx_morph_ring_find(ctx, 14, 14, 1, out) == 1 && out[0].x == 6 && out[0].y == 5);
    return 0;
}

static void test_ring_off_grid(void)
{
    dgx_bit_matrix_t *from = dgx_matrix_init(11, 11), *to = dgx_matrix_init(11, 11);
    dgx_matrix_set_point(to, 5, 5, true);
    dgx_matrix_set_point(from, 6, 5, true);
    dgx_morph_t *m = dgx_morph_create(from, to, off_grid_probe, NULL);
    CHECK(m != NULL && off_grid_found > 0);
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
}

static void test_ring_at(void)
{
    dgx_bit_matrix_t *from = dgx_matrix_init(11, 11), *to = dgx_matrix_init(11, 11);
    dgx_matrix_set_point(to, 5, 5, true);
    dgx_matrix_set_point(from, 6, 5, true); /* radius 1 */
    dgx_matrix_set_point(from, 8, 8, true); /* radius 3, a corner */
    dgx_matrix_set_point(from, 4, 2, true); /* radius 3, next to the axis */
    dgx_matrix_set_point(from, 5, 10, true); /* radius 5 */
    static const int expect[6][3] = {{0, 0, 0}, {1, 6, 5}, {0, 0, 0}, {1, 4, 2}, {0, 0, 0}, {1, 5, 10}};
    for (ring_probe_radius = 0; ring_probe_radius <= 5; ++ring_probe_radius) {
        dgx_morph_t *m = dgx_morph_create(from, to, ring_probe, NULL);
        CHECK(ring_probe_found == expect[ring_probe_radius][0]);
        if (ring_probe_found) CHECK(ring_probe_cell.x == expect[ring_probe_radius][1] && ring_probe_cell.y == expect[ring_probe_radius][2]);
        dgx_morph_destroy(&m);
    }
    ring_probe_radius = 40;
    dgx_morph_t *m = dgx_morph_create(from, to, ring_probe, NULL);
    CHECK(ring_probe_found == 0);
    dgx_morph_destroy(&m);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
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
    test_cells_by_radius();
    test_ring_at();
    test_ring_off_grid();
    test_scan_vector();
    test_cells_by_vector();
    test_edge_cases();
    test_trail_end();
    test_glyphs();
    test_int16_guard();
    test_timing();
    test_text();
    CHECK_DONE();
}
