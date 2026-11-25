#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_vscreen.h"
#include "../dgx_clip_priv.h"
#include "../vscreen/dgx_alpha_priv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Reading texels straight from the buffer needs vscreen.c, which is optional in the component build. */
#if !defined(ESP_PLATFORM) || defined(CONFIG_DGX_ENABLE_VSCREEN)
#define DGX_TEXTURE_VSCREEN_FAST_PATH 1
#endif

static const char TAG[] = "DGX TEXTURE";

/* 16.16 fixed point */
#define DGX_FP_ONE         65536
#define DGX_FP_FLOOR(v)    ((int)((v) >> 16))
#define DGX_FP_ROUND(v)    ((int)(((v) + DGX_FP_ONE / 2) >> 16))
#define DGX_FP_FROM_INT(v) ((int32_t)(v) * DGX_FP_ONE)

/*
 * How the sides of a quad turn at its corners. A convex quad turns the same
 * way at every corner, and with four corners turning one way the sides
 * cannot go round twice. A quad whose sides cross turns both ways, and so
 * does one that bends inward: DGX_TEXTURE_BENT. A corner given twice, or on
 * the line of its neighbours, does not turn: DGX_TEXTURE_FLAT, a triangle or
 * less.
 */
enum { DGX_TEXTURE_CONVEX, DGX_TEXTURE_FLAT, DGX_TEXTURE_BENT };

static int dgx_texture_turns(const int x[4], const int y[4])
{
    bool left = false, right = false, flat = false;
    for (int i = 0; i < 4; ++i) {
        const int     j = (i + 1) & 3, k = (i + 2) & 3;
        const int64_t turn = (int64_t)(x[j] - x[i]) * (y[k] - y[j]) - (int64_t)(y[j] - y[i]) * (x[k] - x[j]);
        left |= turn > 0, right |= turn < 0, flat |= turn == 0;
    }
    return left && right ? DGX_TEXTURE_BENT : flat ? DGX_TEXTURE_FLAT : DGX_TEXTURE_CONVEX;
}

typedef struct {
    dgx_point_2d_t p; /* on the screen */
    dgx_point_2d_t s; /* in the texture */
} dgx_texture_vertex_t;

/* One side of the quad walked down the scanlines. */
typedef struct {
    int32_t x, u, v;
    int32_t dx, du, dv;
} dgx_texture_edge_t;

typedef struct {
    dgx_screen_t  *dst;
    dgx_screen_t  *src;
    uint8_t       *row;      /* one packed scanline in the destination format */
    const uint8_t *texels;   /* 16-bit linear texture: direct access, else NULL */
    int            left, top, right, bottom; /* texture region, inclusive */
    uint32_t       pixel_bits;               /* bits one pixel takes in write_area() data */
    int            y_last;                   /* last scanline that can be drawn */
    /* window left open by the previous span: the next scanline continues it when its x range is the same */
    int            win_x0, win_x1, win_y;
} dgx_texture_ctx_t;

static int32_t dgx_texture_slope(int from, int to, int steps)
{
    return (int32_t)(((int64_t)(to - from) * DGX_FP_ONE) / steps);
}

/* Where a vertex sits inside its scanline, in half rows: the top of the quad, the bottom, or a row center. */
static inline int dgx_texture_vertex_half(int y, int y_start, int y_end)
{
    return y == y_start ? 0 : (y == y_end ? 2 : 1);
}

/*
 * Set the edge a -> b up for scanline y_first. The screen x runs from vertex
 * to vertex. Texture coordinates are sampled at the scanline center, with the
 * quad covering the texture region from its first texel to the far side of
 * its last one, so that every texel gets an equal share of the rows.
 */
static void dgx_texture_edge_init(dgx_texture_edge_t *e, const dgx_texture_vertex_t *a, const dgx_texture_vertex_t *b, int y_first,
                                  int y_start, int y_end)
{
    int dy = b->p.y - a->p.y;
    e->x   = DGX_FP_FROM_INT(a->p.x);
    e->u   = DGX_FP_FROM_INT(a->s.x);
    e->v   = DGX_FP_FROM_INT(a->s.y);
    e->dx = e->du = e->dv = 0;
    if (dy <= 0) return;
    int half_a = dgx_texture_vertex_half(a->p.y, y_start, y_end);
    int half_b = dgx_texture_vertex_half(b->p.y, y_start, y_end);
    int length = 2 * dy + half_b - half_a;          /* edge length in half rows */
    int offset = 2 * (y_first - a->p.y) + 1 - half_a; /* from a to the center of y_first */
    e->dx      = dgx_texture_slope(a->p.x, b->p.x, dy);
    e->x += e->dx * (y_first - a->p.y);
    int64_t span_u = (int64_t)(b->s.x - a->s.x) * DGX_FP_ONE;
    int64_t span_v = (int64_t)(b->s.y - a->s.y) * DGX_FP_ONE;
    e->du          = (int32_t)(span_u * 2 / length);
    e->dv          = (int32_t)(span_v * 2 / length);
    e->u += (int32_t)(span_u * offset / length);
    e->v += (int32_t)(span_v * offset / length);
}

static inline void dgx_texture_edge_step(dgx_texture_edge_t *e)
{
    e->x += e->dx;
    e->u += e->du;
    e->v += e->dv;
}

static void dgx_texture_span(dgx_texture_ctx_t *ctx, int y, const dgx_texture_edge_t *a, const dgx_texture_edge_t *b)
{
    dgx_screen_t *dst = ctx->dst;
    const int     cl = dgx_clip_left(dst), cr = dgx_clip_right(dst);
    if (y < dgx_clip_top(dst) || y >= dgx_clip_bottom(dst)) return;
    int x0 = DGX_FP_ROUND(a->x);
    int x1 = DGX_FP_ROUND(b->x);
    if (x1 < x0) {
        const dgx_texture_edge_t *t = a;
        a                           = b;
        b                           = t;
        int tx                      = x0;
        x0                          = x1;
        x1                          = tx;
    }
    if (x1 < cl || x0 >= cr) return;

    /* sample at pixel centers: the span covers the texture from a's coordinate to b's */
    int     pixels = x1 - x0 + 1;
    int32_t du     = (int32_t)(((int64_t)b->u - a->u) / pixels);
    int32_t dv     = (int32_t)(((int64_t)b->v - a->v) / pixels);
    int32_t u      = a->u + du / 2;
    int32_t v      = a->v + dv / 2;
    if (x0 < cl) {
        u  = (int32_t)(u + (int64_t)du * (cl - x0));
        v  = (int32_t)(v + (int64_t)dv * (cl - x0));
        x0 = cl;
    }
    if (x1 >= cr) x1 = cr - 1;
    int count = x1 - x0 + 1;

    /* the previous scanline may still be on its way to the panel from this buffer */
    if (dst->wait) dst->wait(dst);
    uint8_t *lp = ctx->row;
    if (ctx->texels) {
        const int pitch = ctx->src->width * 2;
        for (int i = 0; i < count; ++i) {
            int tx = DGX_FP_FLOOR(u);
            int ty = DGX_FP_FLOOR(v);
            if (tx < ctx->left) tx = ctx->left;
            else if (tx > ctx->right) tx = ctx->right;
            if (ty < ctx->top) ty = ctx->top;
            else if (ty > ctx->bottom) ty = ctx->bottom;
            const uint8_t *texel = ctx->texels + ty * pitch + tx * 2;
            *lp++                = texel[0];
            *lp++                = texel[1];
            u += du;
            v += dv;
        }
    } else {
        /* packed depths (1, 4, 12 bits) are written with read-modify-write */
        memset(lp, 0, dgx_color_points_to_bytes(dst->color_bits, (uint32_t)count));
        for (int i = 0; i < count; ++i) {
            int tx = DGX_FP_FLOOR(u);
            int ty = DGX_FP_FLOOR(v);
            if (tx < ctx->left) tx = ctx->left;
            else if (tx > ctx->right) tx = ctx->right;
            if (ty < ctx->top) ty = ctx->top;
            else if (ty > ctx->bottom) ty = ctx->bottom;
            lp = dgx_fill_buf_value(dst->color_bits, lp, i, ctx->src->get_pixel(ctx->src, tx, ty));
            u += du;
            v += dv;
        }
    }
    if (y != ctx->win_y || x0 != ctx->win_x0 || x1 != ctx->win_x1) {
        dst->set_area(dst, (uint16_t)x0, (uint16_t)x1, (uint16_t)y, (uint16_t)ctx->y_last);
        ctx->win_x0 = x0;
        ctx->win_x1 = x1;
    }
    /*
     * Pixels that share bytes may end a row inside a byte, and the next row cannot go on from there: it opens
     * a window of its own. A display behind a bus takes whole bytes, and 12-bit pixels go to one: an odd count
     * of them goes with the rest of its last byte, half a pixel that is never finished, or a bus that sends the
     * bits as they are counted cuts the last pixel short. A screen in memory takes no pixel of that half.
     */
    uint32_t lenbits = (uint32_t)count * ctx->pixel_bits;
    if (lenbits % 8u) {
        if (ctx->pixel_bits == 12u) lenbits += 4u;
        ctx->win_y = INT32_MIN;
    } else {
        ctx->win_y = y + 1;
    }
    dst->write_area(dst, ctx->row, lenbits);
}

esp_err_t dgx_draw_texture_quad(dgx_screen_t *scr, const dgx_point_2d_t quad[4], dgx_screen_t *texture, int tx, int ty, int tw, int th)
{
    if (!scr || !quad || !texture) return ESP_ERR_INVALID_ARG;
    if (tw <= 0 || th <= 0) return ESP_OK;
    if (!scr->set_area || !scr->write_area || !texture->get_pixel) return ESP_ERR_NOT_SUPPORTED;
    if (scr->color_bits != texture->color_bits) {
        ESP_LOGE(TAG, "texture color bits %d != screen %d", texture->color_bits, scr->color_bits);
        return ESP_ERR_INVALID_ARG;
    }
    if (dgx_color_points_to_bytes(scr->color_bits, 1) == 0) return ESP_ERR_NOT_SUPPORTED; /* unsupported depth */
    if (tx < 0 || ty < 0 || tw > texture->width - tx || th > texture->height - ty) {
        ESP_LOGE(TAG, "texture region %d,%d %dx%d is outside the %dx%d texture", tx, ty, tw, th, texture->width, texture->height);
        return ESP_ERR_INVALID_ARG;
    }
    /* 16.16 fixed point: a texture coordinate of 32768 and more does not fit */
    if (tx + tw > INT16_MAX || ty + th > INT16_MAX) {
        ESP_LOGE(TAG, "texture region is too large for 16.16 fixed point");
        return ESP_ERR_INVALID_ARG;
    }
    {
        int cx[4], cy[4];
        for (int i = 0; i < 4; ++i) cx[i] = quad[i].x, cy[i] = quad[i].y;
        if (dgx_texture_turns(cx, cy) == DGX_TEXTURE_BENT) {
            ESP_LOGE(TAG, "the quad is not convex: its sides cross or it bends inward");
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* quad[] follows the texture region corners: top-left, top-right, bottom-right, bottom-left */
    dgx_texture_vertex_t vertices[4];
    int                  top = 0;
    for (int i = 0; i < 4; ++i) {
        vertices[i].p   = quad[i];
        vertices[i].s.x = (int16_t)(i == 1 || i == 2 ? tx + tw : tx); /* far side of the last texel */
        vertices[i].s.y = (int16_t)(i >= 2 ? ty + th : ty);
        if (quad[i].y < quad[top].y || (quad[i].y == quad[top].y && quad[i].x < quad[top].x)) top = i;
    }
    int y_start = quad[top].y;
    int y_end   = y_start;
    int x_min = quad[0].x, x_max = quad[0].x;
    for (int i = 0; i < 4; ++i) {
        if (quad[i].y > y_end) y_end = quad[i].y;
        if (quad[i].x < x_min) x_min = quad[i].x;
        if (quad[i].x > x_max) x_max = quad[i].x;
    }
    if (dgx_clip_misses(scr, x_min, x_max, y_start, y_end)) return ESP_OK;
    /* nor a slope of 32768 pixels and more a row */
    if (x_max - x_min > INT16_MAX) {
        ESP_LOGE(TAG, "quad is too large for 16.16 fixed point");
        return ESP_ERR_INVALID_ARG;
    }

    dgx_texture_ctx_t ctx = {
        .dst        = scr,
        .src        = texture,
        .left       = tx,
        .top        = ty,
        .right      = tx + tw - 1,
        .bottom     = ty + th - 1,
        .pixel_bits = scr->color_bits == 18 ? 24u : scr->color_bits,
        .y_last     = y_end < dgx_clip_bottom(scr) ? y_end : dgx_clip_bottom(scr) - 1,
        .win_y      = INT32_MIN,
    };
#ifdef DGX_TEXTURE_VSCREEN_FAST_PATH
    if (texture->color_bits == 16 && dgx_vscreen_is_linear(texture)) {
        ctx.texels = ((dgx_vscreen_t *)texture)->v_array;
    }
#endif
    /* A panel sends its bus buffer by DMA; a screen without one gets a scanline for the duration of the call. */
    uint32_t row_bytes = dgx_color_points_to_bytes(scr->color_bits, (uint32_t)scr->width);
    ctx.row            = scr->draw_buffer && scr->draw_buffer_len >= row_bytes ? scr->draw_buffer : NULL;
    uint8_t *own_row   = NULL;
    if (!ctx.row) {
        ctx.row = own_row = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
        if (!own_row) {
            ESP_LOGE(TAG, "failed to allocate a %u-byte scanline", (unsigned)row_bytes);
            return ESP_ERR_NO_MEM;
        }
    }

#define DGX_QUAD_NEXT(i) (((i) + 1) & 3)
#define DGX_QUAD_PREV(i) (((i) + 3) & 3)
    dgx_texture_edge_t e_prev, e_next;
    dgx_screen_batch_begin(scr);
    if (y_start == y_end) {
        /* a single scanline: from the leftmost vertex to the rightmost one, through the middle of the region */
        int i_min = 0, i_max = 0;
        for (int i = 1; i < 4; ++i) {
            if (quad[i].x < quad[i_min].x) i_min = i;
            if (quad[i].x > quad[i_max].x) i_max = i;
        }
        dgx_texture_edge_init(&e_prev, &vertices[i_min], &vertices[i_min], y_start, y_start, y_end);
        dgx_texture_edge_init(&e_next, &vertices[i_max], &vertices[i_max], y_start, y_start, y_end);
        e_prev.v = e_next.v = DGX_FP_FROM_INT(ty) + DGX_FP_FROM_INT(th) / 2;
        /* and a single pixel through the middle of its columns too */
        if (quad[i_min].x == quad[i_max].x) e_prev.u = e_next.u = DGX_FP_FROM_INT(tx) + DGX_FP_FROM_INT(tw) / 2;
        dgx_texture_span(&ctx, y_start, &e_prev, &e_next);
    } else {
        /* Two chains run from the top vertex to the bottom one, around both sides of the quad. */
        int v_prev = DGX_QUAD_PREV(top);
        int v_next = DGX_QUAD_NEXT(top);
        int from_prev = top, from_next = top;
        /* A horizontal top edge: start that chain from its other end, or the first scanline has no width. */
        if (vertices[v_prev].p.y == y_start) {
            from_prev = v_prev;
            v_prev    = DGX_QUAD_PREV(v_prev);
        }
        if (vertices[v_next].p.y == y_start) {
            from_next = v_next;
            v_next    = DGX_QUAD_NEXT(v_next);
        }
        dgx_texture_edge_init(&e_prev, &vertices[from_prev], &vertices[v_prev], y_start, y_start, y_end);
        dgx_texture_edge_init(&e_next, &vertices[from_next], &vertices[v_next], y_start, y_start, y_end);
        for (int y = y_start; y <= y_end; ++y) {
            dgx_texture_span(&ctx, y, &e_prev, &e_next);
            /* no step past the last row or past a vertex: an edge that runs far aside in a row would leave the 16.16 range */
            if (y == y_end) break;
            /*
             * Past a vertex the next edge of the chain takes over from the following scanline. Vertices on the
             * same scanline (a doubled one: a triangle passed as a quad) are skipped, an edge between them has no rows.
             */
            if (y == vertices[v_prev].p.y) {
                do {
                    from_prev = v_prev;
                    v_prev    = DGX_QUAD_PREV(v_prev);
                } while (y == vertices[v_prev].p.y);
                dgx_texture_edge_init(&e_prev, &vertices[from_prev], &vertices[v_prev], y + 1, y_start, y_end);
            } else {
                dgx_texture_edge_step(&e_prev);
            }
            if (y == vertices[v_next].p.y) {
                do {
                    from_next = v_next;
                    v_next    = DGX_QUAD_NEXT(v_next);
                } while (y == vertices[v_next].p.y);
                dgx_texture_edge_init(&e_next, &vertices[from_next], &vertices[v_next], y + 1, y_start, y_end);
            } else {
                dgx_texture_edge_step(&e_next);
            }
        }
    }
#undef DGX_QUAD_NEXT
#undef DGX_QUAD_PREV
    if (own_row) {
        if (scr->wait) scr->wait(scr);
        free(own_row);
    }
    dgx_screen_touch(scr, x_min, x_max, y_start, y_end);
    dgx_screen_batch_end(scr);
    return ESP_OK;
}

esp_err_t dgx_draw_texture_rect(dgx_screen_t *scr, int x, int y, int w, int h, dgx_screen_t *texture, int tx, int ty, int tw, int th)
{
    if (w <= 0 || h <= 0) return ESP_OK;
    const dgx_point_2d_t quad[4] = {
        {(int16_t)x,           (int16_t)y          },
        {(int16_t)(x + w - 1), (int16_t)y          },
        {(int16_t)(x + w - 1), (int16_t)(y + h - 1)},
        {(int16_t)x,           (int16_t)(y + h - 1)},
    };
    return dgx_draw_texture_quad(scr, quad, texture, tx, ty, tw, th);
}

#ifdef DGX_TEXTURE_VSCREEN_FAST_PATH

/*
 * ---- A texture on a quad whose corners lie between pixels: dgx_draw_texture_quad_with() ----
 *
 * Each pixel the quad may touch is looked at by its centre. Its distance from
 * each side, positive inside, is kept in 1/256 of a pixel and stepped along
 * the row. A soft edge covers a pixel by the share of its area inside the
 * quad, see dgx_tq_cover(); a hard one where the centre is inside. Where it
 * is covered, the point
 * its centre maps to in the texture is found: by an affine map, stepped in
 * 16.16 along the row, for a parallelogram, or by the projective map of the
 * square onto the quad (Heckbert's), one division a pixel, for any other. The
 * pixels to write go into a row as runs, each sent through its own window.
 * A linear virtual screen has the pixels written straight into its memory,
 * as text and segments are. The part of a row the quad covers whole goes by a
 * loop of its own, no coverage there: a texture with no transparent texel to
 * any screen, any texture to such memory.
 */

typedef struct {
    dgx_screen_t             *dst;
    const dgx_texture_quad_t *q;
    const uint8_t            *texels;  /* the rows of the texture */
    int                       pitch;   /* bytes from a row of it to the next */
    bool                      indexed; /* 8 bits: indices into q->lut */
    bool                      read;    /* soft edges and translucent texels are mixed with what lies on the screen */
    uint8_t                  *memory;  /* a linear virtual screen: its pixels, read and written straight; else NULL */
    int                       memory_pitch;
    uint16_t                 *row; /* the run being made, RGB565 as the wire takes it: the high byte first */
    int                       run_x, run_n, y;
    const uint8_t            *alpha_map; /* the opacity of each texel, rows of `alpha_pitch` bytes; else NULL */
    int                       alpha_pitch;
} dgx_tq_t;

/* an RGB565 value as the wire takes it, stored as 16 bits on a little-endian chip, and back */
static inline uint16_t dgx_tq_swap(uint16_t c)
{
    return (uint16_t)(c << 8 | c >> 8);
}

static inline uint16_t dgx_tq_texel(const dgx_tq_t *t, int u, int v, uint32_t *alpha)
{
    const dgx_texture_quad_t *q = t->q;
    uint16_t                  c;
    if (t->indexed) {
        const uint8_t i = t->texels[(size_t)(q->ty + v) * (size_t)t->pitch + (size_t)(q->tx + u)];
        *alpha          = q->alpha ? q->alpha[i] : 255u;
        c               = q->lut[i];
    } else {
        const uint8_t *p = t->texels + (size_t)(q->ty + v) * (size_t)t->pitch + (size_t)(q->tx + u) * 2u;
        c                = (uint16_t)(p[0] << 8 | p[1]);
        *alpha           = (q->flags & DGX_TEXTURE_KEY) && c == (uint16_t)q->key ? 0u : 255u;
    }
    if (t->alpha_map) {
        const uint32_t m = t->alpha_map[(size_t)(q->ty + v) * (size_t)t->alpha_pitch + (size_t)(q->tx + u)];
        *alpha           = *alpha == 255u ? m : (*alpha * m + 127u) / 255u;
    }
    return c;
}

static inline int dgx_tq_clamp(int v, int hi)
{
    return v < 0 ? 0 : v > hi ? hi : v;
}

/* the texture at (s, v) of its region, texels counted in 16.16 from its corner; its opacity, 0 to 255, in *alpha */
static uint16_t dgx_tq_sample(const dgx_tq_t *t, int32_t s, int32_t v, uint32_t *alpha)
{
    const dgx_texture_quad_t *q = t->q;
    if (!(q->flags & DGX_TEXTURE_BILINEAR)) return dgx_tq_texel(t, dgx_tq_clamp(s >> 16, q->tw - 1), dgx_tq_clamp(v >> 16, q->th - 1), alpha);
    /* the four texels whose centres are around the point, and the weights of the right and lower ones in 32 steps */
    const int32_t  s0 = s - 32768, v0 = v - 32768;
    const int      u0 = s0 >> 16, w0 = v0 >> 16;
    const uint32_t fu = (uint32_t)(s0 & 0xffff) >> 11, fv = (uint32_t)(v0 & 0xffff) >> 11;
    const int      ua = dgx_tq_clamp(u0, q->tw - 1), ub = dgx_tq_clamp(u0 + 1, q->tw - 1);
    const int      va = dgx_tq_clamp(w0, q->th - 1), vb = dgx_tq_clamp(w0 + 1, q->th - 1);
    uint32_t       a00, a10, a01, a11;
    const uint16_t c00 = dgx_tq_texel(t, ua, va, &a00), c10 = dgx_tq_texel(t, ub, va, &a10);
    const uint16_t c01 = dgx_tq_texel(t, ua, vb, &a01), c11 = dgx_tq_texel(t, ub, vb, &a11);
    if ((a00 & a10 & a01 & a11) == 255u) {
        *alpha = 255u;
        return dgx_alpha_mix16(dgx_alpha_mix16(c00, c10, fu), dgx_alpha_mix16(c01, c11, fu), fv);
    }
    /* weighted by opacity too, so that a transparent texel lends its neighbours no color */
    const uint32_t w00 = (32u - fu) * (32u - fv) * a00, w10 = fu * (32u - fv) * a10, w01 = (32u - fu) * fv * a01, w11 = fu * fv * a11;
    const uint32_t sum = w00 + w10 + w01 + w11;
    *alpha             = sum / 1024u;
    if (!sum) return 0;
    const uint32_t r = ((c00 >> 11) * w00 + (c10 >> 11) * w10 + (c01 >> 11) * w01 + (c11 >> 11) * w11) / sum;
    const uint32_t g = (((c00 >> 5) & 63u) * w00 + ((c10 >> 5) & 63u) * w10 + ((c01 >> 5) & 63u) * w01 + ((c11 >> 5) & 63u) * w11) / sum;
    const uint32_t b = ((c00 & 31u) * w00 + (c10 & 31u) * w10 + (c01 & 31u) * w01 + (c11 & 31u) * w11) / sum;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static inline void dgx_tq_flush(dgx_tq_t *t)
{
    if (!t->run_n) return;
    dgx_screen_t *dst = t->dst;
    dst->set_area(dst, (uint16_t)t->run_x, (uint16_t)(t->run_x + t->run_n - 1), (uint16_t)t->y, (uint16_t)t->y);
    dst->write_area(dst, (uint8_t *)t->row, (uint32_t)t->run_n * 16u);
    t->run_n = 0;
}

/* the run goes on at x: a run that ends elsewhere is sent first, and a new one waits for the row to be free */
static inline uint16_t *dgx_tq_at(dgx_tq_t *t, int x)
{
    if (t->run_n && t->run_x + t->run_n != x) dgx_tq_flush(t);
    /* the run before may still be on its way to the panel from this buffer */
    if (!t->run_n) {
        if (t->dst->wait) t->dst->wait(t->dst);
        t->run_x = x;
    }
    return t->row + t->run_n;
}

/* the pixel (x, y) of a color put over what is there by a weight of 32 */
static void dgx_tq_put(dgx_tq_t *t, int x, uint16_t color, uint32_t w)
{
    const dgx_texture_quad_t *q = t->q;
    if (w < 32u) {
        uint16_t under;
        if (q->flags & DGX_TEXTURE_PAPER) {
            under = (uint16_t)q->paper;
        } else if (t->memory) {
            under = dgx_tq_swap(*((uint16_t *)(t->memory + t->y * t->memory_pitch) + x));
        } else if (t->read) {
            under = (uint16_t)t->dst->get_pixel(t->dst, x, t->y);
        } else {
            /* nothing to mix with: whole from half on */
            if (w < 16u) {
                dgx_tq_flush(t);
                return;
            }
            w = 32u;
        }
        if (w < 32u) color = dgx_alpha_mix16(under, color, w);
    }
    if (t->memory) {
        *((uint16_t *)(t->memory + t->y * t->memory_pitch) + x) = dgx_tq_swap(color);
        return;
    }
    *dgx_tq_at(t, x) = dgx_tq_swap(color);
    ++t->run_n;
}

/*
 * The map along a row: affine, the point at a pixel in 16.16 and its step;
 * or projective, the numerators and the denominator at the first pixel the
 * quad has in the row and their steps, and `k`, the pixel counted from that
 * one. A pixel has the same point whatever pixel of the row is drawn first.
 */
typedef struct {
    int32_t s, v, ds, dv;
    float   ns, nt, nw, dns, dnt, dnw, ks, kt;
    int     k;
} dgx_tq_map_t;

/* the point of the pixel k of the projective map */
static inline void dgx_tq_project(const dgx_tq_map_t *m, int k, int32_t *s, int32_t *v)
{
    const float fk = (float)k, inv = 1.0f / (m->nw + fk * m->dnw);
    *s = (int32_t)((m->ns + fk * m->dns) * inv * m->ks), *v = (int32_t)((m->nt + fk * m->dnt) * inv * m->kt);
}

/* an opaque texel by the point (s, v), 16.16 texels from the corner of the region: the nearest one, as the wire takes it */
static inline uint16_t dgx_tq_nearest(const uint8_t *base, int pitch, int umax, int vmax, int32_t s, int32_t v)
{
    int u = s >> 16, w = v >> 16;
    u = u < 0 ? 0 : u > umax ? umax : u;
    w = w < 0 ? 0 : w > vmax ? vmax : w;
    return *(const uint16_t *)(base + w * pitch + u * 2);
}

/* the same mixed of the four around the point, each channel in one multiplication of a weight in 32 steps */
static inline uint16_t dgx_tq_bilinear(const uint8_t *base, int pitch, int umax, int vmax, int32_t s, int32_t v)
{
    const int32_t  s0 = s - 32768, v0 = v - 32768;
    const int      u0 = s0 >> 16, w0 = v0 >> 16;
    const uint32_t fu = (uint32_t)(s0 & 0xffff) >> 11, fv = (uint32_t)(v0 & 0xffff) >> 11;
    const int      ua = u0 < 0 ? 0 : u0 > umax ? umax : u0, ub = u0 + 1 < 0 ? 0 : u0 + 1 > umax ? umax : u0 + 1;
    const int      va = w0 < 0 ? 0 : w0 > vmax ? vmax : w0, vb = w0 + 1 < 0 ? 0 : w0 + 1 > vmax ? vmax : w0 + 1;
    const uint8_t *ra = base + va * pitch, *rb = base + vb * pitch;
    const uint32_t m = 0x07E0F81Fu;
    uint32_t       c00 = dgx_tq_swap(*(const uint16_t *)(ra + ua * 2)), c10 = dgx_tq_swap(*(const uint16_t *)(ra + ub * 2));
    uint32_t       c01 = dgx_tq_swap(*(const uint16_t *)(rb + ua * 2)), c11 = dgx_tq_swap(*(const uint16_t *)(rb + ub * 2));
    c00 = (c00 | c00 << 16) & m, c10 = (c10 | c10 << 16) & m, c01 = (c01 | c01 << 16) & m, c11 = (c11 | c11 << 16) & m;
    const uint32_t top = ((c00 * (32u - fu) + c10 * fu) >> 5) & m, bottom = ((c01 * (32u - fu) + c11 * fu) >> 5) & m;
    const uint32_t c   = ((top * (32u - fv) + bottom * fv) >> 5) & m;
    return dgx_tq_swap((uint16_t)(c | c >> 16));
}

/*
 * n pixels the quad covers whole, of a texture with no transparent texel, into
 * `out`: each its texel, nearest or bilinear, by the map stepped along; the
 * map is left at the pixel after them.
 */
static __attribute__((aligned(16))) void dgx_tq_inside(const dgx_tq_t *t, uint16_t *out, int n, dgx_tq_map_t *map, bool bilinear, bool projective)
{
    const dgx_texture_quad_t *q    = t->q;
    const int                 pitch = t->pitch, umax = q->tw - 1, vmax = q->th - 1;
    const uint8_t            *base = t->texels + q->ty * pitch + q->tx * 2;
    int32_t                   s = map->s, v = map->v;
    const int32_t             ds = map->ds, dv = map->dv;
    if (!projective) {
        if (bilinear)
            for (int k = 0; k < n; ++k, s += ds, v += dv) out[k] = dgx_tq_bilinear(base, pitch, umax, vmax, s, v);
        else
            for (int k = 0; k < n; ++k, s += ds, v += dv) out[k] = dgx_tq_nearest(base, pitch, umax, vmax, s, v);
        map->s = s, map->v = v;
        return;
    }
    const dgx_tq_map_t m = *map;
    for (int k = 0; k < n; ++k) {
        dgx_tq_project(&m, m.k + k, &s, &v);
        out[k] = bilinear ? dgx_tq_bilinear(base, pitch, umax, vmax, s, v) : dgx_tq_nearest(base, pitch, umax, vmax, s, v);
    }
    map->k += n;
}

/* a color of an opacity of 0 to 255 over the pixel `at` in memory, or over the paper */
static inline void dgx_tq_over(uint16_t *at, uint16_t color, uint32_t alpha, bool paper, uint16_t paper_color)
{
    const uint32_t w = alpha == 255u ? 32u : (32u * alpha + 127u) / 255u;
    if (!w) return;
    if (w < 32u) color = dgx_alpha_mix16(paper ? paper_color : dgx_tq_swap(*at), color, w);
    *at = dgx_tq_swap(color);
}

/*
 * The texel of an 8-bit texture at (s, v), as dgx_tq_sample() gives it, from
 * the values of the call kept at hand: `bilinear` is the same for a whole
 * loop, which has its own copy of this without the other way.
 */
static inline __attribute__((always_inline)) uint16_t dgx_tq_indexed(const uint8_t *base, int pitch, int umax, int vmax, const uint16_t *lut,
                                                                     const uint8_t *alphas, bool bilinear, int32_t s, int32_t v, uint32_t *alpha)
{
    if (!bilinear) {
        int u = s >> 16, w = v >> 16;
        u = u < 0 ? 0 : u > umax ? umax : u;
        w = w < 0 ? 0 : w > vmax ? vmax : w;
        const uint8_t i = base[w * pitch + u];
        *alpha          = alphas ? alphas[i] : 255u;
        return lut[i];
    }
    const int32_t  s0 = s - 32768, v0 = v - 32768;
    const int      u0 = s0 >> 16, w0 = v0 >> 16;
    const uint32_t fu = (uint32_t)(s0 & 0xffff) >> 11, fv = (uint32_t)(v0 & 0xffff) >> 11;
    const int      ua = u0 < 0 ? 0 : u0 > umax ? umax : u0, ub = u0 + 1 < 0 ? 0 : u0 + 1 > umax ? umax : u0 + 1;
    const int      va = w0 < 0 ? 0 : w0 > vmax ? vmax : w0, vb = w0 + 1 < 0 ? 0 : w0 + 1 > vmax ? vmax : w0 + 1;
    const uint8_t  i00 = base[va * pitch + ua], i10 = base[va * pitch + ub], i01 = base[vb * pitch + ua], i11 = base[vb * pitch + ub];
    const uint16_t c00 = lut[i00], c10 = lut[i10], c01 = lut[i01], c11 = lut[i11];
    const uint32_t a00 = alphas ? alphas[i00] : 255u, a10 = alphas ? alphas[i10] : 255u, a01 = alphas ? alphas[i01] : 255u,
                   a11 = alphas ? alphas[i11] : 255u;
    if ((a00 & a10 & a01 & a11) == 255u) {
        *alpha = 255u;
        return dgx_alpha_mix16(dgx_alpha_mix16(c00, c10, fu), dgx_alpha_mix16(c01, c11, fu), fv);
    }
    const uint32_t w00 = (32u - fu) * (32u - fv) * a00, w10 = fu * (32u - fv) * a10, w01 = (32u - fu) * fv * a01, w11 = fu * fv * a11;
    const uint32_t sum = w00 + w10 + w01 + w11;
    *alpha             = sum / 1024u;
    if (!sum) return 0;
    const uint32_t r = ((c00 >> 11) * w00 + (c10 >> 11) * w10 + (c01 >> 11) * w01 + (c11 >> 11) * w11) / sum;
    const uint32_t g = (((c00 >> 5) & 63u) * w00 + ((c10 >> 5) & 63u) * w10 + ((c01 >> 5) & 63u) * w01 + ((c11 >> 5) & 63u) * w11) / sum;
    const uint32_t b = ((c00 & 31u) * w00 + (c10 & 31u) * w10 + (c01 & 31u) * w01 + (c11 & 31u) * w11) / sum;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* n pixels of an 8-bit texture into memory from `at` on, the way of sampling fixed for the loop */
static inline __attribute__((always_inline)) void dgx_tq_indexed_run(const dgx_tq_t *t, uint16_t *at, int n, dgx_tq_map_t *map, bool projective,
                                                                     bool bilinear)
{
    const dgx_texture_quad_t *q = t->q;
    const int                 pitch = t->pitch, umax = q->tw - 1, vmax = q->th - 1;
    const uint8_t            *base   = t->texels + q->ty * pitch + q->tx;
    const uint16_t           *lut    = q->lut;
    const uint8_t            *alphas = q->alpha;
    const bool                paper  = (q->flags & DGX_TEXTURE_PAPER) != 0;
    const uint16_t            paper_color = (uint16_t)q->paper;
    int32_t                   s = map->s, v = map->v;
    const int32_t             ds = map->ds, dv = map->dv;
    for (int k = 0; k < n; ++k) {
        if (projective) dgx_tq_project(map, map->k + k, &s, &v);
        uint32_t       alpha;
        const uint16_t color = dgx_tq_indexed(base, pitch, umax, vmax, lut, alphas, bilinear, s, v, &alpha);
        if (!projective) s += ds, v += dv;
        dgx_tq_over(at + k, color, alpha, paper, paper_color);
    }
    if (projective) map->k += n;
    else map->s = s, map->v = v;
}

/*
 * n pixels the quad covers whole, of a texture with transparent texels, in
 * the memory of a linear virtual screen from `at` on: each its texel by its
 * opacity over what is there, or over the paper.
 */
static __attribute__((noinline, aligned(16))) void dgx_tq_inside_mixed(const dgx_tq_t *t, uint16_t *at, int n, dgx_tq_map_t *map, bool projective)
{
    const dgx_texture_quad_t *q     = t->q;
    if (t->indexed) {
        if (q->flags & DGX_TEXTURE_BILINEAR) dgx_tq_indexed_run(t, at, n, map, projective, true);
        else dgx_tq_indexed_run(t, at, n, map, projective, false);
        return;
    }
    const bool                paper = (q->flags & DGX_TEXTURE_PAPER) != 0;
    int32_t                   s = map->s, v = map->v;
    for (int k = 0; k < n; ++k) {
        if (projective) dgx_tq_project(map, map->k + k, &s, &v);
        uint32_t alpha;
        uint16_t color = dgx_tq_sample(t, s, v, &alpha);
        if (!projective) s += map->ds, v += map->dv;
        dgx_tq_over(at + k, color, alpha, paper, (uint16_t)q->paper);
    }
    if (projective) map->k += n;
    else map->s = s, map->v = v;
}

/*
 * The same with an opacity for every texel, alpha_map: each texel sampled
 * with it, 16-bit or 8-bit. A function of its own, so that the loops of
 * dgx_tq_inside_mixed() are compiled as they were without it.
 */
static void dgx_tq_inside_mapped(const dgx_tq_t *t, uint16_t *at, int n, dgx_tq_map_t *map, bool projective)
{
    const dgx_texture_quad_t *q     = t->q;
    const bool                paper = (q->flags & DGX_TEXTURE_PAPER) != 0;
    int32_t                   s = map->s, v = map->v;
    for (int k = 0; k < n; ++k) {
        if (projective) dgx_tq_project(map, map->k + k, &s, &v);
        uint32_t alpha;
        uint16_t color = dgx_tq_sample(t, s, v, &alpha);
        if (!projective) s += map->ds, v += map->dv;
        dgx_tq_over(at + k, color, alpha, paper, (uint16_t)q->paper);
    }
    if (projective) map->k += n;
    else map->s = s, map->v = v;
}

/*
 * The sides of a quad for its soft edges. A side cuts the square of a pixel
 * while the centre of the pixel is closer to it than half of |nx| + |ny|, the
 * farthest a corner of the square lies from the centre along the normal; the
 * coverage goes in 32 steps, so a side that cuts off less than 1/64 of the
 * pixel is taken to leave it whole, or, from outside, to cover nothing of it.
 */
typedef struct {
    float   nx[4], ny[4];    /* the normals, inward, of length 1 */
    float   far[4], near[4]; /* (a + b) / 2 and (a - b) / 2 of a >= b, |nx| and |ny| */
    float   lin[4], tri[4];  /* 1 / a and 1 / (2 a b): a division is a call of the library on the S3 */
    int32_t whole[4];        /* in 1/256 px: from this far inside the side leaves the pixel whole, from this far outside empty */
} dgx_tq_sides_t;

/* the distance from a side, in 1/256 px, from which it cuts off less than 1/64 of a pixel */
static int32_t dgx_tq_whole(float a, float b)
{
    const float eps = 1.0f / 64.0f;
    const float d   = b > 2.0f * a * eps ? (a + b) * 0.5f - sqrtf(2.0f * a * b * eps) : (0.5f - eps) * a;
    return (int32_t)ceilf(d * 256.0f);
}

/*
 * The share of a pixel inside side i whose distance from its centre is d,
 * positive inside, a >= b the two parts of the normal: while the side crosses
 * two opposite edges of the square the share is linear in d, past that it
 * cuts a corner off the square, a triangle.
 */
static float dgx_tq_half_plane(const dgx_tq_sides_t *s, int i, float d)
{
    if (d > s->near[i]) {
        const float t = s->far[i] - d;
        return 1.0f - t * t * s->tri[i];
    }
    if (d < -s->near[i]) {
        const float t = s->far[i] + d;
        return t * t * s->tri[i];
    }
    return 0.5f + d * s->lin[i];
}

/* the area of the square of a pixel left by the sides of `cut`: the square cut by each in turn */
static float dgx_tq_clipped(const dgx_tq_sides_t *s, const int32_t *dist, unsigned cut)
{
    float x[12] = {-0.5f, 0.5f, 0.5f, -0.5f}, y[12] = {-0.5f, -0.5f, 0.5f, 0.5f}, cx[12], cy[12];
    int   n = 4;
    for (int i = 0; i < 4; ++i) {
        if (!(cut >> i & 1u)) continue;
        const float nx = s->nx[i], ny = s->ny[i], d = (float)dist[i] * (1.0f / 256.0f);
        int         m = 0;
        for (int k = 0; k < n; ++k) {
            const int   j  = k + 1 == n ? 0 : k + 1;
            const float fk = nx * x[k] + ny * y[k] + d, fj = nx * x[j] + ny * y[j] + d;
            if (fk >= 0) cx[m] = x[k], cy[m] = y[k], ++m;
            if ((fk >= 0) != (fj >= 0)) {
                const float u = fk / (fk - fj);
                cx[m] = x[k] + (x[j] - x[k]) * u, cy[m] = y[k] + (y[j] - y[k]) * u, ++m;
            }
        }
        if (m < 3) return 0.0f;
        for (int k = 0; k < m; ++k) x[k] = cx[k], y[k] = cy[k];
        n = m;
    }
    float area2 = 0;
    for (int k = 0; k < n; ++k) {
        const int j = k + 1 == n ? 0 : k + 1;
        area2 += x[k] * y[j] - x[j] * y[k];
    }
    return (area2 < 0 ? -area2 : area2) * 0.5f;
}

/*
 * The coverage of a pixel by a soft edge, 0 to 32: the share of the pixel
 * inside the quad, its area counted exactly, as the shapes of shape.c count
 * it. One side across the pixel gives it in closed form; two or more, at a
 * corner or across a thin quad, by the square cut by each. Out of line: only
 * the pixels of the edges come here.
 */
static __attribute__((noinline)) uint32_t dgx_tq_cover(const dgx_tq_sides_t *s, const int32_t *dist)
{
    unsigned cut = 0;
    int      one = 0, count = 0;
    for (int i = 0; i < 4; ++i) {
        if (dist[i] >= s->whole[i]) continue;
        if (dist[i] <= -s->whole[i]) return 0;
        cut |= 1u << i, one = i, ++count;
    }
    if (!count) return 32;
    const float area = count == 1 ? dgx_tq_half_plane(s, one, (float)dist[one] * (1.0f / 256.0f)) : dgx_tq_clipped(s, dist, cut);
    const int   c    = (int)(area * 32.0f + 0.5f);
    return c <= 0 ? 0u : c >= 32 ? 32u : (uint32_t)c;
}

esp_err_t dgx_draw_texture_quad_with(dgx_screen_t *scr, const dgx_texture_quad_t *q)
{
    if (!scr || !q || !q->texture) return ESP_ERR_INVALID_ARG;
    if (q->tw <= 0 || q->th <= 0) return ESP_OK;
    const int turns = dgx_texture_turns(q->x, q->y);
    if (turns == DGX_TEXTURE_BENT) {
        ESP_LOGE(TAG, "the quad is not convex: its sides cross or it bends inward");
        return ESP_ERR_INVALID_ARG;
    }
    if (scr->color_bits != 16 || !scr->set_area || !scr->write_area) return ESP_ERR_NOT_SUPPORTED;
    dgx_screen_t *texture = q->texture;
    const bool    indexed = texture->color_bits == 8;
    if ((texture->color_bits != 16 && !indexed) || !dgx_vscreen_is_linear(texture)) return ESP_ERR_NOT_SUPPORTED;
    if (indexed && !q->lut) {
        ESP_LOGE(TAG, "an 8-bit texture takes the table of its colors, lut");
        return ESP_ERR_INVALID_ARG;
    }
    dgx_screen_t *alpha_map = q->alpha_map;
    if (alpha_map && (alpha_map->color_bits != 8 || !dgx_vscreen_is_linear(alpha_map))) return ESP_ERR_NOT_SUPPORTED;
    if (alpha_map && (alpha_map->width < texture->width || alpha_map->height < texture->height)) {
        ESP_LOGE(TAG, "alpha map %dx%d is smaller than the %dx%d texture", alpha_map->width, alpha_map->height, texture->width, texture->height);
        return ESP_ERR_INVALID_ARG;
    }
    if (q->tx < 0 || q->ty < 0 || q->tw > texture->width - q->tx || q->th > texture->height - q->ty || q->tx + q->tw > INT16_MAX ||
        q->ty + q->th > INT16_MAX) {
        ESP_LOGE(TAG, "texture region %d,%d %dx%d is outside the %dx%d texture", q->tx, q->ty, q->tw, q->th, texture->width, texture->height);
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * The corners in pixels from (ox, oy), the pixel of the first corner, and
     * the pixels from there too: the quad moved by whole pixels is the same
     * numbers, so its pixels are the same, moved.
     */
    const int ox = q->x[0] >= 0 ? q->x[0] / DGX_SUBPIXEL : -((-q->x[0] + DGX_SUBPIXEL - 1) / DGX_SUBPIXEL);
    const int oy = q->y[0] >= 0 ? q->y[0] / DGX_SUBPIXEL : -((-q->y[0] + DGX_SUBPIXEL - 1) / DGX_SUBPIXEL);
    float     X[4], Y[4], x_lo, x_hi, y_lo, y_hi;
    for (int i = 0; i < 4; ++i) X[i] = (float)(q->x[i] - ox * DGX_SUBPIXEL) / DGX_SUBPIXEL, Y[i] = (float)(q->y[i] - oy * DGX_SUBPIXEL) / DGX_SUBPIXEL;
    x_lo = x_hi = X[0], y_lo = y_hi = Y[0];
    for (int i = 1; i < 4; ++i) {
        if (X[i] < x_lo) x_lo = X[i];
        if (X[i] > x_hi) x_hi = X[i];
        if (Y[i] < y_lo) y_lo = Y[i];
        if (Y[i] > y_hi) y_hi = Y[i];
    }
    if (x_hi - x_lo > (float)INT16_MAX || y_hi - y_lo > (float)INT16_MAX) {
        ESP_LOGE(TAG, "quad is wider than 32767 pixels");
        return ESP_ERR_INVALID_ARG;
    }
    /* twice the signed area: positive when the corners go clockwise on the screen */
    float area2 = 0;
    for (int i = 0; i < 4; ++i) area2 += X[i] * Y[(i + 1) & 3] - X[(i + 1) & 3] * Y[i];
    /* a triangle or less has no map of the square onto it: it draws nothing, as a quad of no area */
    if (turns == DGX_TEXTURE_FLAT || (area2 > -1e-3f && area2 < 1e-3f)) return ESP_OK;
    const bool  soft   = (q->flags & DGX_TEXTURE_SOFT) != 0;
    const float margin = soft ? 0.71f : 0.0f;
    /* the box of the pixels the quad may touch: a soft edge reaches a pixel whose centre is up to half a diagonal outside */
    int bx0 = ox + (int)floorf(x_lo - margin - 0.5f), bx1 = ox + (int)ceilf(x_hi + margin - 0.5f);
    int by0 = oy + (int)floorf(y_lo - margin - 0.5f), by1 = oy + (int)ceilf(y_hi + margin - 0.5f);
    if (dgx_clip_misses(scr, bx0, bx1, by0, by1)) return ESP_OK;
    /* the sides: a distance from each, positive inside, in 1/256 of a pixel at the centre of a pixel, and its step along a row */
    const float orient = area2 > 0 ? 1.0f : -1.0f;
    float          nx[4], ny[4], nc[4];
    dgx_tq_sides_t sides = {0};
    for (int i = 0; i < 4; ++i) {
        const int   j = (i + 1) & 3;
        const float ex = X[j] - X[i], ey = Y[j] - Y[i], len = sqrtf(ex * ex + ey * ey);
        if (len < 1e-6f) {
            nx[i] = ny[i] = 0, nc[i] = 1e9f; /* a corner given twice: a triangle, no side here */
            continue;
        }
        nx[i] = -ey / len * orient, ny[i] = ex / len * orient;
        nc[i] = -(nx[i] * X[i] + ny[i] * Y[i]);
        const float ax = fabsf(nx[i]), ay = fabsf(ny[i]), sa = ax > ay ? ax : ay, sb = ax > ay ? ay : ax;
        sides.nx[i] = nx[i], sides.ny[i] = ny[i];
        sides.far[i] = (sa + sb) * 0.5f, sides.near[i] = (sa - sb) * 0.5f;
        sides.lin[i] = 1.0f / sa, sides.tri[i] = sb > 1e-6f ? 0.5f / (sa * sb) : 0.0f;
        sides.whole[i] = dgx_tq_whole(sa, sb);
    }
    /* the map from the screen to the region of the texture, u and v in texels: the inverse of the square onto the quad */
    /*
     * The corners of a parallelogram, each rounded to 1/16 px, are off one by
     * up to two sixteenths: such a quad is mapped as a parallelogram, the mean
     * of its opposite sides, each corner off by 1/32 px at most. The edges are
     * where the corners are.
     */
    const int  off_x = q->x[0] - q->x[1] + q->x[2] - q->x[3], off_y = q->y[0] - q->y[1] + q->y[2] - q->y[3];
    const bool parallelogram = off_x >= -2 && off_x <= 2 && off_y >= -2 && off_y <= 2;
    float      a, b, c, d, e, f, g = 0, h = 0;
    if (parallelogram) {
        a = (X[1] - X[0] + X[2] - X[3]) * 0.5f, b = (X[3] - X[0] + X[2] - X[1]) * 0.5f, c = (X[0] + X[1] + X[2] + X[3]) * 0.25f - (a + b) * 0.5f;
        d = (Y[1] - Y[0] + Y[2] - Y[3]) * 0.5f, e = (Y[3] - Y[0] + Y[2] - Y[1]) * 0.5f, f = (Y[0] + Y[1] + Y[2] + Y[3]) * 0.25f - (d + e) * 0.5f;
    } else {
        const float dx1 = X[1] - X[2], dx2 = X[3] - X[2], dx3 = X[0] - X[1] + X[2] - X[3];
        const float dy1 = Y[1] - Y[2], dy2 = Y[3] - Y[2], dy3 = Y[0] - Y[1] + Y[2] - Y[3];
        const float den = dx1 * dy2 - dx2 * dy1;
        if (den > -1e-9f && den < 1e-9f) return ESP_OK;
        g = (dx3 * dy2 - dx2 * dy3) / den, h = (dx1 * dy3 - dx3 * dy1) / den;
        a = X[1] - X[0] + g * X[1], b = X[3] - X[0] + h * X[3], c = X[0];
        d = Y[1] - Y[0] + g * Y[1], e = Y[3] - Y[0] + h * Y[3], f = Y[0];
    }
    /* the adjugate: s = (A x + B y + C) / (G x + H y + I), t likewise */
    const float A = e - f * h, B = c * h - b, C = b * f - c * e;
    const float D = f * g - d, E = a - c * g, F = c * d - a * f;
    const float G = d * h - e * g, H = b * g - a * h, I = a * e - b * d;
    if (I > -1e-12f && I < 1e-12f && parallelogram) return ESP_OK;

    dgx_tq_t t = {
        .dst     = scr,
        .q       = q,
        .texels  = ((dgx_vscreen_t *)texture)->v_array,
        .pitch   = indexed ? texture->width : texture->width * 2,
        .indexed = indexed,
        .read    = dgx_screen_is_readable(scr),
    };
    if (alpha_map) t.alpha_map = ((dgx_vscreen_t *)alpha_map)->v_array, t.alpha_pitch = alpha_map->width;
    if (dgx_vscreen_is_linear(scr)) t.memory = ((dgx_vscreen_t *)scr)->v_array, t.memory_pitch = scr->width * 2;
    const uint32_t row_bytes = (uint32_t)scr->width * 2u;
    /* a screen not in memory takes the runs from a row, written 16 bits at a time */
    t.row = scr->draw_buffer && scr->draw_buffer_len >= row_bytes && !((uintptr_t)scr->draw_buffer & 1u) ? (uint16_t *)scr->draw_buffer : NULL;
    uint16_t *own_row = NULL;
    if (!t.row && !t.memory) {
        t.row = own_row = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
        if (!own_row) {
            ESP_LOGE(TAG, "failed to allocate a %u-byte row", (unsigned)row_bytes);
            return ESP_ERR_NO_MEM;
        }
    }
    const int cl = dgx_clip_left(scr), cr = dgx_clip_right(scr), ct = dgx_clip_top(scr), cb = dgx_clip_bottom(scr);
    if (by0 < ct) by0 = ct;
    if (by1 > cb - 1) by1 = cb - 1;
    const int32_t tw16 = q->tw * 65536, th16 = q->th * 65536;
    /* a texture with no transparent texel goes inside the quad by dgx_tq_inside(); there a side is at least this far, in 1/256 px */
    const bool    opaque   = !indexed && !(q->flags & DGX_TEXTURE_KEY) && !alpha_map;
    const bool    bilinear = (q->flags & DGX_TEXTURE_BILINEAR) != 0;
    /* a side at least this far from the centre of a pixel, in 1/256 px, leaves the pixel whole */
    int32_t inside[4];
    for (int i = 0; i < 4; ++i) inside[i] = soft ? sides.whole[i] : 0;
    dgx_screen_batch_begin(scr);
    for (int y = by0; y <= by1; ++y) {
        const float yc = (float)(y - oy) + 0.5f;
        /* the pixels of this row the quad may touch: where its sides cross the band of the row, with the margin */
        float xa = 1e9f, xb = -1e9f;
        for (int i = 0; i < 4; ++i) {
            const int   j = (i + 1) & 3;
            const float ya = Y[i] < Y[j] ? Y[i] : Y[j], yb = Y[i] < Y[j] ? Y[j] : Y[i];
            const float lo = yc - 0.5f - margin, hi = yc + 0.5f + margin;
            if (yb < lo || ya > hi) continue;
            const float y0 = lo < ya ? ya : lo, y1 = hi > yb ? yb : hi;
            for (int k = 0; k < 2; ++k) {
                const float yy = k ? y1 : y0;
                const float xx = Y[j] == Y[i] ? (k ? X[j] : X[i]) : X[i] + (X[j] - X[i]) * (yy - Y[i]) / (Y[j] - Y[i]);
                if (xx < xa) xa = xx;
                if (xx > xb) xb = xx;
            }
        }
        if (xa > xb) continue;
        /* the first pixel the quad has in the row, xs, counts the steps along it, whatever the clip leaves of the row */
        const int xr = (int)floorf(xa - margin - 0.5f), xs = ox + xr;
        int       x0 = xs, x1 = ox + (int)ceilf(xb + margin - 0.5f);
        if (x0 < cl) x0 = cl;
        if (x1 > cr - 1) x1 = cr - 1;
        if (x0 > x1) continue;
        t.y = y, t.run_n = 0;
        const float xc   = (float)xr + 0.5f;
        const int   skip = x0 - xs;
        int32_t     dist[4], step[4];
        for (int i = 0; i < 4; ++i) {
            step[i] = (int32_t)lroundf(nx[i] * 256.0f);
            dist[i] = (int32_t)lroundf((nx[i] * xc + ny[i] * yc + nc[i]) * 256.0f) + skip * step[i];
            if (nc[i] > 1e8f) dist[i] = INT32_MAX / 2, step[i] = 0;
        }
        /* the map at xs; for a parallelogram, stepped on to x0 */
        dgx_tq_map_t map = {.ns = A * xc + B * yc + C, .nt = D * xc + E * yc + F, .nw = G * xc + H * yc + I, .dns = A, .dnt = D, .dnw = G,
                            .ks = (float)tw16, .kt = (float)th16, .k = skip};
        if (parallelogram) {
            map.ds = (int32_t)lroundf(A / I * (float)tw16), map.dv = (int32_t)lroundf(D / I * (float)th16);
            map.s  = (int32_t)lroundf(map.ns / I * (float)tw16) + skip * map.ds, map.v = (int32_t)lroundf(map.nt / I * (float)th16) + skip * map.dv;
        }
        /* the pixels x0 + in_lo .. x0 + in_hi have every side at least `inside` away: the quad covers them whole */
        int in_lo = 0, in_hi = opaque || t.memory ? x1 - x0 : -1;
        for (int i = 0; i < 4 && in_lo <= in_hi; ++i) {
            if (dist[i] >= inside[i]) {
                if (step[i] < 0) {
                    const int k = (dist[i] - inside[i]) / -step[i];
                    if (k < in_hi) in_hi = k;
                }
            } else if (step[i] > 0) {
                const int k = (inside[i] - dist[i] + step[i] - 1) / step[i];
                if (k > in_lo) in_lo = k;
            } else {
                in_hi = -1;
            }
        }
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 + in_lo && in_lo <= in_hi) {
                const int n   = in_hi - in_lo + 1;
                uint16_t *mem = t.memory ? (uint16_t *)(t.memory + y * t.memory_pitch) + x : NULL;
                if (t.alpha_map) {
                    dgx_tq_inside_mapped(&t, mem, n, &map, !parallelogram);
                } else if (!opaque) {
                    dgx_tq_inside_mixed(&t, mem, n, &map, !parallelogram);
                } else if (mem) {
                    dgx_tq_inside(&t, mem, n, &map, bilinear, !parallelogram);
                } else {
                    dgx_tq_inside(&t, dgx_tq_at(&t, x), n, &map, bilinear, !parallelogram);
                    t.run_n += n;
                }
                for (int i = 0; i < 4; ++i) dist[i] += n * step[i];
                x += n - 1;
                continue;
            }
            uint32_t cover;
            if (soft) {
                cover = dgx_tq_cover(&sides, dist);
            } else {
                int32_t m = dist[0];
                for (int i = 1; i < 4; ++i)
                    if (dist[i] < m) m = dist[i];
                cover = m >= 0 ? 32u : 0u;
            }
            if (cover) {
                if (!parallelogram) dgx_tq_project(&map, map.k, &map.s, &map.v);
                uint32_t       alpha;
                const uint16_t color = dgx_tq_sample(&t, map.s, map.v, &alpha);
                const uint32_t w     = alpha == 255u ? cover : (cover * alpha + 127u) / 255u;
                if (w) dgx_tq_put(&t, x, color, w);
                else dgx_tq_flush(&t);
            } else {
                dgx_tq_flush(&t);
            }
            for (int i = 0; i < 4; ++i) dist[i] += step[i];
            if (parallelogram) map.s += map.ds, map.v += map.dv;
            else ++map.k;
        }
        dgx_tq_flush(&t);
    }
    if (own_row) {
        if (scr->wait) scr->wait(scr);
        free(own_row);
    }
    dgx_screen_touch(scr, bx0, bx1, by0, by1);
    dgx_screen_batch_end(scr);
    return ESP_OK;
}

#else

/* the texture is a virtual screen, and there are none */
esp_err_t dgx_draw_texture_quad_with(dgx_screen_t *scr, const dgx_texture_quad_t *q)
{
    (void)scr, (void)q;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
