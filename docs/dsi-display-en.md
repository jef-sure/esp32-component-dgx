# A MIPI-DSI Display in DGX: Frame Buffer, Scan and Tearing

[Русская версия](dsi-display-ru.md)

An internal note: how the MIPI-DSI bus for the ESP32-P4 is built in DGX, what
has been measured on it, and why the screen is updated the way it is. What a
program needs to know to use such a display is shorter and lives in
[dsi-usage-en.md](dsi-usage-en.md).

The code described is version 0.6.2. The bus is `src/bus/dsi_esp32.c`, the
frame buffer core `src/bus/fb_area.c`, the tile maps `src/bus/fb_tiles.c`,
the swap of two buffers `src/bus/fb_flip.c`, the drivers
`src/drivers/jd9165.c` and `src/drivers/jd9365.c`.

How to read this note.

- **There are two boards**, and they are always named by the panel
  controller: **JD9165 (7")**, 1024x600, and **JD9365 (10")**, 800x1280.
  Nearly everything was measured on the JD9165 (7"). When a table does not
  name a board, it is that one.
- **Every table of numbers** is preceded by one line saying which board,
  which mode and which IDF it was taken on, and what the cells hold.
- **The text describes the current code.** How it changed from release to
  release is collected at the end, in "How it changed".
- **Every number** is either measured on a board or marked as a calculation.
  KB is 1024 bytes. Speeds are in KB/ms; the one reference point for them is
  the display of the JD9165 (7"), which reads the frame buffer at 77 KB/ms.

## Contents

- [The two boards](#the-two-boards): [what is known about them](#what-is-known-about-the-boards)
- [The bench and its scenes](#the-bench-and-its-scenes)
- [Terms](#terms): [window](#window), [batch](#batch), [tile](#tile)
- [What the bus looks like to a driver](#what-the-bus-looks-like-to-a-driver)
- [Byte order](#byte-order)
- [The cache and PSRAM](#the-cache-and-psram)
- [The scan](#the-scan): the frame period, where the scan is now, how the
  end of a frame is reported
- [Three strategies](#three-strategies)
  - [One buffer](#one-buffer)
  - [Two buffers](#two-buffers): the swap, batches of several tasks in one
    frame, the copy after the swap, counters, the time of a frame,
    evenness, [how a program keeps the pace](#how-a-program-keeps-the-pace),
    one drawing task
  - [Behind the scan](#behind-the-scan)
- [The catch-up copy](#the-catch-up-copy): the cost of copying, tiles,
  [the size of a tile](#the-size-of-a-tile), how LVGL 9 does it
- [The 2D DMA and the PPA](#the-2d-dma-and-the-ppa): the IDF interface, the
  byte exchange, [a virtual screen put to the display](#a-virtual-screen-put-to-the-display),
  fills, internal RAM
- [Measuring the board](#measuring-the-board): `dgx_dsi_bench()`,
  `dgx_dsi_tune()`, `dgx_dsi_advise()`
- [When the PSRAM cannot keep up](#when-the-psram-cannot-keep-up): flashes
  on the JD9365 (10")
- [A picture scrolled](#a-picture-scrolled)
- [Summary measurements](#summary-measurements): the working time of a
  batch, frame rate
- [What is not done](#what-is-not-done)
- [How it was checked](#how-it-was-checked)
- [How it changed](#how-it-changed)

## The two boards

| | JD9165 (7") | JD9365 (10") |
| --- | --- | --- |
| Board | Guition JC1060P470, 7 inches | Guition JC8012P4A1C, 10.1 inches, with an ESP32-C6 for radio |
| Panel | JD9165, 1024x600, landscape | JD9365, 800x1280, portrait. The newer of the two panels the board was sold with |
| Chip | ESP32-P4 revision v1.3 as `esptool` reports it, two cores | ESP32-P4 revision v3.2 as `esptool` reports it ("Dual Core + LP Core, 400MHz"), ROM `esp32p4-eco7-20260109`, efuse block v1.2 |
| CPU clock | 360 MHz, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360`, the default for revisions below 3 | 400 MHz, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y` |
| Build for the revision | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, minimum revision 0 | minimum revision 3.1, `CONFIG_ESP32P4_REV_MIN_FULL=301` |
| Flash | 16 MB | 16 MB, Boya (manufacturer 0x68, device 0x4018); IDF warns that it drives it with the generic driver |
| PSRAM | 32 MB, HEX mode, 200 MHz | 32 MB AP Memory generation 4, X16 HEX mode, fixed latency of 18 clocks. Runs at 200 and at 250 MHz (`CONFIG_SPIRAM_SPEED_250M`, revision 3 and newer only); the memory test at start passes at both |
| L2 cache | 256 KB, 64-byte line, unless said otherwise | not varied |
| DSI lanes | 2 at 750 Mbit/s | 2 at 1000 Mbit/s |
| Pixel clock | 52 MHz | 80 MHz, see 70 and 80 below |
| Frame | 16418 µs, 60.9 Hz, measured | 16682 µs, 59.9 Hz, measured |
| Optimization | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` | the same |
| IDF | timing tables taken under 5.5.4, 6.0.1 and 6.1, each table says which; builds also checked under 5.5.5 and 6.0.3 | 6.0.1; the example also builds under 5.5.5 |
| Panel driver in DGX | `dgx_jd9165_init()`, `src/drivers/jd9165.c` | `dgx_jd9365_init()`, `src/drivers/jd9365.c` |
| What was measured on it | all timing tables, the tearing checks, the choice of tile size, the comparison of the board measurement with the bus's choices | the bus on revision 3, the end of frame on that revision, PSRAM bandwidth, the thresholds of `dgx_dsi_advise()`. No timing tables were taken on it |
| In use since | the start | 2026-10-17 |

Under the DGX bus (`dgx_dsi_init()`, `src/bus/dsi_esp32.c`) both have the
MIPI-DSI driver of the IDF `esp_lcd` component: `esp_lcd_new_dsi_bus()`,
`esp_lcd_new_panel_io_dbi()` for commands, `esp_lcd_new_panel_dpi()` for
video mode. The Espressif component `espressif/esp_lcd_jd9165` is not used
by DGX; only a diagnostic bench used it, to light the panel with the factory
table before the own driver worked.

The chip revision is not a formality. It decides when the IDF driver
reports the end of a frame, see "The scan". The bus has two branches, one
per revision, and each is checked on its own board.

### What is known about the boards

**JD9165 (7").** By the shop's description the board resembles the Waveshare
7B with an EK79007 controller, but it is not that. What it is can be seen
from its own firmware: the flash image holds the strings `Hello,Guition` and
`esp_lcd_new_panel_jd9165`. All parameters below come from there, not from
other people's descriptions.

| Parameter of the JD9165 (7") | Value |
| --- | --- |
| Line: sync pulse, back porch, front porch | 24, 136, 160 pixels |
| Frame: sync pulse, back porch, front porch | 2, 21, 12 rows |
| PHY power | LDO channel 3, 2500 mV |
| Panel reset | GPIO0: high 5 ms, low 10 ms, high 120 ms |
| Backlight | GPIO23, on when high |
| Init table | 60 commands, taken from the factory firmware; differs from the table that circulates on the net for an older panel |

**JD9365 (10").** The factory firmware, `esp_brookesia_demo` 1.0.1 on IDF
v6.0.2, sends the panel a table of 204 commands and asks for a pixel clock
of 70 MHz. All eight orientations of `dgx_jd9365_orientation()` look as
expected on the board.

About 70 and 80 MHz. Asked for 70 MHz, IDF 6.0.1 makes 80 MHz of it with a
longer line at the bridge between the DPI and the DSI host (1006 points),
while it sizes the line of the DSI host itself by 70 MHz (1571 byte clocks).
That is 12.568 µs at the host against 12.575 µs at the bridge: the host takes
each line 7 ns early, about 740 FIFO points a frame. This is somebody else's
[issue 19188](https://github.com/espressif/esp-idf/issues/19188) in esp-idf,
with a patch. The DGX driver therefore asks for 80 MHz with a line of 1008
points: then both lines are equal. The frame is then 16682 µs; with 70 MHz it
was 16645 µs, 60.08 frames a second. The flashes from a shortage of data,
see "When the PSRAM cannot keep up", did not change with this fix, so they
do not come from here.

How much this panel reads from PSRAM, calculated from the timings: 160 MB/s
within a line (80 MHz at 2 bytes), 123 MB/s on average over a frame.

## The bench and its scenes

All numbers in this note were taken by the bench: a separate program not
included in the repository. The bench shows scenes of 10 seconds each and
counts, and they are looked at by eye on the board; every firmware looked at
by eye wrote its build number on the screen. Below, scenes are referred to
by the name and number from this table.

| Scene | What is drawn | Who draws |
| --- | --- | --- |
| 1. Columns, whole screen | running columns are redrawn in a virtual screen of 1024x600 and put to the display whole | one task |
| 2. Columns, band of 200 rows | the same, a virtual screen of 1024x200 | one task |
| 3. Columns by primitives | nine columns 548 high move by 4 pixels right on the display: a strip is erased on the left of each and drawn on the right, 18 fills a batch | one task |
| 4. Columns, band of 80 rows | as scene 2, a virtual screen of 1024x80 | one task |
| 5. Arrow and squares | an arrow going round and four squares in the corners, by primitives | one task |
| 6. Diagonal | a line across the whole screen that moves: erase the old, draw the new | one task |
| 7. Three bands | three bands of 1024x168 one below another; in each the columns are erased and drawn whole in its own virtual screen and put to the display; each task keeps the display's pace by its own clock | three tasks under one mutex |
| 8. Three bands with pauses | as 7, but the tasks sleep 7, 14 and 21 ms after each picture | three tasks |
| 9. Three parts | parts of 320x100 side by side, at the display's pace | three tasks |
| 10. Three parts with pauses | as 9, with pauses of 7, 14 and 21 ms | three tasks |
| 11. Three light bands | as 7, but only the edges of the columns are redrawn: a task spends little on its picture, and the whole band goes to the display | three tasks |
| 12. Three light bands with pauses | as 11, with pauses of 7, 14 and 21 ms | three tasks |
| 13. Three bands every other frame | as 7, a picture every second frame | three tasks |
| 14. Columns by primitives every other frame | as 3, a picture every second frame | one task |
| 23–26. Sources and a drawing task | three sources redraw their virtual screens and send a message to one task, which puts them to the display; the content is that of scenes 9, 11 and 7 | one drawing task |

In the scenes with one buffer the bench holds the pace itself: a pause of
16 ms where drawing goes right to the display, and a timer with the display's
period where a picture is put.

Besides the scenes the bench times single batches, see "The working time of
a batch", and checks correctness: random batches against a reference, see
"How it was checked".

## Terms

- **Frame buffer**, or just buffer: memory in PSRAM that the chip sends to
  the display without end. For the JD9165 (7") it is 1024 × 600 × 2 =
  1228800 bytes at 16 bits a pixel.
- **Frame**: one pass of the display over the frame buffer, 16.4 ms.
- **Scan**: the place in the frame buffer the display is reading right now.
  A liquid crystal panel has no real beam, but it reads the same way, row
  after row from the top down.
- **Window**: a rectangle of the screen that drawing code sets in order to
  send pixels into it one after another. More below.
- **Batch**: a set of changes to the screen that are shown together. More
  below.
- **Picture**: with two buffers, what lies in the hidden buffer waiting to be
  shown. **Finished picture**: the driver has been told of it, and the
  display will take it at the end of the frame.
- **Shown** and **hidden** buffer: with two buffers, the one going to the
  display now and the other.
- **Catch-up copy**: with two buffers, bringing into the hidden buffer what
  the previous batch drew into the other one. **Piece**: a rectangle of such
  a copy.
- **Tile**: a cell of the fixed grid the bus lays over the panel, 32x4
  pixels. Needed only with two buffers. More below.
- **Virtual screen**: a DGX screen that lives in memory and is put to the
  display whole or in part (`dgx_vscreen_to_screen()`).
- **Tearing**: a frame in which part of the screen shows the new and part the
  old.
- **Rows of a program**: rows of pixels a virtual screen hands the bus to put
  to the display, see `write_rows` in "A virtual screen put to the display".

### Window

A window is how DGX writes to any display with a controller. First two
commands set a rectangle: columns from and to, rows from and to. Then come
the pixels, each next one landing to the right of the previous, and at the
end of a row moving to the start of the next row of the window. No
coordinates travel with the pixels.

```c
scr->set_area(scr, left, right, top, bottom);  /* the window */
scr->write_area(scr, pixels, bits);            /* pixels in a row, as many as there are */
scr->write_area(scr, more_pixels, more_bits);  /* continues from the same place */
```

This is how letters, textures and a virtual screen are drawn. The window is
set by drawing code and can be anything: one pixel, a letter, the whole
screen. A window need not be filled to the end: a new one can be set before
the old one is written out.

A pixel and a filled rectangle go to the DSI bus without a window, straight
into the frame buffer.

### Batch

A batch is a set of changes to the screen that must appear at the same time.
It holds everything drawing code did from one show to the next: any number
of windows with pixels, filled rectangles and single pixels, anywhere on the
screen.

Showing is the business of the screen function `update_screen()`: "show what
is drawn". For the DSI bus that means bringing data out of the cache, and
with two buffers also telling the driver a picture is ready: the buffers
themselves change at the end of a frame. One call of `update_screen()` ends
one batch.

No list of changes is kept anywhere. What remains of a batch is only where it
drew, kept in two ways:

- by the screen: one rectangle enclosing all changes of the batch. It is
  what `update_screen()` receives;
- by the DSI bus with two buffers: the tiles the batch drew in.

When a batch ends is decided by a nesting counter the screen keeps:
`dgx_screen_progress_up()` raises it by one, `dgx_screen_progress_down()`
lowers it. There is one rule: if something is drawn and the counter is zero,
`update_screen()` is called with the accumulated rectangle, and the
rectangle is reset. This is checked after every pixel, after every fill and
at every `dgx_screen_progress_down()`.

What follows.

- A line, a circle, a solid circle, text and a virtual screen put to the
  display wrap their work in a pair of `dgx_screen_progress_up()` and
  `dgx_screen_progress_down()` themselves. If the application has not
  touched the counter, it returns to zero at the end of each such function,
  and its result is shown on its own.
- If the application has called `dgx_screen_progress_up()` itself, the inner
  pairs never reach zero. Everything drawn is shown at once when the
  application calls `dgx_screen_progress_down()`.
- A pixel and a filled rectangle do not touch the counter. With the counter
  at zero every pixel and every rectangle is shown at once. That is why a
  pixel inside a batch costs 0.47 µs and one on its own 4.7 µs: it is
  brought out of the cache by itself.

```c
/* three batches: the line, the circle and the text are shown one after another */
dgx_draw_line(scr, 0, 0, 100, 100, color);
dgx_draw_circle(scr, 200, 200, 50, color);
dgx_font_string_utf8_screen(scr, 10, 40, "text", color, DgxOutputNormal, 1, font, NULL, NULL);

/* one batch: everything is shown at once */
dgx_screen_progress_up(scr);
dgx_draw_line(scr, 0, 0, 100, 100, color);
dgx_draw_circle(scr, 200, 200, 50, color);
dgx_font_string_utf8_screen(scr, 10, 40, "text", color, DgxOutputNormal, 1, font, NULL, NULL);
dgx_screen_progress_down(scr);
```

### Tile

Tiles are a fixed grid laid over the panel: cells 32 pixels wide and 4 high;
for 1024x600 that is 32 tiles in a row and 150 rows, 4800 tiles in all. They
differ from windows in that a window is set by drawing code and can be
anything, while the tile grid is one and does not change. A program may set
another cell size, once at start, see "The size of a tile".

For every tile the bus keeps one bit: drawn in or not. One line across the
diagonal of the screen, as a rectangle, takes the whole screen; as tiles it
takes only the cells it passed through.

Tiles are needed only with two buffers: by them the bus knows what has to be
brought from one buffer into the other. How exactly is in "The catch-up
copy".

## What the bus looks like to a driver

A DSI display runs in video mode: the chip sends it the frame buffer from
PSRAM without end. Commands only set the panel up; in video mode no pixels
travel by them.

The interface is nevertheless kept the same as for SPI. The bus takes the
window commands itself and puts the pixels that arrive into the frame
buffer.

| Command | What the bus does |
| --- | --- |
| 0x2A, 0x2B | remembers the columns and rows of the window |
| 0x2C, 0x3C | writes pixels into the window from its start or from where it stopped |
| 0x2E, 0x3E | reads the pixels of the window the same way |
| any other | sends it to the panel over DSI |

The bus takes the window commands itself only after `dgx_dsi_video_start()`.
Before that the driver sets the controller up, and the same numbers may be
its registers: the JD9165 has registers 0x2A, 0x2B and 0x2C on page 1, and
they reach the panel. At the debug log level the bus logs every command it
sent.

A panel driver on this bus is made of the same parts as the driver of any
DGX display: a constructor, `dgx_jd9165_init()` or `dgx_jd9365_init()`, a
table of init commands that the common `dgx_lcd_init()` sends, and an
orientation function. It differs from the driver of an SPI display by four
calls to the bus.

| Call | When | What for |
| --- | --- | --- |
| `dgx_dsi_video_setup()` | before the panel reset | hand over the video timings and the strategy, receive the frame buffers |
| `dgx_dsi_video_start()` | after the command table | start sending the frame buffer to the display |
| `dgx_dsi_screen_functions()` | at the end of the constructor | take the drawing functions from the bus, about them below |
| `dgx_dsi_orientation()` | in the orientation function | the bus performs the orientation; the panel is not given it by a command |

To the rest of the library it is an ordinary screen behind a bus. Text,
shapes and a virtual screen set a window and send pixels with the same calls
as for SPI. Who takes them, the display controller or the bus, is all one to
them.

The bus performs the orientation as it writes: horizontal mirror, vertical
mirror and swapped axes, eight combinations.

The drawing functions the bus gives the screen are `set_pixel`, `get_pixel`,
`fill_rectangle` and `update_screen`. They put a pixel and a rectangle into
the frame buffer directly, without a window. On top of them the DSI bus has
three optional functions no other bus has: `write_rows` takes rows of pixels
from memory and puts them into a rectangle of the screen, see "A virtual
screen put to the display"; `write_indexed` does the same with 8-bit indices
through a colour table, see "An 8-bit screen through a colour table";
`move` moves a rectangle of the screen to
another place on it, see "A picture scrolled".

## Byte order

DGX keeps a 16-bit pixel in memory high byte first. That is how the
controllers of SPI displays expect it, and a buffer goes out to SPI as it
is, with no processing. Everything lies in this order: virtual screens, the
bus buffer, the rows that fonts and textures prepare.

The frame buffer of the ESP32-P4 keeps the same pixel low byte first, like an
ordinary 16-bit number of the processor. With 24 bits it is the same: in DGX
memory red, green, blue; in the frame buffer blue, green, red.

| Pixel | In DGX buffers | In the frame buffer |
| --- | --- | --- |
| 16 bits, value 0x1234 | 0x12, 0x34 | 0x34, 0x12 |
| 24 bits | red, green, blue | blue, green, red |

Not one pixel can be moved into the frame buffer as it is. The bus exchanges
the bytes as it writes, and from outside this is invisible. But it has three
costs.

**Time.** Writing a whole buffer of 1024x600, 16 bits, JD9165 (7"):

| Where the processor writes from | No exchange | With the exchange | Added |
| --- | --- | --- | --- |
| Internal RAM, display not started | 13.7 ms | 17.8 ms | 30% |
| PSRAM, display running | 27.1 ms | 30.3 ms | 12% |

**No `memcpy`.** Writing goes by its own loop. Its fast variant, two pixels
in one 32-bit word, works only on runs of 16 pixels and more and only in the
normal orientation. Short runs, a mirrored and a rotated screen are written
byte by byte: 21.9 ms a buffer instead of 17.8 ms.

**The hardware path only by a detour.** The chip's 2D DMA copies a buffer in
8 ms, while the processor puts a virtual screen to the display in 34 ms. But
the public IDF interface to the 2D DMA does not exchange the bytes of a
16-bit pixel. The exchange could be had only through the private interface
and one register field set directly, see "The 2D DMA and the PPA".

What was considered.

- **Exchange in the bus as it writes.** Chosen as the main path. The
  interface stays uniform: the same code draws to SPI, to the parallel bus
  and to DSI, and a virtual screen can be put to any display. The cost is
  listed above.
- **Exchange in hardware, by the 2D DMA.** Done for putting a virtual screen
  to the display: large pieces go by the DMA, which exchanges the bytes on
  the way. Data format and interface do not change. Works for 16 bits and
  only when the screen is neither mirrored nor rotated.
- **A separate pixel format for the virtual screen**, in the byte order of
  the frame buffer. Putting it to the display would become a plain copy. But
  such a screen could not go to an SPI display without an exchange, and all
  code that puts pixels into buffers (fills, fonts, textures, copies between
  screens) would need a second variant. Not done, and after the previous
  point not needed.
- **Exchange in an intermediate buffer, then send.** Slower: 23.1 ms against
  17.8 ms, see "Writing into the frame buffer".
- **Write a pixel as a 16-bit value.** Slower than two bytes, same place.
- **24 bits.** Exchanging red and blue by the 2D DMA is not done: 24-bit
  pixels are moved by the processor.

LVGL made the opposite choice: the pixel lies in memory in the processor's
order, and for an SPI display the port exchanges the bytes before sending
(`lv_draw_sw_rgb565_swap`). On DSI it pays nothing; on SPI it pays what DGX
pays on DSI.

## The cache and PSRAM

The processor writes to the frame buffer through the cache. The DMA sends
the buffer to the display and reads PSRAM directly, past the cache. Three
things follow.

**The cache stands between writing and showing.** What the processor wrote
into the frame buffer lies in the cache and reaches PSRAM in one of two
cases: the bus brought it out itself with `esp_cache_msync()`, or the cache
evicted the line to make room. Until then the display shows the old.

The cache evicts in the order that suits it, not in drawing order. Drawing
for a long time without bringing anything out, the picture appears on the
screen in pieces, jumbled, with gaps filled in later. That is how a gradient
drawn by pixels looked.

So the bus brings out what was written itself:

- at the end of a batch, always;
- during a batch, as soon as pixels and fills have written more than a
  quarter of the cache. Then the picture appears in the order it is drawn.
  With two buffers this is not done: nobody sees the hidden buffer.

Pixels going through a window are not brought out during a batch. They land
in memory one after another, and the cache evicts them in the same order;
there are no gaps. Forcing them out was tried there and cost 9% of the time
of putting the whole screen.

**A pixel on a new row is a cache miss**, about 1.4 µs. A row of 1024 pixels
fills in 0.026 ms, a column of 600 pixels in 0.5–0.8 ms depending on the
cache size. In portrait orientation every fill becomes columns and goes
slowly.

**The L2 cache takes internal RAM**, and its size is chosen at build time by
`CONFIG_CACHE_L2_CACHE_128KB`, `CONFIG_CACHE_L2_CACHE_256KB` and
`CONFIG_CACHE_L2_CACHE_512KB`. The bigger it is, the faster drawing in PSRAM
goes and the less internal RAM is left to the program.

One and the same firmware built with three cache sizes. JD9165 (7"), one
frame buffer, IDF 5.5.4, everything by the processor, no DMA or PPA. The
first row: internal RAM free at start, before the display is started. The
rest: the time of one operation on the display.

| | Cache 128 KB | Cache 256 KB | Cache 512 KB |
| --- | --- | --- | --- |
| Internal RAM free | 588 KB | 458 KB | 197 KB |
| Fill 100x100 | 0.40 ms | 0.18 ms | 0.19 ms |
| Fill the whole screen | 18 ms | 16 ms | 9.5 ms |
| Solid circle of radius 100 | 1.4 ms | 1.2 ms | 0.7 ms |
| Virtual screen 320x100 to the display | 1.7 ms | 1.1 ms | not measured |
| Whole virtual screen to the display | 35 ms | 34 ms | 29 ms |

With the accelerator and the DMA a fill of the whole screen takes 3.5 ms and
putting a virtual screen 8 ms, see "The 2D DMA and the PPA".

### Writing into the frame buffer

A separate bench without a display, JD9165 (7"): a buffer of 1024x600 is
filled from internal RAM; the cells hold the time for the whole buffer.

| Way of writing | Time |
| --- | --- |
| By bytes, with the exchange | 21.9 ms |
| One pixel as a 16-bit value, with the exchange | 23.7 ms |
| Two pixels as a 32-bit word, with the exchange | 17.8 ms |
| `memcpy`, no exchange | 13.7 ms |

Writing as a 16-bit value turned out slower than two bytes: the exchange by
shifts costs more than two moves. Writing by words is faster on long runs
and slower on runs of 1–4 pixels; they are equal at about 16 pixels. So
words are used for runs of 16 pixels and more, and only when the screen is
not mirrored, not rotated, and red and blue do not change places.

An intermediate buffer was tried too: first exchange the bytes in internal
RAM, then send to PSRAM. The exchange takes 9.5 ms, the sending 13.6 ms,
23.1 ms together against 17.8 ms. Exchanging on the way to PSRAM adds only
4.2 ms to the sending.

## The scan

The display reads the frame buffer without end, row after row. Measured on
the JD9165 (7"):

| Quantity | Value |
| --- | --- |
| Frame period | 16418 µs, 60.9 Hz |
| Rows in a frame | 635: 600 visible and 35 of blanking |
| One row | 25.86 µs |
| Reading speed over the visible rows | 77 KB/ms |

On the JD9365 (10") the frame period is 16682 µs, 59.9 Hz.

Where the scan is now can be learned in two ways.

- **By time.** The IDF driver reports the end of a frame; the bus remembers
  that time. On chip revision v1.3 row y is read (35.2 + y) × 25.86 µs after
  the report: 35 is the rows of blanking. Rows 0–4 are read ahead, right
  after the end of frame, so in its calculations the bus allows 6 rows. This
  is how the bus works.
- **By a register.** The read address lies in a register of the DMA
  controller, `DW_GDMA.ch[0].sar0`. Only the bench used it; it is not taken
  into the library, being an internal register.

### How the end of a frame is reported

The report comes in different ways. It depends on the DPI driver of
`esp_lcd` (`esp_lcd_panel_dpi.c`) and on the chip revision. The driver did
not change by major version: the new events came into the release branches
at different times.

| IDF | Events in the DPI driver |
| --- | --- |
| 5.4.3, 5.5.4, 6.0.1, 6.0.2 | only `on_refresh_done` |
| 5.5.5, 6.0.3, 6.1 | `on_frame_buf_complete` and `on_vsync`; `on_refresh_done` declared deprecated |

Other versions were not looked at.

| Event | Chip revision | Where and when it comes from |
| --- | --- | --- |
| `on_refresh_done` | below 3 | DMA interrupt: the last row has been read. Here the driver also chooses the buffer for the next frame |
| `on_refresh_done` | 3 and above | interrupt of the DSI bridge on the vertical sync, after the buffer is chosen |
| `on_frame_buf_complete` | any | DMA interrupt, as in the first row |
| `on_vsync` | any | the vertical sync; on revisions below 3 the same DMA interrupt stands in for it |

The bus uses `on_frame_buf_complete` when it exists, else `on_refresh_done`.
Whether it exists is found at build time by `CMakeLists.txt`, which looks for
the name in the driver header `esp_lcd_mipi_dsi.h`. The IDF version number
cannot tell.

For the second row of the table, `on_refresh_done` on revision 3 and above,
the bus has a separate branch: the rows of blanking before the first visible
one are counted less the front porch, and a safety pause is kept at the
change of buffers, see "The swap". This branch is checked on the
JD9365 (10") under IDF 6.0.1. The branch with `on_frame_buf_complete` on
revision 3 has only been built, under 5.5.5.

## Three strategies

Tearing arises when the scan and the place being written meet on some row
within one frame. On one side of that row the display showed the new, on the
other the old. There is no way to avoid it that suits every program, so the
driver constructor takes a strategy.

```c
dgx_screen_t *scr = dgx_jd9165_init(bus, rst, 16, DgxScreenRGB, DgxDsiTwoFrames);
```

| Strategy | Buffers | Tearing | Cost |
| --- | --- | --- | --- |
| `DgxDsiOneFrame`, one buffer | 1 | possible | none |
| `DgxDsiTwoFrames`, two buffers | 2 | none | one more buffer in PSRAM |
| `DgxDsiOneFrameAfterScan`, behind the scan | 1 | none for a virtual screen put to the display and for a large fill | a wait of up to one frame for each such put and fill |

### One buffer

`DgxDsiOneFrame`. What is drawn is seen at once. No memory beyond the buffer
is needed. Tearing is possible, and the more noticeable the larger the part
of the screen that changes and the more horizontal the motion.

### Two buffers

`DgxDsiTwoFrames`. The IDF driver can keep up to three frame buffers and
switch which of them goes to the display. It allocates them itself; one's
own cannot be substituted. The switch happens at the end of a frame, never
in the middle of one, so there is no tearing by construction.

The cost:

- one more buffer in PSRAM, 1.23 MB at 16 bits;
- a drawing function called without `dgx_screen_progress_up()` is a batch of
  its own, and on the screen it may appear without the one that follows it.
  Everything that must appear together has to be joined in a batch;
- the bus does not keep the pace, see "How a program keeps the pace";
- the refresh rate can only be 60, 30, 20 or 15 a second, depending on how
  many frames a batch takes. This is a property of any strategy without
  tearing: every frame shown is whole. Values in between are possible only
  with tearing;
- after the swap the hidden buffer does not hold what the last batch drew:
  that went into the other buffer. It has to be copied in: the catch-up
  copy, which has a section of its own.

#### The swap

1. Drawing goes into the hidden buffer.
2. At the end of a batch the bus asks the driver to show this buffer from the
   next frame on. The picture is finished but not yet shown.
3. The first drawing of the next batch looks whether the display has taken
   the picture. If it has, the buffers change roles. If not, the batch either
   draws into the same picture or waits for the swap, see below.

The tile maps change places not at the end of a batch but when the buffers
change roles. At the end of a batch the hidden buffer only receives what it
lacks.

The safety pause. Where the end of frame is reported at the moment the
buffer is chosen (`on_frame_buf_complete` on any chip, `on_refresh_done` on
chips below revision 3), a batch finished in the last 2 ms of a frame is
shown from the next frame with no reservations. Where the end of frame is
reported after the buffer is chosen (`on_refresh_done` on revision 3 and
above), such a batch waits one more frame in case the driver has already
chosen the old buffer.

#### Batches of several tasks in one frame

A program may consist of several tasks that know nothing of each other: each
has its own part of the screen, and they draw in turn under one mutex. If
every batch waited for the swap, every swap would go to one task, and three
tasks would get a third of the frames each: 20.4 pictures a second each, see
"Three tasks on the bench".

So the end of a batch does not show the picture; it only asks the driver to
show it from the next frame on. The IDF driver chooses the buffer for the
next frame at the end of the current one and takes the one it was asked for
last. Until the frame ends, the request can be replaced by another. That is
what everything rests on.

Say buffer A is on the screen. The first task has finished a batch in buffer
B, and the bus has asked the driver to show B. Until the end of the frame B
stays hidden. Now a batch of the second task arrives. Where to draw it?

- Not into A: A is on the screen, and half-drawn things would be seen.
- Into B, while it is hidden. But if the frame ends in the middle of
  drawing, the display switches to B and shows the batch half-drawn.

So the bus first removes that danger, then draws:

1. It asks the driver to stay on A. Now the end of frame can spoil nothing:
   A stays on the screen.
2. It checks that the request was not late: no frame ended meanwhile, and
   more than 2 ms remain of the current one. If a frame ended at just that
   moment, the display may already have taken B, and B cannot be drawn into.
   Then the bus asks for B again, waits for it to be on the screen, and the
   batch goes into A, as it would with one task.
3. It draws the batch of the second task into B. The batch of the first task
   is already there.
4. At the end of the batch it again asks the driver to show B.

At the end of the frame the display switches to B, and both tasks are on the
screen at once. A third task, if it makes it before the end of the frame, is
added the same way. Everything several tasks drew within a frame is shown by
one swap of buffers.

What happens to a batch that arrives when the picture is finished:

| What the batch finds | What happens to it | Why |
| --- | --- | --- |
| The display has taken the picture | the buffers change roles, the batch draws at once | nothing to wait for |
| The display has not taken it, enough time remains in the frame | the batch draws into the same picture | this is the joining |
| Little time remains in the frame | it waits for the swap | a batch being drawn at the end of frame holds the whole picture back a frame |
| The picture has already missed one end of frame | it waits for the swap | a finished picture waits no longer than one extra frame |
| A frame ended at the moment the bus was asking the driver about a buffer | it waits for the swap | whether the display took the picture is unknown at that moment |

Details.

- The bus does not ask who draws. A batch of the same task goes into the
  picture the same way as a batch of another.
- The bus does not keep the pace. A program that draws more often than the
  display shows frames gets the last thing drawn in every frame and drew the
  rest for nothing: it has to hold itself back, see "How a program keeps the
  pace".
- The bus does not order the tasks either. If it matters that every task
  gets into every frame, the program sees to it.
- "Enough time" means: more remains of the frame than the longest of the
  recent batches took, plus 2 ms. The bus measures the time of a batch
  itself, from the first drawing to `update_screen()`. A long batch is
  forgotten by an eighth with every following one. There is one estimate per
  bus: a rare long batch among frequent short ones is forgotten in time.
- The 2 ms are room for inaccuracy: the end of frame is predicted from the
  timing of the last one, and the interrupt may have come late.
- The wait for the swap happens inside a drawing function, that is under the
  application's mutex, if there is one.
- Drawing from several tasks at the same time is not possible; the screen is
  not made for it. The mutex remains the application's. What exactly the
  screen keeps, why it takes one task at a time, and how to build a program
  with several drawing tasks is in the README, section
  [Drawing from several tasks](../README.md#drawing-from-several-tasks).

**A known weakness.** If a task is preempted for longer than a frame twice in
a row, exactly between its calls to the driver, the display may go back to
the previous picture for one frame: it took B, the request to stay on A was
late, and the repeated request to show B was late too. Nothing half-drawn is
seen at that.

The bus cannot fix this; a program can make it rare. It is about task
priorities.

- A drawing task is held up for a frame or longer only by a task of higher
  priority that computes for that long without a pause. The drawing task
  needs a priority no lower than such tasks have.
- Tasks of equal priority FreeRTOS alternates by the tick. At a 1 ms tick, as
  in the DGX examples, 16 ready tasks add up to a frame. At the 10 ms tick,
  the IDF default, two are enough.
- If several tasks draw under one mutex, they all need the priority. A
  low-priority task preempted in the middle of a batch holds the mutex, and
  the others wait with it. A FreeRTOS mutex raises its priority to that of
  the waiting task for that time; a semaphore does not.
- The estimate of a batch's time depends on the same: a preempted batch takes
  longer than expected, and the finished picture misses the end of frame.

#### The copy after the swap

Joining batches runs into the catch-up copy. A finished picture must be
whole when the display takes it, and the driver is told of it at the end of
every batch: the bus has no other moment. If everything missing were copied
at the end of the first batch of a frame, for three tasks with bands of
1024x168 that is the bands of the other two tasks, about 590 KB a frame,
357 MB in 10 seconds. A few milliseconds later those tasks redraw their bands
whole, and the copy proves wasted. The first batch of a frame with such a
copy took about 10 ms, and the third no longer fitted in the frame.

The first thought was to copy just before the end of frame, by a timer. But
timer code does not run in the drawing task, and the bus would have needed a
lock of its own between them. Instead the condition itself was relaxed. The
display reads the buffer row by row, and what has to be whole is not the
picture at the moment of the report but each row at the moment the scan
reaches it. The 2D DMA copies twice as fast as the scan reads, so a copy
started in the blanking stays ahead of the scan. It is the same trick as in
the "behind the scan" strategy, applied to the catch-up copy.

How it is arranged.

1. At the end of a batch, large missing pieces are not copied if by the
   measured DMA speed they will certainly make it ahead of the scan. The bus
   remembers them in a list, up to six, and in the tile maps they stay marked
   as missing. Small pieces, and large ones that would not make it, are
   copied at once.
2. If the next batch goes into the same picture and redraws a piece whole,
   it no longer needs copying: at the end of that batch the list is made
   anew.
3. When the display has taken the picture, the end-of-frame interrupt puts
   the first piece into the 2D DMA queue, and the copy-done handler puts the
   next. Pieces go one at a time, from the top down. Hardware copies: there
   is no task and no lock for it.
4. After the swap, drawing goes into the buffer the copy reads from. A batch
   that wants to write over a piece not yet copied waits for that piece.
5. A task waiting for the swap copies the pieces itself if it can finish
   before the end of frame. Then there is nothing to wait for after the swap.

Details.

- **Who owns the list.** Before the report to the driver the bus arms a
  flag. Only one party can take it, an atomic exchange: either the
  end-of-frame interrupt, which then starts the copy, or a task that draws
  into the same picture or copies the pieces itself.
- **The top of the screen.** The display reads the first rows of a frame
  right after the end of the previous one: it reaches row 4 after 0.7 ms.
  There is no staying ahead of the scan there, so for a piece that starts at
  the top edge the first 8 rows are copied at once.
- **Why one at a time.** The DMA queue takes two pieces at once, and each
  goes half as fast: 83 KB/ms against 77 KB/ms of the scan.
- **A put by the DMA and a fill by the PPA** started by a task during the
  copy slow it down the same way. The bus works out in advance whether the
  pieces will stay ahead of the scan at that speed. If so, only the one who
  writes over a piece waits. If not, the put and the fill wait for the whole
  copy to end.
- **The cache.** The DMA writes to memory past the cache, so before the
  report to the driver the bus brings out and drops the lines under the
  pieces. This costs 8–19 µs a piece at any size.
- **Reading.** A pixel read from the finished picture where a piece is not
  yet copied brings in its tile by the processor, like any missing one.
- **Which piece will make it.** A piece starts when the pieces above it are
  done. It must start before the scan reaches its first row and finish
  before the scan passes its last, by the measured speed with a quarter of
  room. Until the speed is measured, all pieces are copied at once.
- **24 bits.** A row there is one and a half times longer in bytes, the scan
  reads bytes one and a half times faster, and the DMA copies as it did.
  Pieces fell behind the scan even with the check: 1 to 5 in 300 batches. So
  at 24 bits the copy after the swap is off, and everything is copied at the
  end of the batch.
- **A safeguard.** A piece not copied within a quarter of a second is copied
  by the task, and a warning stays in the log. It never fired on the bench.
- **Memory.** The list of pieces, their descriptors and transfers take
  0.85 KB of internal RAM, only with two buffers and only when the 2D DMA
  is built, see `CONFIG_DGX_DSI_PRIVATE_DMA2D`.

What it gives. JD9165 (7"), two buffers, IDF 6.0.1; the cells hold the
working time of one batch from the first drawing to the return from
`update_screen()`, with no waiting for the display, under the two ways of
the catch-up copy.

| Batch | Everything copied at the end of the batch | Large pieces copied after the swap |
| --- | --- | --- |
| Square 48x48 after a batch that filled the whole screen | 8.49 ms | 0.56 ms |
| Band of 200 rows, even batches in one place, odd in another | 5.49 ms | 2.79 ms |
| 320x100 from a virtual screen, likewise in two places | 1.09 ms | 0.55 ms |
| Column 40x548, likewise in two places | 1.03 ms | 0.28 ms |
| Square 64x64, likewise in two places | 0.28 ms | 0.15 ms |

#### What the bus measures itself and what is a fixed number

| Quantity | Where it comes from |
| --- | --- |
| Speed of the 2D DMA copy | measured on every deferred piece of 64 KB and more copied without interference; the slowest of the recent ones counts. On the JD9165 (7") 117 to 170 KB/ms, counter `copy_kb_a_ms`. Until a piece has been measured, the number from the board measurement is used, if the program gave one |
| Speed of the scan | from the frame period the bus measures and the rows of a frame including blanking |
| Time of a batch | measured on every batch |
| How much a second transfer slows the copy | fixed: 2.5 times. Measured: twice |
| How many rows the display reads ahead | fixed: 6 |
| Room for starting the copy | fixed: 0.4 ms. Measured 16–37 µs. The board measurement gives 0.12–0.15 ms |
| Room before the end of frame | fixed: 2 ms |
| The thresholds from which a piece goes by the DMA and the PPA, and the processor's writing speeds in the "behind the scan" strategy | fixed numbers taken on the JD9165 (7"). The board measurement takes them anew |

The fixed numbers were taken on the JD9165 (7"): ESP32-P4 revision 1.3,
PSRAM at 200 MHz, a 1024x600 panel. The bus itself adjusts only the first
three. The thresholds, the writing speeds and the room for starting the copy
for another chip, memory or display are taken by `dgx_dsi_bench()`, see
"Measuring the board". Three numbers of this table nobody measures: the
slowing by a second transfer, the rows the display reads ahead, and the room
before the end of frame.

#### Counters

How many pictures a program made says nothing about what the display showed:
behind the scan the bench refreshed a band 487 times a second at 60.9 frames.
So the bus counts. `dgx_dsi_counters()` gives the counters as they are; the
program takes the difference over a second.

| Counter | What it counts |
| --- | --- |
| `frames` | frames sent to the display. Counted with two buffers and behind the scan |
| `batches` | batches in which something was drawn |
| `flips` | swaps of buffers, that is frames that showed something new |
| `joined` | batches that went into a picture already finished |
| `kept_back` | ends of frames a finished picture missed because of a batch being drawn into it |
| `flip_waits`, `flip_wait_us` | how many times a batch waited for the swap and how many microseconds in all |
| `scan_waits`, `scan_wait_us` | behind the scan: how many times a write waited for the scan and how many microseconds in all |
| `copied_bytes` | bytes of the catch-up copy, in all |
| `copied_late_bytes` | of them, copied after the swap, ahead of the scan |
| `copied_late_behind` | pieces of such a copy that finished after the scan had passed their last row. Should stay zero |
| `copy_waits`, `copy_wait_us` | how many times drawing waited for such a piece and how many microseconds in all |
| `copy_kb_a_ms` | not a counter: the speed of the 2D DMA copy in KB/ms as the bus measured it. 0 until measured |

All counters are 32-bit and wrap. Bytes written by the processor, the DMA
and the PPA are not counted.

#### The time of a frame

More pictures a second than the display has frames cannot be shown. How long
a frame takes the bus tells: `dgx_dsi_frame_us()` gives microseconds, on the
JD9165 (7") 16417, that is 60.9 frames a second. When the last frame ended
`dgx_dsi_frame_ended()` tells. From these two numbers a program keeps the
display's pace itself, see "How a program keeps the pace".

Where the number comes from, JD9165 (7"):

| When | What is given | Value |
| --- | --- | --- |
| Right after the display starts | a calculation from the panel timings: pixels of a line including blanking, times rows of a frame including blanking, divided by the pixel clock | 1344 × 635 / 52 MHz = 16412 µs |
| After 32 frames, half a second | the time of 16 frames in a row divided by 16 | 16417 µs |
| After 256 frames | the time of 256 frames in a row divided by 256. Renewed every 256 frames | 16417 µs |

The calculation differs from the measurement by 0.03%: the real pixel clock
is slightly below the one asked for. The measurement was checked by the
`frames` counter: 600 frames took 9850437 µs, 16417.4 a frame.

The bus counts frames with two buffers and behind the scan. With one buffer
it does not, and the number stays calculated.

What this does not give.

- Precision better than a microsecond. 16417 instead of 16417.4 drifts by one
  frame in 11 minutes: once in 11 minutes two pictures of the program fall
  into one frame, and the display does not show the first of them.
- Waiting. The bus tells when a frame ended, but it neither waits for the end
  of frame itself nor wakes the program. The program sleeps on its own.

#### Evenness

A picture moves evenly when each next one comes after the same number of
frames. The bench counts this for every source: how many frames of the
display passed from one of its pictures to the next.

JD9165 (7"), two buffers, 10 seconds a scene, the tasks keep the pace by
their own clocks. The cells: how many times the next picture came after one,
two and three frames; at 60.9 frames a second a row adds up to about 305 for
a picture every other frame and about 610 for a picture every frame.

| Source | Pictures a second | After 1 frame | After 2 | After 3 | By eye |
| --- | --- | --- | --- | --- | --- |
| Scene 3, columns by primitives | 34.8 | 86 | 262 | 0 | a slight jerk |
| Scene 7, top band, step 2 pixels | 40.0 | 192 | 206 | 2 | even |
| Scene 7, middle band, step 3 pixels | 42.5 | 243 | 183 | 0 | slightly uneven |
| Scene 7, bottom band, step 4 pixels | 40.0 | 196 | 199 | 5 | slightly uneven |
| Scene 11, each of the three light bands | 60.9 | 609–610 | 0 | 0 | even |
| Scene 12, top band | 58.5 | 562 | 24 | 0 | a slight jerk |
| Scene 12, middle band | 39.0 | 179 | 203 | 8 | a slight jerk |
| Scene 12, bottom band | 30.8 | 20 | 275 | 13 | a slight jerk |
| Scene 14, columns by primitives every other frame | 30.5 | 39 | 227 | 38 | fine |
| Scene 13, top band every other frame | 30.5 | 17 | 271 | 17 | fine |
| Scene 13, middle band | 30.4 | 0 | 304 | 0 | fine |
| Scene 13, bottom band | 30.4 | 66 | 171 | 67 | a slight jerk |

- It is uneven where the source cannot make a picture a frame: gaps of one
  and of two frames come mixed. The bus chooses nothing here; it shows a
  finished picture in the nearest frame.
- The top band of scene 7 comes as mixed as the lower ones yet looks even:
  its columns move 2 pixels a picture, the lower ones 3 and 4.
- A program can even it out: ask for a picture not every frame but every
  second one, that is count time by twice `dgx_dsi_frame_us()`. Scenes 13
  and 14 do that.
- It does not come out perfectly even that way. In the bottom band of scene
  13 gaps of one and of three frames occur 66 times each out of 304, and it
  shows. Its batch finishes right at the end of a frame and lands now in this
  frame, now in the next. That is the phase the program does not know.
- The phase differs from run to run: it depends on the moment within a frame
  the scene started. In another run scene 14 had all gaps of two frames, and
  the bottom band of scene 13 had 46 uneven ones of each kind.

#### How a program keeps the pace

A program has to limit itself, and sleeping for the time of a frame is not
enough: where within the frame its picture ends is left to chance. The
picture has to be aimed: finished shortly before the end of a frame. The
program has everything for that.

1. It measures the time of its own picture: from the start of drawing to the
   end of the batch. It takes the longest of the recent ones.
2. It chooses the frame the picture should land in: the next after the last
   one, or the one after that if the picture does not fit in a frame with
   room to spare.
3. It starts "the time of the picture plus 4 ms" before the end of that
   frame. It knows the ends of frames: the end of the last one plus a whole
   number of frame times.

```c
int64_t ended;
if (dgx_dsi_frame_ended(bus, &ended)) {
    /* finish 4 ms before the end of the next frame */
    int64_t from = ended + dgx_dsi_frame_us(bus) - 4000 - picture_takes_us;
    while (esp_timer_get_time() < from) vTaskDelay(1);
}
```

In full, with the choice of frame and the answer to being late, this is the
function `demo_pace()` of the `dsi_demo` example, fifteen lines or so. With
`DgxDsiOneFrame` the bus does not count frames, the call returns `false`,
and the pace is kept by the clock.

Three ways of keeping the pace compared on the bench. JD9165 (7"), two
buffers, 10 seconds a scene. The cells: how many times the next picture of
the source came after one, two and three frames. Where the three tasks
differ, a range is given.

| Source | Way | After 1 frame | After 2 | After 3 |
| --- | --- | --- | --- | --- |
| One task, columns by primitives, a picture every other frame (scene 14) | by its own clock | 76 | 153 | 75 |
| | start right after the end of frame | 76 | 154 | 74 |
| | finish 4 ms before the end of frame | 0 | 302 | 1 |
| Three tasks, parts 320x100, a picture a frame (scene 9) | by their own clocks | 609 | 0 | 0 |
| | start right after the end of frame | 609 | 0 | 0 |
| | finish 4 ms before the end of frame | 583–608 | 0–13 | 0 |
| Three tasks, light bands, a picture a frame (scene 11) | by their own clocks | 608 | 0–1 | 0 |
| | start right after the end of frame | 589–596 | 7–10 | 0 |
| | finish 4 ms before the end of frame | 431–442 | 83–87 | 0–2 |
| Three tasks, heavy bands, every other frame (scene 13) | by their own clocks | 2–67 | 170–300 | 2–67 |
| | start right after the end of frame | 51–72 | 160–202 | 51–72 |
| | finish 4 ms before the end of frame | 35–48 | 193–224 | 42–59 |

- One task. By the clock, evenness is luck: in four runs of the same scene
  the uneven gaps numbered 1, 3, 77 and 151 out of 304. Starting right after
  the end of frame is reliably bad: the picture of this scene takes
  17–18 ms, about a frame, and its end lands on the end of frame by itself.
  Aiming the end of the picture gives exactly two frames between pictures.
- Several tasks that know nothing of each other and draw under one mutex.
  Aiming the end does not suit them: all aim at the same moment, yet draw in
  turn, and the wait for the mutex becomes part of the picture's time. The
  light bands then make 52 pictures a second instead of 61. As long as
  everything together fits in a frame, such tasks had better start right
  after the end of frame or keep the pace by the clock.
- The heavy bands do not fit in two frames with room to spare, and no way
  makes them even.
- The recipe is for a program where one task draws the whole picture.
  Several sources reduce to that case, see below.
- By eye two scenes were looked at: the three light bands starting after the
  end of frame run fine; the three heavy bands by the clock look better than
  before.

#### One drawing task and messages

A mutex protects data, and here shared data can be done without: one task
draws, the others send it messages. On the bench these are scenes 23–26.
Each source redraws its own virtual screen, posts a "ready" message to a
queue and leaves the screen alone until it is answered that the picture has
been taken. The display task takes everything that has arrived from the
queue, puts it in one batch, and aims the end of that batch 4 ms before the
end of frame, like any single drawing task. Its priority is above the
sources'.

JD9165 (7"), two buffers, 10 seconds a scene. The cells: how many times the
next picture of the source came after one, two and three frames.

| Three sources | Arrangement | After 1 frame | After 2 | After 3 |
| --- | --- | --- | --- | --- |
| Parts 320x100, a picture a frame | each draws itself under a mutex, pace by the clock | 609 | 0 | 0 |
| | messages to one drawing task | 607 | 0 | 0 |
| Light bands, a picture a frame | under a mutex | 608 | 0–1 | 0 |
| | messages | 604–606 | 0–1 | 0 |
| Heavy bands, every other frame | under a mutex | 1–53 | 198–302 | 1–53 |
| | messages | 1 | 302 | 0 |
| Heavy bands, a picture a frame | under a mutex | 192–243 | 183–206 | 0–5 |
| | messages | two at 0–2, the third at 196 | two at 302–303, the third at 205 | 0 |

- The heavy bands every other frame became even for all three sources: 302
  gaps out of 303 are two frames. Under a mutex no way gave that.
- There are no waits for the display at all, and batches are not joined
  because there is nothing to join: one batch comes a frame. The catch-up
  copy over 10 seconds is 0.5 MB for the light bands.
- The last row shows the limit. The source cannot make a picture a frame,
  and there are no miracles: two sources that share a core settled on an even
  two frames by themselves, while the third, alone on its core, makes 40
  pictures a second and comes mixed. The pace for such sources is the
  program's choice: every other frame, as in the row above.
- This arrangement avoids priority inversion. The sources do not wait for
  each other; each waits only for the drawing task. That is why it is given a
  priority above the sources: with a lower one it would itself become what a
  more important task waits for.
- So the question of how several sources limit themselves reduces to one
  drawing task. The variant with a mutex still works, but its evenness
  depends on how the tasks share the time.

#### Three tasks on the bench

Scenes 7–12: three tasks, each counting how many pictures a second it put to
the display. JD9165 (7"), two buffers, IDF 6.0.1. The cells hold pictures a
second of the first, second and third task under three arrangements of the
bus: every batch waits for the swap (as it was before 0.5.3); batches are
joined in a frame, the catch-up copy at the end of the batch; batches are
joined, large copies after the swap (the current code).

| Scene | A batch a frame | Joined, copy at the end of the batch | Joined, copy after the swap |
| --- | --- | --- | --- |
| 7. Three bands, at the display's pace | 20.4, 20.3, 20.3 | 34.8, 35.2, 34.8 | 40.2, 41.9, 40.1 |
| 8. The same with pauses | 20.4, 20.3, 20.2 | 37.0, 30.6, 24.2 | 42.1, 30.6, 23.3 |
| 9. Three parts, at the display's pace | 20.4, 20.4, 20.4 | 61.0, 61.0, 61.0 | 61.0, 61.0, 61.0 |
| 10. The same with pauses | 20.4, 20.4, 20.3 | 61.0, 60.9, 40.6 | 121.8, 60.9, 45.5 |
| 11. Three light bands, at the display's pace | 20.4, 20.3, 20.3 | 40.6, 40.8, 40.5 | 60.6, 60.8, 60.5 |
| 12. The same with pauses | 20.4, 20.4, 20.0 | 47.8, 33.2, 30.2 | 59.2, 39.5, 30.8 |

What is behind the numbers.

- While every batch waited for the swap, every swap went to one task, hence
  20.4 for all in every scene.
- Scene 11 shows the limit of the bus: all three full-width bands get into
  every frame. Its counters over 10 seconds: 615 frames, 611 of them showed
  something new, 1825 batches, 1214 of them went into a finished picture, not
  one end of frame missed. The catch-up copy is 0.7 MB against 411 MB with
  the copy at the end of the batch, nearly all of it after the swap: nearly
  always all three tasks had time to redraw their own.
- Scenes 7 and 8 are limited not by the bus but by the redrawing itself: the
  columns in PSRAM are drawn by the processor, and two of the three tasks
  share one core.
- Scene 10: the first task sleeps 7 ms and draws 121.8 pictures a second, two
  a frame. The bus does not hold it back, and every second picture of it
  never reaches the screen. The variant with the copy at the end of the batch
  still held back a task that draws twice in one frame, hence 61.0 in the
  middle column.
- In scenes 10 and 12 the third task sleeps 21 ms after every picture and
  cannot draw more often than 46 a second.
- In no scene did a piece fall behind the scan: the `copied_late_behind`
  counter stayed zero.

Scenes 1–6, where one task draws, did not change with the joining: 42.3,
60.9, 34.8, 60.9, 60.9 and 60.9 frames a second against 42.7, 60.9, 34.7,
60.9, 60.9 and 60.8 with a batch a frame.

By eye, two buffers with the current code: scenes 1, 2, 4–6 and 8–10 look
fine; in scene 11 all three bands run evenly. Scene 3 jerks slightly; in
scene 7 the top band runs evenly and the two lower ones slightly unevenly;
scene 12 jerks slightly. Where this comes from, see "Evenness".

Correctness check. A model of the display on the host,
`test/host/test_fb_flip.c`: the display there takes at the end of a frame the
buffer it was asked for last, tasks draw batches in turn, every call takes a
random time, and the end of frame may fall on any of them. It is checked that
a frame never starts with the buffer being drawn into, that with one task
without pauses every frame shows something new, and that with three tasks
batches of each get into every frame. Tile maps with pieces left for later
are checked by `test/host/test_fb_tiles.c`. On the board: random batches
against a reference, drawn by three tasks in turn, see "How it was checked".

### Behind the scan

`DgxDsiOneFrameAfterScan`. One buffer, no memory beyond it. The idea is to
write when the scan is not in the way. It is a compromise: a picture that
comes from a virtual screen is shown without tearing, while drawing right
on the display is protected only in part.

It works in three ways, depending on what does the writing.

**A put by the 2D DMA.** This is how an area of a virtual screen of 16 KB and
more goes on an unrotated screen, see "The 2D DMA and the PPA". The DMA
writes at 150 KB/ms, the scan reads at 77 KB/ms: writing is twice as fast as
the scan. So it is enough to start when the scan is outside the rows of the
area. If it is above the area, it enters later than the write and does not
catch up. If below, all the more. Nor can the write overtake the scan: it
does not start behind it.

Before starting the DMA the bus waits for the scan to leave the rows of the
area, then starts it. For the whole screen "outside the area" is the 35 rows
of blanking, 0.9 ms a frame: the bus waits for them and puts the whole screen
in 7.9 ms, staying ahead of the scan. So an area of any size, the whole
screen included, goes without tearing on one buffer.

**A fill by the accelerator.** A large rectangle is filled by the PPA
accelerator, four times faster than the scan. The rule is the same: the bus
waits for the scan to leave the rows of the rectangle and starts the fill.
The rectangle appears whole.

This has a consequence. A tall rectangle takes nearly all rows of the screen,
and its fill can be started only in a short gap while the scan is in the
blanking and in the rows above the rectangle. If a batch has many
rectangles, they do not fit in one gap: some are filled in this frame, the
rest in the next. The batch stops being one whole.

On the bench this is scene 3, columns by primitives: 18 fills 548 rows tall a
batch, about ten fit in one gap. A column whose strip on the left is already
erased while the one on the right is not yet drawn is shown narrower for one
frame. By eye this shows as a jerk in the width of the running column. There
is no tearing at that. What must appear at once has to be drawn in a virtual
screen and put in one piece, or two buffers taken.

**Writing by the processor through a window.** This is how areas under
16 KB, text, textures, 24 bits and any put on a mirrored or rotated screen
go. The processor writes slower than the scan, so the rule here is
different. Before writing pixels into a window the bus takes four steps.

1. It works out how long the write will take: the bytes of the window
   divided by the writing speed.
2. From the time since the end of frame it works out where the scan is now.
3. If the scan is in the rows of the window, or will enter them before the
   write ends, it waits for it to leave them.
4. It writes.

At the end of a batch what is left in the cache is brought out the same way,
waiting for the scan.

Step 3 allows writing only in the gap when the scan is not in the rows of the
window: from its leaving the last row to its entering the first. If the write
does not fit in that gap, the bus does not wait at all and writes at once, as
in `DgxDsiOneFrame`. The writing speed is in the code with room to spare,
28 KB/ms; measured over the whole screen it is 36 KB/ms. A full-width band of
up to 165 rows fits. For another board `dgx_dsi_bench()` takes this speed: on
the JD9165 (7") it gets 34 KB/ms and hands the bus a quarter less, 24–25.

This condition is stricter than needed. The scan and the write go the same
way, and writing could start as soon as the scan has passed the first row of
the window: it would get in the way only in the next frame, when it comes
back from the top and catches up with the write.

| | As in the code | What would suffice |
| --- | --- | --- |
| Writing starts | no earlier than the scan has left the last row of the window | as soon as the scan has passed the first row of the window |
| Writing must end | before the scan enters the first row | before the scan catches it up in the next frame |
| Time for writing | the period less the scan's time in the window | the period plus the scan's time in the window |
| For a band of 200 rows | 16.4 − 5.2 = 11.2 ms | 16.4 + 5.2 = 21.6 ms |
| For the whole screen | 16.4 − 15.5 = 0.9 ms | 16.4 + 15.5 = 31.9 ms |

The condition in the right column is not done. It suits only where the order
of writing matches the order of the scan: the normal orientation and the
horizontal mirror. In the normal orientation large areas go by the DMA, so
the gain would be left for 24 bits and the horizontal mirror only.

**What is not held back.** Single pixels, lines and small fills drawn right
on the display go at once, as in `DgxDsiOneFrame`.

**The pace.** Waiting for the scan sets no pace. If the scan is elsewhere,
the wait ends at once, and a program can refresh the same band several times
a frame: on the bench, without a pause of its own, a band of 80 rows was
refreshed 487 times a second, and the picture blurred. The program must
itself refresh no more often than the display shows frames. With two buffers
the bus does not keep the pace either, but the picture does not blur from
it: the last thing drawn by the end of frame is shown.

**What came of it.** Three areas, each drawn in a virtual screen and put to
the display once a frame, the picture in it moving sideways. JD9165 (7"), one
frame buffer; the cells say whether tearing is seen by eye.

| What is put | One buffer, no waiting for the scan | Behind the scan, processor writes | Behind the scan, DMA writes |
| --- | --- | --- | --- |
| Band of 80 rows | tears | does not tear | does not tear |
| Band of 200 rows | tears | tears | does not tear |
| Whole screen | tears | tears | does not tear |

The processor has time to write behind the scan only a band of 80 rows. A
band of 200 rows and the whole screen it cannot write within one pass of the
scan, so the bus does not hold them back, and they tear as without waiting.
The DMA makes everything.

The bench does not see tearing; it counts something else: before and after
each put it reads from the DMA register which row the display is reading and
notes whether the scan was in the rows of the area while the put went on.
JD9165 (7"), one buffer; the cells: in how many puts out of 100 the scan was
in the area.

| What is put | No waiting for the scan, processor writes | Behind the scan, DMA writes |
| --- | --- | --- |
| Band of 80 rows | 13 of 100 | 6 of 100 |
| Band of 200 rows | 42–44 of 100 | 8 of 100 |
| Whole screen | 100 of 100 | 0 of 100 |

These numbers read differently.

- The processor writes slower than the scan. If the scan was in the area
  during the put, it went through the place being written, and that is a
  tear. The first column is the share of puts with tearing.
- The DMA writes twice as fast as the scan and starts when the scan is not in
  the area. If the scan entered the area later, the write is already ahead of
  it, and it reads only the new. The second column does not mean tearing. It
  shows that the scan enters a band seldom, and the whole screen never: the
  put starts in the blanking.

**What was tried and did not suit.**

- Holding a batch in the cache until the scan has gone and bringing it out at
  once. The virtual screen the put comes from lies in the same PSRAM and
  shares the same cache: the batch is evicted sooner.
- Holding back only the end of a large batch. The batch tears in two places
  instead of one and looks worse.

## The catch-up copy

With two buffers, after the swap the hidden buffer does not hold what the
last batch drew: that went into the other buffer. Bringing it into the
hidden buffer is the catch-up copy. The less of it, the faster a batch.

### The cost of copying

Everything rests on what it costs to move a piece from one PSRAM buffer to
another: from one frame buffer into the other, or from a virtual screen into
a frame buffer. JD9165 (7"), two buffers of 1024x600 in PSRAM, display
running, IDF 5.5.4; the cells hold the time of copying the area, including
bringing it out of the cache.

| Area | Bytes | Processor, `memcpy` | Processor, with byte exchange | 2D DMA | 2D DMA, with byte exchange |
| --- | --- | --- | --- | --- | --- |
| 1024x600 | 1228800 | 27.1 ms | 30.3 ms | 8.1 ms | 7.8 ms |
| 1024x200 | 409600 | 9.1 ms | 10.4 ms | 2.75 ms | 2.67 ms |
| 1024x80 | 163840 | 2.35 ms | 3.1 ms | 1.14 ms | 1.11 ms |
| 1024x32 | 65536 | 1.44 ms | 1.79 ms | 0.47 ms | not measured |
| 320x100 | 64000 | 1.27 ms | 1.54 ms | 0.48 ms | 0.50 ms |
| 60x100 | 12000 | 0.40 ms | 0.44 ms | 0.15 ms | not measured |
| 32x32 | 2048 | 0.096 ms | 0.103 ms | 0.066 ms | not measured |
| 32x600 | 38400 | 1.57 ms | 1.67 ms | 0.53 ms | not measured |

What this shows.

- The processor copies large areas at 44 KB/ms. The scan reads at 77 KB/ms,
  nearly twice as fast.
- While an area fits in the cache the copy goes faster: 1024x80 at 68 KB/ms.
- Small pieces are dear: a 32x32 area goes at 20 KB/ms, because each of its
  rows is a cache miss both in the source and in the destination.
- A running display takes about a fifth of the speed: without it the buffer
  copies in 22.8 ms.
- From internal RAM into PSRAM the processor copies at 72 KB/ms without the
  exchange and 57 KB/ms with it, 1.6 times faster than from PSRAM. But a
  whole buffer does not fit in internal RAM.
- The 2D DMA copies at 150 KB/ms, three to four times faster than the
  processor and twice as fast as the scan. The byte exchange costs it
  nothing. The processor is free meanwhile.
- The DMA figures are from a bare bench. In the bus one operation costs more,
  about 0.2 ms over the copying, see "The 2D DMA and the PPA".

### Tiles

The first variant copied one rectangle enclosing everything drawn in the
previous batch. One line across the diagonal of the screen gave a rectangle
of the whole screen and a copy of 27 ms. A list of rectangles is no rescue
from the diagonal: a line is drawn by pixels, and a list of any reasonable
length overflows.

It is done with tiles. The panel is divided into tiles of 32x4 pixels, one
bit a tile. There are two maps. Each has a 32-bit word per row of tiles; for
1024x600 that is 150 words, 1200 bytes together. If the panel is wider than
1024 pixels, the tile is made wider so that a row fits in a word.

| Map | What is marked in it |
| --- | --- |
| `owed` | tiles the hidden buffer lacks: the previous batch drew in them, and it drew into the other buffer |
| `fresh` | tiles the current batch draws in |

### When a tile is copied

The task is one: before the hidden buffer is shown, bring into it from the
shown one all tiles in `owed`, copying as little as possible on the way. So
the copy is put off to the last moment. A tile in `owed` is copied only when
there is no way around it.

| What happens to a tile in `owed` | What is copied | Why |
| --- | --- | --- |
| A pixel is drawn in it | the whole tile, before the drawing | its other pixels must be right |
| A pixel is read from it | the whole tile, before the reading | otherwise stale data is read |
| A fill or a window covers it whole | nothing | all its content is written anew |
| A fill or a window covers part of it | only the part of the tile outside the rectangle | the rest is written anew |
| The batch has ended and it was not touched | the whole tile, before the swap; large pieces after it, see "The copy after the swap" | this buffer is about to be shown |

After the copy the tile's bit in `owed` is cleared, so within a batch a tile
is copied at most once.

When everything is copied, the buffers change roles. The `fresh` map, where
everything this batch drew is marked, becomes the `owed` map for the other
buffer, and a new `fresh` starts empty.

An example: a line across the diagonal of the screen that moves every batch.
The line passes through about 180 tiles of 4800. The next batch erases it by
pixels: these are the same tiles, each copied at its first pixel. The new
line falls partly into the same tiles, partly into neighbours that need no
copy. About 50 KB is copied instead of 1.2 MB; the batch takes 2 ms instead
of 21 ms.

**Neighbouring tiles.** When a tile is copied because of a pixel or a read,
the `owed` tiles adjoining it in the same row are copied with it. One long
piece costs about half of the same tiles one by one, and when drawing by
pixels the neighbouring tiles are usually needed next.

**An unfinished window.** A fill always writes its whole rectangle. A window
may not be written out: `texture.c` sets a window down to the bottom of a
shape, writes as many rows as matched in width, then sets the next. So the
bus does not count a window written until it has made sure. This takes three
steps.

1. The first pixel arrives in the window. The bus copies only what lies
   outside the window in the window's tiles. The tiles themselves stay in
   `owed`.
2. Pixels come. The bus counts them.
3. The window closes: another command came or other drawing started. From
   the count the bus knows how far the pixels got. Everything they did not
   reach is copied from the shown buffer, to the pixel. The window's tiles
   are removed from `owed`.

If the window was written to the end, nothing is copied at step 3: that is
how a virtual screen goes. If writing into the window did not start at its
first pixel, it counts as partial, and its tiles are copied whole, as for a
pixel.

### The size of a tile

A tile 32 pixels wide is 64 bytes, exactly one cache line. Narrower makes no
sense. The height was chosen by measurement, in two rounds.

**First round: 32x32 against 32x8.** JD9165 (7"), two buffers, IDF 5.5.4,
everything by the processor; the cells hold the working time of one batch,
drawing together with the catch-up copy, no waiting for the display.

| What the batch draws | Tiles 32x32 | Tiles 32x8 |
| --- | --- | --- |
| 2000 single pixels scattered over an area of 800x400, new places every batch | 16.65 ms | 7.30 ms |
| A virtual screen of 1024x200 put to the same place, rows 200 to 399 | 13.76 ms | 12.69 ms |
| A line across the diagonal: erase the old, draw the moved one | 2.30 ms | 1.95 ms |
| A gradient of 256x100, pixel by pixel, in one place | 14.81 ms | 14.40 ms |
| Tiles on 1024x600 | 608 | 2400 |
| Memory for two maps | 152 bytes | 600 bytes |

Why the low tile is faster.

- A tile hit by even one pixel is copied whole. A 32x32 tile is 1024 pixels,
  a 32x8 one 256. On scattered pixels four times less is copied, hence the
  first row.
- The band's edges lie on rows 200 and 399. These are boundaries of tiles 8
  high, and the band covers its tiles whole: nothing to copy. The boundaries
  of tiles 32 high run along rows 192 and 416, and the edge tiles are covered
  only in part: 8 rows above the band and 16 below it are copied every batch.
- On the line and the gradient the difference is small: nearly the same
  pixels are copied there.

The low tile costs 448 bytes of internal RAM; it was chosen then.

**Second round: from 32x8 to 64x1.** A tile of one cache line, 32x1, suggests
itself: 64 bytes a pixel are copied instead of 512. The code is the same; the
size is set by `dgx_dsi_tiles()`. JD9165 (7"), two buffers, 16 bits, IDF
6.0.1, DMA and PPA on. The first eight rows hold the working time of one
batch in milliseconds; the last three hold something else, said in the row.

| What the batch draws | 32x8 | 32x4 | 32x2 | 32x1 | 64x1 |
| --- | --- | --- | --- | --- | --- |
| 2000 single pixels over an area of 800x400 | 7.50 | 5.39 | 3.70 | 2.86 | 3.38 |
| A line across the diagonal: erase and draw | 1.86 | 1.94 | 2.16 | 2.46 | 2.71 |
| Nine columns 548 high move by 4 pixels (scene 3) | 17.07 | 18.43 | 21.02 | 25.49 | 31.70 |
| Two squares of 48x48 | 0.29 | 0.28 | 0.30 | 0.34 | 0.37 |
| Fill of the whole screen | 3.58 | 3.65 | 3.79 | 4.04 | 4.03 |
| Gradient 256x100 by pixels | 15.95 | 15.91 | 15.92 | 15.06 | 15.19 |
| Band of 200 rows from a virtual screen from row 213 | 4.07 | 3.99 | 3.99 | 3.97 | 3.97 |
| 320x100 from a virtual screen, the place jumps by 10 pixels | 1.10 | 1.09 | 1.11 | 1.16 | 1.35 |
| Scene 3, frames a second | 34.9 | 34.8 | 34.8 | 30.5 | 23.9 |
| Scene 11, catch-up copy over 10 seconds, MB | 20.2 | 1.2 | 0.3 | 0.7 | 1.2 |
| Internal RAM for the bus and the screen, bytes | 7484 | 8092 | 9308 | 11740 | 11740 |

What the scenes show.

- On scattered pixels, the lower the tile the faster: 32x1 is 2.6 times
  faster than 32x8.
- Everything else is slower with a one-row tile: the line by a third, the
  narrow columns one and a half times, the screen fill by 13%. As many bytes
  are copied, but in one-row pieces. Every row of tiles that drawing touched
  costs about 0.9 µs: walking the map and calling the copy. A column 548
  high has 548 such rows instead of 69.
- Scene 11 copies tens of times more with the 32x8 tile than the rest. The
  bench's bands start at rows 60, 240 and 420: boundaries of tiles 4 high but
  not 8 high, and for each band 4 rows above and 4 below are copied every
  batch. That is the bench's geometry, and it did not affect the frame rate:
  60.9 with any tile.
- A map is a word per row of tiles: 32x4 takes 0.6 KB more than 32x8, 32x2
  1.8 KB more, 32x1 4.2 KB more. 64x1 uses half the bits of a word and takes
  as much memory.

**The demos.** Nine demos from `examples/` were run on the JD9165 (7") as
they are: the bench's bus and screen were put into the demo source, nothing
else touched. Added to them was matrix text drawn right on the display: a
clock line in Verdana 32, a title in Terminus 12 at four times the size, a
log of 18 lines in Terminus 12 where one line is rewritten every frame, and
two lines of small text in Arial 12 and Droid Sans 9; before writing, a line
is erased with a rectangle. Each demo runs 25 seconds. Two buffers, IDF
6.0.1; the cells hold the processor time the demo task spends on one batch,
in milliseconds, without waiting for the display and the demo's own pauses.

| Demo | 32x16 | 32x8 | 32x4 | 32x2 | 32x1 | 64x8 | 64x1 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `glyph_morph_demo`, morphing glyphs | 26.75 | 26.81 | 26.95 | 27.07 | 27.39 | 26.84 | 27.41 |
| `weather_morph_demo`, morphing weather glyphs | 42.83 | 42.57 | 42.67 | 42.84 | 43.22 | 42.66 | 43.26 |
| `morph_demo`, a word morphed letter by letter | 1.446 | 1.423 | 1.441 | 1.453 | 1.499 | 1.554 | 1.605 |
| `life_morph_demo`, Life | 47.96 | 47.70 | 47.87 | 48.05 | 48.34 | 49.48 | 49.98 |
| `hw_font_demo`, handwritten text by a pen | 0.060 | 0.060 | 0.062 | 0.066 | 0.073 | 0.061 | 0.074 |
| `hw_morph_demo`, morphing handwritten text | 45.03 | 44.72 | 44.71 | 44.71 | 44.85 | 44.72 | 44.83 |
| `flip_clock_demo`, a flip clock | 0.500 | 0.494 | 0.496 | 0.510 | 0.559 | 0.495 | 0.559 |
| `texture_demo`, textures | 2.193 | 2.190 | 2.190 | 2.206 | 2.242 | 2.190 | 2.242 |
| `screen_demo`, primitives | 0.607 | 0.598 | 0.602 | 0.625 | 0.654 | 0.660 | 0.740 |
| Matrix text | 7.63 | 6.07 | 5.72 | 5.75 | 5.78 | 6.65 | 6.06 |

The 32x8 tile was run twice, first and last in the series: the numbers agreed
to 0.3%.

What the demos show.

- On all nine demos 32x8 is the fastest or ties with 32x16. The lower the
  tile the slower, but not by much: 32x4 loses up to 1.3%, 3% on handwritten
  text. 32x2 loses up to 4.5%, 10% on handwritten text. 32x1 loses from 0.3%
  to 22%.
- The morphing demos hardly depend on the size: a glow frame goes to the
  display in one window, and tiles are touched only along its edges.
- Matrix text is 5–6% faster with 32x4, 32x2 and 32x1. A line is erased with
  a rectangle whose edges do not lie on tile boundaries, and with a low tile
  less is copied at those edges: over 25 seconds 45 MB of catch-up copy with
  32x8 and 19 MB with 32x4. The tall 32x16 tile is a quarter worse here.
- A tile 64 wide is worse than one 32 wide of the same height everywhere.

**The choice.** The default tile is 32x4, in the header `DGX_DSI_TILE_WIDTH`
and `DGX_DSI_TILE_HEIGHT`. On the demos 32x8 is best, but 32x4 trails it by
0–3%, which is not seen. Where it wins, it wins noticeably: 6% on matrix
text, 28% on scattered pixels. Memory: 0.6 KB more. With the 32x4 tile the
model check and the bench scenes were run again.

A program sets another size itself; the call is optional:

```c
dgx_bus_protocols_t *bus = dgx_dsi_init(0, 2, 750, 3, 2500);
if (dgx_dsi_tiles(bus, 32, 8) != ESP_OK) ESP_LOGW(TAG, "the tiles stay 32x4");
dgx_screen_t *screen = dgx_jd9165_init(bus, rst, 16, DgxScreenRGB, DgxDsiTwoFrames);
```

The call checks the size and answers with an ESP-IDF code. If the size is not
taken, the tiles stay as they were, and a warning appears in the log.

| Answer | When |
| --- | --- |
| `ESP_OK` | the size is taken |
| `ESP_ERR_INVALID_ARG` | no bus; the width is not a power of two from 8 to 1024; the height is not a power of two from 1 to 128 |
| `ESP_ERR_NO_MEM` | no memory for the maps |
| `ESP_ERR_INVALID_STATE` | the call came in the middle of a batch |

Before the display constructor the size is only remembered. After it the call
is made between batches: the bus waits for the display to take the finished
picture, brings into the freed buffer everything it lacks, and starts the
maps anew. The model check changed the size this way every 5 batches, 360
times a run: 0 discrepancies. A panel wider than 32 tiles makes the tile
wider by itself.

Changing the size on the fly, to suit what is drawn this minute, is not the
way. The size is chosen once, for the program, and set before the display
constructor.

- A change is not free. The drawing task waits for the display: in the same
  check 10.6 ms on average and up to 23 ms per change. The picture stands
  still for that time.
- The gain from the right tile is small, a few percent on the demos. On
  matrix text the 32x4 tile saves 0.35 ms a batch, and one change eats that
  saving in thirty batches.
- The maps are taken from the heap anew each time, 0.6 to 4.8 KB. Frequent
  changes fragment the heap.
- In the middle of a batch the call refuses.

The call after the constructor is left for a program that learns the size
late, for instance reads it from its settings.

The one-row tile could be sped up: take rows of tiles with the same mask in
one piece instead of one by one. This is not done.

### The cost for a pixel

With two buffers `set_pixel` goes to the map for every pixel: a bit in `owed`
has to be checked and a bit in `fresh` set. To make this cost less, the bus
remembers the tile of the last pixel and does not go to the maps for a pixel
in the same tile. For a screen in the normal orientation it also does not
convert screen coordinates to panel coordinates. What remains shows on the
gradient by pixels: 14.8 ms against 12.9 ms with the copy by one rectangle,
see "The working time of a batch".

### How it is checked

The tile module does not depend on the platform and is checked on the host,
`test/host/test_fb_tiles.c`. The test keeps two buffers and a reference,
draws random batches from all the cases of the table above, and after every
swap compares the buffer with the reference. Separately it checks that only
what is needed is copied.

### A virtual screen as the source

Another variant was considered: always draw into one's own virtual screen
and copy what changed into the hidden buffer. It is no cheaper. A virtual
screen the size of the display also lives in PSRAM, and a copy from it costs
the same. And the tiles of this batch and the previous one would have to be
copied together, whereas drawing right into the hidden buffer copies only
the previous one's.

### How LVGL 9 does it

LVGL is the most widespread graphics library for such displays, and its task
is the same. I read how it solves it: `lv_refr.c` of LVGL itself and
`esp_lvgl_port_disp.c`, the Espressif port that ties LVGL to the display
driver.

First, the difference in how they draw. In DGX a program calls drawing, and
it lands in the buffer at once. In LVGL a program changes objects: a button,
a label, a line. Each change marks a rectangle of the screen as stale, and
once a frame LVGL itself redraws all stale rectangles.

The port works in one of three modes.

| Port mode | Frame buffers | What is drawn every frame | Tearing |
| --- | --- | --- | --- |
| Normal | 1 | the stale rectangles, into a small separate buffer; the port copies it into the frame buffer | possible |
| `avoid_tearing` with `full_refresh` | 2 | the whole screen, into the hidden buffer | none |
| `avoid_tearing` with `direct_mode` | 2 | the stale rectangles, right into the hidden buffer | none |

The third mode is arranged like `DgxDsiTwoFrames`. A frame goes like this.

1. LVGL has collected the stale rectangles of this frame.
2. The hidden buffer lacks what was drawn in the previous frame. LVGL takes
   the rectangles of the previous frame, subtracts those of this one, and
   copies the rest from the shown buffer into the hidden one. That is the
   same catch-up copy.
3. LVGL redraws the rectangles of this frame in the hidden buffer.
4. The port asks the driver to swap the buffers and waits for the end-of-frame
   report.

| | LVGL, `direct_mode` | DGX, two buffers |
| --- | --- | --- |
| How changes are marked | rectangles, at most 32 | tiles of 32x4, 4800 of them on 1024x600 |
| A line across the diagonal | one rectangle of the whole screen | about 180 tiles |
| If there are too many marks | the list is replaced by one rectangle of the whole screen | nothing to overflow |
| What is not copied | what will be redrawn in this frame: known in advance | what a fill or a window covered: found out as it goes |
| Who copies | the processor, row by row | the processor, and large pieces the 2D DMA |
| No tearing on one buffer | no way | the "behind the scan" strategy, for a virtual screen put to the display |

- The number 32 is `LV_INV_BUF_SIZE`; it can be changed when building LVGL.
- LVGL knows in advance what it will redraw because it first collects the
  changes and then draws. DGX draws at once and does not know the next call.
- The copy in LVGL goes through a replaceable function, `buf_copy_cb` in
  `lv_draw_buf.c`. A hardware one could be put there; the Espressif port
  does not.

## The 2D DMA and the PPA

The ESP32-P4 has two hardware blocks that write to memory without the
processor and past its cache. The 2D DMA moves a rectangle from one picture
into another. The PPA, the pixel processing accelerator, among other things
fills a rectangle with a colour and blends two pictures. In the bus they have
four jobs.

| Job | By | What is done | From what size |
| --- | --- | --- | --- |
| Putting a virtual screen to the display | 2D DMA | rows of a 16-bit virtual screen go into the frame buffer, bytes exchanged | 16 KB and more |
| The catch-up copy with two buffers | 2D DMA | what is left to copy before the swap goes from one frame buffer into the other | rows × (width + 53) at least 8000 |
| Filling a rectangle | PPA | a rectangle of the frame buffer is filled with a colour | rows + bytes / 64 at least 200 |
| Putting an 8-bit screen through a two-colour table | PPA | the rectangle is filled with the first colour, and the second is blended in by a byte per pixel | any, only with two buffers |

The thresholds in the last column are fixed in the code from the
JD9165 (7"); the board measurement takes them anew, see "Measuring the
board". The two blocks cost 0.8 KB of internal RAM: 0.5 KB the DMA and
0.3 KB the PPA. The 2D DMA has an escape hatch, `CONFIG_DGX_DSI_PRIVATE_DMA2D`,
see below; the PPA has none and is built wherever its public header is.

### Which IDF interface

There are three ways to the 2D DMA in IDF.

| Way | Where | Suits |
| --- | --- | --- |
| The `use_dma2d` flag of the DPI driver, in IDF 6 the function `esp_lcd_dpi_panel_enable_dma2d()` | 5.x and 6.x | no: copies a foreign buffer only into the shown frame buffer, and without the byte exchange |
| The public `esp_async_color_convert.h` | 6.0.3, 6.1 | no: it has no byte exchange of 16-bit pixels |
| The private `esp_private/dma2d.h` | all versions with the ESP32-P4 | yes |

The bus uses the private interface. This is a risk: IDF may change a private
interface. One difference between versions has already come up: the
structure with the burst length setting differs between 5.5.4 and 5.5.5 and
newer. The bus does not touch it; the defaults are the same, 128 bytes.
Builds are checked under 5.5.4, 5.5.5, 6.0.1, 6.0.3 and 6.1. If it stops
building under a new IDF, `CONFIG_DGX_DSI_PRIVATE_DMA2D`, on by default, when
turned off hands all the work of the 2D DMA to the processor. It is an escape
hatch, not a setting: it affects nothing else. Up to and including release
0.6.2 it was called `CONFIG_DGX_DSI_HARDWARE_COPY` and turned off the PPA as
well, which needs no such safeguard.

Through the public interface of 6.1 I first made the catch-up copy and
measured: before every operation it brings the whole picture out of the
cache, not the rectangle, and its queue costs 0.7 to 2.9 KB of RAM. The own
call through the private interface brings out only the rows of the
rectangle.

### The byte exchange

The DMA can change the byte order of a pixel: six permutations of three
bytes, the setting is called scramble. For a 16-bit pixel the permutation
`DMA2D_SCRAMBLE_ORDER_BYTE2_0_1` swaps its two bytes:

    in:  a0 10 a1 11 a2 12
    out: 10 a0 11 a1 12 a2

But this works only in the right input mode. In the chip register
`OUT_COLOR_INPUT_SEL` it has two values for the exchange:

| Value | What it means by the register description |
| --- | --- |
| 2 | Other 2byte/pixel type |
| 3 | Other 3byte/pixel type |

The IDF driver sets one of them itself, and which depends on the version.

| IDF | What the driver sets | 16-bit pixels | 24-bit pixels |
| --- | --- | --- | --- |
| 5.3.4, 5.4.3, 5.5.4, 5.5.5, 6.0.1, 6.0.2 | 2 | works | garbage comes out |
| 6.0.3, 6.1, master | 3 | the transfer never completes | works by the commit's intent; not checked |

The value for each version is taken from its source. The behaviour was
measured under 5.5.4, 6.0.1 and 6.1. The value was changed by commit
`78ccd163cc`, "fix non-usable DMA2D_CSC_TX_SCRAMBLE option": it mended 24
bits and broke 16. This is reported in
[issue 19226](https://github.com/espressif/esp-idf/issues/19226) with an
example that reproduces the hang without a display; no answer yet.

After the driver's setup the bus itself writes 2 into the register field. The
driver does not tell the channel number, so the channel is found by the
address of its own descriptor. Under 5.5.4 this changes nothing; under 6.0.3
and 6.1 the transfer hangs without it.

Checked under 5.5.4 and 6.1, the display running from the same PSRAM:

- all 614400 pixels of a 1024x600 picture come out with their bytes
  exchanged;
- rectangles from an odd column and of odd width, down to a one-pixel column
  600 high, are right inside and touch nothing outside: 14 cases. The IDF
  header warns that the destination needs 4-byte alignment; in practice the
  neighbouring pixels do not suffer.

### A virtual screen put to the display

The bus has an optional function `write_rows`: "take rows of pixels from
memory and put them into a rectangle of the screen". No other bus has it. A
virtual screen first offers the rows to it, and if the bus declines, sends
them through a window.

The DSI bus takes the rows when all of this holds:

- the pixels are 16-bit, and red and blue do not change places;
- the screen is neither mirrored nor rotated: the DMA writes only left to
  right and top to bottom;
- the rectangle is at least 16 KB. Anything smaller the processor puts
  through a window while the DMA is still starting. The board measurement
  lowers this threshold to 4–5 KB, see "Measuring the board".

Otherwise the path through a window works.

JD9165 (7"), one buffer, 256 KB cache, IDF 5.5.4; the cells hold the time of
putting an area of a virtual screen to the display.

| What is put | Through a window, processor | By the DMA |
| --- | --- | --- |
| The whole screen, 1024x600 | 34.4 ms | 8.8 ms |
| An area of 320x100 | 1.12 ms | 0.51 ms |
| An area of 60x100, under 16 KB | 0.28 ms | 0.28 ms: goes through a window |

Before starting the DMA the bus sees to the cache itself: it brings out the
rows of the source, and brings out and drops the rows of the destination so
that after the DMA the processor reads them from memory. With two buffers
the tile rule for a rectangle written whole works before this. Behind the
scan the bus waits before starting for the scan to leave the rows.

### An 8-bit screen through a colour table

A screen of 8 bits a pixel goes to a 16-bit display through a colour table,
`dgx_vscreen8_region_to_screen16()`. It has no ready rows in colours. For
it the bus has a third optional function, `write_indexed`: "take a picture
of indices, a colour table and a scale, and put the picture into a
rectangle of the screen". The DSI bus makes rows of the panel from the
indices. The processor reads an index, looks it up in the table, repeats the
colour as many times as the scale asks, and writes it straight in the order
the panel keeps its pixels, whatever the orientation. A row of the panel
that has the same indices as the one above it is a copy of that row. The
rows go into one of two bands of 32 KB of internal RAM, and while the 2D DMA
moves one band into the frame buffer, the processor makes the next. The
memory is taken for the duration of the call. The bus takes the job on the
same conditions as `write_rows`, except the orientation: 16-bit pixels, red
and blue as the panel takes them, at least the `rows_from` bytes of
"Measuring the board". A rectangle the bus declines goes through a window.

This is what makes a turned screen fast. Through a window, a screen rotated
by 90 degrees writes each pixel a row of the panel away from the one before,
1600 bytes on the 10" panel, and the cache brings in a line of 64 bytes for
every 2 bytes written. The same path serves `DGX_LUT_SCALE(n)`: a 320x200
game picture goes to a 1280x800 screen with every pixel a 4x4 square. The
processor reads 64 000 indices, and three of every four rows of the panel
are copies. With `DGX_LUT_SMOOTH` as well, every row of the panel is a line
of the picture smoothed by Scale2x, a row of it or a column of it: Scale2x
gives the same picture whichever way it is turned, so it is computed along
the panel's rows directly. The lines of the picture at half the size that a
line needs are kept, four of them, and the next row of the panel reuses
them.

JC8012P4A1C (10"), JD9365, ESP32-P4 revision 3.2, PSRAM at 250 MHz, 256 KB
cache, IDF 6.0.1 at `-O2`, two buffers, the screen turned to 1280x800
landscape (axes swapped). A game frame of 320x200 indices goes to the whole
screen at `DGX_LUT_SCALE(4)`, measured inside the Prince of Persia port.
"The call" is `dgx_vscreen8_region_to_screen16()`, "the frame" is the whole
picture of the game, before is 0.6.3 through the window.

| | Frame before | Frame now | The call |
| --- | --- | --- | --- |
| Squares | 123–157 ms | 16.1–16.9 ms | 11.0–11.7 ms |
| Smoothed, `DGX_LUT_SMOOTH` | about 145 ms | 31.9–32.6 ms | 26.8–27.4 ms |

The DMA keeps up with the processor; what is left is making the rows.
Timed inside the bus in 0.6.4, the processor took 8.1 ms of the call for
squares and 31 ms of the call for the smoothed frame, and the waits for the
DMA 0.2 ms in either. 0.6.5 made the smoothed rows faster: the lines of the
smaller pictures are kept for the whole call, two rows of the panel that
are a pair are made together, and two pixels go as one word. The smoothed
frame before was the game's own Scale2x, 26 ms on this board, followed by
the window.

Before `write_indexed` the colours were prepared in strips of up to 64 KB
and each strip went through `write_rows`, which took only a screen neither
mirrored nor rotated. The table below measures that path.

The `hw_soft_pen_demo` example: every frame a rectangle of 367 thousand
pixels from a 1024x600 page goes to the display. JD9165 (7"), two buffers,
128 KB cache, IDF 5.5.4.

| | Through a window | In strips by the DMA |
| --- | --- | --- |
| Sending the rectangle | 37.2 ms | 20.7 ms |
| Frames a second with drawing | 17.7 | 25.5 |

Of the 20.7 ms about 12 is the translation itself: the processor reads the
page from PSRAM and looks in the table. With a 256 KB cache the same example
gives 27.8 frames a second, with 512 KB 32.8.

**Blending by the accelerator.** A colour table the accelerator cannot do:
the indexed input is commented out in the IDF driver. It can do something
else: blend a picture with one colour, taking the colour's opacity from an
8-bit mask. A screen where a byte is the amount of ink is such a mask: that
is how a pen with a soft edge draws, and its table runs from the colour of
the paper to the colour of the ink. The put recognises such a table itself:
none of its colours is further from the straight path between the first and
the last than one step of red, green or blue. Then the bus is given two
colours, the first and the last. It fills the rectangle of the frame buffer
with the first and blends the second into it; the destination is the
rectangle itself. The processor moves not one pixel. There are no new calls
for this: the program calls the same `dgx_vscreen8_region_to_screen16()`
with the same table.

Between the fill and the blend the rectangle is all paper. With two buffers
nobody sees it. With one the display would show it every other time, so with
one buffer the bus declines this job, and the picture goes in strips through
the table like any other.

What the accelerator gives was checked by reading the frame buffer back, six
pairs of colours: all 614 400 pixels of the screen get the colour of their
amount, zero ink gives exactly the paper, 255 exactly the ink, rectangles
from one pixel up land in place. Between paper and ink the accelerator
rounds in its own way, and no exact rule could be found: with the table of
the `hw_soft_pen_demo` example it differs in 140 colours of 256, everywhere by
one step of a channel. On one display all pictures with one table go one
way.

A channel step is 3.2% of the range of red or blue and 1.6% of green. In
brightness, with the colours of the same example, it comes to no more than
2.3% of full when all three channels go one way, and 0.5% on average over the
table. Only the colours between the first and the last differ, that is the
pixels of a line's soft edge. A program that needs the colours of the table
and nothing else says so at the put: the last argument of the call is flags,
and `DGX_LUT_EXACT` sends the screen through the table in strips.
`DGX_LUT_TRANSPARENT` means what `true` used to. Where there is nothing to
blend with, the flag changes nothing.

JD9165 (7"), one buffer, 128 KB cache, IDF 5.5.4; blending with one buffer
was turned on for this measurement only. The cells hold the time of putting
the rectangle.

| Rectangle | In strips by the DMA | Blending by the accelerator |
| --- | --- | --- |
| 1024x600 | 28.0 ms | 17.1 ms |
| 900x408 | 17.1 ms | 10.6 ms |
| 320x100 | 1.28 ms | 1.13 ms |
| 100x60 | 0.88 ms | 0.50 ms |
| 40x40 | 0.21 ms | 0.28 ms |

In the `hw_soft_pen_demo` example with two buffers the sending took 13.5 ms
instead of 20.7, and the lines move at 31.2 frames a second. The accelerator
client for blending took no internal RAM by the heap counter.

### The catch-up copy by the DMA

Only what is left in `owed` before the swap goes by the DMA. For that the
remainder is gathered into rectangles: adjoining tiles in a row and the same
run in the rows below go as one piece. Copies at first touch stay with the
processor. After them this place is drawn in, and it is useful to have the
rows in the cache, where they are not after the DMA.

One DMA operation in the bus costs about 0.2 ms over the copying itself:
bringing out the cache, queueing, the interrupt, waking the task. Hence the
threshold: an area of 64x48 is copied by the processor, an area of 320x100
and a column of 40x548 by the DMA.

### A fill by the PPA

The PPA has a public interface, `driver/ppa.h`, the function `ppa_do_fill()`.
It is given a picture, a rectangle in it and a colour. The screen orientation
does not matter for a fill: a rectangle of the screen stays a rectangle on
the panel. Both 16 and 24 bits will do.

The PPA takes the colour as 8 bits of red, green and blue and reduces it to
16 bits itself. For exactly the 16-bit value asked for to come out, the 5
and 6 bits are stretched to 8 by repeating the high bits. Checked
exhaustively: all 65536 colours come out exact.

The PPA does not touch neighbouring pixels: checked on 12 rectangles, from
one pixel to the whole screen, from odd columns and rows.

The driver sees to the cache itself: before the fill it writes back the rows
of the rectangle from the cache and drops them.

JD9165 (7"), display running, IDF 5.5.4; the cells hold the time of one fill,
for the processor together with bringing it out of the cache.

| Rectangle | Bytes | Processor | PPA |
| --- | --- | --- | --- |
| 1024x600 | 1228800 | 17.3 ms | 3.86 ms |
| 1024x200 | 409600 | 6.25 ms | 1.57 ms |
| 320x100 | 64000 | 1.13 ms | 0.29 ms |
| 100x100 | 20000 | 0.41 ms | 0.16 ms |
| 40x548 | 43840 | 1.08 ms | 0.27 ms |
| 4x548 | 4384 | 0.50 ms | 0.16 ms |
| 48x48 | 4608 | 0.12 ms | 0.074 ms |
| 1024x1 | 2048 | 0.038 ms | 0.059 ms |
| 32x8 | 512 | 0.019 ms | 0.057 ms |

The PPA fills at 320 KB/ms, four times faster than the scan, and a running
display does not slow it. One call costs 57 µs, so short runs are filled
faster by the processor.

The threshold is set above the break-even point: rows + bytes / 64 at least
200. The reason is the cache. After the PPA the filled place is not in the
cache, and if it is drawn over at once, every row has to be fetched from
PSRAM. So the 48x48 square stays with the processor, while the 100x100 square
and the 4x548 column go to the PPA.

Letters and shapes consist of short runs, and each goes through the
rectangle fill. So that the threshold check does not slow them, a sieve of
one addition stands before it: width plus height under 64. No rectangle that
passes the threshold falls under this sieve; the smallest sum among them is
110. A line of text of 25 characters takes 1.00 ms, as before the
accelerator.

JD9165 (7"), one buffer, 256 KB cache, IDF 5.5.4; the cells hold the time of
a fill in the bus.

| What is filled | Processor | With the PPA |
| --- | --- | --- |
| The whole screen | 16.4 ms | 3.5 ms |
| 100x100 | 0.18 ms | 0.13 ms |
| A screen-high column, 1x600 | 0.5 ms | 0.15 ms |
| A screen-wide row, 1024x1 | 0.026 ms | 0.026 ms: the processor fills |

### Internal RAM

How much heap `dgx_dsi_init()` and `dgx_jd9165_init()` take together, in
bytes. The sum includes the bus buffer of 4096 bytes.

| Mode | IDF 5.5.4, no DMA or PPA | IDF 5.5.4, with DMA and PPA | IDF 6.0.1, with DMA and PPA, current code |
| --- | --- | --- | --- |
| One buffer | 4920 | 5736 | 5880 |
| Behind the scan | not measured | 5816 | 5964 |
| Two buffers, copy by one rectangle | 5036 | did not exist | did not exist |
| Two buffers, tiles | 5700 | 6492 | 8096 |

The IDF 5.5.4 columns were taken with the 32x8 tile before the joining of
batches, the copy after the swap, the counters and the board measurement;
under IDF 6.1 the same code with two buffers, DMA and PPA took 6580 bytes.
The last column is the current code with the 32x4 tile. The tile maps take 8
bytes per row of tiles: with the 32x8 tile that is 0.6 KB less, with 32x2
1.2 KB more, with 32x1 3.6 KB more.

## Measuring the board

From what size a rectangle goes by the 2D DMA or the PPA rather than the
processor, and how much room the bus leaves before the scan, is decided by
numbers. They depend on the chip, its memory and the display. The code holds
the numbers of the JD9165 (7"), on which the bus was written.
`dgx_dsi_bench()` takes them on the board at hand.

```c
dgx_dsi_tuning_t tuning;
if (dgx_dsi_bench(bus, &tuning)) dgx_dsi_tune(bus, &tuning);
```

| Function | What it does |
| --- | --- |
| `dgx_dsi_bench(bus, &tuning)` | measures the board and gives the numbers. The bus keeps its old ones meanwhile |
| `dgx_dsi_tune(bus, &tuning)` | gives the numbers to the bus. Numbers of another version of the structure, another panel, another colour depth or another PSRAM clock it does not take; it returns `false` and logs the reason |
| `dgx_dsi_tuning(bus, &tuning)` | reads the numbers the bus works with now |
| `dgx_dsi_advise(&tuning)` | says from the measured numbers whether the display needs two buffers, see "Advice from the numbers" |
| `dgx_dsi_tuning_text(&tuning, text, size)` | puts in words what the numbers say about the board, for the developer; `dgx_dsi_bench()` logs the same |

Why a separate call and not at panel start.

- The measurements themselves take 0.07 s at 16 bits and 0.09 s at 24, and
  about a quarter of a second more goes to the PSRAM margin check: about
  0.3 s in all. On every start that is wasted.
- The numbers do not change from start to start. `dgx_dsi_tuning_t` is a flat
  structure of 48 bytes, no pointers. A program puts it in NVS and at the
  next start hands it to the bus from there. The `dsi_demo` example does so.
- The bus does not touch NVS itself. Where to keep the numbers and when to
  measure again is the program's decision.
- A bus given nothing works with its old numbers. The measurement changes
  nothing until the program asks.

Nothing on the screen changes during the measurement. It works in memory of
its own: two pictures of 128 panel rows each, half a megabyte of PSRAM at
1024 pixels a row and 16 bits. At the end it returns it. Checked: the
checksum of the screen before six measurements in a row and after them is
the same, and the heap comes back whole.

The display must be running meanwhile; that is a condition of the
measurement. The display reads the frame buffer from the same PSRAM all the
time, and with a running display the processor reads and writes it at a
different speed. The numbers have to be those of real work, so before
`dgx_dsi_video_start()` the measurement refuses. Drawing during the
measurement is not allowed.

### What is measured

Every operation is done nine times and the median taken. Where a number goes
into a margin, the operation is done three times and the longest taken.
Before every run both pictures are dropped from the cache. The last column
holds what the measurement got on the JD9165 (7") at 16 bits.

| What is measured | How | JD9165 (7"), 16 bits |
| --- | --- | --- |
| Writing through a window | the whole 1024x128 picture in portions of 4 KB, each portion first copied from the second picture as from a virtual screen; brought out of the cache at the end | 7.7 ms, that is 34 KB/ms |
| Bringing out of the cache | 32 KB just written | 107–120 µs |
| Copy by the processor | 32x128 and 512x16 | 218–221 and 361–364 µs |
| Copy by the 2D DMA | 32x8, 1024x32 and 1024x128 | 62–68, 462–470 and 1676–1693 µs |
| Fill by the processor | 16x128 and 512x16 | 153–162 and 234–235 µs |
| Fill by the PPA | 16x16, 16x128 and 1024x64 | 78–85, 94 and 434 µs |
| PSRAM margin | the FIFO level of the bridge before the DSI host is read for 3 frames with no load, then for 12 frames while the 2D DMA copies the measurement's pictures one after another; drops to zero are counted beyond the one a frame the blanking gives | not taken on the JD9165 (7"); on the JD9365 (10") 29–33 a frame with PSRAM at 200 MHz, see "When the PSRAM cannot keep up" |

### How the thresholds are worked out

From the two shapes of one operation come the cost of a row and the cost of
a byte; from the small operation the cost of a call to the DMA or the PPA.
Then the thresholds are worked out so.

- **Copy.** To the processor a rectangle costs "rows times the cost of a row
  plus bytes times the cost of a byte"; to the DMA "the cost of a call plus
  bytes times the DMA cost of a byte". The threshold stands where the DMA
  becomes faster, with room: a fifth at 16 bits, twice at 24.
- **Why the room differs.** The measurement starts with an empty cache. In
  real work a piece drawn a moment ago still lies in the cache, and from
  there the processor copies it four times faster than from PSRAM. At 24 bits
  the piece is copied at the end of the batch, and the batch waits for it:
  the DMA must win with room. At 16 bits the piece for the DMA is left for
  later, and the batch pays only for leaving it, about 50 µs at any size.
- **Fill.** The same comparison, room of two and a half times: after the PPA
  the filled place is not in the cache, see "A fill by the PPA".
- **Rows of a program.** Writing through a window against the DMA, room of
  1.6.
- The speeds of writing through a window and of bringing out of the cache are
  taken a quarter below the measured.
- The starting room is twice the cost of a call to the DMA.

Four numbers the measurement does not touch; they stay as in the code: the
display reads 6 rows ahead, a second transfer slows the copy 2.5 times, the
room for a write behind the scan is 150 µs, the room before the end of frame
is 2 ms. The first and the last need the frame buffer itself under the scan;
they cannot be taken in memory of its own. The slowing by a second transfer
could be taken; this is not done.

The tile size the measurement does not choose. It depends on what the program
draws, not on the board, and is set by a separate call, see "The size of a
tile".

### What came out on the JD9165 (7")

The measurement was repeated six times in each mode; the cells hold the
spread. Beside them the numbers fixed in the code.

| Number | Fixed in the code | Measured, 16 bits | Measured, 24 bits |
| --- | --- | --- | --- |
| Writing through a window, KB/ms | 28 | 24–25 | 27–28 |
| Bringing out of the cache, KB/ms | 256 | 205–237 | 220–240 |
| Speed of the 2D DMA, KB/ms | measured as it goes | 150–152 | 133 |
| Room for starting the DMA and the PPA, µs | 400 | 116–150 | 114–126 |
| Copy by the DMA when rows × (width + A) is at least B | A 53, B 8000 | A 8–10, B 2200–2900 | A 30–34, B 5200–5700 |
| Rows of a program by the DMA from, bytes | 16384 | 3900–5200 | not used |
| Fill by the PPA when rows + bytes / A is at least B | A 64, B 200 | A 65–76, B 244–270 | A 78–130, B 196–300 |
| Sieve before the fill threshold: width + height at least | 64 | 151–158 | 140–151 |

**Do the numbers suit: the bus's choices.** Through `dgx_dsi_tune()` the bus
is given numbers under which it does everything one way, then numbers under
which it does everything the other way, and the same thing is timed both
ways. So for every shape of rectangle it is known which way is faster. The
bus's choices by the fixed numbers and by the measured ones are compared with
that. Shapes: 10 widths from 8 to 1024 by 10 heights from 4 to 548 for fills
and rows of a program, 7 widths from 32 to 1024 by 9 heights from 8 to 544
for the catch-up copy. JD9165 (7"), IDF 6.0.1. The third column is the time
of all shapes when each goes the way that is faster for it; the last two are
the time lost over all shapes together by choosing the slower way.

| Mode | What | All the fast way | Lost by the fixed numbers | Lost by the measured |
| --- | --- | --- | --- | --- |
| One buffer, 16 bits | fills | 24.7 ms | 0.02 ms | 0.11 ms |
| One buffer, 16 bits | rows of a program | 51.8 ms | 4.08 ms | 0.36 ms |
| Two buffers, 16 bits | fills | 29.0 ms | 0.04 ms | 0.02 ms |
| Two buffers, 16 bits | rows of a program | 89.5 ms | 3.83 ms | 0.40 ms |
| Two buffers, 16 bits | catch-up copy | 3.3 ms | 0.24 ms | 0.001 ms |
| Two buffers, 24 bits | fills | 33.9 ms | 0.09 ms | 0.006 ms |
| Two buffers, 24 bits | catch-up copy | 76.2 ms | 0.02 ms | 0.003 ms |

- Fills and the catch-up copy: the difference between the fixed and the
  measured numbers is under half a percent of the time. The fixed ones were
  taken on this same board; nothing else was expected.
- Rows of a program: the measured threshold is ten times better. 16 KB was a
  lot; the DMA pays from 4–5 KB already.
- The catch-up copy at 16 bits: 3.3 ms for 63 shapes because the piece is
  left for later and the batch does not wait for it. The lower the threshold
  the better, and the measured one is lower.
- Behind the scan such a check says nothing: a tall rectangle by the DMA or
  the PPA waits there for the scan up to a whole frame, while the processor
  writes at once, and "faster" no longer means the same.
- The threshold for rows of a program counts bytes only and does not tell
  shapes apart. A narrow tall area, 8x200, the DMA puts three times faster
  than a window, yet the threshold leaves it to the window.

**Do the numbers suit: real work.** With the measured numbers the model check
and the bench scenes were run again. JD9165 (7"), IDF 6.0.1, 32x4 tile.

| What | Fixed numbers | Measured numbers |
| --- | --- | --- |
| Model check: one buffer, behind the scan, two buffers at 16 and 24 bits | 0 discrepancies | 0 discrepancies |
| The same under IDF 5.5.5, two buffers, 16 bits | 0 discrepancies | 0 discrepancies |
| Pieces behind the scan in all scenes and checks | 0 | 0 |
| Two buffers, scenes 1–6, frames a second | 42.3, 60.9, 34.8, 60.9, 60.9, 60.9 | 42.4, 60.9, 34.8, 60.9, 60.9, 60.9 |
| Two buffers, scene 7, pictures a second of the three tasks | 40.2, 41.9, 40.1 | 40.6, 43.1, 40.1 |
| Two buffers, scene 12 | 59.2, 39.5, 30.8 | 59.3, 39.6, 31.0 |
| Two buffers, working time of the bench's fourteen batches | taken on the same bench | differs by no more than 0.12 ms, except the gradient |
| Behind the scan, scenes 1–6, frames a second | 30.4, 60.9, 45.6, 60.9, 60.9, 60.9 | 30.4, 60.9, 53.2, 60.9, 60.9, 60.9 |
| Behind the scan, scenes 1, 2 and 4: refreshes the scan fell into | 0 of 305, 0 of 610, 0 of 610 | 0 of 305, 0 of 610, 0 of 610 |

- Correctness suffered nowhere, and not one piece fell behind the scan.
- The time of the gradient by pixels wanders by a millisecond from build to
  build, 15.0 to 16.0 ms, and has nothing to do with the measured numbers.
- Behind the scan scene 3 sped up from 45.6 to 53.2. Its tall fills can start
  only in the blanking, and the starting room became 0.12 ms instead of
  0.4 ms: more fills fit in one blanking. Nobody looked at this scene by eye
  with the new room.

This comparison was made on one board, the very one the fixed numbers were
taken on. It shows that there it comes out no worse.

### What came out on the JD9365 (10")

The measurement was run at two PSRAM clocks; it itself takes 302–310 ms. No
comparison with the bus's choices like the one on the JD9165 (7") was made
here.

| Number | PSRAM 200 MHz | PSRAM 250 MHz |
| --- | --- | --- |
| Writing through a window, KB/ms | 23 | 26 |
| Bringing out of the cache, KB/ms | 193–198 | 240–250 |
| Speed of the 2D DMA, KB/ms | 128 | 167 |
| FIFO drops a frame under the DMA copy | 28–33 | 8–11 |
| Share of the DSI lanes taken by pixels at 24 bits | 96% | 96% |

What the measured numbers change on this board: a piece of 60x100 of a
virtual screen, 12 KB, goes through a window in 163 µs with the built-in
numbers and by the 2D DMA in 77 µs with the measured ones. The threshold for
rows of a program became 3–3.6 KB instead of 16.

### Advice from the numbers

Four fields of the structure are not thresholds but facts about the board:
`fifo_drops`, FIFO drops a frame under the DMA copy; `lanes_percent`, how
much of what the DSI lanes carry the pixels of the panel take, calculated
from the timings at display start; `psram_mhz`, the PSRAM clock at the
measurement, `CONFIG_SPIRAM_SPEED`; `chip_revision`, the chip revision by
`esp_chip_info()`.

Of this a program needs one decision, and `dgx_dsi_advise()` gives it: at
more than 15 drops a frame it returns `DGX_DSI_ADVISE_TWO_FRAMES`, that is,
with one buffer heavy drawing will leave the display short of data and two
buffers are needed. It measures nothing, so it can be asked with numbers
from NVS before the display is made.

The rest is for the developer: the PSRAM clock and the color depth are
settings of the build, the lanes and the panel are the board, and a program
will not change them. Which PSRAM clocks a chip has the bus cannot know:
today it is 250 MHz on revision 3, another revision will differ. So there
are no flags for these; there is `dgx_dsi_tuning_text()`, which writes one
paragraph from the structure in words: what was measured and what it means.

| What the text says | When |
| --- | --- |
| The PSRAM clock and the chip revision | always |
| How many times a frame the display ran short of data under the 2D DMA; "margin enough" | drops measured, 15 or fewer |
| The same; with one frame buffer heavy drawing leaves it short and the screen flashes for a frame; two frame buffers mend that, as may a faster PSRAM clock if the chip has one | more than 15 drops |
| The margin of the PSRAM was not measured | the measurement ran without the 2D DMA |
| How much of the lanes the pixels take | known, that is the display was made |
| The same; too much, the display may run short with nothing drawn: fewer bits a pixel, or more lanes, or a faster lane rate | more than 85% |

`dgx_dsi_bench()` logs this text: as a warning when the margin is short or
the lanes are nearly full, as information otherwise. A library that cannot
act on it hands the text on to its user, and the facts stay in the structure
for a program that decides differently.

The limits of 15 and 85% were set by eye from the JD9365 (10"), see "When
the PSRAM cannot keep up": at 200 MHz there are 28–33 drops there and
flashes, at 250 MHz 8–11 and no flashes; at 24 bits 96% of the lanes and
flashes with nothing drawn. On another board the line may lie elsewhere.
Numbers taken at another PSRAM clock `dgx_dsi_tune()` does not take, and the
program measures again: so the numbers are bound to the clock they were
measured at.

## When the PSRAM cannot keep up

Everything in this section was taken on the JD9365 (10"). The display reads
from PSRAM 160 MB/s within a line, 123 MB/s on average over a frame,
calculated from the timings. When the processor and the DMA load PSRAM too,
the display may run short.

**What it looks like.** With one frame buffer and PSRAM at 200 MHz, moving
bands of 800x427 copied 58 times a second now and then left the bridge
before the DSI host without data. The bridge then sends the value of its
register `dpi_rsv_dpi_data` until the end of the frame (0x3FFF; at 16-bit
output the low 16 bits, a light blue): a light blue flash with ragged rows.
With that register set to 0 the flashes became dark, with 0xF800 red.

**What did not help.** The bridge's underrun interrupt never fired, even with
`fifo_underrun_discard_vcnt`, which IDF sets to the frame width, set to 0.
AXI priority 15 for the DW-GDMA that reads the frame did not help. The fix
from 70 to 80 MHz, see "What is known about the boards", did not change the
flashes.

**What helped.** PSRAM at 250 MHz; or two frame buffers at 200 MHz.

**The FIFO level of the bridge.** `fifo_flow_status.raw_buf_depth` (up to
980) was read about 9.4 million times a second from the second core. With
nothing drawn it drops to 0 exactly once a frame, for about 4130 reads, in
the vertical blanking; drawing adds short drops within the frame. Moving
bands, 16 bits:

| Mode | Short drops a second | Blanking drop, reads | Flashes by eye |
| --- | --- | --- | --- |
| One buffer, PSRAM 200 MHz | 970–1260 | up to 6559 | seen |
| One buffer, PSRAM 250 MHz | 227–334 | up to 5053 | none |
| Two buffers, PSRAM 200 MHz, 45 frames a second | 311–401 | not stretched | none |

**24 bits.** PSRAM at 250 MHz, one buffer: a frame of 3 MB, the bridge reads
240 MB/s within a line, two lanes carry 1920 of their 2000 Mbit/s. With
nothing drawn the FIFO drops to 0 only in the blanking, so the lanes keep
up. Drawing empties it far more than at 16 bits: 4800 to 11500 short drops a
second while lines, circles and triangles are drawn, one drop of 154243
reads, a whole frame; `dgx_dsi_bench()` measured 139 KB/ms for the 2D DMA.
Here the bridge's underrun flag was set, 5–7 times a second in those seconds,
and IDF printed "underrun happens" 6 times in 40 s. On the screen there was
tearing and, over several rounds of the example, three flashes, blue: the
same 0x3FFF from `dpi_rsv_dpi_data`, whose low 24 bits give R 0x00, G 0x3F,
B 0xFF.

So the underrun report does not betray a flash: at 16 bits the flashes came
without reports, at 24 bits there were far more reports than flashes. Why is
unknown.

## A picture scrolled

`dgx_scroll()` shifts what a rectangle of the screen shows within it. A panel
in video mode has no scroll of its own: the picture is the frame buffer, and
to shift the picture is to shift memory.

The bus has an optional function `move`: "move a rectangle of the screen to
another place on it". No other bus has it; there `dgx_scroll()` reads and
writes pixels through the screen. `dgx_scroll()` first offers the shift to
the bus; the strips the picture vacated it fills like any rectangle.

With two frame buffers the bus first gives the buffer being drawn into what
it lacks in both places together, as before any drawing that reads. Then
`dgx_fb_area_move()` moves the rows of the frame buffer, each whole by
`memmove()`, from the side the picture goes to, so that no row is overwritten
before it is read. A rectangle of the screen is a rectangle of the panel
however the screen lies on it, and shifting one is shifting the other: the
screen may be mirrored and rotated. On a screen in portrait a shift along y
is a shift along the panel's rows, within each row.

The `figures_scroll_demo` example, JD9165 (7"), 16 bits, two buffers, 128 KB
cache, IDF 5.5.4. The first five rows are several shifts in one batch; the
last two are a batch per shift, shown before the next, and the time is that
of the call alone. For comparison, the shift pixel by pixel through the
screen, as it was before `move`.

| What is shifted | Pixel by pixel through the screen | By rows of the frame buffer |
| --- | --- | --- |
| 320x240 up by 14 rows | 61.8 ms | 2.7 ms |
| 320x240 left by 4 columns | 65.3 ms | 3.4 ms |
| 320x240 diagonally by 3 and 2 | 65.0 ms | 6.1 ms |
| The whole screen, 1024x600, up by 28 rows | 501 ms | 24 ms |
| The whole screen left by 6 columns | 524 ms | 32 ms |
| A chart of 400x480 left by 6, a batch per shift | 166–198 ms | 15.5 ms |
| A terminal of 480x488 on a screen in portrait, up by 28, a batch per shift | 471–485 ms | 19.6 ms |

A shift that is a batch of its own costs more than the memory it moves: after
the swap the buffer being drawn into lacks the rectangle, and it receives it
from the other before the shift. The picture could be taken from the other
buffer straight to its new place, one pass instead of two; this is not done.
The 2D DMA is not used for the shift.

Checked on the board in three orientations, landscape, portrait and landscape
turned half round: rectangles, sectors and 296 shifts in any direction, some
larger than their rectangle, were done alike on the display and on a virtual
screen, half of them a batch each, the rest in one batch, and all pixels of
both were compared through `dgx_get_pixel()`. Not one differed, both with the
shift by pixel and with the shift by rows. On the host `test_fb_area` checks
`dgx_fb_area_move()` in all orientations and at both depths.

## Summary measurements

### The working time of a batch

JD9165 (7"), two buffers. Measured is the time from the first drawing to the
return from `update_screen()`, with no waiting for the display: before every
batch the bench waits 40 ms. The three columns are three arrangements of the
catch-up copy:

- "Rectangle": the copy by one enclosing rectangle, the first variant.
  Everything by the processor. IDF 5.5.4.
- "Tiles": tiles of 32x8, everything by the processor. IDF 6.1.
- "Tiles, DMA and PPA": tiles of 32x8, large pieces of the catch-up copy and
  the put of a virtual screen by the 2D DMA, large fills by the PPA. IDF
  5.5.4.

| Batch | Rectangle | Tiles | Tiles, DMA and PPA |
| --- | --- | --- | --- |
| A diagonal across the screen: erase the old, draw the new | 21.2 ms | 2.0 ms | 1.9 ms |
| Two squares of 48x48, one in place, the other jumping between corners | 5.9 ms | 0.31 ms | 0.30 ms |
| 2000 pixels over an area of 800x400 | 16.7 ms | 7.5 ms | 7.4 ms |
| Nine columns 548 high move by 4 pixels (scene 3) | 32.1 ms | 22.7 ms | 17.1 ms |
| 320x100 from a virtual screen, the place jumps by 10 pixels | 3.5 ms | 2.3 ms | 1.2 ms |
| A band of 200 rows from a virtual screen, in one place | 12.7 ms | 12.7 ms | 3.9 ms |
| Fill of the whole screen | 16.7 ms | 16.8 ms | 3.6 ms |
| A gradient of 256x100 by pixels, in one place | 12.9 ms | 14.8 ms | 14.3 ms |
| A square of 48x48 after a batch that filled the whole screen | not measured | 27.4 ms | 8.4 ms |
| A band of 200 rows, even batches in one place, odd in another | not measured | 21.3 ms | 5.5 ms |
| 320x100 from a virtual screen, likewise in two places | not measured | 3.6 ms | 1.1 ms |
| A column of 40x548, likewise in two places | not measured | 3.2 ms | 1.0 ms |

What this shows.

- Everything put from a virtual screen sped up three to four times: that is
  the put by the DMA.
- The fill of the whole screen sped up nearly five times: that is the PPA.
  The columns of narrow strips sped up by a quarter: the strips themselves
  are filled by the PPA, while the catch-up copy around them is still done by
  the processor.
- Batches that draw in different places in turn, and a small batch after a
  large one, sped up thanks to the catch-up copy by the DMA.
- Drawing by pixels and lines does not depend on the DMA and the PPA.
- The gradient by pixels became slower than with the rectangle: that is the
  cost of going to the map for every pixel. Its time changes from build to
  build between 14.3 and 16.2 ms, depending on how the code lands.

The current code differs from the last column in two things. The 32x4 tile
instead of 32x8 changes three rows: 2000 pixels 5.4 ms instead of 7.4, the
nine columns 18.4 instead of 17.1, the diagonal 2.0 instead of 1.9. The copy
after the swap changes the four bottom rows: 0.56, 2.79, 0.55 and 0.28 ms,
see "The copy after the swap".

### Frame rate

Scenes 1–6, 10 seconds each; the cells hold refreshes a second. JD9165 (7"),
IDF 5.5.4, tiles of 32x8, every batch waits for the swap (as it was before
the joining of batches; in each of these scenes one task draws, and for it
the order stayed the same). Where the DMA and the PPA are not named,
everything is by the processor.

| Scene | One buffer, processor | One buffer, DMA and PPA | Behind the scan, DMA and PPA | Two buffers, rectangle | Two buffers, tiles | Two buffers, tiles, DMA and PPA |
| --- | --- | --- | --- | --- | --- | --- |
| 1. Columns, whole screen | 19.6 | 44.3 | 30.5 | 19.7 | 19.6 | 42.7 |
| 2. Columns, band of 200 rows | 54.6 | 60.0 | 60.9 | 46.3 | 44–46 | 61.0 |
| 3. Columns by primitives | 41.7 | 55.6 | 30.4 | 20.3 | 30.4 | 34.8 |
| 4. Columns, band of 80 rows | not measured | 60.0 | 60.9 | 60.7 | 60.8 | 60.9 |
| 5. Arrow and squares | not measured | 62.5 | 60.9 | 30.4 | 60.8 | 60.9 |
| 6. Diagonal | not measured | 61.7 | 61.7 | 35.7 | 60.8 | 60.8 |

In the one-buffer modes the bench keeps the pace, and figures above 60.9 in
those columns are the error of its pause, not the speed of the display.

What is seen by eye.

- One buffer: tearing in every scene where the picture moves, 1 to 4.
- Behind the scan: no tearing in scenes 1, 2 and 4. In scene 3 no tearing
  either, but the width of the running column jerks: its fills spread over
  two frames, see "Behind the scan".
- Two buffers: no tearing in any scene.

What is behind the numbers.

- Scene 1 was limited by putting the screen, 34 ms, three periods. By the DMA
  the put takes 8 ms, and the scene ran into something else: the bench
  redraws the columns in a virtual screen in PSRAM, which takes more than
  half a period. Hence 43–44, not 60.
- Behind the scan scene 1 runs at 30: before every put the bus waits for the
  scan to go into the blanking, and two refreshes do not fit in one frame.
- Scene 2 with two buffers stood at the edge of one period. By the DMA the
  put of the band takes 4 ms instead of 12.7 ms, and it runs at a full 60.
- Scene 3 on one buffer sped up from 41.7 to 55.6: the strips are filled by
  the PPA, the batch takes 2.5 ms instead of 8.4 ms.
- Scene 3 behind the scan runs at 30: 18 tall fills do not fit in one gap,
  and the batch takes two frames.
- Scene 3 with two buffers rose from 30 to 35: the batch takes 17 ms, a
  little over a period, and some batches fit in one frame. The catch-up copy
  of narrow strips over the whole height of the screen still costs more than
  the drawing itself.

With the current code scenes 1–6 with two buffers give 42.3, 60.9, 34.8,
60.9, 60.9 and 60.9, see "Three tasks on the bench".

## What is not done

- **Scroll.** A picture scrolled in a batch of its own moves in two passes,
  by the catch-up copy and the shift; the 2D DMA is not used for the shift.
- **The copy after the swap** works only through the 2D DMA. Without it, and
  with `CONFIG_DGX_DSI_PRIVATE_DMA2D` off, the hidden buffer receives
  everything at the end of the first batch of a frame. At 24 bits it is off.
- **The bus's numbers** were taken on the JD9165 (7"). For another board
  `dgx_dsi_bench()` takes them, but the bus neither runs it nor keeps the
  result itself: the program does. Four numbers it does not measure.
- **The board measurement** was compared with the bus's choices only on the
  JD9165 (7"), the very board the fixed numbers were taken on. On the
  JD9365 (10") it was run and gave numbers, but there was no comparison
  there. The thresholds of `dgx_dsi_advise()` were set by eye from the
  JD9365 (10").
- **IDF versions.** Under 6.0.3 and 6.1 the code was only built after the 2D
  DMA fix, not run on a board: the byte exchange in the 2D DMA is broken
  there, see issue 19226, and the bus works around it by writing the
  register.
- **The time of a batch.** How long a batch will take the bus judges by past
  batches, and it has one estimate. If batches differ in length it is wrong,
  and a finished picture waits an extra frame. In the check with random
  batches the finished picture missed 5% of frame ends, see "How it was
  checked".
- **Evenness.** A picture that comes less often than a frame and not after an
  equal number of frames moves unevenly. The bus does not even this out, see
  "Evenness".
- **The tile size** by default was chosen by the demos on one board. The
  one-row tile is not sped up.
- **The threshold for rows of a program** counts bytes only. Narrow tall areas
  it leaves to the window, though the DMA would put them faster.
- **The end of frame.** There is no way to wait for it in DGX. The bus tells
  the time of a frame and the time of its end; the program sleeps and aims
  itself. For an even delivery several sources need one drawing task and
  messages to it; for tasks that draw themselves under a mutex, evenness is
  down to chance.
- **Behind the scan** a batch is not shown as a whole: of many tall fills some
  appear a frame later. To fix that, the fills of a batch would have to be
  put off to its end, and they still would not fit in one gap. Nor is there a
  way to wait for the next frame there: a program that refreshes more often
  than the display holds itself back. The condition for writing by the
  processor is twice as strict as needed; after the DMA that changes little.
- **A mirrored and a rotated screen, 24 bits** are put by the processor:
  34 ms a screen. The fill by the PPA works there too.
- **Revision 3.** On the JD9365 (10") the branch with `on_refresh_done` was
  checked under IDF 6.0.1; no timing tables were taken on it. The branch with
  `on_frame_buf_complete` on this revision has only been built, under 5.5.5.
- **The private interface.** The 2D DMA is driven through a private IDF
  interface and one register field. No answer yet to
  [issue 19226](https://github.com/espressif/esp-idf/issues/19226).
- **24 bits** on the JD9165 (7") were checked only by reading the buffer
  back; its factory firmware runs at 16 bits. On the JD9365 (10") they were
  looked at by eye with one buffer: tearing and rare flashes from a shortage
  of data, see "When the PSRAM cannot keep up".
- **The benches** everything was taken on are not in the repository.

## How it was checked

**Correctness.** The bench draws random batches to the display and to a
reference virtual screen at the same time and reads the frame buffer back.
The batches hold fills, pixels, lines, circles, areas of a virtual screen, a
whole virtual screen, textures, text and reads in the middle of a batch.
Tearing such a check does not catch: it is not seen in the buffer.

The first run, before the joining of batches: 300 batches in each of three
orientations, JD9165 (7"), DMA and PPA on.

| Mode | Bits | IDF | Discrepancies |
| --- | --- | --- | --- |
| One buffer | 16 | 5.5.4 | 0 |
| Behind the scan | 16 | 5.5.4 | 0 |
| Two buffers | 16 | 5.5.4 | 0 |
| Two buffers | 16 | 6.1 | 0 |
| Two buffers | 24 | 6.1 | 0 |

Before the PPA was hooked up the same checks also passed under 6.0.1 and
6.0.3, and before the move to the private DMA interface under 5.5.5 as well.

The second run, with the joining of batches: to the three passes of 300
batches were added three passes in which the same random batches are drawn
by three tasks in turn. JD9165 (7"); counters over all six passes.

| Mode | Bits | IDF | Discrepancies | Batches | Of them joined a finished picture | Frames | Of them a finished picture missed |
| --- | --- | --- | --- | --- | --- | --- | --- |
| One buffer | 16 | 6.0.1 | 0 | 1660 | the mode does not do this | not counted | the mode does not do this |
| Behind the scan | 16 | 6.0.1 | 0 | 1664 | the mode does not do this | 2274 | the mode does not do this |
| Two buffers | 16 | 6.0.1 | 0 | 1701 | 365 | 3077 | 169 |
| Two buffers | 16 | 5.5.5 | 0 | 1701 | 345 | 3141 | 169 |
| Two buffers | 24 | 6.0.1 | 0 | 1701 | 120 | 3875 | 117 |

The length of a random batch cannot be guessed from past ones, and the
finished picture misses frame ends: 169 of 3077 at 16 bits, 5%, and 117 of
3875 at 24 bits, 3%. More than half fall on the pass with the screen of
600x1024, where a batch with its checks takes 40–60 ms, longer than two
frames: 69 and 89 misses there. One task draws in it without pauses, its next
batch joins its own finished picture and holds it back for its whole time.
While the bus told tasks apart and did not let a task's batch into its own
picture, the misses were 18 and 19. A program with batches of one length has
none of this: in the bench scenes 0 to 5 frame ends of 613 were missed.

The third run: all modes once more with the numbers of the board measurement
and with the tile size changed every 5 batches, see "Measuring the board": 0
discrepancies.

**The swap of buffers.** That a half-drawn picture is not shown and that
batches of different tasks are joined is checked by the model of the display
on the host, `test/host/test_fb_flip.c`, see "Three tasks on the bench". The
tile maps are checked by `test/host/test_fb_tiles.c`, the shift of frame
buffer rows by `test/host/test_fb_area.c`. 19 tests on the host.

**Speed.** The same application is built twice, with the library before the
change and after, and flashed into the same board. Time on the host proves
nothing for the ESP32. Differences under 0.2 µs a call depend on where the
code lands.

**Tearing.** By eye, on the scenes of "Frame rate". Every firmware to be
judged by eye writes its build number on the screen.

**Builds.** Checked under IDF 5.5.4, 5.5.5, 6.0.1, 6.0.3 and 6.1. On the
board, code 0.5.3 and newer ran under IDF 6.0.1 in all modes, and the
two-buffer mode also under 5.5.5. Only built, without warnings: the model
check bench with two buffers under 5.5.4, 6.0.3 and 6.1; the variant with the
2D DMA and the PPA off under 5.5.5, 6.0.1 and 6.1 (one option then); the variant for chip
revision 3 and above with `on_frame_buf_complete` under 5.5.5 and 6.0.3; the
`examples/dsi_demo` example under all five versions; the other nine examples
under 5.5.5 and 6.0.1. The `dsi_demo` example with the measured numbers kept
in NVS ran on the board; with the aiming of the picture's end it was only
built.

## How it changed

| Release | What changed in the DSI bus |
| --- | --- |
| 0.5.0–0.5.2 | the bus, the JD9165 driver, three strategies, the catch-up copy by tiles of 32x8, the 2D DMA and the PPA. The bus took commands 0x2A, 0x2B and 0x2C for window commands from the moment it received the frame buffer, that is before the command table, and those three registers never reached the panel; the panel worked nonetheless. Every batch waited for the swap of buffers: 20.4 pictures a second each for three tasks |
| 0.5.3 | window commands are taken only after `dgx_dsi_video_start()`; batches of several tasks are joined in one frame; the large catch-up copy after the swap; the counters `dgx_dsi_counters()`; `dgx_dsi_frame_us()` and `dgx_dsi_frame_ended()`; the board measurement `dgx_dsi_bench()`, `dgx_dsi_tune()`, `dgx_dsi_tuning()`; the 32x4 tile by default and `dgx_dsi_tiles()`; the safety pause in the last 2 ms of a frame removed where the end of frame is reported at the moment the buffer is chosen |
| 0.5.4 | an 8-bit screen through a colour table goes in strips through `write_rows`; blending of two colours by the accelerator; the flags `DGX_LUT_EXACT` and `DGX_LUT_TRANSPARENT` |
| 0.5.5 | `dgx_scroll()` on DSI pixel by pixel through the screen: right, but half a second for the whole screen |
| 0.5.6 | the bus function `move`: scroll by rows of the frame buffer |
| 0.6.2 | the JD9365 driver and the JD9365 (10"), the first board with a revision 3 chip; the PSRAM margin check in the board measurement, the fields `fifo_drops`, `lanes_percent`, `psram_mhz`, `chip_revision`; `dgx_dsi_advise()` |
| 0.6.3 | the option `CONFIG_DGX_DSI_HARDWARE_COPY` became `CONFIG_DGX_DSI_PRIVATE_DMA2D` and turns off only the 2D DMA through the private interface; the PPA is built with no option. `dgx_dsi_advise()` keeps one flag, `DGX_DSI_ADVISE_TWO_FRAMES`; the rest is words in `dgx_dsi_tuning_text()` and in the log of the measurement |
| 0.6.4 | the bus function `write_indexed`: an 8-bit screen through a colour table goes to the panel in its own order by the 2D DMA, in every orientation, and with `DGX_LUT_SCALE(n)` every pixel a square of n by n, smoothed by Scale2x with `DGX_LUT_SMOOTH`; the strips through `write_rows` are gone |
| 0.6.5 | the smoothed rows of `write_indexed` faster: a full smoothed frame of the game 27 ms instead of 34 |
