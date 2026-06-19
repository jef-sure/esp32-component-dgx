# DGX

DGX is a small display graphics library for microcontrollers. I started writing
it years ago, back when most people reached for Adafruit GFX or TFT_eSPI, and
I wanted something shaped to my own taste: a clean split between bus, screen
and drawing code, no hidden global state, possibility to use several equal
screens simulteneously.

Today it lives as an ESP-IDF component with drivers for a handful of common
panels, a couple of RAM-backed virtual screens, a UTF-8 text renderer, and a
small offline tool for converting fonts.

## Contents

- [What's in the box](#whats-in-the-box)
- [How it fits together](#how-it-fits-together)
- [Getting started](#getting-started)
- [A minimal example](#a-minimal-example)
- [Supported panels](#supported-panels)
- [API reference](#api-reference)
  - [Drawing primitives](#drawing-primitives)
  - [Text and fonts](#text-and-fonts)
  - [Colors](#colors)
  - [Virtual screens](#virtual-screens-api)
  - [Arc gauge](#arc-gauge)
  - [Bus backends](#bus-backends)
  - [Flush control and batching](#flush-control-and-batching)
- [Tutorials](#tutorials)
  - [Flicker-free animation with a virtual screen](#flicker-free-animation-with-a-virtual-screen)
  - [Driving a monochrome panel](#driving-a-monochrome-panel)
  - [Building an arc gauge](#building-an-arc-gauge)
  - [Generating a custom font](#generating-a-custom-font)
- [Coordinates and orientation](#coordinates-and-orientation)
- [Build options](#build-options)
- [Known gaps](#known-gaps)
- [Repository layout](#repository-layout)
- [License](#license)


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

## Supported panels

Each driver returns a `dgx_screen_t *` you can draw on. Color panels take a
pixel depth and a channel order; monochrome panels keep a RAM buffer that DGX
flushes for you. Use `GPIO_NUM_NC` for any reset/CS line your board does not
wire.

| Controller | Type | Buses | Constructor | Kconfig option |
| --- | --- | --- | --- | --- |
| ST7735 | color TFT | SPI, P8 | `dgx_st7735_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_ST7735` |
| ST7789 | color TFT | SPI, P8 | `dgx_st7789_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_ST7789` |
| ILI9341 | color TFT | SPI, P8 | `dgx_ili9341_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_ILI_9341` |
| GC9A01 | round color TFT | SPI, P8 | `dgx_gc9a01_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_GC9A01` |
| SSD1351 | color OLED | SPI, P8 | `dgx_ssd1351_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SSD1351` |
| SSD1306 | mono OLED | SPI, I2C | `dgx_ssd1306_init(bus, resolution, is_ext_vcc, rst)` | `CONFIG_DGX_ENABLE_SSD1306` |
| ST7565R | mono LCD | SPI | `dgx_st7565r_init(bus, rst)` | `CONFIG_DGX_ENABLE_ST7565R` |
| ST7920 | mono LCD | SPI, I2C, P8 | `dgx_st7920_init(bus, rst, cs)` | `CONFIG_DGX_ENABLE_ST7920` |

Common parameters:

- `bus` — a `dgx_bus_protocols_t *` from `dgx_spi_init()`, `dgx_i2c_init()` or
  `dgx_p8_init()`.
- `rst` — reset GPIO, or `GPIO_NUM_NC` if the panel has no reset line.
- `color_bits` — pixel depth; `16` (RGB565) is the usual choice.
- `cbo` — channel order, `DgxScreenRGB` or `DgxScreenBGR`.

Color panels also expose an orientation setter
(`dgx_<driver>_orientation(scr, dir_x, dir_y, swap_xy)`) that reprograms the
controller's scan direction. A few drivers add extras: GC9A01 has
`dgx_gc9a01_display_off()` / `dgx_gc9a01_display_on()`, SSD1351 has
`dgx_ssd1351_brightness()`, and SSD1306 has `dgx_ssd1306_contrast()`.

For SSD1306, pick a geometry from `ssd1306_resolution_t`: `SSD1306_128X32`,
`SSD1306_128X64`, `SSD1306_96X16`, `SSD1306_64X48` or `SSD1306_72X40`.

## API reference

Every drawing and text call takes a `dgx_screen_t *`, so the same code works
across hardware panels and virtual screens. Colors are plain `uint32_t` values
packed for the target pixel format (see [Colors](#colors)).

### Drawing primitives

Declared in [include/dgx_draw.h](include/dgx_draw.h):

| Function | Description |
| --- | --- |
| `dgx_set_pixel(scr, x, y, color)` | Set one pixel. |
| `dgx_get_pixel(scr, x, y)` | Read one pixel (reliable on virtual screens only). |
| `dgx_draw_line(scr, x1, y1, x2, y2, color)` | Single-pixel line. |
| `dgx_draw_line_thick(scr, x1, y1, x2, y2, width, color)` | Thick line with caps. |
| `dgx_draw_line_mask(scr, x1, y1, x2, y2, color, bg, mask, mask_bits)` | Dashed/dotted line; returns the rotated mask to continue the pattern. |
| `dgx_fill_rectangle(scr, x, y, w, h, color)` | Filled rectangle. |
| `dgx_draw_circle(scr, x, y, r, color)` | Circle outline. |
| `dgx_solid_circle(scr, x, y, r, color)` | Filled circle. |
| `dgx_draw_triangle_solid(scr, x0, y0, x1, y1, x2, y2, color)` | Filled triangle. |
| `dgx_draw_polygon4_solid(scr, x0..y3, color)` | Filled simple quadrilateral (convex or concave). |

### Text and fonts

Declared in [include/dgx_font.h](include/dgx_font.h):

| Function | Description |
| --- | --- |
| `dgx_font_string_utf8_screen(scr, x, y, str, color, orientation, scale, font, draw_func, param)` | Render a UTF-8 string. Pass `NULL, NULL` for the last two for default rendering. |
| `dgx_font_char_to_screen(scr, x, y, codePoint, color, orientation, scale, font, draw_func, param)` | Render one code point. |
| `dgx_font_string_bounds(str, font, ycorner, height)` | Measure a string; returns width, fills top offset and height. |
| `dgx_font_find_glyph(codePoint, font, xAdvance)` | Look up a single glyph. |
| `decodeUTF8next(chr, idx)` | Decode the next UTF-8 code point. |
| `dgx_font_make_morph_struct(...)` / `dgx_font_make_morph_struct_destroy(...)` | Build/free a glyph-to-glyph morph descriptor for animation. |

`orientation` is a `dgx_output_orientation_t`: `DgxOutputNormal`,
`DgxOutputMirrorX`, `DgxOutputMirrorY`, `DgxOutputRotate180`,
`DgxOutputTranspose`, `DgxOutputRotate90CCW`, `DgxOutputRotate90CW` or
`DgxOutputTransverse`. `scale` is an integer multiplier (`1` = native size).
Each bundled font header under `include/fonts/` exposes an accessor, e.g.
`ArialRegular12()`.

### Colors

The helpers in [include/dgx_colors.h](include/dgx_colors.h) are macros, not
constants. You pair them with either the uppercase packing macros from
[include/dgx_bits.h](include/dgx_bits.h) or the inline `dgx_rgb_to_*()`
wrappers, so the same color name works across pixel formats:

```c
DGX_LIGHTGREY(DGX_RGB_16)   // pure macro expansion, RGB565
DGX_RED(dgx_rgb_to_16)      // inline wrapper, also RGB565
DGX_RED(DGX_RGB_24)         // 24-bit RGB
DGX_WHITE(DGX_RGB_12)       // 12-bit packed color
```

Available packers: `DGX_RGB_12` / `dgx_rgb_to_12`, `DGX_RGB_16` /
`dgx_rgb_to_16`, `DGX_RGB_18` / `dgx_rgb_to_18`, `DGX_RGB_24` /
`dgx_rgb_to_24`. Named colors include `DGX_BLACK`, `DGX_WHITE`, `DGX_RED`,
`DGX_GREEN`, `DGX_BLUE`, `DGX_CYAN`, `DGX_MAGENTA`, `DGX_YELLOW`,
`DGX_ORANGE`, `DGX_NAVY`, `DGX_GOLD`, `DGX_SILVER`, `DGX_SKYBLUE` and more.
For most ESP32 TFT panels here, `DGX_RGB_16` is the right choice.

### Virtual screens (API)

Virtual screens let you draw into RAM first and decide later when and where to
push the result — handy for flicker-free animation, off-screen composition,
shadow buffers behind monochrome controllers, region copies, and LUT-expanded
8-bit assets. Declared in
[include/drivers/vscreen.h](include/drivers/vscreen.h) (color) and
[include/dgx_bw_screen.h](include/dgx_bw_screen.h) (1-bit):

| Function | Description |
| --- | --- |
| `dgx_vscreen_init(width, height, color_bits, cbo)` | Allocate a color RAM screen. |
| `dgx_vscreen_clone(src)` | Allocate a copy with the same geometry and pixels. |
| `dgx_vscreen_copy(dst, src)` | Copy a whole same-sized screen. |
| `dgx_vscreen_to_vscreen(dst, x, y, src, has_transparency)` | Blit one RAM screen onto another. |
| `dgx_vscreen_to_vscreen_oriented(dst, x, y, src, has_transparency, orientation)` | Blit with rotation/mirroring. |
| `dgx_vscreen_to_screen(dst, x, y, src)` | Push a RAM screen to any destination (incl. hardware). |
| `dgx_vscreen_region_to_screen(dst, x, y, src, x_src, y_src, w, h)` | Push a sub-region. |
| `dgx_vscreen_to_screen_oriented(...)` / `dgx_vscreen_region_to_screen_oriented(...)` | Oriented variants. |
| `dgx_vscreen8_to_screen16(dst, x, y, src, lut, has_transparency)` | Expand an 8-bit indexed screen into 16-bit through a LUT. |
| `dgx_bw_init(width, height)` | Allocate a 1-bit monochrome RAM screen. |
| `dgx_vscreen_2h_init(left, right)` | Compose two screens as one wide logical screen. |

Free any screen with `dgx_screen_destroy(&scr)`.

### Arc gauge

Declared in [include/dgx_gauge.h](include/dgx_gauge.h):

| Function | Description |
| --- | --- |
| `dgx_gauge_init(gauge, scr, cx, cy, inner_radius, width, start_angle, sweep_degrees, min, max, bg_color, color_fn)` | Configure a ring gauge. `color_fn` maps a value to a step color. |
| `dgx_gauge_set_value(gauge, value)` | Update the value, redrawing only changed steps. |
| `dgx_gauge_redraw(gauge)` | Redraw the whole gauge. |

### Bus backends

Declared under [include/bus/](include/bus):

| Function | Description |
| --- | --- |
| `dgx_spi_init(host_id, dma_chan, mosi, miso, sclk, cs, dc, clock_speed_hz, cpolpha_mode)` | SPI master bus. |
| `dgx_i2c_init(i2c_num, i2c_address, sda, sclk, clock_speed_hz)` | I2C master bus. |
| `dgx_p8_init(d0..d7, wr, rd, cs, dc, pclk_hz)` | 8-bit parallel (I80) bus. |

Each returns a `dgx_bus_protocols_t *` to hand to a driver constructor. A
driver pulls in only the transports it needs; enable them in `menuconfig`.

### Flush control and batching

Most DGX drawing APIs flush the touched region automatically by calling
`scr->update_screen(...)` when the current top-level operation finishes.
The `in_progress` field on `dgx_screen_t` is the nesting counter that suppresses
those intermediate flushes while a larger operation is still building up.

Declared in [include/dgx_screen.h](include/dgx_screen.h):

| API | Description |
| --- | --- |
| `scr->in_progress` | Nesting depth for deferred flushes. `0` means a standalone draw may flush immediately; `> 0` means wait until the outermost operation finishes. |
| `dgx_screen_progress_up(scr)` | Increment the nesting counter before a batched operation. Returns the new depth. |
| `dgx_screen_progress_down(scr)` | Decrement the nesting counter after a batched operation. Returns the new depth; when it becomes `0`, callers typically issue one final `update_screen(...)` for the combined dirty area. |
| `dgx_screen_destroy(&scr)` | Destroy a screen allocated by a driver or virtual screen constructor. |

Guidelines:

- Most applications should not touch `in_progress` at all.
- If you batch manually, prefer `dgx_screen_progress_up()` and
  `dgx_screen_progress_down()` over modifying the field directly.
- Always pair every `up` with one `down`.
- While the counter is positive, track the dirty rectangle yourself and flush
  once when the counter drops back to `0`.

## Tutorials

### Flicker-free animation with a virtual screen

Draw each frame into RAM, then push the finished frame to the panel in one go.
This avoids the visible tearing you get when drawing primitives straight to the
display line by line.

```c
#include "drivers/vscreen.h"
#include "dgx_draw.h"
#include "dgx_colors.h"
#include "dgx_bits.h"

// off-screen buffer matching the panel format
dgx_screen_t *frame = dgx_vscreen_init(scr->width, scr->height, 16, DgxScreenRGB);

for (int x = 0; x < scr->width; ++x) {
    // 1. clear and draw this frame into RAM
    dgx_fill_rectangle(frame, 0, 0, frame->width, frame->height,
                       DGX_BLACK(dgx_rgb_to_16));
    dgx_solid_circle(frame, x, frame->height / 2, 12,
                     DGX_CYAN(dgx_rgb_to_16));

    // 2. blit the whole frame to the panel at once
    dgx_vscreen_to_screen(scr, 0, 0, frame);
}

dgx_screen_destroy(&frame);
```

Use `dgx_vscreen_region_to_screen()` to push only the part that changed, or
`dgx_vscreen_to_screen_oriented()` to rotate the buffer on the way out.

### Driving a monochrome panel

Monochrome controllers keep a 1-bit RAM page buffer. You draw with the same
primitives; the driver flushes RAM to the controller through the screen's
`update_screen` function. In normal use you do not need to call
`update_screen` yourself, because the draw and font code already does that
when needed. Colors are simply non-zero (on) or zero (off).

```c
#include "bus/dgx_i2c_esp32.h"
#include "drivers/ssd1306.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/TerminusTTFMedium12.h"

dgx_bus_protocols_t *bus =
    dgx_i2c_init(I2C_NUM_0, 0x3C, GPIO_NUM_21, GPIO_NUM_22, 400000);

dgx_screen_t *oled =
    dgx_ssd1306_init(bus, SSD1306_128X64, 0, GPIO_NUM_NC);

dgx_fill_rectangle(oled, 0, 0, oled->width, oled->height, 0); // clear
dgx_font_string_utf8_screen(oled, 0, 12, "Hello OLED", 1,
                            DgxOutputNormal, 1, TerminusTTFMedium12(),
                            NULL, NULL);
```

### Building an arc gauge

```c
#include "dgx_gauge.h"
#include "dgx_colors.h"
#include "dgx_bits.h"

static uint32_t gauge_color(int value) {
    return value < 70 ? DGX_GREEN(dgx_rgb_to_16)
                      : DGX_RED(dgx_rgb_to_16);
}

dgx_gauge_t gauge;
dgx_gauge_init(&gauge, scr,
               scr->width / 2, scr->height / 2, // center
               40, 12,                          // inner radius, ring width
               2.36f, 270,                      // start angle (rad), sweep
               0, 100,                          // value range
               DGX_DARKGREY(dgx_rgb_to_16),     // inactive step color
               gauge_color);

dgx_gauge_set_value(&gauge, 42); // redraws only the steps that changed
```

### Generating a custom font

When the bundled fonts are not enough, `font2c` converts a TTF/BDF font to a
C source/header pair offline. You need FreeType development headers installed:

```sh
cc font2c/font2c.c -o font2c/font2c $(pkg-config --cflags --libs freetype2)
```

Run it with a font file, a pixel size, and one or more inclusive Unicode
ranges:

```sh
./font2c path/to/YourFont.ttf 16 0x20 0x7e 0x410 0x44f
./font2c <font file> <size> <start range> <end range> [<start range> <end range>]*
```

It writes a `.c` and a matching `.h` named after the font family, style and
size. To use the generated font:

1. Move the `.c` file into `src/fonts/` and the `.h` file into
   `include/fonts/`.
2. Rerun CMake configure once — fonts are added through `file(GLOB ...)`, so
   the build system needs to notice the new file.
3. Include the generated header and pass its accessor to the text API, just
   like a bundled font.

If you prefer a GUI workflow, another option is
[FontCreator](https://github.com/Llerr/FontCreator), which generates embedded
C font data and lets you edit glyphs interactively. Use its exported `.c` and
`.h` files the same way: place the source in `src/fonts/`, the header in
`include/fonts/`, rerun CMake configure once, then include the generated
header in your application.

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

Copyright (c) 2021-2026 Anton Petrusevich. See [LICENSE](LICENSE).
