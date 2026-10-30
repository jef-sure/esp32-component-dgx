# DGX

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/banner-dark.png">
  <img src="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/banner-light.png" width="800" alt="DGX: display graphics for microcontrollers, drawn by DGX itself">
</picture>

DGX is a small display graphics library for microcontrollers. I started writing
it years ago, back when most people reached for Adafruit GFX or TFT_eSPI, and
I wanted something shaped to my own taste: a clean split between bus, screen
and drawing code, no hidden global state, possibility to use several equal
screens simultaneously.

Today it lives as an ESP-IDF component with drivers for a handful of common
panels, a couple of RAM-backed virtual screens, a UTF-8 text renderer, and a
small offline tool for converting fonts. It drives the MIPI-DSI panels of an
ESP32-P4 without tearing, and puts a picture drawn small on a large display
scaled and smoothed on the way, as a game port needs. On top of that it
animates: dots that morph one glyph into another, and handwritten fonts
whose text is written stroke by stroke by a pen or turns into another text.

## Contents

- [Getting started](#getting-started)
- [A minimal example](#a-minimal-example)
- [Recipes](#recipes)
  - [Animation without flicker](#animation-without-flicker)
  - [A monochrome OLED](#a-monochrome-oled)
  - [Text in your own font](#text-in-your-own-font)
  - [Fonts made at build time](#fonts-made-at-build-time)
  - [A gauge](#a-gauge)
  - [A pie chart, arcs, a Wi-Fi icon](#a-pie-chart-arcs-a-wi-fi-icon)
  - [Scrolling text](#scrolling-text)
  - [A picture at any size and angle](#a-picture-at-any-size-and-angle)
  - [Pixel art made larger](#pixel-art-made-larger)
  - [A sprite or an icon from flash](#a-sprite-or-an-icon-from-flash)
  - [A palette, a fade and a flash](#a-palette-a-fade-and-a-flash)
  - [Translucent buttons over a picture](#translucent-buttons-over-a-picture)
  - [Morph one glyph into another](#morph-one-glyph-into-another)
  - [Handwritten text](#handwritten-text)
  - [Morph handwritten text](#morph-handwritten-text)
  - [A MIPI-DSI display without tearing](#a-mipi-dsi-display-without-tearing)
  - [Rotate the screen](#rotate-the-screen)
  - [Draw from several tasks](#draw-from-several-tasks)
  - [An Arduino sketch](#an-arduino-sketch)
- [Examples](#examples)
- [How fast](#how-fast)
- [Supported panels](#supported-panels)
- [What's in the box](#whats-in-the-box)
- [How it fits together](#how-it-fits-together)
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
- [Drawing from several tasks](#drawing-from-several-tasks)
- [Coordinates and orientation](#coordinates-and-orientation)
- [Build options](#build-options)
- [Known gaps](#known-gaps)
- [Host tests](#host-tests)
- [Repository layout](#repository-layout)
- [License](#license)

## Getting started

DGX is an ESP-IDF component, published in the
[ESP Component Registry](https://components.espressif.com/components/jef-sure/dgx)
as `jef-sure/dgx`. Add it to a project:

```sh
idf.py add-dependency "jef-sure/dgx^0.10.0"
```

or put it into `main/idf_component.yml` yourself:

```yaml
dependencies:
  jef-sure/dgx: "^0.10.0"
```

Then enable the drivers you need in `menuconfig`, under the **DGX** menu. A
driver is offered only once a transport it works on is enabled, so enable both;
in `sdkconfig.defaults` a driver option without its transport is silently
dropped, and the link fails later. Virtual screens, which most recipes below
draw into, need `CONFIG_DGX_ENABLE_VSCREEN`. To work on DGX itself, check the
repository out under `components/` instead; a git submodule at
`components/dgx` works well.

## A minimal example

Here's a complete setup for an ST7789 panel over SPI:

```c
#include "bus/dgx_spi_esp32.h"
#include "drivers/st7789.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/LiberationSansRegular12.h"

// 1. create a bus
dgx_bus_protocols_t *bus =
  dgx_spi_init(SPI3_HOST, SPI_DMA_CH_AUTO,
               GPIO_NUM_23, GPIO_NUM_19,
               GPIO_NUM_18, GPIO_NUM_5,
               GPIO_NUM_27, 40 * 1000 * 1000, 0);

// 2. create a screen on top of that bus
dgx_screen_t *scr = dgx_st7789_init(bus, GPIO_NUM_33, 16, DgxScreenRGB, ST7789_RESOLUTION_240x320);

// 3. draw something
dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height,
                   DGX_BLACK(dgx_rgb_to_16));
dgx_font_string_utf8_screen(scr, 10, 18, "Hello world",
                            DGX_WHITE(dgx_rgb_to_16),
                            DgxOutputNormal, 1, LiberationSansRegular12(),
                            NULL, NULL);
```

[examples/screen_demo](examples/screen_demo) is the same program in full,
for an ILI9341 on the usual pins of an ESP32.

## Recipes

Short answers to the things people look for first. Most recipes name a
complete example to run and read; the functions are listed in the
[API reference](#api-reference).

### Animation without flicker

Draw each frame into a virtual screen in RAM, then send the finished frame
to the panel in one go.

```c
#include "drivers/vscreen.h"

dgx_screen_t *frame = dgx_vscreen_init(scr->width, scr->height, 16, DgxScreenRGB);

for (int x = 0; x < scr->width; ++x) {
    dgx_fill_rectangle(frame, 0, 0, frame->width, frame->height, DGX_BLACK(dgx_rgb_to_16));
    dgx_solid_circle(frame, x, frame->height / 2, 12, DGX_CYAN(dgx_rgb_to_16));
    dgx_vscreen_to_screen(scr, 0, 0, frame, 0);
}
```

To send only what has changed, keep a batch open on the virtual screen and
ask it for the changed rectangle:

```c
dgx_screen_progress_up(frame);          // once: changes are collected while a batch is open

draw_something(frame);
int left, top, width, height;
if (dgx_screen_take_dirty(frame, &left, &top, &width, &height)) {
    dgx_vscreen_region_to_screen(scr, left, top, frame, left, top, width, height, 0);
}
```

Example: [examples/hw_soft_pen_demo](examples/hw_soft_pen_demo). More calls:
[Virtual screens](#virtual-screens-api).

### A monochrome OLED

The same drawing calls work. A color is on (non-zero) or off (zero), and the
driver sends its RAM buffer to the panel for you.

```c
#include "bus/dgx_i2c_esp32.h"
#include "drivers/ssd1306.h"
#include "fonts/TerminusTTFMedium12.h"

dgx_bus_protocols_t *bus = dgx_i2c_init(I2C_NUM_0, 0x3C, GPIO_NUM_21, GPIO_NUM_22, 400000);
dgx_screen_t *oled = dgx_ssd1306_init(bus, SSD1306_128X64, 0, GPIO_NUM_NC);

dgx_fill_rectangle(oled, 0, 0, oled->width, oled->height, 0);
dgx_font_string_utf8_screen(oled, 0, 12, "Hello OLED", 1, DgxOutputNormal, 1,
                            TerminusTTFMedium12(), NULL, NULL);
```

Every monochrome panel here is drawn into a virtual screen in RAM, and the
driver sends the panel what has changed: SSD1306, SSD1309, SH1106 and ST7565R
keep a page screen laid out as the controller wants its bytes, ST7920 an
ordinary virtual screen at 1 bit a pixel.
Outside a batch the changed rectangle goes to the panel after every
primitive, so a frame drawn in several calls can be seen half done.
Wrap the frame in a batch and it reaches the panel in one transfer, with no
flicker and no second screen of your own:

```c
dgx_screen_progress_up(oled);
draw_frame(oled);               // clear, text, figures: nothing is sent yet
dgx_screen_progress_down(oled); // the changed rectangle goes to the panel once
```

### Text in your own font

DGX comes with a few fonts, in `src/fonts/`:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/fonts-dark.png">
  <img src="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/fonts-light.png" width="700" alt="The bundled fonts, each line drawn in the font it names">
</picture>

When they are not enough, `font2c` converts a TTF or BDF font into a C
source/header pair. Give it the pixel size and the Unicode ranges you need:

```sh
cc font2c/font2c.c -o font2c/font2c $(pkg-config --cflags --libs freetype2)
./font2c/font2c path/to/YourFont.ttf 16 0x20 0x7e 0x410 0x44f
```

Move the `.c` file into `src/fonts/` and the `.h` file into
`include/fonts/`, and use the font like a bundled one; the next build picks
the new file up by itself:

```c
#include "fonts/YourFontRegular16.h"

dgx_font_string_utf8_screen(scr, 10, 30, "Привет", color, DgxOutputNormal, 1,
                            YourFontRegular16(), NULL, NULL);
```

Details: [font2c/README.md](font2c/README.md).

A glyph that comes out wrong on the panel is fixed by hand in the font
editor, [font-editor/index.html](font-editor/README.md): open the `.c`,
redraw the glyph and save the patch, `YourFontRegular16.patch.json`. Then
make the font again with it, `./font2c/font2c -p YourFontRegular16.patch.json
path/to/YourFont.ttf 16 ...`; a font made at build time finds its patch by
itself, see "Glyphs fixed by hand" in [docs/fonts-en.md](docs/fonts-en.md).

### Fonts made at build time

A program, and the components it uses, declare the fonts they need in
their CMakeLists files: the characters, the size and the face of each one.
DGX renders all of them while the program is built, one font per face and
size, and gives every need a function. Nothing is run by hand.

```cmake
# main/CMakeLists.txt, after idf_component_register()
dgx_fonts_board(PIXELS 320 240 MM 57.6 43.2)             # the display, for sizes in millimeters
dgx_fonts_rules(../fonts.rules)                          # which faces may be used at which sizes
dgx_font_need(body SIZE 3mm MONO TEXTS strings.txt)      # a monospaced face picked by the rules, with the characters of the strings
dgx_font_need(title SIZE 5mm POSTSCRIPT DejaVuSans-Bold TEXTS strings.txt)  # an exact face, 5 mm high
dgx_font_need(clock DGX DejaVuSansBook32 LETTERS "0123456789:")             # one of DGX's fonts, used as is; its characters are checked
```

```c
#include "dgx_fonts.h"

dgx_font_string_utf8_screen(scr, 10, 40, "Привет", ink, DgxOutputNormal, 1, dgx_font_body(), NULL, NULL);
```

A font is rendered by `font2c` on the build machine from a TTF or OTF, or
from any font file a need names, a BDF at a size it has, with the
characters of the need's texts and no others; an existing font must
already have them, or the build stops. The build machine needs a C compiler
with FreeType, and fontconfig; a project without needs requires nothing
extra. Needs, sizes, characters, the rules, needs from a component and what
the build produces: [docs/fonts-en.md](docs/fonts-en.md)
([Russian](docs/fonts-ru.md)). The example: [fonts_demo](examples/fonts_demo).

DGX does not check font licenses; you do. Each build lists every font the
program uses, with what its file says about its license, in
`build/dgx_fonts/fonts-used.txt`. A `deny` line in the rules keeps a font
out, even when a component asks for it by name; the rule `strict` admits
only the fonts the rules allow. See
[Fonts and their licenses](docs/fonts-en.md#fonts-and-their-licenses).

### A gauge

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/gauge-dark.png">
  <img src="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/gauge-light.png" width="480" alt="A gauge at three values">
</picture>

```c
#include "dgx_gauge.h"

static uint32_t gauge_color(int value) {
    return value < 70 ? DGX_GREEN(dgx_rgb_to_16) : DGX_RED(dgx_rgb_to_16);
}

dgx_gauge_t gauge;
dgx_gauge_init(&gauge, scr,
               scr->width / 2, scr->height / 2, // center
               40, 12,                          // inner radius, ring width
               135.0f, 270,                     // start angle in degrees, sweep clockwise
               0, 100,                          // value range
               DGX_DARKGREY(dgx_rgb_to_16),     // color of the inactive steps
               gauge_color);                    // draws the empty ring

dgx_gauge_set_value(&gauge, 42);                // redraws only the steps that changed
```

### A pie chart, arcs, a Wi-Fi icon

Angles are in degrees, counter-clockwise, and 0 points to the right. With
equal radii an ellipse is a circle.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/figures-dark.png">
  <img src="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/figures-light.png" width="700" alt="A pie chart, sectors, arcs drawn with pens of several widths and a Wi-Fi icon">
</picture>

```c
// a pie chart
dgx_solid_sector(scr, cx, cy, r, r, 0, 100, red);
dgx_solid_sector(scr, cx, cy, r, r, 100, 190, green);
dgx_solid_sector(scr, cx, cy, r, r, 190, 360, blue);
dgx_draw_ellipse(scr, cx, cy, r, r, white);     // the outline

// a Wi-Fi icon: a dot and three quarter-turn arcs, pen 3 pixels wide
dgx_solid_circle(scr, cx, cy, 2, white);
for (int i = 1; i <= 3; ++i) dgx_draw_arc(scr, cx, cy, i * 9, i * 9, 45, 135, 3, white);
```

Example: [examples/figures_scroll_demo](examples/figures_scroll_demo).

### Scrolling text

`dgx_scroll()` moves the contents of a rectangle and fills the freed strip.
The usual way is to scroll a virtual screen and send it to the display:

```c
// a terminal: everything moves up by one line, the new line is drawn at the bottom
dgx_scroll(page, 0, 0, page->width, page->height, 0, -line_height, paper);
dgx_font_string_utf8_screen(page, 4, page->height - 4, text, ink, DgxOutputNormal, 1,
                            font, NULL, NULL);
dgx_vscreen_to_screen(lcd, 0, 0, page, 0);
```

Some displays can be scrolled directly, without a virtual screen: an ILI9341
scrolls by its own hardware along the long side of the panel, an ST77916 along
y of a screen upright, and a MIPI-DSI display is scrolled in its frame buffer. The call is the same; it returns
`ESP_OK`, or `ESP_ERR_NOT_SUPPORTED` when the display cannot do that scroll:

```c
// an ILI9341 in portrait: the rows below a 24-row header move up, the header stays
dgx_scroll(lcd, 0, 24, lcd->width, lcd->height - 24, 0, -line_height, paper);
```

On such a panel a move costs its commands and the fill of the strip that
leaves. When the new line or column is drawn right after the
move anyway, use `dgx_scroll_no_fill()`: the same move without the fill, and
on the panel it is the commands alone.

In that case a virtual screen always works: scroll the virtual screen as
above and send it to the display. It can be sent in any
orientation, so the text scrolls the way you need whatever way the panel is
mounted:

```c
dgx_vscreen_to_screen(lcd, 0, 0, page, DGX_VSCREEN_ORIENTED(DgxOutputRotate90CW));
```

Example: [examples/figures_scroll_demo](examples/figures_scroll_demo), a
terminal and a chart on both kinds of display. The rules are under
[Drawing primitives](#drawing-primitives).

### A picture at any size and angle

Draw the picture once into a virtual screen, then put it on the display at
any size and onto any four corners.

```c
// the texture: the same color depth as the screen it goes to
dgx_screen_t *tex = dgx_vscreen_init(64, 24, 16, DgxScreenRGB);
dgx_fill_rectangle(tex, 0, 0, tex->width, tex->height, DGX_NAVY(dgx_rgb_to_16));
dgx_font_string_utf8_screen(tex, 6, 17, "DGX", DGX_WHITE(dgx_rgb_to_16),
                            DgxOutputNormal, 1, LiberationSansRegular12(), NULL, NULL);

// three times larger
dgx_draw_texture_rect(scr, 10, 10, 192, 72, tex, 0, 0, tex->width, tex->height);

// onto four corners: top-left, top-right, bottom-right, bottom-left
const dgx_point_2d_t card[4] = {{40, 110}, {200, 100}, {220, 170}, {20, 180}};
dgx_draw_texture_quad(scr, card, tex, 0, 0, tex->width, tex->height);

// mirrored: swap the left and the right corners
const dgx_point_2d_t mirrored[4] = {{237, 10}, {174, 10}, {174, 33}, {237, 33}};
dgx_draw_texture_quad(scr, mirrored, tex, 0, 0, tex->width, tex->height);
```

To rotate or animate the picture, compute the four corners again for every
frame. The quad must be convex; the mapping is affine, without perspective.
For a whole number of times larger, a quarter turn or a mirror image, the
flags of a transfer do the same pixel for pixel, faster and with smoothing:
see [Pixel art made larger](#pixel-art-made-larger).
Examples: [examples/texture_demo](examples/texture_demo) scales, rotates and
wobbles a card (`card_corners()`),
[examples/flip_clock_demo](examples/flip_clock_demo) folds the digits of a
split-flap clock.

### Pixel art made larger

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/pixel-scale-dark.png">
  <img src="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/pixel-scale-light.png" width="640" alt="A small picture three times as large: by squares, by Scale3x and by Scale3xSFX">
</picture>

A virtual screen can be put on a screen larger than it is: every pixel a
square of 2, 3 or more pixels, or smoothed at 2, 3 and 4, where the steps
of a slanted edge are filled in. The same flags turn the picture by a
quarter or mirror it and leave its pixels of value 0 out, on every
transfer:

```c
// a 160x120 frame of 8-bit indices on a 320x240 display, smoothed
dgx_vscreen8_to_screen16(lcd, 0, 0, frame, lut, DGX_VSCREEN_SCALE(2) | DGX_VSCREEN_SMOOTH);

// a 16-bit sprite into the 16-bit virtual screen of a scene:
// three times larger, mirrored, its pixels of value 0 left out
dgx_vscreen_to_screen(scene, x, y, sprite,
                      DGX_VSCREEN_SCALE(3) | DGX_VSCREEN_SMOOTH_SFX |
                      DGX_VSCREEN_ORIENTED(DgxOutputMirrorX) | DGX_VSCREEN_TRANSPARENT);
```

`DGX_VSCREEN_SMOOTH` is Scale2x at 2, Scale3x at 3 and Scale2x twice at 4:
a corner where two equal neighbours meet takes their pixel. Pixels are equal
when their values are, the indices and not their colors through a table.
`DGX_VSCREEN_SMOOTH_SFX` is Scale2xSFX and Scale3xSFX: a thin diagonal line
stays thin where Scale2x gives it lumps, and edges at 45 degrees come out
even at 3; it looks at more neighbours and does more work per pixel. A region
is smoothed as the whole picture is, so a frame may be sent by its changed
parts. Put a sprite with transparent pixels into the scene in RAM rather than
straight on a display behind a bus, which takes a transparent picture as runs
of pixels, a window each. Any other size or angle is a textured quad, see
[A picture at any size and angle](#a-picture-at-any-size-and-angle).
Example: [examples/pixel_scale_demo](examples/pixel_scale_demo) shows every
way in turn, with the time each takes.

### A sprite or an icon from flash

Keep small pictures in flash as indexed pixels of 1, 4 or 8 bits and draw
them into an 8-bit virtual screen, then show it through a LUT. Pixel value 0
is transparent. A 4-bit picture gets its colors from a map of 15 indices, so
the same data in other colors only needs another map.

```c
#include "dgx_image.h"

// a gem of 4 x 3 pixels of 4 bits, two pixels a byte, the left one in the high half
static const uint8_t gem_data[] = {0x01, 0x10, 0x12, 0x21, 0x01, 0x10};
static const dgx_image_t gem = {
    .width = 4, .height = 3,
    .origin_x = 0, .origin_y = 1, // the pixel put at (x, y)
    .format = DGX_IMAGE_4BPP, .data = gem_data,
};

static const uint8_t red[15] = {10, 11};   // value 1 -> index 10, value 2 -> 11
dgx_image_blit(frame, 100, 50, &gem, &(dgx_image_blit_t){.map = red});

// mirrored around its origin, and only inside a rectangle of the frame
dgx_image_blit(frame, 100, 80, &gem, &(dgx_image_blit_t){
    .map = red, .orientation = DgxOutputMirrorX,
    .flags = DGX_IMAGE_CLIP, .clip_x = 0, .clip_y = 0, .clip_w = 120, .clip_h = 240,
});

// every visible pixel in one color: a shadow or a flash
dgx_image_blit(frame, 102, 52, &gem, &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE, .color = 1});

dgx_vscreen8_to_screen16(lcd, 0, 0, frame, lut, 0);
```

The picture is mirrored and rotated around its origin, so a sprite that
turns round stays where it stands. A 1-bit picture is drawn in `color`; an
8-bit one uses its values as indices, or a map of 255. Unlike
`dgx_vscreen_to_screen()`, the source is constant data rather than a
screen, it is clipped, and it can be recolored; mirrored or not, it costs
the same.
Example: [examples/sprite_palette_demo](examples/sprite_palette_demo).

### A palette, a fade and a flash

An 8-bit virtual screen holds indices; its table of 256 RGB565 colors is
used only when the frame goes to the display. Change the table and the whole
frame changes color, without redrawing a pixel. `dgx_lut.h` fills the table:

```c
#include "dgx_bits.h"   // dgx_rgb_to_24()
#include "dgx_colors.h" // DGX_SKYBLUE()
#include "dgx_lut.h"

static uint16_t lut[256];
// the palette kept as RGB888, here in flash: entry k goes to lut[16 + k]
static const uint8_t level[3][3] = {{20, 20, 40}, {90, 60, 30}, {200, 180, 120}};

dgx_lut_gradient(lut, 0, 16, 0x000000, DGX_SKYBLUE(dgx_rgb_to_24)); // a ramp in 0..15
dgx_lut_from_rgb(lut, 16, 3, level);                               // the palette in 16..18
dgx_vscreen8_to_screen16(lcd, 0, 0, frame, lut, 0);

// fade the palette to black in 16 steps: each step is mixed from RGB888 again
for (int step = 1; step <= 16; ++step) {
    dgx_lut_mix_color(lut, 16, 3, level, 0x000000, (uint8_t)(step * 255 / 16));
    dgx_vscreen8_to_screen16(lcd, 0, 0, frame, lut, 0);
}

// a flash: halfway to white for one frame, then back
dgx_lut_mix_color(lut, 16, 3, level, 0xffffff, 128);
```

`dgx_lut_mix()` mixes two palettes the same way, to change one into another.
The colors are mixed from the RGB888 palette every time, so a fade in many
steps does not gather rounding errors. A color given as one number is
`0xRRGGBB`, as `dgx_rgb_to_24()` and `DGX_*(dgx_rgb_to_24)` make it. The
table of a soft pen from the paper to the ink is
`dgx_lut_gradient(lut, 0, 256, paper, ink)`.
Example: [examples/sprite_palette_demo](examples/sprite_palette_demo).

### Translucent buttons over a picture

A screen can have one overlay layer: an 8-bit virtual screen that lies over
a part of it, with its own colors and one transparency for all of them.
Index 0 is empty. Whatever goes to the screen from a virtual screen is mixed
with the layer on the way, so the picture shows through the buttons.

```c
dgx_screen_t *buttons = dgx_vscreen_init(1280, 800, 8, DgxScreenRGB);
dgx_solid_circle(buttons, 120, 680, 60, 1);                  // index 1
dgx_overlay_set(display, buttons, 0, 0, button_lut, 160);    // 160 of 255: mostly the button

// every frame: the places the layer changed are put again from the picture, a 320x200 frame shown x4 at (0, 0)
int x, y, w, h;
if (dgx_overlay_take_dirty(display, &x, &y, &w, &h)) {
    const int sx = x / 4, sy = y / 4;   // the pixels of the frame that cover the place
    dgx_vscreen8_region_to_screen16(display, 4 * sx, 4 * sy, frame, sx, sy, (x + w + 3) / 4 - sx, (y + h + 3) / 4 - sy, lut,
                                    DGX_VSCREEN_SCALE(4) | DGX_VSCREEN_SMOOTH);
}
```

The screen cannot redraw what lies under a button by itself, so drawing
into the layer, moving it or taking it away only marks the place; whoever
owns the picture puts that place again. Lines, texts and fills drawn
straight on the screen are not mixed. The drawing into the layer and the
output to the screen belong to one task.

### Morph one glyph into another

A glyph becomes a matrix of dots, and the dots of one glyph fly to the
places of the other. Build with `CONFIG_DGX_ENABLE_MORPH`.

```c
#include "dgx_morph.h"
#include "dgx_morph_render.h"
#include "dgx_morph_sources.h"

const int cell = 8, margin = cell;      // a dot is 8 pixels; its glow needs a margin around the glyph

// both glyphs as dot matrices on the same grid
dgx_bit_matrix_t *from = dgx_morph_glyph_matrix(font, '1');
dgx_bit_matrix_t *to = dgx_morph_glyph_matrix(font, '8');

// the plan: which dot flies where
dgx_morph_t *morph = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);

// a renderer that draws glowing dots into a buffer of its own
const int width = from->width * cell + 2 * margin, height = from->height * cell + 2 * margin;
dgx_morph_glow_t *glow = dgx_morph_glow_create(width, height, cell, scr->color_bits);

// the frames: half a second from the first glyph to the second
const int64_t start = esp_timer_get_time();
float         t;
do {
    t = dgx_morph_progress(start, esp_timer_get_time(), 500000);
    dgx_morph_draw(morph, t, margin, margin, cell, true, dgx_morph_glow_dot, glow);
    dgx_morph_glow_present(glow, t, scr, x, y);
    vTaskDelay(1);
} while (t < 1.0f);

dgx_morph_glow_destroy(&glow);
dgx_morph_destroy(&morph);
dgx_matrix_destroy(&from);
dgx_matrix_destroy(&to);
```

In a dot font whose dots are listed in an order meant for morphing, such as
`CasusDotView`, a glyph can also morph dot by dot: dot i of one glyph flies
to dot i of the other, and the dots one glyph has more than the other go into
its last dot or come out of it. Such a plan is made in pixels and drawn by the
same renderers:

```c
// both glyphs at (x, y) of the glow's frame, a dot of cell x cell pixels
dgx_font_symbol_morph_t *digit = dgx_font_make_morph_struct(CasusDotView(), '0', '2', x, y, x, y, cell);

dgx_morph_symbol_draw(digit, dgx_morph_ease(DGX_MORPH_SMOOTHSTEP, t), dgx_morph_glow_dot, glow);
dgx_morph_glow_present(glow, t, scr, left, top);

dgx_font_make_morph_struct_destroy(&digit);
```

Examples: [examples/glyph_morph_demo](examples/glyph_morph_demo) (one large
symbol, sized to the screen), [examples/morph_demo](examples/morph_demo)
(words, letter by letter),
[examples/weather_morph_demo](examples/weather_morph_demo) and
[examples/life_morph_demo](examples/life_morph_demo). How the plan is made,
the other renderers and your own patterns in place of glyphs are in
[docs/morphing-en.md](docs/morphing-en.md)
([Russian](docs/morphing-ru.md)).

### Handwritten text

A handwritten font stores every symbol as pen strokes on a grid. `scale` is
the number of pixels per grid cell: a line of `font0ss` is 202 cells high, so
0.25 gives a line of about 50 pixels. The pen width is in pixels.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/handwriting-dark.png">
  <img src="https://raw.githubusercontent.com/jef-sure/esp32-component-dgx/main/docs/images/handwriting-light.png" width="560" alt="A handwritten line, finished and half written">
</picture>

```c
#include "dgx_hw_font.h"
#include "fonts/font0ss.h"

dgx_font_t *font = font0ss();

// x is the left end of the line, y its base line;     scale  pen         joined letters
dgx_hw_draw_text(scr, 10, 80, font, "Hello, World", 0.25f, 3, color, true);
```

To fit a text to the screen, ask for its box in grid cells. Columns count
from the start of the line and rows from the base line, so `top` is negative:

```c
int left, top, right, bottom;
dgx_hw_text_box(font, text, &left, &top, &right, &bottom);
const float cells_wide = right - left + 1, cells_high = bottom - top + 1;

// the largest scale that fits the width; the pen adds half its width on each side
const float scale = (scr->width - 2 * margin - pen) / cells_wide;
// the box in the middle of the screen
const int x = (int)lroundf((scr->width - cells_wide * scale) / 2 - left * scale);
const int y = (int)lroundf((scr->height - cells_high * scale) / 2 - top * scale);

dgx_hw_draw_text(scr, x, y, font, text, scale, pen, color, true);
```

To write it stroke by stroke, begin a writing and draw it on as time goes.
Effort is the distance the pen has travelled, in grid cells:

```c
dgx_hw_pace_t    pace = dgx_hw_pace(font, 6);       // the pen slows down in bends; 0 for an even speed
dgx_hw_writing_t writing;
dgx_hw_writing_begin(&writing, scr, x, y, font, text, scale, pen, color, true, &pace);

const float   tempo = 1500;                         // grid cells per second
const int64_t start = esp_timer_get_time();
while (!dgx_hw_writing_draw_to(&writing, tempo * (esp_timer_get_time() - start) * 1e-6f)) {
    vTaskDelay(1);
}
```

For a soft (anti-aliased) pen edge pass `DGX_PEN_SOFT | pen` and draw into
an 8-bit virtual screen, where `color` is an amount of ink up to 255; send
it to the display through a color table that runs from the paper color to
the ink color.

Examples: [examples/hw_font_demo](examples/hw_font_demo),
[examples/hw_soft_pen_demo](examples/hw_soft_pen_demo). Details:
[docs/handwriting-en.md](docs/handwriting-en.md)
([Russian](docs/handwriting-ru.md)).

### Morph handwritten text

Every stroke is a Bezier curve, so one text can turn into another. Each frame
is a new picture: draw it into a virtual screen and send that to the display.

```c
// the band holds amounts of ink, 0 to 255; lut[] holds the screen colors for them
dgx_screen_t *band = dgx_vscreen_init(scr->width, 100, 8, DgxScreenRGB);
dgx_hw_morph_t *morph = dgx_hw_morph_create(font0ss(), "hello", "world", true);

const int64_t start = esp_timer_get_time();
float         t;
do {
    t = (esp_timer_get_time() - start) / 1500000.0f;   // 1.5 s
    if (t > 1) t = 1;
    dgx_fill_rectangle(band, 0, 0, band->width, band->height, 0);
    dgx_hw_morph_draw(morph, t * t * (3 - 2 * t), band, 10, 70, 0.4f, 4, 255);
    dgx_vscreen8_to_screen16(scr, 0, 70, band, lut, 0);
    vTaskDelay(1);
} while (t < 1);

dgx_hw_morph_destroy(&morph);
```

`dgx_hw_morph_text_create()` morphs letter by letter instead, each letter
with a progress of its own, so the letters that stay the same do not move:
a clock, for one. Example: [examples/hw_morph_demo](examples/hw_morph_demo).

### A MIPI-DSI display without tearing

Ask for two frame buffers and put everything that belongs to one picture
into one batch. The batch is shown as a whole on the next frame.

```c
#include "bus/dgx_dsi_esp32.h"
#include "drivers/jd9165.h"

dgx_bus_protocols_t *bus = dgx_dsi_init(0, 2, 750, 3, 2500);
dgx_screen_t *lcd = dgx_jd9165_init(bus, GPIO_NUM_0, 16, DgxScreenRGB, DgxDsiTwoFrames);

dgx_screen_progress_up(lcd);
draw_picture(lcd);
dgx_screen_progress_down(lcd);
```

Example: [examples/dsi_demo](examples/dsi_demo). The other update modes, an
even frame rate and board settings:
[docs/dsi-usage-en.md](docs/dsi-usage-en.md)
([Russian](docs/dsi-usage-ru.md)).

### Rotate the screen

Every color driver has an orientation call. `swap_xy` exchanges the axes,
and the width and height of the screen with them.

```c
dgx_ili9341_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true);   // landscape on a CYD
```

A virtual screen is not turned; it is put on the display turned, with
`DGX_VSCREEN_ORIENTED(o)` among the flags of the transfer, see
[Scrolling text](#scrolling-text).

See [Coordinates and orientation](#coordinates-and-orientation).

### Draw from several tasks

A screen takes one task at a time. Let one task own the display and draw,
and let the others send it messages or finished virtual screens; or guard
each batch with a mutex of your own. Both ways are in
[Drawing from several tasks](#drawing-from-several-tasks).

### An Arduino sketch

DGX is an ESP-IDF component, and the Arduino-ESP32 core is itself a layer
on top of ESP-IDF, so a sketch can call both APIs in one file. The sketch
stays an ordinary one — `setup()`, `loop()`, `millis()`, `delay()`,
`Serial` — while the project around it is an ESP-IDF project with Arduino
as a component, and DGX keeps its `menuconfig` configuration. What cannot
build it is the Arduino IDE, and PlatformIO with `framework = arduino`
alone: those use a precompiled ESP-IDF with a fixed configuration and never
run the CMake and Kconfig DGX brings. Two tools do:

- the **ESP-IDF tools**: `idf.py` on the command line, or the ESP-IDF
  extension of VS Code — the steps below;
- **PlatformIO** with `framework = espidf, arduino`: the same CMake build
  behind a `platformio.ini` — see the end of this section.

With the ESP-IDF tools, add both libraries to `main/idf_component.yml`:

```yaml
dependencies:
  espressif/arduino-esp32: "^3.3.0"
  jef-sure/dgx: "*"
```

Arduino wants a 1 ms tick, and calls `setup()` and `loop()` from its own
`app_main()` when autostart is enabled. Put both into `sdkconfig.defaults`
with the DGX drivers you need:

```
CONFIG_FREERTOS_HZ=1000
CONFIG_AUTOSTART_ARDUINO=y
CONFIG_DGX_ENABLE_SPI=y
CONFIG_DGX_ENABLE_SPI_ILI_9341=y
```

The sketch is `main/main.cpp`, listed in `main/CMakeLists.txt` as
`idf_component_register(SRCS "main.cpp")`. The DGX headers are C++-ready
and are included as they are. C++ does not convert an `int` to an enum
implicitly, so pins are given as `GPIO_NUM_13`, not `13`. The display of a
CYD is on `SPI2_HOST`; Arduino's `SPI` keeps `SPI3_HOST`, where the CYD has
its SD card:

```cpp
#include <Arduino.h>
#include "bus/dgx_spi_esp32.h"
#include "drivers/ili9341.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/DejaVuSansBook32.h"

static dgx_screen_t *screen;

void setup()
{
    dgx_bus_protocols_t *bus = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_13, GPIO_NUM_12,
                                            GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_2, 40 * 1000 * 1000, 0);
    screen = dgx_ili9341_init(bus, GPIO_NUM_NC, GPIO_NUM_21, 16, DgxScreenRGB);
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(dgx_rgb_to_16));
}

void loop()
{
    char text[16];
    snprintf(text, sizeof text, "%lu s", (unsigned long)(millis() / 1000));
    dgx_fill_rectangle(screen, 0, 0, screen->width, 40, DGX_BLACK(dgx_rgb_to_16));
    dgx_font_string_utf8_screen(screen, 10, 34, text, DGX_WHITE(dgx_rgb_to_16), DgxOutputNormal, 1,
                                DejaVuSansBook32(), NULL, NULL);
    delay(1000);
}
```

Two Arduino libraries meet DGX on the hardware:

- `SPI`, and libraries built on it such as `SD`, drive their SPI host
  directly, past the ESP-IDF driver DGX uses. The global `SPI` object takes
  `SPI3_HOST` on an ESP32 and `SPI2_HOST` on the other chips. Give the
  display a host of its own, or give the library the other one with
  `SPIClass(HSPI)`.
- `Wire` runs on the ESP-IDF I2C master driver, so a display can share its
  bus with the sensors of the sketch (ESP-IDF 5.4 or newer):

  ```cpp
  Wire.begin(21, 22, 400000);
  auto i2c = (i2c_master_bus_handle_t)i2cBusHandle(0);
  dgx_screen_t *oled = dgx_ssd1306_init(dgx_i2c_init_on_bus(i2c, 0x3C, 400000), SSD1306_128X64, 0, GPIO_NUM_NC);
  ```

Everything else in this README, fonts made at build time included, works in
a sketch unchanged.

PlatformIO builds the same project for those who already use it:
`framework = espidf, arduino` in `platformio.ini` runs the same CMake
build, `sdkconfig.defaults` keeps working, and `pio run -t menuconfig`
opens the same configuration. Take the layout of the platform's
`espidf-arduino-blink` example, with the sketch under `src/`, and:

```ini
[env:cyd]
platform = espressif32
framework = espidf, arduino
board = esp32dev
```

Arduino comes from the platform package in PlatformIO, so it is not a
dependency there; `jef-sure/dgx`, named in an `idf_component.yml` next to
the sketch, is the only one to add.

Example: [examples/arduino_demo](examples/arduino_demo).

## Examples

Each example is an ESP-IDF project with its own README. Each names
`jef-sure/dgx` in `main/idf_component.yml` with an `override_path` to the
repository it sits in, so built from a checkout it takes that DGX; ESP-IDF 6
warns that the name differs unless the checkout is a directory called
`dgx`. A copy of an example elsewhere, without the `override_path` line,
takes DGX from the ESP Component Registry. From a checkout:

```sh
cd examples/screen_demo
idf.py set-target esp32
idf.py flash monitor
```

| Example | Board | What it shows |
| --- | --- | --- |
| [screen_demo](examples/screen_demo) | ESP32 with an ILI9341 | a graphics test: fills, text, lines, rectangles, circles, triangles |
| [arduino_demo](examples/arduino_demo) | CYD | DGX in an Arduino sketch: `setup()`, `loop()` and Arduino as an ESP-IDF component |
| [fonts_demo](examples/fonts_demo) | CYD | fonts made at build time: a title in millimeters, a paragraph in a face picked by rules, a clock in a bundled font |
| [dsi_demo](examples/dsi_demo) | JC1060P470 or JC8012P4A1C (ESP32-P4, 7-inch or 10.1-inch MIPI-DSI) | the same kind of test for a DSI display, update modes, an even frame rate, the board measured and the numbers kept |
| [figures_scroll_demo](examples/figures_scroll_demo) | CYD or JC1060P470 | a pie chart, arcs, a Wi-Fi icon, a scrolling terminal and chart; prints timings |
| [graphicstest](examples/graphicstest) | CYD, JC4827W543 or JC1060P470 | the PDQ graphics test of Adafruit_GFX, TFT_eSPI and LovyanGFX made with DGX, its table in the log and on the screen; see [How fast](#how-fast) |
| [texture_demo](examples/texture_demo) | CYD | a textured card scaled, rotated and wobbled |
| [sprite_palette_demo](examples/sprite_palette_demo) | CYD | sprites of 1, 4 and 8 bits in maps of colors, mirrored, turned and clipped; day, night, lightning and a fade by the color table alone |
| [pixel_scale_demo](examples/pixel_scale_demo) | CYD, JC1060P470 or JC8012P4A1C | a small picture two, three and four times as large by squares, Scale2x and Scale3x, Scale2xSFX and Scale3xSFX, each timed |
| [flip_clock_demo](examples/flip_clock_demo) | CYD | a split-flap clock built on textured quads |
| [morph_demo](examples/morph_demo) | CYD | words morphing letter by letter |
| [glyph_morph_demo](examples/glyph_morph_demo) | CYD | one large symbol morphing into the next |
| [weather_morph_demo](examples/weather_morph_demo) | CYD | the weather symbols of a clock morphing in the order weather takes |
| [life_morph_demo](examples/life_morph_demo) | CYD | Conway's Game of Life with morphing generations |
| [hw_font_demo](examples/hw_font_demo) | CYD | a text written by hand, stroke by stroke |
| [hw_morph_demo](examples/hw_morph_demo) | CYD | handwritten words morphing one into another |
| [hw_soft_pen_demo](examples/hw_soft_pen_demo) | CYD or JC1060P470 | handwriting with a soft pen, lines that move to make room |

A CYD is an ESP32-2432S028R: an ESP32 with a 320x240 ILI9341 on SPI.

## How fast

The PDQ graphics test that Adafruit_GFX, TFT_eSPI and LovyanGFX ship, with
each library on the same CYD: an ESP32 at 240 MHz, the ILI9341 on SPI at
40 MHz, upright at 240x320. DGX runs it as
[examples/graphicstest](examples/graphicstest); the others run their own
sketch, built with the Arduino core. A time ends when the display has the
last pixel.

PDQ graphics test on a CYD, every call drawing straight on the display,
microseconds:

| Test | DGX | TFT_eSPI 2.5.43 | LovyanGFX 1.2.7 | Adafruit_ILI9341 1.6.2 |
| --- | ---: | ---: | ---: | ---: |
| Hackaday logo, 16 times | 533 959 | 511 150 | 504 381 | 657 095 |
| Screen fill | 31 287 | 31 183 | 30 816 | 40 037 |
| Text | 22 833 | 15 283 | 10 172 | 33 025 |
| Pixels | 650 896 | 224 013 | 226 978 | 583 127 |
| Lines | 599 548 | 266 229 | 258 198 | 1 000 627 |
| Horiz/Vert Lines | 17 053 | 14 376 | 12 594 | 18 297 |
| Rectangles (outline) | 12 300 | 8 905 | 8 097 | 11 990 |
| Rectangles (filled) | 327 554 | 324 090 | 318 123 | 416 103 |
| Circles (filled) | 66 462 | 46 696 | 40 792 | 73 377 |
| Circles (outline) | 50 385 | 26 350 | 23 097 | 112 500 |
| Triangles (outline) | 34 537 | 16 672 | 15 539 | 55 918 |
| Triangles (filled) | 164 448 | 112 404 | 106 946 | 154 952 |
| Rounded rects (outline) | 23 683 | 16 572 | 12 467 | 46 627 |
| Rounded rects (filled) | 346 428 | 326 820 | 322 011 | 423 124 |
| Total | 2 881 373 | 1 940 743 | 1 890 211 | 3 626 799 |

Fills and pictures take as long as the wire takes, in DGX, TFT_eSPI and
LovyanGFX. Small figures are slower in DGX, and that is a deliberate trade:
every drawing goes through the functions of a screen and a bus, so the same
code draws on a virtual screen and on SPI, QSPI and MIPI-DSI displays. DGX
gave up speed in primitives to keep that flexibility.

No speed removes flicker anyway. What is drawn on the panel piece by piece
is seen piece by piece, however fast the bus. The only way to a picture
without flicker is to prepare it first and send it whole, as a virtual
screen or a ready image, which is why a DGX program works that way: see
[Animation without flicker](#animation-without-flicker).

Here is the same test run that way on the same CYD. Each test draws into an
8-bit virtual screen the size of the display (for TFT_eSPI, an 8-bit
sprite), and the whole of it is then sent to the display once. Each time
covers both, the drawing and that one send. The send alone takes 32 ms in
DGX and 39 ms in TFT_eSPI, so in most rows it is nearly the whole number.

PDQ graphics test on a CYD, drawn in memory and sent once, microseconds:

| Test | DGX, virtual screen | TFT_eSPI, sprite |
| --- | ---: | ---: |
| Hackaday logo, 16 times | 48 668 | 327 052 |
| Screen fill | 32 330 | 37 860 |
| Text | 36 425 | 40 720 |
| Pixels | 92 062 | 79 425 |
| Lines | 74 041 | 71 186 |
| Horiz/Vert Lines | 32 387 | 41 099 |
| Rectangles (outline) | 32 409 | 39 846 |
| Rectangles (filled) | 34 979 | 41 337 |
| Circles (filled) | 35 602 | 40 448 |
| Circles (outline) | 36 246 | 41 857 |
| Triangles (outline) | 34 648 | 40 419 |
| Triangles (filled) | 35 335 | 40 826 |
| Rounded rects (outline) | 33 476 | 40 788 |
| Rounded rects (filled) | 35 479 | 40 597 |
| Total | 594 087 | 923 460 |

The same test with DGX on the next chips and displays:

| Board | Chip | Display | Bus | Pixels |
| --- | --- | --- | --- | ---: |
| CYD | ESP32, 240 MHz | ILI9341, 240x320 | SPI, 40 MHz | 76 800 |
| Guition JC4827W543 | ESP32-S3, 240 MHz | NV3041A, 272x480 | QSPI, 40 MHz | 130 560 |
| Guition JC1060P470 | ESP32-P4, 360 MHz | 7-inch JD9165, 600x1024 | MIPI-DSI, 2 lanes | 614 400 |

PDQ graphics test with DGX on the three boards, microseconds. In the
"display" columns every call draws straight on the display; in the "memory"
columns each test draws into an 8-bit virtual screen, which is then sent once:

| Test | CYD 240x320, display | CYD 240x320, memory | JC4827W543 272x480, display | JC4827W543 272x480, memory | JC1060P470 600x1024, display | JC1060P470 600x1024, memory |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Hackaday logo, 16 times | 533 959 | 48 668 | 163 874 | 29 164 | 1 734 100 | 41 594 |
| Screen fill | 31 287 | 32 330 | 13 371 | 14 078 | 3 547 | 38 685 |
| Text | 22 833 | 36 425 | 19 878 | 17 388 | 4 936 | 33 503 |
| Pixels | 650 896 | 92 062 | 1 013 447 | 99 832 | 3 295 808 | 191 330 |
| Lines | 599 548 | 74 041 | 988 612 | 72 571 | 632 674 | 238 744 |
| Horiz/Vert Lines | 17 053 | 32 387 | 11 876 | 14 136 | 60 804 | 43 367 |
| Rectangles (outline) | 12 300 | 32 409 | 8 325 | 14 042 | 37 756 | 34 357 |
| Rectangles (filled) | 327 554 | 34 979 | 120 982 | 17 416 | 77 071 | 176 725 |
| Circles (filled) | 66 462 | 35 602 | 80 046 | 18 964 | 60 266 | 51 517 |
| Circles (outline) | 50 385 | 36 246 | 81 253 | 19 429 | 65 880 | 50 297 |
| Triangles (outline) | 34 537 | 34 648 | 41 273 | 16 361 | 39 623 | 39 602 |
| Triangles (filled) | 164 448 | 35 335 | 135 978 | 17 331 | 1 935 747 | 67 570 |
| Rounded rects (outline) | 23 683 | 33 476 | 23 767 | 15 236 | 49 920 | 37 098 |
| Rounded rects (filled) | 346 428 | 35 479 | 154 621 | 17 923 | 637 672 | 189 094 |
| Total | 2 881 373 | 594 087 | 2 857 303 | 383 871 | 8 635 804 | 1 233 483 |

Each board draws at the size of its own screen, so a row shows what that
board does, not one picture drawn three times; only the logo is 240x320
everywhere. The memory rows that draw least show what sending the whole
screen takes: about 32 ms on the CYD, 14 ms on the JC4827W543 with 1.7 times
the pixels, and 33 ms on the JC1060P470 with eight times the pixels.

The display of the JC1060P470 is memory in PSRAM, and outside a batch every
drawing writes the cache of the processor back over the rows it changed.
Its panel is 1024x600 and the test runs it upright, so neighbours in a row of
the screen lie 2 KB apart in the frame buffer, each on a cache line of its own:
that is the 1.7 s of the logo. So figures drawn straight on it take long, and
a program there draws in memory: the test takes 8.6 s on the display and 1.2 s
in memory.

How it was measured, the other boards, and the numbers published for these
libraries: [docs/benchmarks-en.md](docs/benchmarks-en.md).

## Supported panels

Each driver returns a `dgx_screen_t *` you can draw on. Color panels take a
pixel depth and a channel order; monochrome panels keep a RAM buffer that DGX
flushes for you. Use `GPIO_NUM_NC` for any reset/CS line your board does not
wire.

| Controller | Type | Buses | Constructor | Kconfig option |
| --- | --- | --- | --- | --- |
| ST7735 | color TFT, 128x160 with no memory offset | SPI, P8 | `dgx_st7735_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_ST7735` |
| ST7789 | color TFT | SPI, P8 | `dgx_st7789_init(bus, rst, color_bits, cbo, resolution)` | `CONFIG_DGX_ENABLE_SPI_ST7789` |
| ILI9341 | color TFT | SPI, P8 | `dgx_ili9341_init(bus, rst, backlight, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_ILI_9341` |
| NV3041A | color TFT, 480x272 | QSPI | `dgx_nv3041a_init(bus, rst, backlight, color_bits, cbo)` | `CONFIG_DGX_ENABLE_NV3041A` |
| ST77916 | round color TFT, 360x360 | QSPI, SPI | `dgx_st77916_init(bus, rst, backlight, color_bits, cbo)` | `CONFIG_DGX_ENABLE_ST77916` |
| GC9A01 | round color TFT | SPI, P8 | `dgx_gc9a01_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_GC9A01` |
| SSD1351 | color OLED | SPI, P8 | `dgx_ssd1351_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SSD1351` |
| SSD1306 | mono OLED | SPI, I2C | `dgx_ssd1306_init(bus, resolution, is_ext_vcc, rst)` | `CONFIG_DGX_ENABLE_SSD1306` |
| SSD1309 | mono OLED | SPI, I2C | `dgx_ssd1309_init(bus, resolution, rst)` | `CONFIG_DGX_ENABLE_SSD1309` |
| SH1106 | mono OLED | SPI, I2C | `dgx_sh1106_init(bus, resolution, rst)` | `CONFIG_DGX_ENABLE_SH1106` |
| ST7565R | mono LCD | SPI | `dgx_st7565r_init(bus, rst)` | `CONFIG_DGX_ENABLE_ST7565R` |
| ST7920 | mono LCD | SPI | `dgx_st7920_init(bus, rst, cs)` | `CONFIG_DGX_ENABLE_ST7920` |
| JD9165 | color TFT, 1024x600 | DSI | `dgx_jd9165_init(bus, rst, color_bits, cbo, frames)` | `CONFIG_DGX_ENABLE_JD9165` |
| JD9365 | color TFT, 800x1280 | DSI | `dgx_jd9365_init(bus, rst, color_bits, cbo, frames)` | `CONFIG_DGX_ENABLE_JD9365` |

Common parameters:

- `bus` — a `dgx_bus_protocols_t *` from `dgx_spi_init()`, `dgx_qspi_init()`,
  `dgx_i2c_init()`, `dgx_p8_init()` or `dgx_dsi_init()`.
- `rst` — reset GPIO, or `GPIO_NUM_NC` if the panel has no reset line.
- `backlight` (ILI9341, NV3041A and ST77916) — backlight GPIO, or `GPIO_NUM_NC` if it is not
  under software control; their backlight calls then return
  `ESP_ERR_NOT_SUPPORTED`.
- `color_bits` — pixel depth; `16` (RGB565) is the usual choice. ST7735,
  ST7789 and GC9A01 take `12`, `16` or `18`; ILI9341 and SSD1351 `16` or
  `18`; ST77916 `16` or `18`; NV3041A `16`; JD9165 and JD9365 `16` or `24`. A driver given a depth
  it cannot set up logs an error and returns NULL. For `18` see
  [docs/18-bit-pixels-en.md](docs/18-bit-pixels-en.md)
  ([Russian](docs/18-bit-pixels-ru.md)).
- `cbo` — channel order, `DgxScreenRGB` or `DgxScreenBGR`: the order colors
  come out in on the usual modules of the controller. The usual ILI9341
  modules show red as red with the BGR bit of the controller set, so there
  `DgxScreenRGB` sets that bit, the other way round from the other drivers.
  If red and blue are exchanged on your module, pass the other value.
- `resolution` (ST7789) — the panel: `ST7789_RESOLUTION_240x240`,
  `ST7789_RESOLUTION_240x320`, or `ST7789_RESOLUTION_320x240` for a 240x320
  panel in landscape. On a 240x320 panel, swapping the axes in
  `dgx_st7789_orientation()` makes the screen 320x240.

Color panels and ST7920 also expose an orientation setter
(`dgx_<driver>_orientation(scr, dir_x, dir_y, swap_xy)`). A color controller
is told its scan direction. An ST7920 has no command for that: its driver
turns the picture where it sends it to the panel, and with `swap_xy` the
screen is 64x128. A few drivers add extras: ILI9341 has
`dgx_ili9341_backlight()`, NV3041A and ST77916 have `dgx_nv3041a_backlight()` and `dgx_st77916_backlight()`,
GC9A01 has `dgx_gc9a01_display_off()` /
`dgx_gc9a01_display_on()`, SSD1351 has `dgx_ssd1351_brightness()`, SSD1306
has `dgx_ssd1306_contrast()`, SSD1309 has `dgx_ssd1309_contrast()` and
`dgx_ssd1309_column_offset()`, and SH1106 the same two under its own name.

JD9165 is a MIPI-DSI display for an ESP32-P4 with PSRAM. The chip keeps a
frame buffer for it, 1.2 MB at 16 bits per pixel for 1024x600, and drawing
changes that buffer, so a pixel can be read back and all eight orientations
work. The driver has the init commands of the 7-inch panel of the Guition
JC1060P470 board. JD9365 works the same way: an 800x1280 portrait panel with
the init commands of the newer panel of the 10.1-inch Guition JC8012P4A1C
board, 2 MB a frame at 16 bits. The last argument of the constructor,
`frames`, chooses how the screen is updated:

| `frames` | Frame buffers | Tearing |
| --- | --- | --- |
| `DgxDsiOneFrame` | 1 | possible |
| `DgxDsiTwoFrames` | 2 | none, for one more frame of PSRAM |
| `DgxDsiOneFrameAfterScan` | 1 | none for a virtual screen sent to the display and for a large rectangle, at the cost of a wait |

Everything else a program should know about such a display is in
[docs/dsi-usage-en.md](docs/dsi-usage-en.md)
([Russian](docs/dsi-usage-ru.md)): batches and an even frame rate, pieces
that bypass the processor, settings that depend on the board, several
tasks. How the bus is built and what was measured on it is in
[docs/dsi-display-en.md](docs/dsi-display-en.md)
([Russian](docs/dsi-display-ru.md)), and
[examples/dsi_demo](examples/dsi_demo) is a working program.

NV3041A is the controller of 4.3-inch 480x272 panels on a QSPI bus, as on
the Guition JC4827W543 (ESP32-S3). The driver has the init commands of that
board's panel, takes 16-bit color only, and starts in upright landscape. It
keeps the backlight off until the panel memory has been cleared, so the noise
the panel shows while it starts is never seen. That board has no reset line
for the panel (`GPIO_NUM_NC`) and its backlight on GPIO1:

```c
dgx_bus_protocols_t *bus = dgx_qspi_init(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_21, GPIO_NUM_48, GPIO_NUM_40,
                                         GPIO_NUM_39, GPIO_NUM_47 /* sclk */, GPIO_NUM_45 /* cs */, 40 * 1000 * 1000, 0);
dgx_screen_t *scr = dgx_nv3041a_init(bus, GPIO_NUM_NC, GPIO_NUM_1, 16, DgxScreenRGB);
```

40 MHz is the limit the datasheet sets for four lines and RGB565, and the
panel of that board works there. To stay within the datasheet, ask for
26666667, which runs at 26.7 MHz: the SPI clock is 80 MHz divided by a whole
number, and one that does not divide evenly is rounded down. 80 MHz garbles
the picture.

The panel of that board now and then writes a pair of pixels just outside a
window it is sent. On a picture updated in small windows they stay behind as
dots; a picture sent whole from a virtual screen, frame after frame, does not
keep them, so send this panel whole frames. A note on the whole panel, from
how it takes pixels to its TE line, with what was measured and tried:
[docs/nv3041a-en.md](docs/nv3041a-en.md) ([Russian](docs/nv3041a-ru.md)).

ST77916 is the controller of round 360x360 panels, on QSPI as a rule, with
the same commands and pixels on that bus as NV3041A. The driver has the init
table of Arduino_GFX for a 1.5-inch glass, seen right at 40 MHz on a module
marked "TFT 1.53 1.0" with the glass T150XV-C16-04. It takes 16 or 18 bits,
keeps the backlight off until the panel memory is black, as NV3041A does, and
scrolls by its own hardware. An init table belongs to its glass: with the table of
another one the picture comes out as stripes, so a module of another glass
takes its own, see [A panel of your own module](#a-panel-of-your-own-module).

### A panel of your own module

ST7735, ST7789, ILI9341, GC9A01, NV3041A and ST77916 are descriptions of
their panels, `dgx_panel_t` of [include/drivers/dgx_panel.h](include/drivers/dgx_panel.h),
and their constructors make a screen of one with `dgx_panel_init()`. Their
headers have them under the names `dgx_panel_st7735`, `dgx_panel_st7789_240x240`,
`dgx_panel_st7789_240x320`, `dgx_panel_ili9341`, `dgx_panel_gc9a01`,
`dgx_panel_nv3041a` and `dgx_panel_st77916`. A module of another glass is a
copy of one with what the module has otherwise: the size of its glass and
where the glass lies in the memory of the controller, the table of commands
of its maker. For a glass of 240x280 that lies 20 rows down in the 240x320 of
an ST7789:

```c
static dgx_panel_t panel; /* kept by its pointer: it lives as long as the screen */
panel               = dgx_panel_st7789_240x320;
panel.height        = 280;
panel.memory_height = 320;
panel.y_offset      = 20;
dgx_screen_t *scr = dgx_panel_init(bus, &panel, rst, GPIO_NUM_NC, 16, DgxScreenRGB);
```

Turned or mirrored, the window follows the glass: with the rows going up it
lies 20 rows up from the end of the memory. `dgx_panel_orientation()` and
`dgx_panel_backlight()` work for every such screen. A table of the maker
gives MADCTL and COLMOD `dgx_panel_adj_madctl` and `dgx_panel_adj_colmod`
as their adjust functions, so that they follow the order of colors and the
depth of the screen.

For SSD1306, pick a geometry from `ssd1306_resolution_t`: `SSD1306_128X32`,
`SSD1306_128X64`, `SSD1306_96X16`, `SSD1306_64X48` or `SSD1306_72X40`.

SSD1309 is the controller of the larger modules, 2.42 inches as a rule. It
takes the commands of the SSD1306 but has no charge pump, so its constructor
has no supply argument: `dgx_ssd1309_init(bus, SSD1309_128X64, rst)`, or
`SSD1309_128X32`. The init commands are those of the u8g2 driver. Some
modules wire the first column of the glass to segment 2 of the controller
and show the picture shifted right by two pixels: tell the driver with
`dgx_ssd1309_column_offset(scr, 2)`.

SH1106 is what most 1.3-inch 128x64 modules have, and they are often sold
as SSD1306. The two are easy to confuse and easy to tell apart: an SH1106
under the SSD1306 driver shows garbage in the top eight rows and nothing
below, because it does not know the addressing commands of the SSD1306 and
takes the whole picture into one page. It has 132 segments and the page
addressing mode only, so the driver sends a changed rectangle page by page,
each with its own address. `dgx_sh1106_init(bus, SH1106_128X64, rst)` or
`SH1106_64X32`, the 0.42-inch glass. The glass takes a run of the segments:
from segment 2 for 128 columns and from 32 for 64, as u8g2 has it; if a
module of yours shows a blank stripe at an edge, pass the segment of its
first column to `dgx_sh1106_column_offset()`. The init commands are those
of the u8g2 driver written for the controller after its datasheet.

## What's in the box

- Panel drivers for ST7735, ST7789, ILI9341, NV3041A, ST77916, GC9A01, SSD1351, SSD1306,
  SSD1309, SH1106, ST7565R, ST7920, JD9165 and JD9365.
- SPI, QSPI, I2C and 8-bit parallel (I80) transports for ESP32, and MIPI-DSI
  for ESP32-P4.
- A color RAM-backed virtual screen and a 1-bit monochrome virtual screen.
  They are useful on their own, as staging buffers for animation, or as shadow
  buffers behind monochrome controllers. A virtual screen goes to a display or
  to another screen as it is, or larger by squares or smoothed (Scale2x,
  Scale3x and their SFX variants), turned or mirrored, with transparent
  pixels; an 8-bit one through a table of colors.
- One translucent overlay layer per screen, mixed into everything that comes
  from a virtual screen: buttons over a game.
- Sprites and icons of 1, 4 or 8 bits a pixel kept in flash, drawn into an
  8-bit virtual screen with a map of colors, clipped, mirrored and rotated.
- Color tables for 8-bit virtual screens: from a palette, a ramp between two
  colors, fades and flashes, without redrawing the frame.
- A two-head compositor (`vscreen_2h`) that exposes two child screens as one
  logical screen.
- Drawing primitives — pixels, lines, rectangles with square or round
  corners, circles, ellipses, their arcs and sectors, filled and textured
  quads, a rectangle scrolled any way — and an arc gauge helper.
- UTF-8 text rendering with 8-way orientation, glyph lookup, layout and bounds
  queries, plus a small "morph" helper for animating between glyphs.
- Handwritten fonts: symbols kept as pen paths of Bezier curves, written at
  any size with a pen of any thickness, letters joined, at once or stroke by
  stroke at the pace of a pen.
- `font2c`, an offline tool that turns TTF/BDF-style fonts and handwritten
  fonts of the hw-fonts editor into C source/header pairs, and a set of
  pre-converted fonts in `src/fonts/`; a font editor that fixes glyphs of
  those fonts by hand and keeps the fixes as patches `font2c` applies.
- Fonts made at build time from needs declared in CMake by a program and
  its components: a face picked by rules or named exactly, a size in
  points, millimeters or pixels, only the characters the texts use, one
  font per face and size.

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

## API reference

Every drawing and text call takes a `dgx_screen_t *`, so the same code works
across hardware panels and virtual screens. Colors are plain `uint32_t` values
packed for the target pixel format (see [Colors](#colors)).

### Drawing primitives

Declared in [include/dgx_draw.h](include/dgx_draw.h):

| Function | Description |
| --- | --- |
| `dgx_set_pixel(scr, x, y, color)` | Set one pixel. |
| `dgx_get_pixel(scr, x, y)` | Read one pixel: virtual screens, panels drawn through a virtual back screen and a DSI display only; see [Known gaps](#known-gaps). |
| `dgx_screen_is_readable(scr)` | `true` when the screen gives back the pixels drawn on it, `false` for a display that keeps them in the panel behind a bus. |
| `dgx_draw_line(scr, x1, y1, x2, y2, color)` | Single-pixel line by Bresenham's algorithm; the same pixels whichever end it is given from. |
| `dgx_draw_line_thick(scr, x1, y1, x2, y2, width, color)` | Thick line: the rectangle of `width` around the way, its ends cut square; as wide at every angle, and of width 1 the line of `dgx_draw_line()`. |
| `dgx_draw_bezier3(scr, points, pieces, width, color)` | Thick quadratic Bezier curve by three `dgx_point_2d_t` points, drawn by `pieces` straight pieces; 0 finds their number from the points, within half a pixel of the curve. `DGX_PEN_SOFT | width` gives it a soft edge. |
| `dgx_draw_bezier4(scr, points, pieces, width, color)` | Thick cubic Bezier curve by four points. |
| `dgx_bezier3_begin(b, scr, points, pieces, width, color)`, `dgx_bezier4_begin(...)` | Prepare the same curves in a `dgx_bezier_t` for drawing part by part, to animate writing. |
| `dgx_bezier_draw_to(b, t)` | Draw a prepared curve on until it is `t` complete, 0 .. 1 by the way of the pen, and flush what was added; returns `true` when the curve is finished. |
| `dgx_bezier_length(b)` | Length of a prepared curve in pixels, to share time between curves. |
| `dgx_draw_line_mask(scr, x1, y1, x2, y2, color, bg, mask, mask_bits)` | Dashed/dotted line; returns the rotated mask to continue the pattern. |
| `dgx_fill_rectangle(scr, x, y, w, h, color)` | Filled rectangle; a negative `w` or `h` takes it to the left of `x` or up from `y`, on every screen. |
| `dgx_draw_rectangle(scr, x, y, w, h, color)` | Outline of the rectangle `dgx_fill_rectangle()` fills, a pixel wide. |
| `dgx_draw_round_rectangle(scr, x, y, w, h, r, color)` | Outline of a rectangle with round corners: each a quarter of `dgx_draw_circle()` of radius `r`, so a square of 2r + 1 is that circle and `r` 0 the plain outline; a radius too large is cut to (the shorter side - 1) / 2. |
| `dgx_fill_round_rectangle(scr, x, y, w, h, r, color)` | The same rectangle filled, its corners quarters of `dgx_solid_circle()`. |
| `dgx_draw_circle(scr, x, y, r, color)` | Circle outline by the midpoint algorithm. |
| `dgx_solid_circle(scr, x, y, r, color)` | Filled circle, the same outline filled by rows. |
| `dgx_draw_ellipse(scr, x, y, rx, ry, color)` | Ellipse outline, a pixel wide, by the rule of the midpoint algorithm: with equal radii it is `dgx_draw_circle()` pixel for pixel. |
| `dgx_draw_arc(scr, x, y, rx, ry, from, to, width, color)` | Arc of an ellipse, of a circle with equal radii, between two angles in degrees, by a pen of `width`; `DGX_PEN_SOFT | width` gives it a soft edge. A hard pen of 1 draws the pixels of the outline between the rays. |
| `dgx_solid_sector(scr, x, y, rx, ry, from, to, color)` | Filled sector of an ellipse between two angles; a whole turn is the ellipse filled. |
| `dgx_scroll(scr, x, y, w, h, dx, dy, color)` | Move what a rectangle shows by `dx`, `dy` within it, any way, and fill what it has left with `color`. In memory where a screen is read, by the scroll of the panel where an ILI9341 or an ST77916 can. Returns `ESP_OK`, or `ESP_ERR_NOT_SUPPORTED` when neither is possible. |
| `dgx_scroll_no_fill(scr, x, y, w, h, dx, dy)` | The same move without the fill: what the picture has left is for the caller to draw. On a panel that scrolls itself the move is its commands alone. |
| `dgx_draw_triangle_solid(scr, x0, y0, x1, y1, x2, y2, color)` | Filled triangle. |
| `dgx_draw_polygon4_solid(scr, x0..y3, color)` | Filled simple quadrilateral (convex or concave). |
| `dgx_draw_texture_quad(scr, quad, texture, tx, ty, tw, th)` | Stretch a region of a virtual screen onto a convex quadrilateral (affine, nearest texel). |
| `dgx_draw_texture_rect(scr, x, y, w, h, texture, tx, ty, tw, th)` | Scale a region of a virtual screen into a rectangle. |

Lines, circles, ellipses and sectors are one code for every screen, made of
the pixels and the rectangles a screen draws itself: a figure has the same
pixels on a display, on a virtual screen and on a monochrome one.

An angle of an arc or a sector is counted in degrees from the ray that goes
from the center to the right, against the clock as the screen is seen, so 90
is up; it is the angle of a ray on the screen whatever the radii are. The
figure is what lies between the rays of `from` and `to`, a pixel on a ray
counted in. Angles are not brought into a turn: from 350 to 370 is twenty
degrees across the ray that goes right, and 360 degrees or more between the
two is the whole ellipse. The outline, the arc by a hard pen of 1 and the
sector are the same pixels: the arc is the outline between the rays, the
sector is the outline and what is within it. An arc by any other pen is the
pen of the curves led along it, with round ends:

```c
/* the sign of Wi-Fi: a dot and three arcs of a quarter of a turn over it */
dgx_solid_circle(band, cx, cy, dot, ink);
for (int i = 1; i <= 3; ++i) {
    dgx_draw_arc(band, cx, cy, i * step, i * step, 45, 135, DGX_PEN_SOFT | pen, ink);
}
```

`dgx_scroll()` reads the picture from the screen where pixels are memory: on
a virtual screen, on a display drawn through a virtual back screen and on a
DSI display. A virtual screen of whole bytes a pixel is moved in its memory a
row at a time, and so is a DSI display in its frame buffer, by its bus; any
other such screen is moved pixel by pixel.
[examples/figures_scroll_demo](examples/figures_scroll_demo) shows the
figures and every kind of scroll on a CYD and on a 7-inch DSI display.

A display that keeps its pixels in the panel behind a bus is not read. There
the panel moves the picture itself when it can, and no pixel of the picture
is sent. An ILI9341 scrolls along the long side of its panel, an ST77916 along
the rows of its memory: up and down on a screen upright, left and right on one
with its axes swapped. The rectangle has to take
the screen from edge to edge across that side, what is before it and after
it along the side stays, as the head and the foot of a terminal do:

```c
/* an ILI9341 upright, 240x320: a head of 24 rows stays, the lines under it go up by the panel */
dgx_scroll(lcd, 0, 24, lcd->width, lcd->height - 24, 0, -line_height, paper);
/* ... draw the new line at the bottom, where it is seen ... */
```

The panel shows its rows from another row of its memory on, so a row is not
where it is shown; every window is led to where its rows lie, and a picture
is drawn where it is seen whatever has been scrolled. A window that crosses
the place where the rows turn round goes in pieces: two when the screen is
upright, two for every row of it when the screen lies. The panel has one
such area at a time: a rectangle that has been moved is filled, by a move as
large as it, before another is moved. Before the move the strip that leaves
is filled with the color, and that fill is what the move costs besides the
commands of the panel. `dgx_scroll_no_fill()` skips the fill for a caller that draws the strip that
comes in anyway; the strip then arrives holding what has just left at the
other edge. Any other move, across the long side
or aslant, returns `ESP_ERR_NOT_SUPPORTED` and draws nothing; a text that
scrolls so is drawn on a virtual screen, which is moved and sent to the
display, in any orientation:

```c
/* a line of text more: the picture goes up by the height of a line */
dgx_scroll(page, 0, 0, page->width, page->height, 0, -line_height, paper);
/* ... draw the new line at the bottom of `page` ... */
dgx_vscreen_to_screen(lcd, 0, 0, page, 0);
```

How a Bezier curve is split into straight pieces and how a curve is drawn on
by `t` is described in
[docs/handwriting-en.md](docs/handwriting-en.md#bezier-curves). What figures
a pen puts on a screen for a piece, what it counts with and what that costs
on an ESP32 is in [docs/pen-en.md](docs/pen-en.md)
([Russian](docs/pen-ru.md)).

The pen of a curve is its `width` and `color`, and its edge is hard: a pixel
is the line or is not. `DGX_PEN_SOFT` added to the width asks for a soft
edge, `DGX_PEN_SOFT | 2`: a pixel on the edge of the line gets the share of
`color` the pen covers of it, a pixel keeps the larger of what it has and
what it is given, and a soft pen with color 0 takes the ink off every pixel
it touches. That takes a pixel that is an amount of ink, a byte of a virtual
screen of 8 bits a pixel; the table such a screen is sent to the display
through, `dgx_vscreen8_to_screen16()`, runs then from the color of the paper
at 0 to the color of the ink. A display that mixes two colors itself, a DSI
display of an ESP32-P4 with two frame buffers, takes such a table as its
first and its last color and mixes the rest, within a step of a channel of
the table; `DGX_VSCREEN_EXACT` for the last argument of the call asks for the
colors of the table to the last bit. On any other screen a pen asked to be
soft draws as a hard one; see
[A soft edge](docs/handwriting-en.md#a-soft-edge).

`quad` is four `dgx_point_2d_t` vertices in the order of the region's corners:
top-left, top-right, bottom-right, bottom-left. The texture needs the same
color depth as the target screen; see
[A picture at any size and angle](#a-picture-at-any-size-and-angle), and
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
| `dgx_font_make_morph_struct(...)` / `dgx_font_make_morph_struct_destroy(...)` | Build/free a glyph-to-glyph morph descriptor for animation, dot i to dot i; drawn by `dgx_morph_symbol_draw()`. |
| `dgx_bw_bitmap_foreach_set(bmap, func, user_data)` | From [include/dgx_bitmap.h](include/dgx_bitmap.h): iterate set pixels of a 1-bpp bitmap; zero bytes skip 8 pixels at once (LINES format). |
| `dgx_font_<need>()` | A font made at build time for a need declared in CMake, from the generated `dgx_fonts.h`, together with `DGX_FONT_<NEED>_MADE`, `_PX`, `_MM`, `DGX_FONT_NEEDS(X)` and `DGX_FONTS_BOARD_*`; see [Fonts made at build time](#fonts-made-at-build-time). |

`orientation` is a `dgx_output_orientation_t`: `DgxOutputNormal`,
`DgxOutputMirrorX`, `DgxOutputMirrorY`, `DgxOutputRotate180`,
`DgxOutputTranspose`, `DgxOutputRotate90CCW`, `DgxOutputRotate90CW` or
`DgxOutputTransverse`. `scale` is an integer multiplier (`1` = native size).
Each bundled font header under `include/fonts/` exposes an accessor, e.g.
`LiberationSansRegular12()`.

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

`width` is the pen of all of these, and `DGX_PEN_SOFT | width` a pen with a
soft edge: `color` is then an amount of ink, and a text written again by the
same pen with color 0 is taken off, see
[Drawing primitives](#drawing-primitives).

[examples/hw_font_demo](examples/hw_font_demo) fits three lines to the screen
and writes them by hand, [examples/hw_morph_demo](examples/hw_morph_demo)
morphs words one into another both ways.

One such font is bundled: `font0ss()` from `fonts/font0ss.h`. It has the Latin
alphabet with the German letters `ÄÖÜäöüß`, the Russian alphabet, digits and
punctuation, 155 symbols in all, and nothing else: no accented letters of
other languages, no Greek, no scripts written right to left. Drawing a
handwritten alphabet is slow work and I am not going to draw more of them;
if you need another one, draw it in the editor and send it over, co-authors
are welcome. A font comes from the
[hw-fonts](https://github.com/jef-sure/hw-fonts) editor through `font2c`, see
[font2c/README.md](font2c/README.md).

Sizing, the thick Bezier curves underneath, writing at the pace of a pen and
both morphs are described in detail in
[docs/handwriting-en.md](docs/handwriting-en.md)
([Russian](docs/handwriting-ru.md)), with two recipes above:
[Handwritten text](#handwritten-text) and
[Morph handwritten text](#morph-handwritten-text). The format of a font
and the rules of placing and joining letters are described with the editor,
[font-format-ru.md](https://github.com/jef-sure/hw-fonts/blob/main/font-format-ru.md); how a font is converted and
how it lies in the tables of DGX is in
[docs/hw-font-ru.md](docs/hw-font-ru.md) (both in Russian).

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
| Morph | `dgx_morph_create(from, to, sources, user_data)` | Plans flights in cell coordinates. `sources` decides where each new cell flies in from: `dgx_morph_sources_life` (all live neighbors) `dgx_morph_sources_cells` (the nearest free cell, searched vector by vector over the whole grid, so a line that moved flies as a whole), `dgx_morph_sources_cells_within` (the same no further than a radius, an `int` that `user_data` points to), or your own callback, which is many times slower when it defers pass after pass. Sides above 32767 cells are rejected. `dgx_morph_create_with(..., &options)` takes a `dgx_morph_options_t`: with `DGX_MORPH_ORPHANS_MERGE` an old cell nobody took flies into the nearest cell of `to`, along its figure first, and goes out there instead of fading in place; `merge_radius` bounds the way, 0 is no limit. |
| Text | `dgx_morph_text_create(font, from, to, length, sources, user_data)`, `dgx_morph_text_duration_us()` | One morph per letter between two UTF-8 strings; the shorter one is padded with spaces. `changed` tells how many letters actually move. |
| Frame | `dgx_morph_draw(morph, t, x, y, cell, trail, dot, user_data)`, `dgx_morph_progress()` | Stateless: emits the dots of progress `t` in pixels through a `dgx_morph_dot_func_t`. |
| Dot by dot | `dgx_morph_symbol_draw(morph, t, dot, user_data)` | Emits the dots of a `dgx_font_make_morph_struct()` plan, where dot i of one glyph flies to dot i of the other, through the same `dgx_morph_dot_func_t`. A glyph out of nothing lights up, one into nothing goes out; dots that meet in one pixel are emitted once, so the glow does not make them fat. |
| Renderers | `dgx_morph_glow_*` / `dgx_morph_sprite_*` | Additive glow with phosphor persistence, two bytes a pixel at any color depth, or an intensity-scaled dot sprite put over what is there. Both map brightness to colors through a replaceable 256-entry LUT in the screen format (16, 18 or 24 bits); the sprite also draws into an 8-bit screen, where it puts the brightness itself, or the value of its LUT for it. |
| Filter | `dgx_morph_glow_set_filter(glow, filter, user_data)`, `dgx_morph_glow_blur` | One more pass over a copy of every finished glow frame right before it is shown; it does not get into the phosphor. The ready blur smooths the steps of glyphs turned into dots; in one pass it works row by row and needs no copy. |

[Morph one glyph into another](#morph-one-glyph-into-another) is a short
example, and [docs/morphing-en.md](docs/morphing-en.md) walks through all of
it.

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

#### Tables for 8-bit screens

[include/dgx_lut.h](include/dgx_lut.h) fills the `uint16_t lut[256]` of RGB565
colors that `dgx_vscreen8_to_screen16()` and
`dgx_vscreen8_region_to_screen16()` take. Each function fills the entries
`first .. first + count - 1` only; entries past 255 are skipped. A palette is
RGB888 (`const uint8_t (*rgb)[3]`, may be in flash), its entry `k` going to
`lut[first + k]`; a single color is `0xRRGGBB`. Mixing rounds to the nearest,
and `t = 0` and `t = 255` give the two ends exactly.

| Function | Description |
| --- | --- |
| `dgx_lut_from_rgb(lut, first, count, rgb)` | The entries from an RGB888 palette. |
| `dgx_lut_gradient(lut, first, count, rgb_from, rgb_to)` | A ramp from one color to another; the first and the last entry exactly these colors. |
| `dgx_lut_mix(lut, first, count, a, b, t)` | A mix of two RGB888 palettes: `t = 0` is `a`, `t = 255` is `b`. |
| `dgx_lut_mix_color(lut, first, count, a, rgb, t)` | A mix of an RGB888 palette with one color: a fade to black, a flash to white. |

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
| `dgx_vscreen_to_screen(dst, x, y, src, flags)` | Put a RAM screen on a screen of the same depth: a display or another RAM screen. |
| `dgx_vscreen_region_to_screen(dst, x, y, src, x_src, y_src, w, h, flags)` | The same for a sub-region; `x`, `y` are where the corner of the region goes, scaled and turned as the flags ask. |
| `dgx_vscreen8_to_screen16(dst, x, y, src, lut, flags)` | Expand an 8-bit indexed screen into 16-bit through a LUT. |
| `dgx_vscreen8_region_to_screen16(dst, x, y, src, x_src, y_src, w, h, lut, flags)` | The same for a sub-region. |
| `dgx_vscreen_to_argb8888(scr, x, y, w, h, lut, dst, dst_stride)` | Read a rectangle of a screen as 0xAARRGGBB colors, the layout of `SDL_PIXELFORMAT_ARGB8888`: for a window on a PC. An 8-bit screen goes through `lut`, or as grey without one. |
| `dgx_overlay_set(display, layer, x, y, lut, alpha)` | Lay an 8-bit virtual screen over a 16-bit screen at `x`, `y` (`dgx_overlay.h`), or move it, or give it other colors: index 0 empty, the others the colors of `lut` mixed with what lies under them by `alpha` (0 to 255, in 32 steps). One layer a screen. Mixed into what comes from virtual screens: `dgx_vscreen8_to_screen16()`, `dgx_vscreen8_region_to_screen16()`, `dgx_vscreen_to_screen()`, `dgx_vscreen_region_to_screen()`, with all their flags; not into what is drawn straight on the screen. |
| `dgx_overlay_clear(display)` | Take the layer away; its place becomes changed. |
| `dgx_overlay_take_dirty(display, &x, &y, &w, &h)` | The rectangle the layer changed since the last call: drawn into, laid, moved, taken away. Put it again from the picture. |
| `dgx_image_blit(dst, x, y, image, how)` | Draw a picture of 1, 4 or 8 bits from flash or RAM (`dgx_image.h`) into a linear 8-bit screen: value 0 transparent, a map or one color for the others, clipped, mirrored or rotated around its origin. Returns `ESP_ERR_NOT_SUPPORTED` for another destination. |
| `dgx_bw_init(width, height)` | Allocate a 1-bit monochrome RAM screen. |
| `dgx_vscreen_2h_init(left, right)` | Compose two screens as one wide logical screen. |
| `dgx_vscreen_is_linear(scr)` | True when the screen is a `dgx_vscreen_t` with row-major pixels in `v_array`. |
| `dgx_bw_screen_is_paged(scr)` | True when the screen keeps its bits in 8-row pages (`dgx_bw_init()`, SSD1306, SSD1309, SH1106, ST7565R). |
| `dgx_bw_blit_or(scr, x, y, bmap)` | OR a 1-bpp bitmap into a page screen, clipped. |

The last argument of the four transfers is 0 or these added together:

| Flag | What it does |
| --- | --- |
| `DGX_VSCREEN_TRANSPARENT` | A pixel of value 0, index 0 through a table, is not put; `true` is the same. |
| `DGX_VSCREEN_SCALE(n)` | Every pixel a square of `n` by `n`, `n` up to 255. |
| `DGX_VSCREEN_SMOOTH` | With a scale of 2, 3 or 4: Scale2x, Scale3x, Scale2x twice, instead of plain squares. |
| `DGX_VSCREEN_SMOOTH_SFX` | The same by Scale2xSFX and Scale3xSFX: fewer lumps on thin lines, more work. |
| `DGX_VSCREEN_ORIENTED(o)` | Turned or mirrored, `o` a `dgx_output_orientation_t`; scaled and smoothed first. |
| `DGX_VSCREEN_EXACT` | Through a table only: its colors to the last bit, no hardware that mixes two colors. |

Free any screen with `dgx_screen_destroy(&scr)`.

### Arc gauge

Declared in [include/dgx_gauge.h](include/dgx_gauge.h):

| Function | Description |
| --- | --- |
| `dgx_gauge_init(gauge, scr, cx, cy, inner_radius, width, start_angle, sweep_degrees, min, max, bg_color, color_fn)` | Configure a ring gauge and draw its empty ring. `color_fn` maps a value to a step color. |
| `dgx_gauge_set_value(gauge, value)` | Update the value, redrawing only changed steps. |
| `dgx_gauge_redraw(gauge)` | Redraw the whole gauge. |

### Bus backends

Declared under [include/bus/](include/bus):

| Function | Description |
| --- | --- |
| `dgx_spi_init(host_id, dma_chan, mosi, miso, sclk, cs, dc, clock_speed_hz, cpolpha_mode)` | SPI master bus. |
| `dgx_spi_init_on_bus(host_id, cs, dc, clock_speed_hz, cpolpha_mode)` | A display on an SPI host the program has initialized. |
| `dgx_qspi_init(host_id, dma_chan, d0, d1, d2, d3, sclk, cs, clock_speed_hz, cpolpha_mode)` | QSPI bus on an SPI host: commands on one line, pixels on four, no D/C line. |
| `dgx_qspi_init_on_bus(host_id, cs, clock_speed_hz, cpolpha_mode)` | A QSPI display on an SPI host the program has initialized with four data lines. |
| `dgx_spi_init_with(..., options)`, `dgx_spi_init_on_bus_with(..., options)`, `dgx_qspi_init_with(..., options)`, `dgx_qspi_init_on_bus_with(..., options)` | The same four with choices of the program in a `dgx_spi_options_t`, such as the longest buffer; NULL for the defaults. |
| `dgx_i2c_init(i2c_num, i2c_address, sda, sclk, clock_speed_hz)` | I2C master bus. |
| `dgx_i2c_init_on_bus(i2c_bus, i2c_address, clock_speed_hz)` | A display on an I2C master bus the program owns. |
| `dgx_p8_init(d0..d7, wr, rd, cs, dc, pclk_hz)` | 8-bit parallel (I80) bus. |
| `dgx_p8_init_on_bus(i80_bus, rd, cs, pclk_hz)` | A display on an I80 bus the program owns. |
| `dgx_dsi_init(bus_id, data_lanes, lane_bit_rate_mbps, phy_ldo_chan, phy_ldo_mv)` | MIPI-DSI bus of ESP32-P4. `phy_ldo_chan` and `phy_ldo_mv` name the regulator of the chip that powers the DSI PHY, 3 and 2500 on the usual boards; -1 when it is powered otherwise. |

Each returns a `dgx_bus_protocols_t *` to hand to a driver constructor. The
program disposes of a backend with `bus->dispose(bus)` after the screens on
it are destroyed.

A display often shares its bus with other devices: a touch controller or a
clock chip on I2C, an SD card or a touch controller on SPI. Who owns the bus
depends on the constructor:

- `dgx_spi_init()`, `dgx_qspi_init()`, `dgx_i2c_init()` and `dgx_p8_init()`
  put the display on a bus DGX sets up: the SPI host, the I2C master bus of the port, the I80
  bus of those pins. Further displays on the same host or port share it, and
  must name the same pins; an I80 bus takes one display. DGX releases the bus
  with the last of its displays, an I2C or I80 bus unless the program has put
  devices of its own on it. An SPI host DGX sets up takes no devices of the program: the only
  display on it keeps the host between its drawings, and a device added to
  it would wait for ever. A touch controller or an SD card beside the display
  goes on a host the program sets up, with `dgx_spi_init_on_bus()`.
- `dgx_spi_init_on_bus()`, `dgx_qspi_init_on_bus()`, `dgx_i2c_init_on_bus()`
  and `dgx_p8_init_on_bus()` put the display on a bus the program has set up and owns. DGX adds its
  device and removes it on dispose; the program releases the bus afterwards.
  On SPI and QSPI the display holds the host from a window to the end of its
  drawing, or of a batch opened with `dgx_screen_progress_up()`: the other
  devices take it between drawings, and one used from inside such a batch,
  in the same task, would wait for ever.
  An SPI host must have DMA and a `max_transfer_sz` of 0 or at least 4092
  bytes; DGX sends pieces as long as the host takes, see below. An I80 bus must be 8 bits wide with a `max_transfer_bytes` of at
  least 4096; the program sends to its own panels on it only after
  `scr->wait_buffer(scr)`.

```c
// an SSD1306 and a touch controller on one I2C bus the program owns
i2c_master_bus_config_t cfg = {
    .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_21, .scl_io_num = GPIO_NUM_22,
    .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
};
i2c_master_bus_handle_t i2c;
ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c));
dgx_bus_protocols_t *bus = dgx_i2c_init_on_bus(i2c, 0x3c, 400000);
dgx_screen_t *scr = dgx_ssd1306_init(bus, SSD1306_128X64, 0, GPIO_NUM_NC);
// ... i2c_master_bus_add_device(i2c, ...) for the touch controller ...
```

Both I2C constructors probe the address first and fail when no device
answers. The ESP-IDF drivers take turns between the devices of a bus; a long
picture on SPI or QSPI holds the bus for one piece at a time. Every piece
costs the same time to start, so a fast bus needs long pieces: the buffer of
a display is as long as the wire takes about 0.95 ms to send at the clock the
display has really got, and a virtual screen goes in halves of it. The
buffer is 4000 bytes at least and 32000 at most, in internal memory that DMA
reaches on every chip: about 4700 bytes on SPI at 40 MHz, about 19000 on QSPI
at 40 MHz. A slower clock takes a smaller buffer. DGX sets up its own hosts for
32000 bytes; on a host of the program the buffer is no longer than its
`max_transfer_sz`. A program that needs that memory more than the speed sets
the longest buffer itself:

```c
// the QSPI display of the JC4827W543 with a buffer of 8000 bytes instead of about 19000
const dgx_spi_options_t options = {.buffer_max = 8000};
dgx_bus_protocols_t *bus = dgx_qspi_init_with(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_21, GPIO_NUM_48, GPIO_NUM_40, GPIO_NUM_39,
                                              GPIO_NUM_47 /* sclk */, GPIO_NUM_45 /* cs */, 40 * 1000 * 1000, 0, &options);
```

A shorter buffer sends pixels in more pieces. The buffer is never longer
than the limit, which may go below the 4000 bytes DGX takes at least; DGX
cuts it down to a multiple of 16 bytes.

A virtual screen of the depth of the display, sent unturned with no overlay
layer, whose rows lie one after another in memory (its whole width, or a
single row), goes to an SPI or QSPI display straight from where it lies, by
DMA in pieces of the buffer, with nothing copied: from internal memory DMA
reaches, or from PSRAM on an ESP32-S2, S3 or P4 with ESP-IDF 5.5 or later
when DMA from PSRAM keeps up with the wire, up to 26.7 MHz on four lines. A
P4 under flash encryption, or with PSRAM ECC from ESP-IDF 6, takes pixels
from PSRAM so only from an address on 16 bytes. Anything else goes through
the halves of the buffer.

The bus tables of DGX and the init tables of the panel drivers are not
locked: create and dispose of backends and screens from one task.

The 8-bit parallel bus stands on the I80 driver of the `esp_lcd` component,
and that driver is two different ones. On an ESP32-S3 and an ESP32-P4 it can
send data alone. On an ESP32 and an ESP32-S2, where the bus is made of the
I2S peripheral, every transfer starts with a command byte. So on this bus a
command goes in one transfer with its data, and every block of pixels goes
with the command that writes memory: a block that continues a window has the
window set anew, to what is left of it. That is one or two short commands
more for every 4000 bytes of pixels, and the same code for both kinds of
chips.

[include/bus/dgx_dsi_esp32.h](include/bus/dgx_dsi_esp32.h) also has what a
driver of another DSI display is written with: `dgx_dsi_video_setup()` makes
the frame buffer from the size and the timing of the picture,
`dgx_dsi_video_start()` starts sending it after the commands of the display,
`dgx_dsi_orientation()` sets how the screen lies on the panel and
`dgx_dsi_screen_functions()` gives the screen its drawing functions, those
that write straight into the frame buffer among them. The window and the pixels go
through the bus as to any display, with the commands 0x2A, 0x2B and 0x2C of
the MIPI display command set. The bus answers those itself from
`dgx_dsi_video_start()` on; before it everything goes to the display, so a
controller that has registers of its own under those numbers gets them.
[src/drivers/jd9165.c](src/drivers/jd9165.c) is such a driver in full.

For a program the same header has `dgx_dsi_show()`, `dgx_dsi_counters()`,
`dgx_dsi_frame_us()`, `dgx_dsi_frame_ended()`, `dgx_dsi_tiles()`, and
`dgx_dsi_bench()`, `dgx_dsi_tune()`, `dgx_dsi_tuning()`, `dgx_dsi_advise()`
and `dgx_dsi_tuning_text()` with the structure `dgx_dsi_tuning_t`.

### Flush control and batching

For a physical display that should not be updated after every primitive, draw
the complete image into a virtual screen in RAM and send it to the panel when
the frame is ready with `dgx_vscreen_to_screen()` (see
[Animation without flicker](#animation-without-flicker)). The virtual screen is the backbuffer; `in_progress` is not a
pixel buffer.

Some screens stage changes and commit them when `update_screen()` is called
(ST7920, SSD1306, SSD1309, SH1106, ST7565R, the two-head compositor). Every primitive marks
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
| `dgx_screen_take_dirty(scr, &left, &top, &width, &height)` | Take the pending dirty area and clear it without committing; `false` when nothing has changed. For a virtual screen shown by parts: there is something to take only while a batch is open. |
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
- A display on SPI or QSPI holds its host for the whole of a batch. Another
  device of that host is used before or after the batch, not inside it.

## Drawing from several tasks

DGX has no task of its own and no lock of its own, and nothing in it is
global but its tables of the buses in use. All it keeps is in the objects a program makes: a screen with its
bus, a virtual screen, a morph, a writer of handwriting. So what may run at
the same time follows from what each object keeps between calls.

| Object | What it keeps between calls | What follows |
| --- | --- | --- |
| The screen of a display and its bus | the window being written and the place in it, in the controller or, for MIPI-DSI, in the bus; the one transfer buffer of the bus; transfers under way; how deep in batches the screen is and what is changed but not shown yet | one task at a time |
| A virtual screen | its pixels, how deep in batches it is, what is changed | one task writes it at a time; any number of virtual screens may be written at the same time |
| A font | nothing, it is read only | shared freely |
| A morph, a glow renderer, a writer or a morph of handwriting | its own progress and memory | one task an object |
| Several displays | nothing on hosts of their own, the host when they share one | on separate hosts they do not hinder each other; on one SPI or QSPI host they take turns, and one is not drawn on inside a batch of the other; each takes one task at a time |

Why a screen takes one task at a time: a primitive is "set a window, then
send pixels into it", often in several pieces through the one buffer of the
bus. Two tasks that do it at once send their pixels into each other's
windows. Nothing guards against that, on any bus.

A program with several sources of pictures has two ways.

**One task draws, the others send it messages.** The task that owns the
display is the only one that calls DGX for it, so nothing is shared and
there is no mutex: a mutex guards data, and here no data has two owners. A
source sends what has changed, or a virtual screen it has drawn in, and
does not touch that screen until it is told the picture was taken. The
drawing task puts what came in the time of a frame into one batch and keeps
the pace of the display as any single drawing task does. This way also lets
a program keep clear of inversion of priorities: no source waits for another
source, only for the drawing task, so give that task a priority above the
tasks that prepare pictures. It is the way to choose when pictures are to
come evenly.

**Every task draws itself, under a mutex of the program.** It works as
well, and is less to write when each task has a part of the screen of its
own.

- Lock a batch, not a primitive. What is drawn between
  `dgx_screen_progress_up()` and `dgx_screen_progress_down()` is shown
  together, so the mutex goes around the two. The depth of batches is kept
  by the screen, not by the task: two tasks that open batches on one screen
  without a mutex make one batch of them.
- Draw into a virtual screen without the lock, send it with the lock: each
  task has a virtual screen of its own and takes the mutex only for
  `dgx_vscreen_to_screen()`.
- A mutex, not a binary semaphore. A task of low priority that is put aside
  while it holds the lock keeps all the others waiting; a mutex of FreeRTOS
  raises its priority meanwhile, a semaphore does not.

In either way no drawing call may be made from an interrupt handler: they
wait for transfers.

What a drawing call may wait for, with the lock held if there is one:

| Bus | What a drawing call waits for |
| --- | --- |
| SPI, QSPI, I2C, 8-bit parallel | the transfer of the pixels, as long as the bus takes for them; on a shared SPI or QSPI host also the turn of the other device |
| MIPI-DSI, one frame buffer | nothing but a transfer or a fill by the hardware of the chip, when the piece is large |
| MIPI-DSI, two frame buffers | the same, and the display: the first drawing after a finished picture may wait until the display has taken it, up to a frame, 16 ms on the JD9165 panel |
| MIPI-DSI, behind the scan | the same as with one frame buffer, and the scan of the display to leave the rows, up to a frame |

What the DSI bus does for several tasks with two frame buffers, how evenly
pictures come by the two ways, as measured, and where DGX uses interrupts,
is in
[docs/dsi-usage-en.md](docs/dsi-usage-en.md#several-tasks).

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
on any screen without touching the framebuffer layout. Turned text is the
upright text turned around the top left corner of the pixel `x`, `y` it is
drawn at, so its letters keep their places along the line. Handwritten text is
the exception: it has no orientation parameter and is always written left to
right, see [Known gaps](#known-gaps).

The `dir_x`, `dir_y` and `swap_xy` fields on the screen struct are currently
*metadata* on virtual screens — they describe the screen but do not transform
framebuffer access. In practice, treat virtual screens as always stored in
their canonical layout.

## Build options

Everything is wired through Kconfig. Toggle only what you need, a driver
with its transport: see [Getting started](#getting-started).

| Option | Adds | Notes |
| --- | --- | --- |
| `CONFIG_DGX_ENABLE_SPI` | `bus/spi_esp32.c`, `bus/qspi_esp32.c`, `drivers/panel.c` | SPI and QSPI transports, and `dgx_panel_init()` |
| `CONFIG_DGX_ENABLE_I2C` | `bus/i2c_esp32.c` | I2C transport |
| `CONFIG_DGX_ENABLE_P8` | `bus/p8_esp32.c`, `drivers/panel.c` | 8-bit parallel (I80) transport, and `dgx_panel_init()` |
| `CONFIG_DGX_ENABLE_DSI` | `bus/dsi_esp32.c`, `bus/fb_area.c`, `bus/fb_tiles.c`, `bus/fb_flip.c` | MIPI-DSI transport; chips that have it (ESP32-P4) |
| `CONFIG_DGX_ENABLE_SPI_ST7735` | `drivers/st7735.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SPI_ST7789` | `drivers/st7789.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SPI_GC9A01` | `drivers/gc9a01.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SPI_ILI_9341` | `drivers/ili9341.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_SSD1351` | `drivers/ssd1351.c` | needs SPI or P8 |
| `CONFIG_DGX_ENABLE_NV3041A` | `drivers/nv3041a.c` | needs SPI (its QSPI bus) |
| `CONFIG_DGX_ENABLE_ST77916` | `drivers/st77916.c` | needs SPI (its QSPI bus, or SPI for a panel of one data line) |
| `CONFIG_DGX_ENABLE_SSD1306` | `drivers/ssd1306.c` | needs SPI or I2C; selects `V_BW_SCREEN` |
| `CONFIG_DGX_ENABLE_SSD1309` | `drivers/ssd1309.c` | needs SPI or I2C; selects `V_BW_SCREEN` |
| `CONFIG_DGX_ENABLE_SH1106` | `drivers/sh1106.c` | needs SPI or I2C; selects `V_BW_SCREEN` |
| `CONFIG_DGX_ENABLE_ST7565R` | `drivers/st7565r.c` | needs SPI; selects `V_BW_SCREEN` |
| `CONFIG_DGX_ENABLE_ST7920` | `drivers/st7920.c` | needs SPI; selects `VSCREEN` |
| `CONFIG_DGX_ENABLE_JD9165` | `drivers/jd9165.c` | needs DSI |
| `CONFIG_DGX_ENABLE_JD9365` | `drivers/jd9365.c` | needs DSI |
| `CONFIG_DGX_ENABLE_V_BW_SCREEN` | `vscreen/bw_screen.c` | 1-bit virtual screen |
| `CONFIG_DGX_ENABLE_VSCREEN` | `vscreen/vscreen.c`, `vscreen/transfer.c`, `vscreen/image.c` | color RAM-backed screen and the transfers |
| `CONFIG_DGX_ENABLE_VSCREEN_2H` | `vscreen/vscreen_2h.c` | needs `VSCREEN` |
| `CONFIG_DGX_ENABLE_MORPH` | `morph/*.c` | dot morphing; selects `VSCREEN` |
| `CONFIG_DGX_DSI_PRIVATE_DMA2D` | nothing | DSI on chips with a 2D DMA: large pieces are moved past the processor through the private 2D DMA interface of IDF; on by default, an escape hatch for an IDF that no longer builds it |
| `CONFIG_DGX_DSI_INDEXED_BAND_KB` | nothing | DSI: the size of each of the two bands of internal RAM an 8-bit screen is made into on its way to the frame buffer, 64 KB by default; larger is faster, smaller ones are tried where there is no room |
| `CONFIG_DGX_HW_FONT_IN_IRAM` | nothing | ESP32 only: the code that lays out a handwritten text is kept in IRAM, 4.1 KB |
| `CONFIG_DGX_BEZIER_IN_IRAM` | nothing | ESP32 only: the code of the pen of Bezier curves is kept in IRAM, 9.3 KB |

The two `IN_IRAM` options are for a handwritten text on an ESP32. There code
and the tables of a font are read from flash through one cache of two ways,
and the time of the same text changes from build to build with where the
linker has put the font. With the code in IRAM it does not.
IRAM of an ESP32 is no part of the heap, but there are 128 KB of it and a
program with Wi-Fi leaves a few; see
[docs/pen-en.md](docs/pen-en.md#the-flash-cache).

`dgx_lcd_init.c`, the drawing and font code, handwritten fonts included, and
everything in `src/fonts/` are always compiled. Fonts are picked up via a `file(GLOB)` on
`src/fonts/*.c`, so the linker keeps only the font objects your application
actually references. The glob is a configure dependency, so a font file added
or removed is picked up by the next build.

## Known gaps

A couple of practical limitations are worth knowing up front:

- **Pixel readback works only from RAM.** `get_pixel()` reads virtual screens,
  panels that draw into a virtual back screen first (ST7920, SSD1306,
  SSD1309, SH1106, ST7565R) and a DSI display, whose frame buffer is memory of the chip
  (JD9165, JD9365). Direct panels (ILI9341, ST7789, ...) return 0: many SPI modules
  have no MISO line at all, and where RAMRD works it is far too slow. If you
  need to read what you drew, draw into a `vscreen` and push it with
  `dgx_vscreen_to_screen()`. `dgx_screen_is_readable()` tells which of the two
  a screen is. `dgx_scroll()` reads the picture it moves; on a screen that is
  not read it moves only what the panel can scroll itself, an ILI9341 along
  its long side, an ST77916 along y of a screen upright.
- **The scroll of the panel is used on an ILI9341 and an ST77916 only.** ST7789, ST7735 and
  GC9A01 have the same two commands and take the same code, three lines of a
  driver each; they are not switched on, for there was no panel to see them
  on. `dgx_scroll()` returns `ESP_ERR_NOT_SUPPORTED` there.

- **Virtual screens don't honor `dir_x`/`dir_y`/`swap_xy` for framebuffer
  access.** Those fields describe orientation metadata, but they do not
  rotate or mirror the stored pixel data. A virtual screen is turned where
  it is put on another screen: `DGX_VSCREEN_ORIENTED(o)` in the flags of the
  transfer.

- **Handwritten text goes left to right only.** There is no orientation
  parameter. A soft edge of the lines, `DGX_PEN_SOFT`, comes out on a virtual
  screen of 8 bits a pixel only; drawn straight to a display or into a screen
  of another depth, the lines are not antialiased. The direction has a plain
  reason: I have never written right to left or top to bottom
  myself, have no handwritten fonts of that kind and do not know such
  languages, so there was nothing to build it on. Send the virtual screen to
  the display turned, with `DGX_VSCREEN_ORIENTED(o)`, if you need the text
  turned.

- **The handwritten font knows Latin with German letters and Russian.** That
  is all `font0ss` has. Other alphabets are to be drawn in the
  [hw-fonts](https://github.com/jef-sure/hw-fonts) editor by those who need
  them; contributions are welcome.

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
and the SSD1309, SH1106 and SSD1351 drivers against models of their
controllers. What every color driver sends, through a bus that records it,
is kept in `test/host/panel_streams.txt`, and a driver that sends a byte
more, less or in another place fails:

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
directory of the file. A window shows a virtual screen through
`dgx_vscreen_to_argb8888()`.

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
[examples/figures_scroll_demo](examples/figures_scroll_demo), for a CYD or
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
host/                    dgx_host.mk and ESP-IDF stubs: the platform-independent part built on a PC
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

See [CHANGES.md](CHANGES.md) for the release history.

## License

Copyright (c) 2021-2026 Anton Petrusevich. See [LICENSE](LICENSE).

The fonts in `src/fonts/` are under the licenses of the fonts they were made
from: [src/fonts/README.md](src/fonts/README.md) names each, and
[src/fonts/licenses](src/fonts/licenses) holds their texts.
