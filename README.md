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
2. Build `examples/screen_demo` for a general ILI9341 graphics test, or
  `examples/morph_demo` for the CYD word-morphing demo.

Either way, enable only the pieces you need in `menuconfig`, under the **DGX**
menu. Drivers automatically pull in the transports they require.

To build and flash the graphics demo:

```sh
cd examples/screen_demo
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py flash monitor
```

To run the morphing demo on the CYD board:

```sh
cd examples/morph_demo
idf.py set-target esp32
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
| `dgx_bw_bitmap_foreach_set(bmap, func, user_data)` | Iterate set pixels of a 1-bpp bitmap; zero bytes skip 8 pixels at once (LINES format). |

### Dot morphing framework

Enable with `CONFIG_DGX_ENABLE_MORPH`. Everything morphs one bit matrix into
another; declared in [include/dgx_matrix_morph.h](include/dgx_matrix_morph.h),
[include/dgx_morph.h](include/dgx_morph.h),
[include/dgx_morph_sources.h](include/dgx_morph_sources.h) and
[include/dgx_morph_render.h](include/dgx_morph_render.h):

| Part | API | Description |
| --- | --- | --- |
| Data | `dgx_bit_matrix_t`, `dgx_matrix_*()` | Packed 1-bit grid; inline `get_point`/`set_point`. |
| Sources | `dgx_morph_glyph_matrix(font, cp)` | Glyph of a dot or bitmap font as a matrix in the font-wide box. |
| Morph | `dgx_morph_create(from, to, sources, user_data)` | Plans flights in cell coordinates. `sources` decides where each new cell flies in from: `dgx_morph_sources_life` (all live neighbors) or `dgx_morph_sources_cells` (one free neighbor, then an expanding ring), or your own callback. |
| Frame | `dgx_morph_draw(morph, t, x, y, cell, trail, dot, user_data)`, `dgx_morph_progress()` | Stateless: emits the dots of progress `t` in pixels through a `dgx_morph_dot_func_t`. |
| Renderers | `dgx_morph_glow_*` / `dgx_morph_sprite_*` | Additive glow with phosphor persistence and its own vscreen, or an intensity-scaled dot sprite. Both map brightness to colors through a replaceable 256-entry LUT in the screen format (16, 18 or 24 bits; need `CONFIG_DGX_ENABLE_VSCREEN`). |

### Tutorial: morph two glyphs

This example turns `1` into `8`. It assumes `screen` is an initialized color
display and the component was built with `CONFIG_DGX_ENABLE_MORPH` and
`CONFIG_DGX_ENABLE_VSCREEN` enabled.

**1. Convert glyphs to matrices.** `dgx_morph_glyph_matrix()` rasterizes a
glyph into a 1-bit matrix. Both glyphs use the same font-wide box and baseline,
so their cells line up even when the glyphs have different shapes. The
matrices use cell coordinates, not screen pixels.

**2. Choose a cell size that fits.** A glow dot extends beyond its cell. The
code reserves `radius` pixels on every edge, where the default glow radius is
`cell / 2 + cell / 4`. It then picks the largest integer cell size for which
the scaled matrix and that margin fit on screen.

**3. Build the morph.** `dgx_morph_create()` compares the two matrices and
stores the flights, static dots and fading dots. `dgx_morph_sources_cells`
chooses one available source for each new cell: an adjacent cell first, then
an expanding ring. Use `dgx_morph_sources_life` instead when every live
neighbor should contribute, as in Game of Life.

**4. Animate the transition.** On each frame, `dgx_morph_progress()` maps
elapsed microseconds to `[0, 1]`. `dgx_morph_draw()` emits the dots at that
progress to the glow accumulator; `dgx_morph_glow_present()` blends the frame
with the retained phosphor and displays it. Create the glow object once for
the transition, not once per frame. With an additive glow renderer,
`trail = true` draws the lagging half-brightness tail as well as the head.

```c
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dgx_draw.h"
#include "dgx_morph.h"
#include "dgx_morph_render.h"
#include "dgx_morph_sources.h"

static void morph_glyphs(dgx_screen_t *screen, dgx_font_t *font)
{
  // Rasterize both glyphs into the same font-wide cell grid.
    dgx_bit_matrix_t *from = dgx_morph_glyph_matrix(font, '1');
    dgx_bit_matrix_t *to = dgx_morph_glyph_matrix(font, '8');
    dgx_morph_t *morph = NULL;
    dgx_morph_glow_t *glow = NULL;
    if (!from || !to) goto cleanup;

    int cell = 0;
    int radius = 0;
    // Include the glow falloff margin when fitting the glyph to the screen.
    for (int candidate = screen->width; candidate > 0; --candidate) {
        int candidate_radius = candidate / 2 + candidate / 4;
        if (candidate_radius < 1) candidate_radius = 1;
        int width = from->width * candidate + 2 * candidate_radius;
        int height = from->height * candidate + 2 * candidate_radius;
        if (width <= screen->width && height <= screen->height) {
            cell = candidate;
            radius = candidate_radius;
            break;
        }
    }
    if (!cell) goto cleanup;

    int width = from->width * cell + 2 * radius;
    int height = from->height * cell + 2 * radius;
    int x = (screen->width - width) / 2;
    int y = (screen->height - height) / 2;
    // CELLS uses a free neighbor first, then searches expanding rings.
    morph = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
    glow = dgx_morph_glow_create(width, height, cell, screen->color_bits);
    if (!morph || !glow) goto cleanup;

    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, 0);
    int64_t start_us = esp_timer_get_time();
    float t;
    do {
        // Draw inside the glow margin; present() places this buffer on screen.
        t = dgx_morph_progress(start_us, esp_timer_get_time(), 500000);
        dgx_morph_draw(morph, t, radius, radius, cell, true,
                       dgx_morph_glow_dot, glow);
        dgx_morph_glow_present(glow, t, screen, x, y);
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (t < 1.0f);

cleanup:
    // The morph owns its plan; these source matrices remain caller-owned.
    dgx_morph_glow_destroy(&glow);
    dgx_morph_destroy(&morph);
    dgx_matrix_destroy(&from);
    dgx_matrix_destroy(&to);
}
```

The source matrices are only needed while `dgx_morph_glyph_matrix()` and
`dgx_morph_create()` build their owned data, so the cleanup releases them along
with the morph and renderer. To morph your own patterns instead of font
glyphs, create matrices with `dgx_matrix_init()` and set their cells with
`dgx_matrix_set_point()`.

For the complete discussion of source callbacks, trails, glow, and the CYD
word demo, see the [English morphing tutorial](docs/morphing-en.md) or the
[Russian version](docs/morphing-ru.md).

`orientation` is a `dgx_output_orientation_t`: `DgxOutputNormal`,
`DgxOutputMirrorX`, `DgxOutputMirrorY`, `DgxOutputRotate180`,
`DgxOutputTranspose`, `DgxOutputRotate90CCW`, `DgxOutputRotate90CW` or
`DgxOutputTransverse`. `scale` is an integer multiplier (`1` = native size).
Each bundled font header under `include/fonts/` exposes an accessor, e.g.
`ArialRegular12()`.

### Colors

The helpers in [include/dgx_colors.h](include/dgx_colors.h) are parameterized
color constructors, not stored color values. `DGX_RED(rgb_func)` substitutes
the supplied packer into a call with the color's RGB channels. The screen's
pixel format is selected by passing either an uppercase packing macro from
[include/dgx_bits.h](include/dgx_bits.h) or an inline `dgx_rgb_to_*()` wrapper:

```c
DGX_LIGHTGREY(DGX_RGB_16)   // pure macro expansion, RGB565
DGX_RED(dgx_rgb_to_16)      // inline wrapper, also RGB565
DGX_RED(DGX_RGB_24)         // 24-bit RGB
DGX_WHITE(DGX_RGB_12)       // 12-bit packed color
```

For example, `DGX_RED(DGX_RGB_16)` preprocesses all the way to the bitwise
RGB565 packing expression with channels `(255, 0, 0)`. With
`DGX_RED(dgx_rgb_to_16)`, it preprocesses to `dgx_rgb_to_16(255, 0, 0)`.
The argument is a macro or function name, not a function pointer: there is no
indirect call or runtime format switch.

That keeps the color's meaning separate from its binary representation: one
palette of names works for 12-, 16-, 18- and 24-bit screens. Switching a
renderer from RGB565 to 18-bit RGB means changing the packer, not maintaining
a second set of `RED_565`, `RED_666` and `RED_888` constants. Passing the
packing macro gives direct macro expansion; passing the inline wrapper gives
the same result through a typed function-like interface.

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

For a physical display that should not be updated after every primitive, draw
the complete image into a virtual screen in RAM and send it to the panel when
the frame is ready with `dgx_vscreen_to_screen()` (see the virtual-screen
tutorial above). The virtual screen is the backbuffer; `in_progress` is not a
pixel buffer.

Some screens stage changes and commit them when `update_screen()` is called.
For those drivers, `in_progress` lets a group of drawing operations finish
before the automatic commit happens. This is useful on slower displays, where
showing each intermediate change can create visible pauses or partial updates.
It is a nesting counter: inner drawing operations defer their update while an
outer batch is active. On drivers that transmit pixels immediately from
`write_area()`, the counter cannot buffer those transfers; use a virtual screen
when the whole frame must be composed before it is sent.

Declared in [include/dgx_screen.h](include/dgx_screen.h):

| API | Description |
| --- | --- |
| `scr->in_progress` | Nesting depth for deferred `update_screen()` calls. `0` allows an update; `> 0` defers it. |
| `dgx_screen_progress_up(scr)` | Increment the nesting counter before a batched operation. Returns the new depth. |
| `dgx_screen_progress_down(scr)` | Decrement the nesting counter after a batched operation. Returns the new depth; when it becomes `0`, the caller can commit the accumulated dirty area with `update_screen(...)`. |
| `dgx_screen_destroy(&scr)` | Destroy a screen allocated by a driver or virtual screen constructor. |

```c
dgx_screen_progress_up(scr);
dgx_fill_rectangle(scr, x, y, w, h, bg);
dgx_draw_line(scr, x1, y1, x2, y2, fg);
if (!dgx_screen_progress_down(scr)) {
  scr->update_screen(scr, dirty_left, dirty_right, dirty_top, dirty_bottom);
}
```

Guidelines:

- Most applications should not touch `in_progress` at all.
- If you batch manually, prefer `dgx_screen_progress_up()` and
  `dgx_screen_progress_down()` over modifying the field directly.
- Always pair every `up` with one `down`.
- Track the dirty rectangle while batching; when the outermost `down` returns
  `0`, call `update_screen()` once for that region if the driver needs it.

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
For example, `DGX_RED(DGX_RGB_16)` preprocesses to the RGB565 packing
expression with channels `(255, 0, 0)`. `DGX_RED(dgx_rgb_to_16)` preprocesses
  framebuffer in RAM, so `get_pixel()` behaves as expected there. On physical
  panels, hardware readback is still incomplete and should not be relied on.

- **Virtual screens don't honor `dir_x`/`dir_y`/`swap_xy` for framebuffer
  access.** Those fields describe orientation metadata, but they do not
  rotate or mirror the stored pixel data.

packing macro gives direct macro expansion; passing the inline wrapper gives
the same result through a typed function-like interface.
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
examples/morph_demo/     sequential CYD word-morphing demo
Kconfig                  feature toggles
CMakeLists.txt           ESP-IDF component build
```

See [CHANGES.md](CHANGES.md) for the release history.

## License

Copyright (c) 2021-2026 Anton Petrusevich. See [LICENSE](LICENSE).
