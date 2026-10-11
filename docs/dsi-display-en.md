# A MIPI-DSI Display in DGX: Frame Buffer, Scan and Tearing

[Русская версия](dsi-display-ru.md)

A note on how the MIPI-DSI bus for the ESP32-P4 is made in DGX, what was
measured on it, and why the screen is updated the way it is.

It describes the code of version 0.5.2: the bus `src/bus/dsi_esp32.c`, the
frame buffer core `src/bus/fb_area.c`, the tile maps `src/bus/fb_tiles.c`,
the driver `src/drivers/jd9165.c`.

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
| IDF | measurements under 5.5.4 and 6.1, each table says which; checks also under 5.5.5, 6.0.1 and 6.0.3 |

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
- **Tile**: a cell of a fixed grid the bus cuts the panel into, 32x8 pixels.
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
and with two buffers changing the buffers too. One call of `update_screen()`
shows one batch.

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

Tiles are a fixed grid laid over the panel: cells 32 pixels wide and 8 high,
for 1024x600 that is 32 tiles in a row and 75 rows. They differ from windows
in that a window is set by the drawing code and may be anything, and the grid
of tiles is one and does not change.

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
| `DgxDsiTwoFrames` | 2 | none | one more buffer in PSRAM, at most a batch a frame |
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
2. At the end of a batch the bus asks the driver to show this buffer from the
   next frame on.
3. The first drawing of the next batch waits until the change has happened.
   After that the buffers change roles.

The cost:

- one more buffer in PSRAM, 1.23 MB at 16 bits;
- at most one batch a frame. A drawing function called without
  `dgx_screen_progress_up()` is a batch of its own, and the one after it
  waits for a frame. So in this mode everything that has to appear together
  has to be put into a batch;
- the update rate can only be 60, 30, 20 or 15 times a second, by how many
  frames a batch takes. That is a property of any strategy without tearing:
  every frame shown is whole. Values in between are possible only with
  tearing;
- after the change the hidden buffer does not hold what the last batch drew:
  that one drew into the other buffer. It has to be copied in. From here on
  that is called the catch-up copy; see "The cost of copying" and "The
  catch-up copy by tiles".

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
to 165 rows of the full width fits.

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
that is not needed: there the bus waits for the buffers to change.

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

It is made by tiles. The panel is cut into tiles of 32x8 pixels, a bit a
tile. There are two maps. Each has a 32-bit word for a row of tiles, for
1024x600 that is 75 words, 600 bytes together. If a panel is wider than 1024
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
batch. The line goes through about a hundred tiles of 2400. The next batch
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

The low tile costs 448 bytes of internal RAM. It was chosen.

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
| What marks the changed | rectangles, 32 at most | tiles of 32x8, 2400 of them on 1024x600 |
| A line from corner to corner | one rectangle of the whole screen | about a hundred tiles |
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
a rectangle with a color. In the bus they have three jobs.

| Job | By what | What is done | From what size |
| --- | --- | --- | --- |
| A virtual screen put to the display | 2D DMA | rows of a 16-bit virtual screen go into the frame buffer, with the byte exchange | from 16 KB |
| The catch-up copy with two buffers | 2D DMA | what is left to copy before the buffers change goes from one frame buffer into the other | rows × (width + 53) of 8000 or more |
| A fill of a rectangle | PPA | a rectangle of the frame buffer is filled with a color | rows + bytes / 64 of 200 or more |

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

Speed. The same application is built twice, with the library before a change
and after it, and flashed to the same board. A time on the host proves
nothing for an ESP32. Differences below 0.2 us a call depend on where the
code happens to lie.

Tearing. By eye, on the scenes of "Frame rate". Every firmware that has to be
judged by eye shows its build number on the screen.

The build was checked under IDF 5.5.4, 5.5.5, 6.0.1, 6.0.3 and 6.1. Built
separately but not run: the variant for a chip of revision 3 and later under
5.5.4, the variant with the option of the DMA and the PPA off under 5.5.4 and
6.1, the strategy behind the scan under 6.1. The example `examples/dsi_demo` and an example for
a board with an ESP32 were built under 5.5.4 and 6.1. There are 18 tests on
the host.
