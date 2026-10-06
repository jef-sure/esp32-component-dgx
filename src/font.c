#include <stdint.h>
#include <stdlib.h>

#include "dgx_bitmap.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/vscreen.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* The packed glyph blit needs vscreen.c, which is optional in the component build. */
#if !defined(ESP_PLATFORM) || defined(CONFIG_DGX_ENABLE_VSCREEN)
#define DGX_FONT_VSCREEN_FAST_PATH 1
#endif
#if !defined(ESP_PLATFORM) || defined(CONFIG_DGX_ENABLE_V_BW_SCREEN)
#include "dgx_bw_screen.h"
#define DGX_FONT_BW_SCREEN_FAST_PATH 1
#endif

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
    uint32_t c = (uint8_t)chr[*idx];
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
        uint32_t nc = (uint8_t)chr[*idx];
        // truncated sequence: leave the terminator or the next character unconsumed
        if ((nc & 0xC0) != 0x80) break;
        ++*idx;
        c <<= 6;
        c |= nc & 0x3f;
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
#ifdef DGX_FONT_VSCREEN_FAST_PATH
            if (font->f_type == DGX_FONT_BITMAP_LINES &&
                scale == 1 && !swap_xy &&
                xdir == DgxScreenLeftRight && ydir == DgxScreenTopBottom &&
                color != 0 && scr->color_bits == 1 &&
                scr->width % 8 == 0 && dgx_vscreen_is_linear(scr)) {
                dgx_vscreen_t  *vscr = (dgx_vscreen_t *)scr;
                dgx_bw_bitmap_t dst  = dgx_bw_bitmap_make_of(vscr->v_array, scr->width, scr->height, false);
                int             gx0  = x + x_shift;
                int             gy0  = y + y_shift;
                dgx_bw_bitmap_blit_or(&dst, gx0, gy0, &bmap);
                dgx_screen_touch(scr, gx0, gx0 + g->width - 1, gy0, gy0 + g->height - 1);
                return xAdvance * scale;
            }
#endif
#ifdef DGX_FONT_BW_SCREEN_FAST_PATH
            /* The same for a page-organised screen (SSD1306, ST7565R): set the bits right in its buffer. */
            if (font->f_type == DGX_FONT_BITMAP_LINES &&
                scale == 1 && !swap_xy &&
                xdir == DgxScreenLeftRight && ydir == DgxScreenTopBottom &&
                color != 0 && dgx_bw_screen_is_paged(scr)) {
                dgx_bw_blit_or(scr, x + x_shift, y + y_shift, &bmap);
                return xAdvance * scale;
            }
#endif
            /*
             * Every run of set pixels in a glyph row goes out as one
             * rectangle: on a panel a rectangle costs a window setup, so a
             * pixel at a time is several times slower.
             */
            const bool is_lines = font->f_type == DGX_FONT_BITMAP_LINES;
            const int  pitch    = (g->width + 7) / 8;
            dgx_screen_progress_up(scr);
            for (int by = 0; by < g->height; ++by) {
                const uint8_t *row = (const uint8_t *)g->bitmap + by * pitch;
                for (int bx = 0; bx < g->width; ++bx) {
                    if (is_lines) {
                        if ((bx & 7) == 0 && row[bx >> 3] == 0) { // whole byte empty: skip 8 pixels
                            bx += 7;
                            continue;
                        }
                        if (!(row[bx >> 3] & (0x80u >> (bx & 7)))) continue;
                    } else if (!dgx_bw_bitmap_get_pixel(&bmap, bx, by)) {
                        continue;
                    }
                    int run_start = bx;
                    while (bx + 1 < g->width && (is_lines ? (row[(bx + 1) >> 3] & (0x80u >> ((bx + 1) & 7))) != 0
                                                          : dgx_bw_bitmap_get_pixel(&bmap, bx + 1, by))) {
                        ++bx;
                    }
                    /* glyph columns run_start..bx of row by, placed the way the orientation asks */
                    int rx, ry, rw, rh;
                    if (!swap_xy) {
                        rx = xdir == DgxScreenLeftRight ? run_start : right - bx;
                        ry = ydir == DgxScreenTopBottom ? by : bottom - by;
                        rw = bx - run_start + 1;
                        rh = 1;
                    } else {
                        rx = xdir == DgxScreenLeftRight ? by : right - by;
                        ry = ydir == DgxScreenTopBottom ? run_start : bottom - bx;
                        rw = 1;
                        rh = bx - run_start + 1;
                    }
                    dgx_fill_rectangle(scr, x + x_shift + rx * scale, y + y_shift + ry * scale, rw * scale, rh * scale, color);
                }
            }
            dgx_screen_progress_down(scr);
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
    dgx_screen_progress_up(scr);
    while (str[idx]) {
        uint32_t cp = decodeUTF8next(str, &idx);
        int16_t  offset =
            dgx_font_char_to_screen(scr, x, y, cp, color, orientation, scale, font, draw_func, param);
        if (swap_xy) {
            y += offset * ydir;
        } else {
            x += offset * xdir;
        }
    }
    dgx_screen_progress_down(scr);
}

