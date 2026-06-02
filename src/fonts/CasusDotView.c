#include "dgx_font.h"

static const dgx_font_dot_t dots[] = {
    {2, 2 },
    {1, 3 },
    {1, 4 },
    {5, 4 },
    {5, 5 },
    {5, 6 },
    {4, 6 },
    {3, 7 },
    {2, 8 },
    {1, 6 },
    {1, 5 },
    {3, 2 },
    {4, 2 },
    {5, 3 }, //
    {5, 10},
    {5, 11},
    {5, 9 },
    {5, 8 },
    {5, 7 },
    {4, 12},
    {3, 12},
    {2, 12},
    {1, 11},
    {1, 10},
    {1, 9 },
    {1, 8 },
    {1, 7 }, // '0'
    {1, 4 },
    {2, 3 },
    {3, 2 },
    {3, 3 },
    {3, 4 },
    {3, 5 },
    {3, 6 },
    {3, 7 },
    {3, 8 },
    {3, 9 },
    {3, 10},
    {3, 11},
    {1, 12},
    {2, 12}, //
    {3, 12},
    {4, 12},
    {5, 12}, // '1'
    {1, 4 },
    {1, 3 },
    {2, 2 },
    {3, 2 },
    {4, 2 },
    {5, 3 },
    {5, 4 },
    {5, 5 },
    {5, 6 },
    {5, 7 },
    {4, 8 },
    {3, 9 },
    {2, 10},
    {1, 11}, //
    {1, 12},
    {2, 12},
    {3, 12},
    {4, 12},
    {5, 12}, // '2'
    {1, 4 },
    {1, 3 },
    {2, 2 },
    {3, 2 },
    {4, 2 },
    {5, 3 },
    {5, 4 },
    {5, 5 },
    {5, 6 },
    {4, 7 },
    {3, 7 },
    {5, 8 },
    {5, 9 },
    {5, 10}, //
    {5, 11},
    {4, 12},
    {3, 12},
    {2, 12},
    {1, 11},
    {1, 10}, // '3'
    {1, 2 },
    {1, 3 },
    {1, 4 },
    {1, 5 },
    {1, 6 },
    {1, 7 },
    {2, 7 },
    {3, 7 },
    {4, 7 },
    {5, 7 },
    {6, 7 },
    {5, 3 },
    {5, 4 },
    {5, 5 }, //
    {5, 6 },
    {5, 8 },
    {5, 9 },
    {5, 10},
    {5, 11},
    {5, 12}, // '4'
    {1, 2 },
    {1, 3 },
    {1, 4 },
    {1, 5 },
    {1, 6 },
    {1, 7 },
    {2, 6 },
    {3, 6 },
    {4, 6 },
    {5, 7 },
    {5, 8 },
    {2, 2 },
    {3, 2 },
    {4, 2 }, //
    {5, 2 },
    {5, 9 },
    {5, 10},
    {5, 11},
    {4, 12},
    {3, 12},
    {2, 12},
    {1, 11}, // '5'
    {2, 2 },
    {1, 3 },
    {1, 4 },
    {1, 5 },
    {1, 6 },
    {1, 7 },
    {2, 6 },
    {3, 6 },
    {4, 6 },
    {5, 7 },
    {5, 8 },
    {3, 2 },
    {4, 2 },
    {5, 3 }, //
    {5, 9 },
    {5, 10},
    {5, 11},
    {4, 12},
    {3, 12},
    {2, 12},
    {1, 11},
    {1, 10},
    {1, 9 },
    {1, 8 }, // '6'
    {2, 2 },
    {1, 2 },
    {1, 3 },
    {5, 3 },
    {5, 4 },
    {5, 5 },
    {4, 6 },
    {3, 7 },
    {3, 8 },
    {3, 9 },
    {3, 10},
    {3, 2 },
    {4, 2 },
    {5, 2 }, //
    {3, 11},
    {3, 12}, // '7'
    {2, 2 },
    {1, 3 },
    {1, 4 },
    {5, 4 },
    {5, 5 },
    {5, 6 },
    {4, 7 },
    {3, 7 },
    {2, 7 },
    {1, 6 },
    {1, 5 },
    {3, 2 },
    {4, 2 },
    {5, 3 }, //
    {5, 11},
    {4, 12},
    {5, 10},
    {5, 9 },
    {5, 8 },
    {3, 12},
    {2, 12},
    {1, 11},
    {1, 10},
    {1, 9 },
    {1, 8 }, // '8'
    {2, 2 },
    {1, 3 },
    {1, 4 },
    {5, 4 },
    {5, 5 },
    {5, 6 },
    {4, 7 },
    {3, 7 },
    {2, 7 },
    {1, 6 },
    {1, 5 },
    {3, 2 },
    {4, 2 },
    {5, 3 }, //
    {5, 10},
    {5, 11},
    {5, 9 },
    {5, 8 },
    {5, 7 },
    {4, 12},
    {3, 12},
    {2, 12},
    {1, 11}, // '9'
};

static const dgx_font_dot_t minus_dots[] = {
    {1, 7},
    {2, 7},
    {3, 7},
    {4, 7},
    {5, 7}, // '-'
};

static const dgx_font_dot_t degree_dots[] = {
    {2, 2},
    {3, 2},
    {4, 3},
    {4, 4},
    {3, 5},
    {2, 5},
    {1, 4},
    {1, 3}, // '°'
};

static const dgx_font_dot_t colon_dots[] = {
    {3, 4},
    {3, 5},
    {3, 8},
    {3, 9}, // ':'
};

static const dgx_font_dot_t capital_r_dots[] = {
    // Left vertical stem (Rows 2 to 12)
    {1, 2 },
    {1, 3 },
    {1, 4 },
    {1, 5 },
    {1, 6 },
    {1, 7 },
    {1, 8 },
    {1, 9 },
    {1, 10},
    {1, 11},
    {1, 12},
    // Top horizontal bar
    {2, 2 },
    {3, 2 },
    {4, 2 },
    // Right side of the top loop
    {5, 3 },
    {5, 4 },
    {5, 5 },
    {5, 6 },
    // Middle horizontal bar
    {4, 7 },
    {3, 7 },
    {2, 7 },
    // Diagonal right leg (Corrected path)
    {3, 8 },
    {4, 9 },
    {5, 10},
    {5, 11},
    {5, 12}, // 'R'
};

static const dgx_font_dot_t capital_h_dots[] = {
    // Left vertical stem (Rows 2 to 12)
    {1, 2 },
    {1, 3 },
    {1, 4 },
    {1, 5 },
    {1, 6 },
    {1, 7 },
    {1, 8 },
    {1, 9 },
    {1, 10},
    {1, 11},
    {1, 12},
    // Middle horizontal crossbar (Row 7)
    {2, 7 },
    {3, 7 },
    {4, 7 },
    // Right vertical stem (Rows 2 to 12)
    {5, 2 },
    {5, 3 },
    {5, 4 },
    {5, 5 },
    {5, 6 },
    {5, 7 },
    {5, 8 },
    {5, 9 },
    {5, 10},
    {5, 11},
    {5, 12}, // 'H'
};

static const dgx_font_dot_t percent_dots[] = {
    // Upper-left loop block
    {1, 3 },
    {2, 3 },
    {1, 4 },
    {2, 4 },
    // Main diagonal slash (Top-Right to Bottom-Left)
    {5, 3 },
    {5, 4 },
    {4, 5 },
    {4, 6 },
    {3, 7 },
    {2, 8 },
    {2, 9 },
    {1, 10},
    {1, 11},
    // Lower-right loop block
    {4, 10},
    {5, 10},
    {4, 11},
    {5, 11}  // '%'
};

static const dgx_font_dot_t dot_dots[] = {
    {1, 11},
    {2, 11},
    {1, 12},
    {2, 12}  // '.'
};

static const dgx_font_dot_t celsius_dots[] = {
    // STROKE 1: Serif 'C' (Counter-clockwise flow)
    {6, 4 }, // Start at top-right serif tip
    {5, 3 },
    {4, 3 }, // Curve left along the top arch
    {3, 4 },
    {3, 5 },
    {3, 6 },
    {3, 7 },
    {3, 8 },
    {3, 9 },
    {3, 10},
    {3, 11}, // Vertical spine
    {4, 12},
    {5, 12}, // Push right along the bottom arch
    {6, 11}, // Finish up at the bottom-right serif tip
    // STROKE 2: Hollow Degree Diamond (Clockwise loop)
    {1, 1 }, // Top peak
    {2, 2 }, // Right peak
    {1, 3 }, // Bottom peak
    {0, 2 }  // Left peak
};

static const glyph_t space_glyph[] = {
    {{.dots = NULL, .number_of_dots = 0}, 7, 14, 7, 0, -12}, /* ' ' */
};

static const glyph_t minus_glyph[] = {
    {{.dots = minus_dots, .number_of_dots = 5}, 7, 14, 7, 0, -12}, /* '-' */
};

static const glyph_t degree_glyph[] = {
    {{.dots = degree_dots, .number_of_dots = 8}, 7, 14, 7, 0, -12}, /* '°' */
};

static const glyph_t colon_glyph[] = {
    {{.dots = colon_dots, .number_of_dots = 4}, 7, 14, 7, 0, -12}, /* ':' */
};

static const glyph_t capital_r_glyph[] = {
    {{.dots = capital_r_dots, .number_of_dots = 26}, 7, 14, 7, 0, -12}, /* 'R' */
};

static const glyph_t capital_h_glyph[] = {
    {{.dots = capital_h_dots, .number_of_dots = 25}, 7, 14, 7, 0, -12}, /* 'H' */
};

static const glyph_t percent_glyph[] = {
    {{.dots = percent_dots, .number_of_dots = 17}, 7, 14, 7, 0, -12}, /* '%' */
};

static const glyph_t dot_glyph[] = {
    {{.dots = dot_dots, .number_of_dots = 4}, 4, 14, 4, 0, -12}, /* '.' */
};

static const glyph_t celsius_glyph[] = {
    {{.dots = celsius_dots, .number_of_dots = 18}, 7, 14, 7, 0, -12}, /* '℃' */
};

static const glyph_t glyphs[] = {
    {{.dots = dots + 0, .number_of_dots = 27},   7, 14, 7, 0, -12}, /* '0' */
    {{.dots = dots + 27, .number_of_dots = 17},  7, 14, 7, 0, -12}, /* '1' */
    {{.dots = dots + 44, .number_of_dots = 19},  7, 14, 7, 0, -12}, /* '2' */
    {{.dots = dots + 63, .number_of_dots = 20},  7, 14, 7, 0, -12}, /* '3' */
    {{.dots = dots + 83, .number_of_dots = 20},  7, 14, 7, 0, -12}, /* '4' */
    {{.dots = dots + 103, .number_of_dots = 22}, 7, 14, 7, 0, -12}, /* '5' */
    {{.dots = dots + 125, .number_of_dots = 24}, 7, 14, 7, 0, -12}, /* '6' */
    {{.dots = dots + 149, .number_of_dots = 16}, 7, 14, 7, 0, -12}, /* '7' */
    {{.dots = dots + 165, .number_of_dots = 25}, 7, 14, 7, 0, -12}, /* '8' */
    {{.dots = dots + 190, .number_of_dots = 23}, 7, 14, 7, 0, -12}, /* '9' */
};

static const glyph_array_t glyph_ranges[] = {
    {0x0020, 1,   space_glyph    }, //
    {0x0025, 1,   percent_glyph  }, //
    {0x002D, 1,   minus_glyph    }, //
    {0x002E, 1,   dot_glyph      }, //
    {0x0030, 10,  glyphs + 0     }, //
    {0x003A, 1,   colon_glyph    }, //
    {0x0048, 1,   capital_h_glyph}, //
    {0x0052, 1,   capital_r_glyph}, //
    {0x00B0, 1,   degree_glyph   }, //
    {0x2103, 1,   celsius_glyph  }, //
    {0x0,    0x0, 0              }  //
};

dgx_font_t *CasusDotView()
{
    static dgx_font_t rval = {
        .glyph_ranges  = glyph_ranges, //
        .yAdvance      = 14,           //
        .yOffsetLowest = -12,          //
        .xWidest       = 7,            //
        .xWidthAverage = 7.000000,     //
        .f_type        = DGX_FONT_DOTS //
    };
    return &rval;
}
