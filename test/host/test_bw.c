#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

static int count_set(dgx_screen_t *s)
{
    int n = 0;
    for (int y = 0; y < s->height; ++y)
        for (int x = 0; x < s->width; ++x) n += !!s->get_pixel(s, x, y);
    return n;
}

static bool same_pixels(dgx_screen_t *a, dgx_screen_t *b)
{
    for (int y = 0; y < a->height; ++y)
        for (int x = 0; x < a->width; ++x)
            if (!a->get_pixel(a, x, y) != !b->get_pixel(b, x, y)) return false;
    return true;
}

static void test_bw_screen(void)
{
    dgx_screen_t *s = dgx_bw_init(32, 16);
    CHECK(s->screen_subtype == DgxVirtualBackScreen);
    CHECK(!dgx_vscreen_is_linear(s));

    /* out-of-range pixels are ignored (ASan catches the write otherwise) */
    dgx_set_pixel(s, -1, 0, 1);
    dgx_set_pixel(s, 0, -1, 1);
    dgx_set_pixel(s, 32, 0, 1);
    dgx_set_pixel(s, 0, 16, 1);
    dgx_set_pixel(s, 1000, 1000, 1);
    CHECK(count_set(s) == 0);
    CHECK(s->get_pixel(s, -1, -1) == 0 && s->get_pixel(s, 32, 16) == 0);
    dgx_set_pixel(s, 31, 15, 1);
    CHECK(s->get_pixel(s, 31, 15) == 1 && count_set(s) == 1);
    dgx_set_pixel(s, 31, 15, 0);

    /* a rectangle hanging over the top edge is clipped, not dropped */
    dgx_fill_rectangle(s, 2, -3, 4, 5, 1);
    CHECK(count_set(s) == 4 * 2);
    CHECK(s->get_pixel(s, 2, 0) && s->get_pixel(s, 5, 1) && !s->get_pixel(s, 2, 2));
    dgx_fill_rectangle(s, 0, 0, 32, 16, 0);

    /* over the other edges, and fully outside */
    dgx_fill_rectangle(s, 30, 14, 10, 10, 1);
    CHECK(count_set(s) == 2 * 2);
    dgx_fill_rectangle(s, 0, 16, 4, 4, 1);
    dgx_fill_rectangle(s, 32, 0, 4, 4, 1);
    dgx_fill_rectangle(s, -8, -8, 4, 4, 1);
    CHECK(count_set(s) == 2 * 2);
    dgx_fill_rectangle(s, -5, -5, 100, 100, 0);
    CHECK(count_set(s) == 0);

    dgx_bw_fast_vline(s, 3, -4, 100, 1);
    CHECK(count_set(s) == 16);
    dgx_bw_fast_vline(s, 40, 0, 4, 1);
    CHECK(count_set(s) == 16);

    /* shapes crossing the border go through the same primitives */
    dgx_draw_line(s, -10, -10, 50, 30, 1);
    dgx_solid_circle(s, 31, 15, 9, 1);
    dgx_draw_circle(s, 0, 0, 12, 1);

    /* writing more than the area holds wraps inside the area */
    dgx_fill_rectangle(s, 0, 0, 32, 16, 0);
    uint8_t ones[8];
    memset(ones, 0xff, sizeof(ones));
    s->set_area(s, 0, 7, 14, 15);
    s->write_area(s, ones, 16);
    s->write_area(s, ones, 64);
    CHECK(count_set(s) == 16);

    dgx_screen_destroy(&s);
}

/* Thin thick-lines used to lose the pieces that started above the top edge. */
static void test_bw_thick_lines(void)
{
    for (int width = 1; width <= 3; ++width) {
        for (int dx = -12; dx <= 12; dx += 3) {
            for (int dy = -12; dy <= 12; dy += 3) {
                dgx_screen_t *bw  = dgx_bw_init(32, 32);
                dgx_screen_t *ref = dgx_vscreen_init(32, 32, 8, DgxScreenRGB);
                dgx_draw_line_thick(bw, 2, 2, 2 + dx, 2 + dy, width, 1);
                dgx_draw_line_thick(ref, 2, 2, 2 + dx, 2 + dy, width, 1);
                CHECK(same_pixels(ref, bw));
                dgx_screen_destroy(&bw);
                dgx_screen_destroy(&ref);
            }
        }
    }
}

/* Text on a page-organised screen must match the same text on a linear one. */
static void test_bw_text(void)
{
    for (int width = 40; width <= 43; ++width) {
        dgx_screen_t *bw  = dgx_bw_init(width, 24);
        dgx_screen_t *lin = dgx_vscreen_init(width, 24, 1, DgxScreenRGB);
        dgx_screen_t *ref = dgx_vscreen_init(width, 24, 8, DgxScreenRGB);
        CHECK(dgx_vscreen_is_linear(lin));
        for (int x = -6; x <= width - 6; x += 9) {
            dgx_font_string_utf8_screen(bw, x, 14, "Ag", 1, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
            dgx_font_string_utf8_screen(lin, x, 14, "Ag", 1, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
            dgx_font_string_utf8_screen(ref, x, 14, "Ag", 1, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
        }
        CHECK(count_set(ref) > 20);
        CHECK(same_pixels(ref, bw));
        CHECK(same_pixels(ref, lin));
        /* raw copies between the two layouts are refused */
        CHECK(!dgx_vscreen_copy(bw, lin) || bw->height % 8 != 0);
        CHECK(dgx_vscreen_clone(bw) == NULL);
        dgx_screen_destroy(&bw);
        dgx_screen_destroy(&lin);
        dgx_screen_destroy(&ref);
    }
}

/* Rectangles on both 1-bit layouts against a pixel-by-pixel reference, set and clear, every alignment. */
static void test_fills(void)
{
    static const int sizes[][2] = {{1, 1}, {3, 2}, {7, 9}, {8, 8}, {9, 7}, {16, 17}, {29, 23}};
    for (int kind = 0; kind < 2; ++kind) {
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
            for (int x = -2; x < 12; ++x) {
                for (int y = -2; y < 12; ++y) {
                    for (int color = 0; color < 2; ++color) {
                        /* 29 wide: rows of the linear layout do not start on a byte */
                        dgx_screen_t *got = kind ? dgx_bw_init(29, 26) : dgx_vscreen_init(29, 26, 1, DgxScreenRGB);
                        dgx_screen_t *ref = dgx_vscreen_init(29, 26, 8, DgxScreenRGB);
                        if (!color) {
                            dgx_fill_rectangle(got, 0, 0, 29, 26, 1);
                            dgx_fill_rectangle(ref, 0, 0, 29, 26, 1);
                        }
                        dgx_fill_rectangle(got, x, y, sizes[k][0], sizes[k][1], (uint32_t)color);
                        for (int ry = y; ry < y + sizes[k][1]; ++ry)
                            for (int rx = x; rx < x + sizes[k][0]; ++rx) dgx_set_pixel(ref, rx, ry, (uint32_t)color);
                        if (!same_pixels(ref, got)) {
                            fprintf(stderr, "fill: kind %d, %dx%d at %d,%d color %d differs\n", kind, sizes[k][0], sizes[k][1], x, y, color);
                            ++check_failures;
                        }
                        dgx_screen_destroy(&got);
                        dgx_screen_destroy(&ref);
                    }
                }
            }
        }
    }
}

/* One pixel at a time, the way glyphs were drawn before runs were merged. */
static int reference_char(dgx_screen_t *scr, int x, int y, uint32_t cp, dgx_output_orientation_t o, int scale, dgx_font_t *font)
{
    dgx_orientation_t xdir = dgx_output_orientation_xdir(o);
    dgx_orientation_t ydir = dgx_output_orientation_ydir(o);
    bool              swap = dgx_output_orientation_swap_xy(o);
    int16_t           advance;
    const glyph_t    *g = dgx_font_find_glyph(cp, font, &advance);
    if (!g) return advance;
    dgx_bw_bitmap_t bmap   = dgx_bw_bitmap_make_of((uint8_t *)g->bitmap, g->width, g->height, font->f_type == DGX_FONT_BITMAP_STREAM);
    int             right  = g->width - 1, bottom = g->height - 1;
    int             x_shift = g->xOffset * scale, y_shift = g->yOffset * scale;
    if (swap) {
        int t   = x_shift;
        x_shift = y_shift;
        y_shift = t;
        t       = right;
        right   = bottom;
        bottom  = t;
    }
    dgx_point_2d_t p = _dgx_start_area_pixel(0, right, 0, bottom, xdir, ydir);
    for (int by = 0; by < g->height; ++by) {
        for (int bx = 0; bx < g->width; ++bx) {
            if (dgx_bw_bitmap_get_pixel(&bmap, bx, by)) {
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx) dgx_set_pixel(scr, x + x_shift + p.x * scale + dx, y + y_shift + p.y * scale + dy, 1);
            }
            p = _dgx_move_to_next_area_pixel(p, 0, right, 0, bottom, xdir, ydir, swap);
        }
    }
    return advance * scale;
}

static void test_text_orientations(void)
{
    dgx_font_t *font = TerminusTTFMedium12();
    for (int o = DgxOutputNormal; o <= DgxOutputTransverse; ++o) {
        for (int scale = 1; scale <= 3; ++scale) {
            dgx_screen_t *got = dgx_vscreen_init(90, 90, 8, DgxScreenRGB);
            dgx_screen_t *ref = dgx_vscreen_init(90, 90, 8, DgxScreenRGB);
            dgx_font_string_utf8_screen(got, 45, 45, "Ag#1", 1, (dgx_output_orientation_t)o, scale, font, NULL, NULL);
            int  x = 45, y = 45;
            bool swap = dgx_output_orientation_swap_xy((dgx_output_orientation_t)o);
            for (const char *c = "Ag#1"; *c; ++c) {
                int step = reference_char(ref, x, y, (uint32_t)*c, (dgx_output_orientation_t)o, scale, font);
                if (swap) y += step * dgx_output_orientation_ydir((dgx_output_orientation_t)o);
                else x += step * dgx_output_orientation_xdir((dgx_output_orientation_t)o);
            }
            CHECK(count_set(ref) > 30);
            if (!same_pixels(ref, got)) {
                fprintf(stderr, "text: orientation %d, scale %d differs\n", o, scale);
                ++check_failures;
            }
            dgx_screen_destroy(&got);
            dgx_screen_destroy(&ref);
        }
    }
}

static void test_utf8(void)
{
    /* exact-size heap copies so that ASan sees any read past the terminator */
    static const char *const broken[] = { "a\xd0", "\xe2\x82", "\xf0\x9f", "\xf0", "\xe2", "\xd0" };
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        size_t len = strlen(broken[i]);
        char  *str = malloc(len + 1);
        memcpy(str, broken[i], len + 1);
        size_t idx = 0;
        int    n   = 0;
        while (str[idx] && n < 10) {
            decodeUTF8next(str, &idx);
            ++n;
        }
        CHECK(idx == len);
        dgx_font_string_bounds(str, TerminusTTFMedium12(), NULL, NULL);
        free(str);
    }
    /* a character after a broken sequence is not swallowed */
    size_t idx = 0;
    decodeUTF8next("\xd0Z", &idx);
    CHECK(idx == 1);
    CHECK(decodeUTF8next("\xd0Z", &idx) == 'Z');
    idx = 0;
    CHECK(decodeUTF8next("\xd0\x96", &idx) == 0x416 && idx == 2);
    idx = 0;
    CHECK(decodeUTF8next("\xe2\x82\xac", &idx) == 0x20ac && idx == 3);
    idx = 0;
    CHECK(decodeUTF8next("\xf0\x9f\x98\x80", &idx) == 0x1f600 && idx == 4);
}

static void test_packed_depths(void)
{
    /* 4 bits: value lives in the high nibble */
    uint8_t  buf4[2] = { 0, 0 };
    uint8_t *lp      = buf4;
    lp               = dgx_fill_buf_value_4(lp, 0, 0xa0);
    lp               = dgx_fill_buf_value_4(lp, 1, 0x50);
    lp               = dgx_fill_buf_value_4(lp, 2, 0xf0);
    CHECK(buf4[0] == 0xa5 && buf4[1] == 0xf0);
    lp = buf4;
    CHECK(dgx_read_buf_value_4(&lp, 0) == 0xa0);
    CHECK(dgx_read_buf_value_4(&lp, 1) == 0x50);
    CHECK(dgx_read_buf_value_4(&lp, 2) == 0xf0);

    for (int bits = 4; bits <= 12; bits += 8) {
        /* odd sizes: the last pixel ends exactly at the end of the buffer */
        dgx_screen_t *s = dgx_vscreen_init(5, 3, (uint8_t)bits, DgxScreenRGB);
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < 15; ++i) {
                int      p     = pass ? 14 - i : i; /* both write orders: neighbours must survive */
                uint32_t color = bits == 4 ? (uint32_t)((p + 1) << 4) : DGX_RGB_12((p * 16 + 16) & 0xff, (p * 48) & 0xff, 0xf0 - p * 16);
                dgx_set_pixel(s, p % 5, p / 5, color);
            }
            for (int p = 0; p < 15; ++p) {
                uint32_t color = bits == 4 ? (uint32_t)((p + 1) << 4) : DGX_RGB_12((p * 16 + 16) & 0xff, (p * 48) & 0xff, 0xf0 - p * 16);
                CHECK(s->get_pixel(s, p % 5, p / 5) == color);
            }
        }
        uint32_t fill = bits == 4 ? 0x70 : DGX_RGB_12(0x10, 0x20, 0x30);
        uint32_t keep = s->get_pixel(s, 0, 1);
        dgx_fill_rectangle(s, 1, 1, 3, 2, fill);
        CHECK(s->get_pixel(s, 1, 1) == fill && s->get_pixel(s, 3, 2) == fill);
        CHECK(s->get_pixel(s, 0, 1) == keep && s->get_pixel(s, 4, 2) != fill);
        dgx_screen_destroy(&s);
    }
}

static void test_font_morph(void)
{
    dgx_font_t *font = TerminusTTFMedium12();
    int16_t     adv;
    CHECK(dgx_font_find_glyph('A', font, &adv) != NULL);
    CHECK(dgx_font_find_glyph(0x10ffff, font, &adv) == NULL);

    dgx_font_symbol_morph_t *m = dgx_font_make_morph_struct(font, 'A', 0x10ffff, 0, 12, 20, 12, 1);
    CHECK(m && m->number_of_dots > 0 && !m->is_from_empty && m->is_to_empty);
    if (m) dgx_font_make_morph_struct_destroy(&m);

    m = dgx_font_make_morph_struct(font, 0x10ffff, 'A', 0, 12, 20, 12, 1);
    CHECK(m && m->number_of_dots > 0 && m->is_from_empty && !m->is_to_empty);
    if (m) dgx_font_make_morph_struct_destroy(&m);

    m = dgx_font_make_morph_struct(font, 0x10ffff, 0x10fffe, 0, 12, 20, 12, 1);
    CHECK(m && m->number_of_dots == 0 && m->is_from_empty && m->is_to_empty);
    if (m) dgx_font_make_morph_struct_destroy(&m);
}

/* src pixel (x, y) holds x + 16 * y + 1; dst starts at zero */
static bool region_matches(dgx_screen_t *dst, int x_dst, int y_dst, int x_src, int y_src, int w, int h,
                           dgx_output_orientation_t o)
{
    for (int y = 0; y < dst->height; ++y) {
        for (int x = 0; x < dst->width; ++x) {
            int ox = x - x_dst, oy = y - y_dst; /* position inside the output rectangle */
            int sx, sy;
            switch (o) {
            case DgxOutputMirrorX: sx = w - 1 - ox, sy = oy; break;
            case DgxOutputMirrorY: sx = ox, sy = h - 1 - oy; break;
            case DgxOutputRotate180: sx = w - 1 - ox, sy = h - 1 - oy; break;
            case DgxOutputTranspose: sx = oy, sy = ox; break;
            case DgxOutputRotate90CCW: sx = w - 1 - oy, sy = ox; break;
            case DgxOutputRotate90CW: sx = oy, sy = h - 1 - ox; break;
            case DgxOutputTransverse: sx = w - 1 - oy, sy = h - 1 - ox; break;
            default: sx = ox, sy = oy; break;
            }
            uint32_t want = 0;
            if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
                int ax = x_src + sx, ay = y_src + sy;
                if (ax >= 0 && ay >= 0 && ax < 8 && ay < 8) want = (uint32_t)(ax + 16 * ay + 1);
            }
            if (dst->get_pixel(dst, x, y) != want) return false;
        }
    }
    return true;
}

static void test_region_crop(void)
{
    static const int cases[][6] = {
        /* x_dst, y_dst, x_src, y_src, w, h */
        { 2, 3, 1, 2, 4, 3 },     /* nothing cropped */
        { 4, 4, -2, -3, 6, 7 },   /* source hangs over its left/top edge */
        { 1, 1, 5, 6, 6, 5 },     /* source hangs over its right/bottom edge */
        { -3, -2, 1, 1, 6, 6 },   /* destination hangs over its left/top edge */
        { 9, 8, 0, 0, 8, 8 },     /* destination hangs over its right/bottom edge */
        { -2, 7, -3, 4, 12, 12 }, /* both at once */
    };
    dgx_screen_t *src = dgx_vscreen_init(8, 8, 8, DgxScreenRGB);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) dgx_set_pixel(src, x, y, (uint32_t)(x + 16 * y + 1));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const int *c = cases[i];
        for (int o = DgxOutputNormal; o <= DgxOutputTransverse; ++o) {
            dgx_screen_t *dst = dgx_vscreen_init(12, 11, 8, DgxScreenRGB);
            dgx_vscreen_region_to_screen_oriented(dst, c[0], c[1], src, c[2], c[3], c[4], c[5], (dgx_output_orientation_t)o);
            if (!region_matches(dst, c[0], c[1], c[2], c[3], c[4], c[5], (dgx_output_orientation_t)o)) {
                fprintf(stderr, "region crop: case %d, orientation %d\n", (int)i, o);
                ++check_failures;
            }
            dgx_screen_destroy(&dst);
        }
    }
    dgx_screen_destroy(&src);
}

int main(void)
{
    test_bw_screen();
    test_bw_thick_lines();
    test_bw_text();
    test_fills();
    test_text_orientations();
    test_utf8();
    test_packed_depths();
    test_font_morph();
    test_region_crop();
    CHECK_DONE();
}
