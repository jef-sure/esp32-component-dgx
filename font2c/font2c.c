/*
 * MIT License
 *
 * Copyright (c) 2021 Anton Petrusevich
 *
 */

#include <ctype.h>
#include <errno.h>
#include <ft2build.h>
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

// Sorted array based character set for efficient lookup from file
typedef struct _char_set {
    uint32_t *codepoints;
    int       count;
    int       capacity;
} char_set_t;

char_set_t *CharSetFile = 0;

cp_ranges_t *CPRanges = 0;

int charset_contains(char_set_t *set, uint32_t cp);

int isInRange(int codePoint)
{
    // CharSetFile filter: character must be in the set (if loaded)
    if (CharSetFile && !charset_contains(CharSetFile, codePoint)) return 0;
    // Range filter: character must be in a range (if specified)
    if (CPRanges) {
        for (cp_ranges_t *r = CPRanges; r; r = r->next)
            if (codePoint >= r->first && codePoint < r->first + r->number) return 1;
        return 0;
    }
    // No filters or only CharSetFile: allow if CharSetFile passed above
    return 1;
}

cp_ranges_t *SortedCharMap = 0;

void cpr_insert_cp(int codePoint)
{
    cp_ranges_t *r;
    if (SortedCharMap == 0) {
        SortedCharMap         = malloc(sizeof(cp_ranges_t));
        SortedCharMap->first  = codePoint;
        SortedCharMap->number = 1;
        SortedCharMap->next   = 0;
        return;
    } else {
        if (codePoint < SortedCharMap->first) {
            if (codePoint == SortedCharMap->first - 1) {
                SortedCharMap->first--;
                SortedCharMap->number++;
                return;
            }
            cp_ranges_t *t        = SortedCharMap;
            SortedCharMap         = malloc(sizeof(cp_ranges_t));
            SortedCharMap->first  = codePoint;
            SortedCharMap->number = 1;
            SortedCharMap->next   = t;
            return;
        }
        for (r = SortedCharMap; r; r = r->next) {
            if (codePoint >= r->first && codePoint < r->first + r->number) return;
            if (codePoint == r->first + r->number) {
                r->number++;
                if (r->next && r->next->first - 1 == codePoint) {
                    cp_ranges_t *t = r->next;
                    r->number += t->number;
                    r->next = t->next;
                    free(t);
                }
                return;
            }
            if (!r->next || r->next->first > codePoint) {
                cp_ranges_t *t = malloc(sizeof(cp_ranges_t));
                t->first       = codePoint;
                t->number      = 1;
                t->next        = r->next;
                r->next        = t;
                return;
            }
        }
    }
}

char_set_t *charset_create() {
    char_set_t *set = malloc(sizeof(char_set_t));
    if (!set) return 0;
    set->count    = 0;
    set->capacity = 1024;
    set->codepoints = malloc(sizeof(uint32_t) * (size_t)set->capacity);
    if (!set->codepoints) {
        free(set);
        return 0;
    }
    return set;
}

void charset_add(char_set_t *set, uint32_t cp) {
    // binary search for insertion point
    int left = 0, right = set->count - 1;
    while (left <= right) {
        int mid = (left + right) / 2;
        if (set->codepoints[mid] == cp) return; // duplicate
        if (set->codepoints[mid] < cp) left = mid + 1;
        else right = mid - 1;
    }
    if (set->count >= set->capacity) {
        int new_capacity = set->capacity * 2;
        uint32_t *tmp = realloc(set->codepoints, sizeof(uint32_t) * (size_t)new_capacity);
        if (!tmp) return;
        set->codepoints = tmp;
        set->capacity = new_capacity;
    }
    memmove(&set->codepoints[left + 1], &set->codepoints[left], sizeof(uint32_t) * (set->count - left));
    set->codepoints[left] = cp;
    set->count++;
}

int charset_contains(char_set_t *set, uint32_t cp) {
    if (!set || set->count == 0) return 0;
    int left = 0, right = set->count - 1;
    while (left <= right) {
        int mid = (left + right) / 2;
        if (set->codepoints[mid] == cp) return 1;
        if (set->codepoints[mid] < cp) left = mid + 1;
        else right = mid - 1;
    }
    return 0;
}

void charset_free(char_set_t *set) {
    if (set) {
        free(set->codepoints);
        free(set);
    }
}

char_set_t *charset_load_from_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error opening charset file: %s\n", filename);
        return 0;
    }
    char_set_t *set = charset_create();
    if (!set) {
        fprintf(stderr, "Out of memory creating charset set\n");
        fclose(f);
        return 0;
    }
    // read whole file into buffer for simpler UTF-8 handling
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "Error seeking charset file: %s\n", filename);
        fclose(f);
        charset_free(set);
        return 0;
    }
    long fsize = ftell(f);
    if (fsize < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Error reading charset file size: %s\n", filename);
        fclose(f);
        charset_free(set);
        return 0;
    }
    unsigned char *buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fprintf(stderr, "Out of memory reading charset file: %s\n", filename);
        fclose(f);
        charset_free(set);
        return 0;
    }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "Error reading charset file: %s\n", filename);
        fclose(f);
        free(buf);
        charset_free(set);
        return 0;
    }
    buf[fsize] = 0;
    fclose(f);

    for (long i = 0; i < fsize;) {
        unsigned char c = buf[i];
        // skip comment lines starting with #
        if (c == '#') {
            while (i < fsize && buf[i] != '\n') i++;
            if (i < fsize) i++; // skip newline
            continue;
        }
        // skip whitespace
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            i++;
            continue;
        }
        // decode UTF-8
        uint32_t cp = 0;
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c >> 5) == 0x6) {
            if (i + 1 >= fsize) break;
            cp = ((uint32_t)(c & 0x1F) << 6) | (buf[i + 1] & 0x3F);
            i += 2;
        } else if ((c >> 4) == 0xE) {
            if (i + 2 >= fsize) break;
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(buf[i + 1] & 0x3F) << 6) | (buf[i + 2] & 0x3F);
            i += 3;
        } else if ((c >> 3) == 0x1E) {
            if (i + 3 >= fsize) break;
            cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(buf[i + 1] & 0x3F) << 12) | ((uint32_t)(buf[i + 2] & 0x3F) << 6) | (buf[i + 3] & 0x3F);
            i += 4;
        } else {
            i++;
            continue;
        }
        charset_add(set, cp);
    }
    free(buf);
    fprintf(stderr, "Loaded %d characters from %s\n", set->count, filename);
    return set;
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

    if (argc < 3) {
        fprintf(stderr, "Usage: %s fontfile size [-f charset_file] [first last] .. [firstN lastN]\n", argv[0]);
        fprintf(stderr, "  -f charset_file: UTF-8 text file with characters to include\n");
        fprintf(stderr, "  first last: Unicode range(s) to include (hex or decimal)\n");
        return 1;
    }

    size = atoi(argv[2]);

    for (int ri = 3; ri < argc; ri++) {
        if (strcmp(argv[ri], "-f") == 0) {
            if (ri + 1 >= argc) {
                fprintf(stderr, "Error: -f requires a filename argument\n");
                return 1;
            }
            CharSetFile = charset_load_from_file(argv[ri + 1]);
            if (!CharSetFile) return 1;
            ri++; // skip filename
        } else if (ri + 1 < argc) {
            int first = strtol(argv[ri], 0, 0);
            int last  = strtol(argv[ri + 1], 0, 0);
            if (!CPRanges) {
                CPRanges       = malloc(sizeof(cp_ranges_t));
                CPRanges->next = 0;
            } else {
                cp_ranges_t *r = malloc(sizeof(cp_ranges_t));
                r->next        = CPRanges;
                CPRanges       = r;
            }
            CPRanges->first  = first;
            CPRanges->number = last - first + 1;
            ri++; // skip second number in pair
        }
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

    if ((err = FT_New_Face(library, argv[1], 0, &face))) {
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
    int32_t yOffsetLowest = 0, xWidest = 0;
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
            xWidest       = glyphs[j].ginfo.width;
            xWidthAverage = glyphs[j].ginfo.width;
        } else {
            if (yOffsetLowest > glyphs[j].ginfo.yOffset) yOffsetLowest = glyphs[j].ginfo.yOffset;
            if (xWidest < glyphs[j].ginfo.width) xWidest = glyphs[j].ginfo.width;
            xWidthAverage += glyphs[j].ginfo.width;
        }
        cpr_insert_cp(i);
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
    for (cp_ranges_t *r = SortedCharMap; r; r = r->next) {
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
            ".f_type = DGX_FONT_BITMAP_LINES\n\t"
            "};\n\t"
            "return &rval;\n}\n",
            funcname,                                                                                           //
            (face->size->metrics.height == 0 ? (long)glyphs[0].ginfo.height : face->size->metrics.height >> 6), //
            yOffsetLowest,                                                                                      //
            xWidest,                                                                                            //
            xWidthAverage                                                                                       //
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
    if (CharSetFile) {
        charset_free(CharSetFile);
        CharSetFile = 0;
    }

    return 0;
}
