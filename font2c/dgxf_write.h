/*
 * dgxf_write.h: a font of DGX written as a file of bytes, .dgxf, which dgx_font_file_load() of dgx_font_file.h reads.
 *
 * Copyright (c) 2026 Anton Petrusevich. Licensed under the MIT License, as DGX.
 *
 * The one writer of the format: font2c includes it for the fonts it makes, font2bin.c for a font of C already
 * compiled. It needs nothing of DGX. Every number is little-endian, of the width given; every table begins at a
 * multiple of 4 from the start of the file. The format, version 1:
 *
 *   the header, 64 bytes
 *     0  "DGXF"                          32  u32 offset of the ranges
 *     4  u16 version, 1                  36  u32 offset of the glyphs
 *     6  u16 model, dgx_font_model_t     40  u32 offset of the data
 *     8  i16 y_advance                   44  u32 length of the data
 *    10  i16 y_offset_lowest             48  u32 offset of the part of a handwritten font, 0 for none
 *    12  i16 x_widest                    52  u32 offset of the text about the font, 0 for none
 *    14  i16 x_width_average             56  u32 length of that text
 *    16  i16 y_bottom_max                60  u32 0
 *    18  i16 x_offset_lowest
 *    20  i16 x_right_max
 *    22  u16 ranges, the ending one not counted
 *    24  u32 glyphs
 *    28  u32 length of the file
 *   the ranges, 12 bytes each, as dgx_glyph_array_t, ascending, and {0, 0, 0} after them
 *     u32 first code point, u32 how many, u32 the number of the glyph of the first one
 *   the glyphs, 16 bytes each, as dgx_glyph_t
 *     u32 offset of its data in the data, i16 width, height, x_advance, x_offset, y_offset, u16 count
 *   the data: the rows of the pictures of a bitmap font, the dots of a dot font (2 bytes each), the elements of the
 *   pen paths of a handwritten one (14 bytes each, as dgx_hw_element_t)
 *   the part of a handwritten font, 28 bytes
 *     u16 format_version, i16 base_line, x_height, symbol_space, space_width, symbol_offset_x, symbol_offset_y,
 *     symbol_size_x, symbol_size_y, u16 0, u32 offset of the symbols, u32 offset of the name
 *   its symbols, 30 bytes each, as dgx_hw_symbol_t, one for every glyph in their order; its name, UTF-8, ending in 0
 *   the text about the font, last: UTF-8 lines "key: value" (name, made-by, family, style, size, bits, copyright,
 *   version, license, license URL), with no 0 at its end
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DGXF_VERSION        1
#define DGXF_HEADER         64
#define DGXF_RANGE_BYTES    12
#define DGXF_GLYPH_BYTES    16
#define DGXF_HW_BYTES       28
#define DGXF_SYMBOL_BYTES   30
#define DGXF_ELEMENT_BYTES  14
#define DGXF_DOT_BYTES      2

typedef struct {
    uint32_t first, number, glyph;
} dgxf_range_t;

typedef struct {
    uint32_t offset;
    int16_t  width, height, x_advance, x_offset, y_offset;
    uint16_t count;
} dgxf_glyph_t;

typedef struct {
    uint16_t            model;
    /* y_advance, y_offset_lowest, x_widest, x_width_average, y_bottom_max, x_offset_lowest, x_right_max */
    int16_t             measures[7];
    uint32_t            number_of_ranges, number_of_glyphs, data_length;
    const dgxf_range_t *ranges; /* ascending */
    const dgxf_glyph_t *glyphs;
    const uint8_t      *data;
    int                 hw;     /* a handwritten font: the four below */
    /* format_version, base_line, x_height, symbol_space, space_width, symbol_offset_x, symbol_offset_y, symbol_size_x,
     * symbol_size_y */
    int16_t        hw_fields[9];
    const uint8_t *symbols; /* DGXF_SYMBOL_BYTES little-endian bytes for every glyph */
    const char    *name;
    const char    *about;   /* or NULL */
} dgxf_font_t;

static inline void dgxf_put(uint8_t *at, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i) at[i] = (uint8_t)(value >> (8 * i));
}

static inline uint32_t dgxf_round4(uint32_t n) { return (n + 3u) & ~3u; }

/* The file of a font into out; 0, or -1 when it cannot be written. */
static inline int dgxf_write(FILE *out, const dgxf_font_t *f)
{
    const uint32_t ranges  = DGXF_HEADER;
    const uint32_t glyphs  = ranges + (f->number_of_ranges + 1) * DGXF_RANGE_BYTES;
    const uint32_t data    = glyphs + f->number_of_glyphs * DGXF_GLYPH_BYTES;
    const uint32_t hw      = f->hw ? dgxf_round4(data + f->data_length) : 0;
    const uint32_t symbols = f->hw ? hw + DGXF_HW_BYTES : 0;
    const uint32_t name    = f->hw ? dgxf_round4(symbols + f->number_of_glyphs * DGXF_SYMBOL_BYTES) : 0;
    const uint32_t name_length = f->hw ? (uint32_t)strlen(f->name ? f->name : "") + 1 : 0;
    const uint32_t end     = f->hw ? name + name_length : data + f->data_length;
    const uint32_t about_length = f->about ? (uint32_t)strlen(f->about) : 0;
    const uint32_t about   = f->about ? dgxf_round4(end) : 0;
    const uint32_t length  = f->about ? about + about_length : end;

    uint8_t *file = calloc(length, 1);
    if (!file) return -1;
    memcpy(file, "DGXF", 4);
    dgxf_put(file + 4, DGXF_VERSION, 2);
    dgxf_put(file + 6, f->model, 2);
    for (int i = 0; i < 7; ++i) dgxf_put(file + 8 + 2 * i, (uint16_t)f->measures[i], 2);
    dgxf_put(file + 22, f->number_of_ranges, 2);
    dgxf_put(file + 24, f->number_of_glyphs, 4);
    dgxf_put(file + 28, length, 4);
    dgxf_put(file + 32, ranges, 4);
    dgxf_put(file + 36, glyphs, 4);
    dgxf_put(file + 40, data, 4);
    dgxf_put(file + 44, f->data_length, 4);
    dgxf_put(file + 48, hw, 4);
    dgxf_put(file + 52, about, 4);
    dgxf_put(file + 56, about_length, 4);
    for (uint32_t i = 0; i < f->number_of_ranges; ++i) {
        uint8_t *r = file + ranges + i * DGXF_RANGE_BYTES;
        dgxf_put(r, f->ranges[i].first, 4), dgxf_put(r + 4, f->ranges[i].number, 4), dgxf_put(r + 8, f->ranges[i].glyph, 4);
    }
    for (uint32_t i = 0; i < f->number_of_glyphs; ++i) {
        const dgxf_glyph_t *g = &f->glyphs[i];
        uint8_t            *p = file + glyphs + i * DGXF_GLYPH_BYTES;
        dgxf_put(p, g->offset, 4);
        dgxf_put(p + 4, (uint16_t)g->width, 2), dgxf_put(p + 6, (uint16_t)g->height, 2), dgxf_put(p + 8, (uint16_t)g->x_advance, 2);
        dgxf_put(p + 10, (uint16_t)g->x_offset, 2), dgxf_put(p + 12, (uint16_t)g->y_offset, 2), dgxf_put(p + 14, g->count, 2);
    }
    if (f->data_length) memcpy(file + data, f->data, f->data_length);
    if (f->hw) {
        for (int i = 0; i < 9; ++i) dgxf_put(file + hw + 2 * i, (uint16_t)f->hw_fields[i], 2);
        dgxf_put(file + hw + 20, symbols, 4);
        dgxf_put(file + hw + 24, name, 4);
        if (f->number_of_glyphs) memcpy(file + symbols, f->symbols, (size_t)f->number_of_glyphs * DGXF_SYMBOL_BYTES);
        memcpy(file + name, f->name ? f->name : "", name_length);
    }
    if (about_length) memcpy(file + about, f->about, about_length);
    const int ok = fwrite(file, 1, length, out) == length;
    free(file);
    return ok ? 0 : -1;
}
