#include <stdlib.h>
#include <string.h>

#include "dgx_bitmap.h"
#include "dgx_morph_sources.h"

typedef struct {
    dgx_bit_matrix_t *matrix;
    int x0;
    int y0;
} dgx_morph_glyph_ctx_t;

static bool dgx_morph_glyph_set(void *user_data, int x, int y)
{
    dgx_morph_glyph_ctx_t *ctx = user_data;
    dgx_matrix_set_point(ctx->matrix, ctx->x0 + x, ctx->y0 + y, true);
    return true;
}

/* Fonts generated before yBottomMax/xOffsetLowest/xRightMax fall back to a glyph scan. */
static bool dgx_morph_font_box(const dgx_font_t *font, int *left, int *top, int *right, int *bottom)
{
    if (font->xRightMax > font->xOffsetLowest && font->yBottomMax > font->yOffsetLowest) {
        *left = font->xOffsetLowest;
        *top = font->yOffsetLowest;
        *right = font->xRightMax;
        *bottom = font->yBottomMax;
        return true;
    }
    bool any = false;
    for (const glyph_array_t *r = font->glyph_ranges; r && r->number; ++r) {
        for (int i = 0; i < r->number; ++i) {
            const glyph_t *g = &r->glyphs[i];
            if (g->width <= 0 || g->height <= 0) continue;
            int l = g->xOffset, t = g->yOffset;
            int rr = l + g->width, b = t + g->height;
            if (!any || l < *left) *left = l;
            if (!any || t < *top) *top = t;
            if (!any || rr > *right) *right = rr;
            if (!any || b > *bottom) *bottom = b;
            any = true;
        }
    }
    return any;
}

dgx_bit_matrix_t *dgx_morph_glyph_matrix(dgx_font_t *font, uint32_t code_point)
{
    if (!font) return NULL;
    int left, top, right, bottom;
    if (!dgx_morph_font_box(font, &left, &top, &right, &bottom)) return NULL;
    dgx_bit_matrix_t *m = dgx_matrix_init((uint16_t)(right - left), (uint16_t)(bottom - top));
    if (!m) return NULL;

    int16_t x_advance;
    const glyph_t *g = dgx_font_find_glyph(code_point, font, &x_advance);
    if (!g) return m;
    dgx_morph_glyph_ctx_t ctx = {
        .matrix = m,
        .x0 = g->xOffset - left,
        .y0 = g->yOffset - top,
    };
    if (font->f_type == DGX_FONT_DOTS) {
        for (size_t i = 0; i < g->number_of_dots; ++i) {
            dgx_morph_glyph_set(&ctx, g->dots[i].x, g->dots[i].y);
        }
    } else if (g->bitmap) {
        dgx_bw_bitmap_t bmap = dgx_bw_bitmap_make_of(
            (uint8_t *)g->bitmap, g->width, g->height, font->f_type == DGX_FONT_BITMAP_STREAM);
        dgx_bw_bitmap_foreach_set(&bmap, dgx_morph_glyph_set, &ctx);
    }
    return m;
}

dgx_bit_matrix_t *dgx_matrix_from_bw_bitmap(dgx_bw_bitmap_t *bmap)
{
    if (!bmap || !bmap->bitmap || bmap->width <= 0 || bmap->height <= 0 ||
        bmap->width > UINT16_MAX || bmap->height > UINT16_MAX) {
        return NULL;
    }
    dgx_bit_matrix_t *m = dgx_matrix_init((uint16_t)bmap->width, (uint16_t)bmap->height);
    if (!m) return NULL;
    dgx_morph_glyph_ctx_t ctx = { .matrix = m, .x0 = 0, .y0 = 0 };
    dgx_bw_bitmap_foreach_set(bmap, dgx_morph_glyph_set, &ctx);
    return m;
}

size_t dgx_morph_text_length(const char *utf8)
{
    if (!utf8) return 0;
    size_t len = strlen(utf8), idx = 0, count = 0;
    while (idx < len) {
        decodeUTF8next(utf8, &idx);
        ++count;
    }
    return count;
}

/* Returns ' ' past the end of the string. */
static uint32_t dgx_morph_text_next(const char *utf8, size_t len, size_t *idx)
{
    return utf8 && *idx < len ? decodeUTF8next(utf8, idx) : ' ';
}

dgx_morph_text_t *dgx_morph_text_create(
    dgx_font_t              *font,
    const char              *from,
    const char              *to,
    size_t                   length,
    dgx_morph_sources_func_t sources,
    void                    *user_data)
{
    if (!font) return NULL;
    if (!length) {
        size_t a = dgx_morph_text_length(from), b = dgx_morph_text_length(to);
        length = a > b ? a : b;
    }
    dgx_morph_text_t *text = calloc(1, sizeof(*text) + length * sizeof(dgx_morph_t *));
    if (!text) return NULL;
    text->length = length;
    text->letters = (dgx_morph_t **)(text + 1);

    size_t from_len = from ? strlen(from) : 0, to_len = to ? strlen(to) : 0;
    size_t from_idx = 0, to_idx = 0;
    for (size_t i = 0; i < length; ++i) {
        uint32_t f = dgx_morph_text_next(from, from_len, &from_idx);
        uint32_t t = dgx_morph_text_next(to, to_len, &to_idx);
        dgx_bit_matrix_t *fm = dgx_morph_glyph_matrix(font, f);
        dgx_bit_matrix_t *tm = dgx_morph_glyph_matrix(font, t);
        if (fm && tm) {
            if (!dgx_matrix_equals(fm, tm)) text->changed = i + 1;
            text->width = fm->width;
            text->height = fm->height;
            text->letters[i] = dgx_morph_create(fm, tm, sources, user_data);
        }
        dgx_matrix_destroy(&fm);
        dgx_matrix_destroy(&tm);
        if (!text->letters[i]) {
            dgx_morph_text_destroy(&text);
            return NULL;
        }
    }
    return text;
}

void dgx_morph_text_destroy(dgx_morph_text_t **text)
{
    if (!text || !*text) return;
    for (size_t i = 0; i < (*text)->length; ++i) {
        dgx_morph_destroy(&(*text)->letters[i]);
    }
    free(*text);
    *text = NULL;
}

int64_t dgx_morph_text_duration_us(const dgx_morph_text_t *text, int64_t duration_us, int64_t stagger_us)
{
    if (!text || !text->changed) return 0;
    return (int64_t)(text->changed - 1) * stagger_us + duration_us;
}
