# Morphing Dotted Structures: A Shared Framework in DGX

[Русская версия](morphing-ru.md)

This is not my first experiment with animating transitions between graphic
shapes. The first used Bezier curves: [a Bezier-curve font][habr-bezier],
where control points move along straight lines and define a new curve at each
step. Then came [the matrix-font clock][habr-clock], where digits and weather
symbols smoothly turn into one another. After that, [morphing generations of
Game of Life][habr-life]. The next experiment was
[cyd-dotview-morphing][gh-dotview-morphing]: morphing a pair of arbitrary
glyphs from an ordinary bitmap font, automatically finding a source for every
new dot.

[habr-bezier]: https://habr.com/ru/articles/818873/
[habr-clock]: https://habr.com/ru/articles/817927/
[habr-life]: https://habr.com/ru/articles/1081830/
[gh-dotview-morphing]: https://github.com/jef-sure/cyd-dotview-morphing
[gh-life-morphing]: https://github.com/jef-sure/cyd-life-morphing
[gh-dotview-clock]: https://github.com/jef-sure/dotview-clock

In the last three projects, it is dots that flow between states. Each time I
copied the same things from one project to the next: a cell matrix, the search
for "what flies where", interpolation, the "head" and "tail", and a brightness
map with phosphor persistence. Only the data changes: digit dots, Life cells,
glyph dots. So in DGX 0.1.0 this code has been moved into a shared component
(`CONFIG_DGX_ENABLE_MORPH`), leaving each project with its own logic: where
the next state comes from and how its sources are chosen.

## The Idea

There are two states: current and next. Both are cell matrices, where each
cell is either lit or not. We scan the whole matrix and calculate
`cell_case = from + to * 2`. This gives four possible values:

| `cell_case` | was | will be | what to draw |
| --- | --- | --- | --- |
| 0 | off | off | nothing |
| 1 | on | off | fade the dot in place |
| 2 | off | on | a dot flies in from somewhere |
| 3 | on | on | the dot stays where it is |

Case 2 is the interesting one. In Game of Life, it is straightforward: when a
cell comes alive, it has exactly 3 live neighbors. Those are its "parents", and
a dot flies from each one into the new cell. A pair of glyphs has no such rule,
so the source has to be chosen. That is where the projects differ, which is
why the framework makes this part a callback and keeps everything else
common.

Case 1 is checked afterward: a cell that does not survive may still have been
a source for another cell's birth. If so, it has already "flown away" and
should not also fade separately. If it was not used as a source, it simply
fades out, without a "head" or "tail", linearly from 255 to 0 as `t` advances.

The framework's model is simple: a finite set of cells transitions to another
finite set along straight segments. This works for cellular automata, dotted
and bitmap fonts, and pixel icons. Curves, outlines and polygons can also be
rasterized into a matrix, but their connectivity and vertex order are not
preserved. That is why the Bezier font does not fit this framework: it animates
curve control points, not lit cells.

### Head and Tail

A dot flying from cell A to cell B is drawn as two disks, one after the other;
their brightness adds together. This creates a "head" and a "tail", with a
bright bridge between them. The head is always at `t`; the tail starts out
still, then catches up: `t_tail = max(t * 1.5 - 0.5, 0)`.

| `t` | head | tail |
| ---: | ---: | ---: |
| 0 | 0 | 0 |
| 1/3 | 1/3 | 0 |
| 2/3 | 2/3 | 1/2 |
| 1 | 1 | 1 |

The tail starts moving at `t = 1/3`, not halfway through as the animation
might suggest, and catches the head at `t = 1`.

Both disks are always drawn, each with half the flight's brightness. At the
start and end they coincide and add back to full brightness. This means a tail
only makes sense with an additive renderer that accumulates brightness. With a
renderer that overwrites pixels, the tail should be disabled.

### Brightness Is Shared by Everyone Drawing a Dot

Each disk is a light source. If every source used full brightness, dots with
multiple incoming sources would overexpose. In Game of Life, a new cell has 3
parents, and each launches 2 disks, so there are 3 × 2 = 6 light sources for
one cell. Each is drawn at 255/6 so their brightness adds up correctly at the
destination. A glyph dot has one source, so each of its two disks is drawn at
half intensity.

The framework splits this rule into two parts. The planner divides 255 among
converging sources (3 parents get 85 each), then drawing divides each flight's
brightness between head and tail. Integer division means the sum may not be
exactly 255: `85 / 2 = 42`, `6 × 42 = 252`. This is not visible to the eye.

One side effect is that a bright dot becomes 3–6 times dimmer when it becomes
a parent. But the head moves ahead, so the change is not distracting; if
anything, it looks natural.

### Phosphor Persistence

To avoid abrupt frame changes, the glow renderer keeps two brightness maps.
`glow_next` collects the brightness of all dots in the current frame, while
`glow_prev` contains the previous result. The new result is:

```c
intensity = glow_next[i] * t_smooth + (1 - t_smooth) * glow_prev[i];
```

where `t_smooth = t * t * (3 - 2 * t)` is [smoothstep][smoothstep]. The result
is written back to `glow_prev`, and `glow_next` is cleared. On the very first
frame there is no previous result, so the current frame is taken as-is.

[smoothstep]: https://en.wikipedia.org/wiki/Smoothstep

Dot motion is linear, as it was in all three source projects: smoothstep is
applied only to light blending. If you want eased motion, use
`dgx_morph_ease()`, but pass the original `t` to the renderer; otherwise the
smoothstep will be applied twice.

The renderer blends each new frame with the previous result, rather than
crossfading two fixed images. Its weight depends only on `t`, while the number
of frames during the transition depends on FPS. More frames mean the old
brightness is multiplied by `1 - t_smooth` more often, so the trail fades
faster. At a lower frame rate, phosphor persistence lasts longer.

## How It Works

The path from data to pixels has four steps:

```
┌────────────────┐    ┌────────────────┐    ┌────────────────┐    ┌────────────────┐
│    SOURCE      │    │     PLAN       │    │     FRAME      │    │   RENDERER     │
│                │    │                │    │                │    │                │
│ "which cells   │ ─▶ │ "what flies    │ ─▶ │ "where are the │ ─▶ │ "draw a dot"   │
│  are lit?"     │    │  where?"       │    │  dots now?"    │    │                │
└────────────────┘    └────────────────┘    └────────────────┘    └────────────────┘
dgx_bit_matrix_t      dgx_morph_create()    dgx_morph_draw()      dgx_morph_dot_func_t
                      once per transition   once per frame        glow or sprite
```

A few decisions determine everything else:

1. **Everything is a matrix.** A glyph, a Game-of-Life generation, or a weather
   icon becomes a `dgx_bit_matrix_t`; a morph is always built between two
   matrices. Glyphs are just one source of matrices.
2. **The plan uses cells; pixels are handled only when drawing.** A morph knows
   neither the cell size nor its screen position. These are passed to
   `dgx_morph_draw()`, so the same morph can be drawn anywhere and at any scale.
3. **Source selection is a callback.** The framework handles everything else:
   cell classification, brightness sharing, tracking used sources, and memory.
4. **Frames are stateless.** The application keeps time. Several morphs can be
   drawn with the same `t`, or with different values, for example to make the
   letters of a word morph in sequence.
5. **The renderer is also a callback**:
   `void (*)(void *, const dgx_point_2d_t *, uint8_t)`. A glow renderer lives
   longer than the morphs; its "phosphor" smooths transitions between
   generations.

### Source: Matrices

`dgx_bit_matrix_t` is a packed grid, one bit per cell, with fast
`dgx_matrix_get_point()`/`dgx_matrix_set_point()` accessors and a count of lit
cells. For Game of Life, this is the field itself; custom data can be filled in
manually.

For glyphs, use `dgx_morph_glyph_matrix(font, code_point)`. Every glyph from a
font gets a matrix with the same font-wide bounds and baseline, so a narrow
`1` and a wider `8` line up correctly. A space or missing character is simply
an empty matrix. This works with both dotted and bitmap fonts.

The shared bounds come from font metrics: `xOffsetLowest`..`xRightMax`
horizontally and `yOffsetLowest`..`yBottomMax` vertically. For fonts generated
before these metrics existed, the bounds are calculated by scanning all
glyphs.

For example, `TerminusTTFMedium12` has a 6 × 12 cell box. On a 320 × 240
screen, the largest cell size is `min(320 / 6, 240 / 12) = min(53, 20) = 20`,
so the image is 120 × 240 pixels.

#### Dotted Fonts

DGX fonts can be stored as bitmaps (`DGX_FONT_BITMAP_LINES`, or the legacy
`DGX_FONT_BITMAP_STREAM`) or as lists of dot coordinates (`DGX_FONT_DOTS`).
The dotted format is especially useful for animation:

```c
// src/fonts/CasusDotView.c: '0', 27 dots
static const dgx_font_dot_t dots[] = {
    {2, 2}, {1, 3}, {1, 4}, {5, 4}, {5, 5}, {5, 6}, {4, 6}, {3, 7},
    {2, 8}, {1, 6}, {1, 5}, {3, 2}, {4, 2}, {5, 3},
    {5, 10}, {5, 11}, {5, 9}, {5, 8}, {5, 7}, {4, 12}, {3, 12}, {2, 12},
    {1, 11}, {1, 10}, {1, 9}, {1, 8}, {1, 7},          // ... '0'
    {1, 4}, {2, 3}, {3, 2}, /* ... dots for '1' */      // dots + 27
};
```

In the clock, the order of dots in the array is the choreography: dot `i`
flies to dot `i` in the other glyph. The dots are arranged so unchanged parts
stay in place and changing parts cross less. I made the digits and a couple of
degree symbols in `CasusDotView` myself. For `WeatherIconsRegular13`, I started
with a weather-icon font and refined the icons by hand in
[FontCreator](https://github.com/Llerr/FontCreator), which lets you assign an
index to each dot while editing a glyph.

A matrix does not preserve dot order, so it does not matter for matrix-based
morphing: sources are selected geometrically. The clock's index-based morph
remains in `dgx_font_make_morph_struct()`, as before.

### Plan: What Flies Where

```c
dgx_morph_t *dgx_morph_create(const dgx_bit_matrix_t *from,
                              const dgx_bit_matrix_t *to,
                              dgx_morph_sources_func_t sources,
                              void *user_data);
```

The framework scans the union of the two matrices (their dimensions may
differ) and classifies the cells:

- Set in both: a static dot with brightness 255.
- Set only in `to`: ask the callback where the dot flies from.
- Set only in `from` and not used as a source: fade in place.

There is one special case: an empty `to`, for example when a glyph turns into
a space. Nothing fades in place; all dots converge on the center of the matrix
and fade from 255 to 0, as in cyd-dotview-morphing. The reverse case, an empty
`from`, happens naturally: new cells have no sources, so they appear from the
center.

The callback receives the coordinates of a new cell and returns a list of
sources:

```c
typedef int (*dgx_morph_sources_func_t)(const dgx_morph_ctx_t *ctx, int x, int y,
                                        int pass, dgx_point_2d_t out[8],
                                        void *user_data);
```

Return N sources and a dot flies from each one with brightness `255/N`; the
sources are marked as used. Return 0 and the dot appears from the center of the
matrix with brightness increasing from 0 to 255. Return `DGX_MORPH_DEFER` and
the cell is retried on the next pass, after every cell in the current pass has
been assigned a source. A cell may be deferred as many times as there are
vectors to every cell of the grid, and 8 times at least; one still deferred
on the last pass appears from the center. That is a large number: 4 r (r + 1)
for a grid r + 1 cells wide, hundreds of thousands of calls for one cell of a
320 x 240 grid. A callback that defers should give its last answer as soon as
it knows there will be no other.

A callback is a call per waiting cell per pass, and that is what it costs.
The planner does the search of `dgx_morph_sources_cells` itself, on words of
bits, when it is given that very function; asked through a callback that only
calls it, the same search with the same plan takes 6 to 16 times longer on
average and up to 50 times at worst. For the weather symbols on an ESP32 at
240 MHz, a plan in milliseconds, on average and at most:

| transitions | the planner itself | through a callback |
| --- | --- | --- |
| day and night of the same weather | 1.6, 4.8 | 11, 68 |
| weather that follows | 1.6, 4.2 | 10, 65 |
| any weather to any other | 2.6, 7.8 | 42, 412 |

So a callback that defers pass after pass is for a search of another kind,
not for a variation of this one; one that answers on its first or second
pass, as `dgx_morph_sources_life` does, costs next to nothing.

Inside the callback, `dgx_morph_was_set(ctx, x, y)` tells whether a cell was
set in `from`, and `dgx_morph_is_used(ctx, x, y)` tells whether it has already
been used. There are two built-in callbacks, one for each source project.

#### Game of Life: Every Live Neighbor Is a Parent

```c
int dgx_morph_sources_life(const dgx_morph_ctx_t *ctx, int x, int y, int pass,
                           dgx_point_2d_t out[8], void *user_data)
{
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        if (dgx_morph_was_set(ctx, x + nx[i], y + ny[i])) {
            out[n++] = (dgx_point_2d_t){ x + nx[i], y + ny[i] };
        }
    }
    return n;
}
```

This callback does not check whether a source is already used: one parent may
contribute to several births, and a surviving cell may also be a parent. A
dying parent is marked used and does not fade separately; it flies into the
new cell.

#### Glyphs: The Nearest Source, Vector by Vector

A pair of glyphs has no birth rule, so dotview-morphing turns the idea of
"parents" into a geometric rule: a new dot flies in from the nearest dot of
the old glyph that feeds nothing yet. A static dot may also be a source: it
stays lit and launches a flight as well.

The search goes not cell by cell but vector by vector. A pass is one vector,
the same for the whole grid: every new cell still without a source looks at
the one cell that lies by that vector from it, and takes it if it is an unused
dot of the old glyph. Then the next vector, again for the whole grid.

The vectors go ring by ring, radius 1, then 2 and so on. On a ring the axes
come first: up, down, right, left. Then one cell away from each axis toward
the corners: from the top to the right and to the left, from the bottom to the
right and to the left, from the right up and down, from the left up and down.
Then two cells away in the same order, and so on to the corners. For radius 2,
with `X` the new cell:

```
14  6  1  5 13
11  .  .  .  9
 4  .  X  .  3
12  .  .  . 10
16  8  2  7 15
```

Two things follow. A line that has moved aside by a vector is found by that
vector as a whole, each of its cells flying from its own cell of the old
line, so the line moves instead of scattering. And the order in which the
cells are asked decides nothing: two cells never look at the same cell on a
pass, so none can take a source from another. Only the order of the vectors is
a preference, and some order there has to be.

It was not always so. At first there were two passes: neighbors, and then,
cell after cell, all the rings at once; a cell early in the scan could take a
source four cells away which a cell after it had three cells away. On the
weather symbols of `WeatherIconsRegular27` the flights are now 17% shorter on
average, 4.1 cells instead of 5.0, and those of four cells and more a fifth
fewer.

For example, suppose the source has one dot at (7, 5) and the target has one
at (5, 2). Nothing is found on the rings of radius 1 and 2; on the ring of
radius 3 the vector (2, 3) finds (7, 5), turning what would have been an
appearance from the center into a flight from (7, 5) to (5, 2).

No dot appears from nowhere while an unused source exists, so the morph looks
like a flow. Once the sources run out, the cells left appear from the center
at once.

Asked through the callback, this is one call per waiting cell per vector. The
planner does the same on rows of bits when it is given
`dgx_morph_sources_cells` itself: the cells of a row still waiting, and the
unused sources of the row a vector points to moved back by it, meet in one
AND of two words. A ring on which no waiting cell has anything is passed
without its vectors, which is seen from bits kept by rows and by columns, four
looks a cell. A vector is passed over as well when it cannot lead from the box
of the waiting cells into the box of the unused sources; the boxes shrink as
the cells are served, and the search ends at the ring that is further than
both.

Timed on an ESP32 with the weather symbols a clock shows, 42 x 39 cells. Day
and night of the same weather, or weather that follows one another, such as
clouds and clouds with rain: 1.6 ms a plan on average, under 5 ms at most. Any
weather to any other: 2.6 ms on average, 8 ms at most. Of that 0.5 ms is the
plan without any search. The plan is built once per transition, not once per
frame.

The same on other chips, a plan in milliseconds, on average and at most:

| chip | clock | weather that follows | any weather to any other | without search | speed against the ESP32 |
| --- | --- | --- | --- | --- | --- |
| ESP32-P4, rev 1.3 | 360 MHz | 0.7, 1.9 | 1.2, 3.6 | 0.2 | 120% higher |
| ESP32-S3, rev 0.2 | 240 MHz | 1.2, 3.4 | 2.1, 6.3 | 0.3 | 31% higher |
| ESP32-D0WD-V3, rev 3.1 | 240 MHz | 1.6, 4.2 | 2.6, 7.8 | 0.5 | |
| ESP32-C3, rev 0.4 | 160 MHz | 1.8, 4.7 | 3.0, 9.0 | 0.5 | 10% lower |
| ESP32-C6FH4, rev 0.2 | 160 MHz | 1.8, 4.8 | 3.0, 9.3 | 0.5 | 11% lower |

The speed in the last column is taken from the average time of a plan with
the search over all the pairs of symbols timed; it changes by a few percent
from one set of pairs to another. The ESP32-P4 here is an engineering sample.

#### Within a Radius

`dgx_morph_sources_cells_within` is the same search kept within a radius. A
new cell that has no unused cell within it appears from the center of the
grid at once, an old cell that nobody took fades where it is, and the rings
beyond the radius are not gone through. The radius, in cells, is an `int`
that `user_data` points to; NULL or a radius below 1 is no limit.

```c
int radius = 6;
morph = dgx_morph_create(from, to, dgx_morph_sources_cells_within, &radius);
```

The planner does this search itself as well, on words of bits. What a radius
gives and what it takes, for any weather symbol into any other on an ESP32 at
240 MHz: the time of a plan on average and at most, the share of the new
cells that appear from the center instead of flying, and how far those that
fly go on average.

| radius | plan, ms | from the center | flight, cells |
| --- | --- | --- | --- |
| 3 | 1.1, 1.7 | 45% | 1.7 |
| 5 | 1.4, 2.5 | 33% | 2.2 |
| 8 | 1.9, 3.8 | 23% | 2.8 |
| 12 | 2.2, 4.9 | 17% | 3.3 |
| no limit | 2.6, 7.8 | 10% | 4.5 |

A small radius makes a calm morph, where nothing crosses the picture, at the
price of more dots that simply light up; without a limit every dot that can
fly does.

In a callback of your own, `dgx_morph_scan_vector()` gives the vector of a
pass, `dgx_morph_ring_at()` looks at the whole ring of one radius and
`dgx_morph_ring_find()` at all the rings from a radius up.

### Frame: Where the Dots Are Now

```c
float t = dgx_morph_progress(t0, esp_timer_get_time(), 1000000);
dgx_morph_draw(morph, t, x, y, cell_width, true, dgx_morph_glow_dot, glow);
```

`dgx_morph_progress()` computes `(now - start) / duration`, clamped to [0, 1].
`dgx_morph_draw()` emits every dot to the renderer callback in pixel
coordinates: cell (cx, cy) maps to `(x + cx * cell + cell / 2, y + cy * cell + cell / 2)`.
Flights emit the head and, if enabled, the tail at half brightness each;
static dots use 255; fading dots use `255 * (1 - t)`.

Frames have no state; the morph is only read, and the application keeps time.
Several morphs at different screen positions can therefore use the same `t`,
like clock digits, or be staggered so the letters of a word morph one after
another:

```c
for (int i = 0; i < letters; ++i) {
    float t = dgx_morph_progress(t0 + i * delay_us, now, duration_us);
    dgx_morph_draw(morph[i], t, x0 + i * letter_w, y0, cell, false, dot, ud);
}
```

`dgx_morph_text_create()` builds the morphs for such a line: it takes two
UTF-8 strings, pads the shorter one with spaces and creates one morph per
letter in the font-wide box. The `changed` field tells how many letters from
the left actually change, and `dgx_morph_text_duration_us()` computes when the
last of them finishes, so trailing identical spaces do not stretch the
transition:

```c
dgx_morph_text_t *text = dgx_morph_text_create(font, "hello", "world", 0,
                                               dgx_morph_sources_cells, NULL);
int64_t total_us = dgx_morph_text_duration_us(text, duration_us, delay_us);
/* draw text->letters[i] with the loop above */
dgx_morph_text_destroy(&text);
```

No synchronization is needed. One thing to keep in mind with glow is that a
renderer has one frame blend for its entire image and accepts a single `t`.
For letters with independent progress, it is simplest to create one glow
renderer per letter, sized to that letter. The total memory is about the same
as one renderer for the whole word, while each letter blends using its own
`t`. A dot extends `cell / 4` beyond the edge of its cell, so leave that much
margin around each letter's glow area. Keep neighboring margins from
overlapping: `present()` copies the whole rectangle, including its black
background.

### Renderers

#### Glow: Disks with a Soft Falloff

A dot is rendered as a disk with radius `R = cell / 2 + cell / 4` and
brightness `255 * (1 - 3x² + 2x³)`, where `x = d² / R²`. If the calculated
radius is below 2 pixels, `R = 2` is used. The disks accumulate into an 8-bit
brightness map, which is then blended with the previous frame
(see Phosphor Persistence above). Brightness is mapped to color when the frame
is presented, using a precomputed 256-entry LUT in the screen's color format:
16, 18, or 24 bits. The default LUT is grayscale.

```c
dgx_morph_glow_t *glow = dgx_morph_glow_create(width, height, cell_width, 16);
dgx_morph_glow_set_lut(glow, my_lut);   // can be changed at runtime
// each frame:
dgx_morph_draw(morph, t, 0, 0, cell_width, true, dgx_morph_glow_dot, glow);
dgx_morph_glow_present(glow, t, screen, ox, oy);
```

The LUT is a color layer, not part of the morph. As in the clock, where digit
color follows the time of day and weather color follows the temperature, the
table can be recalculated on the fly. Dots can glow copper, white, blue or
red without changing their paths.

The renderer owns its virtual screen. `present()` blends the frame and draws
it on the screen at (ox, oy). `dgx_morph_glow_reset()` clears the phosphor
when you want to start fresh. There is no need to recreate the renderer
between morphs; its retained phosphor is what smooths transitions between
generations.

Memory usage is two brightness maps at one byte per pixel, plus the virtual
screen: two bytes per pixel for 16-bit color and three for 18- or 24-bit color.
A 240 × 240 image on a 16-bit screen therefore uses 230,400 bytes. That is a
lot for an ESP32 without PSRAM, so the Game-of-Life project chooses the cell
size based on available memory.

A frame can be given one more filter right before it goes to the screen:

```c
static const int passes = 1;
dgx_morph_glow_set_filter(glow, dgx_morph_glow_blur, (void *)&passes);
```

The filter gets a copy of the finished frame as a brightness map, one byte per
pixel, and what it leaves is mapped to colors and shown. Nothing it does gets
into the phosphor or into the frames that follow, so it can be switched on and
off at any moment. `dgx_morph_glow_blur()` is a ready one. Glyphs of an
outline font turned into dots keep the steps of its one-bit picture, weather
symbols with their thin slanted lines most of all, and a pass or two of blur
smooths them out. Your own filter is any function of the same signature. The
copy costs one more byte per pixel; on a 216 × 203 frame one pass of blur
takes the CYD glyph demo from 44 to 35 frames a second, two passes to 31.

#### Sprite: A Dot as a Grayscale Matrix

As in the clock, a dot is a small 8-bit grayscale matrix. Sprites of 1 to 4
pixels are drawn by hand; larger ones are generated from filled circles with
brightness falling toward the edge (`dgx_font_make_point8()`). When drawn,
each sprite pixel is multiplied by the dot's intensity and passed through the
same kind of 256-entry LUT as glow. Zero-valued pixels are transparent.

```c
dgx_morph_sprite_t *sprite = dgx_morph_sprite_create(8);
dgx_morph_sprite_set_lut(sprite, my_lut);   // recolor at runtime
dgx_morph_sprite_set_target(sprite, vscreen);
dgx_morph_draw(morph, t, x, y, cell, false, dgx_morph_sprite_dot, sprite);
```

The sprite overwrites pixels instead of adding brightness, so trails are
disabled with it.

## Optimizations for a Microcontroller

Glow accumulation is the hottest part of the renderer. The optimizations from
the Game-of-Life project remain, with a few additions.

**Precomputed brightness.** Brightness as a function of distance from the
disk center is calculated once when the renderer is created. The lookup index
is squared distance, so no square root is needed in the inner loop.

**Skip the corners.** Points outside the brightness disk contribute nothing,
so there is no reason to scan them. A table of X extents is precomputed, with
`dy` as its index. For `R = 13`, this skips 212 of 729 points, about 29%.

**Division with shifts.** Instead of dividing by 255 for values in 0..255:

```c
static inline uint32_t div255(uint32_t n)
{
    return (n + 1 + (n >> 8)) >> 8;
}
```

**Fixed-point frame blending.** The blend coefficient is calculated once
before the pixel loop; inside, only integer arithmetic remains:

```c
uint32_t blend = (uint32_t)(256.0f * smoothstep3(t));
for (size_t i = 0; i < pixels; ++i) {
    uint8_t v = (next[i] * blend + prev[i] * (256 - blend)) >> 8;
    /* ... */
}
```

New compared with the Game-of-Life project:

- **X symmetry.** Contributions for `px - dx` and `px + dx` are identical, so
  each is calculated once and written to both sides. This halves the work in
  the inner loop.
- **Squared distance without multiplication.** Instead of calculating
  `dx * dx` at each step, add the difference `dx² - (dx - 1)² = 2 * dx - 1`.
- **LUT with byte-swapped entries.** For a 16-bit screen, the color table is
  stored in the same byte order as pixels in memory, leaving one table lookup
  and one write per pixel.

## Full Example: Game of Life

```c
int cell = 16;
dgx_morph_glow_t *glow = dgx_morph_glow_create(15 * cell, 15 * cell, cell, 16);
dgx_bit_matrix_t *now = create_pulsar();

for (;;) {
    dgx_bit_matrix_t *next = next_generation(now);
    dgx_morph_t *morph = dgx_morph_create(now, next, dgx_morph_sources_life, NULL);
    int64_t t0 = esp_timer_get_time();
    float t;
    do {
        t = dgx_morph_progress(t0, esp_timer_get_time(), 1000000);
        dgx_morph_draw(morph, t, 0, 0, cell, true, dgx_morph_glow_dot, glow);
        dgx_morph_glow_present(glow, t, screen, ox, oy);
        vTaskDelay(1);
    } while (t < 1.0f);
    dgx_morph_destroy(&morph);
    dgx_matrix_destroy(&now);
    now = next;
}
```

A 15 × 15 pulsar with cell size 16 is 240 × 240 pixels and has 48, 56 or 72
live cells depending on its phase. The original ESP32 setup (CYD, ILI9341
320 × 240) ran at 25–26 FPS.

A glyph pair looks much the same; only the source matrices and callback
change:

```c
dgx_bit_matrix_t *from = dgx_morph_glyph_matrix(font, cp_from);
dgx_bit_matrix_t *to = dgx_morph_glyph_matrix(font, cp_to);
dgx_morph_t *morph = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
```

## How the Projects Fit

The first two projects, rewritten this way, are in the examples:
`examples/glyph_morph_demo` and `examples/life_morph_demo`.

**[cyd-dotview-morphing][gh-dotview-morphing].** From roughly 800 lines in
`main.c`, the cell matrix, segment vector, `create_cell_morphing()` with its
ring search (about 300 lines), the glow code, and `collect_initial_glow()` go
away. The first frame is now taken as-is, replacing the phosphor pre-warm.
Another 316 lines of unused wrappers disappear. Display initialization, the
symbol loop and timing remain.

**[cyd-life-morphing][gh-life-morphing].** `LifeTransformation`,
`collect_glow()`, `draw_life_transformation()` and buffer management go away,
about 300 of 714 lines. `LifeGeneration` becomes `dgx_bit_matrix_t`: checking
for extinction becomes `number_of_set_cells == 0`; generation comparison can
remain `memcmp`. The rules, patterns, restart logic and button remain in the
project. The plan is now built once per generation instead of being rebuilt
every frame as in the original.

**[dotview-clock][gh-dotview-clock].** The clock stays as it is. Its morph is
index-based and depends on point order in the font, which a matrix does not
preserve. Besides, the clock's morph code is already only a few lines:
`dgx_font_make_morph_struct()` is in DGX, and drawing is a loop over points
that blits the sprite.

## Pitfalls

These are real issues found by tests and while comparing against the source
projects.

**`continue` in a `for` loop does not skip its increments.** The first version
of `dgx_matrix_foreach_set()` skipped zero bytes with `x += 7; continue;`, but
the loop still applied its own `x++, idx++`. The counters drifted at every
byte and coordinates shifted. It was rewritten as a flat `while` over the
linear index.

**Neighbor order matters.** In dotview-morphing, neighbors are checked axes
first; in Game of Life they are scanned row by row. When only one source is
needed, the first match wins. With a shared order, CELLS chose a diagonal
where the original chose the neighbor above. The callback now defines the
order.

**Static dots can also be sources.** The first CELLS version reserved static
dots, which changed the result from the original. There, a static dot stays
lit and can also launch a flight into a neighboring cell.

**Zero configuration is not the same as a default configuration.** The first
version had a player object with trail settings. A compound literal such as
`(config_t){ .duration_us = ... }` zeroed the trail coefficients and left the
tail stuck at the start forever. The config structs were removed from the API.

**Font metrics.** The bundled fonts did not have `yBottomMax`, so the bottom
rows of `0` in `CasusDotView` were lost: the box was 12 cells high instead of
14. Also, `xWidest` is the maximum glyph width, not the right edge. In
`VerdanaRegular32`, the right edge is 32, while `xWidest - xOffsetLowest` is
31. That is why `xRightMax` was added and all bundled fonts now have the
metrics.

**A dot outside the screen.** In the first glow implementation, the center
column was written without checking X. A dot left of the screen wrote into
the previous row; in the first row, it wrote before the buffer.

**The number of flights is not known in advance.** A new Game-of-Life cell has
3 parents, but a callback may return up to 8. A fixed-size flight array
silently overflowed, and the program later crashed in an unrelated `free()`.
The array now grows as needed.

**Duplicate points in font data.** Two icons in `WeatherIconsRegular13` have
dots with identical coordinates: 6 in U+F00C and 1 in U+F010. A matrix
collapses these duplicates; in the clock's index-based morph, two dots flew
to the same place, which was not noticeable by eye.

## What Is Still Open

1. **The clock's index-based order.** A morph based on font point order does
   not fit the matrix model and remains separate.
2. **Retargeting mid-flight.** A new morph can start from the last discrete
   state and the phosphor will smooth brightness, but flying dots may jump to
   those discrete positions.
3. **Persistence depends on FPS.** To make it frame-rate independent, the
   decay coefficient should be calculated from the time between frames.
4. **The tail is the same for every flight.** The lag `t * 1.5 - 0.5` does not
   depend on flight length: the tail behaves the same for a one-cell hop and
   a flight across half the screen. It would make more sense to adapt the lag
   to distance.
5. **There is no thread safety.** A morph is read-only after creation, so it
   is safe to draw it from one task. Other concurrent access requires
   external synchronization.

## Tests

Host tests live in `test/host`. They are built with gcc using ESP-IDF stubs
and run under ASan/UBSan with `make -C test/host`. They cover:

- a Life blinker: 6 flights at 85 brightness, one static dot, no fading dots;
- CELLS: flight (7, 5) → (5, 2) via a ring, static source, axis neighbor before
  diagonal, unused diagonal fades;
- empty `from`: appearance from the center; endless `DGX_MORPH_DEFER` does
  not loop forever;
- matrices of different sizes, `NULL`, and a dense checkerboard that grows
  the flight array;
- drawing: cell-to-pixel mapping; two half-intensity stamps at the end of a
  flight add to 254;
- glyph matrices: equal bounds for different glyphs, every `CasusDotView`
  dot preserved, glyph-to-space collapses all dots to the center;
- glow dots outside screen bounds, the disk radius for small cells, frame
  blending; sprite rendering to a virtual screen;
- `dgx_morph_text`: space padding, number of changed letters, duration;
- a morph wider than 32767 cells is rejected, dots outside `int16_t` are not
  drawn;
- matrix copy, comparison and clearing, a matrix from a 1-bpp bitmap;
- metrics for all 11 bundled fonts match their glyph data, and
  `TerminusTTFMedium12` has every letter of the example's words.

Hardware-dependent behavior is not covered on the host: visual appearance,
FPS and SPI output.

## Conclusion

This is what I wanted: instead of three copies of the same code, one component
where each project only has to say where the next state comes from and how to
choose sources for new dots. The hardest part was not the morph itself, but
making the shared code behave like the originals: neighbor order, static dots
and font metrics.

The code is available in [esp32-component-dgx](https://github.com/jef-sure/esp32-component-dgx)
under the MIT license. If you find a bug or know how to squeeze out more
performance, let me know.
