# DGX Handwritten Font Demo

Writes three lines by hand on a CYD with an ILI9341 display, in landscape:
«привет», «участникам», «соревнований», and then "Greetings", "to all the",
"contestants", in turn. The font is `font0-ss` of the
[hw-fonts](https://github.com/jef-sure/hw-fonts) editor: every symbol is a
pen path of Bezier curves, so the text can be drawn at any size, with a pen of
any thickness, and written stroke by stroke.

1. `dgx_hw_text_box()` measures each line. The size is the largest one at
   which the widest line fits the width of the screen and all the lines, a
   line height of the font apart, fit its height; the thickness of the pen
   follows the size. Each line stands in the middle.
2. `dgx_hw_writing_begin()` prepares a line and `dgx_hw_writing_draw_to()`
   writes it on by the clock: the pen goes through `EXAMPLE_TEMPO` units of
   effort a second, and only what is new is drawn. Letters are joined, the
   postponed strokes — the breve of «й» — are written when the pen leaves the
   paper.
3. The finished text stays for a few seconds, then the other one is fitted
   and written.

Things to change at the top of `main/main.c`: the texts themselves,
`EXAMPLE_JOINED` (joined writing or every letter apart), `EXAMPLE_TEMPO`,
`EXAMPLE_BEND_EFFORT` (how much the pen slows down in bends) and
`EXAMPLE_PEN_CELLS` (the thickness of the pen). The time each line took and
what the drawing itself cost go to the serial log.

The font is bundled with DGX as `fonts/font0ss.h`. It is made by `font2c`
from the editor's `font0-ss.json`; a font of your own is made the same way
and goes to `src/fonts/` and `include/fonts/`:

```sh
../../font2c/font2c path/to/your-font.json
```

The format of such a font and the rules of writing are described in
[docs/hw-font-ru.md](../../docs/hw-font-ru.md) (in Russian); writing itself in
[docs/handwriting-en.md](../../docs/handwriting-en.md).

CYD wiring:

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```
