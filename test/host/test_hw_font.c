#include <stdint.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_hw_font.h"
#include "dgx_morph_sources.h"
#include "dgx_vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

/* the calls of before, deprecated until DGX 1.0, are checked here beside those of now; they warn everywhere else */
DGX_DEPRECATED_USE_BEGIN

/* a font as font2c writes it: "1", "7" with a postponed bar, a lowercase letter with both connections */
static const dgx_hw_element_t elements[] = {
    {DGX_HW_LINE, 1, 12000, 120, {{110, 67}, {110, 187}}},
    {DGX_HW_CURVE, 6, 9050, 80, {{80, 70}, {150, 60}, {120, 120}, {100, 187}}},
    {DGX_HW_LINE, 1, 4000, 40, {{90, 130}, {130, 130}}},
    {DGX_HW_CURVE3P, 2, 2000, 18, {{67, 158}, {74, 150}, {78, 133}}},
    {DGX_HW_CURVE, 9, 14000, 125, {{78, 133}, {120, 120}, {60, 200}, {110, 187}}},
    {DGX_HW_DOT, 0, 0, 1, {{110, 187}}},
    {DGX_HW_CURVE, 3, 3100, 28, {{110, 187}, {120, 185}, {128, 170}, {134, 158}}},
};

static const dgx_hw_symbol_t symbols[] = {
    {0, 1, 0, 0, 1, -120, 0, 44, 44, 44, 44, 1, 44, 44, 17, 0, 16},
    {0, 1, 0, 1, 51, -121, 0, 14, 64, 34, 64, 31, 14, 64, 51, 8, 12},
    {1, 2, 1, 0, 40, -58, 0, 12, 51, 12, 51, 40, DGX_HW_NONE, DGX_HW_NONE, 56, 0, 16},
};

static const dgx_glyph_t glyphs[] = {
    {     0, 1, 121, 17, 0, -120, 1 },
    {    14, 51, 122, 51, -12, -121, 2 },
    {    42, 40, 59, 56, 0, -58, 4 },
};

static const dgx_glyph_array_t glyph_ranges[] = {
    {0x31, 1, 0},
    {0x37, 1, 1},
    {0x430, 1, 2},
    {0, 0, 0},
};

static const dgx_hw_font_t hw = {
    .name = "test",
    .format_version = DGX_HW_FONT_FORMAT_VERSION,
    .base_line = 139,
    .x_height = 58,
    .symbol_space = 16,
    .space_width = 40,
    .symbol_offset_x = 66,
    .symbol_offset_y = 48,
    .symbol_size_x = 100,
    .symbol_size_y = 202,
    .symbols = symbols,
};

static dgx_font_t font = {
    .glyph_ranges = glyph_ranges,
    .y_advance = 202,
    .y_offset_lowest = -121,
    .x_widest = 51,
    .x_width_average = 31,
    .f_type = DGX_FONT_HW,
    .y_bottom_max = 1,
    .x_offset_lowest = -12,
    .x_right_max = 40,
    .number_of_ranges = 3,
    .hw = &hw,
    .glyphs = glyphs,
    .data = (const uint8_t *)elements,
};

int main(void)
{
    /* a handwritten symbol is found as any glyph and leads to its path and measures */
    int16_t        advance = 0;
    const dgx_glyph_t *g = dgx_font_glyph(&font, 0x430, &advance);
    CHECK(g == glyphs + 2 && advance == 56);
    CHECK(dgx_hw_glyph_symbol(&font, g)->begin_connection == 1 && dgx_hw_glyph_symbol(&font, g)->main_segments == 2 && dgx_hw_glyph_symbol(&font, g)->end_connection == 1 && dgx_hw_glyph_symbol(&font, g)->post_segments == 0);
    CHECK(dgx_hw_glyph_elements(&font, g)[0].type == DGX_HW_CURVE3P && dgx_hw_glyph_elements(&font, g)[1].points[3].y == 187 && dgx_hw_glyph_elements(&font, g)[2].type == DGX_HW_DOT);
    CHECK(dgx_hw_glyph_symbol(&font, g)->up_left == DGX_HW_NONE && g->x_offset == dgx_hw_glyph_symbol(&font, g)->left - dgx_hw_glyph_symbol(&font, g)->line_left && g->y_offset == dgx_hw_glyph_symbol(&font, g)->top);
    CHECK(dgx_hw_glyph_symbol(&font, g)->space_before == 0 && dgx_hw_glyph_symbol(&font, g)->space_after == font.hw->symbol_space);
    CHECK(g->x_advance == dgx_hw_glyph_symbol(&font, g)->line_width + dgx_hw_glyph_symbol(&font, g)->space_after);
    g = dgx_font_glyph(&font, 0x37, &advance);
    CHECK(g == glyphs + 1 && dgx_hw_glyph_symbol(&font, g)->post_segments == 1 && g->height == dgx_hw_glyph_symbol(&font, g)->bottom - dgx_hw_glyph_symbol(&font, g)->top + 1);
    CHECK(g->x_offset == dgx_hw_glyph_symbol(&font, g)->space_before + dgx_hw_glyph_symbol(&font, g)->left - dgx_hw_glyph_symbol(&font, g)->line_left);
    CHECK(g->x_advance == dgx_hw_glyph_symbol(&font, g)->space_before + dgx_hw_glyph_symbol(&font, g)->line_width + dgx_hw_glyph_symbol(&font, g)->space_after);
    CHECK(font.hw->base_line == 139 && font.hw->format_version == 1);

    /* the effort of an element is length + k * pieces in cells; a dot takes the least one, nothing takes less */
    dgx_hw_pace_t even = {0, 0}, pace = {4, 30};
    CHECK(dgx_hw_element_effort(&elements[1], &even) == 90.5f && dgx_hw_element_effort(&elements[1], &pace) == 90.5f + 4 * 6);
    CHECK(dgx_hw_element_effort(&elements[0], &pace) == 120 + 4);
    CHECK(dgx_hw_element_effort(&elements[5], &even) == 0 && dgx_hw_element_effort(&elements[5], &pace) == 30);
    CHECK(dgx_hw_element_effort(&elements[3], &even) == 20 && dgx_hw_element_effort(&elements[3], &pace) == 30);

    /* by halving and in order: the same glyph for every code, the first glyph's advance for a missing one */
    dgx_font_t in_order = font;
    in_order.number_of_ranges = 0;
    for (uint32_t cp = 0; cp < 0x500; ++cp) {
        int16_t        a1 = -1, a2 = -1;
        const dgx_glyph_t *g1 = dgx_font_glyph(&font, cp, &a1);
        const dgx_glyph_t *g2 = dgx_font_glyph(&in_order, cp, &a2);
        CHECK(g1 == g2 && a1 == a2);
        CHECK((g1 != 0) == (cp == 0x31 || cp == 0x37 || cp == 0x430));
    }
    const dgx_font_t *bitmap_font = &TerminusTTFMedium12;
    dgx_font_t  halved = *bitmap_font;
    for (const dgx_glyph_array_t *r = bitmap_font->glyph_ranges; r->number; ++r) ++halved.number_of_ranges;
    for (uint32_t cp = 0; cp < 0x3000; ++cp) {
        int16_t a1 = -1, a2 = -1;
        CHECK(dgx_font_glyph(bitmap_font, cp, &a1) == dgx_font_glyph(&halved, cp, &a2) && a1 == a2);
    }

    /* the functions of bitmap and dot fonts leave it alone */
    dgx_screen_t *s = dgx_vscreen_init(64, 64, 8, DgxScreenRGB);
    CHECK(dgx_font_char_to_screen(s, 10, 40, 0x31, 0xff, DgxOutputNormal, 1, &font, NULL, NULL) == 17);
    dgx_font_string_utf8_screen(s, 10, 40, "17", 0xff, DgxOutputNormal, 1, &font, NULL, NULL);
    int set = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) set += dgx_get_pixel(s, x, y) != 0;
    }
    CHECK(set == 0);
    dgx_font_symbol_morph_t *morph = dgx_font_morph_create(&font, 0x31, 0x37, 0, 0, 0, 0, 1);
    CHECK(morph == NULL);
    /* what it gives back is destroyed as any morph is */
    dgx_font_morph_destroy(&morph);
    dgx_font_morph_destroy(NULL);
    CHECK(dgx_morph_glyph_matrix(&font, 0x31) == NULL);
    dgx_screen_destroy(&s);
    CHECK_DONE();
}

DGX_DEPRECATED_USE_END
