# DGX Pixel Scale Demo

A small 8-bit picture shown two, three and four times as large through a
table of colors, every way in turn:

- **squares**: every pixel a square, `DGX_VSCREEN_SCALE(n)`;
- **Scale2x / Scale3x**: `DGX_VSCREEN_SCALE(n) | DGX_VSCREEN_SMOOTH`, Scale2x
  at 2, Scale3x at 3 and Scale2x twice at 4;
- **Scale2xSFX / Scale3xSFX**: `DGX_VSCREEN_SCALE(n) | DGX_VSCREEN_SMOOTH_SFX`.

The picture has what tells the ways apart: a fan of thin lines at every
slope, rings, steps of one and of two pixels, a solid triangle, a
checkerboard that no way of smoothing changes, and a word. At 2 the picture
fills the display; at 3 and 4 its top left part is shown.

The band at the top names the way and the time one call of
`dgx_vscreen8_region_to_screen16()` takes, the mean of eight and the
longest, with the build number. The log has the same line for every way and
a summary of all nine after each round:

```
I (...) dgx_pixel_scale_demo: build 347, ms a call: squares, plain, SFX: x2 ..., x3 ..., x4 ...
```

On a display behind SPI a call takes as long as the bus takes to send the
picture, and the processor smooths the next part of it while the part
before goes, so smoothing costs only what does not fit into that time. On a
DSI display the processor makes the rows of the panel and the 2D DMA moves
them, so the time of smoothing is the time of the call.

Every way is timed with a pause of 60 ms before each call: with two frame
buffers a call right after another waits for the frame to be shown.

On a display that can be read back, a DSI one, the demo checks itself
first: every way at 1 to 4, in all eight turns, with and without holes,
8-bit through the table and 16-bit as it is, goes to the display and into a
virtual screen, and the two are compared. The band and the log tell how
many pixels differ; 0 is right.

## Boards

- **CYD** (ESP32-2432S028R), ILI9341 320x240 on SPI at 40 MHz: a picture of
  160x112.
- **Guition JC1060P470**, 7-inch JD9165 1024x600 on DSI: a picture of 512x292.
- **Guition JC8012P4A1C**, 10.1-inch JD9365 800x1280 on DSI, turned to
  1280x800: a picture of 640x392; at 4 the part of 320x196 fills the
  display, as the 320x200 frame of a game does.

Build for a CYD:

```
idf.py -D IDF_TARGET=esp32 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" build flash monitor
```

For the 7-inch board:

```
idf.py -D IDF_TARGET=esp32p4 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4" build flash monitor
```

For the 10.1-inch board, whose chip is of revision 3:

```
idf.py -B build_jc8012p4a1c -D SDKCONFIG=build_jc8012p4a1c/sdkconfig -D IDF_TARGET=esp32p4 \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.jc8012p4a1c" build flash monitor
```

If the picture on the 10.1-inch board is mirrored, change
`EXAMPLE_LCD_DIR_X` in `main/main.c`.
