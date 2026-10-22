# DGX MIPI-DSI Demo

This example drives a MIPI-DSI display on an ESP32-P4, on one of two boards:

- Guition JC1060P470, a 7-inch 1024x600 display with a JD9165 controller,
  the revision that has the reset of the display on GPIO 0. This is the
  default;
- Guition JC8012P4A1C, a 10.1-inch 800x1280 display with a JD9365
  controller, the newer of its two panels; see the end of this file.

It opens the DGX DSI bus, creates the screen and cycles through a
test card, ramps of colors, lines, circles, triangles and color bars. The
color is 16-bit; `CONFIG_EXAMPLE_LCD_COLOR_BITS=24` (menu "DGX DSI demo")
makes it 24-bit, and the ramps show the difference: steps in 16-bit color,
none in 24-bit. The
test card names two corners of the screen and the three colors: if "top
left" is somewhere else, change `EXAMPLE_LCD_DIR_X`, `EXAMPLE_LCD_DIR_Y` and
`EXAMPLE_LCD_SWAP_XY` at the top of `main/main.c`; if the colors are named
wrong, pass `DgxScreenBGR` to the constructor. The number of the build is
shown in the lower left corner of every scene.

The last scene is bars moving sideways in a band of the screen. It is there to
show tearing: the display is sent the frame buffer all the time, also while
the band is being written into it, and now and then a bar is seen broken.
`CONFIG_EXAMPLE_LCD_TWO_FRAMES` gives the display a second frame buffer
(`DgxDsiTwoFrames` in the last argument of `dgx_jd9165_init()`): a batch of
drawing is then shown whole at a frame boundary and nothing tears, for one
more frame of PSRAM, 1.2 MB at 16 bits.

## Tuning the bus to the board

From what size on a piece goes past the processor, and how much room the bus
leaves before the scan of the display, are numbers that depend on the chip,
its memory and the display. The bus starts with the numbers of the board DGX
was written on. The example shows the whole cycle, `demo_tune()` in
`main/main.c`:

1. At its first start it measures the board by `dgx_dsi_bench()`, about
   0.3 s, in memory of its own: nothing shows on the screen. It then sends a
   60x100 piece of a virtual screen to the display with the built-in numbers
   and with the measured ones, and logs both times: at 16 bits the piece is
   12 KB, which the built-in numbers send through the window and the
   measured ones by the 2D DMA.
2. The numbers go to the bus by `dgx_dsi_tune()` and into NVS, namespace
   `dgx`, key `dsi_tuning`. At the next starts they come from there; numbers
   of another panel, color depth or PSRAM clock are not taken, and the board
   is measured again.
3. `dgx_dsi_advise()` reads the numbers and says whether the display needs
   two frame buffers: whether it ran short of data often enough under the 2D
   DMA that one frame buffer will flash under heavy drawing. The advice
   needs no display, so a start that finds the numbers in NVS makes the
   display by it: two frame buffers when it says so, unless the menu asks for
   two anyway. At the first start, when the display was made before the
   numbers were there and the advice asks for two frame buffers it does not
   have, the example restarts once. The rest of what the numbers say, the
   PSRAM clock and the chip revision, the margin, how much of the lanes the
   pixels take and whether that is too much for the color depth, is for the
   developer: `dgx_dsi_tuning_text()` puts it in words, and the example logs
   them.

The second scene, "tuning", shows all of this on the screen: the built-in
numbers beside the measured ones, where they came from, the advice, what the
display was made with and why, and the two times of the piece.

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

The text was measured in Verdana 32, which DGX no longer ships; the demo now
draws DejaVu Sans 32, a font of nearly the same height. Not measured again.

A pixel set inside a batch takes 0.45 us, a pixel set alone 4.7 us, a whole
virtual screen sent to the display 34 ms.

What is cheap on such a display and what is not: a row is one piece of the
memory the display is sent from, a column is a pixel in every row it
crosses, and each of those is a miss of the cache, about 1.4 us. So fill by
rows where there is a choice, as the ramps of the demo do with a virtual
screen one row high.

## The 10.1-inch board

The Guition JC8012P4A1C has a chip of revision 3, which a build for
revision 1 does not start on, so it has a file of settings of its own,
read after `sdkconfig.defaults`:

```sh
idf.py -B build_jc8012p4a1c -D SDKCONFIG=build_jc8012p4a1c/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.jc8012p4a1c" \
       flash monitor
```

What the board gives:

- DSI: 2 data lanes at 1000 Mbit/s, the PHY powered by regulator channel 3
  of the chip at 2500 mV; 80 MHz pixel clock, 60 frames a second
- `RST`: GPIO 27
- `Backlight`: GPIO 23, on at high level
- three USB-C sockets: "USB FULL SPEED" is the USB-Serial-JTAG of the chip,
  for flashing and the console; "USB_UART" goes to UART0 through a WCH USB-serial chip

`sdkconfig.defaults.jc8012p4a1c` chooses the board in the menu, the chip
revision, 400 MHz and PSRAM at 250 MHz. At 200 MHz the display, which reads
160 MB/s from PSRAM within a row and 123 MB/s averaged over a frame, now and
then ran short of data while the moving bars were drawn, and the screen flashed light blue for a frame; at 250 MHz it
did not. With two frame buffers (`CONFIG_EXAMPLE_LCD_TWO_FRAMES`) there were
no flashes at 200 MHz either: that is the way for a chip older than
revision 3, which has no 250 MHz. See "PSRAM speed" in
[docs/dsi-usage-en.md](../../docs/dsi-usage-en.md).

The panel is portrait. `EXAMPLE_LCD_SWAP_XY` makes it 1280x800. With one
frame buffer the moving bars tear more than on the 7-inch board: the 2D DMA
writes rows at 82 a millisecond and the display reads them at 79.5, so the
two run side by side for most of the band. Two frame buffers, 2 MB more of
PSRAM, show them whole.

