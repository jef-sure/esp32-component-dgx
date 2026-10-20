#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_font.h"
#include "dgx_morph_render.h"
#include "drivers/vscreen.h"
#include "esp_log.h"

static const char TAG[] = "DGX MORPH";

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

/* a brightness map read four pixels at a time */
typedef uint32_t __attribute__((may_alias)) dgx_glow_word_t;
/* a 16-bit pixel in the buffer a screen sends from */
typedef uint16_t __attribute__((may_alias)) dgx_glow_color16_t;

struct dgx_morph_glow {
    int width, height;
    int radius;
    int *rlut;          /* falloff by squared distance */
    int *xcell_offset;  /* max |dx| per |dy| */
    uint8_t *glow_prev; /* the frame on the screen, kept as the phosphor */
    uint8_t *glow_next;
    uint8_t color_bits;
    uint32_t lut[256];
    uint16_t lut16_swapped[256]; /* 16-bit fast path: one store per pixel */
    bool has_frame;
    dgx_morph_glow_filter_t filter; /* gets a copy of every frame right before it is shown */
    void *filter_data;
    uint8_t *shown;                 /* that copy */
    uint8_t *rows;                  /* four rows instead of it for dgx_morph_glow_blur() in one pass: three blurred across, one to show */
    int row_of[3];                  /* which rows of the frame the three are */
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

    g->rlut = malloc((size_t)rlut_limit * sizeof(*g->rlut));
    g->xcell_offset = malloc((size_t)(g->radius + 1) * sizeof(*g->xcell_offset));
    g->glow_prev = calloc(pixels, 1);
    g->glow_next = calloc(pixels, 1);
    if (!g->rlut || !g->xcell_offset || !g->glow_prev || !g->glow_next) {
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
        free((*glow)->rlut);
        free((*glow)->xcell_offset);
        free((*glow)->glow_prev);
        free((*glow)->glow_next);
        free((*glow)->shown);
        free((*glow)->rows);
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

/* every pixel with half of its neighbours on each side: across in place, then down with the row above kept aside */
void dgx_morph_glow_blur(void *user_data, uint8_t *brightness, int width, int height)
{
    int passes = user_data ? *(const int *)user_data : 1;
    if (!brightness || width < 1 || height < 1 || passes < 1) return;
    uint8_t *above = malloc((size_t)width);
    if (!above) return;
    for (; passes > 0; --passes) {
        for (int y = 0; y < height; ++y) {
            uint8_t *row = brightness + (size_t)y * width;
            int      left = row[0];
            for (int x = 0; x < width; ++x) {
                int here = row[x], right = x + 1 < width ? row[x + 1] : here;
                row[x] = (uint8_t)((left + 2 * here + right + 2) >> 2);
                left = here;
            }
        }
        memcpy(above, brightness, (size_t)width);
        for (int y = 0; y < height; ++y) {
            uint8_t       *row = brightness + (size_t)y * width;
            const uint8_t *below = y + 1 < height ? row + width : row;
            for (int x = 0; x < width; ++x) {
                int here = row[x];
                row[x] = (uint8_t)((above[x] + 2 * here + below[x] + 2) >> 2);
                above[x] = (uint8_t)here;
            }
        }
    }
    free(above);
}

/* how many times dgx_morph_glow_blur() goes over a frame with this user_data */
static int glow_blur_passes(const void *user_data)
{
    return user_data ? *(const int *)user_data : 1;
}

/*
 * What a filter needs besides the glow itself. dgx_morph_glow_blur() in one
 * pass looks one row up and one down, so its frame is made row by row on the
 * way to the screen and four rows are enough; any other filter gets a copy
 * of the whole frame.
 */
static bool glow_filter_room(dgx_morph_glow_t *glow)
{
    if (glow->filter == dgx_morph_glow_blur && glow_blur_passes(glow->filter_data) < 1) return true;
    if (glow->filter == dgx_morph_glow_blur && glow_blur_passes(glow->filter_data) == 1) {
        if (!glow->rows) glow->rows = malloc((size_t)glow->width * 4);
        return glow->rows != NULL;
    }
    if (!glow->shown) glow->shown = malloc((size_t)glow->width * glow->height);
    return glow->shown != NULL;
}

bool dgx_morph_glow_set_filter(dgx_morph_glow_t *glow, dgx_morph_glow_filter_t filter, void *user_data)
{
    if (!glow) return false;
    free(glow->shown);
    free(glow->rows);
    glow->shown = glow->rows = NULL;
    glow->filter = filter;
    glow->filter_data = user_data;
    if (!filter) return true;
    if (!glow_filter_room(glow)) glow->filter = NULL;
    return glow->filter != NULL;
}

/* a row of the frame as dgx_morph_glow_blur() in one pass leaves it; rows asked for in turn are each blurred across once */
static const uint8_t *glow_blurred_row(dgx_morph_glow_t *g, int y)
{
    int            w = g->width;
    const uint8_t *across[3];
    for (int k = 0; k < 3; ++k) {
        int at = y + k - 1;
        if (at < 0) at = 0;
        if (at > g->height - 1) at = g->height - 1;
        uint8_t *row = g->rows + (size_t)(at % 3) * w;
        if (g->row_of[at % 3] != at) {
            const uint8_t *frame = g->glow_prev + (size_t)at * w;
            int            left = frame[0];
            for (int x = 0; x < w; ++x) {
                int here = frame[x], right = x + 1 < w ? frame[x + 1] : here;
                row[x] = (uint8_t)((left + 2 * here + right + 2) >> 2);
                left = here;
            }
            g->row_of[at % 3] = at;
        }
        across[k] = row;
    }
    uint8_t *shown = g->rows + (size_t)3 * w;
    for (int x = 0; x < w; ++x) shown[x] = (uint8_t)((across[0][x] + 2 * across[1][x] + across[2][x] + 2) >> 2);
    return shown;
}

/* brightness into colors as a screen keeps or sends them */
static inline void glow_colors(const dgx_morph_glow_t *g, uint8_t *to, const uint8_t *row, int count)
{
    if (g->color_bits == 16) {
        /* most of a frame is black: where the row is word-aligned, four black pixels are one look */
        dgx_glow_color16_t *out = (dgx_glow_color16_t *)to;
        const uint16_t      black = g->lut16_swapped[0];
        int                 i = 0;
        for (; i < count && ((uintptr_t)(row + i) & 3u); ++i) out[i] = g->lut16_swapped[row[i]];
        for (; i + 4 <= count; i += 4) {
            if (*(const dgx_glow_word_t *)(row + i) == 0) {
                out[i] = out[i + 1] = out[i + 2] = out[i + 3] = black;
                continue;
            }
            out[i] = g->lut16_swapped[row[i]];
            out[i + 1] = g->lut16_swapped[row[i + 1]];
            out[i + 2] = g->lut16_swapped[row[i + 2]];
            out[i + 3] = g->lut16_swapped[row[i + 3]];
        }
        for (; i < count; ++i) out[i] = g->lut16_swapped[row[i]];
    } else {
        for (int i = 0; i < count; ++i) to = dgx_fill_buf_value_24(to, 0, g->lut[row[i]]);
    }
}

/*
 * The frame goes to the screen as colors straight from its brightness, so no
 * frame of colors is kept: a pixel of the glow is a byte of the phosphor and
 * a byte of the frame being collected. A display gets it row by row through
 * the buffer it sends from, a virtual screen right into its pixels. `map` is
 * the brightness to show; NULL is the phosphor blurred on the way.
 */
static void glow_send(dgx_morph_glow_t *g, dgx_screen_t *scr, int x_dst, int y_dst, const uint8_t *map)
{
    if (scr->color_bits != g->color_bits) {
        ESP_LOGE(TAG, "glow color bits %d != screen %d", g->color_bits, scr->color_bits);
        return;
    }
    /* the part of the frame that is on the screen */
    int left = x_dst < 0 ? -x_dst : 0, top = y_dst < 0 ? -y_dst : 0;
    int right = scr->width - x_dst < g->width ? scr->width - x_dst : g->width;
    int bottom = scr->height - y_dst < g->height ? scr->height - y_dst : g->height;
    if (left >= right || top >= bottom) return;
    int    width = right - left;
    size_t pixel_bytes = g->color_bits == 16 ? 2 : 3;
    g->row_of[0] = g->row_of[1] = g->row_of[2] = -1;
    dgx_screen_progress_up(scr);
    if (dgx_vscreen_is_linear(scr)) {
        uint8_t *pixels = ((dgx_vscreen_t *)scr)->v_array;
        for (int y = top; y < bottom; ++y) {
            const uint8_t *row = (map ? map + (size_t)y * g->width : glow_blurred_row(g, y)) + left;
            glow_colors(g, pixels + pixel_bytes * ((size_t)(y_dst + y) * scr->width + x_dst + left), row, width);
        }
    } else if (scr->set_area && scr->write_area) {
        /* a screen without a buffer of its own gets a scanline for the duration of the call */
        uint8_t *buffer = scr->draw_buffer, *own = NULL;
        size_t   buffer_pixels = scr->draw_buffer_len / pixel_bytes, filled = 0;
        if (!buffer || !buffer_pixels) {
            buffer = own = malloc((size_t)width * pixel_bytes);
            buffer_pixels = (size_t)width;
        }
        if (buffer) {
            scr->set_area(scr, (uint16_t)(x_dst + left), (uint16_t)(x_dst + right - 1), (uint16_t)(y_dst + top), (uint16_t)(y_dst + bottom - 1));
            /* what was sent before may still be on its way to the panel from this buffer */
            if (scr->wait_buffer) scr->wait_buffer(scr);
            for (int y = top; y < bottom; ++y) {
                const uint8_t *row = (map ? map + (size_t)y * g->width : glow_blurred_row(g, y)) + left;
                for (int todo = width; todo > 0;) {
                    if (filled == buffer_pixels) {
                        scr->write_area(scr, buffer, (uint32_t)(8u * filled * pixel_bytes));
                        if (scr->wait_buffer) scr->wait_buffer(scr);
                        filled = 0;
                    }
                    int count = buffer_pixels - filled < (size_t)todo ? (int)(buffer_pixels - filled) : todo;
                    glow_colors(g, buffer + filled * pixel_bytes, row, count);
                    filled += (size_t)count;
                    row += count;
                    todo -= count;
                }
            }
            if (filled) {
                scr->write_area(scr, buffer, (uint32_t)(8u * filled * pixel_bytes));
                if (scr->wait_buffer) scr->wait_buffer(scr);
            }
            free(own);
        }
    }
    dgx_screen_touch(scr, x_dst + left, x_dst + right - 1, y_dst + top, y_dst + bottom - 1);
    dgx_screen_progress_down(scr);
}

void dgx_morph_glow_present(dgx_morph_glow_t *glow, float t, dgx_screen_t *screen, int x, int y)
{
    if (!glow) return;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    uint32_t blend = glow->has_frame ? (uint32_t)(256.0f * dgx_morph_smoothstep3(t)) : 256u;
    uint32_t inv = 256u - blend;
    size_t pixels = (size_t)glow->width * glow->height;
    uint8_t *prev = glow->glow_prev;
    uint8_t *next = glow->glow_next;
    /*
     * Most of a glow frame is black, so the brightness maps are read a word
     * at a time (they come from calloc and are word-aligned) and a black word
     * is passed by.
     */
    const dgx_glow_word_t *next4 = (const dgx_glow_word_t *)next;
    const dgx_glow_word_t *prev4 = (const dgx_glow_word_t *)prev;
    size_t                 words = pixels / 4;
    for (size_t w = 0; w < words; ++w) {
        if ((next4[w] | prev4[w]) == 0) continue;
        for (size_t i = w * 4, end = i + 4; i < end; ++i) {
            prev[i] = (uint8_t)((next[i] * blend + prev[i] * inv) >> 8);
            next[i] = 0;
        }
    }
    for (size_t i = words * 4; i < pixels; ++i) {
        prev[i] = (uint8_t)((next[i] * blend + prev[i] * inv) >> 8);
        next[i] = 0;
    }
    glow->has_frame = true;

    /*
     * A filter never touches the phosphor: it works on a copy of the frame,
     * or, the blur in one pass, on rows made of it on the way to the screen.
     * Its user_data is read anew every frame, so what it needs may change;
     * with no memory for that the frame is shown unfiltered.
     */
    const uint8_t *map = prev;
    if (glow->filter && glow_filter_room(glow)) {
        if (glow->filter != dgx_morph_glow_blur || glow_blur_passes(glow->filter_data) > 1) {
            memcpy(glow->shown, prev, pixels);
            glow->filter(glow->filter_data, glow->shown, glow->width, glow->height);
            map = glow->shown;
        } else if (glow_blur_passes(glow->filter_data) == 1) {
            map = NULL;
        }
    }
    if (screen) glow_send(glow, screen, x, y, map);
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
    bool ok = vscreen && dgx_morph_color_bits_ok(vscreen->color_bits) && dgx_vscreen_is_linear(vscreen);
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
