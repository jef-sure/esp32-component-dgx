# DGX MIPI-DSI Demo

This example drives a 7-inch 1024x600 display with a JD9165 controller over
MIPI-DSI on an ESP32-P4. It was made on a Guition JC1060P470 board, the
revision that has the reset of the display on GPIO 0.

It opens the DGX DSI bus, creates a `JD9165` screen and cycles through a
test card, ramps of colors, lines, circles, triangles and color bars. The
color is 16-bit; `CONFIG_EXAMPLE_LCD_COLOR_BITS=24` (menu "DGX DSI demo")
makes it 24-bit, and the ramps show the difference: steps in 16-bit color,
none in 24-bit. The
test card names two corners of the screen and the three colors: if "top
left" is somewhere else, change `EXAMPLE_LCD_DIR_X`, `EXAMPLE_LCD_DIR_Y` and
`EXAMPLE_LCD_SWAP_XY` at the top of `main/main.c`; if the colors are named
wrong, pass `DgxScreenBGR` to `dgx_jd9165_init()`.

The last scene is bars moving sideways in a band of the screen. It is there to
show tearing: the display is sent the frame buffer all the time, also while
the band is being written into it, and now and then a bar is seen broken.
`CONFIG_EXAMPLE_LCD_TWO_FRAMES` gives the display a second frame buffer
(`DgxDsiTwoFrames` in the last argument of `dgx_jd9165_init()`): a batch of
drawing is then shown whole at a frame boundary and nothing tears, for one
more frame of PSRAM, 1.2 MB at 16 bits.

At its first start the example measures the board by `dgx_dsi_bench()`: from
what size on a piece goes past the processor and how much room the bus
leaves before the scan of the display are numbers that depend on the chip,
its memory and the display. Nothing shows on the screen meanwhile. The
numbers are kept in NVS, namespace `dgx`, key `dsi_tuning`, and at the next
starts the bus gets them from there. With another panel or another color
depth the numbers kept are not taken, and the board is measured again.

What the board gives:

- DSI: 2 data lanes at 750 Mbit/s, the PHY powered by regulator channel 3 of
  the chip at 2500 mV
- `RST`: GPIO 0
- `Backlight`: GPIO 23, on at high level

Build and flash it from this example directory:

```sh
idf.py set-target esp32p4
idf.py flash monitor
```

`sdkconfig.defaults` has what such a display needs:

- PSRAM at 200 MHz: the frame buffer the display is sent from lives there,
  1.2 MB for 16-bit color;
- `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, because the board this was made on
  has a chip of revision 1.3. Remove it for a chip of revision 3.0 or later;
- a 256 KB cache for the PSRAM. Drawing goes through that cache, and its size
  is a trade against internal RAM. Measured on this board in 16-bit color,
  ESP32-P4 at 360 MHz:

| L2 cache | Internal RAM left free | Text, 25 symbols of Verdana 32 | Solid circle, radius 100 | Rectangle 100x100 | Whole screen filled |
| --- | --- | --- | --- | --- | --- |
| 128 KB (the default) | 588 KB | 1.0 ms | 1.4 ms | 0.40 ms | 18 ms |
| 256 KB | 458 KB | 1.0 ms | 1.2 ms | 0.18 ms | 16 ms |
| 512 KB | 197 KB | 0.9 ms | 0.7 ms | 0.19 ms | 9.5 ms |

A pixel set inside a batch takes 0.45 us, a pixel set alone 4.7 us, a whole
virtual screen sent to the display 34 ms.

What is cheap on such a display and what is not: a row is one piece of the
memory the display is sent from, a column is a pixel in every row it
crosses, and each of those is a miss of the cache, about 1.4 us. So fill by
rows where there is a choice, as the ramps of the demo do with a virtual
screen one row high.
