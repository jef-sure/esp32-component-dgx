# DGX Glyph Morph Demo

Shows one large symbol at a time on a CYD with an ILI9341 display and morphs it into the next one, in three sequences: digits, the weather symbols a clock shows, each by day and then by night so that alike symbols follow one another, then Latin letters with umlauts and Cyrillic. It is [cyd-dotview-morphing](https://github.com/jef-sure/cyd-dotview-morphing) rewritten on the DGX morphing framework:

1. `dgx_morph_glyph_matrix()` rasterizes a glyph of `TerminusTTFMedium12` or `WeatherIconsRegular27` into a matrix. All glyphs of a font share one box, so the matrices line up and one renderer serves every symbol of a sequence; a sequence comes from nothing, dots gathering from the center, and goes back into it before the next font takes the screen.
2. `dgx_morph_create_with(from, to, dgx_morph_sources_cells, NULL, &options)` plans the transition: common cells stay, a new cell takes a free neighboring cell as its source or, failing that, the nearest free one around it. What becomes of an old cell nobody took is set by `MORPH_ORPHANS` at the top of `main/main.c`: with `DGX_MORPH_ORPHANS_MERGE`, the default here, it flies into the nearest cell that stays, along its own figure first, and goes out there, so the bottom bar of "E" is swept into the stem of "F"; with `DGX_MORPH_ORPHANS_FADE` it fades where it is. `MORPH_MERGE_RADIUS` bounds the way, 0 is no limit. The mode goes to the serial log at start. The digits begin with E, F, 8, 3, 0, 1 to compare the two modes on.
3. `dgx_morph_draw()` and `dgx_morph_glow_present()` render each frame with glow and phosphor persistence.

A morph takes one second, then the symbol is held for another second. The cell size is the largest one that fits the screen together with the glow margin, 17 px for the letters and 5 px for the weather symbols; if the renderer does not fit the heap, the demo retries with smaller cells. FPS goes to the serial log: about 67 for the letters and 45 for the weather symbols on a CYD.

To try another font, add a sequence in `main/main.c` or change a font there; dot fonts and bitmap fonts both work. A symbol the font lacks becomes an empty matrix, so the dots gather in the center and spread out again.

For a whole line of text morphing letter by letter, see `examples/morph_demo`.

CYD wiring:

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```
