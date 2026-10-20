# Using a MIPI-DSI Display in DGX

[Русская версия](dsi-usage-ru.md)

What a program needs to know to draw on a MIPI-DSI display with DGX: how the
screen is updated, what the `frames` argument chooses, how to keep an even
frame rate, and which settings depend on the board. How the bus is built,
what was measured and why it works this way is in
[dsi-display-en.md](dsi-display-en.md). A working program is
[examples/dsi_demo](../examples/dsi_demo).

The numbers below were measured on a Guition JC1060P470: an ESP32-P4 with a
JD9165 panel of 1024x600, except where the 10.1-inch JC8012P4A1C is named.

- [The display and its frame buffer](#the-display-and-its-frame-buffer)
- [Tearing and the `frames` argument](#tearing-and-the-frames-argument)
- [Two frame buffers](#two-frame-buffers)
- [Large pieces bypass the processor](#large-pieces-bypass-the-processor)
- [Scrolling](#scrolling)
- [Settings that depend on the board](#settings-that-depend-on-the-board)
- [One buffer, after the scan](#one-buffer-after-the-scan)
- [Several tasks](#several-tasks)

## The display and its frame buffer

A DSI display in video mode has no memory of its own. The chip keeps a frame
buffer in PSRAM and sends it to the panel all the time: 1.2 MB for 16-bit
color at 1024x600, 1.8 MB for 24-bit. Drawing changes that buffer.

```c
dgx_bus_protocols_t *bus = dgx_dsi_init(0, 2, 750, 3, 2500);
dgx_screen_t *lcd = dgx_jd9165_init(bus, GPIO_NUM_0, 16, DgxScreenRGB, DgxDsiTwoFrames);
```

- The JD9165 driver takes 16 or 24 bits per pixel. Its init commands belong
  to the glass it drives; they are those of the 7-inch panel of the
  JC1060P470 board, taken from the firmware the board ships with.
- Orientation is done by the bus when it stores pixels, so all eight
  orientations work; with `swap_xy` the screen is 600x1024.
- A pixel and a rectangle are written straight into the frame buffer, as on
  a virtual screen, and a pixel can be read back.
- The backlight is a GPIO of the board and is left to the application.

Group drawing into batches with `dgx_screen_progress_up()` and
`dgx_screen_progress_down()`. Anything drawn outside a batch is sent to the
display at once. A batch is sent as it goes and at its end, which is ten
times faster for pixels set one by one.

## Tearing and the `frames` argument

The display reads its frame buffer continuously, also while a program draws
into it. A picture that changes over a large part of the screen can be seen
half updated for one frame. No single remedy suits every program, so the
constructor takes the choice in its last argument:

| `frames` | Frame buffers | Tearing | Cost |
| --- | --- | --- | --- |
| `DgxDsiOneFrame` | 1 | possible | none: drawing shows at once |
| `DgxDsiTwoFrames` | 2 | none | one more frame of PSRAM, 1.2 MB for 1024x600 at 16 bits |
| `DgxDsiOneFrameAfterScan` | 1 | none for a virtual screen sent to the display and for a large filled rectangle | a wait of up to one frame for each of them |

## Two frame buffers

Drawing goes into the buffer the display is not reading, and a batch is
shown as a whole, starting with the next frame.

- The end of a batch does not show it immediately. It marks that a picture
  is ready, and the display takes it when its current frame ends. Batches
  that follow before that go into the same picture. So everything drawn
  within one frame time is shown by one swap of the buffers, whoever drew
  it. Three tasks, each with a 320x100 part of the screen, show 61 pictures
  a second each; they got 20 when frames were given to them in turn.
- A primitive drawn outside a batch is a batch of its own and can be shown
  without the next one. Put everything that belongs to one picture into one
  batch.
- The display does not set the pace. A program that draws more often than
  frames are shown only gets its last picture into each frame, so it has to
  hold itself back. `dgx_dsi_frame_us()` returns the length of a frame
  (16.4 ms here) and `dgx_dsi_frame_ended()` the time the last frame ended.
  With these two a program can start a picture so that it is finished
  shortly before a frame ends, and motion is even. `demo_pace()` in
  `examples/dsi_demo` shows this in full; the reasoning is in
  [dsi-display-en.md](dsi-display-en.md), "How a program keeps the pace".
- `dgx_dsi_counters()` reports what the display has done: frames sent,
  buffer swaps, batches, batches that joined a picture, and waits.

### What is copied between the buffers

After a batch, the other buffer lacks what the batch drew. The bus tracks
this in tiles of 32x4 pixels. A tile is copied when something is first drawn
in it or read from it, and the remaining tiles are copied when the next
picture is finished. Anything fully covered by a rectangle, or by a window
that is sent whole, is not copied at all. So a line from corner to corner
costs the tiles it crosses, not the screen, and a picture that is sent again
every frame costs nothing.

At 16 bits per pixel a large part of that remainder is copied even later:
after the display has taken the picture, by the 2D DMA, ahead of the scan,
started from the end-of-frame interrupt. If another task draws over the
piece before that, it is not copied at all. Three tasks with a 1024x168 band
each, redrawing a small part of it, show 61 pictures a second each, and less
than 1 MB is copied in 10 seconds instead of 411 MB.

## Large pieces bypass the processor

- A part of a 16-bit virtual screen of 16 KB or more is sent to the display
  by the 2D DMA of the chip, which also swaps the bytes of the pixels: a
  full 1024x600 screen takes 8 ms instead of 34. The screen must not be
  mirrored or rotated. Otherwise, and for 24-bit color, the processor moves
  the pixels as before.
- A large rectangle is filled by the pixel processing accelerator (PPA): the
  full screen in 3.5 ms instead of 16, in any orientation and color depth.
- An 8-bit virtual screen whose table is a ramp between two colors, like the
  one a soft pen draws on, is blended from those two colors by the same
  accelerator when the display has two frame buffers: 367 thousand pixels in
  10.6 ms instead of 17.1 ms through the table. The first and last colors
  are exactly those of the table. A color in between can differ from the
  table by one step of a channel: 3.2% of the range of red or blue, 1.6% of
  green, at most 2.3% of full brightness and 0.5% on average. Pass
  `DGX_VSCREEN_EXACT` to the call to get exactly the colors of the table; the
  screen then goes through its table.

The 2D DMA is driven through a private interface of IDF, and in case a new
IDF stops building it there is `CONFIG_DGX_DSI_PRIVATE_DMA2D`: on by default,
off it hands the moves back to the processor. The fill and the mix by the
accelerator do not depend on it.

## Scrolling

`dgx_scroll()` on a DSI display moves rows of the frame buffer; the panel
has no scroll of its own. The full screen scrolled by 28 rows takes 24 ms,
a 320x240 rectangle 2.7 ms. A scroll that is a batch of its own first
brings the back buffer up to date and costs more: 15.5 ms for a 400x480
chart. See "A picture scrolled" in [dsi-display-en.md](dsi-display-en.md)
and [examples/figures_scroll_demo](../examples/figures_scroll_demo).

## Settings that depend on the board

**Thresholds.** The size from which a piece bypasses the processor, and the
margin the bus keeps ahead of the scan, depend on the chip, its memory and
the display. The bus starts with the values of the board it was written on.
`dgx_dsi_bench()` measures them on the board at hand, in memory of its own
while the display keeps running, so nothing on the screen changes. It takes
about 0.3 s, which is too long for every start. The result is a flat 48-byte
structure, `dgx_dsi_tuning_t`: keep it, in NVS for example, and give it to
the bus with `dgx_dsi_tune()`. The bus does not take numbers of another
panel, color depth or PSRAM clock: the program then measures again.
`examples/dsi_demo` shows the whole cycle: the board measured and the
numbers written to NVS at the first start, read from there at the next
ones, and a second scene, "tuning", with the numbers on the screen. What the
numbers change is seen there on one piece: a 60x100 part of a virtual
screen reaches the display of the 10.1-inch board in 163 us with the
built-in numbers and in 77 us with the measured ones, through the window
against the 2D DMA.

**Advice.** The last part of `dgx_dsi_bench()` has the 2D DMA copy for a
dozen frames as fast as it can, while the bus watches the FIFO of the bridge
that feeds the DSI host: how many times a frame it runs empty
(`fifo_drops`). The structure also has how much of the lanes the pixels
take (`lanes_percent`, by the timings). Of this a program needs one decision, and `dgx_dsi_advise()` gives
it: whether the display needs two frame buffers. It measures nothing, so a
program can ask it with the numbers from NVS before it makes the display.
`examples/dsi_demo` does so: at a start that finds the numbers in NVS the
display gets two frame buffers when the advice says so; at the very first
start, when the display was made before the measurement and the advice asks
for two buffers, the example restarts once.

```c
dgx_dsi_tuning_t tuning;
/* ... read from NVS, or dgx_dsi_bench() once ... */
const uint8_t advice = dgx_dsi_advise(&tuning);
const dgx_dsi_frames_t frames = advice & DGX_DSI_ADVISE_TWO_FRAMES ? DgxDsiTwoFrames : DgxDsiOneFrame;
```

| Flag | Means | What to do |
| --- | --- | --- |
| `DGX_DSI_ADVISE_TWO_FRAMES` | more than 15 drops a frame: with one frame buffer heavy drawing leaves the display short, and the screen flashes for a frame | `DgxDsiTwoFrames` |

The rest of what the numbers say is for the developer, not the program: the
PSRAM clock and the color depth are settings of the build, the lanes and
the panel are the board. `dgx_dsi_tuning_text()` puts it in words, one
paragraph: the PSRAM clock and the chip revision; how often the display ran
short of data under the 2D DMA and whether that is margin enough, and if
not what mends it (two frame buffers, or a faster PSRAM clock if the chip
has one); how much of the lanes the pixels take and whether that is too
much for the color depth. `dgx_dsi_bench()` logs the same text, as a warning
when the margin is short. A library that cannot act on it hands the text on
to its user.

```c
char text[400];
dgx_dsi_tuning_text(&tuning, text, sizeof(text));
ESP_LOGI(TAG, "%s", text);
```

The limits were found by eye on the 10.1-inch JC8012P4A1C, chip revision
3.2, IDF 6.0.1: 16 bits, PSRAM at 200 MHz gave 29 to 33 drops a frame, a
moving picture with one buffer flashed and with two did not; at 250 MHz 8
to 10, no flashes; 24 bits took 96% of the lanes and flashed now and then
with nothing drawn. Other boards may draw the line elsewhere.

**Tile size.** The best size depends on what a program draws. The default
is 32x4. Compared with 32x8, which is the fastest for the DGX demos, it is
0 to 3% slower there, which cannot be seen, 6% faster for bitmap-font text
and 28% faster for scattered single pixels. To use another size call
`dgx_dsi_tiles(bus, width, height)` once, before the display constructor.
The call also works later, between two batches, but a change then costs a
wait for the display of 11 ms or more, so it is not meant for switching
sizes while a program runs.

**PSRAM cache.** `examples/dsi_demo` has the settings of a board and shows
what the size of the PSRAM cache costs and gives.

**PSRAM speed.** The display reads its frame from PSRAM all the time:
pixel clock times bytes a pixel within a row, 104 MB/s for the 1024x600
panel and 160 MB/s for the 800x1280 one at 16 bits, 75 and 123 MB/s averaged
over a frame. Drawing shares the same memory. When the bridge in front of
the DSI host runs out of data, it sends a color of its own for the rest of
the frame, and the screen flashes light blue for a frame, with torn rows
around it. IDF has an interrupt for this, but on the 10.1-inch board it
never fired, so the flash is the only sign. What was seen on the JC8012P4A1C
with a chip of revision 3.2:

- one frame buffer, PSRAM at 200 MHz: the moving bars of `examples/dsi_demo`
  cause such flashes now and then; raising the AXI priority of the DMA that
  reads the frame does not stop them;
- one buffer, PSRAM at 250 MHz: no flashes. 250 MHz exists on chips of
  revision 3 and later (`CONFIG_SPIRAM_SPEED_250M`);
- two frame buffers (`DgxDsiTwoFrames`), PSRAM at 200 MHz: no flashes. That
  is the way for an older chip, whose PSRAM goes up to 200 MHz; such a chip
  has not been tried with this panel, this was seen on the chip of revision
  3.2 with its PSRAM set to 200 MHz;
- 24 bits a pixel: the display reads half as much again, and drawing leaves
  the bridge short even at 250 MHz. IDF then prints "underrun happens", and
  a flash is seen now and then. Prefer 16 bits there.

## One buffer, after the scan

The DMA and the PPA write faster than the display reads.
`DgxDsiOneFrameAfterScan` uses that: it starts them when the scan of the
display has left the rows they are going to write. They stay ahead of the
scan, and the part or the rectangle is seen whole with a single frame
buffer, even when it is the whole screen.

This mode is a compromise:

- a picture that comes from a virtual screen does not tear;
- drawing directly on the display is held back only for large rectangles,
  and a batch is not shown as one: of many tall rectangles some appear one
  frame later than the others;
- the wait does not set the pace, so a program that updates more often than
  the display shows frames still has to hold itself back.

## Several tasks

Tasks have to draw on a screen one at a time, on a DSI display as on any
other; see "Drawing from several tasks" in the
[README](../README.md#drawing-from-several-tasks).

With two frame buffers the bus does one thing for several tasks: whatever
they draw within one frame time is shown by one swap of the buffers. The bus
does not know which task draws and does not care. Which task draws when, how
often and how evenly is up to the program; `dgx_dsi_frame_us()` and
`dgx_dsi_frame_ended()` give it what it needs to know about the display.

The DSI bus is the only place where DGX uses interrupts. At the end of a
frame it counts the frame and starts the copy a picture still lacks, and it
is notified when a 2D DMA transfer is done. These handlers touch only the
bus and never call code of the program.
