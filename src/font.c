#include <stdlib.h>

#include "dgx_bitmap.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/vscreen.h"

/*
 * @brief Get Unicode codepoint from UTF-8 encoded string
 *
 * @param chr UTF-8 encoded string
 * @param idx Pointer to index of the character in the string
 *        that will be incremented according to the number of bytes of the codepoint
 * @return uint32_t Unicode codepoint
 */
uint32_t decodeUTF8next(const char *chr, size_t *idx)
{
    uint32_t c = chr[*idx];
    if (c < 0x80) {
        if (c) ++*idx;
        return c;
    }
    ++*idx;
    uint8_t len;
    if ((c & 0xE0) == 0xC0) {
        c &= 0x1f;
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0xf;
        len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x7;
        len = 4;
    } else {
        return c;
    }
    while (--len) {
        uint32_t nc = chr[(*idx)++];
        if ((nc & 0xC0) == 0x80) {
            c <<= 6;
            c |= nc & 0x3f;
        } else {
            break;
        }
    }
    return c;
}

/*
 * @brief Finds the glyph for the given code point in the given font.
 *
 * @param codePoint
 * @param font
 * @param xAdvance [out] x-offset for next character
 * @return glyph
 *
 * @note The xAdvance value is only valid if the glyph is found.
 */
const glyph_t *dgx_font_find_glyph(uint32_t codePoint, dgx_font_t *font, int16_t *xAdvance)
{
    const glyph_t *g = 0, *fg = 0;
    for (const glyph_array_t *r = font->glyph_ranges; r->number; ++r) {
        if (!fg) fg = r->glyphs;
        if (codePoint >= r->first && codePoint < r->first + r->number) {
            g = r->glyphs + (codePoint - r->first);
            break;
        }
    }
    if (g == 0 && fg) {
        *xAdvance = fg->xAdvance;
        return 0;
    }
    if (g == 0 && fg == 0) {
        *xAdvance = 0;
        return 0;
    }
    *xAdvance = g->xAdvance;
    return g;
}

/*
 * @brief Makes a thick point on virtual screen as big as screen itself
 *
 * @param vpoint 8 bit virtual screen.
 */
void dgx_font_make_point8(dgx_screen_t *vpoint)
{
    int point_size = vpoint->width;
    if (point_size == 1) {
        dgx_set_pixel(vpoint, 0, 0, 255);
    } else if (point_size == 2) {
        dgx_set_pixel(vpoint, 0, 0, 255);
        dgx_set_pixel(vpoint, 0, 1, 255);
        dgx_set_pixel(vpoint, 1, 0, 255);
        dgx_set_pixel(vpoint, 1, 1, 255);
    } else if (point_size == 3) {
        dgx_set_pixel(vpoint, 1, 1, 255);
        dgx_set_pixel(vpoint, 1, 0, 128);
        dgx_set_pixel(vpoint, 0, 1, 128);
        dgx_set_pixel(vpoint, 2, 1, 128);
        dgx_set_pixel(vpoint, 1, 2, 128);
        dgx_set_pixel(vpoint, 0, 0, 96);
        dgx_set_pixel(vpoint, 2, 0, 96);
        dgx_set_pixel(vpoint, 0, 2, 96);
        dgx_set_pixel(vpoint, 2, 2, 96);
    } else if (point_size == 4) {
        dgx_set_pixel(vpoint, 1, 1, 255);
        dgx_set_pixel(vpoint, 1, 2, 255);
        dgx_set_pixel(vpoint, 2, 1, 255);
        dgx_set_pixel(vpoint, 2, 2, 255);
        dgx_set_pixel(vpoint, 1, 0, 128);
        dgx_set_pixel(vpoint, 2, 0, 128);
        dgx_set_pixel(vpoint, 1, 3, 128);
        dgx_set_pixel(vpoint, 2, 3, 128);
        dgx_set_pixel(vpoint, 0, 1, 128);
        dgx_set_pixel(vpoint, 0, 2, 128);
        dgx_set_pixel(vpoint, 3, 1, 128);
        dgx_set_pixel(vpoint, 3, 2, 128);
        dgx_set_pixel(vpoint, 0, 0, 96);
        dgx_set_pixel(vpoint, 3, 0, 96);
        dgx_set_pixel(vpoint, 0, 3, 96);
        dgx_set_pixel(vpoint, 3, 3, 96);
    } else {
        if (point_size % 2 == 1) {
            for (int16_t r = vpoint->width / 2; r >= 1; --r) {
                uint8_t v = (vpoint->width / 2 - r + 1) * 510.0f / vpoint->width;
                dgx_solid_circle(vpoint, vpoint->width / 2, vpoint->height / 2, r, v);
            }
        } else {
            for (int16_t r = vpoint->width / 2 - 1; r >= 1; --r) {
                uint8_t v = (vpoint->width / 2 - r) * 255.0f / (vpoint->width / 2 - 1);
                dgx_solid_circle(vpoint, vpoint->width / 2, vpoint->height / 2, r, v);
                dgx_solid_circle(vpoint, vpoint->width / 2 - 1, vpoint->height / 2, r, v);
                dgx_solid_circle(vpoint, vpoint->width / 2, vpoint->height / 2 - 1, r, v);
                dgx_solid_circle(vpoint, vpoint->width / 2 - 1, vpoint->height / 2 - 1, r, v);
            }
        }
    }
}

/*
 * @brief Draws a single character to the screen
 *
 * @param scr
 * @param x
 * @param y
 * @param codePoint
 * @param color
 * @param orientation
 * @param scale
 * @param font
 * @param draw_func
 * @param param
 * @return x-offset for next character
 */

int dgx_font_char_to_screen(            //
    dgx_screen_t            *scr,       //
    int16_t                  x,         //
    int16_t                  y,         //
    uint32_t                 codePoint, //
    uint32_t                 color,     //
    dgx_output_orientation_t orientation, //
    int                      scale,     //
    struct dgx_font_        *font,      //
    dgx_font_draw_sym_func_t draw_func, //
    void                    *param      //
)
{
    dgx_orientation_t xdir    = dgx_output_orientation_xdir(orientation);
    dgx_orientation_t ydir    = dgx_output_orientation_ydir(orientation);
    bool              swap_xy = dgx_output_orientation_swap_xy(orientation);
    if (!draw_func) {
        int16_t        xAdvance;
        const glyph_t *g = dgx_font_find_glyph(codePoint, font, &xAdvance);
        if (!g) return xAdvance;
        if (font->f_type == DGX_FONT_BITMAP_LINES || font->f_type == DGX_FONT_BITMAP_STREAM) {
            dgx_bw_bitmap_t bmap   = dgx_bw_bitmap_make_of((uint8_t *)g->bitmap, g->width, g->height,
                                                           font->f_type == DGX_FONT_BITMAP_STREAM);
            int             left   = 0;
            int             right  = left + g->width - 1;
            int             top    = 0;
            int             bottom = top + g->height - 1;
            if (!scale) scale = 1;
            int x_shift = g->xOffset * scale;
            int y_shift = g->yOffset * scale;
            if (swap_xy) {
                int t   = x_shift;
                x_shift = y_shift;
                y_shift = t;
                t       = right;
                right   = bottom;
                bottom  = t;
            }
            /*
             * Fast path: stamp a non-stream glyph into a 1-bpp linear virtual
             * screen via packed OR-blit. ~5-8x fewer writes than the per-pixel
             * loop. Conditions are kept narrow to preserve exact semantics of
             * the slow path; any mismatch falls through to it.
             */
            if (font->f_type == DGX_FONT_BITMAP_LINES &&
                scale == 1 && !swap_xy &&
                xdir == DgxScreenLeftRight && ydir == DgxScreenTopBottom &&
                color != 0 && scr->color_bits == 1 &&
                (scr->screen_subtype == DgxVirtualScreen ||
                 scr->screen_subtype == DgxVirtualBackScreen)) {
                dgx_vscreen_t  *vscr = (dgx_vscreen_t *)scr;
                dgx_bw_bitmap_t dst  = dgx_bw_bitmap_make_of(vscr->v_array, scr->width, scr->height, false);
                int             top_progress = dgx_screen_progress_up(scr);
                dgx_bw_bitmap_blit_or(&dst, x + x_shift, y + y_shift, &bmap);
                if (dgx_screen_progress_down(scr) == 0 && scr->update_screen) {
                    int gx0 = x + x_shift;
                    int gy0 = y + y_shift;
                    int gx1 = gx0 + g->width  - 1;
                    int gy1 = gy0 + g->height - 1;
                    scr->update_screen(scr, gx0, gx1, gy0, gy1);
                }
                (void)top_progress;
                return xAdvance * scale;
            }
            dgx_point_2d_t current_point = _dgx_start_area_pixel(left, right, top, bottom, xdir, ydir);
            for (int by = 0; by < g->height; ++by) {
                for (int bx = 0; bx < g->width; bx++) {
                    bool pix = dgx_bw_bitmap_get_pixel(&bmap, bx, by);
                    if (pix) {
                        dgx_fill_rectangle(scr, x + x_shift + current_point.x * scale,
                                           y + y_shift + current_point.y * scale, scale, scale, color);
                    }
                    current_point =
                        _dgx_move_to_next_area_pixel(current_point, left, right, top, bottom, xdir, ydir, swap_xy);
                }
            }
        } else {
            int x_shift = g->xOffset * scale;
            int y_shift = g->yOffset * scale;
            if (param != NULL) {
                dgx_font_sym8_params_t *vParams = (dgx_font_sym8_params_t *)param;
                for (int i = 0; i < g->number_of_dots; i++) {
                    int px = g->dots[i].x * scale + x_shift;
                    int py = g->dots[i].y * scale + y_shift;
                    if (swap_xy) {
                        int t = px;
                        px    = py;
                        py    = t;
                    }
                    vParams->dot_func(scr, x + px, y + py, vParams);
                }
            } else {
                for (int i = 0; i < g->number_of_dots; i++) {
                    int px = g->dots[i].x * scale + x_shift;
                    int py = g->dots[i].y * scale + y_shift;
                    if (swap_xy) {
                        int t = px;
                        px    = py;
                        py    = t;
                    }
                    dgx_fill_rectangle(scr, x + px, y + py, scale, scale, color);
                }
            }
        }
        return xAdvance * scale;
    } else {
        return draw_func(scr, x, y, codePoint, color, orientation, scale, font, param);
    }
}

int dgx_font_string_bounds(const char *str, dgx_font_t *font, int16_t *ycorner, int16_t *height)
{
    size_t  idx   = 0;
    int16_t width = 0, yb = 0, yt = 0;
    bool    is_first = true;
    while (str[idx]) {
        uint32_t       cp = decodeUTF8next(str, &idx);
        int16_t        xAdvance;
        const glyph_t *g = dgx_font_find_glyph(cp, font, &xAdvance);
        width += xAdvance;
        if (g) {
            int16_t bl = g->yOffset;
            int16_t bh = g->height + g->yOffset;
            if (is_first || bl < yt) yt = bl;
            if (is_first || bh > yb) yb = bh;
            is_first = false;
        }
    }
    if (ycorner) {
        *ycorner = yt;
    }
    if (height) {
        *height = yb - yt;
    }
    return width;
}

void dgx_font_string_utf8_screen(       //
    dgx_screen_t            *scr,       //
    int16_t                  x,         //
    int16_t                  y,         //
    const char              *str,       //
    uint32_t                 color,     //
    dgx_output_orientation_t orientation, //
    int                      scale,     //
    struct dgx_font_        *font,      //
    dgx_font_draw_sym_func_t draw_func, //
    void                    *param      //
)
{
    dgx_orientation_t xdir    = dgx_output_orientation_xdir(orientation);
    dgx_orientation_t ydir    = dgx_output_orientation_ydir(orientation);
    bool              swap_xy = dgx_output_orientation_swap_xy(orientation);
    size_t idx = 0;
    idx        = 0;
    int left = x, right = x, top = y, bottom = y;
    dgx_screen_progress_up(scr);
    while (str[idx]) {
        uint32_t cp = decodeUTF8next(str, &idx);
        int16_t  offset =
            dgx_font_char_to_screen(scr, x, y, cp, color, orientation, scale, font, draw_func, param);
        if (swap_xy) {
            y += offset * ydir;
            if (ydir == DgxScreenTopBottom) {
                if (y > bottom) bottom = y;
            } else {
                if (y < top) top = y;
            }
        } else {
            x += offset * xdir;
            if (xdir == DgxScreenLeftRight) {
                if (x > right) right = x;
            } else {
                if (x < left) left = x;
            }
        }
    }
    if (!dgx_screen_progress_down(scr)) {
        if (left == right && top == bottom) {
            return;
        }
        int16_t ycorner;
        int16_t height;
        dgx_font_string_bounds(str, font, &ycorner, &height);
        ycorner *= scale;
        height  *= scale;
        if (left == right) {
            if (xdir == DgxScreenLeftRight) {
                right = x + height + ycorner;
                left  = x - height;
            } else {
                right = x - height;
                left  = x + height + ycorner;
            }
        } else if (top == bottom) {
            if (ydir == DgxScreenTopBottom) {
                bottom = y + height + ycorner;
                top    = y - height;
            } else {
                bottom = y - height;
                top    = y + height + ycorner;
            }
        }
        scr->update_screen(scr, left, right, top, bottom);
    }
}

