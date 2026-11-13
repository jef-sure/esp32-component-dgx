# DGX Alpha Demo

Three scenes, shown one after another again and again, each for six
seconds:

- **A crossfade.** Two pictures fade into each other through
  `DGX_VSCREEN_ALPHA(a)`. A DSI display reads its frame buffer, so the
  second picture is mixed straight over the first there. A display behind
  SPI cannot be read: the two pictures are mixed in a 16-bit virtual screen,
  which then goes to the display. After that the first picture fades to
  black and back straight on the SPI display: black is what lies under a
  picture on a display that cannot be read, so nothing has to be read.
  Last the two fade into each other again with no 16-bit screen: the
  second picture, drawn as large as the display, is the overlay layer at
  the alpha of the frame, and the first is put under it and mixed with the
  layer on the way.
- **A soft shape put many times.** A disc with a soft edge and the word
  "DGX" in a font of 4 bits a pixel are drawn once into an 8-bit virtual
  screen of amounts of ink. `DGX_VSCREEN_MASK` puts that shape in one color
  at twelve places over a picture: turned in all eight ways, some fainter
  through `DGX_VSCREEN_ALPHA`, and on a DSI display some twice as large.
  Behind SPI they go into the 16-bit virtual screen again, and it goes to
  the display.
- **A translucent palette.** An overlay layer of three buttons lies over a
  picture that scrolls. The face of a button is solid and its shadow faint,
  set by `dgx_overlay_set_alphas()`; the alpha of the layer fades all of
  them in and out together.

The band at the top shows the build number, the scene and, at its end, the
mean time of a frame. The log has the same line:

```
I (...) dgx_alpha_demo: 1/3 crossfade: ... ms a frame, mixed in a virtual screen and sent whole
I (...) dgx_alpha_demo: 1/3 fade to black: ... ms a frame, straight on the display
I (...) dgx_alpha_demo: 1/3 crossfade by a layer: ... ms a frame, picture B the overlay layer
I (...) dgx_alpha_demo: 2/3 soft shapes: ... ms a frame of 12, ... shapes put
I (...) dgx_alpha_demo: 3/3 overlay: ... ms a frame
```

The pictures are 8-bit virtual screens shown through a table of colors,
twice as large on the CYD and four times as large on a DSI display.

## Boards

- **CYD** (ESP32-2432S028R), ILI9341 320x240 on SPI at 40 MHz: pictures of
  160x112.
- **Guition JC1060P470**, 7-inch JD9165 1024x600 on DSI: pictures of
  256x146.
- **Guition JC8012P4A1C**, 10.1-inch JD9365 800x1280 on DSI, turned to
  1280x800: pictures of 320x196.

Table 1. The mean time of a frame, ms. The PSRAM runs at 200 MHz on the
7-inch board and at 250 MHz on the 10.1-inch one. With two frame buffers a
frame on a DSI board waits for the display to take the one before; a frame
of the display there is 16.4 and 16.7 ms.

| Scene | CYD | JC1060P470 | JC8012P4A1C |
|---|---|---|---|
| crossfade | 34.1 | 31.7 | 48.6 |
| fade to black | 30.0 | — | — |
| crossfade by the overlay layer | 31.8 | — | — |
| 12 soft shapes over a picture | 50.0 | 15.9 | 32.9 |
| overlay over a scrolling picture | 30.2 | 15.9 | 23.3 |

On the CYD sending a picture takes 30 ms, as long as the bus takes to send
320x224 pixels; the rest is mixing. A crossfade frame there is two pictures
mixed in memory and the whole one sent; by the overlay layer, the picture
sent and the layer mixed into it as it goes. A frame of shapes adds the
twelve shapes. On the 10.1-inch board a crossfade frame is the first
picture put and the second mixed over it in the frame buffer, 1280x784
pixels each, by the PPA.

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

`sdkconfig.defaults.jc8012p4a1c` sets the PSRAM to 250 MHz, an
experimental setting of ESP-IDF, as the other examples for this board do. A
program that would rather not use it sets `CONFIG_SPIRAM_SPEED_200M`: the
demo has two frame buffers, which keep the display from flashing at 200 MHz,
and there the overlay scene takes 27.6 ms a frame instead of 23.3. This load
once stopped the demo now and then with "Illegal instruction", at 200 MHz as
well: a line of the instruction cache came as zeros. DGX keeps the code it
hit in the cache, and the demo has run twenty minutes at 250 MHz without a
stop. See "Illegal instruction on the 10.1-inch board" in
[docs/dsi-display-en.md](../../docs/dsi-display-en.md).

## The word in the shape

`main/CMakeLists.txt` asks for the font of the word with
`dgx_font_need()`, and its letters are given as `"GXD"`, not `"DGX"`:
`DGX` alone is a keyword of `dgx_font_need()`, and CMake takes the
argument for it.
