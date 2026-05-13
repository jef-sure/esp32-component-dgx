# DGX

DGX is a small display graphics library for microcontrollers. I started writing
it years ago, back when most people reached for Adafruit GFX or TFT_eSPI, and
I wanted something shaped to my own taste: a clean split between bus, screen
and drawing code, no hidden global state, possibility to use several equal
screens simulteneously.

Today it lives as an ESP-IDF component with drivers for a handful of common
panels, a couple of RAM-backed virtual screens, a UTF-8 text renderer, and a
small offline tool for converting fonts.

## What's in the box

- Panel drivers for ST7735, ST7789, ILI9341, GC9A01, SSD1351, SSD1306, ST7565R
  and ST7920.
- SPI, I2C and 8-bit parallel (I80) transports for ESP32.
- A color RAM-backed virtual screen and a 1-bit monochrome virtual screen.
  They are useful on their own, as staging buffers for animation, or as shadow
  buffers behind monochrome controllers.
- A two-head compositor (`vscreen_2h`) that exposes two child screens as one
  logical screen.
- Drawing primitives — pixels, lines, rectangles, circles, filled quads — and
  an arc gauge helper.
- UTF-8 text rendering with 8-way orientation, glyph lookup, layout and bounds
  queries, plus a small "morph" helper for animating between glyphs.
- `font2c`, an offline tool that turns TTF/BDF-style fonts into C source/header
  pairs, and a set of pre-converted fonts in `src/fonts/`.

## How it fits together

Everything above the transport layer talks to a `dgx_screen_t` vtable. Bus
backends produce a `dgx_bus_protocols_t`. Driver constructors return either a
plain `dgx_screen_t *` for RAM-only screens, or a `dgx_screen_with_bus_t *`
for hardware panels. The drawing and font code never touches SPI, I2C or P8
directly — it only goes through the screen vtable. Monochrome panels keep a
RAM page buffer and flush it to the controller, and the ST7920 driver reuses
the color virtual screen the same way.

```text
application
  └─ dgx_draw.h / dgx_font.h / dgx_gauge.h
        └─ dgx_screen_t  (vtable)
              ├─ dgx_screen_with_bus_t  → bus backend (SPI / I2C / P8)
              ├─ dgx_bw_vscreen_t       → 1-bit RAM buffer
              ├─ dgx_vscreen_t          → color RAM buffer
              └─ vscreen_2h             → composes two child screens
```

## Getting started

DGX is a component, not a standalone firmware image. There are two normal ways
to use it:

1. Drop it into your own ESP-IDF project under `components/` (a git submodule
   at `components/dgx` works well).
2. Build the bundled demo in `examples/screen_demo` to see it run end-to-end.

Either way, enable only the pieces you need in `menuconfig`, under the **DGX**
menu. Drivers automatically pull in the transports they require.

To build and flash the demo:

```sh
cd examples/screen_demo
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py flash monitor
```

In your own application, just build the project the normal ESP-IDF way. Once
DGX is on the component search path, ESP-IDF will pick it up automatically.

## A minimal example

Here's a complete setup for an ST7789 panel over SPI:

```c
#include "bus/dgx_spi_esp32.h"
#include "drivers/st7789.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/ArialRegular12.h"

// 1. create a bus
dgx_bus_protocols_t *bus =
  dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO,
               GPIO_NUM_23, GPIO_NUM_19,
               GPIO_NUM_18, GPIO_NUM_5,
               GPIO_NUM_27, 40 * 1000 * 1000, 0);

// 2. create a screen on top of that bus
dgx_screen_t *scr = dgx_st7789_init(bus, GPIO_NUM_33, 16, DgxScreenRGB);

// 3. draw something
dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height,
                   DGX_BLACK(dgx_rgb_to_16));
dgx_font_string_utf8_screen(scr, 10, 18, "Hello world",
                            DGX_WHITE(dgx_rgb_to_16),
                            DgxOutputNormal, 1, ArialRegular12(),
                            NULL, NULL);
```

A working version of this lives in
[examples/screen_demo](examples/screen_demo).

## Colors

The helpers in `dgx_colors.h` are macros, not constants. You can pair them
with either the uppercase packing macros from `dgx_bits.h` or the inline
`dgx_rgb_to_*()` wrappers, so the same color name works across pixel formats:

```c
DGX_LIGHTGREY(DGX_RGB_16)   // pure macro expansion, RGB565
DGX_RED(dgx_rgb_to_16)      // inline wrapper, also RGB565
DGX_RED(DGX_RGB_24)         // 24-bit RGB
DGX_WHITE(DGX_RGB_12)       // 12-bit packed color
```

For most ESP32 TFT panels in this repository, `DGX_RGB_16` is the right
choice.

## Virtual screens

Virtual screens are one of the most useful parts of DGX. They let you draw
into RAM first and decide later when and where to push the result.

They earn their keep when:

- you want smooth animation without writing directly to the panel line by
  line;
- you want to compose widgets, sprites or text off-screen and blit them into
  place;
- you need a shadow buffer for a monochrome controller like SSD1306 or
  ST7565R;
- you want to copy full screens or partial regions between surfaces;
- you want to render indexed 8-bit assets and expand them through a LUT into
  a 16-bit destination.

The public API in `include/drivers/vscreen.h` covers cloning, whole-screen
copy, partial-region copy, oriented blits and screen-to-screen transfer. The
same pattern shows up inside several drivers: draw into RAM first, flush to
hardware second.

## Coordinates and orientation

Virtual screens use a single canonical layout:

- origin in the top-left, `x` going right, `y` going down;
- pixels stored row-major at offset `x + y * width`;
- `set_area`, `write_area` and `read_area` always use those canonical bounds
  and traverse left-to-right, top-to-bottom.

Hardware panel drivers have their own orientation setters
(`dgx_st7789_orientation()`, `dgx_gc9a01_orientation()` and so on) that
reprogram the controller's scan direction. Text rendering takes an explicit
`dgx_output_orientation_t`, which means you can draw rotated or mirrored text
on any screen without touching the framebuffer layout.

The `dir_x`, `dir_y` and `swap_xy` fields on the screen struct are currently
*metadata* on virtual screens — they describe the screen but do not transform
framebuffer access. In practice, treat virtual screens as always stored in
their canonical layout.

## Working with fonts

The headers under `include/fonts/` declare every bundled font. Pick the one
you want, include it, and pass its accessor to the text API:

```c
#include "fonts/ArialRegular12.h"

dgx_font_string_utf8_screen(scr, 10, 10, "Hello world",
                            DGX_WHITE(DGX_RGB_16),
                            DgxOutputNormal, 1, ArialRegular12(),
                            NULL, NULL);
```

When the bundled fonts are not enough, `font2c` generates new ones offline.
You need FreeType development headers installed:

```sh
cc font2c/font2c.c -o font2c/font2c $(pkg-config --cflags --libs freetype2)
```

Run it with a font file, a pixel size, and one or more inclusive Unicode
ranges:

```sh
./font2c path/to/YourFont.ttf 16 0x20 0x7e 0x410 0x44f
./font2c <font file> <size> <start range> <end range> [<start range> <end range>]*
```

It writes a `.c` file and a matching `.h` file into the current directory,
with names derived from the font family, style and size. To use the generated
font:

1. Move the `.c` file into `src/fonts/` and the `.h` file into
   `include/fonts/`.
2. Rerun CMake configure once if this is a brand new file — fonts are added
   through `file(GLOB ...)`, so the build system needs to notice it.
3. Include the generated header and pass its accessor to the text API, just
   like a bundled font.

## Build options

Everything is wired through Kconfig. Toggle only what you need; drivers pull
in the transports they require.

| Option | Adds | Notes |
| --- | --- | --- |
| `CONFIG_DGX_ENABLE_SPI` | `bus/spi_esp32.c` | SPI transport |
| `CONFIG_DGX_ENABLE_I2C` | `bus/i2c_esp32.c` | I2C transport |
| `CONFIG_DGX_ENABLE_P8` | `bus/p8_esp32.c` | 8-bit parallel (I80) transport |
| `CONFIG_DGX_ENABLE_SPI_ST7735` | `drivers/st7735.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SPI_ST7789` | `drivers/st7789.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SPI_GC9A01` | `drivers/gc9a01.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SPI_ILI_9341` | `drivers/ili9341.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SSD1351` | `drivers/ssd1351.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SSD1306` | `drivers/ssd1306.c` | needs SPI or I2C; selects `V_BW_SCREEN` |
| `CONFIG_DGX_ENABLE_ST7565R` | `drivers/st7565r.c` | needs SPI; selects `V_BW_SCREEN` |
| `CONFIG_DGX_ENABLE_ST7920` | `drivers/st7920.c` | needs SPI/I2C/P8; selects `VSCREEN` |
| `CONFIG_DGX_ENABLE_V_BW_SCREEN` | `bw_screen.c` | 1-bit virtual screen |
| `CONFIG_DGX_ENABLE_VSCREEN` | `drivers/vscreen.c` | color RAM-backed screen |
| `CONFIG_DGX_ENABLE_VSCREEN_2H` | `drivers/vscreen_2h.c` | needs `VSCREEN` |

`dgx_lcd_init.c`, the drawing and font code, and everything in `src/fonts/`
are always compiled. Fonts are picked up via a `file(GLOB)` on
`src/fonts/*.c`, so the linker keeps only the font objects your application
actually references. If you add or remove a font file, rerun CMake configure
once.

## Driver-specific extras

Some drivers expose a few extra helpers beyond the common screen API.

**GC9A01.** Two small runtime helpers let you blank and re-enable the panel
without rebuilding the screen object:

```c
#include "drivers/gc9a01.h"

dgx_screen_t *scr = dgx_gc9a01_init(bus, GPIO_NUM_33, 16, DgxScreenRGB);

dgx_gc9a01_display_off(scr);
dgx_gc9a01_display_on(scr);
```

They send the controller's `DISPOFF` and `DISPON` commands. They do not put
the panel into sleep mode or reinitialize driver state.

## Known gaps

A couple of practical limitations are worth knowing up front:

- **Pixel readback is reliable on virtual screens.** They keep the full
  framebuffer in RAM, so `get_pixel()` behaves as expected there. On physical
  panels, hardware readback is still incomplete and should not be relied on.
- **Virtual screens don't honor `dir_x`/`dir_y`/`swap_xy` for framebuffer
  access.** Those fields describe orientation metadata, but they do not
  rotate or mirror the stored pixel data.

## Repository layout

```
include/                 public headers
  bus/                   transport interfaces (SPI, I2C, P8)
  drivers/               panel drivers + virtual screens
  fonts/                 generated font headers
src/                     implementations matching include/
  bus/                   ESP32 bus backends
  drivers/               panel + virtual screen sources
  fonts/                 generated font sources (glob-built)
font2c/                  offline TTF/BDF -> C font generator
examples/screen_demo/    minimal end-to-end example
Kconfig                  feature toggles
CMakeLists.txt           ESP-IDF component build
```

See [CHANGES.md](CHANGES.md) for the release history.

## License

See [LICENSE](LICENSE).
