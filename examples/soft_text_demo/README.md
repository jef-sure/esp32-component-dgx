# Text with soft edges

A CYD (ESP32-2432S028R: an ESP32 with a 320x240 ILI9341 on SPI) shows the
same line in fonts of one bit and of 4 bits a pixel, on light paper and on
dark, a counter drawn again over itself ten times a second, and a line over
a picture. `main/CMakeLists.txt` asks for the fonts:

```cmake
dgx_fonts_board(PIXELS 320 240 MM 57.6 43.2)
dgx_font_need(soft SIZE 16px POSTSCRIPT DejaVuSans BITS 4 TEXTS strings.txt)
dgx_font_need(hard SIZE 16px POSTSCRIPT DejaVuSans TEXTS strings.txt)
dgx_font_need(small SIZE 12px POSTSCRIPT DejaVuSans BITS 4 TEXTS strings.txt)
dgx_font_need(small_hard SIZE 12px POSTSCRIPT DejaVuSans TEXTS strings.txt)
dgx_font_need(count SIZE 36px POSTSCRIPT DejaVuSans BITS 4 LETTERS "0123456789.")
```

The build prints what it made:

```
soft: DejaVu Sans Book 16 px (16 px), 4 bits a pixel, 31 glyphs
hard: DejaVu Sans Book 16 px (16 px), 31 glyphs
small: DejaVu Sans Book 12 px (12 px), 4 bits a pixel, 31 glyphs
small_hard: DejaVu Sans Book 12 px (12 px), 31 glyphs
count: DejaVu Sans Book 36 px (36 px), 4 bits a pixel, 11 glyphs
```

What is on the screen, from the top:

- a band of light paper with dark letters: the line in one bit at 16 px,
  then in 4 bits, then the same pair at 12 px;
- the same on dark paper with light letters;
- at the bottom left a counter in tenths of a second. It is drawn with
  `DGX_FONT_FILL`: its cell is filled with the paper each time, so the old
  digits go without an erase and without a flicker;
- at the bottom right a gradient in a virtual screen of 16 bits with a line
  over it. The line is drawn without a paper: the virtual screen is read, and
  the edges are mixed with the picture under them. The screen then goes to
  the display.

The display behind the bus cannot be read, so the lines on it are drawn with
`DGX_FONT_BG` and the color of their paper. How each kind of screen mixes
the edges: [Text with soft edges](../../README.md#text-with-soft-edges).

At start the program draws the line of 16 px twenty times each way and
writes the mean times to the log, before it draws the page:

```
TIME one bit                                ... us a line on the display
TIME 4 bits, cut as one bit (no paper)      ... us a line on the display
TIME 4 bits, DGX_FONT_BG                    ... us a line on the display
TIME 4 bits, DGX_FONT_FILL                  ... us a line on the display
TIME 4 bits, mixed with the picture         ... us a line on a 16-bit virtual screen
```

`EXAMPLE_BUILD` in `main/main.c`, when it is not 0, is shown in the top
right corner: set it in a copy that is flashed again and again, to tell the
pictures apart.

The build machine needs what any program with font needs needs: a C compiler
with FreeType and fontconfig, and the DejaVu fonts installed; see
[docs/fonts-en.md](../../docs/fonts-en.md).
