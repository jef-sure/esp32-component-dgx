# A MIPI-DSI Display in DGX: Frame Buffer, Scan and Tearing

[Русская версия](dsi-display-ru.md)

A note on how the MIPI-DSI bus for the ESP32-P4 is made in DGX, what was
measured on it, and why the screen is updated the way it is.

It describes the code of version 0.5.4: the bus `src/bus/dsi_esp32.c`, the
frame buffer core `src/bus/fb_area.c`, the tile maps `src/bus/fb_tiles.c`,
the change of the two buffers `src/bus/fb_flip.c`, the driver
`src/drivers/jd9165.c`.

Every number here is either measured on the board or marked as calculated. A
KB is 1024 bytes.

## What it was measured on

| What | Value |
| --- | --- |
| Board | Guition JC1060P470, 7 inches |
| Chip | ESP32-P4 revision v1.3 as `esptool` reports it, two cores |
| Processor frequency | 360 MHz, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360`: the default for revisions below 3 |
| Build for the revision | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, minimum revision 0 |
| Panel | JD9165, 1024x600, 16 bits a pixel unless said otherwise |
| Panel driver | `dgx_jd9165_init()` of DGX, `src/drivers/jd9165.c` |
| Bus | `dgx_dsi_init()` of DGX, `src/bus/dsi_esp32.c` |
| Under the bus | the MIPI-DSI driver of the `esp_lcd` component of IDF: `esp_lcd_new_dsi_bus()`, `esp_lcd_new_panel_io_dbi()` for commands, `esp_lcd_new_panel_dpi()` for video mode |
| PSRAM | 32 MB, HEX mode, 200 MHz |
| Second-level cache | 256 KB, a line of 64 bytes, unless said otherwise |
| Optimization | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` |
| IDF | measurements under 5.5.4, 6.0.1 and 6.1, each table says which; checks also under 5.5.5 and 6.0.3 |

The Espressif component `espressif/esp_lcd_jd9165` is not used in DGX. Only a
diagnostic bench used it, to show the panel with the factory table before the
driver of DGX worked.

The chip revision is not a formality here. It decides at which moment the IDF
driver reports the end of a frame, see "The scan". All timings and all checks
for tearing were made on revision v1.3. There was no board with a chip of
revision 3 or later.

## Terms

- **Frame buffer**, or just buffer: memory in PSRAM, 1024 × 600 × 2 = 1228800
  bytes at 16 bits, that the chip sends to the display all the time.
- **Frame**: one pass of the display over the frame buffer, 16.4 ms.
- **Scan**: the place of the frame buffer the display is reading right now.
  A liquid crystal panel has no beam, but it is read like one, row after row
  from the top.
- **Window**: a rectangle of the screen the drawing code sets in order to
  send pixels into it one after another. More below.
- **Batch**: a set of changes of the screen that are shown together: windows
  with pixels, filled rectangles, single pixels. More below.
- **Tile**: a cell of a fixed grid the bus cuts the panel into, 32x4 pixels.
  Needed with two buffers only. More below.
- **Virtual screen**: a screen of DGX that lives in memory and is put to a
  display whole or in part (`dgx_vscreen_to_screen()`).
- **Tearing**: a frame in which a part of the screen is shown new and a part
  old.

### Window

A window is how DGX writes to any display that has a controller. First two
commands set a rectangle: columns from and to, rows from and to. Then pixels
follow, each next to the one before it, and at the end of a row the next one
goes to the start of the next row of the window. No coordinates travel with a
pixel.

```c
scr->set_area(scr, left, right, top, bottom);  /* the window */
scr->write_area(scr, pixels, bits);            /* pixels one after another, as many as there are */
scr->write_area(scr, more_pixels, more_bits);  /* going on from the same place */
```

Letters, textures and a virtual screen put to a display are drawn this way.
The drawing code sets the window, and it may be anything: one pixel, a
letter, the whole screen. A window does not have to be filled: a new one may
be set before the old one is written to its end.

A pixel and a filled rectangle on the DSI bus go without a window, straight
into the frame buffer.

### Batch

A batch is a set of changes of the screen that have to appear at once. It
holds everything the drawing code has done from one showing to the next: any
number of windows with pixels, of filled rectangles and of single pixels,
anywhere on the screen.

Showing is the business of a function of the screen, `update_screen()`: "show
what has been drawn". For the DSI bus that is bringing data out of the cache,
and with two buffers telling the driver that a picture is ready as well: the
buffers themselves change at the end of a frame. One call of
`update_screen()` ends one batch.

The list of changes is kept nowhere. What is left of a batch is where it has
drawn, and that is kept in two ways:

- by the screen: one rectangle around all the changes of the batch. It is
  what `update_screen()` is given;
- by the DSI bus with two buffers: the tiles the batch has drawn in.

When a batch ends is decided by a counter of nesting the screen keeps:
`dgx_screen_progress_up()` adds one to it, `dgx_screen_progress_down()` takes
one away. There is one rule: if something has been drawn and the counter is
zero, `update_screen()` is called with the rectangle gathered, and the
rectangle is cleared. That is checked after every pixel, after every fill and
at every `dgx_screen_progress_down()`.

What follows from it.

- A line, a circle, a solid circle, text and a virtual screen put to a
  display wrap their own work in a pair of `dgx_screen_progress_up()` and
  `dgx_screen_progress_down()`. If the application has not touched the
  counter, it comes back to zero at the end of every such function, and what
  the function drew is shown on its own.
- If the application has called `dgx_screen_progress_up()` itself, the pairs
  inside do not reach zero. Everything drawn is shown at once, when the
  application calls `dgx_screen_progress_down()`.
- A pixel and a filled rectangle do not touch the counter. With the counter
  at zero every pixel and every rectangle is shown at once. That is why a
  pixel costs 0.47 us with the counter above zero and 4.7 us with it at zero.

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

Tiles are a fixed grid laid over the panel: cells 32 pixels wide and 4 high,
for 1024x600 that is 32 tiles in a row and 150 rows, 4800 tiles in all. They
differ from windows in that a window is set by the drawing code and may be
anything, and the grid of tiles is one and does not change. A program may
give the cell another size, once at its start, see "The size of a tile".

The bus keeps one bit for a tile: whether something was drawn in it. A line
from corner to corner of the screen is the whole screen as a rectangle, and
as tiles only the cells it went through.

Tiles are needed with two buffers only: by them the bus knows what has to be
moved from one buffer into the other. How, see "The catch-up copy by tiles".

## The board

By the description of the shop the board looks like a Waveshare 7B with an
EK79007 controller, but it is not one. What the board is can be seen from its
own firmware: the flash image has the strings `Hello,Guition` and
`esp_lcd_new_panel_jd9165`. All the parameters below were taken from the same
place, the factory firmware, and not from descriptions of others.

| Parameter | Value |
| --- | --- |
| Panel | JD9165, 1024x600 |
| DSI data lanes | 2, at 750 Mbit/s |
| Pixel clock | 52 MHz |
| A row: sync pulse, back porch, front porch | 24, 136, 160 pixels |
| A frame: sync pulse, back porch, front porch | 2, 21, 12 rows |
| PHY power | LDO channel 3, 2500 mV |
| Panel reset | GPIO0: high 5 ms, low 10 ms, high 120 ms |
| Backlight | GPIO23, on at high level |
| Memory | flash 16 MB, PSRAM 32 MB |

The initialization table of 60 commands was taken from the factory firmware
too. It differs from the table that goes around for an older panel.

## What the bus looks like to a driver

A display on DSI works in video mode: the chip sends it a frame buffer from
PSRAM all the time. Commands only set the panel up; pixels are not sent by
them in video mode.

The interface is nevertheless left the same as that of SPI. The bus takes the
commands of the window itself and puts the pixels that follow into the frame
buffer.

| Command | What the bus does |
| --- | --- |
| 0x2A, 0x2B | remembers the columns and the rows of the window |
| 0x2C, 0x3C | writes pixels into the window from its start or from where it stopped |
| 0x2E, 0x3E | reads pixels of the window the same way |
| any other | sends it to the panel over DSI |

The bus takes the commands of the window itself only after
`dgx_dsi_video_start()`. Before that a driver sets its controller up, and
the same numbers may be registers of the controller: JD9165 has the
registers 0x2A, 0x2B and 0x2C on its page 1. In the releases 0.5.0 to 0.5.2
the bus took them for commands of the window from the moment it got a frame
buffer, which is before the table of commands, and those three registers
did not reach the panel. The panel worked all the same. Now they reach it:
at the debug level of the log the bus writes every command it has sent.
On build 159, with them already, the scenes of the bench were looked at
with the eyes, and no change in the picture itself was noticed.

A panel driver on this bus is made of the same parts as a driver of any
display of DGX: a constructor `dgx_jd9165_init()`, a table of initialization
commands that the common `dgx_lcd_init()` sends, and an orientation function.
It differs from a driver of an SPI display by four calls of the bus.

| Call | When | What for |
| --- | --- | --- |
| `dgx_dsi_video_setup()` | before the panel is reset | to give the timings of the video mode and the strategy, to get the frame buffers |
| `dgx_dsi_video_start()` | after the table of commands | to start sending the frame buffer to the display |
| `dgx_dsi_screen_functions()` | at the end of the constructor | to take the drawing functions of the bus, see below |
| `dgx_dsi_orientation()` | in the orientation function | the orientation is carried out by the bus, the panel is not told it by a command |

For the rest of the library this is an ordinary screen behind a bus. Text,
figures and a virtual screen put to a display set a window and send pixels by
the same calls as for SPI. Who takes them, a display controller or the bus,
makes no difference to them.

The orientation is carried out by the bus when it writes, too: a mirror
across, a mirror down and an exchange of the axes, eight combinations.

The drawing functions the bus gives a screen are `set_pixel`, `get_pixel`,
`fill_rectangle` and `update_screen`. They put a pixel and a rectangle
straight into the frame buffer, without a window.

## Byte order

DGX keeps a 16-bit pixel in memory with its high byte first. That is how the
controllers of SPI displays want it, and a buffer goes to SPI as it is, with
no processing. Everything lies in this order: virtual screens, the buffer of
a bus, the rows that fonts and textures prepare.

The frame buffer of the ESP32-P4 keeps the same pixel with its low byte
first, as an ordinary 16-bit number of the processor. It is the same with 24
bits: DGX has red, green, blue in memory, the frame buffer blue, green, red.

| Pixel | In the buffers of DGX | In the frame buffer |
| --- | --- | --- |
| 16 bits, the value 0x1234 | 0x12, 0x34 | 0x34, 0x12 |
| 24 bits | red, green, blue | blue, green, red |

Not one pixel can be put into the frame buffer as it is. The bus exchanges
the bytes when it writes, and that is not seen from outside. But it is no
small thing; it has three costs.

**Time.** Writing a whole buffer of 1024x600 at 16 bits:

| From where | Without the exchange | With the exchange | Added |
| --- | --- | --- | --- |
| From internal RAM, the display not started | 13.7 ms | 17.8 ms | 30% |
| From PSRAM, the display running | 27.1 ms | 30.3 ms | 12% |

**No `memcpy`.** Writing goes by a loop of its own. Its fast variant, two
pixels as one 32-bit word, works only on runs of 16 pixels and longer and
only in the usual orientation. Short runs, a mirrored and a turned screen are
written byte by byte: 21.9 ms a buffer instead of 17.8 ms.

**The hardware way only by a detour.** The 2D DMA of the chip copies a buffer
in 8 ms, and the processor puts a virtual screen to the display in 34 ms. But
the public interface of IDF to the 2D DMA does not exchange the bytes of a
16-bit pixel. The exchange could be had only through the private interface
and one field of a register set directly, see "The 2D DMA and the PPA".

What was considered.

- **To exchange in the bus when writing.** Chosen as the main way. The
  interface stays uniform: the same code draws on SPI, on a parallel bus and
  on DSI, and a virtual screen can be put to any display. The costs are
  listed above.
- **To exchange in hardware, by the 2D DMA.** Done for a virtual screen put
  to the display: large pieces go by the DMA, which exchanges the bytes on
  the way. Neither the format of the data nor the interface changes. It works
  for 16 bits and only when the screen is neither mirrored nor turned.
- **A pixel format of its own for a virtual screen**, with the byte order of
  the frame buffer. Putting it to the display would be a plain copy. But such
  a screen could not go to an SPI display without an exchange, and all the
  code that puts pixels into buffers (fills, fonts, textures, copies between
  screens) would need a second variant. The sources would stop being common
  to all displays. Not done, and not needed after the point above.
- **To exchange in a buffer in between, then to move.** Slower, 23.1 ms
  against 17.8 ms, see "Writing into the frame buffer".
- **To write a pixel as a 16-bit value.** Slower than two bytes, same place.
- **24 bits.** The exchange of red and blue by the 2D DMA is not done: 24-bit
  pixels are moved by the processor.

LVGL made the opposite choice: a pixel lies in memory in the order of the
processor, and for an SPI display the port exchanges the bytes before sending
(`lv_draw_sw_rgb565_swap`). On DSI it pays nothing, on SPI it pays what DGX
pays on DSI.

## The cache and PSRAM

The processor writes into the frame buffer through the cache. The buffer is
sent to the display by a DMA, and it reads PSRAM directly, past the cache.
Three things follow.

First. The cache stands between writing and showing. What the processor has
written into the frame buffer lies in the cache and gets to PSRAM in one of
two cases: the bus has brought it out itself by `esp_cache_msync()`, or the
cache has put a line out to make room. Until then the display shows the old.

The cache puts lines out in an order that suits it, not in the order of
drawing. If drawing goes on for long and nothing is brought out, the picture
appears on the screen in pieces out of order, with holes that fill later.
That is how a gradient drawn by pixels looked.

So the bus brings out what was written itself:

- at the end of a batch, always;
- in the course of a batch, as soon as pixels and fills have written more
  than a quarter of the cache. The picture then appears in the order it is
  drawn. With two buffers this is not done: nobody sees the hidden buffer.

Pixels that go through a window are not brought out in the course of a batch.
They lie in memory one after another, and the cache puts them out in the same
order; there are no holes. Bringing them out by force was tried there and
cost 9% of the time of a whole screen.

Second. A pixel on a new row is a cache miss, about 1.4 us. A row of 1024
pixels is filled in 0.026 ms, a column of 600 pixels in 0.5–0.8 ms depending
on the size of the cache. In portrait orientation every fill turns into
columns and goes slowly.

Third. The second-level cache takes internal RAM, and its size is chosen at
build time by the options `CONFIG_CACHE_L2_CACHE_128KB`,
`CONFIG_CACHE_L2_CACHE_256KB` and `CONFIG_CACHE_L2_CACHE_512KB`. The larger
it is, the faster drawing into PSRAM goes and the less internal RAM is left
to the program.

The table shows this trade. The same firmware was built with three sizes of
the cache. The first row is the internal RAM free at start, before the
display is started. The rest is the time of the same operation on the
display. IDF 5.5.4, one frame buffer.

| Second-level cache | 128 KB | 256 KB | 512 KB |
| --- | --- | --- | --- |
| Internal RAM free | 588 KB | 458 KB | 197 KB |
| Fill 100x100 | 0.40 ms | 0.18 ms | 0.19 ms |
| Fill of the whole screen | 18 ms | 16 ms | 9.5 ms |
| Solid circle of radius 100 | 1.4 ms | 1.2 ms | 0.7 ms |
| Virtual screen 320x100 to the display | 1.7 ms | 1.1 ms | not measured |
| Whole virtual screen to the display | 35 ms | 34 ms | 29 ms |

All the rows of this table were taken when the processor did the filling
and the moving. With the accelerator and the DMA a fill of the whole screen
takes 3.5 ms and a virtual screen put to the display 8 ms, see "The 2D DMA
and the PPA".

A pixel in a batch costs 0.47 us, a pixel on its own 4.7 us: it is brought
out of the cache by itself.

### Writing into the frame buffer

A separate bench without a display: a buffer of 1024x600 is filled from
internal RAM.

| Way | Time |
| --- | --- |
| Byte by byte, with the exchange | 21.9 ms |
| A pixel as one 16-bit value, with the exchange | 23.7 ms |
| Two pixels as one 32-bit word, with the exchange | 17.8 ms |
| `memcpy` without the exchange | 13.7 ms |

Writing a 16-bit value turned out slower than two bytes: an exchange by
shifts costs more than two moves. Writing by words is faster on long runs and
slower on runs of 1–4 pixels, they are equal at about 16 pixels. So runs of
16 pixels and longer are written by words, and only when the screen is not
mirrored, not turned and red and blue do not change places.

A buffer in between was tried as well: first the bytes are exchanged in
internal RAM, then moved to PSRAM. The exchange takes 9.5 ms, the move
13.6 ms, together 23.1 ms against 17.8 ms. An exchange on the way to PSRAM
adds only 4.2 ms to the move.

## The scan

The display reads the frame buffer all the time, row after row. Measured on
the board:

| Quantity | Value |
| --- | --- |
| Frame period | 16418 us, 60.9 Hz |
| Rows in a frame | 635: 600 visible and 35 of blanking |
| One row | 25.86 us |
| Reading speed over the visible rows | 77 KB/ms |

There are two ways to know where the scan is now.

The first: by time. The IDF driver reports the end of a frame, and the bus
remembers that time. On revision v1.3 the row y is read (35.2 + y) × 25.86 us
after the report: 35 are the rows of blanking. The rows 0–4 are read ahead,
right after the end of a frame, so in its calculations the bus allows 6 rows
of room.

The second: the address being read lies in a register of the DMA controller,
`DW_GDMA.ch[0].sar0`. I used it on the bench only. It was not taken into the
library: it is an internal register.

The end of a frame is reported differently. It depends on the DPI driver of
`esp_lcd` (`esp_lcd_panel_dpi.c`) and on the chip revision. The driver did
not change by the number of the major version: the new events came to the
release branches at different times.

| IDF | What the driver has |
| --- | --- |
| 5.4.3, 5.5.4, 6.0.1, 6.0.2 | only `on_refresh_done` |
| 5.5.5, 6.0.3, 6.1 | `on_frame_buf_complete` and `on_vsync`; `on_refresh_done` is deprecated |

I did not look at other versions.

| Event | Chip revision | From where and when |
| --- | --- | --- |
| `on_refresh_done` | below 3 | the DMA interrupt: the last row has been read, and here the driver chooses the buffer for the next frame |
| `on_refresh_done` | 3 and later | the interrupt of the DSI bridge at the vertical sync pulse, later than the choice of the buffer |
| `on_frame_buf_complete` | any | the DMA interrupt, as in the first row |
| `on_vsync` | any | the vertical sync pulse; on a revision below 3 the same DMA interrupt stands in for it |

The bus uses `on_frame_buf_complete` if it is there, otherwise
`on_refresh_done`. Whether it is there is found out at build time by
`CMakeLists.txt`: it looks for the name in the header of the driver,
`esp_lcd_mipi_dsi.h`. The number of the IDF version does not tell it.

For the second row of the table of events the bus has a branch of its own:
the rows of blanking before the first visible one are counted less by the
front porch, and a safety pause is left when the buffers are changed, see
"Frame rate". This branch builds, but was not run on a board.

## Tearing

Tearing happens when the scan and the place being written meet at some row
inside one frame. On one side of that row the display has shown the new, on
the other the old. There is no way to avoid it that would suit every program,
so the constructor of the driver takes a strategy.

```c
dgx_screen_t *scr = dgx_jd9165_init(bus, rst, 16, DgxScreenRGB, DgxDsiTwoFrames);
```

| Strategy | Buffers | Tearing | Cost |
| --- | --- | --- | --- |
| `DgxDsiOneFrame` | 1 | possible | none |
| `DgxDsiTwoFrames` | 2 | none | one more buffer in PSRAM |
| `DgxDsiOneFrameAfterScan` | 1 | none for a virtual screen put to the display and for a large fill | a wait of up to a frame for every such transfer and fill |

### One buffer

`DgxDsiOneFrame`. What is drawn is seen at once. No memory beyond the buffer
is needed. Tearing is possible, and the more visible the larger the part of
the screen that changes and the more sideways the motion.

### Two buffers

`DgxDsiTwoFrames`. The IDF driver can keep up to three frame buffers and
change which of them goes to the display. It allocates the buffers itself;
one's own cannot be given to it. The change takes effect at the end of a
frame, never in the middle of one, so there is no tearing by construction.

From here on the buffer that goes to the display now is called the shown one,
and the other the hidden one.

1. Drawing goes into the hidden buffer.
2. At the end of a batch the bus asks the driver to show this buffer from
   the next frame on. The picture is ready and not shown yet.
3. The first drawing of the next batch looks whether the display has taken
   the picture. If it has, the buffers change roles. If not, the batch is
   either drawn into the same picture or waits for the change, see "Several
   tasks".

The cost:

- one more buffer in PSRAM, 1.23 MB at 16 bits;
- a drawing function called without `dgx_screen_progress_up()` is a batch of
  its own, and it may get on the screen without the one after it. Everything
  that has to appear together has to be put into a batch;
- the bus does not set the pace, see "Several tasks";
- the update rate can only be 60, 30, 20 or 15 times a second, by how many
  frames a batch takes. That is a property of any strategy without tearing:
  every frame shown is whole. Values in between are possible only with
  tearing;
- after the change the hidden buffer does not hold what the last batch drew:
  that one drew into the other buffer. It has to be copied in. From here on
  that is called the catch-up copy; see "The cost of copying" and "The
  catch-up copy by tiles".

#### Several tasks

At first the first drawing after a batch always waited for the change: a
batch a frame. But a program may be made of several tasks that know nothing
of one another: each has a part of the screen of its own, and they draw in
turn under a common mutex. Then every change of the buffers went to one task.
Three tasks got a third of the frames each, 20.4 pictures a second, see the
table below.

Now the end of a batch does not show the picture, it only asks the driver to
show it from the next frame on. The IDF driver chooses the buffer for the
next frame at the end of the current one and takes the one it was asked for
last. As long as the frame has not ended, the request can be replaced by
another. Everything is built on that.

Say buffer A is on the screen. The first task has finished its batch in
buffer B, and the bus has asked the driver to show B. Until the frame ends B
stays hidden. Now a batch of the second task comes. Where is it to be drawn?

- Not into A: A is on the screen, and what is half-drawn would be seen.
- Into B it may go, while B is hidden. But if the frame ends in the middle
  of the drawing, the display changes to B and shows the batch half-drawn.

So the bus removes that danger first and draws afterwards:

1. It asks the driver to stay with A. Now the end of the frame spoils
   nothing: A stays on the screen.
2. It checks that the request was not late: no frame has ended meanwhile,
   and more than 2 ms are left of the current one. If a frame ended at that
   very moment, the display may have taken B already, and B must not be
   drawn into. Then the bus asks for B again, waits until B is on the
   screen, and the batch goes into A, as it would with one task.
3. It draws the batch of the second task into B. The batch of the first
   task lies there already.
4. At the end of the batch it asks the driver to show B again.

At the end of the frame the display changes to B, and both tasks are on the
screen at once. A third task, if it comes before the frame ends, is added
the same way. Everything several tasks drew in the time of a frame is shown
by one change of the buffers.

What happens to a batch that comes when a picture is ready:

| What the batch finds | What happens to it | Why |
| --- | --- | --- |
| The display has taken the picture | the buffers change roles, the batch is drawn at once | nothing to wait for |
| The display has not taken the picture, there is time enough before the frame ends | the batch is drawn into the same picture | this is the joining |
| Little time is left before the frame ends | it waits for the change | a batch that is being drawn when a frame ends keeps the whole picture back a frame |
| The picture has missed one end of a frame already | it waits for the change | a ready picture does not wait longer than one extra frame |
| A frame ended at the moment the bus was asking the driver for a buffer | it waits for the change | whether the display took the picture is not known at that moment |

The details.

- The bus does not ask who draws. A batch of the same task gets into the
  picture just as a batch of another one does.
- The bus does not set the pace. A program that draws more often than the
  display shows frames gets the last of what it drew into every frame and
  drew the rest for nothing: it has to hold itself back. How long a frame
  takes the bus tells it, see "The time of a frame". In release 0.5.2 the
  pace came by itself, for every batch waited for the change.
- Nor does the bus set the order of the tasks. If every task has to get into
  every frame, the program sees to that.
- "Time enough" means: more is left before the frame ends than the longest of
  the last batches took, and 2 ms more. The bus times a batch itself, from
  the first drawing to `update_screen()`. A long batch is forgotten by an
  eighth with every batch that follows. There is one estimate for the bus: a
  rare long batch among frequent short ones gets forgotten.
- The 2 ms are room for inaccuracy: the end of a frame is foretold from the
  time of the last one, and the interrupt may have come late.
- The wait for the change happens inside a drawing function, that is under
  the mutex of the application, if there is one. It was so before as well.
- Drawing from several tasks at once is still not possible, the screen is not
  made for it. The mutex stays with the application. What a screen keeps,
  why it takes one task at a time and how to build a program with several
  tasks that draw is told in the README, section
  [Drawing from several tasks](../README.md#drawing-from-several-tasks).
- The maps of tiles change places not at the end of a batch but when the
  buffers change roles. At the end of a batch the hidden buffer only gets
  what it lacks.

A known weakness. If the task is put aside for longer than a frame twice in a
row, right between its calls to the driver, the display may go back to the
picture before for one frame: it took B, the request to stay with A came
late, and the repeated request to show B came late as well. Nothing
half-drawn is seen then.

The bus cannot mend that, but a program can make it rare: it is a matter of
the priorities of the tasks.

- A task that draws is held up for a frame and longer only by a task of a
  higher priority that computes that long without a pause. The drawing task
  needs a priority not lower than such tasks have.
- Tasks of equal priority FreeRTOS runs in turn, a tick each. With a tick of
  1 ms, as in the examples of DGX, it takes 16 ready tasks to make a frame.
  With a tick of 10 ms, the default of IDF, two are enough.
- If several tasks draw under one mutex, all of them need the priority. A
  task of a low priority that is put aside in the middle of a batch holds
  the mutex, and the others wait with it. A mutex of FreeRTOS raises its
  priority to that of the waiting task meanwhile, a semaphore does not.
- The estimate of how long a batch takes depends on the same: a batch that
  was put aside takes longer than expected, and a ready picture misses the
  end of a frame.

#### The copy after the change

The joining of batches ran into the catch-up copy. A ready picture had to be
whole at the moment the driver is told of it, and it is told at the end of
every batch: the bus has no other moment. So already at the end of the first
batch of a frame the hidden buffer was given all it lacked. With three tasks
that have bands of 1024x168 those are the bands of the other two tasks, about
590 KB a frame, 357 MB in 10 seconds. A few milliseconds later those tasks
redrew their bands whole, and the copy turned out to be wasted. The first
batch of a frame with the copy took about 10 ms, and the third no longer
fitted into the frame.

The first thought was to copy just before the frame ends, by a timer. But the
code of a timer does not run in a task that draws, and the bus would need a
lock of its own between the two. The condition itself was loosened instead.
The display reads a buffer row after row, and what has to be whole is not the
picture at the moment the driver is told, but a row at the moment the scan
comes to it. The 2D DMA copies twice as fast as the scan reads, so a copy
started in the blanking stays ahead of the scan. It is the same means as in
the strategy behind the scan, only put to the catch-up copy.

How it is made.

1. At the end of a batch the large pieces that are lacked are not copied, if
   by the measured speed of the DMA they are sure to be ahead of the scan.
   The bus keeps them as a list, up to six, and in the maps of tiles they
   stay marked as lacked. What is small, and what is large but would not be
   in time, is copied at once, as before.
2. If the next batch goes into the same picture and redraws a piece whole,
   it does not have to be copied any more: at the end of that batch the list
   is made anew.
3. When the display has taken the picture, the interrupt of the end of the
   frame puts the first piece into the queue of the 2D DMA, and the handler
   of the end of a copy puts the next one. The pieces go one at a time, from
   the top down. The hardware copies: there is neither a task nor a lock for
   it.
4. After the change drawing goes into the buffer the copy reads from. A batch
   that is about to write over a piece not copied yet waits for that piece.
5. A task that waits for the change copies the pieces itself, if it can
   before the frame ends. Then there is nothing to wait for after the change.

The details.

- Who owns the list. Before the driver is told the bus sets a mark. Only one
  can take it, by an atomic exchange: either the interrupt of the end of the
  frame, which then starts the copy, or a task that draws into the same
  picture or copies the pieces itself.
- The top of the screen. The display reads the first rows of a frame as soon
  as the one before has ended: it comes to row 4 in 0.7 ms. Nothing gets
  ahead of the scan there, so of a piece that starts at the upper edge the
  first 8 rows are copied at once.
- Why one at a time. The queue of the DMA takes two pieces at once, and each
  goes half as fast: 83 KB/ms against the 77 KB/ms of the scan.
- A transfer by the DMA and a fill by the PPA that a task starts during the
  copy slow it down the same way. The bus reckons beforehand whether the
  pieces stay ahead of the scan at that speed. If they do, only one who
  writes over a piece waits. If not, a transfer and a fill wait until the
  whole copy is over.
- The cache. The DMA writes to memory past the cache, so before the driver is
  told the bus brings out of the cache and makes it forget the rows under the
  pieces. That costs 8 to 19 us a piece whatever its size.
- Reading. A pixel read from a ready picture where a piece is not copied yet
  brings its tile by the processor, as any tile that is lacked.
- Which piece will be in time. A piece starts when the pieces above it are
  done. It has to start before the scan comes to its first row and to end
  before the scan has passed its last one, by the measured speed with a
  quarter to spare. Until a speed is measured all pieces are copied at once.
- 24 bits. A row is half as long again in bytes there, the scan reads bytes
  half as fast again, and the DMA copies as it did. Pieces fell behind the
  scan even with the check: 1 to 5 in 300 batches. So at 24 bits the copy
  after the change is off, everything is copied at the end of a batch.
- The safety net. A piece that is not copied in a quarter of a second is
  copied by the task, and a warning is left in the log. On the bench it did
  not come to that.

What of this the bus measures itself, and what is set by a number in the
code:

| Quantity | Where it comes from |
| --- | --- |
| The speed of a copy by the 2D DMA | measured on every piece of 64 KB and more left for later and copied undisturbed; the slowest of the last ones is reckoned with. On the bench from 117 to 170 KB/ms, the counter `copy_kb_a_ms`. Until a piece has been measured, the number of the measuring of the board is taken, if the program gave it |
| The speed of the scan | from the period of a frame, which the bus measures, and the number of rows of a frame with the blanking |
| The length of a batch | measured on every batch |
| How many times a second transfer slows a copy down | set: 2.5 times. Twice was measured |
| How many rows the display reads beforehand | set: 6 |
| The room for starting a copy | set: 0.4 ms. 16 to 37 us were measured. The measuring of the board gives 0.12 to 0.15 ms |
| The room before the end of a frame | set: 2 ms |
| The thresholds from which a piece goes by the DMA and the PPA, and the speeds of writing by the processor in the strategy behind the scan | set by numbers taken on this board. The measuring of the board takes them again |

The numbers that are set were taken on an ESP32-P4 of revision 1.3 with
PSRAM at 200 MHz and a panel of 1024x600. The bus itself adjusts only the
first three. The thresholds, the speeds of writing and the room for
starting a copy are taken for another chip, another memory or another
display by `dgx_dsi_bench()`, see "Measuring the board". Three numbers of
this table nobody measures: the slowing by a second transfer, the rows the display
reads beforehand, and the room before the end of a frame.

What this changed in the work time of a batch. Two buffers, IDF 6.0.1, the
rows are those of "The working time of a batch", measured the same way. The
other rows of that table did not change.

| Batch | Release 0.5.2 | With the copy after the change |
| --- | --- | --- |
| A square 48x48 after a batch that filled the whole screen | 8.49 ms | 0.56 ms |
| A band of 200 rows, even batches in one place, odd ones in another | 5.49 ms | 2.79 ms |
| 320x100 from a virtual screen, in two places the same way | 1.09 ms | 0.55 ms |
| A bar 40x548, in two places the same way | 1.03 ms | 0.28 ms |
| A square 64x64, in two places the same way | 0.28 ms | 0.15 ms |

The cost in memory: the list of pieces, their descriptors and transfers take
0.85 KB of internal RAM, only with two buffers and only when the option of
the DMA and the PPA is on.

#### Three tasks on the bench

The bench starts three tasks. Each redraws moving bars in a virtual screen of
its own and puts it to the display under a common mutex. What is counted is
how many pictures a second each task has put. IDF 6.0.1.

- Scenes 7 and 8: bands of 1024x168 one under another, the bars are erased
  and drawn whole every time.
- Scenes 9 and 10: parts of 320x100 side by side.
- Scenes 11 and 12: the same bands of 1024x168, but only the edges of the
  bars are redrawn, so a task spends little on its picture, while the whole
  band goes to the display.
- In the scenes 7, 9 and 11 the tasks keep the pace of the display by their
  own clock, a picture a frame. In the scenes 8, 10 and 12 they sleep 7, 14
  and 21 ms after every picture instead.

| Scene | Release 0.5.2 | Joining, the copy at once | Joining, the copy after the change |
| --- | --- | --- | --- |
| 7. Three bands of 1024x168, at the pace of the display | 20.4, 20.3, 20.3 | 34.8, 35.2, 34.8 | 40.2, 41.9, 40.1 |
| 8. The same with pauses | 20.4, 20.3, 20.2 | 37.0, 30.6, 24.2 | 42.1, 30.6, 23.3 |
| 9. Three parts of 320x100, at the pace of the display | 20.4, 20.4, 20.4 | 61.0, 61.0, 61.0 | 61.0, 61.0, 61.0 |
| 10. The same with pauses | 20.4, 20.4, 20.3 | 61.0, 60.9, 40.6 | 121.8, 60.9, 45.5 |
| 11. Three bands of 1024x168, little redrawn, at the pace of the display | 20.4, 20.3, 20.3 | 40.6, 40.8, 40.5 | 60.6, 60.8, 60.5 |
| 12. The same with pauses | 20.4, 20.4, 20.0 | 47.8, 33.2, 30.2 | 59.2, 39.5, 30.8 |

What is behind the numbers.

- Before the change every change of the buffers went to one task, hence 20.4
  for all of them in every scene.
- Scene 11 shows the limit of the bus: all three bands of the whole width get
  into every frame. The counters over its 10 seconds: 615 frames, 611 of
  them showed something new, 1825 batches, 1214 of them went into a ready
  picture, no end of a frame missed. 0.7 MB of catch-up copy against 411 MB
  with the copy at once, nearly all of it after the change: nearly always
  all three tasks were in time to redraw their own.
- The scenes 7 and 8 are held not by the bus but by the redrawing itself: the
  processor draws the bars in PSRAM, and two of the three tasks share a core.
- Scene 10: the first task sleeps 7 ms and draws 121.8 pictures a second,
  two a frame. The bus does not hold it back, and every second picture of
  it does not get to the screen. The variant with the copy at once still
  held back a task that draws twice in one frame, hence 61.0 in the middle
  column.
- In the scenes 10 and 12 the third task sleeps 21 ms after every picture and
  cannot draw more often than 46 times a second.
- In no scene did a piece fall behind the scan: the counter
  `copied_late_behind` stayed zero.

The scenes 1 to 6, where one task draws, did not change: 42.3, 60.9, 34.8,
60.9, 60.9 and 60.9 against 42.7, 60.9, 34.7, 60.9, 60.9 and 60.8 in release
0.5.2.

The check of correctness. A model of the display on the host,
`test/host/test_fb_flip.c`: the display there takes at the end of a frame
the buffer it was asked for last, tasks draw batches in turn, every call takes a random
time, and a frame may end in any of them. It is checked that a frame never
starts from a buffer that is being drawn into, that with one task without
pauses every frame shows something new, and that with three tasks batches
of each get into every frame. The maps of tiles
with pieces left for later are checked by `test/host/test_fb_tiles.c`. On the
board: random batches against a model, drawn by three tasks in turn, see "How
it was checked". The scenes 7 to 10 were looked at with the copy at once and
look right to the eye. With the copy after the change and the numbers of
the measuring of the board all twelve scenes were looked at on build 159.
The scenes 1, 2, 4 to 6 and 8 to 10 look right, in scene 11 all three bands
move evenly. Scene 3 jerks a little. In scene 7 the top band moves evenly,
the two lower ones a little unevenly. Scene 12 jerks a little. Where that
comes from see "The time of a frame", "Evenness".

#### Counters

How many pictures a program made does not tell what the display showed:
behind the scan the bench updated a band 487 times a second at 60.9 frames.
So the bus counts. `dgx_dsi_counters()` gives the counters as they are, the
program takes the difference over a second.

| Counter | What it counts |
| --- | --- |
| `frames` | frames sent to the display. Counted with two buffers and behind the scan |
| `batches` | batches in which something was drawn |
| `flips` | changes of the buffers, that is frames that showed something new |
| `joined` | batches that went into a picture already ready |
| `kept_back` | ends of frames a ready picture missed because of a batch being drawn into it |
| `flip_waits`, `flip_wait_us` | how many times a batch waited for the change and how many microseconds in all |
| `scan_waits`, `scan_wait_us` | behind the scan: how many times a write waited for the scan and how many microseconds in all |
| `copied_bytes` | bytes of the catch-up copy, all of them |
| `copied_late_bytes` | of them, copied after the change, ahead of the scan |
| `copied_late_behind` | pieces of such a copy that ended when the scan had passed their last row already. It has to stay zero |
| `copy_waits`, `copy_wait_us` | how many times drawing waited for such a piece and how many microseconds in all |
| `copy_kb_a_ms` | not a count: the speed of a copy by the 2D DMA in KB/ms, as the bus has measured it. 0 until it has |

All counters are 32-bit and go round. Bytes written by the processor, the DMA
and the PPA are not counted.

#### The time of a frame

No more pictures a second can be shown than the display has frames. How long
a frame takes the bus tells: `dgx_dsi_frame_us()` gives microseconds, 16417
on the bench, which is 60.9 frames a second. When the last frame ended
`dgx_dsi_frame_ended()` tells. By those two numbers a program keeps the pace
of the display itself, see "How a program keeps the pace" below.

The bus does the rest. A batch finished before the end of a frame is shown
from the next frame on. Batches of several tasks get into one picture. What
a picture lacks is copied after the change.

Where the number comes from.

| When | What is given | On the bench |
| --- | --- | --- |
| Right after the display is started | reckoned from the timings of the panel: the pixels of a row with the blanking, times the rows of a frame with the blanking, divided by the pixel clock | 1344 × 635 / 52 MHz = 16412 us |
| After 32 frames, half a second | the time of 16 frames in a row, divided by 16 | 16417 us |
| After 256 frames | the time of 256 frames in a row, divided by 256. Renewed every 256 frames | 16417 us |

The reckoning is 0.03% off the measurement: the real pixel clock is a
little lower than the one asked for. The measurement was checked by the
counter `frames`: 600 frames took 9850437 us, 16417.4 a frame.

The bus counts frames with two frame buffers and behind the scan. With one
buffer it does not, and the number stays the reckoned one.

What this does not give.

- More than a microsecond of precision. 16417 for 16417.4 makes a frame of
  difference in 11 minutes: once in 11 minutes two pictures of the program
  fall into one frame, and the display does not show the first of them.
- A wait. The bus tells when a frame ended, but it does not wait for the end
  of a frame itself and does not wake a program. A program sleeps itself.

Evenness. A picture moves evenly when every next one comes after the same
number of frames. The bench counts that for every source: how many frames
of the display went by from one picture of it to the next. Two buffers, 10
seconds a scene. The scenes 3, 7, 11 and 12 were counted on build 160 and
looked at on build 159. The scenes 13 and 14 were counted and looked at on
build 192.

| Source | Pictures a second | After 1 frame | After 2 frames | After 3 frames | To the eye |
| --- | --- | --- | --- | --- | --- |
| Scene 3, bars by primitives | 34.8 | 86 | 262 | 0 | jerks a little |
| Scene 7, the top band, a step of 2 pixels | 40.0 | 192 | 206 | 2 | even |
| Scene 7, the middle band, a step of 3 pixels | 42.5 | 243 | 183 | 0 | a little uneven |
| Scene 7, the bottom band, a step of 4 pixels | 40.0 | 196 | 199 | 5 | a little uneven |
| Scene 11, each of the three bands | 60.9 | 609 to 610 | 0 | 0 | even |
| Scene 12, the top band | 58.5 | 562 | 24 | 0 | jerks a little |
| Scene 12, the middle band | 39.0 | 179 | 203 | 8 | jerks a little |
| Scene 12, the bottom band | 30.8 | 20 | 275 | 13 | jerks a little |
| Scene 14: as 3, but a picture every second frame | 30.5 | 39 | 227 | 38 | right |
| Scene 13: as 7, but every second frame, the top band | 30.5 | 17 | 271 | 17 | right |
| Scene 13, the middle band | 30.4 | 0 | 304 | 0 | right |
| Scene 13, the bottom band | 30.4 | 66 | 171 | 67 | jerks slightly |

- It is uneven where a source cannot make a picture a frame: gaps of one
  frame and of two come mixed. The bus chooses nothing here, it shows a
  ready picture in the nearest frame.
- The top band of scene 7 comes as mixed as the lower ones and looks even:
  its bars move 2 pixels a picture, those of the lower ones 3 and 4.
- A program can even it out: ask for a picture not every frame but every
  second one, that is, reckon the time by twice `dgx_dsi_frame_us()`. Scene
  14 is scene 3 at such a pace, scene 13 is scene 7. To the eye scene 14 and
  the two upper bands of scene 13 move right.
- It does not come out quite even so. The bottom band of scene 13 has gaps
  of one frame and of three 66 times each of 304, and that is seen. Its
  batch ends at the very end of a frame and gets now into this frame, now
  into the next. That is the phase a program does not know.
- The phase differs from run to run: it depends on where in a frame the
  scene started. On build 160 all the gaps of scene 14 were two frames, and
  the bottom band of scene 13 had 46 uneven ones of each kind.

How a program keeps the pace. A program has to hold itself back, and
sleeping for the time of a frame is not enough for that: where in a frame
its picture is finished stays a matter of chance. A picture has to be
aimed: finished a little before a frame ends. A program has all it takes.

1. How long its picture takes it measures itself: from the start of the
   drawing to the end of the batch. It reckons with the longest of the last
   ones.
2. The frame the picture is to get into it chooses itself: the one after
   the last, or the one after that when a picture with room does not fit a
   frame.
3. It starts "the time of a picture plus 4 ms" before the end of that
   frame. The ends of frames it knows: the end of the last one plus a whole
   number of frame times.

```c
int64_t ended;
if (dgx_dsi_frame_ended(bus, &ended)) {
    /* to be finished 4 ms before the next frame ends */
    int64_t from = ended + dgx_dsi_frame_us(bus) - 4000 - picture_takes_us;
    while (esp_timer_get_time() < from) vTaskDelay(1);
}
```

In full, with the choice of the frame and with what to do when late, it is
the function `demo_pace()` of the example `dsi_demo`, a dozen and a half
lines. With one buffer the bus does not count frames, the call returns
`false`, and the pace is kept by the clock.

Three ways were compared on the bench, build 194, two buffers, 10 seconds a
scene. The cells tell how many times the next picture of a source came
after one, two and three frames.

| Source | By its own clock | Started right after a frame ended | Finished 4 ms before a frame ends |
| --- | --- | --- | --- |
| One task, bars by primitives, a picture every second frame | 76, 153, 75 | 76, 154, 74 | 0, 302, 1 |
| Three tasks, parts of 320x100, a picture a frame | 609, 0, 0 | 609, 0, 0 | 583 to 608, 0 to 13, 0 |
| Three tasks, bands little redrawn, a picture a frame | 608, 0 to 1, 0 | 589 to 596, 7 to 10, 0 | 431 to 442, 83 to 87, 0 to 2 |
| Three tasks, bands redrawn whole, every second frame | 2 to 67, 170 to 300, 2 to 67 | 51 to 72, 160 to 202, 51 to 72 | 35 to 48, 193 to 224, 42 to 59 |

- One task. By the clock evenness is chance: in four runs of one and the
  same scene 1, 3, 77 and 151 gaps of 304 were uneven. Starting right after
  a frame has ended is bad every time: a picture of this scene takes 17 to
  18 ms, about a frame, and its end falls on the end of a frame by itself.
  Aiming the end of a picture gives exactly two frames between pictures.
- Several tasks that know nothing of each other and draw under a common
  mutex. Aiming the end does not suit them: all of them aim at one and the
  same moment and draw in turn, and the time of a picture has the wait for
  the mutex in it. The light bands make 52 pictures a second so instead of
  61. As long as all of it fits a frame, such tasks do better to start right
  after a frame has ended, or to keep the pace by the clock.
- The heavy bands do not fit two frames with room, and no way makes them
  even.
- The recipe above is for a program where one task draws the whole picture.
  Several sources come down to that case, see below.
- Two scenes were looked at with the eyes on build 194: three light bands
  started after a frame has ended move right, three heavy bands by the
  clock look better than on build 192.

Several sources without a mutex. A mutex guards data, and here one can do
without shared data: one task draws, the others send it messages. On the
bench those are the scenes 23 to 26, build 195. Every source redraws its
virtual screen, puts a message "ready" into a queue and does not touch the
screen until it gets the answer that the picture was taken. The task of the
display takes from the queue all that has come, puts it to the display as
one batch and aims the end of that batch 4 ms before the end of a frame, as
any single drawing task does. Its priority is above that of the sources.

| Three sources | Each draws itself under a mutex, the pace by the clock | Messages to one task that draws |
| --- | --- | --- |
| Parts of 320x100, a picture a frame | 609, 0, 0 | 607, 0, 0 |
| Bands little redrawn, a picture a frame | 608, 0 to 1, 0 | 604 to 606, 0 to 1, 0 |
| Bands redrawn whole, every second frame | 1 to 53, 198 to 302, 1 to 53 | 1, 302, 0 |
| Bands redrawn whole, a picture a frame | 192 to 243, 183 to 206, 0 to 5 | two of them 0 to 2, 302 to 303, 0; the third 196, 205, 0 |

- The heavy bands every second frame became even for all three sources: 302
  gaps of 303 are two frames. Under a mutex no way gave that.
- There are no waits for the display at all, and batches do not join, for
  there is nothing to join: one batch comes in a frame. 0.5 MB of catch-up
  copy in 10 seconds with the light bands.
- The last row shows the limit. A source cannot make a picture a frame, and
  there are no wonders: two sources that share a core settled on an even two
  frames by themselves, and the third, alone on its core, makes 40 pictures
  a second and comes mixed. The pace for such sources is the program's to
  choose: every second frame, as in the row above.
- This way lets a program keep clear of inversion of priorities. The sources
  do not wait for each other, each waits for the drawing task only. That is
  why its priority is above that of the sources: with a lower one it would
  itself be what a more important task waits for.
- So the question of how several sources hold themselves back comes down to
  one drawing task. The way with a mutex still works, but how even it is
  depends on how the tasks happen to share the time.

### Behind the scan

`DgxDsiOneFrameAfterScan`. One buffer, no memory beyond it. The idea is to
write when the scan is not in the way. It is a compromise: a picture that
comes from a virtual screen is shown without tearing, and drawing straight
on the display is protected only in part.

It works in three ways, by what does the writing.

#### A transfer by the 2D DMA

That is how a part of a virtual screen of 16 KB or more goes on a screen that
is not turned, see "The 2D DMA and the PPA". The DMA writes at 150 KB/ms, the
scan reads at 77 KB/ms: writing is twice as fast as the scan. So it is enough
to start when the scan is outside the rows of the part. If it is above the
part, it enters it later than the writing and does not catch up. If it is
below, all the more. Writing cannot overtake the scan either: it does not
start behind it.

Before it starts the DMA the bus waits until the scan has left the rows of
the part, and starts it. For the whole screen "outside the part" is the 35
rows of blanking, 0.9 ms a frame: the bus waits for them and moves the whole
screen in 7.9 ms, staying ahead of the scan.

A part of any size goes without tearing that way, the whole screen as well,
on one buffer.

#### A fill by the accelerator

A large rectangle is filled by the accelerator, the PPA, four times as fast
as the scan. The rule is the same: the bus waits until the scan has left the
rows of the rectangle, and starts the fill. The rectangle appears whole.

That has a consequence. A tall rectangle takes nearly all the rows of the
screen, and its fill can be started only in a short gap, while the scan is in
the blanking and in the rows above the rectangle. If a batch has many
rectangles, they do not fit one gap: some are filled in this frame, the rest
in the next one. The batch stops being one.

On the bench that is the third scene: nine bars move, a strip is erased at
the left of each and drawn at the right, 18 fills 548 rows high in all. About
ten fit one gap. A bar whose strip at the left is erased already and whose
strip at the right is not drawn yet is shown narrower for one frame. By eye
that is a twitching of the width of a running bar. There is no tearing in it.

What has to appear at once has to be drawn in a virtual screen and put to the
display in one piece, or two buffers have to be taken.

#### Writing by the processor through a window

That is how parts smaller than 16 KB go, text, textures, 24 bits, and
anything on a mirrored or a turned screen. The processor writes slower than
the scan reads, so the rule is another here. Before it writes pixels into a
window the bus makes four steps.

1. It counts how long the writing takes: the bytes of the window divided by
   the writing speed.
2. From the time since the end of the frame it works out where the scan is.
3. If the scan is in the rows of the window, or will enter them before the
   writing ends, it waits until the scan has left them.
4. It writes.

At the end of a batch what is left in the cache is brought out the same way,
with a wait for the scan.

Step 3 allows writing only in the gap when the scan is not in the rows of the
window: from its leaving the last row to its entering the first. If the
writing does not fit that gap, the bus does not wait at all and writes at
once, as with `DgxDsiOneFrame`. The writing speed in the code is taken with
room, 28 KB/ms; the one measured on a whole screen is 36 KB/ms. A band of up
to 165 rows of the full width fits. For another board `dgx_dsi_bench()` takes
this speed, see "Measuring the board": on the bench it gets 34 KB/ms and
gives the bus a quarter less, 24 to 25.

This condition is stricter than it has to be. The scan and the writing go the
same way, and writing may start as soon as the scan has passed the first row
of the window: it gets in the way only in the next frame, when it comes back
from the top and catches up with the place being written.

| | As in the code | As is enough |
| --- | --- | --- |
| Writing starts | not before the scan has left the last row of the window | as soon as the scan has passed the first row of the window |
| Writing has to end | before the scan enters the first row | before the scan catches up with it in the next frame |
| Time for writing | the period less the time of the scan in the window | the period plus the time of the scan in the window |
| For a band of 200 rows | 16.4 − 5.2 = 11.2 ms | 16.4 + 5.2 = 21.6 ms |
| For the whole screen | 16.4 − 15.5 = 0.9 ms | 16.4 + 15.5 = 31.9 ms |

The condition of the right column is not made. It holds only where the order
of writing is the order of the scan: the usual orientation and a mirror
across. In the usual orientation large parts now go by the DMA, so its gain
would be left only for 24 bits and for a mirror across.

#### What is not held back

Single pixels, lines and small fills drawn straight on the display go at
once, as with `DgxDsiOneFrame`.

#### The pace

The wait for the scan does not set the pace. If the scan is elsewhere, the
wait is over at once, and a program can update the same band several times a
frame: on the bench without a pause of its own a band of 80 rows was updated
487 times a second, and the picture blurred. A program has to keep itself
from updating more often than the display shows frames. With two buffers
the bus does not set the pace either, but the picture does not blur for
that: what is shown is the last of what was drawn by the end of the frame.

#### What came of it

Three parts were tried. Each is drawn in a virtual screen and put to the
display once a frame, with a picture in it that moves sideways. There is one
question: is tearing seen.

| What is put to the display | One buffer, no wait for the scan | Behind the scan, the processor writes | Behind the scan, the DMA writes |
| --- | --- | --- | --- |
| Band of 80 rows | tears | does not tear | does not tear |
| Band of 200 rows | tears | tears | does not tear |
| Whole screen | tears | tears | does not tear |

The whole table is what is seen by eye. The processor manages to write
behind the scan only a band of 80 rows. A band of 200 rows and the whole
screen it cannot write in one pass of the scan, so the bus does not hold them
back, and they tear the same as without a wait. The DMA manages all of them.

The bench does not see tearing; it counts something else. Before every
transfer and after it the bench reads from the register of the DMA which row
the display is reading, and notes whether the scan was in the rows of the
part while the transfer went on.

| What is put to the display | One buffer, no wait for the scan, the processor writes | Behind the scan, the DMA writes |
| --- | --- | --- |
| Band of 80 rows | the scan was in the band in 13 transfers of 100 | in 6 of 100 |
| Band of 200 rows | in 42–44 of 100 | in 8 of 100 |
| Whole screen | in 100 of 100 | in 0 of 100 |

These numbers are to be read differently.

- The processor writes slower than the scan reads. If the scan was in the
  part during a transfer, it went through the place being written, and that
  is tearing. The numbers of the first column are the share of transfers
  that tore.
- The DMA writes twice as fast as the scan reads, and starts when the scan
  is not in the part. If the scan has entered the part later, the writing is
  already ahead of it, and it reads only the new. The numbers of the second
  column do not mean tearing. They show that the scan seldom enters a band,
  and never enters when the whole screen is put: the transfer starts in the
  blanking.

#### What was tried and did not suit

- To hold a batch in the cache until the scan has left and to bring it out at
  once. The virtual screen the transfer comes from lies in the same PSRAM and
  shares the same cache: the batch is put out earlier.
- To hold back only the end of a large batch. The batch tears in two places
  instead of one and looks worse.

## The cost of copying

All that follows comes down to what it costs to move a piece from one buffer
in PSRAM into another: from one frame buffer into the other, or from a
virtual screen into the frame buffer. The bench: two buffers of 1024x600 in
PSRAM, the display running, IDF 5.5.4. The time includes bringing out of the
cache.

| Part | Bytes | Processor, `memcpy` | Processor, with the byte exchange | 2D DMA | 2D DMA, with the byte exchange |
| --- | --- | --- | --- | --- | --- |
| 1024x600 | 1228800 | 27.1 ms | 30.3 ms | 8.1 ms | 7.8 ms |
| 1024x200 | 409600 | 9.1 ms | 10.4 ms | 2.75 ms | 2.67 ms |
| 1024x80 | 163840 | 2.35 ms | 3.1 ms | 1.14 ms | 1.11 ms |
| 1024x32 | 65536 | 1.44 ms | 1.79 ms | 0.47 ms | not measured |
| 320x100 | 64000 | 1.27 ms | 1.54 ms | 0.48 ms | 0.50 ms |
| 60x100 | 12000 | 0.40 ms | 0.44 ms | 0.15 ms | not measured |
| 32x32 | 2048 | 0.096 ms | 0.103 ms | 0.066 ms | not measured |
| 32x600 | 38400 | 1.57 ms | 1.67 ms | 0.53 ms | not measured |

What it shows.

- The processor copies large parts at 44 KB/ms. The scan reads at 77 KB/ms,
  nearly twice as fast.
- While a part fits the cache the copy goes faster: 1024x80 at 68 KB/ms.
- Small pieces are dear: a part of 32x32 goes at 20 KB/ms, because every row
  of it is a cache miss both in the source and in the destination.
- A running display takes about a fifth of the speed: without it a buffer is
  copied in 22.8 ms.
- From internal RAM into PSRAM the processor copies at 72 KB/ms without the
  exchange and at 57 KB/ms with it, 1.6 times as fast as from PSRAM. But a
  whole buffer does not fit internal RAM.
- The 2D DMA copies at 150 KB/ms, three to four times as fast as the
  processor and twice as fast as the scan. The byte exchange costs it
  nothing. The processor is free meanwhile.
- The DMA numbers were taken on a bare bench. In the bus one operation costs
  more, about 0.2 ms on top of the copying, see "The 2D DMA and the PPA".

## The catch-up copy by tiles

The first variant copied one rectangle around everything drawn in the last
batch. One line from corner to corner of the screen gave a rectangle of the
whole screen and a copy of 27 ms. A list of rectangles does not save a
diagonal: a line is drawn by pixels, and a list of any sensible length
overflows.

It is made by tiles. The panel is cut into tiles of 32x4 pixels, a bit a
tile. There are two maps. Each has a 32-bit word for a row of tiles, for
1024x600 that is 150 words, 1200 bytes together. At first a tile was 32x8;
how the size changed see in "The size of a tile". If a panel is wider than 1024
pixels, a tile is made wider, for a row to fit a word.

| Map | What is marked in it |
| --- | --- |
| `owed` | the tiles the hidden buffer lacks: the last batch drew in them, and it drew into the other buffer |
| `fresh` | the tiles the present batch draws in |

### When a tile is copied

There is one task: before the hidden buffer is shown, to move into it from
the shown one all the tiles of `owed`, copying as little as possible. So the
copy is put off to the last moment. A tile of `owed` is copied only when
there is no way around it any more.

| What happens to a tile of `owed` | What is copied | Why |
| --- | --- | --- |
| A pixel is drawn in it | the whole tile, before the drawing | its other pixels have to be right |
| A pixel is read from it | the whole tile, before the reading | otherwise the old would be read |
| A fill or a window covers it whole | nothing | all of it is written anew |
| A fill or a window covers a part of it | only the part of the tile outside the rectangle | the rest is written anew |
| The batch has ended and it was not touched | the whole tile, before the buffers change | this buffer is about to be shown |

After the copy the bit of the tile in `owed` is cleared, so a tile is copied
at most once a batch.

When everything is copied the buffers change roles. The map `fresh`, where
all that this batch drew is marked, becomes the map `owed` for the other
buffer, and a new `fresh` starts empty.

An example: a line from corner to corner of the screen that moves every
batch. The line goes through about 180 tiles of 4800. The next batch
erases it by pixels: those are the same tiles, each is copied at its first
pixel. The new line falls partly into them, partly into neighbouring ones,
which need no copy. About 50 KB are copied instead of 1.2 MB, and the batch
takes 2 ms instead of 21 ms.

### Two details

**Neighbouring tiles.** When a tile is copied because of a pixel or a read,
the tiles of `owed` that stand right next to it in the same row are copied
with it. One long piece costs about half of what the same tiles cost one at a
time, and when drawing by pixels the neighbouring tiles are usually needed
next.

**A window left unfinished.** A fill always writes its whole rectangle. A
window may not: `texture.c` sets a window down to the bottom of the figure
and writes into it as many rows as have the same width, then sets the next
one. So the bus does not take a window as written until it is sure. That is
done in three steps.

1. The first pixel comes into the window. The bus copies only what the tiles
   of the window have outside the window. The tiles themselves stay in
   `owed`.
2. Pixels come. The bus counts them.
3. The window is closed: another command has come, or other drawing has
   begun. By the count the bus knows how far the pixels went. Everything they
   did not reach is copied from the shown buffer, to the pixel. The tiles of
   the window are taken out of `owed`.

If the window is written to its end, nothing is copied at step 3: that is how
a virtual screen put to the display goes. If writing into a window did not
start from its first pixel, the window is taken as partial, and its tiles are
copied whole, as when a pixel is drawn.

### The size of a tile

A tile 32 pixels wide is 64 bytes, exactly one line of the cache. There is no
sense in making it narrower.

The height was chosen by measuring. Two grids were compared, tiles of 32x32
and of 32x8, on the same bench under IDF 5.5.4 with two buffers. The cells
hold the working time of one batch: the drawing together with the catch-up
copy, without the wait for the display.

| What a batch draws | Tiles 32x32 | Tiles 32x8 |
| --- | --- | --- |
| 2000 single pixels scattered over an area of 800x400, in new places every batch | 16.65 ms | 7.30 ms |
| A virtual screen of 1024x200 put to the same place, rows 200 to 399 | 13.76 ms | 12.69 ms |
| A line from corner to corner of the screen: the old one erased, a shifted one drawn | 2.30 ms | 1.95 ms |
| A gradient of 256x100, pixel by pixel, in one place | 14.81 ms | 14.40 ms |

| | Tiles 32x32 | Tiles 32x8 |
| --- | --- | --- |
| Tiles on 1024x600 | 608 | 2400 |
| Memory for the two maps | 152 bytes | 600 bytes |

Why the low tile is faster.

- A tile that got even one pixel is copied whole. A tile of 32x32 is 1024
  pixels, one of 32x8 is 256. On scattered pixels a quarter is copied, hence
  the first row.
- The edges of the band lie on the rows 200 and 399. Those are boundaries of
  tiles 8 rows high, and the band covers its tiles whole: nothing to copy.
  The boundaries of tiles 32 rows high are at the rows 192 and 416, and the
  tiles at the edges are covered only in part: 8 rows above the band and 16
  below it are copied every batch.
- On the line and on the gradient the difference is small: nearly the same
  pixels are copied there.

The low tile costs 448 bytes of internal RAM. It was chosen then.

Below 8 rows the height was not measured then. A tile of one line of the
cache, 32x1, suggests itself: 64 bytes are copied for a pixel, not 512.
Later the sizes were compared anew, in two ways. The code is the same, the
size is given by the call `dgx_dsi_tiles()`. Two buffers, 16 bits, IDF
6.0.1.

The scenes of the bench. The time one batch works in milliseconds; the last
three rows hold something else, each says what.

| What a batch draws | 32x8 | 32x4 | 32x2 | 32x1 | 64x1 |
| --- | --- | --- | --- | --- | --- |
| 2000 separate pixels over an area of 800x400 | 7.50 | 5.39 | 3.70 | 2.86 | 3.38 |
| A line from corner to corner: erased and drawn | 1.86 | 1.94 | 2.16 | 2.46 | 2.71 |
| Nine bars 548 high move by 4 pixels | 17.07 | 18.43 | 21.02 | 25.49 | 31.70 |
| Two squares 48x48 | 0.29 | 0.28 | 0.30 | 0.34 | 0.37 |
| The whole screen filled | 3.58 | 3.65 | 3.79 | 4.04 | 4.03 |
| A gradient 256x100 by pixels | 15.95 | 15.91 | 15.92 | 15.06 | 15.19 |
| A band of 200 rows from a virtual screen at row 213 | 4.07 | 3.99 | 3.99 | 3.97 | 3.97 |
| 320x100 from a virtual screen, the place moves by 10 pixels | 1.10 | 1.09 | 1.11 | 1.16 | 1.35 |
| Scene 3, frames a second | 34.9 | 34.8 | 34.8 | 30.5 | 23.9 |
| Scene 11, catch-up copy in 10 seconds, MB | 20.2 | 1.2 | 0.3 | 0.7 | 1.2 |
| Internal RAM for the bus and the screen, bytes | 7484 | 8092 | 9308 | 11740 | 11740 |

The demos. Nine demos of `examples/` were run on this display as they are:
the bus and the screen of the bench were put into the source of a demo, the
rest was not touched. To them drawing of text of bitmap fonts on the display
itself was added: a line of a clock in Verdana 32, a title in Terminus 12
four times as large, a log of 18 lines of Terminus 12 of which one is
written anew every frame, and two lines of small print in Arial 12 and Droid
Sans 9. A line is erased by a rectangle before it is written. Every demo
runs for 25 seconds. The table holds the processor time the task of a demo
spends on one batch, in milliseconds. The waits for the display and the
pauses of the demo itself are not in it.

| Demo | 32x16 | 32x8 | 32x4 | 32x2 | 32x1 | 64x8 | 64x1 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `glyph_morph_demo`, a morph of symbols | 26.75 | 26.81 | 26.95 | 27.07 | 27.39 | 26.84 | 27.41 |
| `weather_morph_demo`, a morph of weather symbols | 42.83 | 42.57 | 42.67 | 42.84 | 43.22 | 42.66 | 43.26 |
| `morph_demo`, a morph of a word letter by letter | 1.446 | 1.423 | 1.441 | 1.453 | 1.499 | 1.554 | 1.605 |
| `life_morph_demo`, the game of Life | 47.96 | 47.70 | 47.87 | 48.05 | 48.34 | 49.48 | 49.98 |
| `hw_font_demo`, handwriting by a pen | 0.060 | 0.060 | 0.062 | 0.066 | 0.073 | 0.061 | 0.074 |
| `hw_morph_demo`, a morph of handwriting | 45.03 | 44.72 | 44.71 | 44.71 | 44.85 | 44.72 | 44.83 |
| `flip_clock_demo`, a flip clock | 0.500 | 0.494 | 0.496 | 0.510 | 0.559 | 0.495 | 0.559 |
| `texture_demo`, textures | 2.193 | 2.190 | 2.190 | 2.206 | 2.242 | 2.190 | 2.242 |
| `screen_demo`, primitives | 0.607 | 0.598 | 0.602 | 0.625 | 0.654 | 0.660 | 0.740 |
| Text of bitmap fonts | 7.63 | 6.07 | 5.72 | 5.75 | 5.78 | 6.65 | 6.06 |

The tile of 32x8 was run twice, first and last in the series: the numbers
agreed within 0.3%.

What is seen on the demos.

- On all nine demos 32x8 is the fastest or shares the first place with
  32x16. The lower the tile the slower, but not by much: 32x4 loses up to
  1.3%, and 3% on the handwriting. 32x2 loses up to 4.5%, and 10% on the
  handwriting. 32x1 loses from 0.3% to 22%.
- The demos with a morph hardly depend on the size: a frame of the glow goes
  to the display as one window, and the tiles are touched only at its edges.
- Text of bitmap fonts is 5 to 6% faster with tiles of 32x4, 32x2 and 32x1.
  A line is erased by a rectangle whose edges do not lie on boundaries of
  tiles, and with a low tile less is copied at those edges: 45 MB of
  catch-up copy in 25 seconds with 32x8 and 19 MB with 32x4. A tall tile of
  32x16 is a quarter worse here.
- A tile 64 wide is worse than a tile 32 wide of the same height everywhere.

What is seen on the scenes.

- On scattered pixels the lower the tile the faster: 32x1 is 2.6 times as
  fast as 32x8.
- Everything else is slower with a tile of one row: the line by a third, the
  narrow bars one and a half times, the fill of the screen by 13%. As many
  bytes are copied, but in pieces of one row. Every row of tiles that
  drawing touches takes about 0.9 us: going through the map and calling the
  copy. A bar 548 high has 548 such rows instead of 69.
- Scene 11 copies tens of times more with a tile of 32x8 than with the
  others. The bands of the bench start at the rows 60, 240 and 420: those
  are boundaries of tiles 4 rows high but not of tiles 8 rows high, and 4
  rows at the top and 4 at the bottom of every band are copied every batch.
  That is the geometry of the bench, and it did not change the frame rate:
  60.9 with any tile.
- A map is a word a row of tiles: 32x4 takes 0.6 KB more than 32x8, 32x2
  takes 1.8 KB more, 32x1 takes 4.2 KB more. With 64x1 half of the bits of a
  word are used, and the memory is the same.

The choice. The tile is 32x4 now unless a program says otherwise,
`DGX_DSI_TILE_WIDTH` and `DGX_DSI_TILE_HEIGHT` in the header. The best on
the demos is 32x8, but 32x4 is 0 to 3% behind it, and that is not seen. What
it gains is seen: 6% on text of bitmap fonts, 28% on scattered pixels. The
memory: 0.6 KB more. With a tile of 32x4 the check against a model and the
scenes of the bench were passed again.

Another size a program gives itself, by an optional call:

```c
dgx_bus_protocols_t *bus = dgx_dsi_init(0, 2, 750, 3, 2500);
if (dgx_dsi_tiles(bus, 32, 8) != ESP_OK) ESP_LOGW(TAG, "the tiles stay 32x4");
dgx_screen_t *screen = dgx_jd9165_init(bus, rst, 16, DgxScreenRGB, DgxDsiTwoFrames);
```

The call checks the size and answers by a code of ESP-IDF. When the size is
not taken the tiles stay as they were, and a warning appears in the log.

| Answer | When |
| --- | --- |
| `ESP_OK` | the size is taken |
| `ESP_ERR_INVALID_ARG` | there is no bus; the width is not a power of two from 8 to 1024; the height is not a power of two from 1 to 128 |
| `ESP_ERR_NO_MEM` | there is no memory for the maps |
| `ESP_ERR_INVALID_STATE` | the call came in the middle of a batch |

Before the constructor of the display the size is only remembered. After it
the call is made between two batches: the bus waits until the display has
taken the ready picture, gives the buffer that becomes free all it lacks,
and makes the maps anew. The check against a model changed the size so
every 5 batches, 360 times a run: 0 mismatches. A panel wider than 32 tiles
makes the tile wider itself.

The size is not to be changed as a program goes, to suit what is drawn this
minute. It is chosen once, for the program, and given before the
constructor of the display.

- A change is not free. The drawing task waits for the display: 10.6 ms on
  average and up to 23 ms a change in that same check. The picture stands
  still meanwhile.
- The gain from a tile that suits is small, some percent on the demos. On
  text of bitmap fonts a tile of 32x4 saves 0.35 ms a batch, and one change
  eats that saving of thirty batches.
- The maps are taken from the heap anew every time, 0.6 to 4.8 KB. Changing
  often breaks the heap up.
- In the middle of a batch the call refuses.

The call after the constructor is left for a program that learns the size
late, one that reads it from its settings for instance.

A tile of one row can be made faster: rows of tiles with the same mask taken
as one piece, not one by one. That is not done.

### The cost for a pixel

With two buffers `set_pixel` goes to the maps for every pixel: a bit in
`owed` has to be checked and a bit in `fresh` set. To make that cost less the
bus remembers the tile of the last pixel and does not go to the maps for a
pixel in the same tile. For a screen in the usual orientation it also does
not turn the coordinates of the screen into those of the panel. What is left
is seen on the gradient in "The working time of a batch": 14.8 ms instead of
12.9 ms.

### How it is checked

The tile module does not depend on the platform and is checked on the host,
`test/host/test_fb_tiles.c`. The test keeps two buffers and a model, draws
random batches of all the cases of the table above, and after every change of
the buffers compares the buffer with the model. It is checked separately that
only what is needed is copied.

### A virtual screen as the source

Another variant was considered: to draw always into a virtual screen of one's
own, and to copy what has changed into the hidden buffer. It is not cheaper.
A virtual screen as large as the display lives in PSRAM too, and a copy from
it costs the same. And the tiles of this batch and of the last one have to be
copied together, while with drawing straight into the hidden buffer only
those of the last one.

### How LVGL 9 does it

LVGL is the most widespread graphics library for such displays, and its task
is the same. I read how it solves it: `lv_refr.c` of LVGL itself and
`esp_lvgl_port_disp.c`, the port by Espressif that ties LVGL to the display
driver.

First, the difference in how drawing is done. In DGX a program calls drawing,
and it gets into the buffer at once. In LVGL a program changes objects: a
button, a label, a line. Every change marks a rectangle of the screen as
outdated, and once a frame LVGL redraws all the outdated rectangles itself.

The port works in one of three modes.

| Mode of the port | Frame buffers | What is drawn every frame | Tearing |
| --- | --- | --- | --- |
| Usual | 1 | the outdated rectangles, into a small separate buffer; the port copies it into the frame buffer | possible |
| `avoid_tearing` and `full_refresh` | 2 | the whole screen, into the hidden buffer | none |
| `avoid_tearing` and `direct_mode` | 2 | the outdated rectangles, straight into the hidden buffer | none |

The third mode is made like `DgxDsiTwoFrames`. A frame goes like this in it.

1. LVGL has gathered the outdated rectangles of this frame.
2. The hidden buffer lacks what was drawn in the last frame. LVGL takes the
   rectangles of the last frame, takes away from them the rectangles of this
   one, and copies the rest from the shown buffer into the hidden one. That
   is the same catch-up copy.
3. LVGL redraws the rectangles of this frame in the hidden buffer.
4. The port asks the driver to change the buffers and waits for the report of
   the end of the frame.

Compared with `DgxDsiTwoFrames`.

| | LVGL, `direct_mode` | DGX, two buffers |
| --- | --- | --- |
| What marks the changed | rectangles, 32 at most | tiles of 32x4, 4800 of them on 1024x600 |
| A line from corner to corner | one rectangle of the whole screen | about 180 tiles |
| If there are too many marks | the list is replaced by one rectangle of the whole screen | nothing can overflow |
| What is not copied | what will be redrawn in this frame: known beforehand | what a fill or a window has covered: found out on the way |
| Who copies | the processor, row by row | the processor, and the 2D DMA for large pieces |
| No tearing on one buffer | no way | the strategy behind the scan, for a virtual screen put to the display |

Notes to the table.

- The number 32 is `LV_INV_BUF_SIZE`; it can be changed when LVGL is built.
- LVGL knows beforehand what it will redraw, because it gathers the changes
  first and draws after. DGX draws at once and does not know the next call.
- The copy in LVGL goes through a function that can be replaced,
  `buf_copy_cb` in `lv_draw_buf.c`. A hardware one could be put there; the
  port by Espressif does not do it.

## The 2D DMA and the PPA

The ESP32-P4 has two hardware blocks that write memory without the processor
and past its cache. The 2D DMA moves a rectangle from one picture into
another. The PPA, the pixel processing accelerator, among other things fills
a rectangle with a color and mixes two pictures. In the bus they have four
jobs.

| Job | By what | What is done | From what size |
| --- | --- | --- | --- |
| A virtual screen put to the display | 2D DMA | rows of a 16-bit virtual screen go into the frame buffer, with the byte exchange | from 16 KB |
| The catch-up copy with two buffers | 2D DMA | what is left to copy before the buffers change goes from one frame buffer into the other | rows × (width + 53) of 8000 or more |
| A fill of a rectangle | PPA | a rectangle of the frame buffer is filled with a color | rows + bytes / 64 of 200 or more |
| An 8-bit screen put to the display through a table of two colors | PPA | a rectangle is filled with the first color, and the second is mixed into it by a byte a pixel | any, with two buffers only |

The option `CONFIG_DGX_DSI_HARDWARE_COPY` turns both blocks off; the
processor does everything then. They cost 0.8 KB of internal RAM: 0.5 KB the
DMA and 0.3 KB the PPA.

### Which interface of IDF

IDF has three ways to the 2D DMA.

| Way | Where it is | Does it suit |
| --- | --- | --- |
| The flag `use_dma2d` of the DPI driver, in IDF 6 the function `esp_lcd_dpi_panel_enable_dma2d()` | 5.x and 6.x | no: it copies a foreign buffer only into the shown frame buffer, and without the byte exchange |
| The public `esp_async_color_convert.h` | 6.0.3, 6.1 | no: it has no exchange of the bytes of 16-bit pixels |
| The private `esp_private/dma2d.h` | all versions with the ESP32-P4 | yes |

The bus uses the private interface. That is a risk: IDF is free to change a
private interface. One difference between versions has come up already: the
structure that sets the burst length is not the same in 5.5.4 and in 5.5.5
and later. The bus does not touch it; the default is the same, 128 bytes. The
build was checked under 5.5.4, 5.5.5, 6.0.1, 6.0.3 and 6.1. Should it stop
building under a new IDF, the option above puts everything back on the
processor.

Through the public interface of 6.1 I first made the catch-up copy and
measured it: before every operation it brings the whole picture out of the
cache, not the rectangle, and its queue costs from 0.7 to 2.9 KB of RAM. The
bus's own call through the private interface brings out only the rows of the
rectangle.

### The byte exchange

The DMA can change the order of the bytes of a pixel: six orders of three
bytes; the setting is called scramble. For a 16-bit pixel the order
`DMA2D_SCRAMBLE_ORDER_BYTE2_0_1` exchanges its two bytes:

    in:  a0 10 a1 11 a2 12
    out: 10 a0 11 a1 12 a2

But it works only in the right input mode. The register of the chip,
`OUT_COLOR_INPUT_SEL`, has two values for scrambling:

| Value | What it means by the description of the register |
| --- | --- |
| 2 | Other 2byte/pixel type |
| 3 | Other 3byte/pixel type |

The IDF driver sets one of them itself, and which one depends on the version.

| IDF | What the driver sets | 16-bit pixels | 24-bit pixels |
| --- | --- | --- | --- |
| 5.3.4, 5.4.3, 5.5.4, 5.5.5, 6.0.1, 6.0.2 | 2 | works | garbage comes out |
| 6.0.3, 6.1, master | 3 | the transfer never ends | works by the intent of the commit, not checked |

The value for every version is taken from its source. The behaviour was
measured under 5.5.4, 6.0.1 and 6.1.

The value was changed by the commit `78ccd163cc`, "fix non-usable
DMA2D_CSC_TX_SCRAMBLE option": it mended 24 bits and broke 16. There is an
[issue 19226](https://github.com/espressif/esp-idf/issues/19226) about it,
with an example that reproduces the hang without a display.

After the driver has done its setting the bus writes 2 into the field of the
register itself. The driver does not tell the number of the channel, so the
channel is found by the address of its descriptor. Under 5.5.4 that changes
nothing; under 6.0.3 and 6.1 the transfer hangs without it.

Checked under 5.5.4 and 6.1, with the display running from the same PSRAM:

- all 614400 pixels of a picture of 1024x600 come out with their bytes
  exchanged;
- rectangles at an odd column and of an odd width, down to a column one pixel
  wide and 600 high, are right inside and touch nothing outside: 14 cases.
  The header of IDF warns that the destination needs an alignment of 4 bytes;
  in practice the neighbouring pixels do not suffer.

### A virtual screen put to the display

A bus got an optional function, `write_rows`: "take rows of pixels from
memory and put them into a rectangle of the screen". Other buses do not have
it. A virtual screen offers its rows to it first, and if the bus has
declined, sends them through the window as before.

The DSI bus takes the rows when all of this holds:

- the pixels are 16-bit, and red and blue do not change places;
- the screen is neither mirrored nor turned: the DMA writes only from left to
  right and from the top down;
- the rectangle is 16 KB or more. A smaller one the processor has moved
  through the window while the DMA is still being started.

Otherwise the old way through the window works.

| What is put to the display | Through the window, the processor | By the DMA |
| --- | --- | --- |
| The whole screen, 1024x600 | 34.4 ms | 8.8 ms |
| A part of 320x100 | 1.12 ms | 0.51 ms |
| A part of 60x100, less than 16 KB | 0.28 ms | 0.28 ms, it goes through the window |

One buffer, a cache of 256 KB, IDF 5.5.4.

Before it starts the DMA the bus deals with the cache itself: it brings the
rows of the source out of it, and brings out and forgets the rows of the
destination, for the processor to read them from memory after the DMA. With
two buffers the rule of the tiles for a rectangle written whole works before
that. Behind the scan the bus waits before the start until the scan has left
the rows.

A screen of 8 bits a pixel goes to a 16-bit display through a table of
colors, `dgx_vscreen8_region_to_screen16()`. It has no rows of colors ready.
The processor used to turn the pixels into colors in the buffer of the
window, 4 KB, and to carry them into the frame buffer itself. Now, when the
bus takes rows, the colors are made ready by bands of up to 64 KB in memory
the bus gives, internal RAM with DSI, and every band goes through
`write_rows`. The memory is taken for the time of the call. A band the bus
does not take goes through the window; if the bus refuses the first one, the
whole rectangle goes through the window, as before.

Measured on the example `hw_soft_pen_demo`: every frame a rectangle of 367
thousand pixels of a page of 1024x600 goes to the display. Two buffers, a
cache of 128 KB, IDF 5.5.4:

| What | Through the window | By bands through the DMA |
| --- | --- | --- |
| The rectangle sent | 37.2 ms | 20.7 ms |
| Frames a second, the drawing counted | 17.7 | 25.5 |

Of the 20.7 ms about 12 is the turning itself: the processor reads the page
from PSRAM and looks into the table. With a cache of 256 KB the same example
gives 27.8 frames a second, with 512 KB 32.8.

A table of colors the accelerator cannot do: the indexed input is commented
out in the driver of IDF. It can do another thing: mix a picture with one
color, taking how opaque the color is from an 8-bit mask. A screen whose byte
is an amount of ink is such a mask: a pen with a soft edge draws so, and its
table goes from the color of the paper to the color of the ink. Such a table
the transfer tells by itself: none of its colors is further than a step of
red, green or blue from the straight way between the first and the last.
Then the bus is given two colors, the first and the last. It fills the
rectangle of the frame buffer with the first and mixes the second into it,
the rectangle itself being where the result goes. The processor carries no
pixel. There are no new calls for it: a program calls the same
`dgx_vscreen8_region_to_screen16()` with the same table.

Between the fill and the mixing the rectangle is paper alone. With two
buffers nobody sees it. With one the display would show it every other time,
so with one buffer the bus refuses this job, and the picture goes by bands
through the table, as any other does.

What the accelerator gives was checked by reading the frame buffer, six pairs
of colors: all the 614 400 pixels of the screen get the color of their
amount, no ink gives the paper exactly, 255 the ink exactly, rectangles from
one pixel up fall in their places. Between paper and ink the accelerator
rounds in its own way, and its exact rule was not found: from the table of
the example `hw_soft_pen_demo` it differs in 140 colors of 256, by one step
of a channel everywhere. A display takes all the pictures of one table one
way.

A step of a channel is 3.2% of the range of red or blue and 1.6% of green.
In brightness, with the colors of the same example, that is 2.3% of full at
most, when all three channels go one way, and 0.5% on the average over the
table. Only the colors between the first and the last differ, that is the
pixels of the soft edge of a line. A program that wants the colors of its
table and nothing else says so when it sends the screen: the last argument
of the call is flags now, and `DGX_LUT_EXACT` in place of `false` sends the
screen through the table by bands. `true` and `false` mean what they did:
`DGX_LUT_TRANSPARENT` and nothing. Where there is nothing to mix with, the
flag changes nothing.

The time of a rectangle, one buffer, a cache of 128 KB, IDF 5.5.4; the mixing
with one buffer was let in for this timing alone:

| Rectangle | By bands through the DMA | Mixed by the accelerator |
| --- | --- | --- |
| 1024x600 | 28.0 ms | 17.1 ms |
| 900x408 | 17.1 ms | 10.6 ms |
| 320x100 | 1.28 ms | 1.13 ms |
| 100x60 | 0.88 ms | 0.50 ms |
| 40x40 | 0.21 ms | 0.28 ms |

In the example `hw_soft_pen_demo`, with two buffers, the sending took 13.5 ms
instead of 20.7, and the lines move at 31.2 frames a second. The client of
the accelerator for mixing took no internal RAM by the count of the heap.

### The catch-up copy

Only what is left in `owed` before the buffers change goes by the DMA. For
that the rest is gathered into rectangles: tiles next to each other in a row,
and the same run in the rows below, go as one piece. Copies at the first
touch stay with the processor. After them the place is drawn in, and it is
good to have its rows in the cache, and after the DMA they are not there.

One operation of the DMA in the bus costs about 0.2 ms on top of the copying
itself: bringing out of the cache, queueing, the interrupt, waking the task.
Hence the threshold: a part of 64x48 is copied by the processor, a part of
320x100 and a column of 40x548 by the DMA.

### A fill by the PPA

The PPA has a public interface, `driver/ppa.h`, the function `ppa_do_fill()`.
It is given a picture, a rectangle in it and a color. The orientation of the
screen makes no difference to a fill: a rectangle of the screen stays a
rectangle on the panel. Both 16 and 24 bits will do.

The PPA takes a color as 8 bits of red, green and blue and brings it down to
16 bits itself. For exactly the 16-bit value that was asked for to come out,
5 and 6 bits are spread to 8 by repeating their high bits. Checked by going
through all of them: all 65536 colors come out exact.

The PPA does not touch neighbouring pixels: checked on 12 rectangles, from
one pixel to the whole screen, at odd columns and rows.

The driver deals with the cache itself: before a fill it writes the rows of
the rectangle out of the cache and forgets them.

The time of one fill on the bench, the display running, IDF 5.5.4. For the
processor it includes bringing out of the cache.

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

The PPA fills at 320 KB/ms, four times as fast as the scan, and a running
display does not slow it down. One call costs 57 us, so short runs are
filled faster by the processor.

The threshold is taken above the point where they meet: rows + bytes / 64 of
200 or more. The reason is the cache. After the PPA the place filled is not
in the cache, and if something is drawn over it at once, every row has to be
fetched from PSRAM. So a square of 48x48 stays with the processor, a square
of 100x100 and a column of 4x548 go to the PPA.

Letters and figures are made of short runs, and each goes through the fill
of a rectangle. For the check of the threshold not to slow them down, a test
by one addition stands before it: width plus height below 64. No rectangle
that passes the threshold falls under that test; the smallest sum among them
is 110. A line of text of 25 symbols takes 1.00 ms, as before the
accelerator.

Fills in the bus, one buffer, a cache of 256 KB, IDF 5.5.4:

| What is filled | Processor | With the PPA |
| --- | --- | --- |
| The whole screen | 16.4 ms | 3.5 ms |
| 100x100 | 0.18 ms | 0.13 ms |
| A column of the height of the screen, 1x600 | 0.5 ms | 0.15 ms |
| A row of the width of the screen, 1024x1 | 0.026 ms | 0.026 ms, the processor fills it |

### The working time of a batch

Two buffers. The time from the start of drawing to the return from
`update_screen` is measured, without the wait for the display: the bench
waits 40 ms before every batch.

The columns:

- "Rectangle": the catch-up copy by one rectangle around everything, the
  first variant. The processor does everything. IDF 5.5.4.
- "Tiles": tiles of 32x8, the processor does everything. IDF 6.1.
- "Tiles, DMA and PPA": the same, with large pieces of the catch-up copy and
  a virtual screen put to the display going by the 2D DMA, and large fills
  by the PPA. IDF 5.5.4.

| Batch | Rectangle | Tiles | Tiles, DMA and PPA |
| --- | --- | --- | --- |
| A line across the screen: the old one erased, a new one drawn | 21.2 ms | 2.0 ms | 1.9 ms |
| Two squares of 48x48, one in place, the other jumping between corners | 5.9 ms | 0.31 ms | 0.30 ms |
| 2000 pixels over an area of 800x400 | 16.7 ms | 7.5 ms | 7.4 ms |
| Nine bars 548 high move by 4 pixels: a strip is erased at the left of each and drawn at the right | 32.1 ms | 22.7 ms | 17.1 ms |
| 320x100 from a virtual screen, the place jumps by 10 pixels | 3.5 ms | 2.3 ms | 1.2 ms |
| A band of 200 rows from a virtual screen, in one place | 12.7 ms | 12.7 ms | 3.9 ms |
| A fill of the whole screen | 16.7 ms | 16.8 ms | 3.6 ms |
| A gradient of 256x100 by pixels, in one place | 12.9 ms | 14.8 ms | 14.3 ms |
| A square of 48x48 after a batch that filled the whole screen | not measured | 27.4 ms | 8.4 ms |
| A band of 200 rows, even batches in one place, odd ones in another | not measured | 21.3 ms | 5.5 ms |
| 320x100 from a virtual screen, in two places the same way | not measured | 3.6 ms | 1.1 ms |
| A column of 40x548, in two places the same way | not measured | 3.2 ms | 1.0 ms |

What it shows.

- Everything that is put from a virtual screen got three to four times
  faster: that is the transfer by the DMA.
- A fill of the whole screen got nearly five times faster; that is the PPA.
  Bars made of narrow strips got a quarter faster: the strips themselves are
  filled by the PPA, and the catch-up copy around them is still made by the
  processor.
- Batches that draw in different places in turn, and a small batch after a
  large one, got faster by the catch-up copy by the DMA.
- Drawing by pixels and lines does not depend on the DMA and the PPA.
- A gradient by pixels got slower than with the rectangle: that is the cost
  of going to the maps for every pixel. Its time changes from build to build
  between 14.3 and 16.2 ms, by how the code happens to lie.
- The table was taken with a tile of 32x8. With the present 32x4 the size of
  the tile changed three rows of the last column: 2000 pixels 5.4 ms instead
  of 7.4, the nine bars 18.4 instead of 17.1, the diagonal 2.0 instead of
  1.9. The four bottom rows were changed by the copy after the change, see
  its section.

### Measuring the board

From what size on a rectangle goes by the 2D DMA or the PPA and not by the
processor, and how much room the bus leaves before the scan, is decided by
numbers. They depend on the chip, its memory and the
display. The code has the numbers of the board the bus was written on.
`dgx_dsi_bench()` takes them on the board at hand.

```c
dgx_dsi_tuning_t tuning;
if (dgx_dsi_bench(bus, &tuning)) dgx_dsi_tune(bus, &tuning);
```

| Function | What it does |
| --- | --- |
| `dgx_dsi_bench(bus, &tuning)` | measures the board and gives the numbers. The bus stays with those it had |
| `dgx_dsi_tune(bus, &tuning)` | gives the numbers to the bus. Numbers of another version of the structure, another panel or another color depth it does not take: it returns `false` and writes the reason to the log |
| `dgx_dsi_tuning(bus, &tuning)` | reads the numbers the bus works with now |

Why a call of its own and not a part of starting the panel.

- The measuring takes 0.07 s at 16 bits and 0.09 s at 24. At every start
  that is too much.
- The numbers do not change from start to start. `dgx_dsi_tuning_t` is a
  flat structure of 40 bytes with no pointers. A program puts it into NVS
  and gives it to the bus from there at the next start. The example
  `dsi_demo` does so.
- The bus does not touch NVS itself. Where to keep the numbers and when to
  measure again is for the program to decide.
- A bus that was given nothing works with the numbers it had. The measuring
  changes nothing until a program asks for it.

Nothing changes on the screen while it measures. It works in memory of its
own: two pictures of 128 rows of the panel, half a megabyte of PSRAM with
1024 pixels in a row at 16 bits. It gives that back at the end. Checked: a
checksum of the screen before six measurings in a row and after them is the
same, and all of the heap comes back.

The display has to be running meanwhile, and that is a condition of the
measuring. The display reads the frame buffer from the same PSRAM all the
time, and with the display at work the processor reads and writes it at
another speed. The numbers have to be those of the work, so before
`dgx_dsi_video_start()` the measuring refuses. Nothing may be drawn while it
goes.

What is measured. Every operation is made nine times and the middle result
is taken. Where the number is one to leave room by, the operation is made
three times and the longest is taken. Before every go both pictures are
thrown out of the cache.

| What is measured | How | On the bench at 16 bits |
| --- | --- | --- |
| Writing through a window | the whole picture 1024x128 in portions of 4 KB, each copied from the other picture first, as from a virtual screen; the cache brought out at the end | 7.7 ms, which is 34 KB/ms |
| Bringing out of the cache | 32 KB just written | 107 to 120 us |
| A copy by the processor | 32x128 and 512x16 | 218 to 221 and 361 to 364 us |
| A copy by the 2D DMA | 32x8, 1024x32 and 1024x128 | 62 to 68, 462 to 470 and 1676 to 1693 us |
| A fill by the processor | 16x128 and 512x16 | 153 to 162 and 234 to 235 us |
| A fill by the PPA | 16x16, 16x128 and 1024x64 | 78 to 85, 94 and 434 us |

Two shapes of one operation give what a row costs and what a byte costs, a
small operation gives what asking the DMA or the PPA costs. The thresholds
are reckoned from those as follows.

- The copy. A rectangle costs the processor "rows times the cost of a row
  plus bytes times the cost of a byte", it costs the DMA "the cost of asking
  plus bytes times the cost of a byte by the DMA". The threshold is where the
  DMA becomes the faster one, with room: a fifth at 16 bits, twice at 24.
- Why the room differs. The measuring starts with an empty cache. In work a
  piece drawn a moment ago is still in the cache, and from there the
  processor copies it four times as fast as from PSRAM. At 24 bits a piece
  is copied at the end of a batch, and the batch waits for it: the DMA has
  to win with room. At 16 bits a piece for the DMA is left for later, and
  the batch pays only for leaving it, about 50 us at any size.
- The fill. The same comparison, with room of two and a half times: what the
  PPA has filled is not in the cache, see "A fill by the PPA".
- Rows of a program. Writing through a window against the DMA, room 1.6.
- The speeds of writing through a window and of bringing out of the cache
  are taken a quarter less than measured.
- The room for a start is twice the cost of asking the DMA.

What came out on the bench. The measuring was repeated six times in every
mode, the cells hold the spread. Next to them the numbers set in the code.

| Number | Set in the code | Measured, 16 bits | Measured, 24 bits |
| --- | --- | --- | --- |
| Writing through a window, KB/ms | 28 | 24 to 25 | 27 to 28 |
| Bringing out of the cache, KB/ms | 256 | 205 to 237 | 220 to 240 |
| The speed of the 2D DMA, KB/ms | measured at work | 150 to 152 | 133 |
| The room for a start of the DMA and the PPA, us | 400 | 116 to 150 | 114 to 126 |
| A copy by the DMA when rows × (width + A) is B or more | A 53, B 8000 | A 8 to 10, B 2200 to 2900 | A 30 to 34, B 5200 to 5700 |
| Rows of a program by the DMA from, bytes | 16384 | 3900 to 5200 | not used |
| A fill by the PPA when rows + bytes / A is B or more | A 64, B 200 | A 65 to 76, B 244 to 270 | A 78 to 130, B 196 to 300 |
| The sieve before the threshold of the fill: width + height is at least | 64 | 151 to 158 | 140 to 151 |

Four numbers the measuring does not touch, they stay as in the code: the
display reads 6 rows beforehand, a second transfer slows a copy down 2.5
times, the room for a write behind the scan is 150 us, the room before the
end of a frame is 2 ms. The first and the last need the frame buffer itself
under the scan and cannot be taken in memory of one's own. The slowing by a
second transfer could be taken; that is not done.

The size of the tiles the measuring does not choose. It depends on what a
program draws, not on the board, and is given by a call of its own, see "The
size of a tile".

Do the numbers suit: the choice of the bus. Through `dgx_dsi_tune()` the bus
is given numbers that make it do everything one way, then numbers that make
it do everything the other way, and the same thing is timed both ways. So
for every shape of a rectangle it is known which way is faster. With that
the choice of the bus by the set numbers and by the measured ones is
compared. The shapes: 10 widths from 8 to 1024 by 10 heights from 4 to 548
for fills and rows of a program, 7 widths from 32 to 1024 by 9 heights from
8 to 544 for the catch-up copy. The last two columns hold the time lost on
all the shapes together by choosing the slower way.

| Mode | What | All by the faster way | The set numbers lose | The measured ones lose |
| --- | --- | --- | --- | --- |
| One buffer, 16 bits | fills | 24.7 ms | 0.02 ms | 0.11 ms |
| One buffer, 16 bits | rows of a program | 51.8 ms | 4.08 ms | 0.36 ms |
| Two buffers, 16 bits | fills | 29.0 ms | 0.04 ms | 0.02 ms |
| Two buffers, 16 bits | rows of a program | 89.5 ms | 3.83 ms | 0.40 ms |
| Two buffers, 16 bits | the catch-up copy | 3.3 ms | 0.24 ms | 0.001 ms |
| Two buffers, 24 bits | fills | 33.9 ms | 0.09 ms | 0.006 ms |
| Two buffers, 24 bits | the catch-up copy | 76.2 ms | 0.02 ms | 0.003 ms |

- Fills and the catch-up copy: the difference between the set numbers and
  the measured ones is less than half a percent of the time. The set ones
  were taken on this very board, nothing else was to be expected.
- Rows of a program: the measured threshold is ten times better. 16 KB was
  much, the DMA pays from 4 to 5 KB on.
- The catch-up copy at 16 bits: 3.3 ms for 63 shapes because a piece is left
  for later and the batch does not wait for it. The lower the threshold the
  better, and the measured one is lower.
- Behind the scan such a check tells nothing: a tall rectangle by the DMA or
  the PPA waits for the scan there, up to a whole frame, while the processor
  writes at once, and "faster" does not mean the same any more.
- The threshold for rows of a program counts bytes only and does not tell
  shapes apart. A narrow tall area, 8x200, the DMA moves three times as fast
  as the window does, and the threshold leaves it to the window.

Do the numbers suit: the work. With the measured numbers the check against
a model and the scenes of the bench were run again, IDF 6.0.1.

| What | The set numbers | The measured numbers |
| --- | --- | --- |
| The check against a model: one buffer, behind the scan, two buffers at 16 and at 24 bits | 0 mismatches | 0 mismatches |
| The same under IDF 5.5.5, two buffers, 16 bits | 0 mismatches | 0 mismatches |
| Pieces that fell behind the scan, in all scenes and checks | 0 | 0 |
| Two buffers, scenes 1 to 6, frames a second | 42.3, 60.9, 34.8, 60.9, 60.9, 60.9 | 42.4, 60.9, 34.8, 60.9, 60.9, 60.9 |
| Two buffers, scene 7, pictures a second of the three tasks | 40.2, 41.9, 40.1 | 40.6, 43.1, 40.1 |
| Two buffers, scene 12 | 59.2, 39.5, 30.8 | 59.3, 39.6, 31.0 |
| Two buffers, the time of the fourteen batches of the bench | taken on the same bench | differs by no more than 0.12 ms, but for the gradient |
| Behind the scan, scenes 1 to 6, frames a second | 30.4, 60.9, 45.6, 60.9, 60.9, 60.9 | 30.4, 60.9, 53.2, 60.9, 60.9, 60.9 |
| Behind the scan, scenes 1, 2 and 4: updates the scan got into | 0 of 305, 0 of 610, 0 of 610 | 0 of 305, 0 of 610, 0 of 610 |

- Correctness suffered nowhere, and no piece fell behind the scan.
- The time of the gradient by pixels wanders from build to build by a
  millisecond, from 15.0 to 16.0 ms, and has nothing to do with the
  measured numbers.
- Both columns of two buffers were taken with a tile of 32x4.
- Behind the scan scene 3 went from 45.6 to 53.2. Its tall fills can be
  started only in the blanking, and the room for a start became 0.12 ms
  instead of 0.4 ms: more fills fit one blanking. Nobody has looked at this
  scene with the new room with the eyes.

What is not here. The measuring was made and checked on one board, the very
one the set numbers were taken on. It shows that on that board it comes out
no worse. That it comes out right on another chip, with another memory or
another display, nobody has checked.

### Internal RAM

How much of the heap `dgx_dsi_init()` and `dgx_jd9165_init()` take together.
The sum includes the buffer of the bus, 4096 bytes. IDF 5.5.4.

| Mode | Without the DMA and the PPA | With the DMA and the PPA |
| --- | --- | --- |
| One buffer | 4920 | 5736 |
| Behind the scan | not measured | 5816 |
| Two buffers, a copy by a rectangle | 5036 | there was none |
| Two buffers, tiles | 5700 | 6492 |

Under IDF 6.1 two buffers with tiles, the DMA and the PPA take 6580 bytes.

After the joining of batches, the copy after the change, the counters and
the measuring of the board, under IDF 6.0.1 with the DMA and the PPA: one
buffer 5880 bytes, behind the scan 5964, two buffers 8096. That is with a
tile of 32x4. The maps of the tiles take 8 bytes a row of tiles: with a tile
of 32x8 that is 0.6 KB less, with 32x2 1.2 KB more, with 32x1 3.6 KB more.

## Frame rate

The bench shows six scenes of 10 seconds each and counts updates a second. In
the scenes 1, 2 and 4 the picture is drawn in a virtual screen and put to the
display. In the scenes 3, 5 and 6 primitives are drawn straight on the
display.

| Scene | One buffer, processor | One buffer, DMA and PPA | Behind the scan, DMA and PPA | Two buffers, rectangle | Two buffers, tiles | Two buffers, tiles, DMA and PPA |
| --- | --- | --- | --- | --- | --- | --- |
| 1. Bars move sideways, the whole screen | 19.6 | 44.3 | 30.5 | 19.7 | 19.6 | 42.7 |
| 2. The same in a band of 200 rows | 54.6 | 60.0 | 60.9 | 46.3 | 44–46 | 61.0 |
| 3. Bars move sideways, by primitives | 41.7 | 55.6 | 30.4 | 20.3 | 30.4 | 34.8 |
| 4. The same as 2 in a band of 80 rows | not measured | 60.0 | 60.9 | 60.7 | 60.8 | 60.9 |
| 5. A hand going round and four squares in the corners | not measured | 62.5 | 60.9 | 30.4 | 60.8 | 60.9 |
| 6. A line across the screen, moving | not measured | 61.7 | 61.7 | 35.7 | 60.8 | 60.8 |

All of it was taken under IDF 5.5.4. In the column "processor" and in the
columns where the DMA and the PPA are not named the processor does
everything.

What is seen by eye.

- One buffer: tearing in every scene where the picture moves, the 1st to the
  4th.
- Behind the scan: no tearing in the scenes 1, 2 and 4. None in scene 3
  either, but the width of a running bar twitches: its fills fall into two
  frames, see "Behind the scan", "A fill by the accelerator".
- Two buffers: no tearing in any scene.

The table was taken before the batches of different tasks began to join, see
"Several tasks". In each of its scenes one task draws, and for it the order
is as it was. The scenes were not taken again.

With one buffer the bench holds the pace itself: by a pause of 16 ms where
drawing goes straight on the display, and by a timer with the period of the
display where a picture is put to it. Numbers above 60.9 in those columns are
the inaccuracy of the pause, not the speed of the display.

What is behind the numbers.

- Scene 1 was held by the transfer of the screen, 34 ms, three periods. By
  the DMA the transfer takes 8 ms, and the scene is now held by something
  else: the bench redraws the bars in a virtual screen in PSRAM, which takes
  more than half a period. Hence 43–44 and not 60.
- Behind the scan scene 1 goes at 30: before every transfer the bus waits for
  the scan to go into blanking, and two updates do not fit one frame.
- Scene 2 with two buffers stood at the boundary of one period. By the DMA
  the transfer of the band takes 4 ms instead of 12.7 ms, and it goes at a
  full 60.
- Scene 3 on one buffer got from 41.7 to 55.6: the strips are filled by the
  PPA, and a batch takes 2.5 ms instead of 8.4 ms.
- Scene 3 behind the scan goes at 30: 18 tall fills do not fit one gap, and
  a batch takes two frames.
- Scene 3 with two buffers rose from 30 to 35: a batch takes 17 ms, a little
  more than a period, and some batches fit one frame. The catch-up copy of
  narrow strips over the whole height of the screen still costs more than
  drawing them.

One more change concerns two buffers only. A batch finished in the last 2 ms
of a frame used to wait a frame more, in case the driver had already chosen
the old buffer. Where the end of a frame is reported at the moment the buffer
is chosen that pause is not needed: with the event `on_frame_buf_complete` on
any chip, and with `on_refresh_done` on chips of a revision below 3. The
pause is gone, and no tearing is seen by eye after that.

## What is not done

- The copy after the change works only by the 2D DMA. Without it, and with
  the option of the DMA and the PPA off, the hidden buffer gets everything at
  the end of the first batch of a frame, as in release 0.5.2.
- The numbers the bus decides by were taken on one board. For another one
  `dgx_dsi_bench()` takes them, but the bus does not run it and does not
  keep what it gives: the program does, see "Measuring the board". Four
  numbers it does not measure.
- The measuring of the board was checked on one board, the very one the set
  numbers were taken on. On another chip, with another memory or another
  display nobody has run it.
- After this change the code was only built under IDF 5.5.4, 6.0.3 and 6.1,
  not run on the board: under 6.0.3 and 6.1 the exchange of bytes by the 2D
  DMA is broken, see issue 19226, and 5.5.4 was replaced by 5.5.5.
- How long a batch will take the bus judges by the last batches, and it has
  one estimate. If the batches are of different lengths it is wrong, and a
  ready picture waits an extra frame. In the check with random batches a
  ready picture missed 5% of the ends of frames, see "How it was checked".
- A picture that comes less often than a frame, and not after the same
  number of frames every time, moves unevenly. The bus does not even that
  out, see "The time of a frame", "Evenness".
- The size of the tiles a display has unless a program gives another was
  chosen by the demos on one board. A tile of one row was not made faster,
  see "The size of a tile".
- The threshold from which rows of a program go by the DMA counts bytes
  only. Narrow tall areas it leaves to the window, though the DMA would
  move them faster.
- There is no way in DGX to wait for the end of a frame. The bus tells how
  long a frame takes and when one ended, and a program sleeps and aims
  itself, see "The time of a frame". For pictures to come evenly several
  sources need one drawing task and messages to it; with tasks that draw
  themselves under a mutex evenness is a matter of chance.
- At 24 bits the copy after the change is off.
- Behind the scan a batch is not shown as one: of many tall fills some
  appear a frame later. To mend that the fills of a batch would have to be
  put off to its end, and they would still not fit one gap.
- Behind the scan there is no way to wait for the next frame. A program that
  updates more often than the display has to hold itself back.
- A mirrored and a turned screen, and 24 bits, are moved by the processor.
  Everything there is as it was before the DMA: 34 ms a screen. A fill by
  the PPA works there as well.
- The condition for writing by the processor behind the scan is twice as
  strict as it has to be, see its section. After the DMA that changes little.
- Nothing was checked on chips of revision 3 and later. For a chip of
  revision 3.2 there is an
  [issue 19188](https://github.com/espressif/esp-idf/issues/19188) by another
  author: a display on DSI shows an even color instead of the frame buffer.
  Under an IDF without the event `on_frame_buf_complete` the bus also has a
  branch of its own for such chips, see the table of events in "The scan".
- The 2D DMA is driven through a private interface of IDF and one field of a
  register. There is no answer to
  [issue 19226](https://github.com/espressif/esp-idf/issues/19226) yet.
- 24 bits were checked only by reading the buffer back. The factory firmware
  of the board works in 16 bits.
- The benches all of this was taken on are not in the repository.

## How it was checked

Correctness. A bench draws random batches both on the display and into a
virtual screen that is the model, and reads the frame buffer back. The
batches have fills, pixels, lines, circles, parts of a virtual screen, a
whole virtual screen, textures, text and reads in the middle of a batch. 300
batches in each of three orientations, all with the DMA and the PPA on:

| Mode | Bits | IDF | Pixels that differ |
| --- | --- | --- | --- |
| One buffer | 16 | 5.5.4 | 0 |
| Behind the scan | 16 | 5.5.4 | 0 |
| Two buffers | 16 | 5.5.4 | 0 |
| Two buffers | 16 | 6.1 | 0 |
| Two buffers | 24 | 6.1 | 0 |

Before the PPA was added the same checks passed under 6.0.1 and 6.0.3 as
well, and before the move to the private interface of the DMA under 5.5.5
too. With the final code there was only a build under those three versions.

Tearing is not caught by such a check: it is not seen in the buffer.

The change of the buffers. That a half-drawn picture is not shown and that
the batches of different tasks join is checked by a model of the display on
the host, `test/host/test_fb_flip.c`, see "Several tasks". On the board the
check was passed again after this change, under IDF 6.0.1, and with two
buffers at 16 bits under 5.5.5 as well. To the three runs of 300 batches
three more were added, in which the same random batches are drawn by three
tasks in turn. The counters over all six runs:

| Mode | Bits | IDF | Mismatches | Batches | Of them went into a ready picture | Frames | Of them a ready picture missed |
| --- | --- | --- | --- | --- | --- | --- | --- |
| One buffer | 16 | 6.0.1 | 0 | 1660 | the mode does not do that | not counted | the mode does not do that |
| Behind the scan | 16 | 6.0.1 | 0 | 1664 | the mode does not do that | 2274 | the mode does not do that |
| Two buffers | 16 | 6.0.1 | 0 | 1701 | 365 | 3077 | 169 |
| Two buffers | 16 | 5.5.5 | 0 | 1701 | 345 | 3141 | 169 |
| Two buffers | 24 | 6.0.1 | 0 | 1701 | 120 | 3875 | 117 |

The length of a random batch cannot be told from those before it, and a
ready picture misses ends of frames: 169 of 3077 at 16 bits, which is 5%,
and 117 of 3875 at 24 bits, which is 3%. More than half of them fall on the
run with a screen of 600x1024, where a batch with the checks takes 40 to 60
ms, longer than two frames: 69 and 89 misses there. One task draws in it
without pauses, its next batch goes into its own ready picture and keeps
that back for all the time it takes. While the bus told tasks apart and did
not let a batch into a picture of the same task, there were 18 and 19
misses. A program with batches of one length does not have this: in the
scenes of the bench from 0 to 5 ends of frames of 613 were missed.

Later all the modes were passed once more with the numbers of the measuring
of the board and with the size of the tiles changed every 5 batches, see
"Measuring the board": 0 mismatches.

Speed. The same application is built twice, with the library before a change
and after it, and flashed to the same board. A time on the host proves
nothing for an ESP32. Differences below 0.2 us a call depend on where the
code happens to lie.

Tearing. By eye, on the scenes of "Frame rate". Every firmware that has to be
judged by eye shows its build number on the screen.

The build was checked under IDF 5.5.4, 5.5.5, 6.0.1, 6.0.3 and 6.1. Built
separately but not run: the variant for a chip of revision 3 and later under
5.5.4, the variant with the option of the DMA and the PPA off under 5.5.4,
6.0.1 and 6.1, the strategy behind the scan under 6.1. The example
`examples/dsi_demo` was built under 5.5.4, 6.0.1 and 6.1, an example for a
board with an ESP32 under 5.5.4, 6.0.1 and 6.1. There are 19 tests on the
host.

That was before the joining of batches. The code of version 0.5.3 was run on
the board under IDF 6.0.1 in every mode, and two buffers under 5.5.5 as
well. Only built, with no warnings: the bench of the check against a model
with two buffers under 5.5.4, 6.0.3 and 6.1; the variant with the option of
the DMA and the PPA off under 5.5.5, 6.0.1 and 6.1; the variant for a chip
of revision 3 and later under 5.5.5 and 6.0.3; the example
`examples/dsi_demo` under all five versions; the other nine examples under
5.5.5 and 6.0.1. The example `dsi_demo` with the measured numbers kept in
NVS was run on the board, with the new way of keeping the pace it was only
built.
