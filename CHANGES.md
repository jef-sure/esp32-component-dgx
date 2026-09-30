# Changes

## 0.1.0 - 2026-09-30

- `dgx_point_2d_t` fields are now `int16_t` (were `int`): halves point arrays
  (morph segments, glyph dots); code taking `int *` to `.x`/`.y` must adapt
- added font metrics `yBottomMax`, `xOffsetLowest` and `xRightMax` to
  `dgx_font_t` (bounding box of all glyphs); `font2c` emits them and all
  bundled fonts carry them
- added the dot morphing framework (`DGX_ENABLE_MORPH`): one bit-matrix to
  bit-matrix morph (`dgx_morph_create`) with pluggable source-selection
  callbacks (`dgx_morph_sources_life`, `dgx_morph_sources_cells`,
  `dgx_morph_ring_find`), glyph-to-matrix source, stateless per-frame
  `dgx_morph_draw` with head/tail trails, and glow / sprite renderers
  (optimized `collect_glow` core: dx-symmetric writes, incremental squared
  distance, preswapped grayscale LUT)
- added `examples/morph_demo`: CYD demo that morphs Russian words one letter
  at a time using independent per-letter morphs and glow renderers
- added `dgx_bw_bitmap_foreach_set()`: set-pixel scanner that skips whole zero
  bytes (8 pixels at once) for LINES-format bitmaps; used by glyph morphing

## 0.0.13 - 2026-06-22

- added sanity checks to utf-8 decoding

## 0.0.12 - 2026-06-19

- fixed backlight control to ILI9341 driver

## 0.0.11 - 2026-06-19

- added backlight control to ILI9341 driver

## 0.0.10 - 2026-06-15

- fixed MADCTL orientation command data length in ST7735, ST7789 and ILI9341 drivers

## 0.0.9 - 2026-06-14

- `font2c`: added `-f charset_file` (UTF-8 text input)

## 0.0.8 - 2026-06-14

- fixed OOB access in ILI9341/ST7789/ST7735/SSD1351 by embedding `dgx_screen_with_bus_t`
- thanks to **yuwgle** for pointing to the problem

## 0.0.7 - 2026-06-02

- added percent, celsius, dot, `R` and `H` to `CasusDotView` font

## 0.0.6 - 2026-06-01

- fixed `:` (colon) in `CasusDotView` font vertical alignment

## 0.0.5 - 2026-06-01

- added `:` (colon) to `CasusDotView` font

## 0.0.4 - 2026-05-30

- fix GPIO handling in SPI bus functions

## 0.0.3 - 2026-05-30

- added `dgx_gauge_redraw()` to redraw the whole gauge
- improved documentation

## 0.0.2 - 2026-05-13

- added `dgx_gc9a01_display_off()` and `dgx_gc9a01_display_on()` as public GC9A01 driver functions
