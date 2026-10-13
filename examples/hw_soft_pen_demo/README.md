# DGX Soft Pen Demo

Writes four lines by hand with a pen that has a soft edge, `DGX_PEN_SOFT`, on
one of two boards: a CYD (ESP32-2432S028R, an ILI9341 of 320x240 on SPI) or
a Guition JC1060P470 (ESP32-P4, a JD9165 of 1024x600 on MIPI-DSI). Two lines, «Привет,» and «Perl!», are
written as large as the screen takes them. Then they get lower, as wide as
they were, and go up to make room, and two more are written under them: «&»
and «Modern::Perl». After a while the same pen with no ink goes the same way
and takes everything off, and it starts again.

What it shows:

- a soft pen: `DGX_PEN_SOFT | pen` in place of the pen, into a virtual screen
  of 8 bits a pixel, where a byte is an amount of ink, sent to the display
  through a table that runs from the color of the paper to the color of the
  ink. Only what has changed goes to the display, `dgx_screen_take_dirty()`;
- a size of its own along a line and up it, `dgx_hw_writing_begin_xy()` and
  `dgx_hw_draw_text_xy()`: the two upper lines keep their width when they get
  lower, the last line is as large as the first one on the screen though it
  has more letters, and the pen stays round;
- a move that is no morph: a frame is the two lines drawn anew at a size and a
  place between the two layouts;
- taking off: a soft pen with color 0 leaves nothing of what it wrote.

Things to change at the top of `main/main.c`: the lines, how large the sign
of the third line is beside the others, the pace of the pen, the durations.
`EXAMPLE_BUILD`, when it is not 0, is shown in a corner before a round
starts.

The frame rate of the move and the time a frame takes to draw go to the
serial log.

## Build

For a CYD:

```sh
cd examples/hw_soft_pen_demo
idf.py set-target esp32
idf.py build flash monitor
```

For a JC1060P470:

```sh
cd examples/hw_soft_pen_demo
idf.py set-target esp32p4
idf.py build flash monitor
```

On the P4 the move of the lines runs at 31 frames a second: a frame is wiped
and drawn in 18 ms and its part of the page, 367 thousand pixels, is sent to
the display in 13.5 ms. The table of the page is two colors and what lies
between them, so the pixel accelerator of the chip mixes the color of the
ink into the color of the paper, and the processor carries no pixel. A color
between the two may differ from the table by a step of a channel; with
`DGX_LUT_EXACT` in place of `false` in `show()` the page goes through the
table, to the last bit, in 20.7 ms and at 25 frames a second.

The settings of each board are in `sdkconfig.defaults.esp32` and
`sdkconfig.defaults.esp32p4`. On the P4 the page of ink, 1024x600 bytes, and
two frame buffers of the display live in PSRAM; the board the demo was made
on has a chip of revision 1.3, see the note in `sdkconfig.defaults.esp32p4`.
