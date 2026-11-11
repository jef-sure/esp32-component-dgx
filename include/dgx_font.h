#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dgx_bitmap.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef struct dgx_font_dot_ {
    int8_t x;
    int8_t y;
} dgx_font_dot_t;

/*
 * @brief Quasi-constructor for dgx_font_dot_t
 *
 * @param x
 * @param y
 * @return dgx_font_dot_t
 */
static inline dgx_font_dot_t dgx_font_dot_new(int8_t x, int8_t y) {
    dgx_font_dot_t ret = {
        .x = x,  //
        .y = y   //
    };
    return ret;
}

struct dgx_font_sym8_params_;

typedef void (*dgx_font_dot_func_t)(dgx_screen_t *_scr_dst, int x_dst, int y_dst, struct dgx_font_sym8_params_ *vParam);

typedef struct dgx_font_sym8_params_ {
    dgx_screen_t *vpoint;
    uint16_t *lut;
    dgx_font_dot_func_t dot_func;
} dgx_font_sym8_params_t;

typedef enum {                   //
    DGX_FONT_BITMAP_LINES = 1,   //
    DGX_FONT_BITMAP_STREAM = 2,  //
    DGX_FONT_DOTS = 3,           //
    DGX_FONT_HW = 4,             ///< Handwritten: pen paths of Bezier curves, see dgx_hw_font.h
    /**
     * Bitmaps of 2 or 4 bits a pixel, font2c -b 2 or -b 4: a pixel is how much of it the glyph covers, 0 to 3 or 0
     * to 15, for soft edges. Rows as in DGX_FONT_BITMAP_LINES, each from a new byte, the leftmost pixel in the high
     * bits: (width * 2 + 7) / 8 or (width * 4 + 7) / 8 bytes a row.
     */
    DGX_FONT_BITMAP_DEPTH2 = 5,
    DGX_FONT_BITMAP_DEPTH4 = 6
} dgx_font_model_t;

struct dgx_hw_element_;
struct dgx_hw_symbol_;
struct dgx_hw_font_;

typedef struct {
    union {
        const uint8_t *bitmap;  ///< Pointer into GFXfont->bitmap
        struct {
            const dgx_font_dot_t *dots;
            size_t number_of_dots;
        };
        struct {                                   ///< DGX_FONT_HW
            const struct dgx_hw_element_ *elements; ///< The pen path of the symbol
            const struct dgx_hw_symbol_ *hw;        ///< Its segments and the rest of its measures
        };
    };
    int16_t width;     ///< Bitmap dimensions in pixels; DGX_FONT_HW: of the drawing in cells of its grid, as all below
    int16_t height;    ///< Bitmap dimensions in pixels
    int16_t xAdvance;  ///< Distance to advance cursor (x axis)
    int16_t xOffset;
    int16_t yOffset;
} glyph_t;

typedef struct {
    int first, number;
    const glyph_t *glyphs;
} glyph_array_t;

typedef struct {
    uint32_t fromCP;
    uint32_t toCP;
    int number_of_dots;
    dgx_point_2d_t *m_start;
    dgx_point_2d_t *m_end;
    bool is_from_empty;
    bool is_to_empty;
    int scale;  ///< A dot is scale x scale pixels; m_start and m_end are their top left corners.
} dgx_font_symbol_morph_t;

struct dgx_font_;
typedef int (*dgx_font_draw_sym_func_t)(  //
    dgx_screen_t *scr,                    //
    int16_t x,                            //
    int16_t y,                            //
    uint32_t codePoint,                   //
    uint32_t color,                       //
    dgx_output_orientation_t orientation, //
    int scale,                            //
    struct dgx_font_ *font,               //
    void *param                           //
);

typedef struct dgx_font_ {
    const glyph_array_t *glyph_ranges;
    int16_t yAdvance, yOffsetLowest, xWidest, xWidthAverage;
    dgx_font_model_t f_type;
    int16_t yBottomMax, xOffsetLowest, xRightMax;
    int16_t number_of_ranges;       ///< Ranges in glyph_ranges, ascending, for a search by halving; 0: not told, they are tried in order
    const struct dgx_hw_font_ *hw;  ///< DGX_FONT_HW: what the font has besides its glyphs
} dgx_font_t;

/**
 * @brief Decode the next UTF-8 code point from a string.
 * @param chr Pointer to the UTF-8 string.
 * @param idx In/out byte index; advanced past the decoded code point.
 * @return The decoded Unicode code point.
 */
uint32_t decodeUTF8next(const char *chr, size_t *idx);

/**
 * @brief Look up the glyph for a code point in a font.
 * @param codePoint Unicode code point.
 * @param font      Font to search.
 * @param xAdvance  Out: cursor advance for this glyph (may be NULL).
 * @return Pointer to the glyph, or NULL if not present in the font.
 */
const glyph_t *dgx_font_find_glyph(uint32_t codePoint, dgx_font_t *font, int16_t *xAdvance);

/**
 * @brief Render a single code point onto a screen.
 *
 * Fonts of bitmaps and dots only. A DGX_FONT_HW font is drawn by its own
 * functions; here nothing is drawn for it.
 * @param scr         Target screen.
 * @param x,y         Baseline origin of the glyph.
 * @param codePoint   Unicode code point to draw.
 * @param color       Foreground color.
 * @param orientation Text orientation (see dgx_output_orientation_t): the
 *                    upright glyph turned around the top left corner of
 *                    pixel x, y.
 * @param scale       Integer scale factor (1 = native size).
 * @param font        Font accessor.
 * @param draw_func   Optional custom per-glyph draw callback (NULL for default).
 * @param param       Optional callback parameter.
 * @return Cursor advance in pixels.
 */
int dgx_font_char_to_screen(             //
    dgx_screen_t *scr,                   //
    int16_t x,                           //
    int16_t y,                           //
    uint32_t codePoint,                  //
    uint32_t color,                      //
    dgx_output_orientation_t orientation,//
    int scale,                           //
    struct dgx_font_ *font,              //
    dgx_font_draw_sym_func_t draw_func,  //
    void *param                          //
);

/**
 * @brief Compute the vertical bounds of a string in a font.
 * @param str    UTF-8 string.
 * @param font   Font accessor.
 * @param ycorner Out: top Y offset relative to baseline.
 * @param height  Out: total height in pixels.
 * @return Total width in pixels, counted in 16 bits: up to 32767.
 */
int dgx_font_string_bounds(const char *str, dgx_font_t *font, int16_t *ycorner, int16_t *height);

/**
 * @brief Render a UTF-8 string onto a screen.
 * @param scr         Target screen.
 * @param x,y         Baseline origin of the first glyph.
 * @param str         UTF-8 string to draw.
 * @param color       Foreground color.
 * @param orientation Text orientation (see dgx_output_orientation_t): the
 *                    upright text turned around the top left corner of
 *                    pixel x, y.
 * @param scale       Integer scale factor (1 = native size).
 * @param font        Font accessor.
 * @param draw_func   Optional custom per-glyph draw callback (NULL for default).
 * @param param       Optional callback parameter.
 */
void dgx_font_string_utf8_screen(        //
    dgx_screen_t *scr,                   //
    int16_t x,                           //
    int16_t y,                           //
    const char *str,                     //
    uint32_t color,                      //
    dgx_output_orientation_t orientation,//
    int scale,                           //
    struct dgx_font_ *font,              //
    dgx_font_draw_sym_func_t draw_func,  //
    void *param                          //
);

/*
 * Flags of a dgx_font_context_t.
 */
/**
 * Mix the soft edges of the letters with ctx->bg instead of with what lies on
 * the screen: the screen is not read, so the edges are soft on a display
 * behind a bus too. The pixels the letters do not touch stay as they are.
 */
#define DGX_FONT_BG   0x1u
/**
 * Fill the cell of the text with ctx->bg and draw the letters over it, their
 * edges mixed with it; implies DGX_FONT_BG. The cell is as high as the font,
 * yOffsetLowest to yBottomMax, whatever the letters, and runs from x to the
 * end of the text, wider where a letter sticks out: a number drawn again over
 * itself leaves nothing of the one before when it is as long.
 */
#define DGX_FONT_FILL 0x2u
/**
 * Draw a font of 2 or 4 bits a pixel as one of one bit: a pixel covered by
 * 110/255 or more takes the color, the rest is left. This is how a font of
 * one bit is cut from the same render, and how a screen that cannot mix
 * draws such a font anyway.
 */
#define DGX_FONT_MONO 0x4u
/**
 * Draw a font of 2 or 4 bits a pixel with hard edges: every pixel a letter
 * touches takes the color whole. To erase soft text: drawn again in the color
 * of the paper, soft text leaves a halo, which this does not. Wins over
 * DGX_FONT_MONO.
 */
#define DGX_FONT_HARD 0x8u

/**
 * @brief How text is drawn: the screen, the font, the colors and the rest that
 * stays the same from one text to the next.
 *
 * A program keeps a few of these and passes one to dgx_font_draw() with the
 * place and the text. Fill it by the names of its members: zero is the
 * default of each, so `{.scr = display, .font = f, .color = c}` is a text in
 * one color, upright, at the size of the font. Members may be added at the
 * end. The calls do not change it: one context may serve the whole program,
 * and a text in another color is a copy with that member changed.
 *
 * The edges of a font of 2 or 4 bits a pixel (DGX_FONT_BITMAP_DEPTH2,
 * DGX_FONT_BITMAP_DEPTH4) are soft: an edge pixel is the color mixed with
 * what is under it by how much of the pixel the letter covers. Without
 * DGX_FONT_BG the color under it is read from the screen, which a screen in
 * memory can do: a virtual screen of 12, 16, 18 or 24 bits, a DSI display, and
 * an 8-bit virtual screen, where a pixel is an amount of ink and the amounts
 * are mixed. A display behind a bus cannot be read, nor does a screen of one
 * bit have a color between: there such a font is drawn as with DGX_FONT_MONO,
 * unless DGX_FONT_BG gives the color under it. A font of one bit draws the
 * same with or without any flag but DGX_FONT_FILL.
 *
 * Drawn again over itself without the paper put back, soft text grows darker
 * at its edges, and drawn in the color of the paper it leaves a halo: erase it
 * with DGX_FONT_HARD, or draw the new text with DGX_FONT_FILL.
 */
typedef struct dgx_font_context_t {
    dgx_screen_t            *scr;         ///< Where the text goes.
    dgx_font_t              *font;        ///< The font; a handwritten one is drawn by dgx_hw_font.h, here only measured.
    uint32_t                 color;       ///< The color of the letters, in the format of the screen.
    uint32_t                 bg;          ///< The paper, with DGX_FONT_BG or DGX_FONT_FILL.
    uint32_t                 flags;       ///< DGX_FONT_BG, DGX_FONT_FILL, DGX_FONT_MONO, DGX_FONT_HARD.
    dgx_output_orientation_t orientation; ///< 0 is DgxOutputNormal: the upright text turned around the top left corner of pixel x, y.
    int                      scale;       ///< Every pixel of the font as scale x scale pixels; 0 is the same as 1.
    /**
     * A dot font (DGX_FONT_DOTS): draws one dot whose top left corner is at x,
     * y, scale x scale pixels; NULL draws it as a square of ctx->color.
     */
    void (*dot_func)(const struct dgx_font_context_t *ctx, int x, int y);
    void *user_data; ///< The program's own, for dot_func; DGX does not touch it.
} dgx_font_context_t;

/**
 * @brief Draw a UTF-8 text.
 * @param ctx How: see dgx_font_context_t.
 * @param x,y The start of the base line of the first letter.
 * @param str The text.
 * @return How far the text goes along its line, in pixels of the screen, the
 *         scale counted: the next text of the line starts that far on, along
 *         x for a text that is not turned, along y for one that is.
 */
int dgx_font_draw(const dgx_font_context_t *ctx, int x, int y, const char *str);

/**
 * @brief Draw one character, as dgx_font_draw() draws each of a text.
 *
 * With DGX_FONT_FILL its own cell is filled: the cell of a text is one, which
 * a text of these calls is not.
 * @return How far the next character starts along the line, scale counted.
 */
int dgx_font_draw_char(const dgx_font_context_t *ctx, int x, int y, uint32_t code_point);

/**
 * @brief Measure a text as dgx_font_draw() would draw it, the scale counted.
 * @param ctx  Its font and scale are taken.
 * @param str  UTF-8 text.
 * @param top    Out, may be NULL: the top of the highest letter, from the base line (negative above it).
 * @param height Out, may be NULL: from that top to the bottom of the lowest letter.
 * @return How far the text goes along its line, as dgx_font_draw() returns.
 */
int dgx_font_bounds(const dgx_font_context_t *ctx, const char *str, int *top, int *height);

/**
 * @brief Set up an 8-bit indexed virtual screen as a glyph "dot" render target.
 * @param vpoint Virtual screen used as the dot canvas.
 */
void dgx_font_make_point8(dgx_screen_t *vpoint);

/**
 * @brief Build a morph descriptor that interpolates between two dot-model glyphs.
 *
 * Used to animate one glyph smoothly into another. Dot i of one glyph flies
 * to dot i of the other, so a font whose dots are listed in an order chosen
 * for that, as CasusDotView, morphs stroke into stroke. The dots one glyph
 * has more than the other come out of its last dot, or go into it; a glyph
 * out of nothing comes out of its middle. dgx_morph_symbol_draw() draws it
 * with the renderers of the morphing framework. Free the result with
 * dgx_font_make_morph_struct_destroy().
 *
 * @param font  Dot-model font.
 * @param fromCP Source code point.
 * @param toCP   Target code point.
 * @param from_x,from_y Position of the source glyph.
 * @param to_x,to_y     Position of the target glyph.
 * @param scale  Integer scale factor.
 * @return Newly allocated morph descriptor, or NULL on failure.
 */
dgx_font_symbol_morph_t *dgx_font_make_morph_struct(  //
    dgx_font_t *font,                                 //
    uint32_t fromCP,                                  //
    uint32_t toCP,                                    //
    int from_x,                                       //
    int from_y,                                       //
    int to_x,                                         //
    int to_y,                                         //
    int scale                                         //
);

/**
 * @brief Destroy a morph descriptor created by dgx_font_make_morph_struct().
 * @param ms In/out pointer to the descriptor; set to NULL on return. NULL
 *           and a pointer to NULL are valid, as when the descriptor was not
 *           made.
 */
void dgx_font_make_morph_struct_destroy(dgx_font_symbol_morph_t **ms);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

