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
