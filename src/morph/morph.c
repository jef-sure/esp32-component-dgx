#include <stdlib.h>
#include <string.h>

#include "dgx_morph.h"

/* a callback may put a cell off this many times at least; the planner allows as many as the grid is across */
#define DGX_MORPH_MIN_PASSES 8
#define DGX_MORPH_FAR (1 << 20)

struct dgx_morph_ctx {
    const dgx_bit_matrix_t *from;
    int width;
    int height;
    uint8_t *used;
    size_t free_sources; /* cells set in `from` that feed nothing yet */
    /* those cells as bits, by rows and by columns: a row, or a side of a ring, is looked at a word at a time */
    uint32_t *rows, *columns;
    int row_words, column_words;
};

bool dgx_morph_was_set(const dgx_morph_ctx_t *ctx, int x, int y)
{
    return ctx && dgx_matrix_get_point(ctx->from, x, y);
}

bool dgx_morph_is_used(const dgx_morph_ctx_t *ctx, int x, int y)
{
    if (!ctx || x < 0 || x >= ctx->width || y < 0 || y >= ctx->height) return false;
    return ctx->used[y * ctx->width + x] != 0;
}

int dgx_morph_ctx_width(const dgx_morph_ctx_t *ctx)
{
    return ctx ? ctx->width : 0;
}

int dgx_morph_ctx_height(const dgx_morph_ctx_t *ctx)
{
    return ctx ? ctx->height : 0;
}

static int dgx_morph_max_int(int a, int b)
{
    return a > b ? a : b;
}

static inline bool dgx_morph_is_free_source(const dgx_morph_ctx_t *ctx, int x, int y)
{
    if (x < 0 || x >= ctx->width || y < 0 || y >= ctx->height) return false;
    return (ctx->rows[(size_t)y * ctx->row_words + (x >> 5)] >> (x & 31)) & 1u;
}

/* any bit set from lo to hi, both within the line of bits */
static bool dgx_morph_bits_any(const uint32_t *bits, int lo, int hi)
{
    if (lo > hi) return false;
    int      first = lo >> 5, last = hi >> 5;
    uint32_t head = 0xffffffffu << (lo & 31), tail = 0xffffffffu >> (31 - (hi & 31));
    if (first == last) return (bits[first] & head & tail) != 0;
    if (bits[first] & head) return true;
    for (int i = first + 1; i < last; ++i) {
        if (bits[i]) return true;
    }
    return (bits[last] & tail) != 0;
}

/* whether the square ring of a radius around a cell has an unused cell at all: its four sides, each in one look */
static bool dgx_morph_ring_has(const dgx_morph_ctx_t *ctx, int x, int y, int r)
{
    /* the cell may be off the grid: each side is taken only if it is on it; callers keep the numbers within DGX_MORPH_FAR */
    int w = ctx->width, h = ctx->height;
    int left = x - r < 0 ? 0 : x - r, right = x + r >= w ? w - 1 : x + r;
    int top = y - r + 1 < 0 ? 0 : y - r + 1, bottom = y + r - 1 >= h ? h - 1 : y + r - 1;
    if (y - r >= 0 && y - r < h && dgx_morph_bits_any(ctx->rows + (size_t)(y - r) * ctx->row_words, left, right)) return true;
    if (y + r >= 0 && y + r < h && dgx_morph_bits_any(ctx->rows + (size_t)(y + r) * ctx->row_words, left, right)) return true;
    if (x - r >= 0 && x - r < w && dgx_morph_bits_any(ctx->columns + (size_t)(x - r) * ctx->column_words, top, bottom)) return true;
    if (x + r >= 0 && x + r < w && dgx_morph_bits_any(ctx->columns + (size_t)(x + r) * ctx->column_words, top, bottom)) return true;
    return false;
}

int dgx_morph_sources_life(
    const dgx_morph_ctx_t *ctx, int x, int y, int pass,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    static const int8_t nx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int8_t ny[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    (void)pass;
    (void)user_data;
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        if (dgx_morph_was_set(ctx, x + nx[i], y + ny[i])) {
            out[n++] = (dgx_point_2d_t){ .x = (int16_t)(x + nx[i]), .y = (int16_t)(y + ny[i]) };
        }
    }
    return n;
}

/* axes (up, left, right, down) first, then diagonals */
static const int8_t dgx_morph_axis_nx[8] = {0, -1, 1, 0, -1, 1, 1, -1};
static const int8_t dgx_morph_axis_ny[8] = {-1, 0, 0, 1, -1, -1, 1, 1};

int dgx_morph_ring_at(
    const dgx_morph_ctx_t *ctx, int x, int y, int radius,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES])
{
    if (!ctx || radius < 1) return 0;
    /* a cell off the grid may be asked about; one too far for any grid is not looked at */
    if (x < -DGX_MORPH_FAR || x > DGX_MORPH_FAR || y < -DGX_MORPH_FAR || y > DGX_MORPH_FAR || radius > 2 * DGX_MORPH_FAR) return 0;
    int r = radius;
    /* most rings are empty, and that is seen without going around them */
    if (!dgx_morph_ring_has(ctx, x, y, r)) return 0;
    /* d walks along the square side from the axis point to the corner: the nearest cells of the ring first */
    for (int d = 0; d <= r; ++d) {
        for (int k = 0; k < 4; ++k) {
            /* corners are covered by the vertical axes */
            if (d == r && dgx_morph_axis_nx[k] != 0) continue;
            int px = -dgx_morph_axis_ny[k];
            int py = dgx_morph_axis_nx[k];
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                if (d == 0 && sgn == 1) continue;
                int cx = x + dgx_morph_axis_nx[k] * r + sgn * d * px;
                int cy = y + dgx_morph_axis_ny[k] * r + sgn * d * py;
                if (dgx_morph_is_free_source(ctx, cx, cy)) {
                    out[0] = (dgx_point_2d_t){ .x = (int16_t)cx, .y = (int16_t)cy };
                    return 1;
                }
            }
        }
    }
    return 0;
}

int dgx_morph_ring_find(
    const dgx_morph_ctx_t *ctx, int x, int y, int min_radius,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES])
{
    if (!ctx || !ctx->free_sources) return 0;
    if (x < -DGX_MORPH_FAR || x > DGX_MORPH_FAR || y < -DGX_MORPH_FAR || y > DGX_MORPH_FAR) return 0;
    int max_radius = ctx->width > ctx->height ? ctx->width : ctx->height;
    /* from a cell off the grid the rings are as much further as the cell is */
    int off = dgx_morph_max_int(dgx_morph_max_int(-x, x - ctx->width + 1), dgx_morph_max_int(-y, y - ctx->height + 1));
    if (off > 0) max_radius += off;
    for (int r = min_radius < 1 ? 1 : min_radius; r <= max_radius; ++r) {
        if (dgx_morph_ring_at(ctx, x, y, r, out)) return 1;
    }
    return 0;
}

/* a vector of a ring by its number on the ring, 0 .. 8r - 1 */
static void dgx_morph_ring_vector(int r, int pass, int *dx, int *dy)
{
    int x, y;
    if (pass < 4) {
        /* the axes: up, down, right, left */
        x = pass == 2 ? r : pass == 3 ? -r : 0;
        y = pass == 0 ? -r : pass == 1 ? r : 0;
    } else {
        /* from every axis toward the corners, a cell further each round; the corners belong to the top and the bottom */
        int d = (pass - 4) / 8 + 1;
        switch ((pass - 4) % 8) {
        case 0: x = d, y = -r; break;  /* from the top to the right */
        case 1: x = -d, y = -r; break; /* from the top to the left */
        case 2: x = d, y = r; break;   /* from the bottom to the right */
        case 3: x = -d, y = r; break;  /* from the bottom to the left */
        case 4: x = r, y = -d; break;  /* from the right up */
        case 5: x = r, y = d; break;   /* from the right down */
        case 6: x = -r, y = -d; break; /* from the left up */
        default: x = -r, y = d; break; /* from the left down */
        }
    }
    *dx = x;
    *dy = y;
}

bool dgx_morph_scan_vector(int pass, int *dx, int *dy)
{
    if (pass < 0) return false;
    int r = 1, x, y;
    for (; pass >= 8 * r; ++r) pass -= 8 * r;
    dgx_morph_ring_vector(r, pass, &x, &y);
    if (dx) *dx = x;
    if (dy) *dy = y;
    return true;
}

/* how many vectors the rings up to a radius have */
static int dgx_morph_scan_vectors(int radius)
{
    int64_t vectors = 4 * (int64_t)radius * (radius + 1);
    return vectors > INT32_MAX ? INT32_MAX : (int)vectors;
}

/*
 * The search goes not cell by cell but vector by vector. A pass is one
 * vector, the same for the whole grid: every new cell still without a source
 * looks at the one cell that lies by that vector from it. The vectors go ring
 * by ring, on a ring from the axes toward the corners, see
 * dgx_morph_scan_vector(). So a line that has moved by a vector is found by
 * that vector as a whole, each of its cells from its own cell of the old
 * line; and since two cells never look at the same cell on a pass, the order
 * in which the cells are asked decides nothing.
 */
int dgx_morph_sources_cells(
    const dgx_morph_ctx_t *ctx, int x, int y, int pass,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    (void)user_data;
    /* with nothing left anywhere there is no vector to wait for */
    if (!ctx || !ctx->free_sources) return 0;
    int dx, dy;
    if (!dgx_morph_scan_vector(pass, &dx, &dy)) return 0;
    if (dgx_morph_is_free_source(ctx, x + dx, y + dy)) {
        out[0] = (dgx_point_2d_t){ .x = (int16_t)(x + dx), .y = (int16_t)(y + dy) };
        return 1;
    }
    /* beyond the rings that reach across the grid there is nothing to find */
    int reach = dgx_morph_max_int(ctx->width, ctx->height) - 1;
    return pass + 1 < dgx_morph_scan_vectors(reach) ? DGX_MORPH_DEFER : 0;
}

/* the radius a search is kept within, 0 for none */
static int dgx_morph_within(const void *user_data)
{
    int radius = user_data ? *(const int *)user_data : 0;
    return radius < 1 ? 0 : radius;
}

int dgx_morph_sources_cells_within(
    const dgx_morph_ctx_t *ctx, int x, int y, int pass,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    int radius = dgx_morph_within(user_data);
    /* the rings of the radius are passed: nothing nearer was found */
    if (radius && pass >= dgx_morph_scan_vectors(radius)) return 0;
    return dgx_morph_sources_cells(ctx, x, y, pass, out, NULL);
}

/* 32 bits of a line of bits from a position on, which may lie before its start or go past its end */
static inline uint32_t dgx_morph_bits_window(const uint32_t *bits, int words, int start)
{
    int      word = start >> 5, shift = start & 31;
    uint32_t low = word >= 0 && word < words ? bits[word] : 0;
    if (!shift) return low;
    uint32_t high = word + 1 >= 0 && word + 1 < words ? bits[word + 1] : 0;
    return (low >> shift) | (high << (32 - shift));
}

void dgx_morph_destroy(dgx_morph_t **morph)
{
    if (morph && *morph) {
        free((*morph)->segments);
        free((*morph)->static_points);
        free((*morph)->fading_points);
        free((*morph)->merging);
        free(*morph);
        *morph = NULL;
    }
}

static bool dgx_morph_push_segment(dgx_morph_t *m, size_t *capacity, dgx_morph_segment_t seg)
{
    if (m->number_of_segments == *capacity) {
        size_t cap = *capacity ? *capacity * 2 : 16;
        dgx_morph_segment_t *p = realloc(m->segments, cap * sizeof(*p));
        if (!p) return false;
        m->segments = p;
        *capacity = cap;
    }
    m->segments[m->number_of_segments++] = seg;
    return true;
}

/* the cells of a matrix as bits by rows; a row of the matrix starts anywhere in its bytes */
static void dgx_morph_rows_of(const dgx_bit_matrix_t *m, int row_words, uint32_t *rows)
{
    if (!m || !m->bits) return;
    size_t bytes = ((size_t)m->width * m->height + 7) / 8;
    for (int y = 0; y < m->height; ++y) {
        uint32_t *row = rows + (size_t)y * row_words;
        for (int x = 0; x < m->width; x += 32) {
            size_t   bit = (size_t)y * m->width + x, byte = bit >> 3;
            uint64_t five = 0;
            for (size_t k = 0; k < 5 && byte + k < bytes; ++k) five |= (uint64_t)m->bits[byte + k] << (8 * k);
            uint32_t word = (uint32_t)(five >> (bit & 7));
            if (m->width - x < 32) word &= (1u << (m->width - x)) - 1;
            row[x >> 5] = word;
        }
    }
}

/* the box of the set bits, x0 y0 x1 y1; `in_row` counts them by rows, and there is at least one */
static void dgx_morph_bits_box(const uint32_t *rows, const uint16_t *in_row, int row_words, int height, int box[4])
{
    box[0] = box[1] = INT16_MAX;
    box[2] = box[3] = -1;
    for (int y = 0; y < height; ++y) {
        if (!in_row[y]) continue;
        if (box[1] > y) box[1] = y;
        box[3] = y;
        const uint32_t *row = rows + (size_t)y * row_words;
        for (int word = 0; word < row_words; ++word) {
            if (!row[word]) continue;
            int first = word * 32 + __builtin_ctz(row[word]), last = word * 32 + 31 - __builtin_clz(row[word]);
            if (box[0] > first) box[0] = first;
            if (box[2] < last) box[2] = last;
        }
    }
}

/* a cell begins to feed a new one */
static void dgx_morph_take(struct dgx_morph_ctx *ctx, int x, int y)
{
    ctx->used[y * ctx->width + x] = 1;
    uint32_t *word = &ctx->rows[(size_t)y * ctx->row_words + (x >> 5)];
    if (!(*word & (1u << (x & 31)))) return;
    *word &= ~(1u << (x & 31));
    ctx->columns[(size_t)x * ctx->column_words + (y >> 5)] &= ~(1u << (y & 31));
    --ctx->free_sources;
}

/* brightness of a merging orphan is full to here and goes to zero by the end */
#define DGX_MORPH_MERGE_FADE_FROM 0.5f

/* whether a cell is set in rows of bits of the grid */
static inline bool dgx_morph_bit_at(const uint32_t *rows, int row_words, int w, int h, int x, int y)
{
    if (x < 0 || x >= w || y < 0 || y >= h) return false;
    return (rows[(size_t)y * row_words + (x >> 5)] >> (x & 31)) & 1u;
}

/*
 * The orphans, old cells nobody took that are not in `to`, fly into the
 * nearest cell of `to` instead of fading where they are. A cell of `to` is
 * not used up: any number of orphans may flow into it. Each orphan gets its
 * cell by the first of three steps that gives it one.
 *
 * A. Along the figure. An orphan with a cell of `to` among its eight
 *    neighbours takes it, the first by dgx_morph_axis_nx/ny. The rest take
 *    the cell of a neighbouring orphan, wave after wave over the orphans
 *    joined by sides and corners, so that the bottom bar of an "E" flows whole
 *    into the cell of the stem it touches, and not its right part up into
 *    the middle bar, which is nearer by the rings. An orphan reached by
 *    several at once takes the first by the same order of neighbours.
 * B. The nearest cell of `to` by the rings, vector after vector of
 *    dgx_morph_scan_vector(): for a piece that is cut off, or one further
 *    along the figure than the radius. Nothing is taken from anybody, so each
 *    orphan is served on its own, in any order.
 * C. What is left, further than the radius both ways, stays in `ctx->rows`
 *    and fades where it is, as it does without the option.
 *
 * The radius bounds the way of a flight in cells, along the figure as well as
 * across; 0 is no bound, and then nothing is left for C while `to` has a cell.
 */
static bool dgx_morph_merge_orphans(struct dgx_morph_ctx *ctx, const uint32_t *target, dgx_morph_t *m, int radius)
{
    int    w = ctx->width, h = ctx->height, rw = ctx->row_words;
    size_t cells = (size_t)w * h, n_orphans = 0, n_goals = 0;
    for (size_t i = 0; i < (size_t)h * rw; ++i) {
        n_orphans += (size_t)__builtin_popcount(ctx->rows[i] & ~target[i]);
        n_goals += (size_t)__builtin_popcount(target[i]);
    }
    if (!n_orphans || !n_goals) return true;

    /* the cells of `to` as bits by rows and by columns: the same looks at rings as at the sources */
    struct dgx_morph_ctx goals = *ctx;
    goals.rows = (uint32_t *)target;
    goals.columns = calloc((size_t)w * ctx->column_words, sizeof(uint32_t));
    int32_t *goal = malloc(cells * sizeof(*goal)); /* the cell an orphan flows into, -1 for none yet */
    uint8_t *wave = calloc(cells, 1);               /* 1 on the wave at hand, 2 on the next, 3 served by an earlier one */
    int32_t *queue = malloc(n_orphans * sizeof(*queue));
    m->merging = calloc(n_orphans, sizeof(*m->merging));
    if (!goals.columns || !goal || !wave || !queue || !m->merging) goto fail;
    memset(goal, 0xff, cells * sizeof(*goal));
    for (int y = 0; y < h; ++y) {
        for (int word = 0; word < rw; ++word) {
            for (uint32_t left = target[(size_t)y * rw + word]; left; left &= left - 1) {
                int x = word * 32 + __builtin_ctz(left);
                goals.columns[(size_t)x * ctx->column_words + (y >> 5)] |= 1u << (y & 31);
            }
        }
    }

    /* A: an orphan next to a cell of `to` takes it, the first in the order of the neighbours */
    size_t head = 0, tail = 0;
    for (int y = 0; y < h; ++y) {
        for (int word = 0; word < rw; ++word) {
            for (uint32_t left = ctx->rows[(size_t)y * rw + word] & ~target[(size_t)y * rw + word]; left; left &= left - 1) {
                int x = word * 32 + __builtin_ctz(left);
                for (int k = 0; k < 8; ++k) {
                    int nx = x + dgx_morph_axis_nx[k], ny = y + dgx_morph_axis_ny[k];
                    if (!dgx_morph_bit_at(target, rw, w, h, nx, ny)) continue;
                    goal[(size_t)y * w + x] = ny * w + nx;
                    wave[(size_t)y * w + x] = 1;
                    queue[tail++] = y * w + x;
                    break;
                }
            }
        }
    }
    /* the rest take the cell of a neighbouring orphan, wave after wave; a wave is a cell further along the figure */
    for (int way = 1; head < tail && (!radius || way + 1 <= radius); ++way) {
        size_t next = tail;
        for (size_t at = head; at < next; ++at) {
            int x = queue[at] % w, y = queue[at] / w;
            for (int k = 0; k < 8; ++k) {
                int nx = x + dgx_morph_axis_nx[k], ny = y + dgx_morph_axis_ny[k];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h || wave[(size_t)ny * w + nx]) continue;
                if (!dgx_morph_bit_at(ctx->rows, rw, w, h, nx, ny) || dgx_morph_bit_at(target, rw, w, h, nx, ny)) continue;
                wave[(size_t)ny * w + nx] = 2;
                queue[tail++] = ny * w + nx;
            }
        }
        /* a cell of the next wave takes the goal of its first neighbour on this one */
        for (size_t at = next; at < tail; ++at) {
            int x = queue[at] % w, y = queue[at] / w;
            for (int k = 0; k < 8; ++k) {
                int nx = x + dgx_morph_axis_nx[k], ny = y + dgx_morph_axis_ny[k];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h || wave[(size_t)ny * w + nx] != 1) continue;
                goal[queue[at]] = goal[(size_t)ny * w + nx];
                break;
            }
        }
        for (size_t at = head; at < tail; ++at) wave[queue[at]] = at < next ? 3 : 1;
        head = next;
    }

    /* B: an orphan the waves did not reach looks for the nearest cell of `to` by the rings */
    int reach = dgx_morph_max_int(dgx_morph_max_int(w, h) - 1, 1);
    if (radius && radius < reach) reach = radius;
    for (int y = 0; y < h; ++y) {
        for (int word = 0; word < rw; ++word) {
            for (uint32_t left = ctx->rows[(size_t)y * rw + word] & ~target[(size_t)y * rw + word]; left; left &= left - 1) {
                int x = word * 32 + __builtin_ctz(left);
                if (goal[(size_t)y * w + x] >= 0) continue;
                for (int r = 1; r <= reach && goal[(size_t)y * w + x] < 0; ++r) {
                    if (!dgx_morph_ring_has(&goals, x, y, r)) continue;
                    for (int pass = 0; pass < 8 * r; ++pass) {
                        int dx, dy;
                        dgx_morph_ring_vector(r, pass, &dx, &dy);
                        if (!dgx_morph_bit_at(target, rw, w, h, x + dx, y + dy)) continue;
                        goal[(size_t)y * w + x] = (y + dy) * w + x + dx;
                        break;
                    }
                }
            }
        }
    }

    /* the flights, in scan order; an orphan that flies is no longer an old cell to fade */
    for (int y = 0; y < h; ++y) {
        for (int word = 0; word < rw; ++word) {
            for (uint32_t left = ctx->rows[(size_t)y * rw + word] & ~target[(size_t)y * rw + word]; left; left &= left - 1) {
                int     x = word * 32 + __builtin_ctz(left);
                int32_t g = goal[(size_t)y * w + x];
                if (g < 0) continue;
                m->merging[m->number_of_merging++] =
                    (dgx_morph_segment_t){ { .x = (int16_t)x, .y = (int16_t)y }, { .x = (int16_t)(g % w), .y = (int16_t)(g / w) }, 255, 0 };
                dgx_morph_take(ctx, x, y);
            }
        }
    }
    free(goals.columns);
    free(goal);
    free(wave);
    free(queue);
    return true;

fail:
    free(goals.columns);
    free(goal);
    free(wave);
    free(queue);
    return false;
}

dgx_morph_t *dgx_morph_create(
    const dgx_bit_matrix_t  *from,
    const dgx_bit_matrix_t  *to,
    dgx_morph_sources_func_t sources,
    void                    *user_data)
{
    return dgx_morph_create_with(from, to, sources, user_data, NULL);
}

dgx_morph_t *dgx_morph_create_with(
    const dgx_bit_matrix_t    *from,
    const dgx_bit_matrix_t    *to,
    dgx_morph_sources_func_t   sources,
    void                      *user_data,
    const dgx_morph_options_t *options)
{
    dgx_morph_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->width = dgx_morph_max_int(from ? from->width : 0, to ? to->width : 0);
    m->height = dgx_morph_max_int(from ? from->height : 0, to ? to->height : 0);
    int w = m->width, h = m->height;
    if (w > INT16_MAX || h > INT16_MAX) {
        free(m);
        return NULL;
    }
    if (w == 0 || h == 0) return m;

    /* both matrices are read once, into rows of bits; everything after works on words of them */
    struct dgx_morph_ctx ctx = {
        .from = from,
        .width = w,
        .height = h,
        .used = calloc((size_t)w * h, 1),
        .row_words = (w + 31) / 32,
        .column_words = (h + 31) / 32,
    };
    int rw = ctx.row_words;
    ctx.rows = calloc((size_t)h * rw, sizeof(uint32_t));
    ctx.columns = calloc((size_t)w * ctx.column_words, sizeof(uint32_t));
    uint32_t *target = calloc((size_t)h * rw, sizeof(uint32_t));  /* `to` */
    uint32_t *waiting = calloc((size_t)h * rw, sizeof(uint32_t)); /* the new cells still without a source */
    uint16_t *in_row = calloc((size_t)h * 2, sizeof(*in_row));    /* how many of them a row has */
    uint16_t *free_in_row = in_row ? in_row + h : NULL;           /* and how many unused sources */
    uint16_t *look = malloc((size_t)h * sizeof(*look));           /* rows worth going through on the ring at hand */
    int32_t *deferred = NULL;                                     /* the same cells as a list, for a callback */
    size_t seg_capacity = 1;
    if (!ctx.used || !ctx.rows || !ctx.columns || !target || !waiting || !in_row || !look) goto fail;
    dgx_morph_rows_of(from, rw, ctx.rows);
    dgx_morph_rows_of(to, rw, target);

    size_t n_static = 0, n_new = 0, n_old = 0;
    for (int y = 0; y < h; ++y) {
        for (int word = 0; word < rw; ++word) {
            uint32_t f = ctx.rows[(size_t)y * rw + word], t = target[(size_t)y * rw + word];
            n_static += (size_t)__builtin_popcount(f & t);
            n_old += (size_t)__builtin_popcount(f & ~t);
            waiting[(size_t)y * rw + word] = t & ~f;
            in_row[y] = (uint16_t)(in_row[y] + __builtin_popcount(t & ~f));
        }
        n_new += in_row[y];
    }
    ctx.free_sources = n_static + n_old;
    seg_capacity = n_new ? n_new : 1;
    m->segments = calloc(seg_capacity, sizeof(*m->segments));
    m->static_points = calloc(n_static ? n_static : 1, sizeof(*m->static_points));
    m->fading_points = calloc(n_old ? n_old : 1, sizeof(*m->fading_points));
    if (!m->segments || !m->static_points || !m->fading_points) goto fail;

    const dgx_point_2d_t center = { .x = (int16_t)(w / 2), .y = (int16_t)(h / 2) };
    bool to_nothing = !n_static && !n_new;
    for (int y = 0; y < h; ++y) {
        for (int word = 0; word < rw; ++word) {
            uint32_t t = target[(size_t)y * rw + word];
            for (uint32_t f = ctx.rows[(size_t)y * rw + word]; f; f &= f - 1) {
                int bit = __builtin_ctz(f), x = word * 32 + bit;
                dgx_point_2d_t cell = { .x = (int16_t)x, .y = (int16_t)y };
                ctx.columns[(size_t)x * ctx.column_words + (y >> 5)] |= 1u << (y & 31);
                if (t & (1u << bit)) m->static_points[m->number_of_static_points++] = cell;
                if (to_nothing) {
                    /* nothing to become: every dot flies into the center and fades there */
                    dgx_morph_segment_t seg = { cell, center, 255, 0 };
                    if (!dgx_morph_push_segment(m, &seg_capacity, seg)) goto fail;
                }
            }
        }
    }
    if (to_nothing) goto done;

    size_t pending = n_new;
    int reach = dgx_morph_max_int(dgx_morph_max_int(w, h) - 1, 1);
    bool by_vectors = sources == dgx_morph_sources_cells || sources == dgx_morph_sources_cells_within;
    if (by_vectors && sources == dgx_morph_sources_cells_within && dgx_morph_within(user_data)) {
        /* further than the radius nothing is looked for */
        reach = dgx_morph_max_int(1, dgx_morph_within(user_data) < reach ? dgx_morph_within(user_data) : reach);
    }
    if (by_vectors) {
        /*
         * The same search as the callback does, pass by pass, done on rows of
         * bits: the cells still waiting in a row, and the unused sources of the
         * row a vector points to moved back by it, meet in one AND of words.
         */
        for (int y = 0; y < h; ++y) {
            for (int word = 0; word < rw; ++word) {
                free_in_row[y] = (uint16_t)(free_in_row[y] + __builtin_popcount(ctx.rows[(size_t)y * rw + word]));
            }
        }
        for (int r = 1; r <= reach && pending && ctx.free_sources; ++r) {
            /*
             * A vector can find something only if it leads from the box of
             * the waiting cells into the box of the unused sources. The boxes
             * shrink as the cells are served, and a ring further than both of
             * them has nothing, nor has any ring after it.
             */
            int cells_box[4], free_box[4];
            dgx_morph_bits_box(waiting, in_row, rw, h, cells_box);
            dgx_morph_bits_box(ctx.rows, free_in_row, rw, h, free_box);
            int dx_min = free_box[0] - cells_box[2], dx_max = free_box[2] - cells_box[0];
            int dy_min = free_box[1] - cells_box[3], dy_max = free_box[3] - cells_box[1];
            if (r > dgx_morph_max_int(dgx_morph_max_int(-dx_min, dx_max), dgx_morph_max_int(-dy_min, dy_max))) break;
            /*
             * Far rings are empty for most of the cells left. A row is gone
             * through on this ring only if one of its cells has an unused
             * source somewhere on its ring, which is four looks a cell; a
             * ring nobody has anything on is passed without its vectors.
             */
            int looks = 0;
            for (int y = 0; y < h; ++y) {
                if (!in_row[y]) continue;
                const uint32_t *cells = waiting + (size_t)y * rw;
                bool            has = false;
                for (int word = 0; word < rw && !has; ++word) {
                    for (uint32_t left = cells[word]; left && !has; left &= left - 1) {
                        has = dgx_morph_ring_has(&ctx, word * 32 + __builtin_ctz(left), y, r);
                    }
                }
                if (has) look[looks++] = (uint16_t)y;
            }
            if (!looks) continue;
            for (int pass = 0; pass < 8 * r && pending && ctx.free_sources; ++pass) {
                int dx, dy;
                dgx_morph_ring_vector(r, pass, &dx, &dy);
                if (dx < dx_min || dx > dx_max || dy < dy_min || dy > dy_max) continue;
                for (int at = 0; at < looks; ++at) {
                    int y = look[at];
                    if (!in_row[y] || y + dy < 0 || y + dy >= h || !free_in_row[y + dy]) continue;
                    uint32_t       *cells = waiting + (size_t)y * rw;
                    const uint32_t *row = ctx.rows + (size_t)(y + dy) * rw;
                    for (int word = 0; word < rw; ++word) {
                        if (!cells[word]) continue;
                        uint32_t found = cells[word] & dgx_morph_bits_window(row, rw, word * 32 + dx);
                        for (; found; found &= found - 1) {
                            int bit = __builtin_ctz(found), x = word * 32 + bit;
                            dgx_morph_segment_t seg = { { .x = (int16_t)(x + dx), .y = (int16_t)(y + dy) }, { .x = (int16_t)x, .y = (int16_t)y }, 255, 255 };
                            if (!dgx_morph_push_segment(m, &seg_capacity, seg)) goto fail;
                            cells[word] &= ~(1u << bit);
                            --in_row[y];
                            --pending;
                            --free_in_row[y + dy];
                            dgx_morph_take(&ctx, x + dx, y + dy);
                        }
                    }
                }
            }
        }
        /* what is left has no source anywhere: it appears from the center */
        for (int y = 0; y < h && pending; ++y) {
            for (int word = 0; word < rw && in_row[y]; ++word) {
                for (uint32_t left = waiting[(size_t)y * rw + word]; left; left &= left - 1) {
                    dgx_morph_segment_t seg = { center, { .x = (int16_t)(word * 32 + __builtin_ctz(left)), .y = (int16_t)y }, 0, 255 };
                    if (!dgx_morph_push_segment(m, &seg_capacity, seg)) goto fail;
                    --pending;
                }
            }
        }
    } else if (pending) {
        /* a callback is asked about every waiting cell in scan order, pass after pass */
        deferred = malloc(pending * sizeof(*deferred));
        if (!deferred) goto fail;
        size_t listed = 0;
        for (int y = 0; y < h; ++y) {
            for (int word = 0; word < rw && in_row[y]; ++word) {
                for (uint32_t left = waiting[(size_t)y * rw + word]; left; left &= left - 1) {
                    deferred[listed++] = y * w + word * 32 + __builtin_ctz(left);
                }
            }
        }
        int passes = dgx_morph_max_int(DGX_MORPH_MIN_PASSES, dgx_morph_scan_vectors(reach));
        for (int pass = 0; pass < passes && pending; ++pass) {
            size_t next_pending = 0;
            for (size_t at = 0; at < pending; ++at) {
                int idx = deferred[at];
                int x = idx % w, y = idx / w;
                dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES];
                int n = sources ? sources(&ctx, x, y, pass, out, user_data) : 0;
                if (n == DGX_MORPH_DEFER && pass + 1 < passes) {
                    deferred[next_pending++] = idx;
                    continue;
                }
                if (n < 0) n = 0;
                if (n > DGX_MORPH_MAX_SOURCES) n = DGX_MORPH_MAX_SOURCES;
                dgx_point_2d_t end = { .x = (int16_t)x, .y = (int16_t)y };
                if (n == 0) {
                    dgx_morph_segment_t seg = { center, end, 0, 255 };
                    if (!dgx_morph_push_segment(m, &seg_capacity, seg)) goto fail;
                    continue;
                }
                uint8_t share = (uint8_t)(255 / n);
                for (int i = 0; i < n; ++i) {
                    dgx_morph_segment_t seg = { out[i], end, share, share };
                    if (!dgx_morph_push_segment(m, &seg_capacity, seg)) goto fail;
                    if (out[i].x >= 0 && out[i].x < w && out[i].y >= 0 && out[i].y < h) dgx_morph_take(&ctx, out[i].x, out[i].y);
                }
            }
            pending = next_pending;
        }
    }
    /* an old cell that became nobody's source and is not in `to` flies into the nearest cell of `to` if asked to */
    if (options && options->orphans == DGX_MORPH_ORPHANS_MERGE) {
        if (!dgx_morph_merge_orphans(&ctx, target, m, options->merge_radius < 0 ? 0 : options->merge_radius)) goto fail;
    }
    /* and fades where it is otherwise */
    for (int y = 0; y < h; ++y) {
        for (int word = 0; word < rw; ++word) {
            for (uint32_t left = ctx.rows[(size_t)y * rw + word] & ~target[(size_t)y * rw + word]; left; left &= left - 1) {
                m->fading_points[m->number_of_fading_points++] =
                    (dgx_point_2d_t){ .x = (int16_t)(word * 32 + __builtin_ctz(left)), .y = (int16_t)y };
            }
        }
    }
done:
    free(ctx.used);
    free(ctx.rows);
    free(ctx.columns);
    free(target);
    free(waiting);
    free(in_row);
    free(look);
    free(deferred);
    return m;

fail:
    free(ctx.used);
    free(ctx.rows);
    free(ctx.columns);
    free(target);
    free(waiting);
    free(in_row);
    free(look);
    free(deferred);
    dgx_morph_destroy(&m);
    return NULL;
}

float dgx_morph_progress(int64_t start_us, int64_t now_us, int64_t duration_us)
{
    if (duration_us <= 0) return 1.0f;
    float t = (float)(now_us - start_us) / (float)duration_us;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

float dgx_morph_ease(dgx_morph_easing_t easing, float t)
{
    switch (easing) {
    case DGX_MORPH_EASE_IN_OUT:
        return t < 0.5f ? t * t * 2.0f : 1.0f - (1.0f - t) * (1.0f - t) * 2.0f;
    case DGX_MORPH_EASE_IN_OUT_3:
        return t < 0.5f ? t * t * t * 4.0f : 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t) * 4.0f;
    case DGX_MORPH_SMOOTHSTEP:
        return t * t * (3.0f - 2.0f * t);
    case DGX_MORPH_LINEAR:
    default:
        return t;
    }
}

static bool dgx_morph_pixel(dgx_point_2d_t a, dgx_point_2d_t b, float t, int x, int y, int cell, dgx_point_2d_t *p)
{
    int64_t px = (int64_t)x + cell / 2 + (int64_t)a.x * cell + (int64_t)((b.x - a.x) * (float)cell * t);
    int64_t py = (int64_t)y + cell / 2 + (int64_t)a.y * cell + (int64_t)((b.y - a.y) * (float)cell * t);
    if (px < INT16_MIN || px > INT16_MAX || py < INT16_MIN || py > INT16_MAX) return false;
    *p = (dgx_point_2d_t){ .x = (int16_t)px, .y = (int16_t)py };
    return true;
}

/* the head of a flight where it is at t, and its tail at t_tail, at half the brightness each */
static void dgx_morph_flight(const dgx_morph_segment_t *s, float t, float t_tail, uint8_t intensity, int x, int y, int cell, bool trail,
                             dgx_morph_dot_func_t dot, void *user_data)
{
    dgx_point_2d_t head, tail;
    bool           head_ok = dgx_morph_pixel(s->start, s->end, t, x, y, cell, &head);
    if (trail) {
        if (dgx_morph_pixel(s->start, s->end, t_tail, x, y, cell, &tail)) dot(user_data, &tail, (uint8_t)(intensity / 2));
        if (head_ok) dot(user_data, &head, (uint8_t)(intensity / 2));
    } else if (head_ok) {
        dot(user_data, &head, intensity);
    }
}

void dgx_morph_draw(
    const dgx_morph_t   *morph,
    float                t,
    int                  x,
    int                  y,
    int                  cell_width,
    bool                 trail,
    dgx_morph_dot_func_t dot,
    void                *user_data)
{
    if (!morph || !dot || cell_width <= 0) return;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float t_tail = t * 1.5f - 0.5f;
    if (t_tail < 0.0f) t_tail = 0.0f;

    for (size_t i = 0; i < morph->number_of_segments; ++i) {
        const dgx_morph_segment_t *s = &morph->segments[i];
        int v = s->start_intensity + (int)((s->end_intensity - s->start_intensity) * t);
        dgx_morph_flight(s, t, t_tail, (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)), x, y, cell_width, trail, dot, user_data);
    }
    /* an orphan on its way keeps its brightness to the middle of the way and goes out by the end */
    if (morph->number_of_merging) {
        float   s = (t - DGX_MORPH_MERGE_FADE_FROM) / (1.0f - DGX_MORPH_MERGE_FADE_FROM);
        uint8_t merge = s <= 0.0f ? 255 : (uint8_t)(255.0f * (1.0f - s * s * (3.0f - 2.0f * s)));
        for (size_t i = 0; i < morph->number_of_merging; ++i) {
            dgx_morph_flight(&morph->merging[i], t, t_tail, merge, x, y, cell_width, trail, dot, user_data);
        }
    }
    for (size_t i = 0; i < morph->number_of_static_points; ++i) {
        dgx_point_2d_t p;
        if (dgx_morph_pixel(morph->static_points[i], morph->static_points[i], 0.0f, x, y, cell_width, &p)) {
            dot(user_data, &p, 255);
        }
    }
    uint8_t fade = (uint8_t)(255.0f * (1.0f - t));
    for (size_t i = 0; i < morph->number_of_fading_points; ++i) {
        dgx_point_2d_t p;
        if (dgx_morph_pixel(morph->fading_points[i], morph->fading_points[i], 0.0f, x, y, cell_width, &p)) {
            dot(user_data, &p, fade);
        }
    }
}
