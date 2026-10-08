# Changes

## 0.4.4 - 2026-10-09

- added `dgx_morph_sources_cells_within()`: the search by vectors kept
  within a radius given through `user_data`. A new cell with no unused cell
  within it appears from the center at once and no dot flies across the
  picture; the rings beyond the radius are not gone through, and the planner
  does this search on words of bits as well. On an ESP32 a plan between any
  two weather symbols within 5 cells takes 1.4 ms on average and 2.5 ms at
  most, against 2.6 and 7.8 without a limit
- an 18-bit virtual screen counted 18 bits for a pixel when rows were
  written to it or read from it, while a pixel takes three bytes in every
  buffer. A textured quad or a copy drawn into such a screen put a third more
  pixels than it was given, wrong ones, past the end of the row and of the
  screen
- the morphing guides give the time of a plan on five chips: ESP32, S3, C3,
  C6 and P4, and what the same search costs when it is asked through a
  callback

## 0.4.3 - 2026-10-08

- thick Bezier curves: a pen wider than 46340 overflowed an `int` and went
  through every row of its disc, on the screen or not, which took tens of
  seconds. The width is kept within 32767 and only the rows of the screen are
  gone through
- `dgx_font_find_glyph()` wrote the advance through a NULL pointer when it
  was not asked for
- `dgx_draw_line_mask()`: bits of the mask above `mask_bits` came into the
  pattern as it turned
- handwritten text written by a pen: a line and a dot are one piece at any
  size; above size 1 a line was cut into several, which showed along its edge
- morphing of handwritten text returns NULL for a text of more than 65535
  symbols instead of a wrong plan
- `dgx_morph_ring_at()` and `dgx_morph_ring_find()` read beyond the grid
  when asked about a cell that is not on it; `dgx_morph_ring_find()` now
  reaches the grid from such a cell. The number of passes a callback may
  defer overflowed for a grid wider than 23170
- morphing of handwritten text letter by letter: a symbol of one stroke may
  be taken from its other end as well; NULL, as promised, when memory ends
  while the strokes are paired. `dgx_hw_morph_draw_xy()` takes a size that is
  not above nothing as nothing, as writing a text does
- `examples/texture_demo` frees what it has taken when memory ends at the
  start
- `font2c`, handwritten fonts: the keys of `codePoints` must be code points
  and may not repeat, since a table out of order breaks the search by
  halving; the `.c` file is written aside and put in place when it is whole,
  so a font that fails halfway leaves the old file; a font whose name gives
  `hw`, `glyphs` or another name the written file uses itself gets `_font`
  added; `*/` in a name no longer closes the comment; a short `\u`, a file
  nested too deep and a number that is not one are refused

## 0.4.2 - 2026-10-08

- `dgx_morph_sources_cells()` searches vector by vector instead of cell by
  cell. A pass is one vector, the same for the whole grid: every new cell
  still without a source looks at the one cell that lies by that vector from
  it. The vectors go ring by ring, on a ring from the axes toward the corners.
  A line that has moved aside is found as a whole by its vector, and the
  order of the cells decides nothing: no cell can take a source from another
  on a pass. Before, each cell searched all the rings in its turn and could
  take a source four cells away which the next cell had three cells away. On
  the weather symbols the flights are 17% shorter on average
- the planner does that search on rows of bits, skips the rings on which no
  waiting cell has a source, and lets the rest appear at once when no source
  is left. Both matrices are read once into words of bits, and a vector that
  cannot lead from the box of the waiting cells into the box of the unused
  sources is passed over. A plan between two weather symbols which follow one
  another in real weather takes 1.6 ms on an ESP32 on average and under 5 ms
  at most; between any two of them, 2.6 ms and 8 ms. A callback may defer a
  cell as many times as there are vectors to every cell of the grid, not 8
- added `dgx_morph_scan_vector()`, the vector of a pass, and
  `dgx_morph_ring_at()`, the first unused cell on the ring of one radius
- `dgx_morph_glow_set_filter()`: an extra filter of every frame of the glow
  renderer right before it goes to the screen. It works on a copy of the
  finished frame as a brightness map; nothing it does gets into the phosphor
  or the following frames. `dgx_morph_glow_blur()` is a ready one: glyphs of a
  font turned into dots keep the steps of its one-bit picture, a pass or two
  of blur smooths them

## 0.4.1 - 2026-10-08

- `dgx_hw_morph_text_create()`: the strokes of two symbols are paired by the
  shortest way of their points, not in the order of writing, and a stroke may
  be taken from its other end. Symbols are drawn as a hand writes them, which
  is not the order in which their parts answer one another: with the digits
  of `font0ss` a curve flew across the whole digit in a third of the
  transitions. The points now go 30% less on average
- handwritten text with a size of its own along each axis:
  `dgx_hw_draw_text_xy()`, `dgx_hw_writing_begin_xy()` and
  `dgx_hw_morph_draw_xy()` take `scale_x, scale_y` in place of `scale`. The
  pen stays round; the functions with one scale work as before
- added `examples/texture_demo`: a textured card scaled, turned and wobbled on
  a CYD
- README: a tutorial on stretching a texture onto a quad, with the corners for
  scaling, rotation and wobbling

## 0.4.0 - 2026-10-08

- added `dgx_draw_bezier3()` and `dgx_draw_bezier4()`: thick quadratic and
  cubic Bezier curves with round ends. A curve is drawn by the given number
  of straight pieces, or by as many as keep it within half a pixel when none
  is given; a piece has a flat start and a round end that closes the joint
  with the next one
- added `dgx_bezier3_begin()`, `dgx_bezier4_begin()`, `dgx_bezier_draw_to()`
  and `dgx_bezier_length()`: the same curves drawn part by part, to animate
  writing. A curve is drawn on until it is `t` complete, as in morphing; `t`
  goes by the way of the pen, so the pen moves evenly however the curve is
  split into pieces, and a piece is drawn as far as the pen has got
- handwritten fonts, the data part: a font of type `DGX_FONT_HW` keeps every
  symbol as a pen path of Bezier curves. Its glyphs are ordinary `glyph_t`
  found with `dgx_font_find_glyph()`; what a glyph has no place for is in the
  tables of `dgx_hw_font.h`, which repeat the file of the hw-fonts editor.
  Such a font is not drawn by `dgx_font_char_to_screen()` and the like but
  by its own functions. See `docs/hw-font-ru.md`
- `dgx_hw_draw_text()` writes a text of a handwritten font at any size with a
  pen of any thickness, joined or not: two letters are joined by one curve
  made of their connections, postponed strokes are written when the pen
  leaves the paper
- `dgx_hw_writing_begin()` and `dgx_hw_writing_draw_to()` write it by the pace
  of a pen: up to the effort the pen has gone through, adding only what is
  new. `dgx_hw_text_effort()` gives the whole effort of a text, `dgx_hw_pace()`
  the pace of a font with the least effort taken from its hyphen
- `dgx_hw_writer_begin()` and `dgx_hw_writer_next()` give the strokes of a
  text one by one in the order of writing
- `dgx_hw_text_box()` gives the box a text takes, to choose its size and place
- morphing of handwritten texts: every stroke is a cubic curve, a morph is a
  list of pairs of curves and a frame is every pair taken between its two
  curves. `dgx_hw_morph_create()` plans a line into a line as a whole (the
  whole way of the pen into the whole way of the pen, the longest curves cut
  in two until the numbers are equal), `dgx_hw_morph_text_create()` letter by
  letter (strokes in order, what has no pair grows from the point where the
  other pen ended); `dgx_hw_morph_draw()` draws a frame, `dgx_hw_morph_shift()`
  moves the two texts, `dgx_hw_morph_box()` gives the box no frame leaves
- added `docs/handwriting-en.md` and `docs/handwriting-ru.md`: handwritten
  text in detail, from a text at once to writing at the pace of a pen and
  both morphs
- added `examples/hw_morph_demo`: handwritten words morphing one into another
  on a CYD, as a whole and letter by letter in turn
- added `examples/hw_font_demo`: three lines written by hand on a CYD at the
  largest size that fits the screen
- `dgx_hw_element_effort()` and `dgx_hw_stroke_effort()`: the effort of
  writing an element of a handwritten font, `length + k * pieces` but not less
  than the least effort, which a dot and a move of the pen in the air take;
  the measure for the pace of the pen
- `dgx_hw_line_begin()` and `dgx_hw_line_place()` place the symbols of a
  handwritten font in a line: by width, keeping apart the parts of tall
  symbols above the lowercase band. Handwritten fonts are never of fixed
  width, `widthType` of the file is not read
- added the handwritten font `font0ss` (`fonts/font0ss.h`): 153 symbols, Latin,
  German and Russian letters, digits and punctuation
- `font2c` converts handwritten fonts: given a `.json` file of format
  `hw-font` version 1 it writes such a font. Ranges and `-f` select symbols as
  for other fonts; there is no size argument
- `dgx_font_t` has `number_of_ranges`; when a font tells it,
  `dgx_font_find_glyph()` searches the ranges by halving instead of trying
  them in order. `font2c` writes it for every font; fonts generated before
  work as they did
- `dgx_draw_line_thick()`: an exactly horizontal or vertical line of an even
  width put its wider side down or right whatever its direction; now it is on
  the right of the direction, as it always was for slanted lines
- added `dgx_bw_write_value()`: it was declared in `dgx_bw_screen.h` but never
  defined. It writes one pixel at the current position of the area set with
  `dgx_bw_set_area()` and moves on, the way `dgx_bw_write_data()` does for a run
- README: installing from the ESP Component Registry, the `backlight`
  parameter of `dgx_ili9341_init()`, the `-f` option of `font2c` and other
  corrections; `examples/screen_demo` runs in portrait, not landscape
- ST7735, ST7789: `rst` may be `GPIO_NUM_NC`, as the README says; the reset
  pulse was sent to the pin unconditionally
- SSD1351: `dgx_ssd1351_orientation()` still had the `&= 0x40` fixed in the
  init sequence in 0.3.0, so 18-bit mode lost the other remap bits on every
  orientation change
- P8 bus: a failed `esp_lcd_panel_io_tx_color()` no longer leaves the bus
  waiting forever for a completion that will not come
- panels with a bus: `set_pixel` waits for the transfer before returning; it
  sends a buffer from its stack, and the P8 bus transmits asynchronously
  without copying it
- panels with a bus: `fill_rectangle` and `set_pixel` packed 4- and 12-bit
  pixels by the screen column instead of the position in the buffer, so
  everything starting at an odd column was shifted by a nibble
- `dgx_vscreen8_to_screen16()` with transparency sent the palette index instead
  of the color to a physical screen; transparency is now decided by index 0, as
  documented, and not by the color the palette gives (a black entry was
  skipped on virtual screens)
- `dgx_vscreen_to_screen()`, `dgx_vscreen_region_to_screen()` and
  `dgx_vscreen8_to_screen16()` flush the target once per call instead of once
  per pixel when the row cannot be copied as bytes (1-bit targets)
- stream bitmaps (`is_stream`): `dgx_bw_bitmap_get_pixel()` and
  `dgx_bw_bitmap_set_pixel()` addressed the wrong byte for every bit but the
  first of each byte
- gauge: no division by zero when `min_value == max_value` or
  `sweep_degrees == 0`; `start_angle` is in degrees, the header and the README
  example said radians
- `dgx_read_buf_value_32()`: the top byte was shifted as a signed `int`
- Kconfig: the P8 backend depends on `SOC_LCD_I80_SUPPORTED`, so it is not
  offered on targets without an I80 peripheral (ESP32-C3 and the like)

## 0.3.1 - 2026-10-06

- `dgx_draw_texture_quad()`: a quad with two coinciding vertices on a side (a
  triangle passed as a quad) was drawn as a rectangle below that vertex; the
  edge chains now skip vertices that lie on the same scanline

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
