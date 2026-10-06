# DGX Glyph Morph Demo

Shows one large symbol at a time on a CYD with an ILI9341 display and morphs it into the next one: digits, Latin letters with umlauts, then Cyrillic. It is [cyd-dotview-morphing](https://github.com/jef-sure/cyd-dotview-morphing) rewritten on the DGX morphing framework:

1. `dgx_morph_glyph_matrix()` rasterizes a glyph of `TerminusTTFMedium12` into a matrix. All glyphs of a font share one box, so the matrices line up and one renderer serves every symbol.
2. `dgx_morph_create(from, to, dgx_morph_sources_cells, NULL)` plans the transition: common cells stay, a new cell takes a free neighboring cell as its source or, failing that, the nearest free one around it, and unused old cells fade.
3. `dgx_morph_draw()` and `dgx_morph_glow_present()` render each frame with glow and phosphor persistence.

A morph takes one second, then the symbol is held for another second. The cell size is the largest one that fits the screen together with the glow margin; if the renderer does not fit the heap, the demo retries with smaller cells. FPS goes to the serial log.

To try another font, change the `TerminusTTFMedium12()` call in `main/main.c`; dot fonts and bitmap fonts both work. A symbol the font lacks becomes an empty matrix, so the dots gather in the center and spread out again.

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
