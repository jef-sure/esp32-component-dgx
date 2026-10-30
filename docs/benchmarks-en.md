# Benchmarks

How fast DGX draws on four boards, and beside TFT_eSPI, LovyanGFX and
Adafruit_ILI9341 on one of them. The test is the PDQ graphics test those
libraries ship; DGX runs it as [examples/graphicstest](../examples/graphicstest).
Times are in microseconds, as the test prints them.

Contents:

- [The test](#the-test)
- [The boards](#the-boards)
- [Four libraries on a CYD](#four-libraries-on-a-cyd)
- [Drawn in memory and sent](#drawn-in-memory-and-sent)
- [Three boards side by side](#three-boards-side-by-side)
- [A round ST77916 on QSPI](#a-round-st77916-on-qspi)
- [The other libraries against their published numbers](#the-other-libraries-against-their-published-numbers)
- [What the SPI bus of DGX does](#what-the-spi-bus-of-dgx-does)
- [DGX before and after, on SPI and QSPI](#dgx-before-and-after-on-spi-and-qspi)
- [A MIPI-DSI display](#a-mipi-dsi-display)
- [Where DGX is still slower](#where-dgx-is-still-slower)

## The test

TFT_eSPI keeps the test as `examples/320 x 240/TFT_graphicstest_PDQ`, and
LovyanGFX keeps the same sketch as `examples/Test/TFT_graphicstest_PDQ`. It
was made from Adafruit's `graphicstest` by Xark for his PDQ_GFX library. Each
line of its table times one kind of drawing on a black screen:

| Line | What is drawn and timed |
| --- | --- |
| HaD pushColor | The Hackaday logo, 240x320 pixels in runs of two colors, 16 times, sent into one window; `pushColor()` is the TFT_eSPI call that sends a run of pixels of one color |
| Screen fill | The whole screen filled 5 times, the time divided by 5 |
| Text | Lines of text in the 5x7 font at sizes 1, 2, 3, 5 and 6 |
| Pixels | Every pixel of the screen set alone, each of another color |
| Lines | Lines from each corner to every 6th pixel of the far sides, the four corners summed |
| Horiz/Vert Lines | A horizontal line every 5 rows and a vertical one every 5 columns |
| Rectangles (outline) | Squares about the middle, every 6 pixels larger |
| Rectangles (filled) | The same squares filled, the largest first, each timed alone |
| Circles (filled) | Filled circles of radius 10 over the whole screen |
| Circles (outline) | Circles of radius 10 over the whole screen and past its edges |
| Triangles (outline) | Triangles about the middle, every 5 pixels larger |
| Triangles (filled) | The same triangles filled, each timed alone |
| Rounded rects (outline) | Squares with corners of a radius of 1/8 of their side |
| Rounded rects (filled) | The same squares filled |

Adafruit's own `graphicstest` differs: its fill line is the sum of the 5
fills, its line test keeps only the time of the fourth corner, and it has no
logo and no pixel test. Its numbers are not those of the PDQ test.

The PDQ test ran on every library as it is published, with these changes:

- **All:** the pins of the board, and the prototypes the Arduino IDE adds to
  a sketch, so that it builds as a C++ file.
- **LovyanGFX:** a class for the panel of the board in place of the one
  `LGFX_TFT_eSPI.hpp` would detect. Its sketch calls `startWrite()` once
  and never ends it, so it holds the SPI bus for the whole run; the
  TFT_eSPI sketch takes the bus for every call.
- **Adafruit_ILI9341**, which ships no PDQ test: the TFT_eSPI sketch, with
  `writeColor()` for the runs of the logo, as Adafruit_GFX has no
  `pushColor()` with a count, and `writePixel()` inside `startWrite()` for
  the pixels, as Adafruit's `drawPixel()` takes the bus itself.

DGX runs the same figures, sizes, colors and order. Where DGX has no single
call for a figure it uses the calls a program would: a horizontal or
vertical line is `dgx_fill_rectangle()` a pixel high or wide, the outline
of a triangle is three `dgx_draw_line()`, the logo goes by `set_area()` and
`write_area()` of the screen, and the text is drawn in `GLCD5x7`, the font
of the other libraries, at the same sizes. A time ends when the display has
the last pixel: DGX may still be sending when a call returns, so the clock
is read after `scr->wait_buffer()`. The other libraries return from these
calls when the bus is done.

## The boards

The boards, as they were measured on 2026-10-30:

| Board | Chip | Panel | Bus | Upright screen |
| --- | --- | --- | --- | --- |
| CYD, ESP32-2432S028R | ESP32-D0WD-V3, revision 3.1, 240 MHz | ILI9341, 240x320 | SPI2 (HSPI) on pins 14, 13, 12 and 15 of the IO MUX, D/C on 2, 40 MHz | 240x320 |
| Guition JC4827W543 | ESP32-S3 (QFN56), revision 0.2, 240 MHz | NV3041A, 480x272 | QSPI on SPI2, 40 MHz | 272x480 |
| Guition JC1060P470 | ESP32-P4, revision 1.3, 360 MHz | JD9165, 1024x600 | MIPI-DSI, 2 lanes of 750 Mbit/s, one frame buffer in PSRAM | 600x1024 |
| ESP32-S3 dev board with the round module "TFT 1.53 1.0" | ESP32-S3 (QFN56), revision 0.2, 240 MHz | ST77916, 360x360, glass T150XV-C16-04 | QSPI on SPI2, 40 MHz, jumper leads | 360x360 |

DGX was built with ESP-IDF 5.5.4, GCC 14.2 and `-O2`, the processor at the
clock in the table. TFT_eSPI 2.5.43, LovyanGFX 1.2.7 and Adafruit_ILI9341
1.6.2 with Adafruit GFX Library 1.12.1 were built by PlatformIO 6.2.0,
platform espressif32 6.10.0: the Arduino core 2.0.17 on ESP-IDF 4.4, GCC 8.4
and the `-Os` of that core, the processor at 240 MHz. The other libraries
ran only on the CYD: TFT_eSPI has no QSPI or DSI bus.

## Four libraries on a CYD

The CYD, SPI at 40 MHz, upright at 240x320; the second of two runs of each,
which differed by less than 1 %:

| Test | DGX | TFT_eSPI | LovyanGFX | Adafruit |
| --- | ---: | ---: | ---: | ---: |
| HaD pushColor | 533 959 | 511 150 | 504 381 | 657 095 |
| Screen fill | 31 287 | 31 183 | 30 816 | 40 037 |
| Text | 22 833 | 15 283 | 10 172 | 33 025 |
| Pixels | 650 896 | 224 013 | 226 978 | 583 127 |
| Lines | 599 548 | 266 229 | 258 198 | 1 000 627 |
| Horiz/Vert Lines | 17 053 | 14 376 | 12 594 | 18 297 |
| Rectangles (outline) | 12 300 | 8 905 | 8 097 | 11 990 |
| Rectangles (filled) | 327 554 | 324 090 | 318 123 | 416 103 |
| Circles (filled) | 66 462 | 46 696 | 40 792 | 73 377 |
| Circles (outline) | 50 385 | 26 350 | 23 097 | 112 500 |
| Triangles (outline) | 34 537 | 16 672 | 15 539 | 55 918 |
| Triangles (filled) | 164 448 | 112 404 | 106 946 | 154 952 |
| Rounded rects (outline) | 23 683 | 16 572 | 12 467 | 46 627 |
| Rounded rects (filled) | 346 428 | 326 820 | 322 011 | 423 124 |
| Total | 2 881 373 | 1 940 743 | 1 890 211 | 3 626 799 |

A full screen of 16-bit pixels takes the wire 30.7 ms at 40 MHz. The fills,
the filled rectangles and rounded squares and the logo are that much of the
wire and nearly nothing else, in every library but Adafruit's. Every other line is a great
many small windows, and there DGX is 1.2 to 2.9 times as slow as TFT_eSPI;
see [Where DGX is still slower](#where-dgx-is-still-slower).

## Drawn in memory and sent

However fast the bus, what is drawn on the panel piece by piece is seen
piece by piece. Only a picture finished first and sent whole, a virtual
screen or a prepared image, shows no flicker, so a DGX program draws its
figures in memory and sends the result. The test runs a second time that
way. Each test draws its picture into an 8-bit virtual screen of the size of the display,
its time taken as before, and one transfer of the whole of it to the display
is added, through a table of 256 colors, RGB332. A 16-bit screen of 240x320
is 150 KB, more than a CYD has free in one piece. TFT_eSPI keeps its 8-bit
sprites as RGB332 too, and its sketch run so stands beside DGX: the tests
draw into an 8-bit sprite of the screen, and `pushSprite()` after each is
added to its time.

The CYD, the second of two runs. In "DGX on the display" every call goes
straight to the panel; in the other two columns a time is the drawing in
memory and one send of the whole screen together:

| Test | DGX on the display | DGX in memory | TFT_eSPI, 8-bit sprite |
| --- | ---: | ---: | ---: |
| HaD pushColor | 533 959 | 48 668 | 327 052 |
| Screen fill | 31 287 | 32 330 | 37 860 |
| Text | 22 833 | 36 425 | 40 720 |
| Pixels | 650 896 | 92 062 | 79 425 |
| Lines | 599 548 | 74 041 | 71 186 |
| Horiz/Vert Lines | 17 053 | 32 387 | 41 099 |
| Rectangles (outline) | 12 300 | 32 409 | 39 846 |
| Rectangles (filled) | 327 554 | 34 979 | 41 337 |
| Circles (filled) | 66 462 | 35 602 | 40 448 |
| Circles (outline) | 50 385 | 36 246 | 41 857 |
| Triangles (outline) | 34 537 | 34 648 | 40 419 |
| Triangles (filled) | 164 448 | 35 335 | 40 826 |
| Rounded rects (outline) | 23 683 | 33 476 | 40 788 |
| Rounded rects (filled) | 346 428 | 35 479 | 40 597 |
| Total | 2 881 373 | 594 087 | 923 460 |

A transfer of the whole screen takes DGX 32 ms and TFT_eSPI 39 ms, and that
is most of every test. Pixels set one by one in memory cost DGX 0.78 us
each, 0.53 us TFT_eSPI. They part most on the logo: `write_area()` of a
virtual screen copies whole rows, about 17 ms for the 16 logos, where the
sprite takes about 288 ms. The JC4827W543, its
virtual screen of 130 KB in internal memory:

| Test | on the display | in memory |
| --- | ---: | ---: |
| HaD pushColor | 163 874 | 29 164 |
| Screen fill | 13 371 | 14 078 |
| Text | 19 878 | 17 388 |
| Pixels | 1 013 447 | 99 832 |
| Lines | 988 612 | 72 571 |
| Horiz/Vert Lines | 11 876 | 14 136 |
| Rectangles (outline) | 8 325 | 14 042 |
| Rectangles (filled) | 120 982 | 17 416 |
| Circles (filled) | 80 046 | 18 964 |
| Circles (outline) | 81 253 | 19 429 |
| Triangles (outline) | 41 273 | 16 361 |
| Triangles (filled) | 135 978 | 17 331 |
| Rounded rects (outline) | 23 767 | 15 236 |
| Rounded rects (filled) | 154 621 | 17 923 |
| Total | 2 857 303 | 383 871 |

## Three boards side by side

What the next chips and displays give. Each board draws at the size of its
own screen, so a row shows what that board does, not one picture drawn three
times; only the logo is 240x320 everywhere. In the "display" columns every
call draws straight on the display; in the "memory" columns each test draws
into an 8-bit virtual screen, which is then sent once:

| Test | CYD 240x320, display | CYD 240x320, memory | JC4827W543 272x480, display | JC4827W543 272x480, memory | JC1060P470 600x1024, display | JC1060P470 600x1024, memory |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| HaD pushColor | 533 959 | 48 668 | 163 874 | 29 164 | 1 734 100 | 41 594 |
| Screen fill | 31 287 | 32 330 | 13 371 | 14 078 | 3 547 | 38 685 |
| Text | 22 833 | 36 425 | 19 878 | 17 388 | 4 936 | 33 503 |
| Pixels | 650 896 | 92 062 | 1 013 447 | 99 832 | 3 295 808 | 191 330 |
| Lines | 599 548 | 74 041 | 988 612 | 72 571 | 632 674 | 238 744 |
| Horiz/Vert Lines | 17 053 | 32 387 | 11 876 | 14 136 | 60 804 | 43 367 |
| Rectangles (outline) | 12 300 | 32 409 | 8 325 | 14 042 | 37 756 | 34 357 |
| Rectangles (filled) | 327 554 | 34 979 | 120 982 | 17 416 | 77 071 | 176 725 |
| Circles (filled) | 66 462 | 35 602 | 80 046 | 18 964 | 60 266 | 51 517 |
| Circles (outline) | 50 385 | 36 246 | 81 253 | 19 429 | 65 880 | 50 297 |
| Triangles (outline) | 34 537 | 34 648 | 41 273 | 16 361 | 39 623 | 39 602 |
| Triangles (filled) | 164 448 | 35 335 | 135 978 | 17 331 | 1 935 747 | 67 570 |
| Rounded rects (outline) | 23 683 | 33 476 | 23 767 | 15 236 | 49 920 | 37 098 |
| Rounded rects (filled) | 346 428 | 35 479 | 154 621 | 17 923 | 637 672 | 189 094 |
| Total | 2 881 373 | 594 087 | 2 857 303 | 383 871 | 8 635 804 | 1 233 483 |

The memory rows that draw least show what sending the whole screen takes:
about 32 ms on the CYD, 14 ms on the JC4827W543 with 1.7 times the pixels,
and 33 ms on the JC1060P470 with eight times the pixels. The long rows of
the JC1060P470 on the display are the writes of the cache of its processor
and its turned screen: see [A MIPI-DSI display](#a-mipi-dsi-display).

## A round ST77916 on QSPI

The same chip and clock as the JC4827W543, on a dev board with a round module,
built from a copy of the example with the pins and the driver changed. The
screen has almost the same number of pixels, 129 600 against 130 560. The
filled rectangles, triangles and rounded squares are larger only because the
test sizes them by the shorter side, 360 against 272.

The ST77916, one round, microseconds:

| Test | on the display | in memory |
| --- | ---: | ---: |
| HaD pushColor | 163 901 | 29 058 |
| Screen fill | 13 268 | 13 931 |
| Text | 19 842 | 17 267 |
| Pixels | 1 005 658 | 99 077 |
| Lines | 974 642 | 71 277 |
| Horiz/Vert Lines | 11 524 | 14 048 |
| Rectangles (outline) | 12 914 | 14 097 |
| Rectangles (filled) | 275 480 | 20 823 |
| Circles (filled) | 78 325 | 18 682 |
| Circles (outline) | 80 773 | 19 228 |
| Triangles (outline) | 73 444 | 18 224 |
| Triangles (filled) | 280 250 | 20 153 |
| Rounded rects (outline) | 39 173 | 15 974 |
| Rounded rects (filled) | 337 078 | 21 663 |
| Total | 3 366 272 | 393 502 |

A fill of the whole screen takes 13.3 ms at 16 bits a pixel and 20.0 ms at 18.

## The other libraries against their published numbers

The closest published run is that of embedded-kiddie on the same board,
[Arduino-CYD-2432S028R](https://github.com/embedded-kiddie/Arduino-CYD-2432S028R/tree/main/TFT_graphicstest_PDQ):
TFT_eSPI 2.5.43 and LovyanGFX 1.2.0 with the Arduino core 3.1.1, the
ILI9341 at 40 MHz on HSPI. The runs here against it:

| Test | TFT_eSPI here | TFT_eSPI published | LovyanGFX here | LovyanGFX published |
| --- | ---: | ---: | ---: | ---: |
| HaD pushColor | 511 150 | 512 817 | 504 381 | 504 361 |
| Screen fill | 31 183 | 31 286 | 30 816 | 30 830 |
| Text | 15 283 | 16 595 | 10 172 | 10 917 |
| Pixels | 224 013 | 247 195 | 226 978 | 231 988 |
| Lines | 266 229 | 287 673 | 258 198 | 263 491 |
| Horiz/Vert Lines | 14 376 | 14 406 | 12 594 | 13 855 |
| Rectangles (outline) | 8 905 | 8 974 | 8 097 | 8 583 |
| Rectangles (filled) | 324 090 | 325 090 | 318 123 | 320 345 |
| Circles (filled) | 46 696 | 48 410 | 40 792 | 42 747 |
| Circles (outline) | 26 350 | 28 130 | 23 097 | 25 617 |
| Triangles (outline) | 16 672 | 17 788 | 15 539 | 15 763 |
| Triangles (filled) | 112 404 | 113 723 | 106 946 | 107 887 |
| Rounded rects (outline) | 16 572 | 17 241 | 12 467 | 12 930 |
| Rounded rects (filled) | 326 820 | 328 424 | 322 011 | 322 329 |

What the wire decides is the same within 0.5 %; the rest is up to 10 %
faster here, on the older Arduino core. No run of Adafruit_ILI9341 on a CYD
was found. The table in the README of
[Arduino_GFX](https://github.com/moononournation/Arduino_GFX) has
Adafruit_GFX on another ESP32 board with an ILI9341 at 40 MHz, with
Arduino_GFX's own version of the test: its lines that time the same work
are within about 8 % of the run here (fills 195 782 for five against 5 x 40 037,
lines 1 062 311 against 1 000 627, outlined circles 118 125 against
112 500).

## What the SPI bus of DGX does

Up to 0.9.3 DGX sent every transfer through the SPI master driver of
ESP-IDF, and the driver takes far longer to start a transfer than the wire
takes to send a few bytes. A window of which only the columns change and a single pixel are four transfers
on SPI: the column command, its four bytes, the memory write command and
the pixel. Measured on the CYD, means of 1000:

| What | us |
| --- | ---: |
| One byte by `spi_device_polling_transmit()`, on a host with DMA | 12.3 |
| The same on a host without DMA | 10.6 |
| The same with the bus acquired beforehand | 8.3 |
| `spi_device_acquire_bus()` and `spi_device_release_bus()` | 6.9 |
| One byte by the registers of the host | 0.95 |
| A window and a pixel by the registers of the host | 4.7 |

Since then a display holds the bus from a window to the end of its drawing,
and while it holds it the short transfers go by the registers of the host:
commands, their short parameters and pixels of up to 256 bytes, in transfers of
60 bytes, whole pixels of 2, 3 and 4 bytes. `spi_device_acquire_bus()` sets
the host for the device; before the first transfer by the registers after a
transfer of the driver, the host is set back for them. Longer pixels go by
DMA through the driver as before, and the processor fills the next piece
while one is sent.

- **When the bus is let go of.** A display made by `dgx_spi_init()` or
  `dgx_qspi_init()` is alone on the host DGX set up, and keeps the bus
  between its drawings: acquiring it costs 6.9 us, more than a pixel. A
  second display of DGX on the host makes it let go, and the two take turns
  at every drawing. A display made by `dgx_spi_init_on_bus()` or
  `dgx_qspi_init_on_bus()` lets go at
  the end of every drawing, or of the batch around it, for the other
  devices of the program's host.
- **QSPI** sends an opcode and an address on one line before the data, on one
  line for parameters and on four for pixels, so a window and its pixels
  switch between one and four lines at almost every transfer.
  `spi_ll_master_set_line_mode()` writes a dozen fields of the registers,
  and on an ESP32-S3 a field is a read of the register too: a transfer of a
  command with four bytes took 5.8 us so, and takes 3.0 us with the one field
  that tells the two apart written alone.
- **Registers written whole** rather than field by field would take a byte
  0.77 us instead of 0.95 us on the CYD, as TFT_eSPI does it, but the layout
  of the registers differs between the chips: DGX keeps to the `spi_ll_*`
  functions of ESP-IDF, which serve every chip.

## DGX before and after, on SPI and QSPI

The CYD, SPI at 40 MHz, 240x320, DGX 0.9.3 and DGX now:

| Test | 0.9.3 | now |
| --- | ---: | ---: |
| HaD pushColor | 533 106 | 533 959 |
| Screen fill | 31 308 | 31 287 |
| Text | 123 100 | 22 833 |
| Pixels | 4 202 678 | 650 896 |
| Lines | 4 186 358 | 599 548 |
| Horiz/Vert Lines | 20 665 | 17 053 |
| Rectangles (outline) | 19 708 | 12 300 |
| Rectangles (filled) | 329 696 | 327 554 |
| Circles (filled) | 224 099 | 66 462 |
| Circles (outline) | 333 608 | 50 385 |
| Triangles (outline) | 231 332 | 34 537 |
| Triangles (filled) | 309 468 | 164 448 |
| Rounded rects (outline) | 102 866 | 23 683 |
| Rounded rects (filled) | 379 536 | 346 428 |
| Total | 11 027 528 | 2 881 373 |

The JC4827W543, QSPI at 40 MHz, 272x480, DGX 0.9.3 and DGX now:

| Test | 0.9.3 | now |
| --- | ---: | ---: |
| HaD pushColor | 162 695 | 163 874 |
| Screen fill | 13 372 | 13 371 |
| Text | 85 578 | 19 878 |
| Pixels | 5 114 085 | 1 013 447 |
| Lines | 4 995 066 | 988 612 |
| Horiz/Vert Lines | 13 417 | 11 876 |
| Rectangles (outline) | 12 308 | 8 325 |
| Rectangles (filled) | 122 217 | 120 982 |
| Circles (filled) | 253 944 | 80 046 |
| Circles (outline) | 395 420 | 81 253 |
| Triangles (outline) | 206 197 | 41 273 |
| Triangles (filled) | 232 015 | 135 978 |
| Rounded rects (outline) | 91 322 | 23 767 |
| Rounded rects (filled) | 172 501 | 154 621 |
| Total | 11 870 137 | 2 857 303 |

The screen of the JC4827W543 has 1.7 times the pixels of the CYD, and four
lines send them four times as fast: its fills and the logo take less time.
A pixel set alone takes 7.8 us there against 8.5 us on the CYD, though a
window on QSPI is two transfers, each with an opcode and an address of 32
bits on one line.

## A MIPI-DSI display

The JC1060P470 with one frame buffer: DGX draws into the memory the panel
shows, and the drawing is there when a call returns, and in memory: its
virtual screen of 600 KB lies in PSRAM. The screen is 600x1024, eight times
the pixels of the CYD, so the lines of the test draw more:

| Test | on the display | in memory |
| --- | ---: | ---: |
| HaD pushColor | 1 734 100 | 41 594 |
| Screen fill | 3 547 | 38 685 |
| Text | 4 936 | 33 503 |
| Pixels | 3 295 808 | 191 330 |
| Lines | 632 674 | 238 744 |
| Horiz/Vert Lines | 60 804 | 43 367 |
| Rectangles (outline) | 37 756 | 34 357 |
| Rectangles (filled) | 77 071 | 176 725 |
| Circles (filled) | 60 266 | 51 517 |
| Circles (outline) | 65 880 | 50 297 |
| Triangles (outline) | 39 623 | 39 602 |
| Triangles (filled) | 1 935 747 | 67 570 |
| Rounded rects (outline) | 49 920 | 37 098 |
| Rounded rects (filled) | 637 672 | 189 094 |
| Total | 8 635 804 | 1 233 483 |

A fill of 1.2 MB takes 3.5 ms. Every drawing outside a batch ends with the
cache of the processor written back to PSRAM over the bytes it has changed,
from the first changed row to the last, whole rows: a large filled triangle
writes back hundreds of kilobytes, a single pixel a line of the cache. That
is what the filled triangles, the filled rounded squares and the pixels
cost here. A batch around many drawings writes back once, and a picture
drawn in memory is written back once as it is sent.

The logo costs something else: nothing is written back while it is drawn. The
panel is 1024x600 and the test runs it upright, so neighbours in a row of the
screen lie 2 KB apart in the frame buffer, each on a cache line of its own,
and `write_area()` moves them a pixel at a time: 1.4 us a pixel all told. A
picture sent whole from memory goes in the order of the panel instead.

## Where DGX is still slower

DGX gave up speed in small figures on purpose: every drawing goes through
the functions of a screen and a bus, so that one code serves every kind of
display. This is what that costs.

A small figure is many windows. The pixel test shows what one costs: a
pixel set alone is a window and two bytes, 8.5 us with DGX on the CYD,
2.9 us with TFT_eSPI and 3.0 us with LovyanGFX. The same four transfers by
the registers, with nothing around them, took 4.7 us. The rest:

- **The transfers** take 1.2 us each against about 0.7 us: the `spi_ll_*`
  functions write fields of the registers, each a read of the register as
  well, where TFT_eSPI writes whole registers of the ESP32.
- **The functions of a screen and a bus** that a window goes through:
  `set_area()` of the screen, the commands of the bus, the window remembered
  so that bounds that did not change are not sent again. The same code
  draws on a virtual screen, a QSPI display or a DSI display; TFT_eSPI is
  written for its bus alone.
- **A pixel set alone** waits for the pixel before it to be sent, so that
  its two bytes can be written over.
