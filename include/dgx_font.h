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
    DGX_FONT_DOTS = 3            //
} dgx_font_model_t;

typedef struct {
    union {
        const uint8_t *bitmap;  ///< Pointer into GFXfont->bitmap
        struct {
            const dgx_font_dot_t *dots;
            size_t number_of_dots;
        };
    };
    int16_t width;     ///< Bitmap dimensions in pixels
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
 * @param scr         Target screen.
 * @param x,y         Baseline origin of the glyph.
 * @param codePoint   Unicode code point to draw.
 * @param color       Foreground color.
 * @param orientation Text orientation (see dgx_output_orientation_t).
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
 * @return Total width in pixels.
 */
int dgx_font_string_bounds(const char *str, dgx_font_t *font, int16_t *ycorner, int16_t *height);

/**
 * @brief Render a UTF-8 string onto a screen.
 * @param scr         Target screen.
 * @param x,y         Baseline origin of the first glyph.
 * @param str         UTF-8 string to draw.
 * @param color       Foreground color.
 * @param orientation Text orientation (see dgx_output_orientation_t).
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

/**
 * @brief Set up an 8-bit indexed virtual screen as a glyph "dot" render target.
 * @param vpoint Virtual screen used as the dot canvas.
 */
void dgx_font_make_point8(dgx_screen_t *vpoint);

/**
 * @brief Build a morph descriptor that interpolates between two dot-model glyphs.
 *
 * Used to animate one glyph smoothly into another. Free the result with
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
 * @param ms In/out pointer to the descriptor; set to NULL on return.
 */
void dgx_font_make_morph_struct_destroy(dgx_font_symbol_morph_t **ms);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

