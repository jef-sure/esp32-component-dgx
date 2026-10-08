/*
 * MIT License
 *
 * Copyright (c) 2021-2026 Anton Petrusevich
 *
 */

#include <ctype.h>
#include <errno.h>
#include <ft2build.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include FT_GLYPH_H
#include FT_MODULE_H
#include FT_FREETYPE_H
#include FT_TRUETYPE_DRIVER_H

typedef struct
{
    size_t   bitmapOffset; ///< Pointer into GFXfont->bitmap
    uint16_t width;        ///< Bitmap dimensions in pixels
    uint16_t height;       ///< Bitmap dimensions in pixels
    uint16_t xAdvance;     ///< Distance to advance cursor (x axis)
    int16_t  xOffset;      ///< X dist from cursor pos to UL corner
    int16_t  yOffset;      ///< Y dist from cursor pos to UL corner
} glyph_t;

typedef struct
{
    uint32_t codePoint;
    glyph_t  ginfo;
} LoadedGlyph;

typedef struct _cp_ranges
{
    int                first, number;
    int                gOffset;
    struct _cp_ranges *next;
} cp_ranges_t;

typedef struct
{
    int      first, number;
    glyph_t *glyphs;
} glyph_array_t;

cp_ranges_t *CPRanges = 0;

int isInRange(int codePoint)
{
    for (cp_ranges_t *r = CPRanges; r; r = r->next) {
        if (codePoint < r->first) return 0;
        if (codePoint < r->first + r->number) return 1;
    }
    return 0;
}

// Insert a range into a sorted list of ranges, merging overlapping/adjacent
// nodes so contiguous code points collapse into one range.
void cpr_insert_range(cp_ranges_t **head, int first, int last)
{
    if (last < first) {
        int t = first;
        first = last;
        last  = t;
    }

    cp_ranges_t *prev = 0;
    cp_ranges_t *cur  = *head;

    while (cur && (cur->first + cur->number - 1) < first - 1) {
        prev = cur;
        cur  = cur->next;
    }

    int mergedFirst = first;
    int mergedLast  = last;

    while (cur && cur->first <= mergedLast + 1) {
        int curLast = cur->first + cur->number - 1;
        if (cur->first < mergedFirst) mergedFirst = cur->first;
        if (curLast > mergedLast) mergedLast = curLast;
        cp_ranges_t *next = cur->next;
        free(cur);
        cur = next;
    }

    cp_ranges_t *node = malloc(sizeof(cp_ranges_t));
    node->first       = mergedFirst;
    node->number      = mergedLast - mergedFirst + 1;
    node->next        = cur;

    if (prev) {
        prev->next = node;
    } else {
        *head = node;
    }
}

// Insert a single code point into a sorted list of ranges.
void cpr_insert(cp_ranges_t **head, int codePoint)
{
    cpr_insert_range(head, codePoint, codePoint);
}

void cpr_insert_filter_cp(int codePoint)
{
    cpr_insert(&CPRanges, codePoint);
}

// Read a UTF-8 text file and add every character it contains to CPRanges.
// Newline, carriage return and tab characters are ignored.
void loadCharsetFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error opening charset file %s: %s\n", path, strerror(errno));
        exit(1);
    }
    int c;
    while ((c = fgetc(f)) != EOF) {
        unsigned char b = (unsigned char)c;
        uint32_t      cp;
        int           extra;
        if (b < 0x80) {
            cp    = b;
            extra = 0;
        } else if ((b & 0xE0) == 0xC0) {
            cp    = b & 0x1F;
            extra = 1;
        } else if ((b & 0xF0) == 0xE0) {
            cp    = b & 0x0F;
            extra = 2;
        } else if ((b & 0xF8) == 0xF0) {
            cp    = b & 0x07;
            extra = 3;
        } else {
            continue; // invalid lead or stray continuation byte, skip
        }
        int ok = 1;
        for (int k = 0; k < extra; k++) {
            int cc = fgetc(f);
            if (cc == EOF || ((unsigned char)cc & 0xC0) != 0x80) {
                ok = 0;
                break;
            }
            cp = (cp << 6) | ((unsigned char)cc & 0x3F);
        }
        if (!ok) continue;
        if (cp == '\n' || cp == '\r' || cp == '\t') continue;
        cpr_insert_filter_cp((int)cp);
    }
    fclose(f);
}

static void show_usage(const char *programName)
{
    fprintf(stderr,
            "Usage: %s [-f charset_file] fontfile size [first last] .. [firstN lastN]\n"
            "       %s [-f charset_file] fontfile.json [first last] .. [firstN lastN]\n"
            "  -f charset_file: UTF-8 text file with characters to include\n"
            "  fontfile.json: a handwritten font saved by the hw-fonts editor\n",
            programName, programName);
}

cp_ranges_t *SortedCharMap = 0;

void cpr_insert_output_cp(int codePoint)
{
    cpr_insert(&SortedCharMap, codePoint);
}

void clearName(char *fname)
{
    while (*fname) {
        char *fl = fname;
        while (*fl && !isalnum(*fl)) {
            ++fl;
        }
        if (fl != fname) {
            memmove(fname, fl, strlen(fl) + 1);
        }
        ++fname;
    }
}

void encodeBMXRLE(uint8_t *bitmap, uint16_t pitch, uint16_t width, uint16_t height, uint8_t **ret, size_t *ret_len)
{
    width        = (width + 7) / 8;
    size_t   sq  = (size_t)width * height;
    uint8_t *xbm = (uint8_t *)malloc(sq);
    memcpy(xbm, bitmap, width);
    for (size_t yi = 1; yi < height; ++yi) {
        for (size_t xi = 0; xi < width; xi++)
            xbm[yi * width + xi] = bitmap[yi * pitch + xi] ^ bitmap[(yi - 1) * pitch + xi];
    }
    uint8_t *rbm = (uint8_t *)calloc(2, sq);
    size_t   ro  = 0;
    for (size_t xi = 0; xi < sq;) {
        uint8_t cb = xbm[xi];
        uint8_t bl = 1;
        for (; bl < 127 && xi + bl < sq; ++bl)
            if (xbm[xi + bl] != cb) break;
        if (bl == 1 && cb < 129) {
            rbm[ro++] = cb;
        } else {
            rbm[ro++] = bl | 128;
            rbm[ro++] = cb;
        }
        xi += bl;
    }
    *ret_len = ro;
    *ret     = (uint8_t *)realloc(rbm, ro);
}

void decodeBMXRLE(uint8_t *bitmap, uint16_t width, uint16_t height, uint8_t *encoded, size_t encoded_len)
{
    width     = (width + 7) / 8;
    size_t sq = (size_t)width * height;
    for (size_t xi = 0, ei = 0; ei < encoded_len && xi < sq;) {
        uint8_t bl = encoded[ei++];
        if (bl < 129) {
            bitmap[xi++] = bl;
        } else {
            uint8_t cb = encoded[ei++];
            bl &= 0x7f;
            while (bl-- && xi < sq)
                bitmap[xi++] = cb;
        }
    }
    for (size_t xi = width; xi < sq; xi++)
        bitmap[xi] = bitmap[xi] ^ bitmap[xi - width];
}

void encodeBM(uint8_t *bitmap, uint16_t pitch, uint16_t width, uint16_t height, uint8_t **ret, size_t *ret_len)
{
    width        = (width + 7) / 8;
    size_t   sq  = (size_t)width * height;
    uint8_t *xbm = (uint8_t *)malloc(sq);
    for (size_t xi = 0; xi < height; ++xi) {
        memcpy(xbm + xi * width, bitmap + pitch * xi, width);
    }
    *ret_len = sq;
    *ret     = xbm;
} // no decode required

/* ------------------------------------------------------------------------- */
/*  handwritten fonts: the JSON file of the hw-fonts editor                   */
/* ------------------------------------------------------------------------- */

typedef struct _json {
    char          type; /* 'o'bject, 'a'rray, 's'tring, 'n'umber, 'z' null, 't'rue, 'f'alse */
    char         *key;  /* of a member of an object */
    char         *str;
    double        num;
    struct _json *child, *next;
} json_t;

static const char *jsonText, *jsonPos;

static void json_fail(const char *what)
{
    int line = 1;
    for (const char *c = jsonText; c < jsonPos; ++c) line += *c == '\n';
    fprintf(stderr, "JSON error in line %d: %s\n", line, what);
    exit(1);
}

static void json_space(void)
{
    while (*jsonPos == ' ' || *jsonPos == '\t' || *jsonPos == '\n' || *jsonPos == '\r') ++jsonPos;
}

static char *json_string(void)
{
    if (*jsonPos != '"') json_fail("a string expected");
    const char *end = ++jsonPos;
    while (*end && *end != '"') end += *end == '\\' && end[1] ? 2 : 1;
    if (!*end) json_fail("a string is not closed");
    char *str = malloc(end - jsonPos + 1), *out = str;
    while (jsonPos < end) {
        char c = *jsonPos++;
        if (c == '\\') {
            c = *jsonPos++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') {
                /* names and keys of a font are plain; anything else is of no use here */
                if (end - jsonPos < 4) json_fail("a short \\u in a string");
                jsonPos += 4;
                c = '?';
            }
        }
        *out++ = c;
    }
    *out = 0;
    ++jsonPos;
    return str;
}

static int jsonDepth; /* a font is a few levels deep; a file of brackets alone must not take the stack */

static json_t *json_value(void)
{
    json_t *v = calloc(1, sizeof(json_t));
    json_space();
    char c = *jsonPos;
    if (c == '{' || c == '[') {
        if (++jsonDepth > 64) json_fail("nested too deep");
        char close = c == '{' ? '}' : ']';
        v->type = c == '{' ? 'o' : 'a';
        ++jsonPos;
        json_t **tail = &v->child;
        json_space();
        while (*jsonPos != close) {
            char *key = 0;
            if (v->type == 'o') {
                json_space();
                key = json_string();
                json_space();
                if (*jsonPos++ != ':') json_fail("':' expected");
            }
            *tail = json_value();
            (*tail)->key = key;
            tail = &(*tail)->next;
            json_space();
            if (*jsonPos == ',') {
                ++jsonPos;
                json_space();
            } else if (*jsonPos != close) {
                json_fail("',' expected");
            }
        }
        ++jsonPos;
        --jsonDepth;
    } else if (c == '"') {
        v->type = 's';
        v->str = json_string();
    } else if (!strncmp(jsonPos, "null", 4)) {
        v->type = 'z';
        jsonPos += 4;
    } else if (!strncmp(jsonPos, "true", 4)) {
        v->type = 't';
        jsonPos += 4;
    } else if (!strncmp(jsonPos, "false", 5)) {
        v->type = 'f';
        jsonPos += 5;
    } else {
        char *end;
        v->type = 'n';
        v->num = strtod(jsonPos, &end);
        if (end == jsonPos) json_fail("a value expected");
        jsonPos = end;
    }
    return v;
}

static json_t *json_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error opening font file %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size + 1);
    if (fread(text, 1, size, f) != (size_t)size) {
        fprintf(stderr, "Error reading font file %s\n", path);
        exit(1);
    }
    text[size] = 0;
    fclose(f);
    jsonText = jsonPos = text;
    json_t *v = json_value();
    json_space();
    if (*jsonPos) json_fail("something after the end of the font");
    return v;
}

static json_t *json_get(json_t *obj, const char *key)
{
    for (json_t *m = obj && obj->type == 'o' ? obj->child : 0; m; m = m->next) {
        if (!strcmp(m->key, key)) return m;
    }
    return 0;
}

static int json_count(json_t *arr)
{
    int n = 0;
    for (json_t *m = arr ? arr->child : 0; m; m = m->next) ++n;
    return n;
}

static const char *hwWhere = "the font"; /* for messages */

static char hwUnfinished[300]; /* the file being written, till it is whole */

static void hw_drop_unfinished(void)
{
    if (hwUnfinished[0]) remove(hwUnfinished);
}

/* a whole number from min to max; without one the font is not what this program can use */
static int hw_number(json_t *obj, const char *key, double scale, int min, int max)
{
    json_t *v = json_get(obj, key);
    if (!v || v->type != 'n') {
        fprintf(stderr, "%s: no number \"%s\". Save the font with the current hw-fonts editor.\n", hwWhere, key);
        exit(1);
    }
    double n = v->num * scale;
    int    r = !(n >= min - 1.0 && n <= max + 1.0) ? max + 1 : (int)(n < 0 ? n - 0.5 : n + 0.5);
    if (r < min || r > max) {
        fprintf(stderr, "%s: \"%s\" is %g, it must be from %g to %g\n", hwWhere, key, v->num, min / scale, max / scale);
        exit(1);
    }
    return r;
}

typedef struct {
    uint32_t codePoint;
    json_t  *symbol;
} hw_symbol_t;

static int hw_symbol_cmp(const void *a, const void *b)
{
    uint32_t ca = ((const hw_symbol_t *)a)->codePoint, cb = ((const hw_symbol_t *)b)->codePoint;
    return ca < cb ? -1 : ca > cb;
}

/* the format of the editor's file this program knows, see font-format-ru.md of hw-fonts */
#define HW_FORMAT_NAME    "hw-font"
#define HW_FORMAT_VERSION 1

static const char *const hwParts[4] = {"beginConnection", "mainSegments", "endConnection", "postSegments"};
static const char *const hwTypes[5] = {0, "dot", "line", "curve3p", "curve"};
static const char *const hwTypeNames[5] = {0, "DGX_HW_DOT", "DGX_HW_LINE", "DGX_HW_CURVE3P", "DGX_HW_CURVE"};

/* the elements of a part of a symbol; returns how many */
static int hw_elements(FILE *out, json_t *symbol, const char *part)
{
    json_t *arr = json_get(symbol, part);
    if (!arr || arr->type != 'a') {
        fprintf(stderr, "%s: no \"%s\"\n", hwWhere, part);
        exit(1);
    }
    int n = 0;
    for (json_t *e = arr->child; e; e = e->next, ++n) {
        json_t *points = json_get(e, "points");
        json_t *type = json_get(e, "type");
        int     np = json_count(points);
        if (np < 1 || np > 4 || !type || type->type != 's' || strcmp(type->str, hwTypes[np])) {
            fprintf(stderr, "%s: an element of \"%s\" is \"%s\" with %d points\n", hwWhere, part,
                    type && type->type == 's' ? type->str : "?", np);
            exit(1);
        }
        fprintf(out, "    {%-14s, %2d, %5d, %3d, {", hwTypeNames[np], hw_number(e, "pieces", 1, 0, 255),
                hw_number(e, "length", 100, 0, 65535), hw_number(e, "pixels", 1, 0, 65535));
        for (json_t *p = points->child; p; p = p->next) {
            fprintf(out, "{%3d, %3d}%s", hw_number(p, "x", 1, 0, 255), hw_number(p, "y", 1, 0, 255), p->next ? ", " : "");
        }
        fprintf(out, "}},\n");
    }
    return n;
}

static int hw_font(const char *fontFile, char **ranges, int numRanges, const char *charsetFile)
{
    for (int ri = 0; ri + 1 < numRanges; ri += 2) {
        cpr_insert_range(&CPRanges, strtol(ranges[ri], 0, 0), strtol(ranges[ri + 1], 0, 0));
    }
    if (charsetFile) loadCharsetFile(charsetFile);

    json_t *font = json_load(fontFile);
    json_t *format = json_get(font, "format");
    json_t *version = json_get(font, "formatVersion");
    json_t *name = json_get(font, "name");
    json_t *codePoints = json_get(font, "codePoints");
    if (!format || format->type != 's' || !version || version->type != 'n') {
        fprintf(stderr,
                "%s has no \"format\" and \"formatVersion\": it is not a font of the hw-fonts editor or an old one.\n"
                "Load an old font into the current editor and save it again.\n",
                fontFile);
        return 1;
    }
    if (strcmp(format->str, HW_FORMAT_NAME)) {
        fprintf(stderr, "%s is of format \"%s\", not \"%s\"\n", fontFile, format->str, HW_FORMAT_NAME);
        return 1;
    }
    if (version->num != HW_FORMAT_VERSION) {
        fprintf(stderr, "%s is of format version %g, this program knows version %d\n", fontFile, version->num, HW_FORMAT_VERSION);
        return 1;
    }
    if (!name || name->type != 's' || !name->str[0] || !codePoints || codePoints->type != 'o') {
        fprintf(stderr, "%s: no \"name\" or \"codePoints\"\n", fontFile);
        return 1;
    }
    int symbolOffsetX = hw_number(font, "symbolOffsetX", 1, 0, 255);
    int symbolOffsetY = hw_number(font, "symbolOffsetY", 1, 0, 255);
    int symbolSizeX = hw_number(font, "symbolSizeX", 1, 1, 256);
    int symbolSizeY = hw_number(font, "symbolSizeY", 1, 1, 256);
    int baseLine = hw_number(font, "baseLine", 1, 0, 255);
    int xHeight = hw_number(font, "xHeight", 1, 0, 255);
    int symbolSpace = hw_number(font, "symbolSpace", 1, 0, 255);
    int spaceWidth = hw_number(font, "spaceWidth", 1, 0, 255);

    int          count = 0;
    hw_symbol_t *symbols = calloc(json_count(codePoints) + 1, sizeof(hw_symbol_t));
    for (json_t *s = codePoints->child; s; s = s->next) {
        char         *rest = 0;
        unsigned long key = s->key && isdigit((unsigned char)s->key[0]) ? strtoul(s->key, &rest, 10) : 0;
        if (!rest || *rest || key > 0x10FFFF) {
            fprintf(stderr, "%s: \"%s\" in \"codePoints\" is not a code point\n", fontFile, s->key ? s->key : "");
            return 1;
        }
        uint32_t cp = (uint32_t)key;
        if (CPRanges && !isInRange(cp)) continue;
        symbols[count].codePoint = cp;
        symbols[count++].symbol = s;
        cpr_insert_output_cp(cp);
    }
    if (!count) {
        fprintf(stderr, "No symbols of %s are in the given ranges\n", fontFile);
        return 1;
    }
    qsort(symbols, count, sizeof(hw_symbol_t), hw_symbol_cmp);
    for (int i = 1; i < count; ++i) {
        if (symbols[i].codePoint == symbols[i - 1].codePoint) {
            fprintf(stderr, "%s: code point %u is in \"codePoints\" twice\n", fontFile, symbols[i].codePoint);
            return 1;
        }
    }

    char funcname[256], fontname[260];
    snprintf(funcname, sizeof(funcname) - 2, "%s", name->str);
    clearName(funcname);
    if (!funcname[0] || isdigit((unsigned char)funcname[0])) {
        memmove(funcname + 2, funcname, strlen(funcname) + 1);
        memcpy(funcname, "hw", 2);
    }
    /* the name must not be one of those the written file uses itself */
    static const char *const taken[] = {"hw", "elements", "symbols", "glyphs", "glyph_ranges", "rval", 0};
    for (int i = 0; taken[i]; ++i) {
        if (!strcmp(funcname, taken[i])) strcat(funcname, "_font");
    }
    /* the name as a C string */
    char cname[256];
    int  ci = 0;
    for (const char *c = name->str; *c && ci < 250; ++c) {
        if (*c == '"' || *c == '\\') cname[ci++] = '\\';
        if ((unsigned char)*c >= ' ') cname[ci++] = *c;
    }
    cname[ci] = 0;
    /* the same for a comment, which the name must not close */
    char comment[256];
    snprintf(comment, sizeof(comment), "%s", cname);
    for (char *c = comment; (c = strstr(c, "*/")) != 0;) c[1] = ' ';
    /* written aside and put in place when all of it is there: a font that fails halfway leaves the old file */
    snprintf(fontname, sizeof(fontname), "%s.c", funcname);
    snprintf(hwUnfinished, sizeof(hwUnfinished), "%s.tmp", fontname);
    atexit(hw_drop_unfinished);
    FILE *out = fopen(hwUnfinished, "w");
    if (!out) {
        fprintf(stderr, "Error opening font file %s for writing: %s\n", hwUnfinished, strerror(errno));
        hwUnfinished[0] = 0;
        return 1;
    }
    fprintf(out, "#include \"dgx_hw_font.h\"\n");
    fprintf(out, "/* %s: made by font2c from a file of format %s, version %d */\n", comment, HW_FORMAT_NAME, HW_FORMAT_VERSION);
    fprintf(out, "static const dgx_hw_element_t elements[] = {\n");
    int   total = 0;
    int (*parts)[4] = calloc(count, sizeof(*parts));
    int  *first = calloc(count, sizeof(int));
    char  where[64];
    for (int i = 0; i < count; ++i) {
        snprintf(where, sizeof(where), "symbol %u (U+%04X)", symbols[i].codePoint, symbols[i].codePoint);
        hwWhere = where;
        fprintf(out, "/* %04X */\n", symbols[i].codePoint);
        first[i] = total;
        for (int k = 0; k < 4; ++k) {
            parts[i][k] = hw_elements(out, symbols[i].symbol, hwParts[k]);
            total += parts[i][k];
            if (parts[i][k] > 255 || total > 65535) {
                fprintf(stderr, "%s: too many elements\n", hwWhere);
                return 1;
            }
        }
    }
    fprintf(out, "};\n\n");

    typedef struct {
        int width, height, xAdvance, xOffset, yOffset;
    } hw_glyph_t;
    hw_glyph_t *gl = calloc(count, sizeof(hw_glyph_t));
    fprintf(out, "static const dgx_hw_symbol_t symbols[] = {\n");
    for (int i = 0; i < count; ++i) {
        json_t *s = symbols[i].symbol;
        snprintf(where, sizeof(where), "symbol %u (U+%04X)", symbols[i].codePoint, symbols[i].codePoint);
        hwWhere = where;
        json_t *up = json_get(s, "upLeft");
        int     hasUp = up && up->type == 'n';
        int     width = hw_number(s, "width", 1, 0, 256), top = hw_number(s, "top", 1, -255, 255);
        int     bottom = hw_number(s, "bottom", 1, -255, 255), left = hw_number(s, "left", 1, -255, 255);
        int     lineLeft = hw_number(s, "lineLeft", 1, -255, 255), advance = hw_number(s, "advance", 1, 0, 1024);
        /* the spaces of its own a symbol may have */
        int spaceBefore = json_get(s, "spaceBefore") ? hw_number(s, "spaceBefore", 1, 0, 255) : 0;
        int spaceAfter = json_get(s, "spaceAfter") ? hw_number(s, "spaceAfter", 1, 0, 255) : symbolSpace;
        fprintf(out, "    {%d, %d, %d, %d, %3d, %4d, %4d, %3d, %3d, %3d, %3d, %3d, ", parts[i][0], parts[i][1], parts[i][2], parts[i][3],
                width, top, bottom, left, hw_number(s, "right", 1, -255, 255), lineLeft, hw_number(s, "lineRight", 1, -255, 255),
                hw_number(s, "lineWidth", 1, 0, 256));
        if (hasUp) fprintf(out, "%3d, %3d, ", hw_number(s, "upLeft", 1, -255, 255), hw_number(s, "upRight", 1, -255, 255));
        else fprintf(out, "DGX_HW_NONE, DGX_HW_NONE, ");
        fprintf(out, "%3d, %2d, %2d}, /* %04X */\n", advance, spaceBefore, spaceAfter, symbols[i].codePoint);
        gl[i] = (hw_glyph_t){width, bottom - top + 1, advance, spaceBefore + left - lineLeft, top};
    }
    fprintf(out, "};\n\n");
    hwWhere = "the font";

    /* what a font tells of all its glyphs together */
    int    yOffsetLowest = gl[0].yOffset, yBottomMax = gl[0].yOffset + gl[0].height, xWidest = gl[0].width;
    int    xOffsetLowest = gl[0].xOffset, xRightMax = gl[0].xOffset + gl[0].width;
    double xWidthAverage = 0;
    fprintf(out, "static const glyph_t glyphs[] = {\n");
    for (int i = 0; i < count; ++i) {
        fprintf(out, "    {{.elements = elements + %4d, .hw = symbols + %3d}, %3d, %3d, %3d, %3d, %4d}, /* %04X */\n", first[i], i,
                gl[i].width, gl[i].height, gl[i].xAdvance, gl[i].xOffset, gl[i].yOffset, symbols[i].codePoint);
        if (yOffsetLowest > gl[i].yOffset) yOffsetLowest = gl[i].yOffset;
        if (yBottomMax < gl[i].yOffset + gl[i].height) yBottomMax = gl[i].yOffset + gl[i].height;
        if (xWidest < gl[i].width) xWidest = gl[i].width;
        if (xOffsetLowest > gl[i].xOffset) xOffsetLowest = gl[i].xOffset;
        if (xRightMax < gl[i].xOffset + gl[i].width) xRightMax = gl[i].xOffset + gl[i].width;
        xWidthAverage += gl[i].width;
    }
    fprintf(out, "};\n\n");

    fprintf(out, "static const glyph_array_t glyph_ranges[] = {\n");
    int numberOfRanges = 0, sidx = 0;
    for (cp_ranges_t *r = SortedCharMap; r; r = r->next, ++numberOfRanges) {
        fprintf(out, "    {0x%04x, %3d, glyphs + %3d},\n", r->first, r->number, sidx);
        sidx += r->number;
    }
    fprintf(out, "    {0, 0, 0},\n};\n\n");

    fprintf(out,
            "static const dgx_hw_font_t hw = {\n"
            "    .name = \"%s\",\n"
            "    .format_version = %d,\n"
            "    .base_line = %d,\n"
            "    .x_height = %d,\n"
            "    .symbol_space = %d,\n"
            "    .space_width = %d,\n"
            "    .symbol_offset_x = %d,\n"
            "    .symbol_offset_y = %d,\n"
            "    .symbol_size_x = %d,\n"
            "    .symbol_size_y = %d,\n"
            "};\n\n",
            cname, HW_FORMAT_VERSION, baseLine, xHeight, symbolSpace, spaceWidth, symbolOffsetX, symbolOffsetY, symbolSizeX,
            symbolSizeY);
    fprintf(out,
            "dgx_font_t *%s()\n{\n"
            "    static dgx_font_t rval = {\n"
            "        .glyph_ranges = glyph_ranges,\n"
            "        .yAdvance = %d,\n"
            "        .yOffsetLowest = %d,\n"
            "        .xWidest = %d,\n"
            "        .xWidthAverage = %d,\n"
            "        .f_type = DGX_FONT_HW,\n"
            "        .yBottomMax = %d,\n"
            "        .xOffsetLowest = %d,\n"
            "        .xRightMax = %d,\n"
            "        .number_of_ranges = %d,\n"
            "        .hw = &hw,\n"
            "    };\n"
            "    return &rval;\n}\n",
            funcname, symbolSizeY, yOffsetLowest, xWidest, (int)(xWidthAverage / count + 0.5), yBottomMax, xOffsetLowest, xRightMax,
            numberOfRanges);
    if (fclose(out) || rename(hwUnfinished, fontname)) {
        fprintf(stderr, "Error writing font file %s: %s\n", fontname, strerror(errno));
        return 1;
    }
    hwUnfinished[0] = 0;

    snprintf(fontname, sizeof(fontname), "%s.h", funcname);
    out = fopen(fontname, "w");
    if (!out) {
        fprintf(stderr, "Error opening font header file %s for writing: %s\n", fontname, strerror(errno));
        return 1;
    }
    fprintf(out, "#pragma once\n");
    fprintf(out, "#include \"dgx_hw_font.h\"\n");
    fprintf(out, "#ifdef __cplusplus\n// @formatter:off\nextern \"C\" {\n// @formatter:on\n#endif\n");
    fprintf(out, "dgx_font_t *%s();\n", funcname);
    fprintf(out, "#ifdef __cplusplus\n// @formatter:off\n}\n// @formatter:on\n#endif\n");
    fclose(out);

    printf("%s: %d symbols in %d ranges, %d elements, about %zu bytes on a 32-bit target\n", funcname, count, numberOfRanges, total,
           total * (size_t)14 + count * (size_t)(30 + 20) + (numberOfRanges + 1) * (size_t)12);
    return 0;
}

int main(int argc, char *argv[])
{
    int                i;
    int                j;
    int                err;
    int                size;
    int                bitmapOffset = 0;
    FT_Library         library;
    FT_Face            face;
    FT_Glyph           glyph;
    FT_Bitmap         *bitmap;
    FT_BitmapGlyphRec *g;
    LoadedGlyph       *glyphs;

    const char *charsetFile = 0;
    int         opt;
    while ((opt = getopt(argc, argv, "f:")) != -1) {
        switch (opt) {
        case 'f':
            charsetFile = optarg;
            break;
        default:
            show_usage(argv[0]);
            return 1;
        }
    }

    if (argc - optind >= 1) {
        size_t len = strlen(argv[optind]);
        if (len > 5 && !strcmp(argv[optind] + len - 5, ".json")) {
            return hw_font(argv[optind], argv + optind + 1, argc - optind - 1, charsetFile);
        }
    }

    if (argc - optind < 2) {
        show_usage(argv[0]);
        return 1;
    }

    const char *fontFile = argv[optind];
    size                 = atoi(argv[optind + 1]);

    for (int ri = optind + 2; ri + 1 < argc; ri += 2) {
        int first = strtol(argv[ri], 0, 0);
        int last  = strtol(argv[ri + 1], 0, 0);
        cpr_insert_range(&CPRanges, first, last);
    }

    if (charsetFile) {
        loadCharsetFile(charsetFile);
    }

    // Init FreeType lib, load font
    if ((err = FT_Init_FreeType(&library))) {
        fprintf(stderr, "FreeType init error: %d", err);
        return err;
    }

    // Use TrueType engine version 35, without subpixel rendering.
    // This improves clarity of fonts since this library does not
    // support rendering multiple levels of gray in a glyph.
    // See https://github.com/adafruit/Adafruit-GFX-Library/issues/103
    //    FT_UInt interpreter_version = TT_INTERPRETER_VERSION_35;
    //    FT_Property_Set(library, "truetype", "interpreter-version", &interpreter_version);

    if ((err = FT_New_Face(library, fontFile, 0, &face))) {
        fprintf(stderr, "Font load error: %d", err);
        FT_Done_FreeType(library);
        return err;
    }
    char fontname[256], funcname[256];
    snprintf(funcname, 256, "%s%s%d", face->family_name, face->style_name, size);
    clearName(funcname);
    strcpy(fontname, funcname);
    strncat(fontname, ".c", 255);
    FILE *fontOut = fopen(fontname, "w");
    if (!fontOut) {
        fprintf(stderr, "Error opening font file %s for writing: ", strerror(errno));
        return -1;
    }
    glyphs = calloc(face->num_glyphs, sizeof(LoadedGlyph));
    err    = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (err) {
        fprintf(stderr, "FreeType FT_Select_Charmap error: %d", err);
        return err;
    }
    /*
     // << 6 because '26dot6' fixed-point format
     FT_Set_Char_Size(face, size << 6, 0, DPI, 0);
     */
    FT_Set_Pixel_Sizes(face, size, 0);

    FT_UInt gindex;

    fprintf(fontOut, "#include \"dgx_font.h\"\n");
    fprintf(fontOut, "static const uint8_t bitmaps[] = {\n  ");
    int     fComma        = 0;
    int32_t yOffsetLowest = 0, yBottomMax = 0, xWidest = 0, xOffsetLowest = 0, xRightMax = 0;
    double  xWidthAverage = 0;
    // Process glyphs and output huge bitmap data array
    for (i = FT_Get_First_Char(face, &gindex), j = 0; gindex != 0; i = FT_Get_Next_Char(face, i, &gindex)) {
        if (CPRanges && !isInRange(i)) continue;
        // MONO renderer provides clean image with perfect crop
        // (no wasted pixels) via bitmap struct.
        if ((err = FT_Load_Char(face, i, FT_LOAD_TARGET_MONO))) {
            fprintf(stderr, "Error %d loading char '%c'\n", err, i);
            continue;
        }

        if ((err = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO))) {
            fprintf(stderr, "Error %d rendering char '%c'\n", err, i);
            continue;
        }

        if ((err = FT_Get_Glyph(face->glyph, &glyph))) {
            fprintf(stderr, "Error %d getting glyph '%c'\n", err, i);
            continue;
        }

        bitmap = &face->glyph->bitmap;
        g      = (FT_BitmapGlyphRec *)glyph;
        if (j >= face->num_glyphs) {
            glyphs = realloc(glyphs, (j + 1) * sizeof(LoadedGlyph));
            if (!glyphs) {
                fprintf(stderr, "Exceeded allocated glyphs array size at char '%c'\n", i);
                break;
            }
        }
        glyphs[j].codePoint          = i;
        glyphs[j].ginfo.bitmapOffset = bitmapOffset;
        glyphs[j].ginfo.width        = bitmap->width;
        glyphs[j].ginfo.height       = bitmap->rows;
        glyphs[j].ginfo.xAdvance     = face->glyph->advance.x >> 6;
        glyphs[j].ginfo.xOffset      = g->left;
        glyphs[j].ginfo.yOffset      = 1 - g->top;
        if (j == 0) {
            yOffsetLowest = glyphs[j].ginfo.yOffset;
            yBottomMax    = glyphs[j].ginfo.yOffset + glyphs[j].ginfo.height;
            xWidest       = glyphs[j].ginfo.width;
            xOffsetLowest = glyphs[j].ginfo.xOffset;
            xRightMax     = glyphs[j].ginfo.xOffset + glyphs[j].ginfo.width;
            xWidthAverage = glyphs[j].ginfo.width;
        } else {
            if (yOffsetLowest > glyphs[j].ginfo.yOffset) yOffsetLowest = glyphs[j].ginfo.yOffset;
            if (yBottomMax < glyphs[j].ginfo.yOffset + glyphs[j].ginfo.height) yBottomMax = glyphs[j].ginfo.yOffset + glyphs[j].ginfo.height;
            if (xWidest < glyphs[j].ginfo.width) xWidest = glyphs[j].ginfo.width;
            if (xOffsetLowest > glyphs[j].ginfo.xOffset) xOffsetLowest = glyphs[j].ginfo.xOffset;
            if (xRightMax < glyphs[j].ginfo.xOffset + glyphs[j].ginfo.width) xRightMax = glyphs[j].ginfo.xOffset + glyphs[j].ginfo.width;
            xWidthAverage += glyphs[j].ginfo.width;
        }
        cpr_insert_output_cp(i);
        char gname[64];
        if (0 == FT_Get_Glyph_Name(face, gindex, gname, sizeof(gname))) {
            clearName(gname);
            fprintf(fontOut, "/* %04X - %.63s */", i, gname);
        } else {
            fprintf(fontOut, "/* %04X */", i);
        }
        uint8_t *ret;
        size_t   ret_len;
        encodeBM(bitmap->buffer, bitmap->pitch, bitmap->width, bitmap->rows, &ret, &ret_len);
        for (int m = 0; m < bitmap->rows; ++m) {
            fprintf(fontOut, "\n/* ");
            for (int n = 0; n < bitmap->width; ++n) {
                uint8_t b = bitmap->buffer[bitmap->pitch * m + n / 8];
                fprintf(fontOut, "%c", (b & (0x80 >> (n & 7))) ? '#' : ' ');
            }
            fprintf(fontOut, " */");
        }
        for (int m = 0; m < ret_len; ++m) {
            if (fComma != 0) {
                fprintf(fontOut, ", ");
            }
            ++fComma;
            if (m % 12 == 0) fprintf(fontOut, "\n");
            fprintf(fontOut, "0x%02X", ret[m]);
        }
        bitmapOffset += ret_len;
        free(ret);
        fprintf(fontOut, "\n");
        FT_Done_Glyph(glyph);
        ++j;
    }
    if (j) {
        printf("Total width: %f, #glyphs: %d; avg = %f\n", xWidthAverage, j, xWidthAverage / j);
        xWidthAverage /= j;
    }
    fprintf(fontOut, "};\n\n"); // End bitmap array
    fprintf(stderr, "Total encoded length: %d\n", bitmapOffset);
    fprintf(fontOut, "static const glyph_t glyphs[] = {\n  ");
    int gidx = 0;
    for (cp_ranges_t *r = SortedCharMap; r; r = r->next) {
        r->gOffset = gidx;
        for (int cp = r->first; cp < r->first + r->number; ++cp) {
            if (gidx != 0) fprintf(fontOut, ", ");
            for (int gi = 0; gi < j; ++gi) {
                if (glyphs[gi].codePoint == cp) {
                    fprintf(fontOut, "  { {.bitmap = bitmaps + %5lu }, %3u, %3d, %3u, %4d, %4d } /* %04X */\n", glyphs[gi].ginfo.bitmapOffset,
                            glyphs[gi].ginfo.width, glyphs[gi].ginfo.height, glyphs[gi].ginfo.xAdvance, glyphs[gi].ginfo.xOffset,
                            glyphs[gi].ginfo.yOffset, cp);
                    gidx++;
                    break;
                }
            }
        }
    }
    fprintf(fontOut, "};\n");
    fprintf(fontOut, "static const glyph_array_t glyph_ranges[] = {\n  ");
    int numberOfRanges = 0;
    for (cp_ranges_t *r = SortedCharMap; r; r = r->next, ++numberOfRanges) {
        if (r != SortedCharMap) fprintf(fontOut, ",");
        fprintf(fontOut, " {0x%-4x, 0x%-4x, glyphs + %d }\n", r->first, r->number, r->gOffset);
    }
    fprintf(fontOut, ", {0x%-4x, 0x%-4x, %d }\n", 0, 0, 0);
    fprintf(fontOut, "};\n");
    fprintf(fontOut, "// Bitmap size: %d\n", bitmapOffset);

    fprintf(fontOut,
            "dgx_font_t* %s() {\n\t"
            "static dgx_font_t rval = { \n\t\t"
            ".glyph_ranges = glyph_ranges,\n\t\t"
            ".yAdvance = %ld,\n\t\t"
            ".yOffsetLowest = %d,\n\t\t"
            ".xWidest = %d,\n\t\t"
            ".xWidthAverage = %f,\n\t"
            ".f_type = DGX_FONT_BITMAP_LINES,\n\t\t"
            ".yBottomMax = %d,\n\t\t"
            ".xOffsetLowest = %d,\n\t\t"
            ".xRightMax = %d,\n\t\t"
            ".number_of_ranges = %d\n\t"
            "};\n\t"
            "return &rval;\n}\n",
            funcname,                                                                                           //
            (face->size->metrics.height == 0 ? (long)glyphs[0].ginfo.height : face->size->metrics.height >> 6), //
            yOffsetLowest,                                                                                      //
            xWidest,                                                                                            //
            xWidthAverage,                                                                                      //
            yBottomMax,                                                                                         //
            xOffsetLowest,                                                                                      //
            xRightMax,                                                                                          //
            numberOfRanges                                                                                      //
    );
    fclose(fontOut);
    strcpy(fontname, funcname);
    strncat(fontname, ".h", 255);
    FILE *fontHeaderOut = fopen(fontname, "w");
    if (!fontHeaderOut) {
        fprintf(stderr, "Error opening font header file %s for writing: ", strerror(errno));
        return -1;
    }
    fprintf(fontHeaderOut, "#pragma once\n");
    fprintf(fontHeaderOut, "#include \"dgx_font.h\"\n");
    fprintf(fontHeaderOut, "#ifdef __cplusplus\n// @formatter:off\nextern \"C\" {\n// @formatter:on\n#endif\n");
    fprintf(fontHeaderOut, "dgx_font_t* %s();\n", funcname);
    fprintf(fontHeaderOut, "#ifdef __cplusplus\n// @formatter:off\n}\n// @formatter:on\n#endif\n");

    fclose(fontHeaderOut);

    FT_Done_FreeType(library);

    return 0;
}
