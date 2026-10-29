/*
 * MIT License
 *
 * Copyright (c) 2021-2026 Anton Petrusevich
 *
 */

#include <ctype.h>
#include <dirent.h>
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

#ifdef _WIN32
/* Windows: no realpath(), and rename() does not replace a file that is there */
#define realpath(path, resolved) _fullpath((resolved), (path), 0)
static int dgx_rename(const char *from, const char *to)
{
    remove(to);
    return rename(from, to);
}
#define rename dgx_rename
#endif

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
            "Usage: %s [-n name] [-f charset_file] [-p patch_file] [-P dir] fontfile size [first last] .. [firstN lastN]\n"
            "       %s [-n name] [-f charset_file] [-g cells] [-t cells] [-x | -i] fontfile.json [first last] .. [firstN lastN]\n"
            "  -n name: the name of the font: its function, its files name.c and name.h, and its\n"
            "           static arrays named name_bitmaps and so on, so that several fonts made\n"
            "           this way go into one C file; without it the family, the style and the size\n"
            "  -f charset_file: UTF-8 text file with characters to include\n"
            "  -p patch_file: the fixes of the font editor, a .patch.json; the glyphs it has stand\n"
            "                 for what is rendered, whole, so a font made again keeps them; of a\n"
            "                 patch of several fonts the font takes its own part\n"
            "  -P dir: look for the patch of the font in dir: a .patch.json with a part for the\n"
            "          font, the name of the font being the family, the style and the size, as\n"
            "          without -n; the first dir that has one wins, -p wins over all; may be\n"
            "          given more than once\n"
            "  (on a line of a -jo list -p and -P patch that font; -P on the command line\n"
            "   goes for every font of the lists, after the -P of its lines)\n"
            "  fontfile.json: a handwritten font saved by the hw-fonts editor\n"
            "  -g cells: list every pair of its symbols that stand closer than this\n"
            "            and the spaces that move them apart, 0 for no check; without\n"
            "            it the closest of those closer than the symbol space of the\n"
            "            font are named\n"
            "  -t cells: how far apart the spaces move such a pair, if further than -g\n"
            "  -x: put those spaces into fontfile.json and convert the font with them\n"
            "  -i: the same, asking about each space\n",
            programName, programName);
}

/* the coverage a pixel must have to count as ink, of 255 */
#define INK_THRESHOLD 110

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

/* ------------------------------------------------------------------------- */
/*  handwritten fonts: the JSON file of the hw-fonts editor                   */
/* ------------------------------------------------------------------------- */

typedef struct _json {
    char          type; /* 'o'bject, 'a'rray, 's'tring, 'n'umber, 'z' null, 't'rue, 'f'alse */
    char         *key;  /* of a member of an object */
    char         *str;
    double        num;
    struct _json *child, *next;
    const char   *name, *from, *to; /* in the text: where its key begins, the value itself */
    int           edit;             /* of jsonEdits, from 1, when the value is to be written anew */
} json_t;

static const char *jsonText, *jsonPos, *jsonPath;

/* what is to be changed in the text of the file: "lead" and a number in place of "len" bytes at "at" */
typedef struct {
    size_t at, len;
    char  *lead;
    int    value, order;
} json_edit_t;

static json_edit_t *jsonEdits;
static int          jsonEditCount;

static void json_fail(const char *what)
{
    int line = 1;
    for (const char *c = jsonText; c < jsonPos; ++c) line += *c == '\n';
    fprintf(stderr, "%s: JSON error in line %d: %s\n", jsonPath, line, what);
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
    v->from = jsonPos;
    char c = *jsonPos;
    if (c == '{' || c == '[') {
        if (++jsonDepth > 64) json_fail("nested too deep");
        char close = c == '{' ? '}' : ']';
        v->type = c == '{' ? 'o' : 'a';
        ++jsonPos;
        json_t **tail = &v->child;
        json_space();
        while (*jsonPos != close) {
            char       *key = 0;
            const char *name = 0;
            if (v->type == 'o') {
                json_space();
                name = jsonPos;
                key = json_string();
                json_space();
                if (*jsonPos++ != ':') json_fail("':' expected");
            }
            *tail = json_value();
            (*tail)->key = key;
            (*tail)->name = name;
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
    v->to = jsonPos;
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
    jsonPath = path;
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

/*
 * Gives a member of an object a whole number, in what was read and in the
 * text of the file as json_save() writes it. A member the object has not is
 * put after its last one, written the way that one is.
 */
static void json_set_number(json_t *obj, const char *key, int value)
{
    json_t *m = json_get(obj, key);
    if (!m) {
        /* the last member read from the file: those put here before have no place in the text */
        json_t *last = obj->child && obj->child->name ? obj->child : 0;
        while (last && last->next && last->next->name) last = last->next;
        if (!last) {
            fprintf(stderr, "Cannot put \"%s\" into an empty object\n", key);
            exit(1);
        }
        const char *space = last->name, *colon = last->from;
        while (space > jsonText && strchr(" \t\r\n", space[-1])) --space;
        while (colon > last->name && colon[-1] != ':') --colon;
        jsonEdits = realloc(jsonEdits, (jsonEditCount + 1) * sizeof(json_edit_t));
        json_edit_t *e = &jsonEdits[jsonEditCount];
        size_t       lead = 1 + (last->name - space) + 1 + strlen(key) + 2 + (last->from - colon) + 1;
        e->lead = malloc(lead);
        snprintf(e->lead, lead, ",%.*s\"%s\":%.*s", (int)(last->name - space), space, key, (int)(last->from - colon), colon);
        e->at = last->to - jsonText;
        e->len = 0;
        e->order = jsonEditCount;
        m = calloc(1, sizeof(json_t));
        m->key = strdup(key);
        m->edit = ++jsonEditCount;
        json_t **tail = &last->next;
        while (*tail) tail = &(*tail)->next;
        *tail = m;
    } else if (!m->edit) {
        jsonEdits = realloc(jsonEdits, (jsonEditCount + 1) * sizeof(json_edit_t));
        jsonEdits[jsonEditCount] = (json_edit_t){m->from - jsonText, m->to - m->from, strdup(""), 0, jsonEditCount};
        m->edit = ++jsonEditCount;
    }
    m->type = 'n';
    m->num = value;
    jsonEdits[m->edit - 1].value = value;
}

static int json_edit_cmp(const void *a, const void *b)
{
    const json_edit_t *ea = a, *eb = b;
    if (ea->at != eb->at) return ea->at < eb->at ? -1 : 1;
    return ea->order - eb->order;
}

/* writes the text of the file with what json_set_number() has changed; aside first, so that a failure leaves the file whole */
static int json_save(const char *path)
{
    char aside[1024];
    if (snprintf(aside, sizeof(aside), "%s.tmp", path) >= (int)sizeof(aside)) return -1;
    FILE *f = fopen(aside, "wb");
    if (!f) return -1;
    qsort(jsonEdits, jsonEditCount, sizeof(json_edit_t), json_edit_cmp);
    size_t at = 0, size = strlen(jsonText);
    for (int i = 0; i < jsonEditCount; ++i) {
        fwrite(jsonText + at, 1, jsonEdits[i].at - at, f);
        fprintf(f, "%s%d", jsonEdits[i].lead, jsonEdits[i].value);
        at = jsonEdits[i].at + jsonEdits[i].len;
    }
    fwrite(jsonText + at, 1, size - at, f);
    if (ferror(f) | fclose(f) || rename(aside, path)) {
        remove(aside);
        return -1;
    }
    return 0;
}

/*
 * -n: the name of the font, the function of the font and the file by it, and its static arrays with it before
 * their names: several fonts made so go into one C file without a clash. Without it the names are as they were.
 */
static const char *nameOverride = 0, *arrayPrefix = "";
static char        arrayPrefixBuf[256];

/* the words of C and of C++, whose compilers take the header too: a font named so makes code that does not build */
static int is_keyword(const char *name)
{
    static const char *const words[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern", "float", "for",
        "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
        "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool",
        "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local", "alignas", "alignof", "bool",
        "constexpr", "false", "nullptr", "static_assert", "thread_local", "true", "typeof", "typeof_unqual", "asm", "catch",
        "char8_t", "char16_t", "char32_t", "class", "concept", "consteval", "constinit", "const_cast", "co_await", "co_return",
        "co_yield", "decltype", "delete", "dynamic_cast", "explicit", "export", "friend", "mutable", "namespace", "new", "noexcept",
        "operator", "private", "protected", "public", "reinterpret_cast", "requires", "static_cast", "template", "this", "throw",
        "try", "typeid", "typename", "using", "virtual", "wchar_t", 0};
    for (int i = 0; words[i]; ++i) {
        if (!strcmp(name, words[i])) return 1;
    }
    return 0;
}

static int name_option(const char *name)
{
    size_t len = strlen(name);
    if (!len || len > 200 || isdigit((unsigned char)name[0])) return 0;
    for (size_t i = 0; i < len; ++i) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_') return 0;
    }
    if (is_keyword(name)) {
        fprintf(stderr, "-n %s: a keyword of C or C++ cannot name a function\n", name);
        return 0;
    }
    nameOverride = name;
    snprintf(arrayPrefixBuf, sizeof(arrayPrefixBuf), "%s_", name);
    arrayPrefix = arrayPrefixBuf;
    return 1;
}

static const char *hwWhere = "the font"; /* for messages */

static char *hwUnfinished; /* the file being written, till it is whole: its name with .tmp after it */

static void hw_drop_unfinished(void)
{
    if (hwUnfinished) remove(hwUnfinished);
}

/* the name `path` is written under till it is whole, as long as the path is; 0 when nothing is being written any more */
static const char *unfinished(const char *path)
{
    free(hwUnfinished);
    hwUnfinished = 0;
    if (path) {
        hwUnfinished = malloc(strlen(path) + sizeof(".tmp"));
        sprintf(hwUnfinished, "%s.tmp", path);
    }
    return hwUnfinished;
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

/*
 * Pairs of symbols that stand too close. A symbol is its main and postponed
 * strokes, what is written of it apart from its neighbours, as straight
 * pieces; the second symbol of a pair stands where a line puts it after the
 * first. The distance is between the lines the middle of the pen goes along,
 * in cells: a pen takes its thickness off it.
 */
typedef struct {
    double x0, y0, x1, y1;
    int    element; /* of the symbol, main strokes and postponed ones counted together */
} hw_piece_t;

typedef struct {
    hw_piece_t *pieces;
    int         n, elements;
    int         lineLeft, lineWidth, spaceBefore, spaceAfter, hasUp, upLeft, upRight;
} hw_ink_t;

typedef struct {
    double d;
    int    a, b;
} hw_pair_t;

#define HW_CURVE_PIECES 32
#define HW_PAIRS_NAMED  20
#define HW_ADVICE_NAMED 8
#define HW_SPACE_MOST   255 /* of "spaceBefore" and "spaceAfter" */
#define HW_PARTS_JOINED 3   /* strokes of a symbol whose lines come this close, in cells, are one part of it */

static double hwPairLimit = -1; /* -g: below 0 it is the symbol space of the font */
static double hwPairApart = -1; /* -t: how far apart the advice moves a pair; below 0 as far as the limit */
static int    hwPairFix;        /* -x: 1, the file of the font is changed as advised; -i: 2, after asking */

static void hw_ink(json_t *symbol, int symbolSpace, hw_ink_t *ink)
{
    json_t *up = json_get(symbol, "upLeft");
    ink->lineLeft = hw_number(symbol, "lineLeft", 1, -255, 255);
    ink->lineWidth = hw_number(symbol, "lineWidth", 1, 0, 256);
    ink->spaceBefore = json_get(symbol, "spaceBefore") ? hw_number(symbol, "spaceBefore", 1, 0, 255) : 0;
    ink->spaceAfter = json_get(symbol, "spaceAfter") ? hw_number(symbol, "spaceAfter", 1, 0, 255) : symbolSpace;
    ink->hasUp = up && up->type == 'n';
    if (ink->hasUp) {
        ink->upLeft = hw_number(symbol, "upLeft", 1, -255, 255);
        ink->upRight = hw_number(symbol, "upRight", 1, -255, 255);
    }
    int elements = json_count(json_get(symbol, "mainSegments")) + json_count(json_get(symbol, "postSegments"));
    free(ink->pieces);
    ink->pieces = calloc(elements * HW_CURVE_PIECES + 1, sizeof(hw_piece_t));
    ink->n = 0;
    ink->elements = 0;
    for (int k = 1; k < 4; k += 2) {
        json_t *part = json_get(symbol, hwParts[k]);
        for (json_t *e = part ? part->child : 0; e; e = e->next, ++ink->elements) {
            double  x[4], y[4];
            int     np = 0;
            json_t *points = json_get(e, "points");
            /* an element that is not one is told of when the elements are written */
            if (json_count(points) < 1 || json_count(points) > 4) continue;
            for (json_t *p = points->child; p; p = p->next, ++np) {
                x[np] = hw_number(p, "x", 1, 0, 255);
                y[np] = hw_number(p, "y", 1, 0, 255);
            }
            int    pieces = np > 2 ? HW_CURVE_PIECES : 1;
            double px = x[0], py = y[0];
            for (int i = 1; i <= pieces; ++i) {
                double t = (double)i / pieces, u = 1 - t, qx, qy;
                if (np == 4) {
                    qx = u * u * u * x[0] + 3 * u * u * t * x[1] + 3 * u * t * t * x[2] + t * t * t * x[3];
                    qy = u * u * u * y[0] + 3 * u * u * t * y[1] + 3 * u * t * t * y[2] + t * t * t * y[3];
                } else if (np == 3) {
                    qx = u * u * x[0] + 2 * u * t * x[1] + t * t * x[2];
                    qy = u * u * y[0] + 2 * u * t * y[1] + t * t * y[2];
                } else {
                    qx = x[np - 1];
                    qy = y[np - 1];
                }
                ink->pieces[ink->n++] = (hw_piece_t){px, py, qx, qy, ink->elements};
                px = qx;
                py = qy;
            }
        }
    }
}

/* distances are kept squared; the root is taken by hand, so that the program needs no library of its own for it */
static double hw_root(double v)
{
    double r = v > 1 ? v : 1;
    for (int i = 0; i < 60 && v > 0; ++i) r = (r + v / r) / 2;
    return v > 0 ? r : 0;
}

static double hw_point_piece(double px, double py, const hw_piece_t *s)
{
    double dx = s->x1 - s->x0, dy = s->y1 - s->y0, len = dx * dx + dy * dy;
    double t = len > 0 ? ((px - s->x0) * dx + (py - s->y0) * dy) / len : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double ex = px - s->x0 - t * dx, ey = py - s->y0 - t * dy;
    return ex * ex + ey * ey;
}

static double hw_side(const hw_piece_t *s, double px, double py)
{
    return (s->x1 - s->x0) * (py - s->y0) - (s->y1 - s->y0) * (px - s->x0);
}

static double hw_piece_piece(const hw_piece_t *a, const hw_piece_t *b)
{
    /* two pieces that cross; those that only touch are caught by their ends */
    if (hw_side(a, b->x0, b->y0) * hw_side(a, b->x1, b->y1) < 0 && hw_side(b, a->x0, a->y0) * hw_side(b, a->x1, a->y1) < 0) return 0;
    double d = hw_point_piece(a->x0, a->y0, b), e = hw_point_piece(a->x1, a->y1, b);
    if (e < d) d = e;
    if ((e = hw_point_piece(b->x0, b->y0, a)) < d) d = e;
    if ((e = hw_point_piece(b->x1, b->y1, a)) < d) d = e;
    return d;
}

static int hw_pair_cmp(const void *a, const void *b)
{
    const hw_pair_t *pa = a, *pb = b;
    if (pa->d != pb->d) return pa->d < pb->d ? -1 : 1;
    return pa->a != pb->a ? pa->a - pb->a : pa->b - pb->b;
}

static void hw_put_symbol(FILE *f, uint32_t cp)
{
    if (cp < 0x80) fprintf(f, "%c", (int)cp);
    else if (cp < 0x800) fprintf(f, "%c%c", (int)(0xC0 | cp >> 6), (int)(0x80 | (cp & 63)));
    else if (cp < 0x10000) fprintf(f, "%c%c%c", (int)(0xE0 | cp >> 12), (int)(0x80 | ((cp >> 6) & 63)), (int)(0x80 | (cp & 63)));
    else fprintf(f, "%c%c%c%c", (int)(0xF0 | cp >> 18), (int)(0x80 | ((cp >> 12) & 63)), (int)(0x80 | ((cp >> 6) & 63)), (int)(0x80 | (cp & 63)));
}

/* the least distance, squared, between a and b standing after it "extra" cells further than a line puts it; "enough" if not less */
static double hw_pair_least(const hw_ink_t *l, const hw_ink_t *r, int extra, double enough)
{
    /* where a line puts b after a, see dgx_hw_line_place() */
    int shift = l->lineLeft + l->lineWidth + l->spaceAfter - r->lineLeft + r->spaceBefore;
    if (l->hasUp && r->hasUp && shift < l->upRight + 1 + l->spaceAfter - r->upLeft + r->spaceBefore) {
        shift = l->upRight + 1 + l->spaceAfter - r->upLeft + r->spaceBefore;
    }
    shift += extra;
    double least = enough;
    for (int j = 0; j < r->n && least > 0; ++j) {
        hw_piece_t moved = r->pieces[j];
        moved.x0 += shift;
        moved.x1 += shift;
        double from = moved.x0 < moved.x1 ? moved.x0 : moved.x1;
        for (int i = 0; i < l->n; ++i) {
            /* most of the first symbol is too far to the left to matter */
            double gap = from - (l->pieces[i].x0 > l->pieces[i].x1 ? l->pieces[i].x0 : l->pieces[i].x1);
            if (gap > 0 && gap * gap >= least) continue;
            double d = hw_piece_piece(&l->pieces[i], &moved);
            if (d < least) least = d;
        }
    }
    return least;
}

/* "y", "n" or "q" to a question; the end of the input is "q" */
static int hw_ask(void)
{
    char line[64];
    fprintf(stderr, " [Y/n/q] ");
    fflush(stderr);
    if (!fgets(line, sizeof(line), stdin)) return 'q';
    char c = (char)tolower((unsigned char)line[0]);
    return c == 'n' || c == 'q' ? c : c == 'y' || c == '\n' || c == '\r' ? 'y' : 'n';
}

/*
 * What to change in the file so that no pair is left: a pair moves apart by
 * the space after its first symbol or the space before its second one. The
 * space that serves the most pairs is taken first, as large as the worst of
 * them needs to stand "apart" cells away, then the same among the pairs
 * still left. With "fix" the spaces are put into the font, each after a
 * question if it is 2. Returns how many were.
 */
static int hw_pairs_advice(const hw_symbol_t *symbols, const hw_ink_t *ink, int count, hw_pair_t *close, int found, double apart,
                           int symbolSpace, int lines, int fix)
{
    int *need = calloc(found, sizeof(int)), *serves = calloc(2 * count, sizeof(int)), *most = calloc(2 * count, sizeof(int));
    int  left = found, more = 0, changed = 0;
    for (int p = 0; p < found; ++p) {
        need[p] = 1;
        while (need[p] < HW_SPACE_MOST && hw_pair_least(&ink[close[p].a], &ink[close[p].b], need[p], apart * apart) < apart * apart) {
            ++need[p];
        }
    }
    fprintf(stderr, "  to move them %g cells apart, in the file of the font:\n", apart);
    while (left) {
        memset(serves, 0, 2 * count * sizeof(int));
        memset(most, 0, 2 * count * sizeof(int));
        for (int p = 0; p < found; ++p) {
            if (!need[p]) continue;
            int sides[2] = {close[p].a, count + close[p].b};
            for (int k = 0; k < 2; ++k) {
                ++serves[sides[k]];
                if (most[sides[k]] < need[p]) most[sides[k]] = need[p];
            }
        }
        int best = 0;
        for (int i = 1; i < 2 * count; ++i) {
            if (serves[i] > serves[best] || (serves[i] == serves[best] && most[i] < most[best])) best = i;
        }
        int         after = best < count, symbol = after ? best : best - count, pairs = serves[best];
        json_t     *s = symbols[symbol].symbol;
        const char *key = after ? "spaceAfter" : "spaceBefore";
        int         now = after ? ink[symbol].spaceAfter : ink[symbol].spaceBefore, space = now + most[best];
        for (int p = 0; p < found; ++p) {
            if (need[p] && (after ? close[p].a : close[p].b) == symbol) need[p] = 0, --left;
        }
        if (lines-- <= 0) {
            ++more;
            continue;
        }
        fprintf(stderr, "    ");
        hw_put_symbol(stderr, symbols[symbol].codePoint);
        fprintf(stderr, " (\"%u\"): %s %d -> %d, %d pair%s", symbols[symbol].codePoint, key, now, space, pairs, pairs == 1 ? "" : "s");
        if (space > HW_SPACE_MOST) {
            fprintf(stderr, ": more than the %d a space may be\n", HW_SPACE_MOST);
            continue;
        }
        int answer = fix == 2 ? hw_ask() : 'y';
        if (fix != 2) fprintf(stderr, "\n");
        if (answer == 'q') fix = 0, lines = 0;
        if (!fix || answer != 'y') continue;
        json_set_number(s, key, space);
        json_t *before = json_get(s, "spaceBefore"), *behind = json_get(s, "spaceAfter");
        json_set_number(s, "advance", (before ? (int)before->num : 0) + ink[symbol].lineWidth + (behind ? (int)behind->num : symbolSpace));
        ++changed;
    }
    if (more > 0) fprintf(stderr, "    and %d more\n", more);
    free(need);
    free(serves);
    free(most);
    return changed;
}

/* the pairs closer than "limit", the closest first; returns how many */
static int hw_pairs_find(const hw_symbol_t *symbols, hw_ink_t *ink, int count, int symbolSpace, double limit, hw_pair_t **pairs)
{
    static char where[64];
    hw_pair_t  *close = 0;
    int         found = 0, room = 0;
    for (int i = 0; i < count; ++i) {
        snprintf(where, sizeof(where), "symbol %u (U+%04X)", symbols[i].codePoint, symbols[i].codePoint);
        hwWhere = where;
        hw_ink(symbols[i].symbol, symbolSpace, &ink[i]);
    }
    hwWhere = "the font";
    for (int a = 0; a < count; ++a) {
        for (int b = 0; b < count; ++b) {
            double least = hw_pair_least(&ink[a], &ink[b], 0, limit * limit);
            if (!(least < limit * limit) || !ink[a].n || !ink[b].n) continue;
            if (found == room) close = realloc(close, (room = room * 2 + 64) * sizeof(hw_pair_t));
            close[found++] = (hw_pair_t){hw_root(least), a, b};
        }
    }
    if (found) qsort(close, found, sizeof(hw_pair_t), hw_pair_cmp);
    *pairs = close;
    return found;
}

/*
 * The least distance between two parts of a symbol, "limit" if not less or if
 * the symbol is of one part. A part is the strokes that touch one another: a
 * dot over a letter is a part of its own, and a pen thicker than its distance
 * to the letter joins them.
 */
static double hw_parts_least(const hw_ink_t *ink, double limit)
{
    int     n = ink->elements;
    double *apart = malloc((size_t)n * n * sizeof(double)), least = limit * limit;
    int    *part = malloc(n * sizeof(int));
    for (int i = 0; i < n * n; ++i) apart[i] = limit * limit;
    for (int i = 0; i < n; ++i) part[i] = i;
    for (int i = 0; i < ink->n; ++i) {
        for (int j = i + 1; j < ink->n; ++j) {
            int a = ink->pieces[i].element, b = ink->pieces[j].element;
            if (a == b) continue;
            double d = hw_piece_piece(&ink->pieces[i], &ink->pieces[j]);
            if (d < apart[a * n + b]) apart[a * n + b] = apart[b * n + a] = d;
        }
    }
    /* the elements that touch get the number of one part, till nothing changes */
    for (int changed = 1; changed;) {
        changed = 0;
        for (int a = 0; a < n; ++a) {
            for (int b = 0; b < n; ++b) {
                if (part[b] > part[a] && apart[a * n + b] <= HW_PARTS_JOINED * HW_PARTS_JOINED) part[b] = part[a], changed = 1;
            }
        }
    }
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            if (part[a] != part[b] && apart[a * n + b] < least) least = apart[a * n + b];
        }
    }
    free(apart);
    free(part);
    return least;
}

/* Tells of the symbols whose own parts stand closer than "limit". */
static void hw_parts(const char *fontName, const hw_symbol_t *symbols, const hw_ink_t *ink, int count, double limit)
{
    hw_pair_t *close = calloc(count, sizeof(hw_pair_t));
    int        found = 0;
    for (int i = 0; i < count; ++i) {
        double least = hw_parts_least(&ink[i], limit);
        if (least < limit * limit) close[found++] = (hw_pair_t){hw_root(least), i, i};
    }
    if (found) {
        qsort(close, found, sizeof(hw_pair_t), hw_pair_cmp);
        int shown = hwPairLimit < 0 && found > HW_PAIRS_NAMED ? HW_PAIRS_NAMED : found;
        fprintf(stderr, "%s: %d symbol%s with parts of %s own closer than %g cells", fontName, found, found == 1 ? "" : "s",
                found == 1 ? "its" : "their", limit);
        fprintf(stderr, shown < found ? ", the closest %d:\n" : ":\n", shown);
        for (int i = 0; i < shown; ++i) {
            fprintf(stderr, i % 8 ? "   " : i ? "\n  " : "  ");
            hw_put_symbol(stderr, symbols[close[i].a].codePoint);
            fprintf(stderr, " %.1f", close[i].d);
        }
        fprintf(stderr, "\n  a part is moved in the editor; strokes within %d cells of one another are one part\n", HW_PARTS_JOINED);
    }
    free(close);
}

/* Tells of the pairs that stand too close and, when asked to, moves them apart in the font and in its file. */
static void hw_pairs(const char *fontFile, const char *fontName, const hw_symbol_t *symbols, int count, int symbolSpace)
{
    double limit = hwPairLimit < 0 ? symbolSpace : hwPairLimit;
    if (!(limit > 0)) return;
    double     apart = hwPairApart < limit ? limit : hwPairApart;
    hw_ink_t  *ink = calloc(count, sizeof(hw_ink_t));
    hw_pair_t *close = 0;
    int        found = hw_pairs_find(symbols, ink, count, symbolSpace, limit, &close);
    hw_parts(fontName, symbols, ink, count, limit);
    if (found) {
        int all = hwPairLimit >= 0 || hwPairFix, shown = !all && found > HW_PAIRS_NAMED ? HW_PAIRS_NAMED : found;
        fprintf(stderr, "%s: %d pair%s of symbols closer than %g cells between the lines of the pen", fontName, found,
                found == 1 ? "" : "s", limit);
        fprintf(stderr, shown < found ? ", the closest %d:\n" : ":\n", shown);
        for (int i = 0; i < shown; ++i) {
            fprintf(stderr, i % 8 ? "   " : i ? "\n  " : "  ");
            hw_put_symbol(stderr, symbols[close[i].a].codePoint);
            hw_put_symbol(stderr, symbols[close[i].b].codePoint);
            fprintf(stderr, " %.1f", close[i].d);
        }
        fprintf(stderr, "\n");
        int changed = hw_pairs_advice(symbols, ink, count, close, found, apart, symbolSpace, all ? found : HW_ADVICE_NAMED, hwPairFix);
        if (changed) {
            if (json_save(fontFile)) {
                fprintf(stderr, "Error writing font file %s: %s\n", fontFile, strerror(errno));
                exit(1);
            }
            free(close);
            found = hw_pairs_find(symbols, ink, count, symbolSpace, limit, &close);
            fprintf(stderr, "  %s: %d space%s changed, %d pair%s left closer than %g cells\n", fontFile, changed, changed == 1 ? "" : "s",
                    found, found == 1 ? " is" : "s are", limit);
        } else if (!hwPairFix) {
            /* other limits to choose from: those a pen half and three quarters as thick as the symbol space closes */
            fprintf(stderr, "  -g %g -x changes the file so, -i asks about each space; -t cells moves the pairs further apart", limit);
            if (hwPairLimit < 0) {
                fprintf(stderr, ";\n  closer than");
                for (int k = 2; k <= 3; ++k) {
                    int less = 0, cells = symbolSpace * k / 4;
                    while (less < found && close[less].d < cells) ++less;
                    fprintf(stderr, "%s %d cells: %d pair%s", k == 2 ? "" : ",", cells, less, less == 1 ? "" : "s");
                }
                fprintf(stderr, "; -g cells lists the pairs closer than that, -g 0 turns the check off");
            }
            fprintf(stderr, "\n");
        }
    }
    for (int i = 0; i < count; ++i) free(ink[i].pieces);
    free(ink);
    free(close);
}

/* a name that may be a function of C: letters, digits and _, not a digit first, not a keyword; `what` names the place in a message */
static int name_valid(const char *name, const char *what)
{
    size_t len = strlen(name);
    int    ok  = len > 0 && len <= 200 && !isdigit((unsigned char)name[0]);
    for (size_t i = 0; ok && i < len; ++i) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_') ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "%s %s: a name is letters, digits and _, not a digit first\n", what, name);
        return 0;
    }
    if (is_keyword(name)) {
        fprintf(stderr, "%s %s: a keyword of C or C++ cannot name a function\n", what, name);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------- */
/*  A handwritten font: its file loaded and checked, and written into a stream                                   */
/* ------------------------------------------------------------------------------------------------------------- */

/* the name a handwritten font is given: `given` as it is, or the font's own name made a name of C: "hw" before a
 * digit; "_font" after a word the written file uses itself or a keyword */
static void hw_function_name(const char *given, const char *jsonName, char out[256])
{
    snprintf(out, 254, "%s", given ? given : jsonName);
    if (!given) clearName(out);
    if (!out[0] || isdigit((unsigned char)out[0])) {
        memmove(out + 2, out, strlen(out) + 1);
        memcpy(out, "hw", 2);
    }
    static const char *const taken[] = {"hw", "elements", "symbols", "glyphs", "glyph_ranges", "rval", 0};
    for (int i = 0; taken[i]; ++i) {
        if (!strcmp(out, taken[i])) strcat(out, "_font");
    }
    if (is_keyword(out)) strcat(out, "_font");
}

/* the file of a handwritten font loaded and checked; its name and its code points given back */
static int hw_load(const char *fontFile, json_t **font, json_t **name, json_t **codePoints)
{
    *font           = json_load(fontFile);
    json_t *format  = json_get(*font, "format");
    json_t *version = json_get(*font, "formatVersion");
    *name           = json_get(*font, "name");
    *codePoints     = json_get(*font, "codePoints");
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
    if (!*name || (*name)->type != 's' || !(*name)->str[0] || !*codePoints || (*codePoints)->type != 'o') {
        fprintf(stderr, "%s: no \"name\" or \"codePoints\"\n", fontFile);
        return 1;
    }
    return 0;
}

static void cpr_free(cp_ranges_t **head)
{
    while (*head) {
        cp_ranges_t *next = (*head)->next;
        free(*head);
        *head = next;
    }
}

/* more functions that give the same font */
static void write_aliases(FILE *out, const char *funcname, const char *const *aliases, int nAliases)
{
    for (int i = 0; i < nAliases; ++i) {
        fprintf(out, "dgx_font_t *%s(void)\n{\n    return %s();\n}\n", aliases[i], funcname);
    }
}

/*
 * A handwritten font, loaded by hw_load(), written into `out` with the function `funcname` and its static
 * arrays named with `prefix` before them, the symbols of it that CPRanges allow. Returns their number, or a
 * negative number when nothing was written.
 */
static int hw_font_write(FILE *out, const char *fontFile, json_t *font, json_t *name, json_t *codePoints, const char *funcname, const char *prefix,
                         const char *const *aliases, int nAliases, int includeLine, int pairsCheck, int quiet)
{
    int symbolOffsetX = hw_number(font, "symbolOffsetX", 1, 0, 255);
    int symbolOffsetY = hw_number(font, "symbolOffsetY", 1, 0, 255);
    int symbolSizeX = hw_number(font, "symbolSizeX", 1, 1, 256);
    int symbolSizeY = hw_number(font, "symbolSizeY", 1, 1, 256);
    int baseLine = hw_number(font, "baseLine", 1, 0, 255);
    int xHeight = hw_number(font, "xHeight", 1, 0, 255);
    int symbolSpace = hw_number(font, "symbolSpace", 1, 0, 255);
    int spaceWidth = hw_number(font, "spaceWidth", 1, 0, 255);

    cpr_free(&SortedCharMap);
    int          count = 0;
    hw_symbol_t *symbols = calloc(json_count(codePoints) + 1, sizeof(hw_symbol_t));
    for (json_t *s = codePoints->child; s; s = s->next) {
        char         *rest = 0;
        unsigned long key = s->key && isdigit((unsigned char)s->key[0]) ? strtoul(s->key, &rest, 10) : 0;
        if (!rest || *rest || key > 0x10FFFF) {
            fprintf(stderr, "%s: \"%s\" in \"codePoints\" is not a code point\n", fontFile, s->key ? s->key : "");
            return -1;
        }
        uint32_t cp = (uint32_t)key;
        if (CPRanges && !isInRange(cp)) continue;
        symbols[count].codePoint = cp;
        symbols[count++].symbol = s;
        cpr_insert_output_cp(cp);
    }
    if (!count) {
        fprintf(stderr, "No symbols of %s are in the given ranges\n", fontFile);
        return -1;
    }
    qsort(symbols, count, sizeof(hw_symbol_t), hw_symbol_cmp);
    for (int i = 1; i < count; ++i) {
        if (symbols[i].codePoint == symbols[i - 1].codePoint) {
            fprintf(stderr, "%s: code point %u is in \"codePoints\" twice\n", fontFile, symbols[i].codePoint);
            return -1;
        }
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
    /* the counts of the elements first: a font with too many of them is told so before the long look at its pairs */
    for (int i = 0, total = 0; i < count; ++i) {
        for (int k = 0; k < 4; ++k) {
            json_t   *arr = json_get(symbols[i].symbol, hwParts[k]);
            const int n   = arr && arr->type == 'a' ? json_count(arr) : 0;
            total += n;
            if (n > 255 || total > 65535) {
                fprintf(stderr, "symbol %u (U+%04X): too many elements\n", symbols[i].codePoint, symbols[i].codePoint);
                return -1;
            }
        }
    }
    /* before anything is written: the pairs that stand too close may be moved apart in the font */
    if (pairsCheck) hw_pairs(fontFile, funcname, symbols, count, symbolSpace);

    if (includeLine) fprintf(out, "#include \"dgx_hw_font.h\"\n");
    fprintf(out, "/* %s: made by font2c from a file of format %s, version %d */\n", comment, HW_FORMAT_NAME, HW_FORMAT_VERSION);
    fprintf(out, "static const dgx_hw_element_t %selements[] = {\n", prefix);
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
                return -1;
            }
        }
    }
    fprintf(out, "};\n\n");

    typedef struct {
        int width, height, xAdvance, xOffset, yOffset;
    } hw_glyph_t;
    hw_glyph_t *gl = calloc(count, sizeof(hw_glyph_t));
    fprintf(out, "static const dgx_hw_symbol_t %ssymbols[] = {\n", prefix);
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
    fprintf(out, "static const glyph_t %sglyphs[] = {\n", prefix);
    for (int i = 0; i < count; ++i) {
        fprintf(out, "    {{.elements = %selements + %4d, .hw = %ssymbols + %3d}, %3d, %3d, %3d, %3d, %4d}, /* %04X */\n", prefix, first[i],
                prefix, i,
                gl[i].width, gl[i].height, gl[i].xAdvance, gl[i].xOffset, gl[i].yOffset, symbols[i].codePoint);
        if (yOffsetLowest > gl[i].yOffset) yOffsetLowest = gl[i].yOffset;
        if (yBottomMax < gl[i].yOffset + gl[i].height) yBottomMax = gl[i].yOffset + gl[i].height;
        if (xWidest < gl[i].width) xWidest = gl[i].width;
        if (xOffsetLowest > gl[i].xOffset) xOffsetLowest = gl[i].xOffset;
        if (xRightMax < gl[i].xOffset + gl[i].width) xRightMax = gl[i].xOffset + gl[i].width;
        xWidthAverage += gl[i].width;
    }
    fprintf(out, "};\n\n");

    fprintf(out, "static const glyph_array_t %sglyph_ranges[] = {\n", prefix);
    int numberOfRanges = 0, sidx = 0;
    for (cp_ranges_t *r = SortedCharMap; r; r = r->next, ++numberOfRanges) {
        fprintf(out, "    {0x%04x, %3d, %sglyphs + %3d},\n", r->first, r->number, prefix, sidx);
        sidx += r->number;
    }
    fprintf(out, "    {0, 0, 0},\n};\n\n");

    fprintf(out,
            "static const dgx_hw_font_t %shw = {\n"
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
            prefix, cname, HW_FORMAT_VERSION, baseLine, xHeight, symbolSpace, spaceWidth, symbolOffsetX, symbolOffsetY, symbolSizeX,
            symbolSizeY);
    fprintf(out,
            "dgx_font_t *%s()\n{\n"
            "    static dgx_font_t rval = {\n"
            "        .glyph_ranges = %sglyph_ranges,\n"
            "        .yAdvance = %d,\n"
            "        .yOffsetLowest = %d,\n"
            "        .xWidest = %d,\n"
            "        .xWidthAverage = %d,\n"
            "        .f_type = DGX_FONT_HW,\n"
            "        .yBottomMax = %d,\n"
            "        .xOffsetLowest = %d,\n"
            "        .xRightMax = %d,\n"
            "        .number_of_ranges = %d,\n"
            "        .hw = &%shw,\n"
            "    };\n"
            "    return &rval;\n}\n",
            funcname, prefix, symbolSizeY, yOffsetLowest, xWidest, (int)(xWidthAverage / count + 0.5), yBottomMax, xOffsetLowest, xRightMax,
            numberOfRanges, prefix);
    write_aliases(out, funcname, aliases, nAliases);
    if (!quiet) {
        printf("%s: %d symbols in %d ranges, %d elements, about %zu bytes on a 32-bit target\n", funcname, count, numberOfRanges, total,
               total * (size_t)14 + count * (size_t)(30 + 20) + (numberOfRanges + 1) * (size_t)12);
    }
    free(symbols), free(parts), free(first), free(gl);
    return count;
}

/* the header of a font written on its own: its one function */
static int write_single_header(const char *funcname, int hw)
{
    char fontname[260];
    snprintf(fontname, sizeof(fontname), "%s.h", funcname);
    /* written aside and put in place when complete, as the .c is */
    FILE *out = fopen(unfinished(fontname), "w");
    if (!out) {
        fprintf(stderr, "Error opening font header file %s for writing: %s\n", hwUnfinished, strerror(errno));
        unfinished(0);
        return 1;
    }
    fprintf(out, "#pragma once\n");
    fprintf(out, hw ? "#include \"dgx_hw_font.h\"\n" : "#include \"dgx_font.h\"\n");
    fprintf(out, "#ifdef __cplusplus\n// @formatter:off\nextern \"C\" {\n// @formatter:on\n#endif\n");
    fprintf(out, hw ? "dgx_font_t *%s();\n" : "dgx_font_t* %s();\n", funcname);
    fprintf(out, "#ifdef __cplusplus\n// @formatter:off\n}\n// @formatter:on\n#endif\n");
    if (ferror(out) | fclose(out) || rename(hwUnfinished, fontname)) {
        fprintf(stderr, "Error writing font header file %s: %s\n", fontname, strerror(errno));
        return 1;
    }
    unfinished(0);
    return 0;
}

/* a handwritten font on its own: fontfile.json [first last]... into <name>.c and <name>.h */
static int hw_font(const char *fontFile, char **ranges, int numRanges, const char *charsetFile)
{
    if (numRanges % 2) {
        fprintf(stderr, "the ranges of code points come in pairs, first and last: %s has no pair\n", ranges[numRanges - 1]);
        return 1;
    }
    for (int ri = 0; ri + 1 < numRanges; ri += 2) {
        cpr_insert_range(&CPRanges, strtol(ranges[ri], 0, 0), strtol(ranges[ri + 1], 0, 0));
    }
    if (charsetFile) loadCharsetFile(charsetFile);

    json_t *font, *name, *codePoints;
    if (hw_load(fontFile, &font, &name, &codePoints)) return 1;
    char funcname[256], fontname[260];
    hw_function_name(nameOverride, name->str, funcname);

    /* written aside and put in place when all of it is there: a font that fails halfway leaves the old file */
    snprintf(fontname, sizeof(fontname), "%s.c", funcname);
    FILE *out = fopen(unfinished(fontname), "w");
    if (!out) {
        fprintf(stderr, "Error opening font file %s for writing: %s\n", hwUnfinished, strerror(errno));
        unfinished(0);
        return 1;
    }
    if (hw_font_write(out, fontFile, font, name, codePoints, funcname, arrayPrefix, 0, 0, 1, 1, 0) < 0) return 1;
    if (fclose(out) || rename(hwUnfinished, fontname)) {
        fprintf(stderr, "Error writing font file %s: %s\n", fontname, strerror(errno));
        return 1;
    }
    unfinished(0);
    return write_single_header(funcname, 1);
}

/* ------------------------------------------------------------------------------------------------------------- */
/*  A bitmap font: a face of FreeType at a size, written into a stream                                            */
/* ------------------------------------------------------------------------------------------------------------- */

/* the name a bitmap font is given when none is told: the family, the style and the size, letters and digits only */
static void bitmap_generated_name(FT_Face face, int size, char out[256])
{
    snprintf(out, 250, "%s%s%d", face->family_name, face->style_name, size);
    clearName(out);
    /* a family like "3270" would begin a name of C with a digit */
    if (!out[0] || isdigit((unsigned char)out[0])) {
        memmove(out + 4, out, strlen(out) + 1);
        memcpy(out, "font", 4);
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/*  A patch of a font: the glyphs the editor fixed, taken over the rendered ones                                  */
/* ------------------------------------------------------------------------------------------------------------- */

/*
 * A patch file is what the font editor saves: the glyphs it changed, each with its rows of '#' and '.' and its
 * metrics. A glyph of the patch stands for the rendered one whole, its size taken from its rows, so a font made
 * again keeps the fixes: the editor does not write the .c, the patch is the source of the changes. Version 2 says
 * which font it fixes, "font" (the name font2c gives it: family, style and size) and "size" in pixels, and a patch
 * of another font is refused; version 1 says nothing of it and is taken as it is. Version 3 holds the fixes of
 * several fonts, as many as a file of -jo has: "fonts" is a list of parts, each with its "font", "size" and
 * "glyphs" as in version 2, and a font takes the part that names it. A lone part may name no font, as version 1.
 */
static json_t *PatchFont = 0;
static int     PatchUsed = 0; /* the glyphs of the patch taken into the font */
static char  **PatchDirs;     /* -P of the command line: where the patch of a font is looked for, after the -P of its line */
static int     nPatchDirs;

/* a glyph as it goes into the font: its ink, a byte a pixel (0 or 1), and its metrics in whole pixels */
typedef struct {
    int      width, height;
    int      xOffset, yOffset, xAdvance;
    uint8_t *ink;
} ink_glyph_t;

/* the parts of a patch, one after another: the patch itself up to version 2, the members of "fonts" in version 3 */
static json_t *patch_parts(json_t *patch)
{
    return (int)json_get(patch, "formatVersion")->num == 3 ? json_get(patch, "fonts")->child : patch;
}

/* a patch file read and checked, or null with the mistake told */
static json_t *patch_read(const char *path)
{
    json_t   *patch   = json_load(path);
    json_t   *format  = json_get(patch, "format");
    json_t   *version = json_get(patch, "formatVersion");
    json_t   *fonts   = json_get(patch, "fonts");
    json_t   *glyphs  = json_get(patch, "glyphs");
    const int v       = version && version->type == 'n' ? (int)version->num : 0;
    if (!format || format->type != 's' || strcmp(format->str, "dgx-font-patch") || v < 1 || v > 3 ||
        (v == 3 ? !fonts || fonts->type != 'a' : !glyphs || glyphs->type != 'o')) {
        return fprintf(stderr, "%s: not a patch of this program (\"format\": \"dgx-font-patch\", \"formatVersion\": 1 or 2 with \"glyphs\": {...}, "
                               "or 3 with \"fonts\": [...])\n", path), (json_t *)0;
    }
    int parts = 0, named = 0;
    for (json_t *p = patch_parts(patch); p; p = p->next, ++parts) {
        json_t *font = json_get(p, "font"), *px = json_get(p, "size");
        glyphs = json_get(p, "glyphs");
        if (p->type != 'o' || !glyphs || glyphs->type != 'o') return fprintf(stderr, "%s: a part of the patch has no \"glyphs\": {...}\n", path), (json_t *)0;
        if ((font && font->type != 's') || (px && px->type != 'n')) {
            return fprintf(stderr, "%s: the \"font\" of a part of the patch is not a name or its \"size\" not a number\n", path), (json_t *)0;
        }
        named += font && px;
        for (json_t *q = patch_parts(patch); font && q != p; q = q->next) {
            json_t *qfont = json_get(q, "font"), *qpx = json_get(q, "size");
            if (qfont && !strcmp(qfont->str, font->str) && (!px || !qpx || qpx->num == px->num)) {
                return fprintf(stderr, "%s: two parts of the patch are for the font %s\n", path, font->str), (json_t *)0;
            }
        }
        for (json_t *g = glyphs->child; g; g = g->next) {
            char *rest = 0;
            long  cp   = strtol(g->key, &rest, 16);
            if (!g->key[0] || *rest || cp < 0 || cp > 0x10FFFF) return fprintf(stderr, "%s: \"%s\" is not a code point in hex\n", path, g->key), (json_t *)0;
        }
    }
    if (parts > 1 && named < parts) return fprintf(stderr, "%s: a patch of several fonts names the font and the size of every part\n", path), (json_t *)0;
    return patch;
}

/*
 * The part of a patch for the font `generated` at `size`, or null: the part that names that font, or the lone part
 * that names none when the file is given for this font (`named`: by -p, or found by its name).
 */
static json_t *patch_part(json_t *patch, const char *generated, int size, int named)
{
    json_t *first = patch_parts(patch);
    for (json_t *p = first; p; p = p->next) {
        json_t *font = json_get(p, "font"), *px = json_get(p, "size");
        if (font && !strcmp(font->str, generated) && (!px || (int)px->num == size)) return p;
    }
    json_t *px = first ? json_get(first, "size") : 0;
    return named && first && !first->next && !json_get(first, "font") && (!px || (int)px->num == size) ? first : 0;
}

/* a patch given for a font that has no part for it, told: what it is for instead */
static int patch_refused(const char *path, json_t *patch, const char *generated, int size)
{
    json_t *first = patch_parts(patch);
    if (first && !first->next) {
        json_t *font = json_get(first, "font"), *px = json_get(first, "size");
        return fprintf(stderr, "%s: the patch is for the font %s at %d px, not for %s at %d px\n", path, font ? font->str : generated,
                       px ? (int)px->num : size, generated, size), 1;
    }
    fprintf(stderr, "%s: the patch has no part for the font %s at %d px; its fonts:", path, generated, size);
    for (json_t *p = first; p; p = p->next) fprintf(stderr, "%s %s at %d px", p == first ? "" : ",", json_get(p, "font")->str, (int)json_get(p, "size")->num);
    fprintf(stderr, first ? "\n" : " none\n");
    return 1;
}

static int patch_load(const char *path, const char *generated, int size)
{
    json_t *patch = patch_read(path);
    if (!patch) return 1;
    json_t *part = patch_part(patch, generated, size, 1);
    if (!part) return patch_refused(path, patch, generated, size);
    PatchFont = part;
    PatchUsed = 0;
    return 0;
}

/* the glyph of code point `cp` in the patch, or null */
static json_t *patch_glyph(uint32_t cp)
{
    if (!PatchFont) return 0;
    for (json_t *g = json_get(PatchFont, "glyphs")->child; g; g = g->next) {
        if (g->type == 'o' && strtol(g->key, 0, 16) == (long)cp) return g;
    }
    return 0;
}

/*
 * The glyph of the patch stands for the rendered one whole: its rows of '#' and '.' (anything else is a pixel off)
 * are its pixels, as many rows as it has and as wide as they are, and the metrics it has replace what FreeType
 * measured. Rows of different lengths are a mistake of the patch, told and refused. The measures of the whole font
 * (yOffsetLowest and the rest) are taken after, over the patched glyphs.
 */
static int patch_apply(ink_glyph_t *g, uint32_t cp, const char *where)
{
    json_t *p = patch_glyph(cp);
    if (!p) return 0;
    json_t *rows = json_get(p, "rows");
    if (rows) {
        if (rows->type != 'a') return fprintf(stderr, "%s: the rows of the patch of U+%04X are not a list\n", where, cp), 1;
        int height = 0, width = 0;
        for (json_t *r = rows->child; r; r = r->next, ++height) {
            if (r->type != 's') return fprintf(stderr, "%s: a row of the patch of U+%04X is not a string\n", where, cp), 1;
            int len = (int)strlen(r->str);
            if (height && len != width) return fprintf(stderr, "%s: the rows of the patch of U+%04X are not all as long\n", where, cp), 1;
            width = len;
        }
        if (!width) height = 0;
        free(g->ink);
        g->ink    = calloc((size_t)width * height + 1, 1);
        g->width  = width;
        g->height = height;
        int y     = 0;
        for (json_t *r = rows->child; r && width; r = r->next, ++y)
            for (int x = 0; x < width; ++x) g->ink[y * width + x] = r->str[x] == '#';
    }
    json_t *v;
    if ((v = json_get(p, "xAdvance")) && v->type == 'n') g->xAdvance = (int)v->num;
    if ((v = json_get(p, "xOffset")) && v->type == 'n') g->xOffset = (int)v->num;
    if ((v = json_get(p, "yOffset")) && v->type == 'n') g->yOffset = (int)v->num;
    ++PatchUsed;
    return 0;
}

/* the glyphs of the patch that did not go into the font: not in the face, which is told always, or not asked for */
static void patch_unused(FT_Face face, const char *where, int quiet)
{
    if (!PatchFont) return;
    for (json_t *g = json_get(PatchFont, "glyphs")->child; g; g = g->next) {
        long cp = strtol(g->key, 0, 16);
        if (!FT_Get_Char_Index(face, (FT_ULong)cp)) fprintf(stderr, "%s: U+%04lX of the patch is not in the face, the patch may be for another font\n", where, cp);
        else if (!quiet && CPRanges && !isInRange((int)cp)) fprintf(stderr, "%s: U+%04lX of the patch is not among the characters asked for\n", where, cp);
    }
}

/*
 * The patch of a font in the first of `dirs` that has one: a .patch.json there with a part for the font, or
 * <generated>.patch.json with a lone part that names no font (version 1). Two patches of one directory for the font
 * are a mistake, told, and so is <generated>.patch.json for another font. 0 with `*found` the path or null, 1 when
 * told.
 */
static int patch_find(const char *generated, int size, char *const *dirs, int nDirs, char **found)
{
    static const char suffix[] = ".patch.json";
    const size_t      sl       = sizeof(suffix) - 1;
    *found                     = 0;
    for (int i = 0; i < nDirs && !*found; ++i) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        for (struct dirent *e; (e = readdir(d));) {
            const size_t len = strlen(e->d_name);
            if (len <= sl || strcmp(e->d_name + len - sl, suffix)) continue;
            char *path = malloc(strlen(dirs[i]) + len + 2);
            sprintf(path, "%s/%s", dirs[i], e->d_name);
            const int named = len - sl == strlen(generated) && !strncmp(e->d_name, generated, len - sl);
            json_t   *patch = patch_read(path);
            json_t   *part  = patch ? patch_part(patch, generated, size, named) : 0;
            int       wrong = !patch || (named && !part && patch_refused(path, patch, generated, size));
            if (part && *found) {
                fprintf(stderr, "the font %s at %d px has fixes in %s and in %s; one font comes from one patch\n", generated, size, *found, path);
                wrong = 1;
            }
            if (wrong) {
                free(path);
                free(*found);
                *found = 0;
                closedir(d);
                return 1;
            }
            if (part) *found = path;
            else free(path);
        }
        closedir(d);
    }
    return 0;
}

/*
 * The glyph rendered in the slot as ink: bilevel at a small size, anti-aliased with hinting and cut to ink at
 * INK_THRESHOLD at a larger one. The bilevel rasterizer drops no pixel of a stem (its dropout control), which at
 * 16 px and less keeps the letters readable, while above that it fused the stems of small letters into blobs (the
 * arches of an 'm') and doubled crossbars; the hinted grayscale render keeps the stems FreeType snaps them to. The
 * grayscale bitmap has the box of the outline, which the cut leaves wider than its ink: it is cropped to the ink.
 * A face that has bitmaps of its own at the size gives those whatever the load asks for, as Terminus does up to
 * 32 px, at 1 bit a pixel: the bitmap is read by its pixel mode, not by the size. Returns -1 for a pixel mode that
 * is neither 1 bit nor 8 bits a pixel.
 */
static int ink_of_slot(FT_GlyphSlot slot, ink_glyph_t *g)
{
    const FT_Bitmap *b    = &slot->bitmap;
    const int        mono = b->pixel_mode == FT_PIXEL_MODE_MONO;
    if (!mono && b->pixel_mode != FT_PIXEL_MODE_GRAY) return -1;
    g->width           = (int)b->width;
    g->height          = (int)b->rows;
    g->xOffset         = slot->bitmap_left;
    g->yOffset         = 1 - slot->bitmap_top;
    g->xAdvance        = (int)(slot->advance.x >> 6);
    g->ink             = calloc((size_t)g->width * g->height + 1, 1);
    for (int m = 0; m < g->height; ++m) {
        const uint8_t *row = b->pitch >= 0 ? b->buffer + (size_t)m * b->pitch : b->buffer + (size_t)(g->height - 1 - m) * -b->pitch;
        for (int n = 0; n < g->width; ++n)
            g->ink[m * g->width + n] = mono ? (row[n >> 3] >> (7 - (n & 7))) & 1 : row[n] >= INK_THRESHOLD;
    }
    if (mono) return 0;
    int left = g->width, right = -1, top = g->height, bottom = -1;
    for (int m = 0; m < g->height; ++m)
        for (int n = 0; n < g->width; ++n)
            if (g->ink[m * g->width + n]) {
                if (n < left) left = n;
                if (n > right) right = n;
                if (m < top) top = m;
                if (m > bottom) bottom = m;
            }
    if (right < 0) {
        g->width = g->height = 0;
        return 0;
    }
    const int width = right - left + 1, height = bottom - top + 1;
    for (int m = 0; m < height; ++m) memmove(g->ink + m * width, g->ink + (m + top) * g->width + left, width);
    g->width = width, g->height = height;
    g->xOffset += left, g->yOffset += top;
    return 0;
}

/*
 * The glyphs of `fontFile` at `size` pixels that CPRanges allow (all of them when there are none), written into
 * `out` with the function `funcname` and its static arrays named with `prefix` before them. Returns the number of
 * glyphs, or a negative number when nothing was written.
 */
static int bitmap_font_write(FILE *fontOut, FT_Library library, const char *fontFile, int size, const char *funcname, const char *prefix,
                             const char *const *aliases, int nAliases, int includeLine, int quiet, const char *patchPath, char *const *patchDirs,
                             int nPatchDirs)
{
    int          i, j, err;
    int          bitmapOffset = 0;
    FT_Face      face;
    LoadedGlyph *glyphs;

    if ((err = FT_New_Face(library, fontFile, 0, &face))) {
        fprintf(stderr, "Font load error: %d", err);
        return -1;
    }
    glyphs = calloc(face->num_glyphs, sizeof(LoadedGlyph));
    err    = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (err) {
        fprintf(stderr, "FreeType FT_Select_Charmap error: %d", err);
        return -1;
    }
    /*
     // << 6 because '26dot6' fixed-point format
     FT_Set_Char_Size(face, size << 6, 0, DPI, 0);
     */
    /* A face of bitmaps alone, a BDF say, has the sizes of its bitmaps and no other: asked for another, FreeType keeps
     * the size it had, and the font would come out at that size under the name of this one. */
    if ((err = FT_Set_Pixel_Sizes(face, size, 0))) {
        if (!FT_IS_SCALABLE(face) && face->num_fixed_sizes > 0) {
            fprintf(stderr, "%s: a font of bitmaps, with no %d px; its size%s:", fontFile, size, face->num_fixed_sizes > 1 ? "s" : "");
            for (int k = 0; k < face->num_fixed_sizes; ++k) fprintf(stderr, " %d px", (int)((face->available_sizes[k].y_ppem + 32) >> 6));
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "%s: FreeType error %d setting the size of %d px\n", fontFile, err, size);
        }
        free(glyphs);
        FT_Done_Face(face);
        return -1;
    }

    /* the patch of the font: the one named, or the one of the first directory that has a part for it */
    char generated[256];
    bitmap_generated_name(face, size, generated);
    PatchFont   = 0;
    char *found = 0;
    if (!patchPath && patch_find(generated, size, patchDirs, nPatchDirs, &found)) return -1;
    if (patchPath || found) {
        if (patch_load(patchPath ? patchPath : found, generated, size)) return -1;
    }
    const int bilevel = size <= 16;

    FT_UInt gindex;

    cpr_free(&SortedCharMap);
    if (includeLine) fprintf(fontOut, "#include \"dgx_font.h\"\n");
    /* what the font is, for the font editor: the patch it saves is for this font */
    fprintf(fontOut, "/* font2c: %s, \"%s\" \"%s\" at %d px */\n", generated, face->family_name ? face->family_name : "",
            face->style_name ? face->style_name : "", size);
    fprintf(fontOut, "static const uint8_t %sbitmaps[] = {\n  ", prefix);
    int     fComma        = 0;
    int32_t yOffsetLowest = 0, yBottomMax = 0, xWidest = 0, xOffsetLowest = 0, xRightMax = 0;
    double  xWidthAverage = 0;
    // Process glyphs and output huge bitmap data array
    for (i = FT_Get_First_Char(face, &gindex), j = 0; gindex != 0; i = FT_Get_Next_Char(face, i, &gindex)) {
        if (CPRanges && !isInRange(i)) continue;
        if ((err = FT_Load_Char(face, i, bilevel ? FT_LOAD_TARGET_MONO : FT_LOAD_DEFAULT))) {
            fprintf(stderr, "Error %d loading char '%c'\n", err, i);
            continue;
        }
        if ((err = FT_Render_Glyph(face->glyph, bilevel ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL))) {
            fprintf(stderr, "Error %d rendering char '%c'\n", err, i);
            continue;
        }
        ink_glyph_t ink;
        if (ink_of_slot(face->glyph, &ink)) {
            fprintf(stderr, "U+%04X: the bitmap of the glyph has pixel mode %d, which is not read; the glyph is left out\n", (unsigned)i,
                    (int)face->glyph->bitmap.pixel_mode);
            continue;
        }
        /* the editor's patch of this glyph, over what was rendered */
        if (patch_apply(&ink, i, funcname)) return -1;
        if (j >= face->num_glyphs) {
            glyphs = realloc(glyphs, (j + 1) * sizeof(LoadedGlyph));
            if (!glyphs) {
                fprintf(stderr, "Exceeded allocated glyphs array size at char '%c'\n", i);
                break;
            }
        }
        glyphs[j].codePoint          = i;
        glyphs[j].ginfo.bitmapOffset = bitmapOffset;
        glyphs[j].ginfo.width        = ink.width;
        glyphs[j].ginfo.height       = ink.height;
        glyphs[j].ginfo.xAdvance     = ink.xAdvance;
        glyphs[j].ginfo.xOffset      = ink.xOffset;
        glyphs[j].ginfo.yOffset      = ink.yOffset;
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
        /* the ink packed into rows of whole bytes, the leftmost pixel in the high bit */
        const size_t bpl     = ((size_t)ink.width + 7) >> 3;
        size_t       ret_len = bpl * ink.height;
        uint8_t     *ret     = calloc(ret_len + 1, 1);
        for (int m = 0; m < ink.height; ++m)
            for (int n = 0; n < ink.width; ++n)
                if (ink.ink[m * ink.width + n]) ret[m * bpl + (n >> 3)] |= 0x80u >> (n & 7);
        for (int m = 0; m < ink.height; ++m) {
            fprintf(fontOut, "\n/* ");
            for (int n = 0; n < ink.width; ++n) fprintf(fontOut, "%c", ink.ink[m * ink.width + n] ? '#' : ' ');
            fprintf(fontOut, " */");
        }
        free(ink.ink);
        for (size_t m = 0; m < ret_len; ++m) {
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
        ++j;
    }
    if (PatchFont) {
        patch_unused(face, funcname, quiet);
        /* a line font2c's callers do not count as glyphs: it does not end in the word */
        printf("%s: patched by %s (%d of its glyphs)\n", funcname, patchPath ? patchPath : found, PatchUsed);
    }
    free(found);
    if (!j) {
        /* a .c with no glyph would not compile; the old file, if any, stays */
        fprintf(stderr, "%s at %d px: none of the characters asked for is in the font, nothing to write\n", fontFile, size);
        free(glyphs);
        FT_Done_Face(face);
        return -1;
    }
    if (!quiet) printf("Total width: %f, #glyphs: %d; avg = %f\n", xWidthAverage, j, xWidthAverage / j);
    xWidthAverage /= j;
    fprintf(fontOut, "};\n\n"); // End bitmap array
    if (!quiet) fprintf(stderr, "Total encoded length: %d\n", bitmapOffset);
    fprintf(fontOut, "static const glyph_t %sglyphs[] = {\n  ", prefix);
    int gidx = 0;
    for (cp_ranges_t *r = SortedCharMap; r; r = r->next) {
        r->gOffset = gidx;
        for (int cp = r->first; cp < r->first + r->number; ++cp) {
            if (gidx != 0) fprintf(fontOut, ", ");
            for (int gi = 0; gi < j; ++gi) {
                if (glyphs[gi].codePoint == cp) {
                    fprintf(fontOut, "  { {.bitmap = %sbitmaps + %5lu }, %3u, %3d, %3u, %4d, %4d } /* %04X */\n", prefix,
                            glyphs[gi].ginfo.bitmapOffset,
                            glyphs[gi].ginfo.width, glyphs[gi].ginfo.height, glyphs[gi].ginfo.xAdvance, glyphs[gi].ginfo.xOffset,
                            glyphs[gi].ginfo.yOffset, cp);
                    gidx++;
                    break;
                }
            }
        }
    }
    fprintf(fontOut, "};\n");
    fprintf(fontOut, "static const glyph_array_t %sglyph_ranges[] = {\n  ", prefix);
    int numberOfRanges = 0;
    for (cp_ranges_t *r = SortedCharMap; r; r = r->next, ++numberOfRanges) {
        if (r != SortedCharMap) fprintf(fontOut, ",");
        fprintf(fontOut, " {0x%-4x, 0x%-4x, %sglyphs + %d }\n", r->first, r->number, prefix, r->gOffset);
    }
    fprintf(fontOut, ", {0x%-4x, 0x%-4x, %d }\n", 0, 0, 0);
    fprintf(fontOut, "};\n");
    fprintf(fontOut, "// Bitmap size: %d\n", bitmapOffset);

    fprintf(fontOut,
            "dgx_font_t* %s() {\n\t"
            "static dgx_font_t rval = { \n\t\t"
            ".glyph_ranges = %sglyph_ranges,\n\t\t"
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
            funcname, prefix,                                                                                   //
            (face->size->metrics.height == 0 ? (long)glyphs[0].ginfo.height : face->size->metrics.height >> 6), //
            yOffsetLowest,                                                                                      //
            xWidest,                                                                                            //
            xWidthAverage,                                                                                      //
            yBottomMax,                                                                                         //
            xOffsetLowest,                                                                                      //
            xRightMax,                                                                                          //
            numberOfRanges                                                                                      //
    );
    write_aliases(fontOut, funcname, aliases, nAliases);
    free(glyphs);
    FT_Done_Face(face);
    return j;
}

/* a bitmap font on its own: fontfile size [first last]... into <name>.c and <name>.h */
static int bitmap_font(FT_Library library, const char *fontFile, int size, const char *patchFile)
{
    FT_Face face;
    int     err;
    char    fontname[256], funcname[256];
    if (nameOverride) {
        snprintf(funcname, 256, "%s", nameOverride);
    } else {
        if ((err = FT_New_Face(library, fontFile, 0, &face))) {
            fprintf(stderr, "Font load error: %d", err);
            return err;
        }
        bitmap_generated_name(face, size, funcname);
        FT_Done_Face(face);
    }
    strcpy(fontname, funcname);
    strncat(fontname, ".c", 255);
    /* written aside and put in place when all of it is there: a font that fails halfway leaves the old file */
    FILE *fontOut = fopen(unfinished(fontname), "w");
    if (!fontOut) {
        fprintf(stderr, "Error opening font file %s for writing: %s\n", hwUnfinished, strerror(errno));
        unfinished(0);
        return -1;
    }
    if (bitmap_font_write(fontOut, library, fontFile, size, funcname, arrayPrefix, 0, 0, 1, 0, patchFile, PatchDirs, nPatchDirs) < 0) return 1;
    if (ferror(fontOut) | fclose(fontOut) || rename(hwUnfinished, fontname)) {
        fprintf(stderr, "Error writing font file %s: %s\n", fontname, strerror(errno));
        return 1;
    }
    unfinished(0);
    return write_single_header(funcname, 0);
}

/* ------------------------------------------------------------------------------------------------------------- */
/*  Lists of fonts, -jo: all of them into one C file                                                              */
/* ------------------------------------------------------------------------------------------------------------- */

/*
 * A line of a list is what a run of this program takes for one font: [-n name] [-f letters.txt]... fontfile size
 * [first last]..., or a handwritten fontfile.json without a size; # begins a comment. A font asked by several lines,
 * of one face at one size or one handwritten file, is made once with every letter they ask, and every -n of them
 * is a function of it; a line without -n gives the function <output>_<FamilyStyleSize>. The arrays of each font are
 * named by the font, so the file holds them all without a clash. Paths are taken from the directory of the list.
 */
typedef struct {
    int          hw;
    char        *file;          /* the font file, the first that asked for this font */
    char        *real;          /* the same resolved, to tell one file from another */
    int          plain;         /* a line asked for it without -n: the function named after the output is one of its names */
    int          size;
    char         generated[256]; /* the family, the style and the size, or the name of a handwritten font */
    json_t      *json;          /* a handwritten font, loaded */
    json_t      *jsonName, *jsonCodePoints;
    cp_ranges_t *ranges;
    int          every;         /* a line asked for every glyph */
    char       **letters;       /* files of letters */
    int          nLetters;
    char       **names;         /* -n names; the first is the function */
    int          nNames;
    double       pairLimit;     /* -g of a handwritten line; below 0 no check */
    char        *patch;         /* -p: the editor's fixes of this font, taken over what is rendered */
    char       **patchDirs;     /* -P: where its patch is looked for by the name of the font, when no -p names one */
    int          nPatchDirs;
} jo_font_t;

static jo_font_t *joFonts;
static int        joCount;

/* the words of a line, "..." keeping its spaces; the line is cut into them in place */
static int jo_split(char *line, char **argv, int most)
{
    int argc = 0;
    for (char *p = line; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p || *p == '#') break;
        if (argc >= most) return -1;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') ++p;
            if (!*p) return -2;
            *p++ = 0;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
            if (*p) *p++ = 0;
        }
    }
    return argc;
}

/*
 * A path as Windows has it too: a backslash divides it as a slash does, and a path that begins with a drive, C:/ or C:\,
 * or with a backslash is not relative, as os.path.isabs() of Python takes it in dgx_fonts.py, which gives font2c such
 * paths.
 */
static int path_absolute(const char *path)
{
#ifdef _WIN32
    if (path[0] == '\\' || (isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '/' || path[2] == '\\'))) return 1;
#endif
    return path[0] == '/';
}

/* the name of the file of a path, after its directory */
static const char *path_name(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *back = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    return slash ? slash + 1 : path;
}

/* a path of a line as it is, or from the directory of the list when relative */
static char *jo_path(const char *listDir, const char *path)
{
    if (path_absolute(path) || !listDir[0]) return strdup(path);
    char *out = malloc(strlen(listDir) + strlen(path) + 2);
    sprintf(out, "%s/%s", listDir, path);
    return out;
}

static void jo_add_string(char ***list, int *count, char *s)
{
    *list            = realloc(*list, (*count + 1) * sizeof(char *));
    (*list)[(*count)++] = s;
}

static int jo_has_name(const jo_font_t *f, const char *name)
{
    for (int i = 0; i < f->nNames; ++i) {
        if (!strcmp(f->names[i], name)) return 1;
    }
    return 0;
}

/* one line of a list taken in: its font found among those asked before, or added */
static int jo_line(const char *listPath, int lineNo, const char *listDir, char **argv, int argc, FT_Library library)
{
    char  where[600];
    snprintf(where, sizeof(where), "%s:%d", listPath, lineNo);
    char       *names[64];
    int         nNames = 0;
    char       *letters[64];
    int         nLetters = 0;
    const char *file     = 0;
    const char *patch    = 0;
    char       *dirs[64];
    int         nDirs = 0;
    int         size = 0, hw = 0;
    double      pairLimit = -1;
    long        ranges[160];
    int         nRanges = 0;
    /* the options stand anywhere on the line, as on a command line; the font file, its size and the ranges in this order */
    for (int a = 0; a < argc; ++a) {
        if (!strcmp(argv[a], "-n")) {
            if (a + 1 >= argc || nNames >= 64) return fprintf(stderr, "%s: -n without a name\n", where), 1;
            if (!name_valid(argv[++a], where)) return 1;
            names[nNames++] = argv[a];
        } else if (!strcmp(argv[a], "-f")) {
            if (a + 1 >= argc || nLetters >= 64) return fprintf(stderr, "%s: -f without a file\n", where), 1;
            letters[nLetters++] = argv[++a];
        } else if (!strcmp(argv[a], "-p")) {
            if (a + 1 >= argc) return fprintf(stderr, "%s: -p without a patch file\n", where), 1;
            patch = argv[++a];
        } else if (!strcmp(argv[a], "-P")) {
            if (a + 1 >= argc || nDirs >= 64) return fprintf(stderr, "%s: -P without a directory\n", where), 1;
            dirs[nDirs++] = argv[++a];
        } else if (!strcmp(argv[a], "-t")) {
            return fprintf(stderr, "%s: -t is advice for -x, which changes the file of a font; a list does neither\n", where), 1;
        } else if (!strcmp(argv[a], "-g")) {
            char  *rest  = 0;
            double cells = a + 1 < argc ? strtod(argv[a + 1], &rest) : -1;
            if (!rest || rest == argv[a + 1] || *rest || !(cells >= 0 && cells <= 1000)) return fprintf(stderr, "%s: %s needs cells\n", where, argv[a]), 1;
            pairLimit = cells;
            ++a;
        } else if (!strcmp(argv[a], "-x") || !strcmp(argv[a], "-i")) {
            return fprintf(stderr, "%s: %s changes the file of a font, which a list does not do\n", where, argv[a]), 1;
        } else if (argv[a][0] == '-' && argv[a][1]) {
            return fprintf(stderr, "%s: unknown option %s\n", where, argv[a]), 1;
        } else if (!file) {
            file = argv[a];
            size_t len = strlen(file);
            hw         = len > 5 && !strcmp(file + len - 5, ".json");
            if (!hw) {
                char *rest = 0;
                size       = a + 1 < argc ? (int)strtol(argv[a + 1], &rest, 10) : 0;
                if (!rest || *rest || size < 1 || size > 1000) return fprintf(stderr, "%s: %s needs a size in pixels after it\n", where, file), 1;
                ++a;
            }
        } else {
            char *rest = 0;
            long  cp   = strtol(argv[a], &rest, 0);
            if (rest == argv[a] || *rest || cp < 0) return fprintf(stderr, "%s: a range of code points is two numbers, not '%s'\n", where, argv[a]), 1;
            ranges[nRanges++] = cp;
        }
    }
    if (!file) return fprintf(stderr, "%s: no font file\n", where), 1;
    if (hw && (patch || nDirs)) return fprintf(stderr, "%s: -p and -P patch bitmap fonts; a handwritten font is fixed in its editor\n", where), 1;
    if (nRanges % 2) return fprintf(stderr, "%s: the ranges of code points come in pairs, first and last: %ld has no pair\n", where, ranges[nRanges - 1]), 1;
    char *path = jo_path(listDir, file);
    char *real = realpath(path, 0);
    if (!real) real = strdup(path);
    /* the font of the line: by the name it would be given */
    char    generated[256];
    json_t *json = 0, *jsonName = 0, *jsonCodePoints = 0;
    if (hw) {
        if (hw_load(path, &json, &jsonName, &jsonCodePoints)) return 1;
        hw_function_name(0, jsonName->str, generated);
    } else {
        FT_Face face;
        int     err = FT_New_Face(library, path, 0, &face);
        if (err) return fprintf(stderr, "%s: %s: font load error %d\n", where, path, err), 1;
        bitmap_generated_name(face, size, generated);
        FT_Done_Face(face);
    }
    jo_font_t *f = 0;
    for (int i = 0; i < joCount; ++i) {
        if (joFonts[i].hw != hw || strcmp(joFonts[i].generated, generated)) continue;
        /* the same name from another file is another font, which the arrays named by the font could not hold */
        if (strcmp(joFonts[i].real, real)) {
            return fprintf(stderr, "%s: %s and %s both make the font %s; one font comes from one file%s\n", where, joFonts[i].file, path, generated,
                           hw ? " (give one of them another name in the editor)" : ""), 1;
        }
        f = &joFonts[i];
    }
    if (!f) {
        joFonts = realloc(joFonts, (joCount + 1) * sizeof(jo_font_t));
        f       = &joFonts[joCount++];
        memset(f, 0, sizeof(*f));
        f->hw = hw, f->file = path, f->real = real, f->size = size, f->json = json, f->jsonName = jsonName, f->jsonCodePoints = jsonCodePoints;
        f->pairLimit = -1;
        strcpy(f->generated, generated);
    }
    if (pairLimit >= 0) f->pairLimit = pairLimit;
    if (patch) {
        /* the line is read into a buffer the next line takes: the path is kept, from the directory of the list */
        char *given = jo_path(listDir, patch);
        if (f->patch && strcmp(f->patch, given)) {
            return fprintf(stderr, "%s: the font %s is patched by %s and by %s; one font comes from one patch\n", where, generated, f->patch, given), 1;
        }
        if (f->patch) free(given);
        else f->patch = given;
    }
    for (int i = 0; i < nDirs; ++i) jo_add_string(&f->patchDirs, &f->nPatchDirs, jo_path(listDir, dirs[i]));
    if (!nRanges && !nLetters) f->every = 1;
    if (!nNames) f->plain = 1;
    for (int r = 0; r + 1 < nRanges; r += 2) cpr_insert_range(&f->ranges, ranges[r], ranges[r + 1]);
    for (int i = 0; i < nLetters; ++i) jo_add_string(&f->letters, &f->nLetters, jo_path(listDir, letters[i]));
    for (int i = 0; i < nNames; ++i) {
        for (int k = 0; k < joCount; ++k) {
            if (&joFonts[k] != f && jo_has_name(&joFonts[k], names[i])) {
                return fprintf(stderr, "%s: the name %s is given to two fonts, %s and %s\n", where, names[i], joFonts[k].generated, generated), 1;
            }
        }
        if (!jo_has_name(f, names[i])) jo_add_string(&f->names, &f->nNames, strdup(names[i]));
    }
    return 0;
}

static int jo_read(const char *listPath, FT_Library library)
{
    FILE *in = fopen(listPath, "r");
    if (!in) return fprintf(stderr, "%s: %s\n", listPath, strerror(errno)), 1;
    /* the directory of the list, which the paths of its lines are taken from, however long */
    const char *name = path_name(listPath);
    char       *dir  = strdup(listPath);
    dir[name > listPath ? name - listPath - 1 : 0] = 0;
    char line[4096];
    for (int lineNo = 1; fgets(line, sizeof(line), in); ++lineNo) {
        char *argv[160];
        int   argc = jo_split(line, argv, 160);
        if (argc < 0) return fprintf(stderr, "%s:%d: %s\n", listPath, lineNo, argc == -2 ? "a quote is not closed" : "too many words"), 1;
        if (!argc) continue;
        if (jo_line(listPath, lineNo, dir, argv, argc, library)) return 1;
    }
    fclose(in);
    free(dir);
    return 0;
}

/* every font of the lists into <outName>.c, and their functions declared in <outName>.h */
static int jo_write(const char *outName, FT_Library library)
{
    if (!joCount) return fprintf(stderr, "the lists name no font\n"), 1;
    /* the functions of fonts without a name of their own begin with the name of the output, without its directory */
    const char *base = path_name(outName);
    int         hw   = 0;
    for (int i = 0; i < joCount; ++i) hw |= joFonts[i].hw;
    /* the output wherever it is, by its whole name */
    char *cname = malloc(strlen(outName) + 3), *hname = malloc(strlen(outName) + 3);
    sprintf(cname, "%s.c", outName);
    sprintf(hname, "%s.h", outName);
    FILE *out = fopen(unfinished(cname), "w");
    if (!out) {
        fprintf(stderr, "Error opening font file %s for writing: %s\n", hwUnfinished, strerror(errno));
        unfinished(0);
        return 1;
    }
    fprintf(out, "/* The fonts of the lists given to font2c -jo, every one once: made, not edited. */\n");
    fprintf(out, "#include \"dgx_font.h\"\n");
    if (hw) fprintf(out, "#include \"dgx_hw_font.h\"\n");
    for (int i = 0; i < joCount; ++i) {
        jo_font_t *f = &joFonts[i];
        char       funcname[600], prefix[300];
        /* a line without -n asked for the function named after the output, beside any -n names of the same font */
        if (f->plain && f->nNames) {
            char plain[600];
            snprintf(plain, sizeof(plain), "%s_%s", base, f->generated);
            if (!jo_has_name(f, plain)) jo_add_string(&f->names, &f->nNames, strdup(plain));
        }
        if (f->nNames) snprintf(funcname, sizeof(funcname), "%s", f->names[0]);
        else snprintf(funcname, sizeof(funcname), "%s_%s", base, f->generated);
        snprintf(prefix, sizeof(prefix), "%s_", f->generated);
        if (!f->nNames && !name_valid(funcname, "the output name")) return 1;
        cpr_free(&CPRanges);
        if (!f->every) {
            CPRanges = f->ranges;
            for (int k = 0; k < f->nLetters; ++k) loadCharsetFile(f->letters[k]);
            f->ranges = CPRanges;
        }
        const char *const *aliases = (const char *const *)(f->nNames ? f->names + 1 : 0);
        const int          nAliases = f->nNames ? f->nNames - 1 : 0;
        if (f->hw) fprintf(out, "\n/* %s: %s */\n", funcname, f->file);
        else fprintf(out, "\n/* %s: %s at %d px */\n", funcname, f->file, f->size);
        PatchFont = 0;
        int made;
        if (f->hw) {
            hwPairLimit = f->pairLimit;
            made        = hw_font_write(out, f->file, f->json, f->jsonName, f->jsonCodePoints, funcname, prefix, aliases, nAliases, 0, f->pairLimit >= 0, 1);
        } else {
            /* the directories of its lines first, then those of the command line */
            char **dirs = malloc((f->nPatchDirs + nPatchDirs + 1) * sizeof(char *));
            if (f->nPatchDirs) memcpy(dirs, f->patchDirs, f->nPatchDirs * sizeof(char *));
            if (nPatchDirs) memcpy(dirs + f->nPatchDirs, PatchDirs, nPatchDirs * sizeof(char *));
            made = bitmap_font_write(out, library, f->file, f->size, funcname, prefix, aliases, nAliases, 0, 1, f->patch, dirs, f->nPatchDirs + nPatchDirs);
            free(dirs);
        }
        if (made < 0) return 1;
        if (f->hw) printf("%s: %s, %d symbols\n", funcname, f->file, made);
        else printf("%s: %s at %d px, %d glyphs\n", funcname, f->file, f->size, made);
        for (int k = 0; k < nAliases; ++k) printf("%s: the same font\n", aliases[k]);
        CPRanges = 0;
    }
    if (ferror(out) | fclose(out) || rename(hwUnfinished, cname)) {
        fprintf(stderr, "Error writing font file %s: %s\n", cname, strerror(errno));
        return 1;
    }
    FILE *h = fopen(unfinished(hname), "w");
    if (!h) {
        fprintf(stderr, "Error opening font header file %s for writing: %s\n", hwUnfinished, strerror(errno));
        unfinished(0);
        return 1;
    }
    fprintf(h, "#pragma once\n/* The fonts of the lists given to font2c -jo: made, not edited. */\n#include \"dgx_font.h\"\n");
    if (hw) fprintf(h, "#include \"dgx_hw_font.h\"\n");
    fprintf(h, "#ifdef __cplusplus\n// @formatter:off\nextern \"C\" {\n// @formatter:on\n#endif\n");
    for (int i = 0; i < joCount; ++i) {
        jo_font_t *f = &joFonts[i];
        if (f->nNames) {
            for (int k = 0; k < f->nNames; ++k) fprintf(h, "dgx_font_t *%s(void);\n", f->names[k]);
        } else {
            fprintf(h, "dgx_font_t *%s_%s(void);\n", base, f->generated);
        }
    }
    fprintf(h, "#ifdef __cplusplus\n// @formatter:off\n}\n// @formatter:on\n#endif\n");
    if (ferror(h) | fclose(h) || rename(hwUnfinished, hname)) {
        fprintf(stderr, "Error writing font header file %s: %s\n", hname, strerror(errno));
        return 1;
    }
    unfinished(0);
    free(cname), free(hname);
    return 0;
}

int main(int argc, char *argv[])
{
    int         err;
    const char *charsetFile = 0, *outName = 0, *patchFile = 0;
    char      **lists = 0;
    int         nLists = 0, opt;
    static const struct option longOptions[] = {{"jo", required_argument, 0, 'J'}, {0, 0, 0, 0}};
    while ((opt = getopt_long_only(argc, argv, "f:g:t:xin:o:p:P:", longOptions, 0)) != -1) {
        switch (opt) {
        case 'f':
            charsetFile = optarg;
            break;
        case 'n':
            if (!name_option(optarg)) {
                show_usage(argv[0]);
                return 1;
            }
            break;
        case 'g':
        case 't': {
            char  *rest = 0;
            double cells = strtod(optarg, &rest);
            if (rest == optarg || *rest || !(cells >= 0 && cells <= 1000)) {
                show_usage(argv[0]);
                return 1;
            }
            *(opt == 'g' ? &hwPairLimit : &hwPairApart) = cells;
            break;
        }
        case 'x':
            hwPairFix = 1;
            break;
        case 'i':
            hwPairFix = 2;
            break;
        case 'J':
            jo_add_string(&lists, &nLists, optarg);
            break;
        case 'o':
            outName = optarg;
            break;
        case 'p':
            patchFile = optarg;
            break;
        case 'P':
            jo_add_string(&PatchDirs, &nPatchDirs, optarg);
            break;
        default:
            show_usage(argv[0]);
            return 1;
        }
    }
    atexit(hw_drop_unfinished);

    if (nLists) {
        /* the lists hold the options of each font; here only the output, and where patches are looked for */
        if (charsetFile || nameOverride || hwPairLimit >= 0 || hwPairApart >= 0 || hwPairFix || patchFile || optind < argc) {
            fprintf(stderr, "-jo takes the fonts and their options from the lists; with it only -o and -P go on the command line\n");
            return 1;
        }
        if (!outName) outName = "fonts";
        if (!name_valid(path_name(outName), "-o")) return 1;
        FT_Library library;
        if ((err = FT_Init_FreeType(&library))) {
            fprintf(stderr, "FreeType init error: %d", err);
            return err;
        }
        for (int i = 0; i < nLists; ++i) {
            if (jo_read(lists[i], library)) return 1;
        }
        const int rc = jo_write(outName, library);
        FT_Done_FreeType(library);
        return rc;
    }
    if (outName) {
        fprintf(stderr, "-o goes with -jo\n");
        return 1;
    }

    if (argc - optind >= 1) {
        size_t len = strlen(argv[optind]);
        if (len > 5 && !strcmp(argv[optind] + len - 5, ".json")) {
            if (patchFile || nPatchDirs) {
                fprintf(stderr, "-p and -P patch bitmap fonts; a handwritten font is fixed in its editor\n");
                return 1;
            }
            return hw_font(argv[optind], argv + optind + 1, argc - optind - 1, charsetFile);
        }
    }

    if (argc - optind < 2) {
        show_usage(argv[0]);
        return 1;
    }

    const char *fontFile = argv[optind];
    const int   size     = atoi(argv[optind + 1]);

    if ((argc - optind - 2) % 2) {
        fprintf(stderr, "the ranges of code points come in pairs, first and last: %s has no pair\n", argv[argc - 1]);
        return 1;
    }
    for (int ri = optind + 2; ri + 1 < argc; ri += 2) {
        int first = strtol(argv[ri], 0, 0);
        int last  = strtol(argv[ri + 1], 0, 0);
        cpr_insert_range(&CPRanges, first, last);
    }

    if (charsetFile) {
        loadCharsetFile(charsetFile);
    }

    // Init FreeType lib, load font
    FT_Library library;
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

    const int rc = bitmap_font(library, fontFile, size, patchFile);
    FT_Done_FreeType(library);
    return rc;
}
