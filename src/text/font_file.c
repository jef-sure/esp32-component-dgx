/*
 * font_file.c: a font of the bytes of a .dgxf file, see dgx_font_file.h; the format is told in font2c/dgxf_write.h.
 *
 *  Copyright (c) 2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */
#include "dgx_font_file.h"

#include <stdbool.h>
#include <string.h>

#include "dgx_hw_font.h"
#include "esp_log.h"

static const char *TAG = "DGX FONT FILE";

/* the tables of a file are these structures byte for byte, on a processor of little-endian order */
_Static_assert(sizeof(dgx_glyph_t) == 16 && offsetof(dgx_glyph_t, width) == 4 && offsetof(dgx_glyph_t, count) == 14, "a glyph as in a file");
_Static_assert(sizeof(dgx_glyph_array_t) == 12 && offsetof(dgx_glyph_array_t, glyph) == 8, "a range as in a file");
_Static_assert(sizeof(dgx_hw_element_t) == 14 && offsetof(dgx_hw_element_t, points) == 6, "an element as in a file");
_Static_assert(sizeof(dgx_hw_symbol_t) == 30 && offsetof(dgx_hw_symbol_t, width) == 4, "a symbol as in a file");

enum {
    HEADER = 64,
    RANGE = 12,
    GLYPH = 16,
    HW_PART = 28,
    SYMBOL = 30,
    ELEMENT = 14,
    DOT = 2,
    MOST = 1024, /* of the width, the height, the advance and the offsets of a glyph */
};

typedef struct {
    const uint8_t *b;
    uint32_t       length;
    uint16_t       model;
    uint32_t       ranges, glyphs, data, data_length, hw, symbols, name;
    uint32_t       number_of_ranges, number_of_glyphs;
} file_t;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t  i16(const uint8_t *p) { return (int16_t)u16(p); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static esp_err_t spoiled(const char *what, uint32_t at)
{
    ESP_LOGE(TAG, "not a font file DGX reads: %s, at byte %u", what, (unsigned)at);
    return ESP_ERR_INVALID_ARG;
}

/* a table of size bytes from offset, inside the file and from a multiple of 4 */
static bool inside(const file_t *f, uint32_t offset, uint64_t size)
{
    return offset >= HEADER && offset % 4 == 0 && (uint64_t)offset + size <= f->length;
}

static bool between(int v, int least, int most) { return v >= least && v <= most; }

/* the bytes of the picture of a bitmap glyph */
static uint64_t picture_bytes(uint16_t model, int w, int h)
{
    const int bits = model == DGX_FONT_BITMAP_DEPTH4 ? 4 : model == DGX_FONT_BITMAP_DEPTH2 ? 2 : 1;
    if (model == DGX_FONT_BITMAP_STREAM) return ((uint64_t)w * h + 7) / 8;
    return (uint64_t)((w * bits + 7) / 8) * h;
}

/* a symbol of a handwritten font in the bounds font2c writes it in */
static bool symbol_sound(const uint8_t *s, unsigned count)
{
    if ((unsigned)s[0] + s[1] + s[2] + s[3] != count) return false;
    const int width = i16(s + 4), line_width = i16(s + 18), up_left = i16(s + 20), up_right = i16(s + 22);
    if (!between(width, 0, 256) || !between(line_width, 0, 256)) return false;
    for (int k = 0; k < 6; ++k) {
        static const uint8_t at[6] = {6, 8, 10, 12, 14, 16}; /* top, bottom, left, right, line_left, line_right */
        if (!between(i16(s + at[k]), -255, 255)) return false;
    }
    if (up_left != DGX_HW_NONE && !between(up_left, -255, 255)) return false;
    if (up_right != DGX_HW_NONE && !between(up_right, -255, 255)) return false;
    return between(i16(s + 24), 0, 1024) && between(i16(s + 26), 0, 255) && between(i16(s + 28), 0, 255);
}

/* every offset and length of the file held to its bounds, before anything of it is given */
static esp_err_t read_file(const void *bytes, size_t length, file_t *f)
{
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    (void)bytes, (void)length, (void)f;
    ESP_LOGE(TAG, "a font file is read on a processor of little-endian byte order");
    return ESP_ERR_NOT_SUPPORTED;
#else
    const uint8_t *b = bytes;
    if (!b || length < HEADER) return spoiled("shorter than its header", 0);
    if (memcmp(b, "DGXF", 4)) return spoiled("no mark DGXF", 0);
    if (u16(b + 4) != DGX_FONT_FILE_VERSION) {
        ESP_LOGE(TAG, "a font file of format %u, of a later DGX: this one reads format %d", u16(b + 4), DGX_FONT_FILE_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    *f = (file_t){.b = b, .length = u32(b + 28), .model = u16(b + 6), .ranges = u32(b + 32), .glyphs = u32(b + 36), .data = u32(b + 40),
                  .data_length = u32(b + 44), .hw = u32(b + 48), .number_of_ranges = u16(b + 22), .number_of_glyphs = u32(b + 24)};
    if (f->length < HEADER || f->length > length) return spoiled("its length is more than the bytes given, or less than a header", 28);
    if (f->model < DGX_FONT_BITMAP_LINES || f->model > DGX_FONT_BITMAP_DEPTH4) return spoiled("no model of a font", 6);
    if (!between(i16(b + 8), 1, 2 * MOST)) return spoiled("the advance of a line", 8);
    if (!f->number_of_ranges || f->number_of_ranges > INT16_MAX || !inside(f, f->ranges, (uint64_t)(f->number_of_ranges + 1) * RANGE))
        return spoiled("the ranges", 32);
    if (!inside(f, f->glyphs, (uint64_t)f->number_of_glyphs * GLYPH)) return spoiled("the glyphs", 36);
    if (!inside(f, f->data, f->data_length)) return spoiled("the data", 40);
    const uint8_t *end = b + f->ranges + f->number_of_ranges * RANGE;
    if (u32(end) || u32(end + 4) || u32(end + 8)) return spoiled("the ranges do not end in {0, 0, 0}", f->ranges + f->number_of_ranges * RANGE);
    uint64_t next = 0;
    for (uint32_t i = 0; i < f->number_of_ranges; ++i) {
        const uint8_t *r = b + f->ranges + i * RANGE;
        const uint64_t first = u32(r), number = u32(r + 4), glyph = u32(r + 8);
        /* ascending without overlap: they are searched by halving */
        if (!number || first < next || first + number > 0x110000 || glyph + number > f->number_of_glyphs) return spoiled("a range", f->ranges + i * RANGE);
        next = first + number;
    }
    if (f->model == DGX_FONT_HW) {
        if (!inside(f, f->hw, HW_PART)) return spoiled("the part of a handwritten font", 48);
        const uint8_t *hw = b + f->hw;
        f->symbols = u32(hw + 20), f->name = u32(hw + 24);
        if (!between(i16(hw + 2), 0, 255) || !between(i16(hw + 4), 0, 255) || !between(i16(hw + 6), 0, 255) || !between(i16(hw + 8), 0, 255) ||
            !between(i16(hw + 10), 0, 255) || !between(i16(hw + 12), 0, 255) || !between(i16(hw + 14), 1, 256) || !between(i16(hw + 16), 1, 256))
            return spoiled("a measure of a handwritten font", f->hw);
        if (!inside(f, f->symbols, (uint64_t)f->number_of_glyphs * SYMBOL)) return spoiled("the symbols", f->hw + 20);
        if (f->name < HEADER || f->name >= f->length || !memchr(b + f->name, 0, f->length - f->name)) return spoiled("the name", f->hw + 24);
    } else if (f->hw) {
        return spoiled("a part of a handwritten font in a font of another model", 48);
    }
    const uint32_t about = u32(b + 52), about_length = u32(b + 56);
    if (about && (about < HEADER || (uint64_t)about + about_length > f->length)) return spoiled("the text about the font", 52);
    /* the box of the font holds every glyph, or it is all 0, as in a font made before font2c counted it */
    const int top = i16(b + 10), bottom = i16(b + 16), left = i16(b + 18), right = i16(b + 20);
    const bool box = top || bottom || left || right;
    for (uint32_t i = 0; i < f->number_of_glyphs; ++i) {
        const uint8_t *g = b + f->glyphs + i * GLYPH;
        const uint32_t at = f->glyphs + i * GLYPH, offset = u32(g), count = u16(g + 14);
        const int      w = i16(g + 4), h = i16(g + 6), xo = i16(g + 10), yo = i16(g + 12);
        if (!between(w, 0, MOST) || !between(h, 0, MOST) || !between(i16(g + 8), -MOST, MOST) || !between(xo, -MOST, MOST) || !between(yo, -MOST, MOST))
            return spoiled("the measures of a glyph", at);
        if (box && w > 0 && h > 0 && (xo < left || xo + w > right || yo < top || yo + h > bottom)) return spoiled("a glyph outside the box of the font", at);
        const uint64_t bytes_of = f->model == DGX_FONT_DOTS ? (uint64_t)count * DOT
                                : f->model == DGX_FONT_HW   ? (uint64_t)count * ELEMENT
                                                            : picture_bytes(f->model, w, h);
        if ((uint64_t)offset + bytes_of > f->data_length) return spoiled("the data of a glyph is past the data", at);
        const uint8_t *d = b + f->data + offset;
        if (f->model == DGX_FONT_DOTS) {
            for (uint32_t k = 0; k < count; ++k) {
                const int x = (int8_t)d[k * DOT], y = (int8_t)d[k * DOT + 1];
                if (x < 0 || x >= w || y < 0 || y >= h) return spoiled("a dot outside its glyph", f->data + offset + k * DOT);
            }
        } else if (f->model == DGX_FONT_HW) {
            if (offset % 2) return spoiled("the elements of a glyph at an odd offset", at);
            for (uint32_t k = 0; k < count; ++k)
                if (d[k * ELEMENT] < DGX_HW_DOT || d[k * ELEMENT] > DGX_HW_CURVE) return spoiled("an element of no type", f->data + offset + k * ELEMENT);
            if (!symbol_sound(b + f->symbols + i * SYMBOL, count)) return spoiled("a symbol", f->symbols + i * SYMBOL);
        }
    }
    return ESP_OK;
#endif
}

static size_t round8(size_t n) { return (n + 7) & ~(size_t)7; }

/* the memory of a font of a file at the address it lies at: what is copied there, and where */
typedef struct {
    size_t font, hw, ranges, glyphs, symbols, elements, total;
} layout_t;

static layout_t layout_of(const file_t *f)
{
    const bool aligned = (uintptr_t)f->b % 4 == 0, odd = (uintptr_t)f->b % 2 != 0, hw = f->model == DGX_FONT_HW;
    layout_t   l = {0};
    size_t     at = round8(sizeof(dgx_font_t));
    if (hw) l.hw = at, at += round8(sizeof(dgx_hw_font_t));
    if (!aligned) {
        l.ranges = at, at += round8((size_t)(f->number_of_ranges + 1) * sizeof(dgx_glyph_array_t));
        l.glyphs = at, at += round8((size_t)f->number_of_glyphs * sizeof(dgx_glyph_t));
        if (hw) l.symbols = at, at += round8((size_t)f->number_of_glyphs * sizeof(dgx_hw_symbol_t));
        if (hw && odd) l.elements = at, at += round8(f->data_length);
    }
    l.total = at;
    return l;
}

esp_err_t dgx_font_file_memory(const void *bytes, size_t length, size_t *memory)
{
    file_t          f;
    const esp_err_t err = read_file(bytes, length, &f);
    if (err != ESP_OK) return err;
    if (memory) *memory = layout_of(&f).total;
    return ESP_OK;
}

esp_err_t dgx_font_file_load(const void *bytes, size_t length, void *memory, size_t size, const dgx_font_t **font)
{
    file_t    f;
    esp_err_t err = read_file(bytes, length, &f);
    if (err != ESP_OK) return err;
    const layout_t l = layout_of(&f);
    if (!memory || !font || size < l.total || (uintptr_t)memory % sizeof(void *)) {
        ESP_LOGE(TAG, "the memory of a font file is %u bytes, aligned as malloc() gives it", (unsigned)l.total);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t       *m = memory;
    const uint8_t *b = f.b;
    /* a table read in place, or its copy in the memory */
    const void *ranges = b + f.ranges, *glyphs = b + f.glyphs, *symbols = b + f.symbols, *data = b + f.data;
    if (l.ranges) ranges = memcpy(m + l.ranges, ranges, (size_t)(f.number_of_ranges + 1) * sizeof(dgx_glyph_array_t));
    if (l.glyphs) glyphs = memcpy(m + l.glyphs, glyphs, (size_t)f.number_of_glyphs * sizeof(dgx_glyph_t));
    if (l.symbols) symbols = memcpy(m + l.symbols, symbols, (size_t)f.number_of_glyphs * sizeof(dgx_hw_symbol_t));
    if (l.elements) data = memcpy(m + l.elements, data, f.data_length);
    const struct dgx_hw_font_ *hw = NULL;
    if (f.model == DGX_FONT_HW) {
        const uint8_t *p  = b + f.hw;
        dgx_hw_font_t *h  = (dgx_hw_font_t *)(m + l.hw);
        *h = (dgx_hw_font_t){.name            = (const char *)b + f.name,
                             .format_version  = (uint8_t)u16(p),
                             .base_line       = i16(p + 2),
                             .x_height        = i16(p + 4),
                             .symbol_space    = i16(p + 6),
                             .space_width     = i16(p + 8),
                             .symbol_offset_x = i16(p + 10),
                             .symbol_offset_y = i16(p + 12),
                             .symbol_size_x   = i16(p + 14),
                             .symbol_size_y   = i16(p + 16),
                             .symbols         = symbols};
        hw = h;
    }
    dgx_font_t *out = (dgx_font_t *)m;
    *out = (dgx_font_t){.glyph_ranges     = ranges,
                        .y_advance        = i16(b + 8),
                        .y_offset_lowest  = i16(b + 10),
                        .x_widest         = i16(b + 12),
                        .x_width_average  = i16(b + 14),
                        .f_type           = (dgx_font_model_t)f.model,
                        .y_bottom_max     = i16(b + 16),
                        .x_offset_lowest  = i16(b + 18),
                        .x_right_max      = i16(b + 20),
                        .number_of_ranges = (int16_t)f.number_of_ranges,
                        .hw               = hw,
                        .glyphs           = glyphs,
                        .data             = data};
    *font = out;
    return ESP_OK;
}
