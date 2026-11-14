# How DGX is built and tested

How the parts of DGX fit together, how its tests run on a computer, and
where things lie in the repository.

## How it fits together

Everything above the transport layer talks to a `dgx_screen_t` vtable. Bus
backends produce a `dgx_bus_protocols_t`. Driver constructors return a
`dgx_screen_t *`, which on a hardware panel is a `dgx_screen_with_bus_t`. The
drawing and font code never touches SPI, QSPI, I2C or P8 directly — it only goes through the screen vtable. Monochrome panels keep a
RAM page buffer and flush it to the controller, and the ST7920 driver reuses
the color virtual screen the same way. A MIPI-DSI display is sent a frame
buffer all the time; its bus takes the window and the pixels a driver sends,
as a display with memory of its own does, and puts them into that buffer.

```text
application
  └─ dgx_draw.h / dgx_font.h / dgx_hw_font.h / dgx_gauge.h / dgx_morph.h
        └─ dgx_screen_t  (vtable)
              ├─ dgx_screen_with_bus_t  → bus backend (SPI / QSPI / I2C / P8 / DSI)
              ├─ dgx_bw_vscreen_t       → 1-bit RAM buffer
              ├─ dgx_vscreen_t          → color RAM buffer
              └─ vscreen_2h             → composes two child screens
```

The functions a display behind a bus runs for every window and every short
transfer, and the basic shapes made of such windows, carry `DGX_HOT`
(`src/dgx_hot_priv.h`). The linker fragment that `CMakeLists.txt` writes
gathers them in one piece of flash, so that on an ESP32 they cannot push each
other out of its flash cache: see
[Where the code lies in flash](benchmarks-en.md#where-the-code-lies-in-flash).
A new function on that way gets the mark. The piece has to stay shorter than
16 KB, a way of that cache; the link map of a build shows it as the
`.dgx_hot` sections.

## How calls are named and shaped

The rules a new call follows. Calls written before some of them differ and
keep working; where a call got a new name, the earlier one stays, see
[Earlier names](api-en.md#earlier-names).

- **Names.** `dgx_<thing>_<verb>()`. A filled figure is `dgx_fill_*()`, an
  outline or a line `dgx_draw_*()`. A call that takes its choices in a
  structure beside one that takes none ends in `_with`:
  `dgx_spi_init_with()`, `dgx_morph_create_with()`. Types are
  `typedef struct dgx_xxx_t { ... } dgx_xxx_t;`, constants `DGX_XXX`.
- **Making and freeing.** `_init()` makes a screen or a bus, freed by
  `dgx_screen_destroy(&scr)` and `dgx_bus_dispose(&bus)`. `_create()` makes
  any other object on the heap, and its `_destroy(&object)` takes the address
  of the pointer, clears it, and accepts `NULL` and a pointer to `NULL`.
  `_begin()` fills a structure the caller owns: nothing to free.
- **Results.** A constructor returns the object or `NULL`, with the reason in
  the log. An operation a screen may not support returns `esp_err_t`, with
  `ESP_ERR_NOT_SUPPORTED` for that. A question returns `bool`.
- **Coordinates.** `int` in the arguments of a call, `int16_t` in stored
  data: glyphs, points, tables. A rectangle a program gives is `x, y, width,
  height`. The functions of a screen itself, `set_area()`, `update_screen()`
  and `dgx_screen_touch()`, take bounds, `left, right, top, bottom`, both
  ends inclusive.
- **Angles.** Degrees, counter-clockwise, 90 up. The sweep of a gauge is the
  one exception: positive is clockwise, as a dial reads.
- **Looks are arguments.** How a drawing looks is said in the call: the pen
  in the width (`DGX_PEN_SOFT | width`), the way of a transfer in its flags
  (`DGX_VSCREEN_*`), a text in its context. A screen has no drawing mode.
- **Callbacks.** `user_data` is the first argument.
- **A faster way is not a new call.** A special case hardware or a shorter
  loop can serve is recognised inside the call a program already makes.

Headers are for a program or for a driver.

Table 1. Which headers a program includes and which only a driver of a new display needs.

| For | Headers |
| --- | --- |
| a program | `dgx_screen.h`, `dgx_draw.h`, `dgx_font.h`, `dgx_hw_font.h`, `dgx_vscreen.h`, `dgx_image.h`, `dgx_lut.h`, `dgx_overlay.h`, `dgx_gauge.h`, `dgx_ease.h`, `dgx_morph.h`, `dgx_morph_sources.h`, `dgx_morph_render.h`, `dgx_matrix_morph.h`, `dgx_colors.h`, `dgx_bits.h`, `dgx_bitmap.h`, `dgx_bw_screen.h`, `drivers/<panel>.h`, `drivers/dgx_panel.h`, `drivers/vscreen_2h.h`, `bus/dgx_<bus>_esp32.h`, `fonts/<font>.h` |
| a driver | `dgx_screen_with_bus.h`, `dgx_screen_functions.h`, `dgx_spi_area_protocol.h`, `drivers/dgx_lcd_init.h`, `bus/dgx_bus_protocols.h`, `dgx_arch.h`; what they declare is not kept stable between versions as the calls of a program are |

## Host tests

The platform-independent code (matrices, morphing, renderers, virtual and page
screens, fills, lines, circles and ellipses with their arcs and sectors,
scrolling in memory, the frame buffer of the DSI bus and the
way its two frame buffers change places, fonts,
textured quads, sprites of 1, 4 and 8 bits, color tables, the transfers of virtual screens with every flag, scaling
and smoothing against the shaders it comes from, Bezier curves, handwritten fonts with their writing and
morphing, the arc gauge) has host tests built with gcc against small ESP-IDF
stubs and run under ASan/UBSan. A few pieces that do depend on the platform
are tested there too, each against a model of what is on its wires: the
8-bit parallel bus with both drivers of IDF under it, the SPI and QSPI buses
with the SPI master driver of IDF under them, the ST7920 and ST7789 drivers
in all their orientations, the ILI9341 driver with the scroll of its panel,
the SSD1309, SH1106 and SSD1351 drivers against models of their
controllers, and the window on the computer against a virtual screen. What
every color driver sends, through a bus that records it, is kept in
`test/host/panel_streams.txt`, and a driver that sends a byte more, less or
in another place fails:

```sh
make -C test/host
```

A program can build the same sources on a PC, to run its own `main.c` there
with a window for the display. `host/dgx_host.mk` lists the sources and the
flags for a Makefile, and `host/stubs` stand in for the ESP-IDF headers they
include; both come with the package from the registry. The host tests are
built from that file too.

```make
include path/to/dgx/host/dgx_host.mk
CPPFLAGS += $(DGX_HOST_CPPFLAGS)
CFLAGS   += $(DGX_HOST_CFLAGS)
LDLIBS   += $(DGX_HOST_LDLIBS)
game: main.c $(DGX_HOST_SRCS)
```

`DGX_HOST_SRCS` is the sum of the groups `DGX_HOST_SRCS_SCREEN`, `_DRAW`,
`_VSCREEN`, `_TEXT`, `_MORPH`, `_FB` and `_FONTS`; `DGX_HOST_DIR` is the
directory of the file. A window of the program's own shows a virtual screen
through `dgx_vscreen_to_argb8888()`; the window of DGX is in
[An effect on the computer](../README.md#an-effect-on-the-computer).

The fonts made at build time have a test of their own, `make -C test/fonts`:
it runs the tool on the needs in that directory, checks the characters and
the pairs of face and size, compiles the generated C, and checks that a
program's mistakes are reported properly. It needs what such a program
needs, FreeType, fontconfig and the DejaVu fonts, and says so when they are
missing. With node installed it also reads and writes fonts with the code of
the font editor: a font `font2c` makes comes back from the editor byte for
byte, and the editor and `dgx_fonts.py` read the same glyphs from every
bitmap font of DGX.

What a host cannot tell, the look of a figure on a panel, the scroll of the
panel itself and the times on the chip, is seen on a board:
[examples/figures_scroll_demo](../examples/figures_scroll_demo), for a CYD or
a JC1060P470. On a display that is read it draws and scrolls the same on the
display and on a virtual screen and compares the two pixel by pixel.

## Repository layout

```
include/                 public headers
  bus/                   transport interfaces (SPI, QSPI, I2C, P8, DSI)
  drivers/               panel drivers + virtual screens
  fonts/                 generated font headers
src/                     implementations, by topic
  screen/                what every screen does: the window of a display behind a bus,
                         pixel runs, 1-bit bitmaps
  draw/                  drawing on any screen: lines, circles, rectangles, polygons, ellipses and arcs,
                         Bezier curves and the pen, textures, scrolling, the gauge
  vscreen/               virtual screens and what puts them on screens: transfers with
                         their flags, scaling and smoothing, the overlay layer, sprites,
                         tables of colors, the 1-bit page screen, two screens as one
  text/                  bitmap and dot fonts, handwritten text and its morphs
  morph/                 the dot morphing framework and its renderers
  bus/                   ESP32 bus backends and the frame buffer of a DSI display
  drivers/               panel drivers
  fonts/                 generated font sources (glob-built), each with the notice of the font it was made from;
                         licenses/ holds the texts of their licenses
font2c/                  offline TTF/BDF -> C font generator, and dgx_fonts.py, which makes a program's fonts from its needs
font-editor/             the editor of the glyphs of a font2c font, in the browser; its fixes are patches for font2c
project_include.cmake    dgx_font_need() and the other font functions for a program's CMakeLists
examples/                one project per example, see "Examples"
host/                    dgx_host.mk and ESP-IDF stubs: the platform-independent part built on a PC;
                         window/ holds the window that stands for the display of a board
test/host/               host tests
test/fonts/              the test of the fonts made at build time
docs/                    morphing articles, handwritten text guides, fonts made at build time (English and Russian),
                         the converter and the tables of handwritten fonts (Russian),
                         a guide to a display on the DSI bus and notes on how its bus is made,
                         the soft pen, the NV3041A panel, the first piece of a transfer, 18-bit
                         color and benchmarks (English and Russian)
Kconfig                  feature toggles
CMakeLists.txt           ESP-IDF component build
```

See [CHANGES.md](../CHANGES.md) for the release history.
