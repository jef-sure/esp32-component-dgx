/*
 * Every deprecated name of DGX, used as a program of before uses it: each on
 * a line of its own that ends with "// deprecated", or "// deprecated in C"
 * where only C has it. deprecated_check.py
 * builds this file as C and as C++ and wants a deprecation warning on every
 * such line and on no other, and no error: an old program still builds, and
 * is told of each name it has to change before DGX 1.0. The list is the
 * table "Deprecated names" of docs/api-en.md.
 *
 * Nothing here runs: the file is only built.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_gauge.h"
#include "dgx_hw_font.h"
#include "dgx_matrix_morph.h"
#include "dgx_morph_sources.h"
#include "dgx_screen.h"
#include "dgx_vscreen.h"
#include "drivers/dgx_panel.h"
#include "drivers/gc9a01.h"
#include "drivers/ili9341.h"
#include "drivers/jd9165.h"
#include "drivers/jd9365.h"
#include "drivers/nv3041a.h"
#include "drivers/sh1106.h"
#include "drivers/ssd1306.h"
#include "drivers/ssd1309.h"
#include "drivers/ssd1322.h"
#include "drivers/ssd1351.h"
#include "drivers/ssd1680.h"
#include "drivers/st7565r.h"
#include "drivers/st7735.h"
#include "drivers/st7789.h"
#include "drivers/st77916.h"
#include "drivers/st7920.h"
#include "drivers/vscreen.h" // deprecated
#include "fonts/TerminusTTFMedium12.h"

/*
 * A font as font2c made it before 0.12: the glyphs by place, the font by the names of its measures then. A font is a
 * C file: as C++ its average width, a fraction, narrows, as it did before 0.12.
 */
#ifndef __cplusplus
static const uint8_t       old_bitmaps[] = {0xa0, 0x40};
static const glyph_t       old_glyphs[]  = {{{old_bitmaps + 0}, 3, 2, 4, 0, -2}}; // deprecated in C
static const glyph_array_t old_ranges[]  = {{0x41, 1, old_glyphs + 0}, {0, 0, 0}}; // deprecated in C
dgx_font_t *OldFont(void);
dgx_font_t *OldFont(void)
{
    static dgx_font_t rval = {
        .glyph_ranges  = old_ranges,
        .yAdvance      = 3,
        .yOffsetLowest = -2,
        .xWidest       = 3,
        .xWidthAverage = 3.000000,
        .f_type        = DGX_FONT_BITMAP_LINES,
        .yBottomMax    = 0,
        .xOffsetLowest = 0,
        .xRightMax     = 3
    };
    return &rval;
}
#endif

/* a program's own callbacks of the text calls of before */
static void my_dot(dgx_screen_t *scr, int x, int y, dgx_font_sym8_params_t *params) { (void)scr, (void)x, (void)y, (void)params; } // deprecated
static int  my_glyph(dgx_screen_t *scr, int16_t x, int16_t y, uint32_t cp, uint32_t color, dgx_output_orientation_t o, int scale, struct dgx_font_ *font,
                     void *param)
{
    (void)scr, (void)x, (void)y, (void)cp, (void)color, (void)o, (void)scale, (void)font, (void)param;
    return 0;
}

void old_program(dgx_screen_t *scr, dgx_screen_t *picture, dgx_bus_protocols_t *bus, dgx_font_t *font, dgx_font_t *hand, dgx_hw_morph_t *morph);
void old_program(dgx_screen_t *scr, dgx_screen_t *picture, dgx_bus_protocols_t *bus, dgx_font_t *font, dgx_font_t *hand, dgx_hw_morph_t *morph)
{
    /* figures */
    dgx_solid_circle(scr, 10, 10, 5, 1);                                    // deprecated
    dgx_solid_sector(scr, 10, 10, 5, 4, 0.0f, 90.0f, 1);                    // deprecated
    dgx_draw_triangle_solid(scr, 0, 0, 5, 0, 0, 5, 1);                      // deprecated
    dgx_draw_polygon4_solid(scr, 0, 0, 5, 0, 5, 5, 0, 5, 1);                // deprecated
    dgx_draw_line_thick(scr, 0, 0, 20, 10, 3, 1);                           // deprecated

    /* the screen and the bus */
    dgx_screen_progress_up(scr);                                            // deprecated
    dgx_screen_progress_down(scr);                                          // deprecated
    scr->wait_buffer(scr);                                                  // deprecated
    bus->dispose(bus);                                                      // deprecated

    /* the calls named after a driver */
    dgx_ili9341_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_ili9341_backlight(scr, true);                                       // deprecated
    dgx_st7789_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_st7789_backlight(scr, true);                                        // deprecated
    dgx_st7735_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_st7735_backlight(scr, true);                                        // deprecated
    dgx_gc9a01_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_gc9a01_backlight(scr, true);                                        // deprecated
    dgx_gc9a01_display_off(scr);                                            // deprecated
    dgx_gc9a01_display_on(scr);                                             // deprecated
    dgx_nv3041a_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_nv3041a_backlight(scr, true);                                       // deprecated
    dgx_st77916_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_st77916_backlight(scr, true);                                       // deprecated
    dgx_panel_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_panel_backlight(scr, true);                                         // deprecated
    dgx_ssd1351_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_ssd1306_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_ssd1309_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_sh1106_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_ssd1322_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_ssd1680_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_st7920_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_st7920_backlight(scr, true);                                        // deprecated
    dgx_st7565r_backlight(scr, true);                                       // deprecated
    dgx_jd9165_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_jd9165_backlight(scr, true);                                        // deprecated
    dgx_jd9365_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); // deprecated
    dgx_jd9365_backlight(scr, true);                                        // deprecated

    /* the names of the resolutions */
    ssd1306_resolution_t r1306 = SSD1306_128X64;                            // deprecated
    ssd1309_resolution_t r1309 = SSD1309_128X64;                            // deprecated
    sh1106_resolution_t  r1106 = SH1106_128X64;                             // deprecated
    ssd1322_resolution_t r1322 = SSD1322_256X64;                            // deprecated
    ssd1680_resolution_t r1680 = SSD1680_128X296_BWR;                       // deprecated
    st7735_resolution_t  r7735 = ST7735_RESOLUTION_128x160;                 // deprecated
    st7789_resolution_t  r7789 = ST7789_RESOLUTION_240x320;                 // deprecated
    (void)r1306, (void)r1309, (void)r1106, (void)r1322, (void)r1680, (void)r7735, (void)r7789;

    /* text */
    dgx_font_string_utf8_screen(scr, 0, 10, "a", 1, DgxOutputNormal, 1, font, my_glyph, NULL); // deprecated
    dgx_font_char_to_screen(scr, 0, 10, 'a', 1, DgxOutputNormal, 1, font, NULL, NULL); // deprecated
    dgx_font_string_bounds("a", font, NULL, NULL);                          // deprecated
    dgx_font_draw_sym_func_t glyph_func = my_glyph;                         // deprecated
    dgx_font_dot_func_t      dot_func   = my_dot;                           // deprecated
    (void)glyph_func, (void)dot_func;
    size_t at = 0;
    decodeUTF8next("a", &at);                                               // deprecated
    dgx_morph_text_length("ab");                                            // deprecated
    dgx_font_symbol_morph_t *sm = dgx_font_make_morph_struct(font, 'a', 'b', 0, 0, 10, 0, 1); // deprecated
    dgx_font_make_morph_struct_destroy(&sm);                                // deprecated
    dgx_bit_matrix_t *m = dgx_matrix_init(4, 4);                            // deprecated
    dgx_matrix_destroy(&m);

    /* fonts and glyphs */
    dgx_font_t *taken = TerminusTTFMedium12();                              // deprecated
    (void)taken;
    const glyph_t *g = dgx_font_find_glyph('a', font, NULL);                // deprecated
    const glyph_array_t *ranges = font->glyph_ranges;                       // deprecated
    int measures = font->yAdvance;                                          // deprecated
    measures += font->yOffsetLowest;                                        // deprecated
    measures += font->xWidest;                                              // deprecated
    measures += font->xWidthAverage;                                        // deprecated
    measures += font->yBottomMax;                                           // deprecated
    measures += font->xOffsetLowest;                                        // deprecated
    measures += font->xRightMax;                                            // deprecated
    dgx_font_symbol_morph_t *dm = dgx_font_morph_create(font, 'a', 'b', 0, 0, 10, 0, 1);
    if (dm) measures += (int)dm->fromCP;                                    // deprecated
    if (dm) measures += (int)dm->toCP;                                      // deprecated
    dgx_font_morph_destroy(&dm);
    (void)g, (void)ranges, (void)measures;

    /* handwriting */
    dgx_hw_writing_t w;
    dgx_hw_writing_begin(&w, scr, 0, 20, hand, "a", 2.0f, 2, 1, true, NULL);       // deprecated
    dgx_hw_writing_begin_xy(&w, scr, 0, 20, hand, "a", 2.0f, 3.0f, 2, 1, true, NULL); // deprecated
    dgx_hw_draw_text(scr, 0, 20, hand, "a", 2.0f, 2, 1, true);              // deprecated
    dgx_hw_draw_text_xy(scr, 0, 20, hand, "a", 2.0f, 3.0f, 2, 1, true);     // deprecated
    dgx_hw_morph_draw(morph, 0.5f, scr, 0, 20, 2.0f, 2, 1);                 // deprecated
    dgx_hw_morph_draw_xy(morph, 0.5f, scr, 0, 20, 2.0f, 3.0f, 2, 1);        // deprecated

    /* the gauge */
    dgx_gauge_t          gauge;
    dgx_gauge_color_fn_t gauge_fn = NULL;                                   // deprecated
    dgx_gauge_init(&gauge, scr, 20, 20, 10, 4, 225.0f, 270, 0, 100, 0, gauge_fn); // deprecated

    /* virtual screens */
    dgx_vscreen_to_screen(scr, 0, 0, picture);                              // deprecated
    dgx_vscreen_region_to_screen(scr, 0, 0, picture, 0, 0, 4, 4);           // deprecated
    dgx_vscreen_to_vscreen(scr, 0, 0, picture, true);                       // deprecated
    uint32_t flags = DGX_LUT_TRANSPARENT;                                   // deprecated
    flags |= DGX_LUT_EXACT;                                                 // deprecated
    flags |= DGX_LUT_SMOOTH;                                                // deprecated
    flags |= DGX_LUT_SCALE(2);                                              // deprecated
    (void)flags;
}

int main(void) { return 0; }
