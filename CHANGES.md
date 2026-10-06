# Changes

## 0.3.0 - 2026-10-06

- added `dgx_draw_texture_quad()` and `dgx_draw_texture_rect()`: a region of a
  virtual screen stretched onto a convex quadrilateral or scaled into a
  rectangle (affine mapping, nearest texel, any screen as the target), and
  `examples/flip_clock_demo`, a split-flap clock built on them
- added `examples/life_morph_demo` (Conway's Game of Life with morphing
  generations) and `examples/glyph_morph_demo` (one large symbol morphing into
  the next): the cyd-life-morphing and cyd-dotview-morphing projects rewritten
  on the morphing framework
- faster drawing, measured on an ESP32 with an ILI9341 at 40 MHz:
  - SPI panels: the address window is set with polling transfers instead of
    queued, interrupt-driven ones; a single pixel, short lines, circle
    outlines and everything else made of small rectangles take about half the
    time (1000 pixels: 157 ms to 79 ms)
  - text: every run of set pixels in a glyph row is drawn as one rectangle
    (Verdana 32 on the panel: 108 ms to 13 ms; Terminus 12 into a 16-bit
    virtual screen: 0.79 ms to 0.32 ms)
  - text on page screens (SSD1306, ST7565R) sets the bits directly in the
    buffer, 7 times faster; `dgx_bw_blit_or()` and `dgx_bw_screen_is_paged()`
    are public
  - `dgx_vscreen_to_screen()` and `dgx_vscreen_region_to_screen()` copy 8-,
    16- and 24-bit rows into the transfer buffer with `memcpy` (210x231:
    28.0 ms to 20.6 ms, of which 19.4 ms is the transfer itself)
  - the glow renderer skips black pixels a word at a time
  - rectangles on 1-bit screens are filled a byte at a time: by page rows on
    page screens, with the new `dgx_fill_bits_msb()` on linear ones (100x40:
    119 us to 8 us and 260 us to 24 us)
  - with all of it `examples/life_morph_demo` went from 28 to 37 FPS and
    `examples/glyph_morph_demo` from 50 to 66 FPS
- black-and-white page screens (SSD1306, ST7565R): `set_pixel`, `get_pixel`
  and `dgx_bw_fast_vline()` check bounds; `fill_rectangle` clips a rectangle
  that starts above the top edge instead of dropping it; `dgx_bw_init()` sets
  `screen_subtype` before installing the default functions (they read past the
  allocation otherwise); `write_area` wraps inside the area when given more
  data than it holds
- added `dgx_vscreen_is_linear()`. The packed glyph blit, `dgx_vscreen_copy()`,
  `dgx_vscreen_clone()` and `dgx_morph_sprite_set_target()` use it, so page
  screens are no longer accessed as a linear `dgx_vscreen_t`; text on them was
  garbage and could write outside the buffer. The glyph blit is also skipped on
  1-bit virtual screens whose width is not a multiple of 8
- `decodeUTF8next()` no longer steps over the terminator (or swallows the next
  character) on a truncated UTF-8 sequence
- fixed 4-bit pixel reads always returning 0; 12-bit virtual screens addressed
  2 bytes per pixel in a 1.5 bytes per pixel buffer, and writing an even pixel
  cleared part of its right neighbour
- `dgx_vscreen_region_to_screen()` and its oriented variant keep the picture in
  place when the region is cropped by the source or destination edge
- `dgx_font_make_morph_struct()` keeps the symbol when the target code point
  has no glyph; temporary dot arrays are freed on the early-return paths
- SSD1351: 18-bit mode set and then cleared the color depth bit (`&= 0x40`)
- SPI bus: a failed `spi_device_queue_trans()` in `write_command`/`write_data`
  no longer waits forever for a result that was never queued; `read_data` and
  `dispose` wait for pending transactions first; `dgx_spi_init()` frees the
  transfer buffer when `spi_bus_add_device()` fails
- I2C bus: `dgx_i2c_init()` returns NULL when the master bus cannot be created
  (it went on with an uninitialized handle); `dispose` logs errors instead of
  aborting, so the memory is still released
- ST7565R and ST7920 no longer switch their log tag to DEBUG at the end of
  init (leftover debugging; every screen update was logged)
- `dgx_vscreen_2h_init()` no longer leaks the temporary screen's buffer when
  the allocation of the composite screen fails

## 0.2.0 - 2026-10-01

- screens accumulate a pending dirty area (`dirty_left/top/right/bottom` in
  `dgx_screen_t`): primitives report changes with the new `dgx_screen_touch()`,
  and the outermost `dgx_screen_progress_down()` commits the bounding box of
  everything drawn in the batch with one `update_screen()` call. Previously
  only the outer operation's own rectangle was committed, so changes made by
  other calls inside a batch were lost on staged screens (ST7920, SSD1306,
  ST7565R, `vscreen_2h`)
- added `dgx_screen_flush()`; `dgx_screen_progress_down()` now flushes at
  depth 0, so code that called `update_screen()` after it can drop that call;
  do not modify `in_progress` directly any more; an unbalanced extra
  `progress_down()` resets the counter to 0 instead of deferring forever
- `dgx_vscreen_copy()` now marks the destination dirty
- fixed `r == 0` in vscreen circles drawing at (0, 0) instead of the center

## 0.1.2 - 2026-10-01

- fixed the full-screen black fill fast path in `vscreen` (compared `y` with
  the height instead of `h`, so `memset` never ran)
- fixed `examples/screen_demo`: `dgx_ili9341_init()` call lacked the backlight
  argument added in 0.0.11; README no longer promises menuconfig pin options
- `DGX_ENABLE_MORPH` now selects `DGX_ENABLE_VSCREEN` (the renderers need it)
- component requirements: `esp_driver_gpio`, `esp_driver_spi`,
  `esp_driver_i2c` on IDF 5.3+, the legacy `driver` component before that;
  minimum IDF raised to 5.2 (the I2C backend uses `driver/i2c_master.h`)
- `dgx_morph_create` returns NULL for sides above `INT16_MAX` cells;
  `dgx_morph_draw` skips dots whose pixel position does not fit `int16_t`
- added matrix helpers `dgx_matrix_clear`, `dgx_matrix_clone`,
  `dgx_matrix_copy`, `dgx_matrix_equals` and `dgx_matrix_from_bw_bitmap`
- added `dgx_morph_text_create` / `dgx_morph_text_destroy` /
  `dgx_morph_text_duration_us`: per-letter morph between two UTF-8 strings;
  `examples/morph_demo` uses it
- documented that `get_pixel` reads only virtual screens and panels with a
  virtual back screen
- marked the commented-out alternative ILI9341 init sequence as taken from
  TFT_eSPI and fixed its command labels
- added host tests (`make -C test/host`, ASan/UBSan)
- repaired the end of README (Known gaps, Repository layout)

## 0.1.1 - 2026-10-01

- corrected the glow falloff documentation to describe its squared-distance LUT
- enforce a minimum glow radius of 2 pixels so small cell sizes retain a soft dot

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
