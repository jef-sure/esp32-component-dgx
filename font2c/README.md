# font2c

`font2c` is an offline tool that converts fonts into C source/header pairs
for DGX. A program on ESP-IDF usually does not run it by hand: it declares
the fonts it needs in its CMakeLists, and `dgx_fonts.py` in this directory
runs `font2c` for all of them at build time; see
[docs/fonts-en.md](../docs/fonts-en.md). `font2c` handles two kinds of fonts:

- TTF/BDF-style fonts, rendered to bitmaps at a given pixel size. A size
  of 16 px and less is rendered bilevel: at that size the dropout control
  of the bilevel rasterizer keeps every stem, which matters more than
  smoothness. A larger size is rendered anti-aliased with hinting, cut to
  ink at 43% coverage and cropped to the ink: there the bilevel rasterizer
  fused the stems of small letters into blobs and doubled crossbars, while
  the hinted grayscale render keeps the stems FreeType snaps them to;
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
./font2c [-n name] [-f charset_file] [-p patch] [-P dir] <font file> <size> [<first> <last>]*
```

It writes a `.c` and a matching `.h` named after the font family, style and
size, or after `-n name`: the font's function, its files and its static
arrays (`name_bitmaps` and so on) then take that name, so that several fonts
made this way can share one C file. To use the generated font:

1. Move the `.c` file into `src/fonts/` and the `.h` file into
   `include/fonts/`.
2. Rerun CMake configure once. Fonts are added through `file(GLOB ...)`, so
   the build system has to notice the new file.
3. Include the generated header and pass its accessor to the text API, like
   a bundled font.

### Glyphs fixed by hand

The [font editor](../font-editor/README.md) saves the glyphs you fixed as a
patch, one for the whole `.c`, with a part for each of its fonts that has
fixes. A part names its font by the name `font2c` gives it from its family,
style and size (`DroidSansRegular9`), and its size; the font takes the part
that names it. `-p patch` takes a patch; `-P dir`, which may be repeated,
looks in the directory for a `.patch.json` with a part for the font, and the
first directory that has one is used. Every glyph of the patch replaces the
rendered glyph whole, with its own width, height, advance and offsets; the
measures of the whole font are taken after. A patch given for a font
(`-p`, or found as `<name>.patch.json`) without a part for it is refused, as
are two patches of one directory for the same font. A glyph whose rows
differ in length stops the run with the code point named, and a glyph of the
patch the face does not have is reported. The `.c` carries a comment saying
which font it is, so the editor can record that in the patch it saves.

The patch file, `"format": "dgx-font-patch"`, is version 3 with a list of
parts, `"fonts": [{"font": ..., "size": ..., "glyphs": {...}}, ...]`; a
patch of one font may leave out its `font` and `size`. A glyph has its
`rows` and may have `xAdvance`, `xOffset` and `yOffset`; its `rightGap`, the
gap the editor keeps on its right when it fits the metrics, is the editor's
alone and `font2c` passes it by. Versions 1 and 2, the patches of older
editors, are one font with its `glyphs` at the top and are read as before.

### Several fonts in one file

A list file holds the arguments of one `font2c` run per line, with paths
relative to the list and `#` for comments; a handwritten JSON goes in
without a size:

```
# fonts.list
-n widgets_title DejaVuSans.ttf 24 0x20 0x7e 0xc4 0xdc
-n widgets_note -f note.letters DejaVuSans.ttf 18
DejaVuSans.ttf 30 0x30 0x39
```

The options of a line may stand anywhere on it, as on a command line. `-jo`
takes such a list, as often as needed, and `-o` names the output:

```sh
./font2c -jo fonts.list -jo more.list -o my_fonts     # my_fonts.c and my_fonts.h
```

Every font of the lists goes into the one `.c`, and every function is
declared in the one `.h`. Two lines that ask for the same face at the same
size produce one font with the characters of both, and each `-n` name of
theirs becomes a function of it; a line without `-n` gets a name from the
family, style and size, prefixed with the output name
(`my_fonts_DejaVuSansBook30`). The static arrays are named after the font,
so nothing collides. `-x`, `-i`, `-n`, `-f`, `-g`, `-t` and `-p` belong to
the lines of a list, not to the command line of a `-jo` run. `-P` may go on
both: on the command line it applies to every font of the lists, after the
`-P` of the font's own lines.

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
