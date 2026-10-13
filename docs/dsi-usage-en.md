# Using a MIPI-DSI Display in DGX

[Русская версия](dsi-usage-ru.md)

What a program needs to know to draw on a MIPI-DSI display with DGX: how the
screen is updated, what the `frames` argument chooses, how to keep an even
frame rate, and which settings depend on the board. How the bus is built,
what was measured and why it works this way is in
[dsi-display-en.md](dsi-display-en.md). A working program is
[examples/dsi_demo](../examples/dsi_demo).

The numbers below were measured on a Guition JC1060P470: an ESP32-P4 with a
JD9165 panel of 1024x600.

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
  `DGX_LUT_EXACT` to the call to get exactly the colors of the table; the
  screen then goes through its table.

`CONFIG_DGX_DSI_HARDWARE_COPY` turns the DMA and the PPA paths off.

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
0.07 s, which is too long for every start. The result is a flat 40-byte
structure, `dgx_dsi_tuning_t`: keep it, in NVS for example, and give it to
the bus with `dgx_dsi_tune()`. `examples/dsi_demo` does this.

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
