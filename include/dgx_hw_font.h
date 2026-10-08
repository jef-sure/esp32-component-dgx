#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dgx_draw.h"
#include "dgx_font.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/*
 * Handwritten fonts: symbols are pen paths made of Bezier curves, drawn by the
 * hw-fonts editor on a 256 x 256 grid. font2c turns the editor's JSON file
 * (format "hw-font") into the tables below. They repeat that file: the same
 * data under the same names, in cells of the grid. What the fields mean, how
 * symbols are placed and joined is described with the editor, font-format-ru.md.
 *
 * Such a font is a dgx_font_t of type DGX_FONT_HW. Its symbols are found as
 * the glyphs of any font, dgx_font_find_glyph(); a glyph points to its
 * elements and to its dgx_hw_symbol_t, the font to its dgx_hw_font_t. The
 * drawing is another matter: letters are joined, the size and the pen are
 * chosen at drawing, so dgx_font_char_to_screen() and the like do not draw it.
 */

/* the version of the "hw-font" format these tables repeat */
#define DGX_HW_FONT_FORMAT_VERSION 1

typedef struct {
    uint8_t x;
    uint8_t y;
} dgx_hw_point_t;

/* "type" of an element; the value is the number of its points */
typedef enum {
    DGX_HW_DOT = 1,     /* "dot" */
    DGX_HW_LINE = 2,    /* "line" */
    DGX_HW_CURVE3P = 3, /* "curve3p": quadratic Bezier curve */
    DGX_HW_CURVE = 4,   /* "curve": cubic Bezier curve */
} dgx_hw_element_type_t;

/* An element: a piece of the pen path, from points[0] to its last point. */
typedef struct dgx_hw_element_ {
    uint8_t        type;   /* dgx_hw_element_type_t */
    uint8_t        pieces; /* straight pieces it takes on the grid; ceil(pieces * sqrt(scale)) at a scale */
    uint16_t       length; /* "length" in 1/100 of a cell */
    uint16_t       pixels; /* cells it paints on the grid */
    dgx_hw_point_t points[4];
} dgx_hw_element_t;

/* "upLeft" and "upRight" of a symbol with nothing above the lowercase band: null in the file */
#define DGX_HW_NONE INT16_MIN

/*
 * A symbol. The elements of its glyph lie one segment after another in the
 * order of writing: "beginConnection", "mainSegments", "endConnection",
 * "postSegments". Columns are counted from symbol_offset_x of the font, rows
 * from its base line, negative above it.
 *
 * The glyph itself has width, height = bottom - top + 1, xAdvance = advance,
 * xOffset = space_before + left - line_left and yOffset = top: the symbol
 * stands with its line_left space_before cells after the cursor of a line,
 * on the base line.
 */
typedef struct dgx_hw_symbol_ {
    uint8_t begin_connection; /* how many elements each segment has */
    uint8_t main_segments;
    uint8_t end_connection;
    uint8_t post_segments;
    int16_t width, top, bottom, left, right;   /* of the whole drawing: main and postponed */
    int16_t line_left, line_right, line_width; /* of its part in the lowercase band */
    int16_t up_left, up_right;                 /* of its part above the band, or DGX_HW_NONE */
    int16_t advance;                           /* its place in a line: space_before + line_width + space_after */
    int16_t space_before;                      /* "spaceBefore": empty space before it, 0 if the file has none */
    int16_t space_after;                       /* "spaceAfter", symbol_space of the font if the file has none */
} dgx_hw_symbol_t;

typedef struct dgx_hw_font_ {
    const char *name;
    uint8_t     format_version;                   /* "formatVersion" of the file it was made from */
    int16_t     base_line;                        /* from symbol_offset_y */
    int16_t     x_height;                         /* the lowercase band is base_line - x_height .. base_line */
    int16_t     symbol_space;                     /* after a symbol placed by its width */
    int16_t     space_width;
    int16_t     symbol_offset_x, symbol_offset_y; /* the active area of a symbol on the grid */
    int16_t     symbol_size_x, symbol_size_y;     /* symbol_size_y is the line height */
} dgx_hw_font_t;

/*
 * What the pace of the pen is counted by. Writing takes effort, in cells of
 * the grid, whatever size the font is drawn at; a pen goes through so much
 * effort a second.
 */
typedef struct {
    float piece; /* k: what every straight piece of an element adds to its length */
    float least; /* the least effort: a dot, and a move of the pen in the air to the next stroke */
} dgx_hw_pace_t;

/**
 * @brief The effort of writing an element.
 *
 * It is length + k * pieces: the way of the pen, and more for a curve that
 * bends more, since such a curve takes more pieces. A dot takes the least
 * effort, and no element takes less than that. The efforts of the elements of
 * a symbol or a line, and the least effort for every move of the pen in the
 * air between them, make its whole effort. The pen is in an element as long
 * as its share of the effort lasts, and that share is the t
 * dgx_bezier_draw_to() takes.
 *
 * @param element The element.
 * @param pace    The measures of the effort.
 * @return The effort, in cells.
 */
float dgx_hw_element_effort(const dgx_hw_element_t *element, const dgx_hw_pace_t *pace);

/*
 * A line being set: where every next symbol of it stands. In cells of the
 * grid, from 0 at the start of the line.
 */
typedef struct {
    dgx_font_t *font;
    int32_t     pen;    /* where the line has come to */
    int32_t     up;     /* how far it is taken above the lowercase band, if has_up */
    bool        has_up;
    bool        first;  /* nothing is in the line yet */
} dgx_hw_line_t;

/**
 * @brief Starts a line of a handwritten font.
 * @param line The line.
 * @param font A font of type DGX_FONT_HW.
 */
void dgx_hw_line_begin(dgx_hw_line_t *line, dgx_font_t *font);

/**
 * @brief Gives the next symbol of a line its place and moves the line on.
 *
 * Symbols are placed by their width; handwritten fonts are never of fixed
 * width. It takes xOffset and xAdvance of the glyph only: a symbol stands
 * xOffset from where the line has come to, the first one of a line stands
 * there itself, and the line moves on by xAdvance. Besides, two symbols with
 * parts above the lowercase band are kept apart by those parts. A space takes
 * space_width, a code point the font has not symbol_size_x; neither gives a
 * glyph.
 *
 * @param line       The line.
 * @param code_point The next code point of the text.
 * @param shift      Out: what to add to x of the points of the symbol to get
 *                   their place in the line. Not touched without a glyph.
 * @return The glyph, or NULL for a space and for a code point not in the font.
 */
const glyph_t *dgx_hw_line_place(dgx_hw_line_t *line, uint32_t code_point, int *shift);

/*
 * A place on a screen: a point of a curve is a whole pixel within the range
 * of dgx_point_2d_t, whatever the size and the place of a text are.
 */
static inline int16_t dgx_hw_pixel(float v)
{
    if (!(v > -32767.0f)) return -32767; /* and what is not a number */
    if (v > 32767.0f) return 32767;
    return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

/**
 * @brief The box the drawing of a text takes, to choose its size and place.
 *
 * In cells of the grid as a text is written from (0, 0): x from the start of
 * a line, y from the base line of the first line, negative above it; "\n"
 * starts the next line. It is the box of the symbols themselves; the curves
 * joining letters lie between them. A pen of some thickness needs half of it
 * around the box.
 *
 * @param font  A font of type DGX_FONT_HW.
 * @param text  UTF-8 text.
 * @param left,top,right,bottom Out: the first and the last column and row of the drawing.
 * @return false when the text has no symbol of the font; nothing is set then.
 */
bool dgx_hw_text_box(dgx_font_t *font, const char *text, int *left, int *top, int *right, int *bottom);

/**
 * @brief The pace of a font: the least effort is that of its hyphen.
 *
 * A dot and a move of the pen in the air take about as long as a hyphen is
 * written. A font without a hyphen gets two thirds of its lowercase height.
 *
 * @param font A font of type DGX_FONT_HW.
 * @param k    What every straight piece adds, in cells; 0 for an even speed.
 * @return The pace.
 */
dgx_hw_pace_t dgx_hw_pace(dgx_font_t *font, float k);

/*
 * A stroke: what the pen writes next. It is an element of a symbol put in
 * its place in the text, or the curve joining two letters. In cells of the
 * grid: x from the start of the line, y from the base line of the first line,
 * negative above it.
 */
typedef struct {
    float   x[4], y[4];
    uint8_t  type;   /* dgx_hw_element_type_t: the number of points */
    uint8_t  pieces; /* straight pieces it takes on the grid */
    float    length; /* in cells */
    bool     lift;   /* the pen comes to its start through the air */
    uint16_t symbol; /* which code point of the text it is of, from 0; a joint is of the letter it leads into */
} dgx_hw_stroke_t;

/**
 * @brief The effort of writing a stroke, see dgx_hw_element_effort().
 */
float dgx_hw_stroke_effort(const dgx_hw_stroke_t *stroke, const dgx_hw_pace_t *pace);

/*
 * A text being written: gives its strokes in the order of writing. Filled by
 * dgx_hw_writer_begin(); the fields are not for the caller.
 */
typedef struct {
    dgx_font_t   *font;
    const char   *text;
    size_t        idx;    /* of the next symbol */
    uint16_t      number; /* of code points passed */
    bool          joined;
    dgx_hw_line_t line;
    int32_t       line_y; /* of the base line of the line being written */
    /* the symbol being written */
    const glyph_t *g;
    uint16_t       g_number;
    int            shift;
    uint8_t        phase, k;
    bool           joined_next;
    const glyph_t *prev;         /* the symbol right before it in the line */
    float          jx[4], jy[4]; /* the last element of the end connection of prev, waiting to be joined */
    bool           has_joint;
    /* the run of joined symbols whose postponed strokes wait */
    bool          run;
    uint16_t      run_number;
    size_t        run_idx, run_end;
    dgx_hw_line_t run_line;
    /* the postponed strokes being written */
    bool           posting;
    size_t         post_idx;
    dgx_hw_line_t  post_line;
    const glyph_t *post_g;
    uint16_t       post_number; /* of the symbol after post_g */
    int            post_shift;
    uint8_t        post_k;
    /* where the pen is */
    bool  pen_set;
    float pen_x, pen_y;
} dgx_hw_writer_t;

/**
 * @brief Starts going through a text stroke by stroke.
 *
 * Symbols are placed as dgx_hw_line_place() places them; "\n" starts the next
 * line, symbol_size_y lower. With @p joined a letter that has an end
 * connection and a next one that has a begin connection are joined by one
 * curve made of those connections, and the postponed strokes of joined
 * letters are written after the last of them, as a hand does. A connection
 * with nothing to join is not written.
 *
 * @param writer The state.
 * @param font   A font of type DGX_FONT_HW.
 * @param text   UTF-8 text; it must live as long as the writer is used.
 * @param joined Joined writing.
 */
void dgx_hw_writer_begin(dgx_hw_writer_t *writer, dgx_font_t *font, const char *text, bool joined);

/**
 * @brief Gives the next stroke of the text.
 * @param writer The state.
 * @param stroke Out: the stroke.
 * @return false when the text is over; @p stroke is not touched then.
 */
bool dgx_hw_writer_next(dgx_hw_writer_t *writer, dgx_hw_stroke_t *stroke);

/**
 * @brief The whole effort of writing a text: its strokes and the moves of the pen between them.
 */
float dgx_hw_text_effort(dgx_font_t *font, const char *text, bool joined, const dgx_hw_pace_t *pace);

/*
 * A text being written on a screen. Filled by dgx_hw_writing_begin(); the
 * fields are not for the caller.
 */
typedef struct {
    dgx_hw_writer_t writer;
    dgx_screen_t   *scr;
    int             x, y;
    float           scale;
    int             width;
    uint32_t        color;
    dgx_hw_pace_t   pace;
    dgx_bezier_t    curve;     /* the stroke the pen is on */
    bool            in_stroke;
    float           start;     /* the effort the pen has when it begins that stroke */
    float           effort;    /* of that stroke */
    float           passed;    /* of everything before it */
    bool            finished;
} dgx_hw_writing_t;

/**
 * @brief Prepares a text for writing on a screen by the pace of a pen; draws nothing.
 *
 * @param writing The state.
 * @param scr     The screen.
 * @param x,y     Where the line starts: its left end on the base line.
 * @param font    A font of type DGX_FONT_HW.
 * @param text    UTF-8 text; it must live as long as the writing goes.
 * @param scale   Pixels in a cell of the grid of the font; its line is symbol_size_y cells high.
 * @param width   Thickness of the pen in pixels.
 * @param color   Color of the ink.
 * @param joined  Joined writing, see dgx_hw_writer_begin().
 * @param pace    How the effort is counted, see dgx_hw_pace(); NULL: by the length alone.
 */
void dgx_hw_writing_begin(dgx_hw_writing_t *writing, dgx_screen_t *scr, int x, int y, dgx_font_t *font, const char *text, float scale,
                          int width, uint32_t color, bool joined, const dgx_hw_pace_t *pace);

/**
 * @brief Writes a text on, up to the effort the pen has gone through.
 *
 * The effort is the tempo of the pen times the time it writes, or a share of
 * dgx_hw_text_effort() of the text. The strokes it covers are written whole,
 * the one the pen is on as far as its share of the effort says, and a move of
 * the pen in the air takes the least effort of the pace. Only what is not
 * written yet is drawn, and it reaches the screen before the call returns.
 *
 * @param writing The state.
 * @param effort  The effort from the start of the text, in cells.
 * @return true when the text is finished.
 */
bool dgx_hw_writing_draw_to(dgx_hw_writing_t *writing, float effort);

/**
 * @brief Writes a text of a handwritten font on a screen at once.
 *
 * The parameters are those of dgx_hw_writing_begin().
 */
void dgx_hw_draw_text(dgx_screen_t *scr, int x, int y, dgx_font_t *font, const char *text, float scale, int width, uint32_t color,
                      bool joined);

/*
 * Morphing. Every stroke is a cubic curve, so a text is a list of cubic
 * curves in the order of writing, and a morph is a list of pairs of them: a
 * frame is every pair with its four points taken between the two curves.
 */

typedef struct {
    float x[4], y[4]; /* in cells, as dgx_hw_stroke_t */
} dgx_hw_curve_t;

/* a curve of a pair that is not there: its partner grows from a point or goes into one */
#define DGX_HW_MORPH_FROM_NOTHING 1
#define DGX_HW_MORPH_TO_NOTHING   2

/* A morph: owned, made by dgx_hw_morph_create() or as a letter of dgx_hw_morph_text_create(). */
typedef struct {
    uint16_t        number;  /* of pairs */
    bool            changes; /* false: both ends are the same, any t gives the same picture */
    dgx_hw_curve_t *from, *to;
    uint8_t        *nothing; /* DGX_HW_MORPH_... of every pair */
} dgx_hw_morph_t;

/**
 * @brief Plans the morph of a line into a line as a whole.
 *
 * The whole way of the pen of one text goes into the whole way of the pen of
 * the other one, whatever letters they have: the texts are made equal in the
 * number of curves by cutting the longest curves in two, which changes no
 * shape, and the curves are paired in the order of writing. A text with no
 * symbol of the font is nothing: the other one grows out of the starts of
 * its strokes.
 *
 * @param font   A font of type DGX_FONT_HW.
 * @param from   UTF-8 text at t = 0.
 * @param to     UTF-8 text at t = 1.
 * @param joined Joined writing, see dgx_hw_writer_begin().
 * @return The morph, or NULL when both texts are empty or there is no memory.
 *         Release with dgx_hw_morph_destroy().
 */
dgx_hw_morph_t *dgx_hw_morph_create(dgx_font_t *font, const char *from, const char *to, bool joined);

void dgx_hw_morph_destroy(dgx_hw_morph_t **morph);

/**
 * @brief Moves the two texts of a morph, each by its own shift.
 *
 * Both texts are planned from (0, 0) of their first lines. To have each in
 * the middle of a screen, say, the first one is moved by one shift and the
 * second one by another; the frames then go from one place to the other.
 *
 * @param morph           The morph; NULL is left alone.
 * @param from_x,from_y   The shift of the text at t = 0, in cells.
 * @param to_x,to_y       The shift of the text at t = 1, in cells.
 */
void dgx_hw_morph_shift(dgx_hw_morph_t *morph, float from_x, float from_y, float to_x, float to_y);

/**
 * @brief Draws a frame of a morph.
 *
 * Nothing is erased: a frame is drawn on a cleared place, a virtual screen as
 * a rule. Every curve of the frame is drawn like one of dgx_hw_draw_text().
 *
 * @param morph The morph; NULL draws nothing.
 * @param t     0: the first text, 1: the second one; put through an easing if needed.
 * @param scr,x,y,scale,width,color As for dgx_hw_draw_text().
 */
void dgx_hw_morph_draw(const dgx_hw_morph_t *morph, float t, dgx_screen_t *scr, int x, int y, float scale, int width, uint32_t color);

/**
 * @brief The box no frame of a morph leaves, in cells, as dgx_hw_text_box().
 *
 * It is the box of the points of all its curves: a Bezier curve does not
 * leave its points, and neither does one between two of them. It may be
 * larger than the texts themselves.
 *
 * @return false for NULL or a morph of no curves.
 */
bool dgx_hw_morph_box(const dgx_hw_morph_t *morph, int *left, int *top, int *right, int *bottom);

/*
 * Letter by letter: one morph for every position of the two texts, each led
 * by its own t, so letters may change one after another.
 */
typedef struct {
    size_t           length;  /* positions: code points of the longer text */
    size_t           changed; /* 1 + index of the last position that changes; 0 if none */
    dgx_hw_morph_t **letters; /* length morphs in the order of the text; NULL where neither text has a symbol */
} dgx_hw_morph_text_t;

/**
 * @brief Plans the morph of a text into a text letter by letter.
 *
 * The symbol at a position of one text goes into the symbol at the same
 * position of the other one: their strokes are paired in the order of
 * writing. A stroke left without a pair, and every stroke of a symbol left
 * without a symbol, grows from the point where the pen of the other text is
 * after that position, or goes into it. All the letters are in the places of
 * their lines, so they are all drawn with the same x and y.
 *
 * @param font,from,to,joined As for dgx_hw_morph_create(). A curve joining two
 *        letters belongs to the second of them.
 * @return The text, or NULL when there is no memory. Release with
 *         dgx_hw_morph_text_destroy().
 */
dgx_hw_morph_text_t *dgx_hw_morph_text_create(dgx_font_t *font, const char *from, const char *to, bool joined);

void dgx_hw_morph_text_destroy(dgx_hw_morph_text_t **text);

/**
 * @brief Moves the two texts of a letter by letter morph, see dgx_hw_morph_shift().
 */
void dgx_hw_morph_text_shift(dgx_hw_morph_text_t *text, float from_x, float from_y, float to_x, float to_y);

/**
 * @brief Time until the last changing letter finishes when letter i starts at
 * i * stagger_us and each letter takes duration_us; 0 if nothing changes.
 */
int64_t dgx_hw_morph_text_duration_us(const dgx_hw_morph_text_t *text, int64_t duration_us, int64_t stagger_us);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
