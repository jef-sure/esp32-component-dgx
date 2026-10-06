#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"
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
    if (y < 0 || y >= dst->height) return;
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
    if (x1 < 0 || x0 >= dst->width) return;

    /* sample at pixel centers: the span covers the texture from a's coordinate to b's */
    int     pixels = x1 - x0 + 1;
    int32_t du     = (int32_t)(((int64_t)b->u - a->u) / pixels);
    int32_t dv     = (int32_t)(((int64_t)b->v - a->v) / pixels);
    int32_t u      = a->u + du / 2;
    int32_t v      = a->v + dv / 2;
    if (x0 < 0) {
        u  = (int32_t)(u + (int64_t)du * -x0);
        v  = (int32_t)(v + (int64_t)dv * -x0);
        x0 = 0;
    }
    if (x1 >= dst->width) x1 = dst->width - 1;
    int count = x1 - x0 + 1;

    /* the previous scanline may still be on its way to the panel from this buffer */
    if (dst->wait_buffer) dst->wait_buffer(dst);
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
    ctx->win_y = y + 1;
    dst->write_area(dst, ctx->row, (uint32_t)count * ctx->pixel_bits);
}

void dgx_draw_texture_quad(dgx_screen_t *scr, const dgx_point_2d_t quad[4], dgx_screen_t *texture, int tx, int ty, int tw, int th)
{
    if (!scr || !quad || !texture || tw <= 0 || th <= 0) return;
    if (!scr->set_area || !scr->write_area || !texture->get_pixel) return;
    if (scr->color_bits != texture->color_bits) {
        ESP_LOGE(TAG, "texture color bits %d != screen %d", texture->color_bits, scr->color_bits);
        return;
    }
    if (dgx_color_points_to_bytes(scr->color_bits, 1) == 0) return; /* unsupported depth */
    if (tx < 0 || ty < 0 || tx + tw > texture->width || ty + th > texture->height) {
        ESP_LOGE(TAG, "texture region %d,%d %dx%d is outside the %dx%d texture", tx, ty, tw, th, texture->width, texture->height);
        return;
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
    if (y_end < 0 || y_start >= scr->height || x_max < 0 || x_min >= scr->width) return;
    /* 16.16 fixed point: a slope or a texture coordinate of 32768 and more does not fit */
    if (x_max - x_min > INT16_MAX || tx + tw > INT16_MAX || ty + th > INT16_MAX) {
        ESP_LOGE(TAG, "quad or texture region is too large for 16.16 fixed point");
        return;
    }

    dgx_texture_ctx_t ctx = {
        .dst        = scr,
        .src        = texture,
        .left       = tx,
        .top        = ty,
        .right      = tx + tw - 1,
        .bottom     = ty + th - 1,
        .pixel_bits = scr->color_bits == 18 ? 24u : scr->color_bits,
        .y_last     = y_end < scr->height ? y_end : scr->height - 1,
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
            return;
        }
    }

#define DGX_QUAD_NEXT(i) (((i) + 1) & 3)
#define DGX_QUAD_PREV(i) (((i) + 3) & 3)
    dgx_texture_edge_t e_prev, e_next;
    dgx_screen_progress_up(scr);
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
            dgx_texture_edge_step(&e_prev);
            dgx_texture_edge_step(&e_next);
            /* past a vertex the next edge of the chain takes over from the following scanline */
            if (y == vertices[v_prev].p.y && y < y_end) {
                from_prev = v_prev;
                v_prev    = DGX_QUAD_PREV(v_prev);
                dgx_texture_edge_init(&e_prev, &vertices[from_prev], &vertices[v_prev], y + 1, y_start, y_end);
            }
            if (y == vertices[v_next].p.y && y < y_end) {
                from_next = v_next;
                v_next    = DGX_QUAD_NEXT(v_next);
                dgx_texture_edge_init(&e_next, &vertices[from_next], &vertices[v_next], y + 1, y_start, y_end);
            }
        }
    }
#undef DGX_QUAD_NEXT
#undef DGX_QUAD_PREV
    if (own_row) {
        if (scr->wait_buffer) scr->wait_buffer(scr);
        free(own_row);
    }
    dgx_screen_touch(scr, x_min, x_max, y_start, y_end);
    dgx_screen_progress_down(scr);
}

void dgx_draw_texture_rect(dgx_screen_t *scr, int x, int y, int w, int h, dgx_screen_t *texture, int tx, int ty, int tw, int th)
{
    if (w <= 0 || h <= 0) return;
    const dgx_point_2d_t quad[4] = {
        {(int16_t)x,           (int16_t)y          },
        {(int16_t)(x + w - 1), (int16_t)y          },
        {(int16_t)(x + w - 1), (int16_t)(y + h - 1)},
        {(int16_t)x,           (int16_t)(y + h - 1)},
    };
    dgx_draw_texture_quad(scr, quad, texture, tx, ty, tw, th);
}
