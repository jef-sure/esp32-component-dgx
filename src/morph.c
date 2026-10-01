#include <stdlib.h>
#include <string.h>

#include "dgx_morph.h"

#define DGX_MORPH_MAX_PASSES 8

struct dgx_morph_ctx {
    const dgx_bit_matrix_t *from;
    int width;
    int height;
    uint8_t *used;
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

static bool dgx_morph_is_free_source(const dgx_morph_ctx_t *ctx, int x, int y)
{
    return dgx_morph_was_set(ctx, x, y) && !dgx_morph_is_used(ctx, x, y);
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

int dgx_morph_ring_find(
    const dgx_morph_ctx_t *ctx, int x, int y, int min_radius,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES])
{
    if (!ctx) return 0;
    int max_radius = ctx->width > ctx->height ? ctx->width : ctx->height;
    for (int r = min_radius < 1 ? 1 : min_radius; r <= max_radius; ++r) {
        /* d walks along the square side from the axis point to the corner */
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
    }
    return 0;
}

int dgx_morph_sources_cells(
    const dgx_morph_ctx_t *ctx, int x, int y, int pass,
    dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES], void *user_data)
{
    (void)user_data;
    if (pass == 0) {
        for (int i = 0; i < 8; ++i) {
            int cx = x + dgx_morph_axis_nx[i];
            int cy = y + dgx_morph_axis_ny[i];
            if (dgx_morph_is_free_source(ctx, cx, cy)) {
                out[0] = (dgx_point_2d_t){ .x = (int16_t)cx, .y = (int16_t)cy };
                return 1;
            }
        }
        return DGX_MORPH_DEFER;
    }
    return dgx_morph_ring_find(ctx, x, y, 2, out);
}

void dgx_morph_destroy(dgx_morph_t **morph)
{
    if (morph && *morph) {
        free((*morph)->segments);
        free((*morph)->static_points);
        free((*morph)->fading_points);
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

static int dgx_morph_max_int(int a, int b)
{
    return a > b ? a : b;
}

dgx_morph_t *dgx_morph_create(
    const dgx_bit_matrix_t  *from,
    const dgx_bit_matrix_t  *to,
    dgx_morph_sources_func_t sources,
    void                    *user_data)
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

    size_t n_static = 0, n_new = 0, n_old = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            bool f = dgx_matrix_get_point(from, x, y);
            bool t = dgx_matrix_get_point(to, x, y);
            n_static += f && t;
            n_new += !f && t;
            n_old += f && !t;
        }
    }
    struct dgx_morph_ctx ctx = { from, w, h, calloc((size_t)w * h, 1) };
    uint8_t *deferred = calloc((size_t)w * h, 1);
    size_t seg_capacity = n_new;
    m->segments = calloc(n_new ? n_new : 1, sizeof(*m->segments));
    m->static_points = calloc(n_static ? n_static : 1, sizeof(*m->static_points));
    m->fading_points = calloc(n_old ? n_old : 1, sizeof(*m->fading_points));
    if (!ctx.used || !deferred || !m->segments || !m->static_points || !m->fading_points) {
        goto fail;
    }
    if (!n_new) seg_capacity = 1;

    const dgx_point_2d_t center = { .x = (int16_t)(w / 2), .y = (int16_t)(h / 2) };
    if (!n_static && !n_new) {
        /* nothing to become: every dot flies into the center and fades there */
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!dgx_matrix_get_point(from, x, y)) continue;
                dgx_morph_segment_t seg = { { .x = (int16_t)x, .y = (int16_t)y }, center, 255, 0 };
                if (!dgx_morph_push_segment(m, &seg_capacity, seg)) goto fail;
            }
        }
        free(ctx.used);
        free(deferred);
        return m;
    }
    size_t pending = 0;
    for (int pass = 0; pass < DGX_MORPH_MAX_PASSES; ++pass) {
        size_t next_pending = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int idx = y * w + x;
                bool f = dgx_matrix_get_point(from, x, y);
                bool t = dgx_matrix_get_point(to, x, y);
                if (pass == 0 && f && t) {
                    m->static_points[m->number_of_static_points++] =
                        (dgx_point_2d_t){ .x = (int16_t)x, .y = (int16_t)y };
                }
                if (f || !t) continue;
                if (pass > 0 && !deferred[idx]) continue;
                dgx_point_2d_t out[DGX_MORPH_MAX_SOURCES];
                int n = sources ? sources(&ctx, x, y, pass, out, user_data) : 0;
                if (n == DGX_MORPH_DEFER && pass + 1 < DGX_MORPH_MAX_PASSES) {
                    deferred[idx] = 1;
                    ++next_pending;
                    continue;
                }
                deferred[idx] = 0;
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
                    if (out[i].x >= 0 && out[i].x < w && out[i].y >= 0 && out[i].y < h) {
                        ctx.used[out[i].y * w + out[i].x] = 1;
                    }
                }
            }
        }
        pending = next_pending;
        if (!pending) break;
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (dgx_matrix_get_point(from, x, y) && !dgx_matrix_get_point(to, x, y) &&
                !ctx.used[y * w + x]) {
                m->fading_points[m->number_of_fading_points++] =
                    (dgx_point_2d_t){ .x = (int16_t)x, .y = (int16_t)y };
            }
        }
    }
    free(ctx.used);
    free(deferred);
    return m;

fail:
    free(ctx.used);
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
        uint8_t intensity = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        dgx_point_2d_t head, tail;
        bool head_ok = dgx_morph_pixel(s->start, s->end, t, x, y, cell_width, &head);
        if (trail) {
            if (dgx_morph_pixel(s->start, s->end, t_tail, x, y, cell_width, &tail)) {
                dot(user_data, &tail, (uint8_t)(intensity / 2));
            }
            if (head_ok) dot(user_data, &head, (uint8_t)(intensity / 2));
        } else if (head_ok) {
            dot(user_data, &head, intensity);
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
