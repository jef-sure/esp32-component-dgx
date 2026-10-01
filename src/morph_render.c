#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_font.h"
#include "dgx_morph_render.h"
#include "drivers/vscreen.h"

static inline uint32_t dgx_morph_div255(uint32_t n)
{
    return (n + 1 + (n >> 8)) >> 8;
}

static inline float dgx_morph_smoothstep3(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

static bool dgx_morph_color_bits_ok(uint8_t color_bits)
{
    return color_bits == 16 || color_bits == 18 || color_bits == 24;
}

static uint32_t dgx_morph_gray(uint8_t color_bits, uint8_t v)
{
    switch (color_bits) {
    case 16: return DGX_RGB_16(v, v, v);
    case 18: return DGX_RGB_18(v, v, v);
    default: return DGX_RGB_24(v, v, v);
    }
}

/* 16-bit pixels are 2 bytes, 18- and 24-bit pixels are 3 bytes */
static inline uint8_t *dgx_morph_put_color(uint8_t color_bits, uint8_t *lp, uint32_t color)
{
    return color_bits == 16 ? dgx_fill_buf_value_16(lp, 0, color) : dgx_fill_buf_value_24(lp, 0, color);
}

/* ------------------------------------------------------------------ */
/* Glow                                                                 */
/* ------------------------------------------------------------------ */

struct dgx_morph_glow {
    dgx_screen_t *vscreen;
    int width, height;
    int radius;
    int *rlut;          /* falloff by squared distance */
    int *xcell_offset;  /* max |dx| per |dy| */
    uint8_t *glow_prev;
    uint8_t *glow_next;
    uint8_t color_bits;
    uint32_t lut[256];
    uint16_t lut16_swapped[256]; /* 16-bit fast path: one store per pixel */
    bool has_frame;
};

dgx_morph_glow_t *dgx_morph_glow_create(int width, int height, int cell_width, uint8_t color_bits)
{
    if (width <= 0 || height <= 0 || cell_width <= 0 || !dgx_morph_color_bits_ok(color_bits)) return NULL;
    dgx_morph_glow_t *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->width = width;
    g->height = height;
    g->color_bits = color_bits;
    g->radius = cell_width / 2 + cell_width / 4;
    if (g->radius < 2) g->radius = 2;
    int rlut_limit = g->radius * g->radius;
    size_t pixels = (size_t)width * (size_t)height;

    g->vscreen = dgx_vscreen_init(width, height, color_bits, DgxScreenRGB);
    g->rlut = malloc((size_t)rlut_limit * sizeof(*g->rlut));
    g->xcell_offset = malloc((size_t)(g->radius + 1) * sizeof(*g->xcell_offset));
    g->glow_prev = calloc(pixels, 1);
    g->glow_next = calloc(pixels, 1);
    if (!g->vscreen || !g->rlut || !g->xcell_offset || !g->glow_prev || !g->glow_next) {
        dgx_morph_glow_destroy(&g);
        return NULL;
    }
    for (int i = 0; i < rlut_limit; ++i) {
        g->rlut[i] = (int)(255.0f * (1.0f - dgx_morph_smoothstep3((float)i / (float)rlut_limit)));
    }
    for (int dy = 0; dy <= g->radius; ++dy) {
        int rem = rlut_limit - 1 - dy * dy;
        g->xcell_offset[dy] = rem >= 0 ? (int)sqrtf((float)rem) : -1;
    }
    for (int i = 0; i < 256; ++i) {
        g->lut[i] = dgx_morph_gray(color_bits, (uint8_t)i);
    }
    dgx_morph_glow_set_lut(g, g->lut);
    return g;
}

void dgx_morph_glow_set_lut(dgx_morph_glow_t *glow, const uint32_t *lut)
{
    if (!glow || !lut) return;
    memmove(glow->lut, lut, sizeof(glow->lut));
    for (int i = 0; i < 256; ++i) {
        uint16_t c = (uint16_t)glow->lut[i];
        glow->lut16_swapped[i] = (uint16_t)((c >> 8) | (c << 8));
    }
}

void dgx_morph_glow_destroy(dgx_morph_glow_t **glow)
{
    if (glow && *glow) {
        dgx_screen_destroy(&(*glow)->vscreen);
        free((*glow)->rlut);
        free((*glow)->xcell_offset);
        free((*glow)->glow_prev);
        free((*glow)->glow_next);
        free(*glow);
        *glow = NULL;
    }
}

void dgx_morph_glow_reset(dgx_morph_glow_t *glow)
{
    if (!glow) return;
    memset(glow->glow_prev, 0, (size_t)glow->width * glow->height);
    memset(glow->glow_next, 0, (size_t)glow->width * glow->height);
    glow->has_frame = false;
}

static inline void glow_add(uint8_t *row, int x, int contribution)
{
    int v = row[x] + contribution;
    row[x] = v > 255 ? 255 : (uint8_t)v;
}

/* dx-symmetric writes with incremental squared distance */
static void glow_collect(dgx_morph_glow_t *g, int px, int py, uint8_t intensity)
{
    if (intensity == 0) return;
    int top = py - g->radius;
    int bottom = py + g->radius;
    if (top < 0) top = 0;
    if (bottom > g->height - 1) bottom = g->height - 1;

    for (int y = top; y <= bottom; ++y) {
        int dy = y - py;
        int max_dx = g->xcell_offset[dy < 0 ? -dy : dy];
        if (max_dx < 0 || px + max_dx < 0 || px - max_dx >= g->width) continue;
        int dy2 = dy * dy;
        uint8_t *row = g->glow_next + (size_t)y * (size_t)g->width;
        if (px >= 0 && px < g->width) {
            glow_add(row, px, (int)dgx_morph_div255((uint32_t)g->rlut[dy2] * intensity));
        }
        int dist = dy2;
        for (int dx = 1; dx <= max_dx; ++dx) {
            dist += (dx << 1) - 1;
            int xl = px - dx;
            int xr = px + dx;
            int c = (int)dgx_morph_div255((uint32_t)g->rlut[dist] * intensity);
            if (xl >= 0 && xl < g->width) glow_add(row, xl, c);
            if (xr >= 0 && xr < g->width) glow_add(row, xr, c);
        }
    }
}

void dgx_morph_glow_dot(void *glow, const dgx_point_2d_t *point, uint8_t intensity)
{
    if (glow && point) glow_collect(glow, point->x, point->y, intensity);
}

void dgx_morph_glow_present(dgx_morph_glow_t *glow, float t, dgx_screen_t *screen, int x, int y)
{
    if (!glow) return;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint32_t blend = glow->has_frame ? (uint32_t)(256.0f * dgx_morph_smoothstep3(t)) : 256u;
    uint32_t inv = 256u - blend;
    size_t pixels = (size_t)glow->width * glow->height;
    uint8_t *out = ((dgx_vscreen_t *)glow->vscreen)->v_array;
    uint8_t *prev = glow->glow_prev;
    uint8_t *next = glow->glow_next;
    if (glow->color_bits == 16) {
        uint16_t *out16 = (uint16_t *)out;
        for (size_t i = 0; i < pixels; ++i) {
            uint8_t v = (uint8_t)((next[i] * blend + prev[i] * inv) >> 8);
            out16[i] = glow->lut16_swapped[v];
            prev[i] = v;
            next[i] = 0;
        }
    } else {
        for (size_t i = 0; i < pixels; ++i) {
            uint8_t v = (uint8_t)((next[i] * blend + prev[i] * inv) >> 8);
            out = dgx_fill_buf_value_24(out, 0, glow->lut[v]);
            prev[i] = v;
            next[i] = 0;
        }
    }
    glow->has_frame = true;
    if (screen) dgx_vscreen_to_screen(screen, x, y, glow->vscreen);
}

/* ------------------------------------------------------------------ */
/* Sprite                                                               */
/* ------------------------------------------------------------------ */

struct dgx_morph_sprite {
    dgx_screen_t *vpoint;  /* 8-bit dot sprite */
    dgx_screen_t *target;
    bool has_lut;
    uint32_t lut[256];
};

dgx_morph_sprite_t *dgx_morph_sprite_create(int diameter)
{
    if (diameter <= 0) return NULL;
    dgx_morph_sprite_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->vpoint = dgx_vscreen_init(diameter, diameter, 8, DgxScreenRGB);
    if (!s->vpoint) {
        free(s);
        return NULL;
    }
    dgx_font_make_point8(s->vpoint);
    return s;
}

void dgx_morph_sprite_destroy(dgx_morph_sprite_t **sprite)
{
    if (sprite && *sprite) {
        dgx_screen_destroy(&(*sprite)->vpoint);
        free(*sprite);
        *sprite = NULL;
    }
}

void dgx_morph_sprite_set_lut(dgx_morph_sprite_t *sprite, const uint32_t *lut)
{
    if (sprite && lut) {
        memcpy(sprite->lut, lut, sizeof(sprite->lut));
        sprite->has_lut = true;
    }
}

void dgx_morph_sprite_set_target(dgx_morph_sprite_t *sprite, dgx_screen_t *vscreen)
{
    if (!sprite) return;
    bool ok = vscreen && dgx_morph_color_bits_ok(vscreen->color_bits) &&
              (vscreen->screen_subtype == DgxVirtualScreen || vscreen->screen_subtype == DgxVirtualBackScreen);
    sprite->target = ok ? vscreen : NULL;
}

void dgx_morph_sprite_dot(void *sprite, const dgx_point_2d_t *point, uint8_t intensity)
{
    dgx_morph_sprite_t *s = sprite;
    if (!s || !s->target || !point || intensity == 0) return;
    int d = s->vpoint->width;
    int x0 = point->x - d / 2;
    int y0 = point->y - d / 2;
    int dw = s->target->width;
    int dh = s->target->height;
    const uint8_t *src = ((dgx_vscreen_t *)s->vpoint)->v_array;
    uint8_t *dst = ((dgx_vscreen_t *)s->target)->v_array;
    uint8_t bits = s->target->color_bits;
    size_t bpp = bits == 16 ? 2 : 3;
    for (int sy = 0; sy < d; ++sy) {
        int ty = y0 + sy;
        if (ty < 0 || ty >= dh) continue;
        for (int sx = 0; sx < d; ++sx) {
            int tx = x0 + sx;
            if (tx < 0 || tx >= dw) continue;
            uint32_t idx = dgx_morph_div255((uint32_t)src[sy * d + sx] * intensity);
            if (idx == 0) continue;
            uint32_t color = s->has_lut ? s->lut[idx] : dgx_morph_gray(bits, (uint8_t)idx);
            dgx_morph_put_color(bits, dst + bpp * ((size_t)ty * dw + tx), color);
        }
    }
    dgx_screen_touch(s->target, x0, x0 + d - 1, y0, y0 + d - 1);
}
