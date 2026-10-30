# DGX Graphics Test

The PDQ graphics test that Adafruit_GFX, TFT_eSPI and LovyanGFX ship
(`TFT_graphicstest_PDQ`), made with DGX: the same figures, sizes, colors and
order, timed the same way, so that its numbers can be put beside theirs.
[docs/benchmarks-en.md](../../docs/benchmarks-en.md) has them for every
board below and beside the other libraries on a CYD.

A round draws, one test after another:

- the Hackaday logo 16 times, in grey from black to white;
- the screen filled white, red, green, blue and black;
- the text of the test in `GLCD5x7`, the 5x7 font of Adafruit_GFX;
- every pixel of the screen, each of its own color;
- lines from each corner, a grid of horizontal and vertical lines;
- squares, outlined and filled; circles of radius 10, filled and outlined;
- triangles and squares with round corners, outlined and filled.

A round runs twice. First on the display, as the sketches of the other
libraries draw: every call goes to the panel. Then in memory, as a program
of DGX draws to show no flicker: each test draws its picture into an 8-bit
virtual screen of the size of the display, its time taken as before, and
one transfer of the whole of it to the display, through a table of 256
colors (RGB332), is added to that time. Where there is no memory for the
virtual screen the second run is left out, and the log says so.

Then the screen shows both tables for 10 seconds, with the build number, and
the next round begins. The log has them as the sketches print them:

```
I (...) graphicstest: Benchmark                Time (microseconds), on the display
I (...) graphicstest: HaD pushColor                533959
...
I (...) graphicstest: Total                       2881373
I (...) graphicstest: Benchmark                Time (microseconds), in memory and sent
I (...) graphicstest: HaD pushColor                419212
...
I (...) graphicstest: Total                        964741
```

A time ends when the display has the last pixel: the bus is waited for
before the clock is read. Where DGX has no single call for a figure, the
test makes it of the calls a program would use: a horizontal or vertical
line is `dgx_fill_rectangle()` a pixel high or wide, the outline of a
triangle is three `dgx_draw_line()`, and the logo goes by `set_area()` and
`write_area()` of the screen in two halves of 4 KB, one filled while the
other is sent.

## Boards

- **CYD** (ESP32-2432S028R), ILI9341 on SPI at 40 MHz, upright at 240x320.
- **Guition JC4827W543** (ESP32-S3), NV3041A on QSPI at 40 MHz, upright at
  272x480.
- **Guition JC1060P470** (ESP32-P4), 7-inch JD9165 on MIPI-DSI with one
  frame buffer, upright at 600x1024. A figure is in the memory the panel
  shows when its call returns, as in the memory of an SPI panel; with two
  frame buffers every test would wait for the next frame.

The settings of each board are in `sdkconfig.defaults.<target>`, which
ESP-IDF takes after `sdkconfig.defaults`:

```
idf.py set-target esp32      # a CYD
idf.py set-target esp32s3    # a JC4827W543
idf.py set-target esp32p4    # a JC1060P470
idf.py build flash monitor
```

The ESP32 and the ESP32-S3 run at 240 MHz, as the sketches of the other
libraries do; the ESP32-P4 at its 360 MHz. The JC1060P470 this was made on
has a chip of revision 1.3: remove `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` from
`sdkconfig.defaults.esp32p4` for one of revision 3.
