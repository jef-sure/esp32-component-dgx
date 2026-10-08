# DGX

DGX is a small display graphics library for microcontrollers. I started writing
it years ago, back when most people reached for Adafruit GFX or TFT_eSPI, and
I wanted something shaped to my own taste: a clean split between bus, screen
and drawing code, no hidden global state, possibility to use several equal
screens simultaneously.

Today it lives as an ESP-IDF component with drivers for a handful of common
panels, a couple of RAM-backed virtual screens, a UTF-8 text renderer, and a
small offline tool for converting fonts. On top of that it animates: dots
that morph one glyph into another, and handwritten fonts whose text is
written stroke by stroke by a pen or turns into another text.

## Contents

- [What's in the box](#whats-in-the-box)
- [How it fits together](#how-it-fits-together)
- [Getting started](#getting-started)
- [A minimal example](#a-minimal-example)
- [Supported panels](#supported-panels)
- [Tutorials](#tutorials)
  - [Flicker-free animation with a virtual screen](#flicker-free-animation-with-a-virtual-screen)
  - [Driving a monochrome panel](#driving-a-monochrome-panel)
  - [Building an arc gauge](#building-an-arc-gauge)
  - [Stretching a texture onto a quad](#stretching-a-texture-onto-a-quad)
  - [Generating a custom font](#generating-a-custom-font)
  - [Morphing two glyphs](#morphing-two-glyphs)
  - [Writing a text by hand](#writing-a-text-by-hand)
  - [Morphing handwritten text](#morphing-handwritten-text)
- [API reference](#api-reference)
  - [Drawing primitives](#drawing-primitives)
  - [Text and fonts](#text-and-fonts)
  - [Handwritten fonts](#handwritten-fonts)
  - [Dot morphing framework](#dot-morphing-framework)
  - [Colors](#colors)
  - [Virtual screens](#virtual-screens-api)
  - [Arc gauge](#arc-gauge)
  - [Bus backends](#bus-backends)
  - [Flush control and batching](#flush-control-and-batching)
- [Coordinates and orientation](#coordinates-and-orientation)
- [Build options](#build-options)
- [Known gaps](#known-gaps)
- [Host tests](#host-tests)
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
- Drawing primitives — pixels, lines, rectangles, circles, filled and textured
  quads — and an arc gauge helper.
- UTF-8 text rendering with 8-way orientation, glyph lookup, layout and bounds
  queries, plus a small "morph" helper for animating between glyphs.
- Handwritten fonts: symbols kept as pen paths of Bezier curves, written at
  any size with a pen of any thickness, letters joined, at once or stroke by
  stroke at the pace of a pen.
- `font2c`, an offline tool that turns TTF/BDF-style fonts and handwritten
  fonts of the hw-fonts editor into C source/header pairs, and a set of
  pre-converted fonts in `src/fonts/`.

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
  └─ dgx_draw.h / dgx_font.h / dgx_hw_font.h / dgx_gauge.h / dgx_morph.h
        └─ dgx_screen_t  (vtable)
              ├─ dgx_screen_with_bus_t  → bus backend (SPI / I2C / P8)
              ├─ dgx_bw_vscreen_t       → 1-bit RAM buffer
              ├─ dgx_vscreen_t          → color RAM buffer
              └─ vscreen_2h             → composes two child screens
```

## Getting started

DGX is a component, not a standalone firmware image. It is published in the
[ESP Component Registry](https://components.espressif.com/components/jef-sure/dgx)
as `jef-sure/dgx`. To add it to your own ESP-IDF project:

```sh
idf.py add-dependency "jef-sure/dgx^0.4.4"
```

or put it into `main/idf_component.yml` yourself:

```yaml
dependencies:
  jef-sure/dgx: "^0.4.4"
```

The next build downloads it into `managed_components/`. To work on DGX itself,
check the repository out under `components/` instead (a git submodule at
`components/dgx` works well).

The repository also has examples to build: `examples/screen_demo` is a general
ILI9341 graphics test, and there are the CYD morphing demos:
`examples/morph_demo` (words, letter by letter), `examples/glyph_morph_demo`
(one large symbol into the next) and `examples/life_morph_demo` (Conway's Game
of Life). `examples/flip_clock_demo` is a split-flap clock for the same board,
built on textured quads, and `examples/texture_demo` scales, turns and wobbles
a textured card. `examples/hw_font_demo` writes a text by hand with a
font of Bezier curves and `examples/hw_morph_demo` morphs such words one into
another.

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

To run a morphing demo on the CYD board (the others, `examples/hw_font_demo`
and `examples/hw_morph_demo` too, build the same way):

```sh
cd examples/morph_demo
idf.py set-target esp32
idf.py flash monitor
```

The examples take DGX from the repository they sit in, so build them from a
checkout.

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
| ILI9341 | color TFT | SPI, P8 | `dgx_ili9341_init(bus, rst, backlight, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_ILI_9341` |
| GC9A01 | round color TFT | SPI, P8 | `dgx_gc9a01_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_GC9A01` |
| SSD1351 | color OLED | SPI, P8 | `dgx_ssd1351_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SSD1351` |
| SSD1306 | mono OLED | SPI, I2C | `dgx_ssd1306_init(bus, resolution, is_ext_vcc, rst)` | `CONFIG_DGX_ENABLE_SSD1306` |
| ST7565R | mono LCD | SPI | `dgx_st7565r_init(bus, rst)` | `CONFIG_DGX_ENABLE_ST7565R` |
| ST7920 | mono LCD | SPI, I2C, P8 | `dgx_st7920_init(bus, rst, cs)` | `CONFIG_DGX_ENABLE_ST7920` |

Common parameters:

- `bus` — a `dgx_bus_protocols_t *` from `dgx_spi_init()`, `dgx_i2c_init()` or
  `dgx_p8_init()`.
- `rst` — reset GPIO, or `GPIO_NUM_NC` if the panel has no reset line.
- `backlight` (ILI9341 only) — backlight GPIO, or `GPIO_NUM_NC` if it is not
  under software control.
- `color_bits` — pixel depth; `16` (RGB565) is the usual choice.
- `cbo` — channel order, `DgxScreenRGB` or `DgxScreenBGR`.

Color panels and ST7920 also expose an orientation setter
(`dgx_<driver>_orientation(scr, dir_x, dir_y, swap_xy)`) that reprograms the
controller's scan direction. A few drivers add extras: ILI9341 has
`dgx_ili9341_backlight()`, GC9A01 has `dgx_gc9a01_display_off()` /
`dgx_gc9a01_display_on()`, SSD1351 has `dgx_ssd1351_brightness()`, and SSD1306
has `dgx_ssd1306_contrast()`.

For SSD1306, pick a geometry from `ssd1306_resolution_t`: `SSD1306_128X32`,
`SSD1306_128X64`, `SSD1306_96X16`, `SSD1306_64X48` or `SSD1306_72X40`.

## Tutorials

Short walks through the things people ask first; the functions they use are
listed in the [API reference](#api-reference) after them.

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
               135.0f, 270,                     // start angle (deg), sweep
               0, 100,                          // value range
               DGX_DARKGREY(dgx_rgb_to_16),     // inactive step color
               gauge_color);

dgx_gauge_set_value(&gauge, 42); // redraws only the steps that changed
```

### Stretching a texture onto a quad

A texture is a virtual screen: draw anything into it once, then put it on the
display at any size and in any four-cornered shape. This is how a picture is
scaled, mirrored, tilted or folded without drawing it again.

```c
#include "drivers/vscreen.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_colors.h"
#include "dgx_bits.h"
#include "fonts/ArialRegular12.h"

// 1. the texture: the same color depth as the screen it goes to
dgx_screen_t *tex = dgx_vscreen_init(64, 24, 16, DgxScreenRGB);
dgx_fill_rectangle(tex, 0, 0, tex->width, tex->height, DGX_NAVY(dgx_rgb_to_16));
dgx_font_string_utf8_screen(tex, 6, 17, "DGX 0.4", DGX_WHITE(dgx_rgb_to_16),
                            DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);

// 2. scaled three times into a rectangle
dgx_draw_texture_rect(scr, 10, 10, 192, 72, tex, 0, 0, tex->width, tex->height);

// 3. onto a quad: corners of the region in the order
//    top-left, top-right, bottom-right, bottom-left
const dgx_point_2d_t card[4] = {{40, 110}, {200, 100}, {220, 170}, {20, 180}};
dgx_draw_texture_quad(scr, card, tex, 0, 0, tex->width, tex->height);

// 4. a part of the texture, mirrored: swap the left and the right corners
const dgx_point_2d_t half[4] = {{238, 10}, {206, 10}, {206, 82}, {238, 82}};
dgx_draw_texture_quad(scr, half, tex, 0, 0, tex->width / 2, tex->height);

dgx_screen_destroy(&tex);
```

To move the picture, move the corners. Scaling, rotation and a wobble are all
the same thing, four corners counted anew for every frame:

```c
// corners of a w x h picture around (cx, cy): scaled, each swung on its own
// by `wobble` pixels, then all turned by `angle`
static void corners(dgx_point_2d_t quad[4], float cx, float cy, float w, float h,
                    float scale, float angle, float wobble, float phase)
{
    static const float side_x[4] = {-1, 1, 1, -1}, side_y[4] = {-1, -1, 1, 1};
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; ++i) {
        float x = side_x[i] * w / 2 * scale + wobble * sinf(phase + i * 1.7f);
        float y = side_y[i] * h / 2 * scale + wobble * cosf(phase * 1.3f + i * 2.3f);
        quad[i].x = (int16_t)lroundf(cx + x * c - y * s);
        quad[i].y = (int16_t)lroundf(cy + x * s + y * c);
    }
}

// a frame: clear a virtual screen, draw the quad, send it to the display
corners(quad, frame->width / 2, frame->height / 2, tex->width, tex->height,
        1.0f + 0.5f * sinf(t * 2), t * 1.6f, 0, 0);
dgx_fill_rectangle(frame, 0, 0, frame->width, frame->height, DGX_BLACK(dgx_rgb_to_16));
dgx_draw_texture_quad(frame, quad, tex, 0, 0, tex->width, tex->height);
dgx_vscreen_to_screen(scr, x, y, frame);
```

The quad must be convex. The mapping is affine, without perspective
correction, and takes the nearest texel. A scaled or turned picture stays a
rectangle and is drawn exactly; an uneven quad, a wobbling one, shows a
slight bend along its diagonal, which suits a wobble well.
[examples/texture_demo](examples/texture_demo) shows scaling, rotation and
wobbling on a CYD, and [examples/flip_clock_demo](examples/flip_clock_demo)
folds the halves of its digits this way.

### Generating a custom font

When the bundled fonts are not enough, `font2c` converts a TTF/BDF font to a
C source/header pair offline. You need FreeType development headers installed:

```sh
cc font2c/font2c.c -o font2c/font2c $(pkg-config --cflags --libs freetype2)
```

Run it with a font file, a pixel size, and the characters to include: inclusive
Unicode ranges, a UTF-8 text file with the characters (`-f`), or both:

```sh
./font2c path/to/YourFont.ttf 16 0x20 0x7e 0x410 0x44f
./font2c -f charset.txt path/to/YourFont.ttf 16
./font2c [-f charset_file] <font file> <size> [<first> <last>]*
```

It writes a `.c` and a matching `.h` named after the font family, style and
size. To use the generated font:

1. Move the `.c` file into `src/fonts/` and the `.h` file into
   `include/fonts/`.
2. Rerun CMake configure once — fonts are added through `file(GLOB ...)`, so
   the build system needs to notice the new file.
3. Include the generated header and pass its accessor to the text API, just
   like a bundled font.

`font2c` also converts handwritten fonts made of Bezier curves, saved as
`.json` by the [hw-fonts](https://github.com/jef-sure/hw-fonts) editor; there
is no size argument for them. The format and the tables it becomes are
described in [docs/hw-font-ru.md](docs/hw-font-ru.md) (in Russian). Such a
font is drawn by its own functions from `dgx_hw_font.h`, not by the text API
above: letters are joined, the size and the pen are chosen at drawing, and
the text may be written by the pace of a pen.

```c
#include "dgx_hw_font.h"

/* the line starts at (10, 80) on its base line; 0.3 pixel per cell of the font, a pen 3 pixels thick, joined */
dgx_hw_draw_text(scr, 10, 80, font0ss(), "Hello, World", 0.3f, 3, color, true);

/* the same written by a pen: the effort it has gone through sets how much is drawn */
dgx_hw_pace_t    pace = dgx_hw_pace(font0ss(), 6);
dgx_hw_writing_t writing;
dgx_hw_writing_begin(&writing, scr, 10, 80, font0ss(), "Hello, World", 0.3f, 3, color, true, &pace);
while (!dgx_hw_writing_draw_to(&writing, tempo * seconds_since_start())) wait_a_tick();
```

```sh
./font2c path/to/font0-ss.json
```

If you prefer a GUI workflow, another option is
[FontCreator](https://github.com/Llerr/FontCreator), which generates embedded
C font data and lets you edit glyphs interactively. Use its exported `.c` and
`.h` files the same way: place the source in `src/fonts/`, the header in
`include/fonts/`, rerun CMake configure once, then include the generated
header in your application.

### Morphing two glyphs

This example turns `1` into `8`. It assumes `screen` is an initialized color
display and the component was built with `CONFIG_DGX_ENABLE_MORPH` (it selects
`CONFIG_DGX_ENABLE_VSCREEN`).

**1. Convert glyphs to matrices.** `dgx_morph_glyph_matrix()` rasterizes a
glyph into a 1-bit matrix. Both glyphs use the same font-wide box and baseline,
so their cells line up even when the glyphs have different shapes. The
matrices use cell coordinates, not screen pixels.

**2. Choose a cell size that fits.** A glow dot extends beyond its cell. The
code reserves `radius` pixels on every edge, where the glow radius is
`cell / 2 + cell / 4`, but at least 2. It then picks the largest integer cell size for which
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
        if (candidate_radius < 2) candidate_radius = 2;
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
`dgx_matrix_set_point()`, or convert a 1-bpp bitmap with
`dgx_matrix_from_bw_bitmap()`. For a whole line of text, `dgx_morph_text_create()`
builds one morph per letter; `examples/morph_demo` uses it.

For the complete discussion of source callbacks, trails, glow, and the CYD
word demo, see the [English morphing tutorial](docs/morphing-en.md) or the
[Russian version](docs/morphing-ru.md).

### Writing a text by hand

A handwritten font is written at any size, with a pen of any thickness, the
letters joined. `scale` is pixels per cell of the font's grid; a line is
`symbol_size_y` cells high.

```c
#include "dgx_hw_font.h"
#include "fonts/font0ss.h"

dgx_font_t *font = font0ss();
const char *text = "Hello, World";

// 1. the largest size at which the text fits the width, and its place in the middle
int left, top, right, bottom;
dgx_hw_text_box(font, text, &left, &top, &right, &bottom);
int   pen = 3;
float scale = (float)(scr->width - 12 - pen) / (right - left + 1);
int   x = (int)lroundf((scr->width - (right - left + 1) * scale) / 2 - left * scale);
int   y = scr->height / 2;   // the base line

// 2a. all at once
dgx_hw_draw_text(scr, x, y, font, text, scale, pen, color, true);

// 2b. or written by a pen: it goes through 1500 units of effort a second
dgx_hw_pace_t    pace = dgx_hw_pace(font, 6);
dgx_hw_writing_t writing;
dgx_hw_writing_begin(&writing, scr, x, y, font, text, scale, pen, color, true, &pace);
int64_t start = esp_timer_get_time();
while (!dgx_hw_writing_draw_to(&writing, 1500.0f * (esp_timer_get_time() - start) * 1e-6f)) {
    vTaskDelay(1);
}
```

Writing only adds ink, so it can go straight to the display. The effort of an
element is `length + k * pieces`: the way of the pen, and more for a curve
that bends more; `k` is the second argument of `dgx_hw_pace()`, 0 for an even
speed. See [docs/handwriting-en.md](docs/handwriting-en.md) and
[examples/hw_font_demo](examples/hw_font_demo).

### Morphing handwritten text

Every stroke is a cubic Bezier curve, so a morph is a list of pairs of curves
and a frame is every pair taken between its two curves. A frame is a new
picture each time: draw it into a virtual screen and send that to the display.

```c
#include "dgx_hw_font.h"
#include "drivers/vscreen.h"
#include "fonts/font0ss.h"

// color 0 of the band is paper, 255 is ink; lut[] holds the screen colors
dgx_screen_t *band = dgx_vscreen_init(scr->width, 100, 8, DgxScreenRGB);

// a line as a whole: the whole way of the pen into the whole way of the pen
dgx_hw_morph_t *morph = dgx_hw_morph_create(font0ss(), "hello", "world", true);

int64_t start = esp_timer_get_time();
float   t;
do {
    t = (esp_timer_get_time() - start) / 1500000.0f;   // 1.5 s
    if (t > 1) t = 1;
    dgx_fill_rectangle(band, 0, 0, band->width, band->height, 0);
    dgx_hw_morph_draw(morph, t * t * (3 - 2 * t), band, 10, 70, 0.4f, 4, 255);
    dgx_vscreen8_to_screen16(scr, 0, 70, band, lut, false);
    vTaskDelay(1);
} while (t < 1);

dgx_hw_morph_destroy(&morph);
```

`dgx_hw_morph_text_create()` plans the same letter by letter: one morph for
every position, `text->letters[i]`, each drawn with a progress of its own, so
letters change one after another and the ones that stay the same do not move
— a clock, for one. See [docs/handwriting-en.md](docs/handwriting-en.md) and
[examples/hw_morph_demo](examples/hw_morph_demo).

## API reference

Every drawing and text call takes a `dgx_screen_t *`, so the same code works
across hardware panels and virtual screens. Colors are plain `uint32_t` values
packed for the target pixel format (see [Colors](#colors)).

### Drawing primitives

Declared in [include/dgx_draw.h](include/dgx_draw.h):

| Function | Description |
| --- | --- |
| `dgx_set_pixel(scr, x, y, color)` | Set one pixel. |
| `dgx_get_pixel(scr, x, y)` | Read one pixel: virtual screens and panels drawn through a virtual back screen only; see [Known gaps](#known-gaps). |
| `dgx_draw_line(scr, x1, y1, x2, y2, color)` | Single-pixel line. |
| `dgx_draw_line_thick(scr, x1, y1, x2, y2, width, color)` | Thick line with caps. |
| `dgx_draw_bezier3(scr, points, pieces, width, color)` | Thick quadratic Bezier curve by three `dgx_point_2d_t` points, drawn by `pieces` straight pieces; 0 finds their number from the points, within half a pixel of the curve. |
| `dgx_draw_bezier4(scr, points, pieces, width, color)` | Thick cubic Bezier curve by four points. |
| `dgx_bezier3_begin(b, scr, points, pieces, width, color)`, `dgx_bezier4_begin(...)` | Prepare the same curves in a `dgx_bezier_t` for drawing part by part, to animate writing. |
| `dgx_bezier_draw_to(b, t)` | Draw a prepared curve on until it is `t` complete, 0 .. 1 by the way of the pen, and flush what was added; returns `true` when the curve is finished. |
| `dgx_bezier_length(b)` | Length of a prepared curve in pixels, to share time between curves. |
| `dgx_draw_line_mask(scr, x1, y1, x2, y2, color, bg, mask, mask_bits)` | Dashed/dotted line; returns the rotated mask to continue the pattern. |
| `dgx_fill_rectangle(scr, x, y, w, h, color)` | Filled rectangle. |
| `dgx_draw_circle(scr, x, y, r, color)` | Circle outline. |
| `dgx_solid_circle(scr, x, y, r, color)` | Filled circle. |
| `dgx_draw_triangle_solid(scr, x0, y0, x1, y1, x2, y2, color)` | Filled triangle. |
| `dgx_draw_polygon4_solid(scr, x0..y3, color)` | Filled simple quadrilateral (convex or concave). |
| `dgx_draw_texture_quad(scr, quad, texture, tx, ty, tw, th)` | Stretch a region of a virtual screen onto a convex quadrilateral (affine, nearest texel). |
| `dgx_draw_texture_rect(scr, x, y, w, h, texture, tx, ty, tw, th)` | Scale a region of a virtual screen into a rectangle. |

How a Bezier curve is split into straight pieces, how a piece is drawn and how
a curve is drawn on by `t` is described in
[docs/handwriting-en.md](docs/handwriting-en.md#bezier-curves).

`quad` is four `dgx_point_2d_t` vertices in the order of the region's corners:
top-left, top-right, bottom-right, bottom-left. The texture needs the same
color depth as the target screen; see
[Stretching a texture onto a quad](#stretching-a-texture-onto-a-quad), and
`examples/flip_clock_demo` uses both functions.

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
| `dgx_bw_bitmap_foreach_set(bmap, func, user_data)` | From [include/dgx_bitmap.h](include/dgx_bitmap.h): iterate set pixels of a 1-bpp bitmap; zero bytes skip 8 pixels at once (LINES format). |

`orientation` is a `dgx_output_orientation_t`: `DgxOutputNormal`,
`DgxOutputMirrorX`, `DgxOutputMirrorY`, `DgxOutputRotate180`,
`DgxOutputTranspose`, `DgxOutputRotate90CCW`, `DgxOutputRotate90CW` or
`DgxOutputTransverse`. `scale` is an integer multiplier (`1` = native size).
Each bundled font header under `include/fonts/` exposes an accessor, e.g.
`ArialRegular12()`.

A font tells its kind in `f_type`: bitmaps (`DGX_FONT_BITMAP_LINES`,
`DGX_FONT_BITMAP_STREAM`), dots (`DGX_FONT_DOTS`) or pen paths
(`DGX_FONT_HW`). The functions above draw the first two kinds; a handwritten
font shares only the glyph lookup with them and is drawn by the functions of
the next section. Fonts made by the current `font2c` also tell the number of
their ranges, so a glyph is found by halving instead of trying the ranges in
order.

### Handwritten fonts

A handwritten font keeps every symbol as the path of a pen: Bezier curves in
the order a hand writes them. It is a `dgx_font_t` of type `DGX_FONT_HW`, its
symbols are found like any glyphs, but it is drawn by its own functions
declared in [include/dgx_hw_font.h](include/dgx_hw_font.h): the size and the
thickness of the pen are chosen at drawing, and letters are joined.

| Function | Description |
| --- | --- |
| `dgx_hw_draw_text(scr, x, y, font, text, scale, width, color, joined)` | Write a UTF-8 text at once. `x, y` is the start of the line on its base line, `scale` pixels per cell of the font's grid (a line is `symbol_size_y` cells high), `width` the thickness of the pen, `joined` joined writing; `\n` starts the next line. |
| `dgx_hw_text_box(font, text, left, top, right, bottom)` | The box the text takes, in cells from the start of its first line: to choose a size and a place. |
| `dgx_hw_writing_begin(writing, scr, x, y, font, text, scale, width, color, joined, pace)` | Prepare a text for writing by the pace of a pen; draws nothing. |
| `dgx_hw_writing_draw_to(writing, effort)` | Write on until the pen has gone through `effort`, adding only what is new; returns `true` when the text is finished. |
| `dgx_hw_text_effort(font, text, joined, pace)` | The whole effort of a text; `t * effort` leads the writing by a progress `t` of 0 .. 1. |
| `dgx_hw_pace(font, k)` | How effort is counted: an element takes `length + k * pieces`, a dot and a move of the pen in the air as much as the hyphen of the font. |
| `dgx_hw_writer_begin(writer, font, text, joined)` / `dgx_hw_writer_next(writer, stroke)` | The strokes of a text one by one in the order of writing, in cells: for a drawing of your own. |
| `dgx_hw_line_begin(line, font)` / `dgx_hw_line_place(line, code_point, shift)` | Where each next symbol of a line stands. |
| `dgx_hw_draw_text_xy(...)`, `dgx_hw_writing_begin_xy(...)`, `dgx_hw_morph_draw_xy(...)` | The same as their namesakes with `scale_x, scale_y` in place of `scale`: a text narrow and tall or wide and low. The pen stays round. |
| `dgx_hw_morph_create(font, from, to, joined)` / `dgx_hw_morph_destroy(&morph)` | Plan the morph of a line into a line as a whole: the whole way of the pen into the whole way of the pen. |
| `dgx_hw_morph_text_create(font, from, to, joined)` / `dgx_hw_morph_text_destroy(&text)` | Plan it letter by letter: `text->letters[i]` is a morph for every position, each led by its own `t`. |
| `dgx_hw_morph_draw(morph, t, scr, x, y, scale, width, color)` | Draw the frame at `t` of 0 .. 1 on a cleared place. |
| `dgx_hw_morph_shift(morph, from_x, from_y, to_x, to_y)` / `dgx_hw_morph_text_shift(...)` | Move the two texts, each by its own shift in cells: to centre both, say. |
| `dgx_hw_morph_box(morph, left, top, right, bottom)` | The box no frame leaves, in cells. |

A stroke of any kind is a cubic curve, so a text is a list of cubic curves in
the order of writing and a morph is a list of pairs of them; a frame is every
pair with its four points taken between the two curves. A frame is drawn anew
every time, so it wants a virtual screen.

[examples/hw_font_demo](examples/hw_font_demo) fits three lines to the screen
and writes them by hand, [examples/hw_morph_demo](examples/hw_morph_demo)
morphs words one into another both ways.

One such font is bundled: `font0ss()` from `fonts/font0ss.h`. It has the Latin
alphabet with the German letters `ÄÖÜäöüß`, the Russian alphabet, digits and
punctuation, 153 symbols in all, and nothing else: no accented letters of
other languages, no Greek, no scripts written right to left. Drawing a
handwritten alphabet is slow work and I am not going to draw more of them;
if you need another one, draw it in the editor and send it over, co-authors
are welcome. A font comes from the
[hw-fonts](https://github.com/jef-sure/hw-fonts) editor through `font2c`, see
[Generating a custom font](#generating-a-custom-font).

Sizing, the thick Bezier curves underneath, writing at the pace of a pen and
both morphs are described in detail in
[docs/handwriting-en.md](docs/handwriting-en.md)
([Russian](docs/handwriting-ru.md)), with two tutorials below:
[Writing a text by hand](#writing-a-text-by-hand) and
[Morphing handwritten text](#morphing-handwritten-text). The format of a font
and the rules of placing and joining letters are in
[docs/hw-font-ru.md](docs/hw-font-ru.md) (in Russian).

### Dot morphing framework

Enable with `CONFIG_DGX_ENABLE_MORPH`. Everything morphs one bit matrix into
another; declared in [include/dgx_matrix_morph.h](include/dgx_matrix_morph.h),
[include/dgx_morph.h](include/dgx_morph.h),
[include/dgx_morph_sources.h](include/dgx_morph_sources.h) and
[include/dgx_morph_render.h](include/dgx_morph_render.h):

| Part | API | Description |
| --- | --- | --- |
| Data | `dgx_bit_matrix_t`, `dgx_matrix_*()` | Packed 1-bit grid; inline `get_point`/`set_point`; `clear`, `clone`, `copy`, `equals`. |
| Sources | `dgx_morph_glyph_matrix(font, cp)`, `dgx_matrix_from_bw_bitmap(bmap)` | Glyph of a dot or bitmap font as a matrix in the font-wide box; any 1-bpp bitmap as a matrix. |
| Morph | `dgx_morph_create(from, to, sources, user_data)` | Plans flights in cell coordinates. `sources` decides where each new cell flies in from: `dgx_morph_sources_life` (all live neighbors) `dgx_morph_sources_cells` (the nearest free cell, searched vector by vector over the whole grid, so a line that moved flies as a whole), `dgx_morph_sources_cells_within` (the same no further than a radius, an `int` that `user_data` points to), or your own callback, which is many times slower when it defers pass after pass. Sides above 32767 cells are rejected. |
| Text | `dgx_morph_text_create(font, from, to, length, sources, user_data)`, `dgx_morph_text_duration_us()` | One morph per letter between two UTF-8 strings; the shorter one is padded with spaces. `changed` tells how many letters actually move. |
| Frame | `dgx_morph_draw(morph, t, x, y, cell, trail, dot, user_data)`, `dgx_morph_progress()` | Stateless: emits the dots of progress `t` in pixels through a `dgx_morph_dot_func_t`. |
| Renderers | `dgx_morph_glow_*` / `dgx_morph_sprite_*` | Additive glow with phosphor persistence and its own vscreen, or an intensity-scaled dot sprite. Both map brightness to colors through a replaceable 256-entry LUT in the screen format (16, 18 or 24 bits). |
| Filter | `dgx_morph_glow_set_filter(glow, filter, user_data)`, `dgx_morph_glow_blur` | One more pass over a copy of every finished glow frame right before it is shown; it does not get into the phosphor. The ready blur smooths the steps of glyphs turned into dots. |

[Morphing two glyphs](#morphing-two-glyphs) walks through all of it.

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
| `dgx_vscreen_is_linear(scr)` | True when the screen is a `dgx_vscreen_t` with row-major pixels in `v_array`. |
| `dgx_bw_screen_is_paged(scr)` | True when the screen keeps its bits in 8-row pages (`dgx_bw_init()`, SSD1306, ST7565R). |
| `dgx_bw_blit_or(scr, x, y, bmap)` | OR a 1-bpp bitmap into a page screen, clipped. |

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
tutorial below). The virtual screen is the backbuffer; `in_progress` is not a
pixel buffer.

Some screens stage changes and commit them when `update_screen()` is called
(ST7920, SSD1306, ST7565R, the two-head compositor). Every primitive marks
what it changed with `dgx_screen_touch()`; the screen keeps one pending dirty
rectangle. Outside a batch the rectangle is committed at once. Inside a batch
(`in_progress > 0`) rectangles are merged, and the outermost
`dgx_screen_progress_down()` commits their bounding box with a single
`update_screen()` call. One transfer of a larger area is usually cheaper than
several small ones: setting up a transfer costs more latency than the pixels
themselves. On drivers that transmit pixels immediately from `write_area()`,
the batch cannot buffer those transfers; use a virtual screen when the whole
frame must be composed before it is sent.

Declared in [include/dgx_screen.h](include/dgx_screen.h):

| API | Description |
| --- | --- |
| `dgx_screen_progress_up(scr)` | Open a batch (nesting counter). Returns the new depth. |
| `dgx_screen_progress_down(scr)` | Close a batch; at depth `0` commits the pending dirty area. Returns the new depth. |
| `dgx_screen_touch(scr, left, right, top, bottom)` | Mark an area as changed (inclusive, clipped). For code that writes pixels directly, e.g. into `v_array`. |
| `dgx_screen_flush(scr)` | Commit the pending dirty area now. |
| `dgx_screen_destroy(&scr)` | Destroy a screen allocated by a driver or virtual screen constructor. |

```c
dgx_screen_progress_up(scr);
dgx_fill_rectangle(scr, x, y, w, h, bg);
dgx_draw_line(scr, x1, y1, x2, y2, fg);
dgx_font_string_utf8_screen(scr, x, y, "42", fg, DgxOutputNormal, 1, font, NULL, NULL);
dgx_screen_progress_down(scr);   // one update_screen() for everything above
```

Guidelines:

- Most applications only need `progress_up`/`progress_down` around a group of
  drawing calls, or nothing at all.
- Always pair every `up` with one `down`; do not modify `in_progress` directly.
- If you write pixels behind the primitives' back, call `dgx_screen_touch()`
  for that area.

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
on any screen without touching the framebuffer layout. Handwritten text is
the exception: it has no orientation parameter and is always written left to
right, see [Known gaps](#known-gaps).

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
| `CONFIG_DGX_ENABLE_MORPH` | `matrix.c`, `morph*.c` | dot morphing; selects `VSCREEN` |

`dgx_lcd_init.c`, the drawing and font code, handwritten fonts included, and
everything in `src/fonts/` are always compiled. Fonts are picked up via a `file(GLOB)` on
`src/fonts/*.c`, so the linker keeps only the font objects your application
actually references. If you add or remove a font file, rerun CMake configure
once.

## Known gaps

A couple of practical limitations are worth knowing up front:

- **Pixel readback works only from RAM.** `get_pixel()` reads virtual screens
  and panels that draw into a virtual back screen first (ST7920, SSD1306,
  ST7565R). Direct panels (ILI9341, ST7789, ...) return 0: many SPI modules
  have no MISO line at all, and where RAMRD works it is far too slow. If you
  need to read what you drew, draw into a `vscreen` and push it with
  `dgx_vscreen_to_screen()`.

- **Virtual screens don't honor `dir_x`/`dir_y`/`swap_xy` for framebuffer
  access.** Those fields describe orientation metadata, but they do not
  rotate or mirror the stored pixel data.

- **Handwritten text goes left to right only, without smoothing.** There is
  no orientation parameter and the lines are not antialiased. The direction
  has a plain reason: I have never written right to left or top to bottom
  myself, have no handwritten fonts of that kind and do not know such
  languages, so there was nothing to build it on. Rotate a virtual screen on
  its way to the display if you need the text turned.

- **The handwritten font knows Latin with German letters and Russian.** That
  is all `font0ss` has. Other alphabets are to be drawn in the
  [hw-fonts](https://github.com/jef-sure/hw-fonts) editor by those who need
  them; contributions are welcome.

## Host tests

The platform-independent code (matrices, morphing, renderers, virtual and page
screens, fonts, textured quads, Bezier curves, handwritten fonts with their
writing and morphing) has host tests built with gcc against small ESP-IDF
stubs and run under ASan/UBSan:

```sh
make -C test/host
```

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
examples/morph_demo/     sequential CYD word-morphing demo
examples/glyph_morph_demo/  CYD demo morphing one large symbol into the next
examples/life_morph_demo/   CYD Game of Life with morphing generations
examples/flip_clock_demo/   CYD split-flap clock drawn with textured quads
examples/texture_demo/      CYD demo scaling, turning and wobbling a textured card
examples/hw_font_demo/      CYD demo writing a text by hand with a font of Bezier curves
examples/hw_morph_demo/     CYD demo morphing handwritten words one into another
test/host/               host tests with ESP-IDF stubs
docs/                    morphing articles and handwritten text guides (English and Russian),
                         the format of handwritten fonts (Russian)
Kconfig                  feature toggles
CMakeLists.txt           ESP-IDF component build
```

See [CHANGES.md](CHANGES.md) for the release history.

## License

Copyright (c) 2021-2026 Anton Petrusevich. See [LICENSE](LICENSE).
