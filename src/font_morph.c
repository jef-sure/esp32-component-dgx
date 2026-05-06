#include <stdlib.h>

#include "dgx_bits.h"
#include "dgx_font.h"
#include "esp_log.h"

static const char TAG[] = "DGX FONT MORPH";

static dgx_font_dot_t *_dgx_dots_from_bitmap(const glyph_t *g, bool is_stream, int *number_of_dots)
{
    if (!g) {
        *number_of_dots = 0;
        return NULL;
    }
    int             count = 0;
    dgx_bw_bitmap_t bmap  = dgx_bw_bitmap_make_of((uint8_t *)g->bitmap, g->width, g->height, is_stream);
    for (int by = 0; by < g->height; ++by) {
        for (int bx = 0; bx < g->width; bx++) {
            bool pix = dgx_bw_bitmap_get_pixel(&bmap, bx, by);
            count += (int)pix;
        }
    }
    *number_of_dots     = count;
    dgx_font_dot_t *ret = calloc((size_t)count, sizeof(dgx_font_dot_t));
    if (!ret) {
        ESP_LOGE(TAG, "Memory allocation for temporary dgx_font_dot_t array failed");
        *number_of_dots = 0;
        return NULL;
    }
    count = 0;
    for (int by = 0; by < g->height; ++by) {
        for (int bx = 0; bx < g->width; bx++) {
            bool pix = dgx_bw_bitmap_get_pixel(&bmap, bx, by);
            if (pix) {
                ret[count++] = dgx_font_dot_new(bx, by);
            }
        }
    }
    return ret;
}

static dgx_point_2d_t _dgx_empty_glyph_anchor(int x, int y, int x_shift, int y_shift, const glyph_t *g, int scale)
{
    dgx_point_2d_t anchor = {
        .x = x + x_shift,
        .y = y + y_shift,
    };

    if (g != NULL) {
        anchor.x += ((g->width - 1) * scale) / 2;
        anchor.y += ((g->height - 1) * scale) / 2;
    }

    return anchor;
}

dgx_font_symbol_morph_t *dgx_font_make_morph_struct( //
    dgx_font_t *font,                                //
    uint32_t    fromCP,                              //
    uint32_t    toCP,                                //
    int         from_x,                              //
    int         from_y,                              //
    int         to_x,                                //
    int         to_y,                                //
    int         scale                                //
)
{
    dgx_font_symbol_morph_t *ret = calloc(1, sizeof(*ret));
    if (!ret) {
        ESP_LOGE(TAG, "Memory allocation for dgx_font_symbol_morph_t failed");
        return NULL;
    }
    ret->fromCP        = fromCP;
    ret->toCP          = toCP;
    ret->is_from_empty = true;
    ret->is_to_empty   = true;
    int16_t               xAdvance;
    const glyph_t        *gFrom = dgx_font_find_glyph(fromCP, font, &xAdvance);
    const glyph_t        *gTo   = dgx_font_find_glyph(toCP, font, &xAdvance);
    int                   from_number_of_dots;
    int                   to_number_of_dots;
    const dgx_font_dot_t *from_dots;
    const dgx_font_dot_t *to_dots;
    dgx_font_dot_t       *from_dots_tmp = NULL;
    dgx_font_dot_t       *to_dots_tmp   = NULL;
    int                   max_sym_height;
    int                   max_sym_width;
    int                   x_shift_from = gFrom ? gFrom->xOffset * scale : 0;
    int                   y_shift_from = gFrom ? gFrom->yOffset * scale : 0;
    int                   x_shift_to   = gTo ? gTo->xOffset * scale : 0;
    int                   y_shift_to   = gTo ? gTo->yOffset * scale : 0;
    max_sym_height                     = gFrom ? gFrom->height : 0;
    max_sym_width                      = gFrom ? gFrom->width : 0;
    max_sym_height                     = gTo ? DGX_MAX(gTo->height, max_sym_height) : 0;
    max_sym_width                      = gTo ? DGX_MAX(gTo->width, max_sym_width) : 0;
    if (font->f_type == DGX_FONT_DOTS) {
        from_dots           = gFrom ? gFrom->dots : NULL;
        from_number_of_dots = gFrom ? gFrom->number_of_dots : 0;
        to_dots             = gTo ? gTo->dots : NULL;
        to_number_of_dots   = gTo ? gTo->number_of_dots : 0;
    } else {
        from_dots = from_dots_tmp =
            _dgx_dots_from_bitmap(gFrom, font->f_type == DGX_FONT_BITMAP_STREAM, &from_number_of_dots);
        to_dots = to_dots_tmp = _dgx_dots_from_bitmap(gTo, font->f_type == DGX_FONT_BITMAP_STREAM, &to_number_of_dots);
    }
    if (from_number_of_dots == 0 && to_number_of_dots == 0) return ret;
    if (max_sym_height == 0 || max_sym_width == 0) return ret;
    ret->is_from_empty  = from_number_of_dots == 0;
    ret->is_to_empty    = to_number_of_dots == 0;
    int maxp            = DGX_MAX(from_number_of_dots, to_number_of_dots);
    ret->number_of_dots = maxp;
    ret->m_start        = (dgx_point_2d_t *)calloc((size_t)maxp * 2, sizeof(dgx_point_2d_t));
    if (!ret->m_start) {
        ESP_LOGE(TAG, "Memory allocation for dots array failed");
        free(ret);
        return NULL;
    }
    ret->m_end = ret->m_start + maxp;
    for (int i = 0; i < maxp; i++) {
        if (i < from_number_of_dots) {
            ret->m_start[i].x = from_x + x_shift_from + from_dots[i].x * scale;
            ret->m_start[i].y = from_y + y_shift_from + from_dots[i].y * scale;
        } else if (from_number_of_dots != 0) {
            ret->m_start[i].x = ret->m_start[from_number_of_dots - 1].x;
            ret->m_start[i].y = ret->m_start[from_number_of_dots - 1].y;
        }
        if (i < to_number_of_dots) {
            ret->m_end[i].x = to_x + x_shift_to + to_dots[i].x * scale;
            ret->m_end[i].y = to_y + y_shift_to + to_dots[i].y * scale;
        } else if (to_number_of_dots != 0) {
            ret->m_end[i].x = ret->m_end[to_number_of_dots - 1].x;
            ret->m_end[i].y = ret->m_end[to_number_of_dots - 1].y;
        }
    }
    if (from_number_of_dots == 0) {
        dgx_point_2d_t anchor = _dgx_empty_glyph_anchor(to_x, to_y, x_shift_to, y_shift_to, gTo, scale);

        for (int i = 0; i < maxp; i++) {
            ret->m_start[i] = anchor;
        }
    }
    if (to_number_of_dots == 0) {
        dgx_point_2d_t anchor = _dgx_empty_glyph_anchor(from_x, from_y, x_shift_from, y_shift_from, gFrom, scale);

        for (int i = 0; i < maxp; i++) {
            ret->m_end[i] = anchor;
        }
    }
    if (font->f_type != DGX_FONT_DOTS) {
        free(from_dots_tmp);
        free(to_dots_tmp);
    }
    return ret;
}

void dgx_font_make_morph_struct_destroy(dgx_font_symbol_morph_t **ms)
{
    free((*ms)->m_start);
    (*ms)->m_start = 0;
    free(*ms);
    *ms = 0;
}
