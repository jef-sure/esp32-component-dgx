#include <stdint.h>
#include <stdlib.h>

#include "dgx_bitmap.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_font_priv.h"
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
 * What is not UTF-8 is U+FFFD, the replacement character: a byte that starts
 * no character, a sequence cut short, one longer than its value needs, a
 * surrogate, a value past U+10FFFF. A byte that is not a part of the
 * sequence, the terminator above all, is left for the next call; every other
 * byte is consumed, at least one, so a loop over a string ends.
 *
 * @param chr UTF-8 encoded string
 * @param idx Pointer to index of the character in the string
 *        that will be incremented according to the number of bytes of the codepoint
 * @return uint32_t Unicode codepoint
 */
uint32_t decodeUTF8next(const char *chr, size_t *idx)
{
    const uint8_t *s = (const uint8_t *)chr + *idx;
    uint32_t       c = s[0];
    if (c < 0x80) {
        if (c) ++*idx;
        return c;
    }
    uint8_t  len;
    uint32_t least;
    if (c >= 0xC2 && c <= 0xDF) {
        c &= 0x1f, len = 2, least = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0f, len = 3, least = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        c &= 0x07, len = 4, least = 0x10000;
    } else {
        ++*idx;
        return 0xFFFD;
    }
    for (uint8_t i = 1; i < len; ++i) {
        const uint8_t nc = s[i];
        if ((nc & 0xC0) != 0x80) {
            *idx += i;
            return 0xFFFD;
        }
        c = c << 6 | (nc & 0x3f);
    }
    *idx += len;
    if (c < least || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return 0xFFFD;
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
    if (font->number_of_ranges > 0) {
        /* the ranges are told to be ascending: found by halving */
        int lo = 0, hi = font->number_of_ranges - 1;
        fg = font->glyph_ranges[0].glyphs;
        while (lo <= hi) {
            int                  mid = (lo + hi) / 2;
            const glyph_array_t *r = font->glyph_ranges + mid;
            if (codePoint < (uint32_t)r->first) {
                hi = mid - 1;
            } else if (codePoint >= (uint32_t)r->first + r->number) {
                lo = mid + 1;
            } else {
                g = r->glyphs + (codePoint - r->first);
                break;
            }
        }
    } else {
        for (const glyph_array_t *r = font->glyph_ranges; r->number; ++r) {
            if (!fg) fg = r->glyphs;
            if (codePoint >= r->first && codePoint < r->first + r->number) {
                g = r->glyphs + (codePoint - r->first);
                break;
            }
        }
    }
    if (xAdvance) *xAdvance = g ? g->xAdvance : fg ? fg->xAdvance : 0;
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
            /* the middle at full brightness, as in the dots of every other size */
            for (int16_t r = vpoint->width / 2; r >= 1; --r) {
                uint8_t v = (vpoint->width / 2 - r + 1) * 255.0f / (vpoint->width / 2);
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
 * The coverage of a glyph of 2 or 4 bits a pixel as the alpha of its color: the levels bent by a^(1/1.45). Mixed in
 * the values of the screen as they were, dark letters on light paper came out thin on the panels tried (the ILI9341
 * of a CYD, the JD9165 of a 7-inch Guition JC1060P470); with this curve they look right there, and light letters on
 * dark paper looked the same either way. It is the drawing's, not the font's: a font keeps the coverage itself.
 */
static const uint8_t soft_alpha4[16] = {0, 39, 64, 84, 102, 120, 136, 151, 165, 179, 193, 206, 219, 231, 243, 255};
static const uint8_t soft_alpha2[4]  = {0, 120, 193, 255};

/* the coverage of 255 a pixel must have to be ink in a font of one bit cut from the gray render, as font2c cuts */
#define DGX_FONT_INK_THRESHOLD 110

static int font_depth(const dgx_font_t *font)
{
    return font->f_type == DGX_FONT_BITMAP_DEPTH4 ? 4 : font->f_type == DGX_FONT_BITMAP_DEPTH2 ? 2 : 1;
}

/* whether colors of this many bits are mixed, channel by channel; 8 bits are an amount of ink */
static bool mixable_bits(int bits)
{
    return bits == 8 || bits == 12 || bits == 16 || bits == 18 || bits == 24;
}

static uint32_t mix_channel(uint32_t under, uint32_t color, unsigned a, unsigned shift, uint32_t mask)
{
    const uint32_t u = (under >> shift) & mask, c = (color >> shift) & mask;
    return ((u * (255u - a) + c * a + 127u) / 255u) << shift;
}

/*
 * `color` over `under` by a of 255, rounded, each channel in its own width: 8 bits one amount of ink; 12 bits nibbles
 * at 15..12, 11..8 and 7..4 (DGX_RGB_12); 16 bits 5, 6 and 5; 18 bits the six high bits of each byte (DGX_RGB_18);
 * 24 bits a byte each.
 */
static uint32_t mix_color(int bits, uint32_t under, uint32_t color, unsigned a)
{
    switch (bits) {
    case 8:
        return mix_channel(under, color, a, 0, 0xff);
    case 12:
        return mix_channel(under, color, a, 12, 15) | mix_channel(under, color, a, 8, 15) | mix_channel(under, color, a, 4, 15);
    case 16:
        return mix_channel(under, color, a, 11, 31) | mix_channel(under, color, a, 5, 63) | mix_channel(under, color, a, 0, 31);
    case 18:
        return mix_channel(under, color, a, 18, 63) | mix_channel(under, color, a, 10, 63) | mix_channel(under, color, a, 2, 63);
    default:
        return mix_channel(under, color, a, 16, 255) | mix_channel(under, color, a, 8, 255) | mix_channel(under, color, a, 0, 255);
    }
}

/*
 * The colors of the levels of a font of 2 or 4 bits over the paper of the context, when it has one and the screen
 * has colors to mix; NULL otherwise.
 */
static const uint32_t *soft_table(const dgx_font_context_t *ctx, uint32_t table[16])
{
    const int depth = font_depth(ctx->font);
    if (depth == 1 || !(ctx->flags & (DGX_FONT_BG | DGX_FONT_FILL)) || !mixable_bits(ctx->scr->color_bits)) return NULL;
    const uint8_t *alpha = depth == 4 ? soft_alpha4 : soft_alpha2;
    for (int k = 0; k < 1 << depth; ++k) table[k] = mix_color(ctx->scr->color_bits, ctx->bg, ctx->color, alpha[k]);
    return table;
}

int dgx_font_glyph_foreach_ink(const dgx_font_t *font, const glyph_t *g, dgx_bw_bitmap_pixel_func_t func, void *user_data)
{
    if (!font || !g || !g->bitmap) return 0;
    const int depth = font_depth(font);
    if (depth == 1) {
        dgx_bw_bitmap_t bmap = dgx_bw_bitmap_make_of((uint8_t *)g->bitmap, g->width, g->height, font->f_type == DGX_FONT_BITMAP_STREAM);
        return dgx_bw_bitmap_foreach_set(&bmap, func, user_data);
    }
    const int top = (1 << depth) - 1, unit = 255 / top, pitch = (g->width * depth + 7) / 8;
    int       visited = 0;
    for (int y = 0; y < g->height; ++y)
        for (int x = 0; x < g->width; ++x) {
            const unsigned bit = (unsigned)x * depth;
            if (((g->bitmap[y * pitch + (bit >> 3)] >> (8 - depth - (bit & 7))) & top) * unit < DGX_FONT_INK_THRESHOLD) continue;
            ++visited;
            if (func && !func(user_data, x, y)) return visited;
        }
    return visited;
}

/* how the pixels of a glyph lie on the screen */
typedef struct {
    int  x, y;          /* the screen pixel of the top left corner of the glyph's box */
    int  right, bottom; /* the last column and row of the box as it lies on the screen, in pixels of the font */
    int  scale;
    bool swap_xy, flip_x, flip_y;
} glyph_place_t;

/* the rectangle of the screen the columns from..to of row `row` of a glyph take, laid as the orientation asks */
static void place_run(const glyph_place_t *p, int row, int from, int to, int *x, int *y, int *w, int *h)
{
    int rx, ry, rw, rh;
    if (!p->swap_xy) {
        rx = !p->flip_x ? from : p->right - to;
        ry = !p->flip_y ? row : p->bottom - row;
        rw = to - from + 1;
        rh = 1;
    } else {
        rx = !p->flip_x ? row : p->right - row;
        ry = !p->flip_y ? from : p->bottom - to;
        rw = 1;
        rh = to - from + 1;
    }
    *x = p->x + rx * p->scale;
    *y = p->y + ry * p->scale;
    *w = rw * p->scale;
    *h = rh * p->scale;
}

static inline int glyph_level(const uint8_t *row, int bx, int depth, int top)
{
    const unsigned bit = (unsigned)bx * depth;
    return (row[bit >> 3] >> (8 - depth - (bit & 7))) & top;
}

/*
 * The columns from..to of row `by` of a glyph, each touched by the letter, to a display behind a bus in one window:
 * their colors of the table put in the buffer of the bus a row of the window at a time. A window costs a run of
 * commands on the bus, so the edge of a letter goes with the run it belongs to, not as rectangles of its own.
 * False when a row of the window does not fit in the buffer, and nothing is sent.
 */
static bool span_to_bus(dgx_screen_t *scr, const glyph_place_t *p, const uint8_t *row, int by, int from, int to, int depth, const uint32_t *table)
{
    int rx, ry, rw, rh;
    place_run(p, by, from, to, &rx, &ry, &rw, &rh);
    const int x0 = rx < 0 ? 0 : rx, x1 = rx + rw > scr->width ? scr->width - 1 : rx + rw - 1;
    const int y0 = ry < 0 ? 0 : ry, y1 = ry + rh > scr->height ? scr->height - 1 : ry + rh - 1;
    if (x0 > x1 || y0 > y1) return true;
    const uint32_t bytes = dgx_color_points_to_bytes(scr->color_bits, (uint32_t)(x1 - x0 + 1));
    if (bytes > scr->draw_buffer_len) return false;
    const int top = (1 << depth) - 1;
    scr->set_area(scr, (uint16_t)x0, (uint16_t)x1, (uint16_t)y0, (uint16_t)y1);
    for (int sy = y0; sy <= y1; ++sy) {
        /* the row before may still be on its way from the buffer */
        if (scr->wait_buffer) scr->wait_buffer(scr);
        uint8_t *lp = scr->draw_buffer;
        for (int sx = x0; sx <= x1; ++sx) {
            /* the column of the glyph this pixel is of: along the run, in the order the orientation lays it */
            const int k   = p->swap_xy ? (sy - ry) / p->scale : (sx - rx) / p->scale;
            const int col = (p->swap_xy ? p->flip_y : p->flip_x) ? to - k : from + k;
            lp            = dgx_fill_buf_value(scr->color_bits, lp, sx - x0, table[glyph_level(row, col, depth, top)]);
        }
        scr->write_area(scr, scr->draw_buffer, bytes * 8u);
    }
    return true;
}

/*
 * A glyph of 2 or 4 bits a pixel. A pixel drawn whole, in the color or in a color of the table, goes in runs of one
 * color, a rectangle each; a pixel mixed with the screen is read and written, each of its scale x scale. On a display
 * behind a bus a run of touched pixels with the colors of the table goes as one window, span_to_bus().
 */
static void soft_glyph(const dgx_font_context_t *ctx, const glyph_t *g, const glyph_place_t *p, const uint32_t *table)
{
    dgx_screen_t  *scr   = ctx->scr;
    const int      depth = font_depth(ctx->font), top = (1 << depth) - 1, unit = 255 / top;
    const int      pitch = (g->width * depth + 7) / 8;
    const int      bits  = scr->color_bits;
    const uint8_t *alpha = depth == 4 ? soft_alpha4 : soft_alpha2;
    /* the level from which a pixel is drawn whole when the levels are not mixed: every one touched, or as font2c cuts */
    int cut = 0;
    if (ctx->flags & DGX_FONT_HARD) cut = 1;
    else if ((ctx->flags & DGX_FONT_MONO) || (!table && !(mixable_bits(bits) && dgx_screen_is_readable(scr))))
        cut = (DGX_FONT_INK_THRESHOLD + unit - 1) / unit;
    /* whole bytes a pixel, as a window of a display takes a row of them */
    const bool spans = table && !cut && scr->screen_subtype == DgxPhysicalScreenWithBus && scr->set_area && scr->write_area && scr->draw_buffer &&
                       (bits == 8 || bits == 16 || bits == 18 || bits == 24);
    for (int by = 0; by < g->height; ++by) {
        const uint8_t *row       = g->bitmap + by * pitch;
        int            run_start = -1;
        uint32_t       run_color = 0;
        int            sent_to   = -1; /* the last column a span has sent */
        for (int bx = 0; bx <= g->width; ++bx) {
            int level = bx < g->width ? glyph_level(row, bx, depth, top) : 0;
            if (spans && level && bx > sent_to) {
                int last = bx;
                while (last + 1 < g->width && glyph_level(row, last + 1, depth, top)) ++last;
                if (span_to_bus(scr, p, row, by, bx, last, depth, table)) {
                    sent_to = last;
                    bx      = last;
                    continue;
                }
                sent_to = last; /* too long a row for the buffer: these go as rectangles below */
            }
            bool     solid = false, mixed = false;
            uint32_t c     = ctx->color;
            if (level) {
                if (cut) solid = level >= cut;
                else if (table) solid = true, c = table[level];
                else if (level == top) solid = true;
                else mixed = true;
            }
            int rx, ry, rw, rh;
            if (run_start >= 0 && (!solid || c != run_color)) {
                place_run(p, by, run_start, bx - 1, &rx, &ry, &rw, &rh);
                dgx_fill_rectangle(scr, rx, ry, rw, rh, run_color);
                run_start = -1;
            }
            if (solid && run_start < 0) run_start = bx, run_color = c;
            if (mixed) {
                place_run(p, by, bx, bx, &rx, &ry, &rw, &rh);
                for (int sy = ry; sy < ry + rh; ++sy)
                    for (int sx = rx; sx < rx + rw; ++sx) dgx_set_pixel(scr, sx, sy, mix_color(bits, dgx_get_pixel(scr, sx, sy), ctx->color, alpha[level]));
            }
        }
    }
}

/*
 * One character by the context: the glyph found, laid as the orientation asks and drawn; the advance in pixels, the
 * scale counted. table: the colors of the levels of a soft font over the paper, or NULL (soft_table()).
 */
static int font_char(const dgx_font_context_t *ctx, int x, int y, uint32_t codePoint, const uint32_t *table)
{
    dgx_screen_t            *scr         = ctx->scr;
    dgx_font_t              *font        = ctx->font;
    const uint32_t           color       = ctx->color;
    const dgx_output_orientation_t orientation = ctx->orientation;
    const int                scale       = ctx->scale ? ctx->scale : 1;
    dgx_orientation_t        xdir        = dgx_output_orientation_xdir(orientation);
    dgx_orientation_t        ydir        = dgx_output_orientation_ydir(orientation);
    bool                     swap_xy     = dgx_output_orientation_swap_xy(orientation);
    int16_t                  xAdvance;
    const glyph_t           *g = dgx_font_find_glyph(codePoint, font, &xAdvance);
    /* a missing glyph advances by the first glyph of the font, as dgx_font_string_bounds() counts it */
    if (!g) return xAdvance * scale;
    /* a handwritten font is not drawn symbol by symbol, see dgx_hw_font.h */
    if (font->f_type == DGX_FONT_HW) return xAdvance * scale;
    if (font->f_type == DGX_FONT_BITMAP_LINES || font->f_type == DGX_FONT_BITMAP_STREAM || font->f_type == DGX_FONT_BITMAP_DEPTH2 ||
        font->f_type == DGX_FONT_BITMAP_DEPTH4) {
        int left    = 0;
        int right   = left + g->width - 1;
        int top     = 0;
        int bottom  = top + g->height - 1;
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
         * A flipped axis turns the glyph around the pen, not inside its own
         * box: the box goes to the other side of the pen, or glyphs of other
         * boxes would not stand on one line.
         */
        if (xdir == DgxScreenRightLeft) x_shift = -x_shift - (right + 1) * scale;
        if (ydir == DgxScreenBottomTop) y_shift = -y_shift - (bottom + 1) * scale;
        const glyph_place_t place = {.x       = x + x_shift,
                                     .y       = y + y_shift,
                                     .right   = right,
                                     .bottom  = bottom,
                                     .scale   = scale,
                                     .swap_xy = swap_xy,
                                     .flip_x  = xdir != DgxScreenLeftRight,
                                     .flip_y  = ydir != DgxScreenTopBottom};
        if (font->f_type == DGX_FONT_BITMAP_DEPTH2 || font->f_type == DGX_FONT_BITMAP_DEPTH4) {
            dgx_screen_progress_up(scr);
            soft_glyph(ctx, g, &place, table);
            dgx_screen_progress_down(scr);
            return xAdvance * scale;
        }
        dgx_bw_bitmap_t bmap = dgx_bw_bitmap_make_of((uint8_t *)g->bitmap, g->width, g->height, font->f_type == DGX_FONT_BITMAP_STREAM);
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
                place_run(&place, by, run_start, bx, &rx, &ry, &rw, &rh);
                dgx_fill_rectangle(scr, rx, ry, rw, rh, color);
            }
        }
        dgx_screen_progress_down(scr);
    } else {
        int x_shift = g->xOffset * scale;
        int y_shift = g->yOffset * scale;
        for (int i = 0; i < g->number_of_dots; i++) {
            int px = g->dots[i].x * scale + x_shift;
            int py = g->dots[i].y * scale + y_shift;
            if (swap_xy) {
                int t = px;
                px    = py;
                py    = t;
            }
            /* a flipped axis puts the dot on the other side of the pen, as it does a bitmap */
            if (xdir == DgxScreenRightLeft) px = -px - scale;
            if (ydir == DgxScreenBottomTop) py = -py - scale;
            if (ctx->dot_func) ctx->dot_func(ctx, x + px, y + py);
            else dgx_fill_rectangle(scr, x + px, y + py, scale, scale, color);
        }
    }
    return xAdvance * scale;
}

/* the dot function of the parameters an old call passes for a dot font, dgx_font_sym8_params_t */
static void sym8_dot(const dgx_font_context_t *ctx, int x, int y)
{
    dgx_font_sym8_params_t *params = (dgx_font_sym8_params_t *)ctx->user_data;
    params->dot_func(ctx->scr, x, y, params);
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
    if (draw_func) return draw_func(scr, x, y, codePoint, color, orientation, scale, font, param);
    const dgx_font_context_t ctx = {.scr         = scr,
                                    .font        = font,
                                    .color       = color,
                                    .orientation = orientation,
                                    .scale       = scale,
                                    .dot_func    = param ? sym8_dot : NULL,
                                    .user_data   = param};
    return font_char(&ctx, x, y, codePoint, NULL);
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


/*
 * A rectangle of the upright text, ox, oy from the start of its base line and w x h, as it lies on the screen when
 * the text is turned: the same turn as of the box of a glyph in font_char().
 */
static void text_rect(dgx_output_orientation_t orientation, int ox, int oy, int w, int h, int *sx, int *sy, int *sw, int *sh)
{
    if (dgx_output_orientation_swap_xy(orientation)) {
        int t = ox;
        ox = oy, oy = t;
        t = w;
        w = h, h = t;
    }
    if (dgx_output_orientation_xdir(orientation) == DgxScreenRightLeft) ox = -ox - w;
    if (dgx_output_orientation_ydir(orientation) == DgxScreenBottomTop) oy = -oy - h;
    *sx = ox, *sy = oy, *sw = w, *sh = h;
}

/*
 * The cell of a text, DGX_FONT_FILL, filled with the paper: as high as the font, yOffsetLowest to yBottomMax, from the
 * start of the line to the end of its advances, wider where a letter sticks out on either side.
 */
static void fill_cell(const dgx_font_context_t *ctx, int x, int y, const char *str)
{
    const int scale = ctx->scale ? ctx->scale : 1;
    int       pen = 0, left = 0, right = 0;
    size_t    idx = 0;
    while (str[idx]) {
        int16_t        advance;
        const glyph_t *g = dgx_font_find_glyph(decodeUTF8next(str, &idx), ctx->font, &advance);
        if (g && ctx->font->f_type != DGX_FONT_DOTS && ctx->font->f_type != DGX_FONT_HW && g->width > 0) {
            if (pen + g->xOffset * scale < left) left = pen + g->xOffset * scale;
            if (pen + (g->xOffset + g->width) * scale > right) right = pen + (g->xOffset + g->width) * scale;
        }
        pen += advance * scale;
    }
    if (pen > right) right = pen;
    const int top = ctx->font->yOffsetLowest * scale, bottom = ctx->font->yBottomMax * scale;
    if (right <= left || bottom <= top) return;
    int sx, sy, sw, sh;
    text_rect(ctx->orientation, left, top, right - left, bottom - top, &sx, &sy, &sw, &sh);
    dgx_fill_rectangle(ctx->scr, x + sx, y + sy, sw, sh, ctx->bg);
}

int dgx_font_draw(const dgx_font_context_t *ctx, int x, int y, const char *str)
{
    if (!ctx || !ctx->scr || !ctx->font || !str) return 0;
    const bool swap_xy = dgx_output_orientation_swap_xy(ctx->orientation);
    const int  step    = swap_xy ? dgx_output_orientation_ydir(ctx->orientation) : dgx_output_orientation_xdir(ctx->orientation);
    uint32_t   colors[16];
    const uint32_t *table = soft_table(ctx, colors);
    dgx_screen_progress_up(ctx->scr);
    if (ctx->flags & DGX_FONT_FILL) fill_cell(ctx, x, y, str);
    int    pen = 0;
    size_t idx = 0;
    while (str[idx]) {
        const uint32_t cp = decodeUTF8next(str, &idx);
        pen += font_char(ctx, swap_xy ? x : x + pen * step, swap_xy ? y + pen * step : y, cp, table);
    }
    dgx_screen_progress_down(ctx->scr);
    return pen;
}

int dgx_font_draw_char(const dgx_font_context_t *ctx, int x, int y, uint32_t code_point)
{
    if (!ctx || !ctx->scr || !ctx->font) return 0;
    uint32_t        colors[16];
    const uint32_t *table = soft_table(ctx, colors);
    dgx_screen_progress_up(ctx->scr);
    if (ctx->flags & DGX_FONT_FILL) {
        /* its cell as of a text of this one character */
        char one[5] = {0};
        if (code_point < 0x80) one[0] = (char)code_point;
        else if (code_point < 0x800) one[0] = (char)(0xC0 | code_point >> 6), one[1] = (char)(0x80 | (code_point & 0x3F));
        else if (code_point < 0x10000)
            one[0] = (char)(0xE0 | code_point >> 12), one[1] = (char)(0x80 | ((code_point >> 6) & 0x3F)), one[2] = (char)(0x80 | (code_point & 0x3F));
        else
            one[0] = (char)(0xF0 | code_point >> 18), one[1] = (char)(0x80 | ((code_point >> 12) & 0x3F)),
            one[2] = (char)(0x80 | ((code_point >> 6) & 0x3F)), one[3] = (char)(0x80 | (code_point & 0x3F));
        if (one[0]) fill_cell(ctx, x, y, one);
    }
    const int advance = font_char(ctx, x, y, code_point, table);
    dgx_screen_progress_down(ctx->scr);
    return advance;
}

int dgx_font_bounds(const dgx_font_context_t *ctx, const char *str, int *top, int *height)
{
    if (!ctx || !ctx->font || !str) return 0;
    const int scale = ctx->scale ? ctx->scale : 1;
    int       width = 0, yt = 0, yb = 0;
    bool      first = true;
    size_t    idx   = 0;
    while (str[idx]) {
        int16_t        advance;
        const glyph_t *g = dgx_font_find_glyph(decodeUTF8next(str, &idx), ctx->font, &advance);
        width += advance * scale;
        if (g) {
            if (first || g->yOffset < yt) yt = g->yOffset;
            if (first || g->yOffset + g->height > yb) yb = g->yOffset + g->height;
            first = false;
        }
    }
    if (top) *top = yt * scale;
    if (height) *height = (yb - yt) * scale;
    return width;
}
