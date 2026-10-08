# DGX Handwritten Morph Demo

Writes a word by hand on a CYD with an ILI9341 display and then morphs it into
the next one, and that into the next, in a loop: «привет», «участникам»,
«соревнований», "Greetings", "to all the", "contestants". The font is
`font0-ss` of the
[hw-fonts](https://github.com/jef-sure/hw-fonts) editor, whose symbols are pen
paths of Bezier curves, so a text is a list of cubic curves and a morph is a
list of pairs of them: a frame is every pair with its points taken between the
two curves.

The two kinds of morph take turns:

1. A line as a whole, `dgx_hw_morph_create()`: the whole way of the pen of one
   word goes into the whole way of the pen of the other, whatever letters they
   have. The longest curves are cut in two until both words have the same
   number of them.
2. Letter by letter, `dgx_hw_morph_text_create()`: the letter at a position
   goes into the letter at the same position, each with a progress of its own,
   120 ms after the one before. Letters and strokes without a pair grow from
   the point where the pen of the other word ended, or go into it.

A frame is drawn anew every time, so it goes into an 8-bit virtual screen as
high as the tallest word and from there to the display.
`dgx_hw_morph_shift()` puts each word in the middle of the screen. The size is
the largest one at which the widest word fits the width. The number of
curves, the frame rate and the time a frame takes to draw go to the serial
log.

Things to change at the top of `main/main.c`: the words, the durations,
`EXAMPLE_JOINED` and the thickness of the pen.

The font is bundled with DGX as `fonts/font0ss.h`; see
[examples/hw_font_demo](../hw_font_demo), which writes a text without
morphing, for how such a font is made. Its format is described in
[docs/hw-font-ru.md](../../docs/hw-font-ru.md) (in Russian), both morphs in
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
