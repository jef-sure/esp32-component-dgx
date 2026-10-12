# Changes

## 0.5.3 - 2026-10-12

- **with `DgxDsiTwoFrames` the bus no longer sets the pace of a program.**
  The end of a batch used to be the order to show it, and the drawing that
  followed waited until the display had: a batch a frame. Now the end of a
  batch tells that there is a picture to show, and the display takes it
  where its frame ends. A batch that comes before that goes into the same
  picture: the display is told to stay with the frame buffer it reads, the
  batch is drawn, the display is told again. So what is drawn in the time of
  a frame is shown together, by one change of the frame buffers, whoever
  drew it. Tasks that draw each its own part of the screen used to get the
  frames in turn, three tasks a third of them each; on an ESP32-P4 three
  tasks that each put a part of 320x100, or a band of 1024x168 of which
  little is redrawn, to the display now get 61 pictures a second each
  instead of 20. A program that draws more often than frames are shown gets
  the last of what it drew into every frame, and has to hold itself back if
  it relied on the wait, see `dgx_dsi_frame_ended()` below. Which of several
  tasks draws when is the program's business as well, and they still have to
  draw on a screen one at a time. A batch does not join a picture when the frame is about to end, by
  the time the longest of the last batches took and 2 ms, and when the
  picture has been kept back over an end of a frame once: it waits then;
- with `DgxDsiTwoFrames` a large piece the frame buffer lacks of the other
  one is not copied when a picture is finished. A row has to be whole when
  the display comes to it, not when the display is told of the picture, so
  the piece is copied after the display has taken the picture, by the 2D
  DMA, which writes twice as fast as the display reads: started from the
  interrupt that tells the end of the frame, one piece after another from
  the top. Until then a batch that goes into the same picture may draw over
  the piece, and then it is not copied at all. With three tasks and bands of
  the whole width less than 1 MB is copied in 10 seconds instead of 411, and
  a batch
  that follows a large one elsewhere takes half the time and less: a square
  of 48x48 after a fill of the whole screen 0.6 ms instead of 8.6, a band of
  200 rows in two places in turn 2.8 ms instead of 5.5. Drawing that would
  write over a piece not copied yet waits for it; a task that waits for the
  display copies the pieces itself meanwhile. The speed of the copy is not
  assumed: the bus times its transfers, and a piece that would not be sure
  to stay ahead of the display is copied at once, as before. For 16-bit
  color only: at 24 bits the display reads half as many bytes more in the
  same time, and pieces fell behind it. Needs the 2D DMA
  (`CONFIG_DGX_DSI_HARDWARE_COPY`) and 0.85 KB of RAM with two frame
  buffers;
- `dgx_dsi_counters()` gives what a display on the DSI bus has done since it
  was started: frames sent, changes of the frame buffers, batches, batches
  that joined a finished picture, ends of frames a finished picture was kept
  back over, waits for the display and for the scan with their time, bytes
  copied from one frame buffer into the other and how many of them after the
  display took the picture, the speed of the 2D DMA as the bus measured it.
  A bus takes 0.2 KB more RAM;
- `dgx_dsi_frame_us()` tells how long a frame of a display on the DSI bus
  takes, so that a program can keep the pace of the display by its own
  clock now that the bus does not hold it back. Reckoned from the timings
  of the panel at first, 16412 us for the JD9165 panel; with two frame
  buffers and behind the scan it is the time of the frames counted from
  half a second on, 256 of them at a time, 16417 us on the same board.
  `dgx_dsi_frame_ended()` tells when the last frame ended. By the two a
  program aims a picture: it starts it so as to finish it a little before a
  frame ends, by how long its last pictures took; `demo_pace()` of
  `examples/dsi_demo` does that. A task that draws a picture every second
  frame got 302 of 303 pictures two frames apart so; by the clock alone it
  was chance, all of them in one run and half in another;
- `dgx_dsi_bench()` measures on the board at hand the numbers a DSI bus
  decides by: from what size on a rectangle is copied by the 2D DMA, rows of
  a program are moved by it and a rectangle is filled by the accelerator,
  how fast the processor writes through a window and the cache brings out,
  how much room a transfer needs to start. They depend on the
  chip, its memory and the display, and the bus had those of one board
  built in. It works in memory of its own, half a megabyte of PSRAM given
  back at the end, while the display runs, for a running display takes its
  share of the memory: nothing on the screen changes. It takes 0.07 s, so
  it is not run by the bus and not at every start: what it gives is a flat
  structure of 40 bytes, `dgx_dsi_tuning_t`, which a program keeps, in NVS
  for one, and gives to the bus by `dgx_dsi_tune()`; `examples/dsi_demo`
  does that. Numbers of another version, another panel or another color
  depth are not taken. `dgx_dsi_tuning()` reads what the bus has. A bus that
  is given nothing works with the numbers it had before. On the board they
  were taken on the measured ones choose the slower way no more often than
  those; rows of a program go by the 2D DMA from 4.5 KB instead of 16, which
  is the faster way for the sizes between. A bus takes 48 bytes more RAM;
- `dgx_dsi_tiles(bus, width, height)`, an optional call that answers by an
  `esp_err_t`, gives the size of
  the tiles a display with two frame buffers keeps what one of them lacks
  by: before the constructor of the display, or between two batches of
  drawing, when the bus waits for the display to take the finished picture
  and makes its maps anew: 11 ms on average and up to 23 ms, so the size is
  to be chosen once and not changed as a program goes;
- **the tiles of a display with two frame buffers are 32x4 pixels, were
  32x8.** The sizes were compared on the nine demos of DGX run on an
  ESP32-P4 and on text of bitmap fonts. Tiles of 32x8 are the fastest on
  every demo, but tiles of 32x4 are only 0 to 3% slower there, which is not
  seen, and 6% faster on such text and 28% faster on pixels set here and
  there, which is. The maps take 0.6 KB of RAM more for a panel of 600
  rows. Tiles one row high are 2.6 times as fast on such pixels, up to one
  and a half times slower on lines, narrow bars and fills, and take 4.2 KB
  more than tiles of 32x8. `dgx_dsi_tiles(bus, 32, 8)` gives the size that
  was;
- the README has a section on drawing from several tasks: what a screen, a
  bus and a virtual screen keep between calls, why a screen takes one task
  at a time, and two ways for a program with several sources of pictures:
  messages to one task that owns the display, which is the way to even
  motion and keeps clear of inversion of priorities, or a mutex of the
  program around a batch. Measured on an ESP32-P4 with three sources that
  each redraw a band of 1024x168 whole, a picture every second frame: by
  messages 302 of 303 pictures of each came two frames apart; with a mutex
  from 170 to 302 of them did;
- fixed: three commands the JD9165 driver sets its controller up with never
  reached the display. They are the registers 0x2A, 0x2B and 0x2C of page 1
  of the controller, the same numbers as the commands of the window, and
  the DSI bus took them for those: it answered the commands of the window
  from the moment it had a frame buffer, which is before the driver sends
  its table. Now it does so from `dgx_dsi_video_start()` on.

## 0.5.2 - 2026-10-12

- **`dgx_jd9165_init()` takes one more argument**, `frames`, the way the
  display is updated. A display on the DSI bus reads its frame buffer all the
  time, also while it is drawn into, so a picture that changes over much of
  the screen may be seen half updated for a frame. There is no way around it
  that suits every program, so the program chooses one of three.
  `DgxDsiOneFrame` is what there was: one frame buffer, drawing shows at
  once. `DgxDsiTwoFrames` draws into a second frame buffer and shows a batch
  whole from the next frame on; nothing tears, for one more frame of PSRAM
  and at most a batch a frame. `DgxDsiOneFrameAfterScan` keeps one frame
  buffer and starts what is put to the display when the scan of the display
  is out of its rows: a compromise, a picture that comes from a virtual
  screen is whole, a batch drawn on the display itself is not shown as one.
  `dgx_dsi_video_setup()`, which a driver calls, takes the same argument;
- a part of a 16-bit virtual screen goes to a DSI display by the 2D DMA of
  the chip when it is 16 KB or more and the screen is not turned. The DMA
  exchanges the bytes of the pixels on the way and writes twice as fast as
  the display reads. On an ESP32-P4 a whole screen of 1024x600 goes in 8 ms
  instead of 34, a part of 320x100 in 0.5 ms instead of 1.1. A bus may now
  have a function `write_rows` for that; a virtual screen offers its rows to
  it before it sends them through the window;
- a large rectangle on a DSI display is filled by the pixel processing
  accelerator of the chip, in any orientation, 16 and 24 bits: the whole
  screen in 3.5 ms instead of 16.4, 100x100 in 0.13 ms instead of 0.18, a
  column of the height of the screen in 0.15 ms instead of 0.5. Short runs
  stay with the processor, text and lines take the time they took;
- `CONFIG_DGX_DSI_HARDWARE_COPY`, on by default, turns the two off; they
  cost 0.8 KB of RAM. Bars moving over the whole screen through a virtual
  screen: 44 frames a second instead of 20 with one frame buffer, 43 instead
  of 20 with two and none broken; in a band of 200 rows 61 instead of 46
  with two. Behind the scan the DMA and the accelerator stay ahead of it, so
  with `DgxDsiOneFrameAfterScan` such a part or rectangle is seen whole, the
  whole screen as well, on one frame buffer;
- with two frame buffers the one drawing goes into lacks what the last batch
  drew. The bus keeps that by tiles of 32x8 pixels, two maps of a bit a tile,
  600 bytes for 1024x600, and copies a tile when something is first drawn in
  it or read from it, the rest before the batch is shown, large pieces of
  the rest by the 2D DMA. What a rectangle or a window sent whole covers is
  not copied; a window that is left before it is full gets what its pixels
  did not reach. The work of a batch against one rectangle around all that
  was drawn: a line from corner to corner erased and drawn 1.9 ms instead of
  21, two squares of 48x48 far apart 0.3 ms instead of 5.9, 2000 pixels all
  over the screen 7.4 ms instead of 16.7, strips of nine bars 17 ms instead
  of 32; a gradient of 256x100 set pixel by pixel 14.3 ms instead of 12.9,
  which is what a look at the maps costs for every pixel;
- the 2D DMA is driven through a private interface of IDF,
  `esp_private/dma2d.h`, and one field of a register of the chip is set
  directly: IDF 6.0.3 and later set up the exchange of bytes for pixels of
  3 bytes only, and a transfer of 16-bit pixels never ends there. Checked
  with IDF 5.5.4, 6.0.3 and 6.1;
- where the driver of IDF has it, in 5.5.5, 6.0.3 and 6.1, the bus is told
  where the display takes a frame buffer by `on_frame_buf_complete`; there,
  and with the older `on_refresh_done` on chips of a revision below 3, a
  batch finished in the last 2 ms of a frame does not wait a frame more.
  Which of the two the IDF has is found out from its header when the
  component is built, not from its version;
- `docs/dsi-display-en.md` and `docs/dsi-display-ru.md`: what was measured
  on a display on the DSI bus and why its updates are made the way they are;
- `examples/dsi_demo`: bars moving sideways, a scene that shows tearing, and
  `CONFIG_EXAMPLE_LCD_TWO_FRAMES` to see it gone.

## 0.5.1 - 2026-10-11

- the DSI bus moves rows of 16-bit pixels into the frame buffer by 4-byte
  words, two pixels at once with their bytes exchanged in the word, where a
  row is 16 pixels or longer and the screen is not mirrored or turned. On an
  ESP32-P4 a whole virtual screen of 1024x600 goes to the display in 34 ms
  instead of 38, a part of 320x100 in 1.1 ms instead of 1.3. A pixel written
  as one 16-bit value was measured too and is slower than two bytes;
- host tests stop at the first report of the sanitizer of undefined behaviour:
  a word written at a wrong boundary is an exception on an ESP32 and only a
  line of text on a PC.

## 0.5.0 - 2026-10-11

- added a MIPI-DSI bus for ESP32-P4, `dgx_dsi_init()`
  (`CONFIG_DGX_ENABLE_DSI`). A display on it is sent a frame buffer in PSRAM
  all the time. For a driver the bus is a display with memory of its own: it
  takes the window and the pixels that follow, as an SPI display does, and
  puts them into the frame buffer in the byte order that buffer has. The
  orientation is carried out by the bus where pixels are put, so all eight
  work with any display, and a pixel can be read back. A pixel and a
  rectangle go straight into the frame buffer. What is drawn is brought out
  of the cache of the chip when the screen is updated, and on the way when a
  batch is long, so it shows in the order it is drawn;
- added a driver of JD9165, `dgx_jd9165_init()` (`CONFIG_DGX_ENABLE_JD9165`):
  1024x600 on two DSI lanes, 16 or 24 bits a pixel, with the commands of the
  7-inch panel of the Guition JC1060P470 board;
- added `examples/dsi_demo` for that board: a test card, ramps of colors that
  show 16-bit color against 24-bit, lines, circles, triangles. Its README has
  what the size of the PSRAM cache costs in internal RAM and gives in speed;
- lines and circles are one code for every screen now, made of the pixels and
  the rectangles a screen draws itself, so a figure has the same pixels on a
  display, on a color virtual screen and on a monochrome one. A line is
  Bresenham's and has the same pixels whichever end it is given from; before,
  a third of the lines depended on the direction, and a pixel could lie 0.86
  of a pixel off the true line, now it is 0.5 at most. A circle is the
  midpoint one, walked for an eighth and reflected; the outline used to send
  every pixel to a display twice. A solid circle is filled by rows, rows of
  one width as one rectangle. **The pixels of lines have changed**, those of
  circles on displays and monochrome screens at radii 4, 11, 134 and 373;
- a color and a monochrome virtual screen take a row and a column of one
  pixel a short way of their own inside `fill_rectangle()`: lines and circles
  are made of them, and the checks a rectangle needs cost more than a few
  pixels. A 16-bit virtual screen is filled by 4-byte words;
- `DGX_FILL_BUFFER` is removed. `dgx_fill_pixels()` fills a run of pixels in
  a buffer for every depth, by words where the bytes of a pixel repeat;
  `dgx_fill_pixels_16()` and `dgx_fill_rows_16()` are the 16-bit case for a
  row and for rows one under another;
- the tables of commands a display is set up with are no longer kept in RAM:
  `dgx_lcd_init()` sends their data through the buffer of the bus. With an
  ILI9341 that is 576 bytes of RAM back; the display is sent the same bytes;
- fixed: a build without the SPI bus did not link, the window code of a
  display always took functions of that bus;
- measured on an ESP32 at 240 MHz against 0.4.9, RAM use the same but for
  the 576 bytes: on an ILI9341 over SPI the outline of a circle is 3.2-3.6
  times faster and a solid circle 1.25-1.3 times, lines, fills and text are
  as they were. On a 16-bit virtual screen a fill of 320x240 takes 350 us
  instead of 1016, a column of 100 pixels 2 us instead of 22, a steep line is
  1.8 times faster, a solid circle up to 2.7 times. On an 8-bit one a steep
  line is 2.5 times faster, circles 1.8-1.9 times, text 1.1 times. On a
  monochrome screen lines are 1.4 times faster, the outline of a circle up to
  5.4 times. Nothing measured is slower;
- host tests: `test_fb_area`, the frame buffer of the DSI bus against a model
  that places every pixel on its own; `test_pixels`, a run filled at once
  against pixels set one by one for every depth; `test_fill`, a rectangle of
  a virtual screen against the same; `test_circle`, lines and circles against
  the algorithms as the textbooks give them.

## 0.4.9 - 2026-10-10

- the glow renderer keeps no frame of colors any more: `dgx_morph_glow_present()`
  maps brightness to colors on the way to the screen, row by row through the
  buffer a display sends from, or right into the pixels of a virtual screen.
  A glow takes 2 bytes a pixel instead of 4 for 16-bit color and instead of 5
  for 18- and 24-bit color; what is shown is the same, byte for byte. An
  18-bit display now gets the frame in transfers of rows and not pixel by
  pixel;
- `dgx_morph_glow_blur()` in one pass no longer takes a copy of the frame: it
  is made row by row on the way to the screen, on four rows, with the same
  result. With the blur on, a glow takes 2 bytes a pixel instead of 5 for
  16-bit color. More passes and filters of one's own get the copy as before,
  made when it is first needed;
- added `examples/weather_morph_demo`: the weather symbols a clock shows,
  one large symbol into the next, in two orders shown in turn. In the first
  every step is one a clock makes, the weather turning into a neighbouring
  one or the day into the night; in the second each weather goes by day and
  then by night;
- a host test of a letter getting its two dots, u to ü and the like: the
  dots bud off the cells of the letter that stay, each from a cell of its
  own, and flow back into them with `DGX_MORPH_ORPHANS_MERGE`.

## 0.4.8 - 2026-10-10

- added `dgx_morph_create_with()`, `dgx_morph_create()` with options: a
  second way for an old cell that nobody took as a source and that is not
  in `to`. By default such a cell fades where it is, as before; with
  `DGX_MORPH_ORPHANS_MERGE` it flies into the nearest cell of `to`, static
  or arriving, and goes out there, so the bottom bar of an "E" is swept into
  the foot of the stem of an "F" instead of melting in place. The nearest
  cell is found along the figure first: a cell next to a cell of `to` takes
  it, the others take the cell of a neighbouring orphan, wave after wave, so
  a bar flows whole into the stem it touches and not partly up into a nearer
  bar; a piece cut off from everything looks by the rings of
  `dgx_morph_scan_vector()`. `merge_radius` bounds the way both ways, 0 is
  no limit. The flights are in a new array `merging` at the end of
  `dgx_morph_t`; `dgx_morph_draw()` draws them full to the middle of the way
  and fading by smoothstep to nothing at the end, where the tail meets the
  head, so the last frame is the clean `to`. `segments`, `static_points` and
  `fading_points` are what they were, and a plan without the option is the
  same bit for bit. On an ESP32 a plan between any two weather symbols
  takes 3.3 ms on average and 9.3 ms at most with the merge, against 2.6 and
  7.7 without; within 8 cells 3.2 and 7.8, with 88% of the orphans merging
  and a flight of 3.7 cells on average. Weather that follows one another
  takes 2.0 ms against 1.6, and every orphan finds a cell within 8. A frame
  costs the same: `examples/glyph_morph_demo` on a CYD runs at 67 frames a
  second either way
- `examples/glyph_morph_demo` has the mode and the radius as defines at the
  top of `main.c`, merging within 8 cells by default, and shows three
  sequences in turn, each with a renderer of its own: digits, beginning with
  E, F, 8, 3, 0, 1 to compare the modes on; the weather symbols a clock
  shows, each by day and then by night, 42x39 cells at 5 px a cell, 45 frames a second;
  the letters

## 0.4.7 - 2026-10-09

- added `dgx_vscreen8_region_to_screen16()`: a rectangular part of an 8-bit
  virtual screen expanded into a 16-bit screen through a LUT, cut by the
  source and by the destination. `dgx_vscreen8_to_screen16()` is that
  function for the whole source and gives the same pixels as before
- added `dgx_screen_take_dirty()`: takes the area a screen has changed in
  since the last time and clears it, without `update_screen()`. With a batch
  kept open on a virtual screen, only what was drawn is sent to the display:
  a step of a pen instead of the whole band. Fill, circles, thick curves,
  writing by a pen and frames of a handwritten morph were checked to mark
  every pixel they change. On an ESP32 with an ILI9341 at 40 MHz a band of
  320x100 takes 16.7 ms whole, a step of a pen 0.09 ms, the place of one
  digit of 60x100 3.2 ms

## 0.4.6 - 2026-10-09

- `font2c` names the symbols of a handwritten font whose own parts stand
  closer than the distance pairs are checked by, such as a dot over a letter
  that a thick pen joins to it
- `font0ss`: the dots stand further from their letters and from each other.
  A dot over "i" was 8 cells from the letter and a pen of 9 cells joined
  them at any size; the dots of "Ö" were 3 cells from it, the two dots of
  "ö" 12 cells apart. The dots of "i", "j", "ё", "ä", "ö", "ü" are on one
  line 22 cells above the lowercase letters, those of "Ё", "Ä", "Ö", "Ü" at
  the top of the cell, and two dots of a letter are 18 cells apart. These
  symbols are taller by 5 to 16 cells
- `font0ss` has a minus sign, U+2212: a line of 50 cells at the middle of
  the digits, 60 cells above the base line. The hyphen-minus "-" stays where
  it is, in the middle of the lowercase letters, 31 cells lower: a name such
  as "Ростов-на-Дону" comes with it. A number is to be written with "−" by
  the code that prints it. The font has 154 symbols

## 0.4.5 - 2026-10-09

- the handwritten font `font0ss`: more space around the punctuation that
  has a dot on the base line. A period, a comma, a colon, a semicolon and
  an ellipsis stand 12 cells further from both neighbours, an exclamation
  and a question mark from the symbol before them. At a small size the dot
  touched the foot of "1" and "2" and the tail of "9": "1.1" and
  "1.9" read as one line. A text with these marks is wider now, 24 cells
  for a period
- `font0ss`: a lowercase letter after "Ф" and after an opening bracket stood
  under them closer than a pen is thick ("Фи", "Фр" at 8.6 cells with a pen
  of 9). "Ф" has 24 cells after it instead of 16, the bracket 18
- `font2c` checks the pairs of symbols of a handwritten font: after
  converting it names the pairs that stand closer in a line than the symbol
  space of the font, by the distance between the lines of the pen in cells.
  `-g cells` sets the distance and lists every such pair, `-g 0` turns the
  check off. With the pairs it tells which `spaceBefore` or `spaceAfter` of a
  symbol moves them apart, and how large it has to be; `-x` puts those spaces
  into the file of the font, `-i` asks about each one, `-t cells` moves the
  pairs further apart than the distance they are found by

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
