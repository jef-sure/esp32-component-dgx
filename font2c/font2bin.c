/*
 * font2bin.c: the .dgxf file of a font of C, compiled with it.
 *
 * Copyright (c) 2026 Anton Petrusevich. Licensed under the MIT License, as DGX.
 *
 * For a font font2c did not make now: a font of DGX as it is, a font of C of a program, one written by hand. It is
 * built on the computer with the font and DGX's headers, the name of the font's constant given as FONT:
 *
 *     cc -DFONT=TerminusTTFMedium12 -I<dgx>/include -I<dgx>/host/stubs -include <dgx>/host/stubs/esp_log.h \
 *        font2bin.c <dgx>/src/fonts/TerminusTTFMedium12.c -o font2bin
 *     ./font2bin TerminusTTFMedium12.dgxf [about.txt]
 *
 * The file is written by dgxf_write.h, as font2c writes it: for a font font2c made, the same bytes but the text
 * about the font, which is the second argument here.
 */
#include "dgxf_font.h"

#ifndef FONT
#error "the name of the constant of the font, -DFONT=Name"
#endif

extern const dgx_font_t FONT;

#define QUOTE_(x) #x
#define QUOTE(x)  QUOTE_(x)

static char *read_all(const char *path)
{
    FILE *in = fopen(path, "rb");
    if (!in) return NULL;
    fseek(in, 0, SEEK_END);
    const long n = ftell(in);
    fseek(in, 0, SEEK_SET);
    char *text = n >= 0 ? calloc((size_t)n + 1, 1) : NULL;
    if (text && fread(text, 1, (size_t)n, in) != (size_t)n) free(text), text = NULL;
    fclose(in);
    return text;
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s out.dgxf [about.txt]: the file of the font %s\n", argv[0], QUOTE(FONT));
        return 2;
    }
    char *about = NULL;
    if (argc == 3 && !(about = read_all(argv[2]))) {
        fprintf(stderr, "%s: %s cannot be read\n", argv[0], argv[2]);
        return 1;
    }
    char  why[160];
    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "%s: %s cannot be written\n", argv[0], argv[1]);
        return 1;
    }
    const int failed = dgxf_of_font(&FONT, about, out, why, sizeof why);
    if (fclose(out) || failed) {
        fprintf(stderr, "%s: %s\n", QUOTE(FONT), failed ? why : "the file cannot be written");
        remove(argv[1]);
        return 1;
    }
    free(about);
    return 0;
}
