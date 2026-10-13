# font2c

`font2c` is an offline tool that converts fonts into C source/header pairs
for DGX. It handles two kinds of fonts:

- TTF/BDF-style fonts, rendered to bitmaps at a given pixel size;
- handwritten fonts made of Bezier curves, saved as `.json` by the
  [hw-fonts](https://github.com/jef-sure/hw-fonts) editor.

## Build

FreeType development headers are required:

```sh
cc font2c/font2c.c -o font2c/font2c $(pkg-config --cflags --libs freetype2)
```

## Bitmap fonts

Give it a font file, a pixel size, and the characters to include: inclusive
Unicode ranges, a UTF-8 text file with the characters (`-f`), or both.

```sh
./font2c path/to/YourFont.ttf 16 0x20 0x7e 0x410 0x44f
./font2c -f charset.txt path/to/YourFont.ttf 16
./font2c [-f charset_file] <font file> <size> [<first> <last>]*
```

It writes a `.c` and a matching `.h` named after the font family, style and
size. To use the generated font:

1. Move the `.c` file into `src/fonts/` and the `.h` file into
   `include/fonts/`.
2. Rerun CMake configure once. Fonts are added through `file(GLOB ...)`, so
   the build system has to notice the new file.
3. Include the generated header and pass its accessor to the text API, like
   a bundled font.

[FontCreator](https://github.com/Llerr/FontCreator) is an alternative with a
GUI: it generates embedded C font data and lets you edit glyphs. Use its
exported `.c` and `.h` files the same way.

## Handwritten fonts

A handwritten font has no size argument: the size and the pen are chosen
when the text is drawn.

```sh
./font2c path/to/font0-ss.json
./font2c -g 20 path/to/font0-ss.json
./font2c -g 10 -t 16 -x path/to/font0-ss.json
```

The font format is described with the editor, in
[font-format-ru.md](https://github.com/jef-sure/hw-fonts/blob/main/font-format-ru.md).
The conversion and the tables it produces are described in
[docs/hw-font-ru.md](../docs/hw-font-ru.md). Both are in Russian. Such a
font is drawn with the functions of `dgx_hw_font.h`, not with the text API
for bitmap fonts; see [docs/handwriting-en.md](../docs/handwriting-en.md).

### The pair check

Before converting, `font2c` lists the pairs of symbols that stand too close
to each other in a line. The distance is measured in grid cells between the
centre lines of the pen, and the limit is the symbol space of the font. A
pen takes its own width off that distance, so such a pair can touch when
written.

For every pair it also says what to change in the editor's file to move the
symbols apart: which `spaceBefore` or `spaceAfter` of a symbol, and to what
value. In the same way it lists symbols whose own parts are too close, such
as a dot that a thick pen would join to its letter; those have to be moved
in the editor.

| Option | Meaning | Default |
| --- | --- | --- |
| `-g cells` | Pairs closer than this are too close. Every such pair and the full advice are listed. `-g 0` turns the check off. | the symbol space of the font; the 20 closest pairs are listed |
| `-t cells` | How far apart the advice moves such a pair, if further than `-g`. | the same as `-g` |
| `-x` | Write the advised spaces into the font file and convert the font with them. | the file is not changed |
| `-i` | The same as `-x`, but ask about each space: `[Y/n/q]`. | |

Only those numbers change in the file; the rest of its text stays as it is.
The advice covers all pairs at once and does not know which of them your
texts contain, so `-i` is often more useful than `-x`.
