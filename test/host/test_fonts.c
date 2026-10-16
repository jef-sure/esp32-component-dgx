#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_font.h"
#include "drivers/vscreen.h"
#include "fonts/ArialRegular12.h"
#include "fonts/CasusDotView.h"
#include "fonts/DroidSansRegular9.h"
#include "fonts/IBMCGALight8x16Light8x1616.h"
#include "fonts/IBMPlexMonoMedium28.h"
#include "fonts/NotoMonoRegular28.h"
#include "fonts/TerminusTTFMedium12.h"
#include "fonts/VerdanaRegular32.h"
#include "fonts/WeatherDotView25.h"
#include "fonts/WeatherIconsRegular13.h"
#include "fonts/WeatherIconsRegular27.h"
#include "fonts/font0ss.h"

/* Generated metrics must match a scan over the glyphs. */
static void check_metrics(const char *name, dgx_font_t *font)
{
    int left = 0, top = 0, right = 0, bottom = 0;
    bool any = false;
    for (const glyph_array_t *r = font->glyph_ranges; r && r->number; ++r) {
        for (int i = 0; i < r->number; ++i) {
            const glyph_t *g = &r->glyphs[i];
            if (g->width <= 0 || g->height <= 0) continue;
            int l = g->xOffset, t = g->yOffset, rr = l + g->width, b = t + g->height;
            if (!any || l < left) left = l;
            if (!any || t < top) top = t;
            if (!any || rr > right) right = rr;
            if (!any || b > bottom) bottom = b;
            any = true;
        }
    }
    CHECK(any);
    if (font->xOffsetLowest != left || font->yOffsetLowest != top ||
        font->xRightMax != right || font->yBottomMax != bottom) {
        fprintf(stderr, "%s: font box %d,%d..%d,%d, glyph scan %d,%d..%d,%d\n", name,
                font->xOffsetLowest, font->yOffsetLowest, font->xRightMax, font->yBottomMax,
                left, top, right, bottom);
        ++check_failures;
    }
}

static void check_covers(dgx_font_t *font, const char *text)
{
    size_t len = strlen(text), idx = 0;
    while (idx < len) {
        uint32_t cp = decodeUTF8next(text, &idx);
        int16_t advance;
        if (!dgx_font_find_glyph(cp, font, &advance)) {
            fprintf(stderr, "missing U+%04X in \"%s\"\n", (unsigned)cp, text);
            ++check_failures;
        }
    }
}

/*
 * Drawing a string moves the pen as far as dgx_font_string_bounds() measures
 * it times the scale (0 counts as 1), a character the font lacks included.
 */
static void check_advance(const char *name, dgx_font_t *font, const char *text)
{
    dgx_screen_t *scr = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
    CHECK(scr != NULL);
    int width = dgx_font_string_bounds(text, font, NULL, NULL);
    for (int scale = 0; scale <= 4; ++scale) {
        int pen = 0;
        size_t idx = 0;
        while (text[idx]) {
            uint32_t cp = decodeUTF8next(text, &idx);
            pen += dgx_font_char_to_screen(scr, 0, 32, cp, 0xffff, DgxOutputNormal, scale, font, NULL, NULL);
        }
        int want = width * (scale ? scale : 1);
        if (pen != want) {
            fprintf(stderr, "%s, \"%s\" at scale %d: drawn %d, measured %d\n", name, text, scale, pen, want);
            ++check_failures;
        }
    }
    dgx_screen_destroy(&scr);
}

int main(void)
{
#define FONT(f) check_metrics(#f, f())
    FONT(ArialRegular12);
    FONT(CasusDotView);
    FONT(DroidSansRegular9);
    FONT(IBMCGALight8x16Light8x1616);
    FONT(IBMPlexMonoMedium28);
    FONT(NotoMonoRegular28);
    FONT(TerminusTTFMedium12);
    FONT(VerdanaRegular32);
    FONT(WeatherDotView25);
    FONT(WeatherIconsRegular13);
    FONT(WeatherIconsRegular27);
#undef FONT

    /* words of examples/morph_demo */
    check_covers(TerminusTTFMedium12(), " приветучастникамсоревнований");

    /* U+2014 and U+10FFFF are in none of these fonts */
    CHECK(dgx_font_find_glyph(0x2014, TerminusTTFMedium12(), NULL) == NULL);
    check_advance("TerminusTTFMedium12", TerminusTTFMedium12(), "\u2014 \u041f\u0440\u0438\u0432\u0435\u0442 \U0010FFFF!");
    check_advance("CasusDotView", CasusDotView(), "1\U0010FFFF2");
    check_advance("font0ss", font0ss(), "a\U0010FFFFb");

    /* the advance is not always wanted */
    CHECK(dgx_font_find_glyph('A', TerminusTTFMedium12(), NULL) != NULL);
    CHECK(dgx_font_find_glyph(0x10FFFF, TerminusTTFMedium12(), NULL) == NULL);
    CHECK_DONE();
}
