# DGX

`DGX` is a small, opinionated display graphics library for microcontrollers.
I wrote the first version a long time ago, back when everyone was using Adafruit GFX
or TFT_eSPI, and I wanted something that fit my own taste: a clean separation
between bus, screen and drawing, no global state, and no surprises about where
pixels actually go.

It is currently packaged as an ESP-IDF component and ships with drivers for
several common SPI/I2C/parallel panels, a few virtual-screen variants, a UTF-8
font renderer, and a handful of bundled fonts.

## What you get

- Drivers for ST7735, ST7789, ILI9341, GC9A01, SSD1351, SSD1306, ST7565R and ST7920.
- SPI, I2C and 8-bit parallel (I80) transports for ESP32.
- A RAM-backed color virtual screen and a 1-bit monochrome screen, usable on
  their own, as staging buffers for smooth animation, or as shadow buffers
  behind hardware drivers.
- A two-head compositor that exposes two child screens as one logical screen.
- Drawing primitives (pixels, lines, rectangles, circles, filled quads) and an
  arc gauge helper.
- UTF-8 text rendering with 8-way orientation, glyph lookup, layout/bounds
  queries and a small "morph" helper for animating between glyphs.
- A `font2c` tool that turns TTF/BDF-style sources into compilable C font
  modules, plus a set of pre-converted fonts in `src/fonts/`.

## Design in one paragraph

Everything above the transport layer talks to a `dgx_screen_t` vtable.
Bus backends produce a `dgx_bus_protocols_t`. Driver constructors return
either a plain `dgx_screen_t *` (for RAM-only screens) or a
`dgx_screen_with_bus_t *` (for hardware panels). `draw.c` and `font.c` never
look at SPI, I2C or P8 directly — they only call through the screen vtable.
Monochrome panels keep a RAM page buffer and flush it to the controller; the
ST7920 driver reuses the color virtual screen the same way.

```text
application
  └─ dgx_draw.h / dgx_font.h / dgx_gauge.h
        └─ dgx_screen_t  (vtable)
              ├─ dgx_screen_with_bus_t  → bus backend (SPI / I2C / P8)
              ├─ dgx_bw_vscreen_t       → 1-bit RAM buffer
              ├─ dgx_vscreen_t          → color RAM buffer
              └─ vscreen_2h             → composes two child screens
```

## Using it in an ESP-IDF project

Add the component (for example as a git submodule under `components/dgx`),
enable the bits you need in `menuconfig` under the **DGX** menu, and link it.

For an ST7789 panel over SPI, a minimal setup looks like this:

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

// 3. draw
dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, DGX_BLACK(dgx_rgb_to_16));
dgx_font_string_utf8_screen(scr, 10, 18, "Hello world",
              DGX_WHITE(dgx_rgb_to_16),
              DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
```

A complete working example lives in [examples/screen_demo](examples/screen_demo).

The color helpers in `dgx_colors.h` are macros, not constants. You can pair
them with either the uppercase packing macros from `dgx_bits.h` or the inline
`dgx_rgb_to_*()` wrappers, so the same color name can be reused across
different pixel formats:

```c
DGX_LIGHTGREY(DGX_RGB_16)   // pure macro expansion, RGB565
DGX_RED(dgx_rgb_to_16)      // inline wrapper, also RGB565
DGX_RED(DGX_RGB_24)         // 24-bit RGB
DGX_WHITE(DGX_RGB_12)       // 12-bit packed color
```

For most ESP32 TFT panels in this repository, `DGX_RGB_16` is the normal
choice.

## Building

This repository is a component, not a standalone firmware image. In practice
you either drop it into your own ESP-IDF project under `components/`, or you
build the bundled demo in `examples/screen_demo`.

To build the demo:

```sh
cd examples/screen_demo
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py flash monitor
```

If you use DGX from your own application, build that application the normal
ESP-IDF way. Once the component is in your component search path, ESP-IDF will
pick it up automatically.

## Why virtual screens matter

Virtual screens are one of the most useful parts of DGX. They let you render
into RAM first and decide later when and where to push the result.

They are useful when:

- you want smooth animation without drawing directly to the panel line by line;
- you want to compose widgets, sprites, or text off-screen and then blit them
  into place;
- you need a shadow buffer for monochrome controllers such as SSD1306 or
  ST7565R;
- you want to copy full screens or partial regions between surfaces;
- you want to render indexed 8-bit assets and expand them through a LUT to a
  16-bit destination.

The public API in `include/drivers/vscreen.h` covers cloning, whole-screen
copy, partial-region copy, oriented blits, and screen-to-screen transfer. The
same idea is used internally by several drivers as well: draw into RAM first,
flush to hardware second.

## Using `font2c`

You only need `font2c` when the bundled fonts are not enough. It is a small
offline converter that turns a font file into a DGX source/header pair.

Build it on a machine with FreeType development files installed:

```sh
cc font2c/font2c.c -o font2c/font2c $(pkg-config --cflags --libs freetype2)
```

Run it with a font file, pixel size, and one or more inclusive Unicode ranges:

```sh
./font2c path/to/YourFont.ttf 16 0x20 0x7e 0x410 0x44f
./font2c <font file> <size> <start range> <end range> [<start range> <end range>]*
```

That command writes a `.c` file and a matching `.h` file into the current
directory. Their names are derived from the font family, style, and size.

To use the generated font:

1. Move the `.c` file into `src/fonts/` and the `.h` file into `include/fonts/`.
2. Rerun CMake configure once if this is a brand new font file, because fonts
   are added through `file(GLOB ...)`.
3. Include the generated header and pass the accessor to the text API.

```c
#include "fonts/YourFont16.h"

dgx_font_string_utf8_screen(scr, 10, 10, "Hello world",
                            DGX_WHITE(DGX_RGB_16),
                            DgxOutputNormal, 1, YourFont16(), NULL, NULL);
```

## Build options

Everything is wired through Kconfig. Toggle only what you need; drivers pull in
the transports they require.

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

The `drivers/`, `dgx_lcd_init.c`, drawing/font code and every file in
`src/fonts/` are always compiled. The fonts are pulled in via a `file(GLOB)`
on `src/fonts/*.c`, so the linker keeps only the font objects your application
actually references. If you add or remove a font file, rerun CMake configure
once.

## Coordinate system and orientation

Virtual screens use a single canonical layout:

- origin in the top-left, `x` going right, `y` going down;
- pixels stored row-major at offset `x + y * width`;
- `set_area`, `write_area` and `read_area` always use those canonical bounds
  and traverse left-to-right, top-to-bottom.

Hardware panel drivers expose their own orientation setters
(`dgx_st7789_orientation()`, `dgx_gc9a01_orientation()` and friends) that
reprogram the controller's scan direction. Text rendering takes an explicit
`dgx_output_orientation_t`, so you can draw rotated/mirrored text on any
screen without changing the framebuffer layout.

The `dir_x`, `dir_y` and `swap_xy` fields on the screen struct are *metadata*
on virtual screens today — they describe the screen but do not transform
framebuffer access. In practice, treat virtual screens as always stored in
their canonical layout.

## Status and known gaps

A couple of practical limitations are worth knowing up front:

- **Pixel readback is dependable on virtual screens.** They keep their full
  framebuffer in RAM, so `get_pixel()` behaves as expected there. On physical
  panels, hardware readback is still incomplete and should not be treated as a
  general DGX feature.
- **Virtual screens do not honor `dir_x`/`dir_y`/`swap_xy` for framebuffer
  access.** Those fields describe orientation metadata, but they do not rotate
  or mirror the stored pixel data.

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

## License

See [LICENSE](LICENSE).
