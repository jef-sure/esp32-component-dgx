#include <stdint.h>

#include "check.h"
#include "dgx_hw_font.h"

/* font0-ss: the numbers below are of the font as it is in src/fonts */
#include "fonts/font0ss.h"

typedef struct {
    const glyph_t *g;
    int            shift;  /* of the points of the symbol */
    int            origin; /* where the left edge of its active area stands in the line */
    int            left, right;       /* of its drawing in the line */
    int            up_left, up_right; /* of its part above the lowercase band */
    int            line_left;
} placed_t;

static dgx_font_t          font;
static const dgx_hw_font_t *fhw;

static int place(const char *text, placed_t *out, dgx_hw_line_t *line)
{
    size_t idx = 0;
    int    n = 0;
    dgx_hw_line_begin(line, &font);
    while (text[idx]) {
        placed_t p = {0};
        p.g = dgx_hw_line_place(line, decodeUTF8next(text, &idx), &p.shift);
        if (p.g) {
            const dgx_hw_symbol_t *s = p.g->hw;
            p.origin = p.shift + fhw->symbol_offset_x;
            p.left = p.origin + s->left;
            p.right = p.origin + s->right;
            p.line_left = p.origin + s->line_left;
            p.up_left = s->up_left == DGX_HW_NONE ? 0 : p.origin + s->up_left;
            p.up_right = s->up_left == DGX_HW_NONE ? 0 : p.origin + s->up_right;
        }
        out[n++] = p;
    }
    return n;
}

int main(void)
{
    placed_t      p[16];
    dgx_hw_line_t line;

    font = *font0ss();
    fhw = font.hw;

    /* no symbol keeps the mark of an absent space: the converter puts the space of the font there */
    for (const glyph_array_t *r = font.glyph_ranges; r->number; ++r) {
        for (int i = 0; i < r->number; ++i) {
            const dgx_hw_symbol_t *s = r->glyphs[i].hw;
            CHECK(s->space_after >= 0 && s->space_before >= 0);
            CHECK(r->glyphs[i].xAdvance == s->space_before + s->line_width + s->space_after);
            CHECK(r->glyphs[i].xOffset == s->space_before + s->left - s->line_left);
        }
    }

    /* "Го": the first symbol is written whole from the start of the line, without its space before */
    CHECK(place("Го", p, &line) == 2 && p[0].g && p[1].g);
    CHECK(p[0].left == 0 && p[0].origin == -11);
    /* the line goes on by xAdvance - xOffset: line_left - left + line_width + space_after */
    CHECK(p[1].line_left == p[0].g->xAdvance - p[0].g->xOffset && p[1].line_left == 53);
    /* "о" has nothing above the band and stands under the bar of "Г" */
    CHECK(p[1].g->hw->up_left == DGX_HW_NONE && p[1].origin == 39);
    CHECK(p[1].left < p[0].up_right && p[0].up_right == 79);
    CHECK(line.pen == 53 + p[1].g->xAdvance && line.has_up && line.up == 96);

    /* "ГЧ": by the band "Ч" would stand at 9 and run into the bar; it is moved behind it */
    CHECK(place("ГЧ", p, &line) == 2);
    CHECK(p[1].origin == 96);
    CHECK(p[1].up_left == 96 + p[1].g->hw->space_before && p[1].up_left - p[0].up_right - 1 == 16 + 8);
    CHECK(line.pen == p[1].line_left + p[1].g->hw->line_width + p[1].g->hw->space_after && line.pen == 203);

    /* "WWW": between a curl and the next W there is space_after + space_before */
    CHECK(place("WWW", p, &line) == 3);
    CHECK(p[0].origin == -8 && p[1].origin == 100 && p[2].origin == 208);
    CHECK(p[1].up_left - p[0].up_right - 1 == 24 && p[2].up_left - p[1].up_right - 1 == 24);
    CHECK(p[0].g->hw->space_after + p[1].g->hw->space_before == 24);

    /* a space and a symbol the font has not take their room, give no glyph and end the part above the band */
    CHECK(place("аб в", p, &line) == 4);
    CHECK(p[0].origin == -21 && p[1].origin == 72 && !p[2].g && p[3].origin == 173);
    CHECK(p[3].line_left == p[1].line_left + p[1].g->hw->line_width + p[1].g->hw->space_after + fhw->space_width);
    CHECK(place("б@Г", p, &line) == 3 && !p[1].g);
    CHECK(p[2].line_left == p[0].line_left + p[0].g->hw->line_width + p[0].g->hw->space_after + fhw->symbol_size_x +
                                p[2].g->hw->space_before);
    /* a symbol after a space is not the first of the line */
    CHECK(place(" Г", p, &line) == 2 && p[1].line_left == fhw->space_width + p[1].g->hw->space_before);

    CHECK_DONE();
}
