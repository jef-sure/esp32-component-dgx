/*
 * dgxf_font.h: the .dgxf file of a font of DGX already compiled, by dgxf_write.h.
 *
 * Copyright (c) 2026 Anton Petrusevich. Licensed under the MIT License, as DGX.
 *
 * font2bin.c writes the file of a font of C with it, and the tests of DGX the files of its fonts. On a computer of
 * little-endian byte order, as the symbols of a handwritten font are taken as they lie in memory.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_font.h"
#include "dgx_hw_font.h"
#include "dgxf_write.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the symbols of a handwritten font are taken as they lie in memory: a computer of little-endian byte order"
#endif

static inline int dgxf_by_first(const void *a, const void *b)
{
    const dgxf_range_t *x = (const dgxf_range_t *)a, *y = (const dgxf_range_t *)b;
    return x->first < y->first ? -1 : x->first > y->first;
}

/* the bytes of the data of a glyph, by the model of its font */
static inline uint32_t dgxf_data_of(const dgx_font_t *f, const dgx_glyph_t *g)
{
    switch (f->f_type) {
    case DGX_FONT_DOTS: return (uint32_t)g->count * DGXF_DOT_BYTES;
    case DGX_FONT_HW: return (uint32_t)g->count * DGXF_ELEMENT_BYTES;
    case DGX_FONT_BITMAP_STREAM: return ((uint32_t)g->width * g->height + 7) / 8;
    default: {
        const int bits = f->f_type == DGX_FONT_BITMAP_DEPTH4 ? 4 : f->f_type == DGX_FONT_BITMAP_DEPTH2 ? 2 : 1;
        return (uint32_t)((g->width * bits + 7) / 8) * g->height;
    }
    }
}

/*
 * The file of a font into out, with the text about it, or NULL. 0, or -1 with what is wrong in why: a font without
 * glyphs or data, ranges that overlap, a handwritten font without its symbols, a file that cannot be written.
 */
static inline int dgxf_of_font(const dgx_font_t *f, const char *about, FILE *out, char *why, size_t why_size)
{
    if (!f->glyphs || !f->data) {
        snprintf(why, why_size, "no glyphs or no data: not a font of DGX 0.12.1 or later");
        return -1;
    }
    uint32_t number = 0, glyphs = 0;
    for (const dgx_glyph_array_t *r = f->glyph_ranges; r->number; ++r) ++number;
    dgxf_range_t *ranges = (dgxf_range_t *)calloc(number + 1, sizeof(*ranges));
    for (uint32_t i = 0; i < number; ++i) {
        const dgx_glyph_array_t *r = &f->glyph_ranges[i];
        ranges[i] = (dgxf_range_t){r->first, r->number, r->glyph};
        if (r->glyph + r->number > glyphs) glyphs = r->glyph + r->number;
    }
    /* a font written by hand may give its ranges in any order; a file has them ascending */
    qsort(ranges, number, sizeof(*ranges), dgxf_by_first);
    for (uint32_t i = 1; i < number; ++i) {
        if (ranges[i].first < ranges[i - 1].first + ranges[i - 1].number) {
            snprintf(why, why_size, "the ranges from U+%04X and U+%04X overlap", (unsigned)ranges[i - 1].first, (unsigned)ranges[i].first);
            free(ranges);
            return -1;
        }
    }
    dgxf_glyph_t *table = (dgxf_glyph_t *)calloc(glyphs + 1, sizeof(*table));
    uint32_t      data  = 0;
    for (uint32_t i = 0; i < glyphs; ++i) {
        const dgx_glyph_t *g = &f->glyphs[i];
        table[i] = (dgxf_glyph_t){g->offset, g->width, g->height, g->x_advance, g->x_offset, g->y_offset, g->count};
        const uint32_t end = g->offset + dgxf_data_of(f, g);
        if (end > data) data = end;
    }
    dgxf_font_t file;
    memset(&file, 0, sizeof(file));
    file.model            = (uint16_t)f->f_type;
    const int16_t measures[7] = {f->y_advance, f->y_offset_lowest, f->x_widest, f->x_width_average, f->y_bottom_max, f->x_offset_lowest, f->x_right_max};
    memcpy(file.measures, measures, sizeof(measures));
    file.number_of_ranges = number;
    file.number_of_glyphs = glyphs;
    file.data_length      = data;
    file.ranges           = ranges;
    file.glyphs           = table;
    file.data             = f->data;
    file.about            = about;
    int failed = 0;
    if (f->f_type == DGX_FONT_HW) {
        const dgx_hw_font_t *hw = f->hw;
        if (!hw || !hw->symbols) {
            snprintf(why, why_size, "a handwritten font without its symbols");
            failed = -1;
        } else {
            file.hw = 1;
            const int16_t fields[9] = {hw->format_version, hw->base_line, hw->x_height, hw->symbol_space, hw->space_width,
                                       hw->symbol_offset_x, hw->symbol_offset_y, hw->symbol_size_x, hw->symbol_size_y};
            memcpy(file.hw_fields, fields, sizeof(fields));
            file.symbols = (const uint8_t *)hw->symbols; /* 30 bytes each, little-endian here as in the file */
            file.name    = hw->name;
        }
    }
    if (!failed && dgxf_write(out, &file)) {
        snprintf(why, why_size, "the file cannot be written");
        failed = -1;
    }
    free(ranges), free(table);
    return failed;
}
