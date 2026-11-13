# API reference

The calls of DGX by header, how screens are shared by tasks, how a screen is
turned, what Kconfig switches on, and what DGX does not do. How to put them
together: the recipes of the [README](../README.md#recipes).

- [The calls](#the-calls)
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

## The calls

Every drawing and text call takes a `dgx_screen_t *`, so the same code works
across hardware panels and virtual screens. Colors are plain `uint32_t` values
packed for the target pixel format (see [Colors](#colors)).

### Drawing primitives

Declared in [include/dgx_draw.h](../include/dgx_draw.h):

| Function | Description |
| --- | --- |
| `dgx_set_pixel(scr, x, y, color)` | Set one pixel. |
| `dgx_get_pixel(scr, x, y)` | Read one pixel: virtual screens, panels drawn through a virtual back screen and a DSI display only; see [Known gaps](#known-gaps). |
| `dgx_screen_is_readable(scr)` | `true` when the screen gives back the pixels drawn on it, `false` for a display that keeps them in the panel behind a bus. |
| `dgx_draw_line(scr, x1, y1, x2, y2, color)` | Single-pixel line by Bresenham's algorithm; the same pixels whichever end it is given from. |
| `dgx_draw_line_thick(scr, x1, y1, x2, y2, width, color)` | Thick line: the rectangle of `width` around the way, its ends cut square; as wide at every angle, and of width 1 the line of `dgx_draw_line()`. |
| `dgx_draw_thick_segment(scr, x1, y1, x2, y2, width, color)` | Straight segment by the pen of the curves, its ends in `float`. `DGX_PEN_SOFT | width` gives it a soft edge, `DGX_PEN_START_DISC` and `DGX_PEN_END_DISC` round ends; without them an end is cut straight and sharp, and the next segment goes on from it without a seam. |
| `dgx_draw_thick_segment_part(scr, x1, y1, x2, y2, width, color, part)` | Draw the same segment on as far as the pen has got along it, `part->t` 0 .. 1, in a `dgx_segment_part_t` filled with zeros before the first call; only what is new is drawn. A disc is drawn when a call first asks for it: `DGX_PEN_START_DISC` at the start, `DGX_PEN_END_DISC` where the pen is. Returns `true` when the pen has reached the end. |
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
| `dgx_solid_circle(scr, x, y, r, color)` | Filled circle, the same outline filled by rows. `DGX_PEN_SOFT \| r` gives it the soft edge of a pen, see below the table. |
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
[examples/figures_scroll_demo](../examples/figures_scroll_demo) shows the
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
[docs/handwriting-en.md](handwriting-en.md#bezier-curves). What figures
a pen puts on a screen for a piece, what it counts with and what that costs
on an ESP32 is in [docs/pen-en.md](pen-en.md)
([Russian](pen-ru.md)).

The pen of a curve is its `width` and `color`, and its edge is hard: a pixel
is the line or is not. A straight segment by the same pen is
`dgx_draw_thick_segment()`, with a round end or a straight one at either
end. `DGX_PEN_SOFT` added to the width asks for a soft
edge, `DGX_PEN_SOFT | 2`: a pixel on the edge of the line gets the share of
`color` the pen covers of it, a pixel keeps the larger of what it has and
what it is given, and a soft pen with color 0 takes the ink off every pixel
it touches. That takes a pixel that is an amount of ink, a byte of a virtual
screen of 8 bits a pixel; the table such a screen is sent to the display
through, `dgx_vscreen8_to_screen16()`, runs then from the color of the paper
at 0 to the color of the ink: `dgx_lut_gradient(lut, 0, 256, paper, ink)`, the
two colors as `0xRRGGBB`, see
[Tables for 8-bit screens](#tables-for-8-bit-screens). A display that mixes
two colors itself, a DSI display of an ESP32-P4 with two frame buffers, takes
such a table as its first and its last color and mixes the rest, within ±1
of the table in red, green or blue, which the eye rarely sees;
`DGX_VSCREEN_EXACT` for the last argument of the call asks for the
colors of the table to the last bit. On any other screen a pen asked to be
soft draws as a hard one; see
[A soft edge](handwriting-en.md#a-soft-edge).

A filled circle takes the same bit in its radius:
`dgx_solid_circle(scr, x, y, DGX_PEN_SOFT | r, ink)` has the edge of the dot of
a soft pen 2r + 1 wide. It covers what is under it, as the hard circle does:
the inside gets the ink, lighter or darker than the paper, and the edge is
mixed with what was there; color 0 draws a circle of paper with a soft edge.
`DGX_PEN_SOFT | DGX_PEN_MAX | r` draws by the rule of the pen instead, a pixel
keeping the larger ink, and is then the dot of the pen byte for byte. That
rule is for ink on clean paper: on a paper with ink of its own it loses the
part of the edge lighter than the paper, and the edge looks hard.

`quad` is four `dgx_point_2d_t` vertices in the order of the region's corners:
top-left, top-right, bottom-right, bottom-left. The texture needs the same
color depth as the target screen; see
[A picture at any size and angle](../README.md#a-picture-at-any-size-and-angle), and
`examples/flip_clock_demo` uses both functions.

### Text and fonts

Declared in [include/dgx_font.h](../include/dgx_font.h):

| Function | Description |
| --- | --- |
| `dgx_font_draw(ctx, x, y, str)` | Draw a UTF-8 text by a context, `dgx_font_context_t`: `scr`, `font`, `color`, `bg`, `flags`, `orientation`, `scale`, and `dot_func` with `user_data` for a dot font. Returns how far the text goes along its line, scale counted. See [Text with soft edges](../README.md#text-with-soft-edges). |
| `dgx_font_draw_char(ctx, x, y, codePoint)` | One character by a context; with `DGX_FONT_FILL` it fills the cell of the one character. |
| `dgx_font_bounds(ctx, str, top, height)` | Measure a text as `dgx_font_draw()` draws it, scale counted: returns its length along the line, fills the top from the base line and the height. |
| `DGX_FONT_BG`, `DGX_FONT_FILL`, `DGX_FONT_MONO`, `DGX_FONT_HARD` | The flags of a context: mix soft edges with `bg`; fill the cell with `bg` too; draw a soft font cut to one bit; draw every pixel a letter touches in full. |
| `dgx_font_string_utf8_screen(scr, x, y, str, color, orientation, scale, font, draw_func, param)` | Render a UTF-8 string. Pass `NULL, NULL` for the last two for default rendering. |
| `dgx_font_char_to_screen(scr, x, y, codePoint, color, orientation, scale, font, draw_func, param)` | Render one code point. |
| `dgx_font_string_bounds(str, font, ycorner, height)` | Measure a string; returns width, fills top offset and height. |
| `dgx_font_find_glyph(codePoint, font, xAdvance)` | Look up a single glyph. |
| `decodeUTF8next(chr, idx)` | Decode the next UTF-8 code point. |
| `dgx_font_make_morph_struct(...)` / `dgx_font_make_morph_struct_destroy(...)` | Build/free a glyph-to-glyph morph descriptor for animation, dot i to dot i; drawn by `dgx_morph_symbol_draw()`. |
| `dgx_bw_bitmap_foreach_set(bmap, func, user_data)` | From [include/dgx_bitmap.h](../include/dgx_bitmap.h): iterate set pixels of a 1-bpp bitmap; zero bytes skip 8 pixels at once (LINES format). |
| `dgx_font_<need>()` | A font made at build time for a need declared in CMake, from the generated `dgx_fonts.h`, together with `DGX_FONT_<NEED>_MADE`, `_PX`, `_MM`, `DGX_FONT_NEEDS(X)` and `DGX_FONTS_BOARD_*`; see [Fonts made at build time](../README.md#fonts-made-at-build-time). |

`orientation` is a `dgx_output_orientation_t`: `DgxOutputNormal`,
`DgxOutputMirrorX`, `DgxOutputMirrorY`, `DgxOutputRotate180`,
`DgxOutputTranspose`, `DgxOutputRotate90CCW`, `DgxOutputRotate90CW` or
`DgxOutputTransverse`. `scale` is an integer multiplier (`1` = native size).
Each bundled font header under `include/fonts/` exposes an accessor, e.g.
`LiberationSansRegular12()`.

A font tells its kind in `f_type`: bitmaps of one bit a pixel
(`DGX_FONT_BITMAP_LINES`, `DGX_FONT_BITMAP_STREAM`), bitmaps of the coverage
of each pixel in 2 or 4 bits (`DGX_FONT_BITMAP_DEPTH2`,
`DGX_FONT_BITMAP_DEPTH4`, made by `font2c -b`), dots (`DGX_FONT_DOTS`) or pen
paths (`DGX_FONT_HW`). The functions above draw every kind but the last; a
handwritten font shares only the glyph lookup with them and is drawn by the
functions of the next section. Fonts made by the current `font2c` also tell the number of
their ranges, so a glyph is found by halving instead of trying the ranges in
order.

#### The paper of a text: no flag, `DGX_FONT_BG` or `DGX_FONT_FILL`

A soft font mixes the edges of its letters with something, and a text drawn
again over itself has to take the old one away. The flags of the context say
with what, and how much of the screen the text writes.

Table 1. What each way writes and leaves, and what it is for.

| Flags | What is written | What stays as it was | For |
| --- | --- | --- | --- |
| none | the letters; on a screen that is read, a virtual screen of 4 to 24 bits or a DSI display, their edges mixed with what lies under them; on any other, the font cut to one bit | everything around and between the letters | text over a picture on a screen that is read |
| `DGX_FONT_BG` | the letters, their edges mixed with `bg`, whatever lies under them | everything around and between the letters | text on a paper of a known color where what lies near the letters must stay: a label over a line or a frame that crosses its cell, two lines set closer than the height of the font, a background with a pattern near the color of `bg`; on a screen that is read, the same look as on a display that is not |
| `DGX_FONT_FILL` | the whole cell of the text: `bg` where no letter is, the letters over it | nothing in the cell | text drawn again over itself, a clock, a counter, a value: the old text goes in the same call, with no erase of its own |

The cell of `DGX_FONT_FILL` is as high as the font, from `yOffsetLowest` to
`yBottomMax`, whatever the letters, and runs from `x` to the end of the
advances of the text, wider where a letter sticks out on either side.
Everything else in it is painted over. A text shorter than the one before
leaves the end of that one outside its cell: draw it as long every time,
with spaces after it, which have their advances. `dgx_font_draw_char()` fills
the cell of its one character.

On a display of 16 bits a pixel behind a bus, an ILI9341 or an ST7789 for
one, `DGX_FONT_FILL` sends the cell in one pass, every pixel of it once, so
the old text does not vanish before the new one comes, and it is faster than
`DGX_FONT_BG` there, which sends a window for every run of the letters. On
one of 18 or 24 bits the cell is filled first and the letters drawn after:
there a text that must not blink goes through a virtual screen. On a virtual
screen and a DSI display nothing blinks either way. The times are in
[What it costs in time](fonts-en.md#what-it-costs-in-time).

A font of one bit has no edges: `DGX_FONT_BG` changes nothing for it, and
`DGX_FONT_FILL` fills its cell as for a soft one. `DGX_FONT_MONO` and
`DGX_FONT_HARD` go with either flag: the letters are cut, the paper is as the
flag says.

### Handwritten fonts

A handwritten font keeps every symbol as the path of a pen: Bezier curves in
the order a hand writes them. It is a `dgx_font_t` of type `DGX_FONT_HW`, its
symbols are found like any glyphs, but it is drawn by its own functions
declared in [include/dgx_hw_font.h](../include/dgx_hw_font.h): the size and the
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

[examples/hw_font_demo](../examples/hw_font_demo) fits three lines to the screen
and writes them by hand, [examples/hw_morph_demo](../examples/hw_morph_demo)
morphs words one into another both ways.

One such font is bundled: `font0ss()` from `fonts/font0ss.h`. It has the Latin
alphabet with the German letters `ÄÖÜäöüß`, the Russian alphabet, digits and
punctuation, 155 symbols in all, and nothing else: no accented letters of
other languages, no Greek, no scripts written right to left. Drawing a
handwritten alphabet is slow work and I am not going to draw more of them;
if you need another one, draw it in the editor and send it over, co-authors
are welcome. A font comes from the
[hw-fonts](https://github.com/jef-sure/hw-fonts) editor through `font2c`, see
[font2c/README.md](../font2c/README.md).

Sizing, the thick Bezier curves underneath, writing at the pace of a pen and
both morphs are described in detail in
[docs/handwriting-en.md](handwriting-en.md)
([Russian](handwriting-ru.md)), with two recipes above:
[Handwritten text](../README.md#handwritten-text) and
[Morph handwritten text](../README.md#morph-handwritten-text). The format of a font
and the rules of placing and joining letters are described with the editor,
[font-format-ru.md](https://github.com/jef-sure/hw-fonts/blob/main/font-format-ru.md); how a font is converted and
how it lies in the tables of DGX is in
[docs/hw-font-ru.md](hw-font-ru.md) (both in Russian).

### Dot morphing framework

Enable with `CONFIG_DGX_ENABLE_MORPH`. Everything morphs one bit matrix into
another; declared in [include/dgx_matrix_morph.h](../include/dgx_matrix_morph.h),
[include/dgx_morph.h](../include/dgx_morph.h),
[include/dgx_morph_sources.h](../include/dgx_morph_sources.h) and
[include/dgx_morph_render.h](../include/dgx_morph_render.h):

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

[Morph one glyph into another](../README.md#morph-one-glyph-into-another) is a short
example, and [docs/morphing-en.md](morphing-en.md) walks through all of
it.

### Colors

Which depths a display takes: [Color depths](panels-en.md#color-depths).

The helpers in [include/dgx_colors.h](../include/dgx_colors.h) are parameterized
color constructors, not stored color values. `DGX_RED(rgb_func)` substitutes
the supplied packer into a call with the color's RGB channels. The screen's
pixel format is selected by passing either an uppercase packing macro from
[include/dgx_bits.h](../include/dgx_bits.h) or an inline `dgx_rgb_to_*()` wrapper:

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

A pixel of an 8-bit virtual screen is an index, or an amount of ink, and it
gets its color on the way to the display: `dgx_vscreen8_to_screen16()`,
`dgx_vscreen8_region_to_screen16()` and `dgx_overlay_set()` take a table of
256 colors, `uint16_t lut[256]`, in RGB565.
[include/dgx_lut.h](../include/dgx_lut.h) fills it, and its functions take the
colors in RGB888: one color as a number `0xRRGGBB`, as `dgx_rgb_to_24(r, g, b)`
and `DGX_SKYBLUE(dgx_rgb_to_24)` make it, a palette as `const uint8_t
rgb[][3]`. A color in RGB565, such as `DGX_SKYBLUE(dgx_rgb_to_16)`, compiles
and gives wrong colors. The colors are mixed in RGB888 and only the result is
turned into RGB565, so a fade in many steps gathers no rounding errors.

```c
static uint16_t lut[256];
// paper at 0, ink at 255, as a soft pen and a soft circle want
dgx_lut_gradient(lut, 0, 256, 0x000000, dgx_rgb_to_24(150, 215, 240));
dgx_vscreen8_to_screen16(lcd, 0, 0, band, lut, 0);
```

Each function fills the entries `first .. first + count - 1` only; entries
past 255 are skipped. Entry `k` of a palette goes to `lut[first + k]`, and a
palette may be in flash. Mixing rounds to the nearest, and `t = 0` and
`t = 255` give the two ends exactly. A palette, a fade and a flash step by
step: [A palette, a fade and a flash](../README.md#a-palette-a-fade-and-a-flash).

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
[include/drivers/vscreen.h](../include/drivers/vscreen.h) (color) and
[include/dgx_bw_screen.h](../include/dgx_bw_screen.h) (1-bit):

| Function | Description |
| --- | --- |
| `dgx_vscreen_init(width, height, color_bits, cbo)` | Allocate a color RAM screen. |
| `dgx_vscreen_from_array(v_array, width, height, color_bits, cbo)` | Make a RAM screen over memory the program owns: a camera frame, a decoded picture, a buffer of another library. Nothing is copied, and `dgx_screen_destroy()` leaves the memory alone. Rows lie one right after another, the high byte of a pixel first; when the rows of the memory are longer than the picture, `width` is their length and the picture goes by `dgx_vscreen_region_to_screen()`. |
| `dgx_vscreen_array_set(scr, pixels, width, height)` | Point such a screen at other memory of any size. It costs a few assignments, so one screen can serve every band a renderer gives. |
| `dgx_vscreen_clone(src)` | Allocate a copy with the same geometry and pixels. |
| `dgx_vscreen_copy(dst, src)` | Copy a whole same-sized screen. |
| `dgx_vscreen_to_screen(dst, x, y, src, flags)` | Put a RAM screen on a screen of the same depth: a display or another RAM screen. |
| `dgx_vscreen_region_to_screen(dst, x, y, src, x_src, y_src, w, h, flags)` | The same for a sub-region; `x`, `y` are where the corner of the region goes, scaled and turned as the flags ask. |
| `dgx_vscreen8_to_screen16(dst, x, y, src, lut, flags)` | Expand an 8-bit indexed screen into 16-bit through a LUT, `uint16_t lut[256]` of RGB565 colors: see [Tables for 8-bit screens](#tables-for-8-bit-screens). |
| `dgx_vscreen8_region_to_screen16(dst, x, y, src, x_src, y_src, w, h, lut, flags)` | The same for a sub-region. |
| `dgx_vscreen_to_argb8888(scr, x, y, w, h, lut, dst, dst_stride)` | Read a rectangle of a screen as 0xAARRGGBB colors, the layout of `SDL_PIXELFORMAT_ARGB8888`: for a window on a PC. An 8-bit screen goes through `lut`, or as grey without one. |
| `dgx_overlay_set(display, layer, x, y, lut, alpha)` | Lay an 8-bit virtual screen over a 16-bit screen at `x`, `y` (`dgx_overlay.h`), or move it, or give it other colors: index 0 empty, the others the colors of `lut` mixed with what lies under them by `alpha` (0 to 255, in 32 steps). One layer a screen. Mixed into what comes from virtual screens: `dgx_vscreen8_to_screen16()`, `dgx_vscreen8_region_to_screen16()`, `dgx_vscreen_to_screen()`, `dgx_vscreen_region_to_screen()`, with all their flags; not into what is drawn straight on the screen. |
| `dgx_overlay_set_alphas(display, alphas)` | A share of its own for every index, 0 to 255, times the `alpha` of the layer: a button solid and its shadow faint, the alpha of the layer still fading all. Kept when the layer is moved or gets other colors or another alpha, until it is taken away; NULL for 255 each. The place of the layer becomes changed. |
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
| `DGX_VSCREEN_ALPHA(a)` | The picture over what lies under it by `a`, 0 to 255, in 32 steps: a virtual screen and a display that is read (DSI) give what they have, a display behind a bus that cannot be read has black under it. Steps of `a` make a crossfade, or a fade to black and from it. A pixel under an overlay layer is read with the layer in it. |
| `DGX_VSCREEN_MASK` | Through a table only: the 8-bit picture is a mask, a pixel how much of `lut[255]` goes over what lies under it, 0 to 255, in 32 steps, times `DGX_VSCREEN_ALPHA`. A soft shape drawn once into an 8-bit screen is put on any picture of a virtual screen or a DSI display; on a display that cannot be read the flag changes nothing, so give it a table from the paper to the ink there. |
| `DGX_VSCREEN_EXACT` | Through a table: its colors to the last bit, no hardware that mixes two colors. With `DGX_VSCREEN_ALPHA` on a DSI display, through a table or of 16 bits: mixed by the processor, not by the accelerator, which may give red, green or blue ±1 off, rarely seen by the eye. |

Free any screen with `dgx_screen_destroy(&scr)`.

### Arc gauge

Declared in [include/dgx_gauge.h](../include/dgx_gauge.h):

| Function | Description |
| --- | --- |
| `dgx_gauge_init(gauge, scr, cx, cy, inner_radius, width, start_angle, sweep_degrees, min, max, bg_color, color_fn)` | Configure a ring gauge and draw its empty ring. `color_fn` maps a value to a step color. |
| `dgx_gauge_set_value(gauge, value)` | Update the value, redrawing only changed steps. |
| `dgx_gauge_redraw(gauge)` | Redraw the whole gauge. |

### Bus backends

Declared under [include/bus/](../include/bus):

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
  The kept host keeps the lock of power management ESP-IDF takes with it:
  with `CONFIG_PM_ENABLE` the chip neither goes into light sleep nor lowers
  its clocks while that display exists. A program that wants them puts the
  display on a host it sets up itself, with `dgx_spi_init_on_bus()` or
  `dgx_qspi_init_on_bus()`, where the display holds the host only while it
  draws.
- `dgx_spi_init_on_bus()`, `dgx_qspi_init_on_bus()`, `dgx_i2c_init_on_bus()`
  and `dgx_p8_init_on_bus()` put the display on a bus the program has set up and owns. DGX adds its
  device and removes it on dispose; the program releases the bus afterwards.
  Named an SPI host that `dgx_spi_init()` or `dgx_qspi_init()` has set up,
  the first two add a display of DGX to it, as those constructors do.
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
chips. 12-bit pixels share bytes and cannot be cut so: on an ESP32 and an
ESP32-S2 only the first block of a window reaches the display, and the log
says once that the rest was dropped. 12 bits on this bus are for an
ESP32-S3 or an ESP32-P4.

[include/bus/dgx_dsi_esp32.h](../include/bus/dgx_dsi_esp32.h) also has what a
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
[src/drivers/jd9165.c](../src/drivers/jd9165.c) is such a driver in full.

For a program the same header has `dgx_dsi_show()`, `dgx_dsi_counters()`,
`dgx_dsi_frame_us()`, `dgx_dsi_frame_ended()`, `dgx_dsi_tiles()`, and
`dgx_dsi_bench()`, `dgx_dsi_tune()`, `dgx_dsi_tuning()`, `dgx_dsi_advise()`
and `dgx_dsi_tuning_text()` with the structure `dgx_dsi_tuning_t`.

### Flush control and batching

For a physical display that should not be updated after every primitive, draw
the complete image into a virtual screen in RAM and send it to the panel when
the frame is ready with `dgx_vscreen_to_screen()` (see
[Animation without flicker](../README.md#animation-without-flicker)). The virtual screen is the backbuffer; `in_progress` is not a
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

Declared in [include/dgx_screen.h](../include/dgx_screen.h):

| API | Description |
| --- | --- |
| `dgx_screen_progress_up(scr)` | Open a batch (nesting counter). Returns the new depth. |
| `dgx_screen_progress_down(scr)` | Close a batch; at depth `0` commits the pending dirty area. Returns the new depth. |
| `dgx_screen_touch(scr, left, right, top, bottom)` | Mark an area as changed (inclusive, clipped). For code that writes pixels directly, e.g. into `v_array`. |
| `dgx_screen_flush(scr)` | Commit the pending dirty area now. |
| `dgx_screen_take_dirty(scr, &left, &top, &width, &height)` | Take the pending dirty area and clear it without committing; `false` when nothing has changed. For a virtual screen shown by parts: open a batch before anything is drawn and keep it open, see [Animation without flicker](../README.md#animation-without-flicker). |
| `dgx_screen_destroy(&scr)` | Destroy a screen allocated by a driver or virtual screen constructor. |

```c
dgx_screen_progress_up(scr);
dgx_fill_rectangle(scr, x, y, w, h, bg);
dgx_draw_line(scr, x1, y1, x2, y2, fg);
dgx_font_string_utf8_screen(scr, x, y, "42", fg, DgxOutputNormal, 1, font, NULL, NULL);
dgx_screen_progress_down(scr);   // one update_screen() for everything above
```

A virtual screen of `dgx_vscreen_init()` or `dgx_vscreen_from_array()` has an
`update_screen()` that does nothing. A program that shows such a screen itself
may give it one of its own, and keep what that needs in `user_data`, a
pointer that belongs to the program: DGX neither reads nor writes it, and it is
`NULL` when a screen is made. Then whatever draws on the screen, in batches of
its own or not, sends the changed area by itself:

```c
typedef struct {
    dgx_screen_t   *display;
    const uint16_t *lut;
} shown_t;

/* left, right, top, bottom: both ends inclusive */
static void show(dgx_screen_t *band, int left, int right, int top, int bottom)
{
    const shown_t *s = band->user_data;
    dgx_vscreen8_region_to_screen16(s->display, left, top, band, left, top, right - left + 1, bottom - top + 1, s->lut, 0);
}

static shown_t shown;
shown.display       = lcd;
shown.lut           = lut;
band->user_data     = &shown;
band->update_screen = show;
```

A screen of a driver sends its picture to the display by its `update_screen()`:
do not replace it there. `dgx_overlay_set()` takes over the `update_screen()`
of a layer and calls the one before it after its own work, so give yours
before that call.

The other way is to take the changed area when the program wants it, once a
frame, with `dgx_screen_take_dirty()`, as
[Animation without flicker](../README.md#animation-without-flicker) does. Then a batch
must stay open around all the drawing, including functions that open batches
of their own: any batch that closes at depth 0 passes the area to the
`update_screen()` that does nothing, and the area is lost. Open one batch
before anything is drawn and leave it open.

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
[docs/dsi-usage-en.md](dsi-usage-en.md#several-tasks).

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
with its transport: see [Getting started](../README.md#getting-started).

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
| `CONFIG_DGX_ENABLE_SSD1322` | `drivers/ssd1322.c` | needs SPI or P8; selects `VSCREEN` |
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
| `CONFIG_DGX_BEZIER_IN_IRAM` | nothing | ESP32 only: the code of the pen of Bezier curves is kept in IRAM, 10.3 KB, and 10.5 KB more for straight segments |

The two `IN_IRAM` options are for a handwritten text on an ESP32. There code
and the tables of a font are read from flash through one cache of two ways,
and the time of the same text changes from build to build with where the
linker has put the font. With the code in IRAM it does not. Text in bitmap
fonts needs no such option: its time did not depend on the place of the font,
see [What it costs in time](fonts-en.md#what-it-costs-in-time).
IRAM of an ESP32 is no part of the heap, but there are 128 KB of it and a
program with Wi-Fi leaves a few; see
[docs/pen-en.md](pen-en.md#the-flash-cache).

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

- **A font of 2 or 4 bits a pixel is not fixed by hand.** The font editor
  and its patches work with one bit a pixel: the editor leaves such a font
  as it is, and `font2c` refuses a patch for it. A glyph that comes out
  wrong is fixed in a font of one bit, or by another size or face. See
  [Soft edges](fonts-en.md#soft-edges).
