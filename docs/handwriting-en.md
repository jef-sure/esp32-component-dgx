# Handwritten Text in DGX: Writing and Morphing

[Русская версия](handwriting-ru.md)

A handwritten font keeps every symbol as the path of a pen: Bezier curves in
the order a hand writes them. That is what makes three things possible that a
bitmap font cannot do:

- a text at any size, with a pen of any thickness, letters joined as in
  cursive writing;
- a text that is written before your eyes, stroke by stroke, at the pace of a
  pen;
- a text that turns into another text, curve by curve.

This document describes all three as they are in DGX. The font itself is drawn
in the [hw-fonts][gh-hw-fonts] editor; the format of its file, the measures of
a symbol and the rules of placing and joining letters are described in
[hw-font-ru.md](hw-font-ru.md) (in Russian). Everything here is declared in
[`dgx_hw_font.h`](../include/dgx_hw_font.h).

[gh-hw-fonts]: https://github.com/jef-sure/hw-fonts

## Contents

- [A text at once](#a-text-at-once)
- [Size and place](#size-and-place)
- [What a text is made of](#what-a-text-is-made-of)
- [Bezier curves](#bezier-curves)
- [Writing at the pace of a pen](#writing-at-the-pace-of-a-pen)
- [Morphing](#morphing)
- [What it costs](#what-it-costs)
- [Limits](#limits)
- [Examples and tests](#examples-and-tests)

## A text at once

```c
#include "dgx_hw_font.h"
#include "fonts/font0ss.h"

dgx_hw_draw_text(scr, 10, 80, font0ss(), "Hello, World", 0.3f, 3, color, true);
```

| Parameter | Meaning |
| --- | --- |
| `10, 80` | Where the line starts: its left end, on the base line the letters stand on. Tails of letters go below it. |
| `font0ss()` | The font. `font0ss` is bundled: Latin, German and Russian letters, digits, punctuation. |
| `0.3f` | The size: pixels in one cell of the grid the font is drawn on. A line is `font->hw->symbol_size_y` cells high, 202 in `font0ss`, so 0.3 gives a line of 60 pixels; lowercase letters are `x_height` cells high, 58, that is 17 pixels. |
| `3` | The thickness of the pen in pixels. The font describes only the middle line of the pen. |
| `true` | Joined writing. `false` writes every letter apart. |

`\n` in the text starts the next line, `symbol_size_y` cells lower. A symbol
the font does not have takes the room of a whole cell and draws nothing.

A handwritten font is an ordinary `dgx_font_t`, of type `DGX_FONT_HW`, and
its symbols are found with `dgx_font_find_glyph()` like any glyphs. But
`dgx_font_string_utf8_screen()` and the other functions of bitmap fonts do not
draw it: they take a whole-number scale and one symbol at a time, while here
the size is free and a letter depends on its neighbours.

## Size and place

`dgx_hw_text_box()` tells what room a text takes, in cells, counted from where
its first line starts:

```c
int left, top, right, bottom;
dgx_hw_text_box(font, "Hello", &left, &top, &right, &bottom);
```

`top` is negative: it is above the base line. To fit a text into a width of
`w` pixels and put it in the middle:

```c
float scale = (float)(w - pen) / (right - left + 1);   /* the pen takes half its thickness on each side */
int   x = (int)lroundf((w - (right - left + 1) * scale) / 2 - left * scale);
```

`examples/hw_font_demo` does this for three lines at once: the widest line
must fit the width and all of them, a line height apart, the height.

The box is that of the symbols themselves. The curves joining letters lie
between them and do not widen it.

### A size of its own along each axis

A text may be narrow and tall or wide and low. `dgx_hw_draw_text_xy()`,
`dgx_hw_writing_begin_xy()` and `dgx_hw_morph_draw_xy()` take two sizes in
place of one: pixels in a cell across and pixels in a cell down.

```c
/* digits a third of the grid across and two thirds down */
dgx_hw_draw_text_xy(scr, x, y, font0ss(), "08", 0.33f, 0.7f, 3, color, false);
```

The pen is not stretched: it stays round and as thick as it is given.
Everything counted in cells stays as it is — the box of a text, the placing
and joining of letters, the effort; columns are multiplied by one size and
rows by the other. A curve takes the pieces of the larger of the two sizes.

## What a text is made of

A text is turned into strokes in three steps, each of which can be used on its
own.

**Placing.** `dgx_hw_line_begin()` and `dgx_hw_line_place()` give every next
symbol of a line its place. Symbols stand by their width in the band of
lowercase letters, so a tail above or below takes no room in the line; two
tall symbols are also kept apart by their parts above that band. A
handwritten font is never of fixed width.

**Joining.** With joined writing a letter that has an end connection and a
next one that has a begin connection are joined by one curve made of those two
connections. A connection that has nothing to join is not written, so a word
does not begin or end with a loose tail. Digits and punctuation have no
connections and are written apart; a capital letter goes on into the next
lowercase one but is not joined to the one before.

**Order.** The pen writes a letter, the joint, the next letter. The postponed
strokes — the dots of «ё», the bar of «t» — are written when the pen leaves
the paper: after the last of the joined letters.

`dgx_hw_writer_begin()` and `dgx_hw_writer_next()` give the result: the
strokes of a text one by one in the order of writing.

```c
dgx_hw_writer_t writer;
dgx_hw_stroke_t stroke;
dgx_hw_writer_begin(&writer, font, "ёж", true);
while (dgx_hw_writer_next(&writer, &stroke)) {
    /* stroke.type points in stroke.x[], stroke.y[], in cells from the start of the line */
}
```

A stroke is a dot, a line, a quadratic or a cubic Bezier curve
(`stroke.type` is the number of its points, 1 to 4). It also says how long it
is, how many straight pieces it takes, whether the pen had to be lifted to
begin it (`lift`) and which symbol of the text it belongs to (`symbol`). This
is what `dgx_hw_draw_text()` draws, and what to start from for a drawing of
your own.

## Bezier curves

Everything above ends in drawing thick Bezier curves, and these are a part of
DGX on their own, declared in [`dgx_draw.h`](../include/dgx_draw.h):

| Function | What it does |
| --- | --- |
| `dgx_draw_bezier3(scr, points, pieces, width, color)` | A quadratic curve by three points: start, control point, end. |
| `dgx_draw_bezier4(scr, points, pieces, width, color)` | A cubic curve by four points: start, two control points, end. |
| `dgx_bezier3_begin(&b, ...)`, `dgx_bezier4_begin(&b, ...)` | The same curve prepared in a `dgx_bezier_t`; draws nothing. |
| `dgx_bezier_draw_to(&b, t)` | Draws the prepared curve on until it is `t` complete, 0 to 1; `true` when it is finished. |
| `dgx_bezier_length(&b)` | The length of the prepared curve in pixels. |

```c
dgx_point_2d_t p[4] = {{20, 20}, {120, 20}, {120, 120}, {20, 120}};
dgx_draw_bezier4(scr, p, 0, 5, color);   /* 0 pieces: find the number from the points */
```

`points` are whole pixels, `width` is the thickness of the line, and `pieces`
is how many straight pieces the curve is split into; 0 lets the library find
the number. The line has round ends.

### From a curve to pieces

A curve is drawn as a chain of straight pieces. Its points are turned into
coefficients once, so that a point of the curve is

```
point(t) = ((k0 × t + k1) × t + k2) × t + k3
```

for `x` and for `y`. The pieces are equal in `t`: piece `i` of `n` ends at
`t = i / n`. The ends of the pieces are counted one at a time, as the pen
comes to them, and rounded to pixels; nothing is kept in an array. The last
one is the end point itself.

### How many pieces

Too few pieces and the curve is angular, too many and time is wasted: the
time of drawing follows the number of pieces. The right number depends on how
much the curve bends, not on how long it is.

A chord over `1/n` of the parameter is never further from the curve than an
eighth of the second derivative of the curve over `n` squared. So for the
polyline to stay within `d` of the curve it takes

```
n = ceil(sqrt(bend / (8 × d)))
```

pieces, where `bend` is the largest second derivative: `2 × |P0 − 2·P1 + P2|`
for a quadratic curve, and for a cubic one `6 × |P0 − 2·P1 + P2|` or
`6 × |P1 − 2·P2 + P3|`, whichever is larger: it is the largest at one of the
ends. A straight line with evenly set points gives 1. This is what
`pieces = 0` does, with `d` of half a pixel. The bound is safe but not tight:
it gives about 20% more pieces than are really needed.

The curves of a font have a tighter number. The editor tries the numbers one
by one and keeps, for every curve, the smallest that holds half a cell on its
own grid. At a scale it is

```
n = ceil(pieces × √scale)
```

The root is there because the deviation falls as the square of the number of
pieces: a curve twice as large needs 1.41 times more of them, not twice. The
right half of the «0» of `font0ss`, 169 cells long, takes 7, 10, 13 and 19
pieces at scales of 0.25, 0.5, 1 and 2; a piece every 6 pixels would give 8,
15, 29 and 57.

### A piece is a bullet

The plain way to draw a thick polyline is a thick line for every piece and a
filled circle at every joint, to close the wedge two pieces leave open on the
outer side of a turn. Measured on a CYD, the circles alone took 33–48% of the
time of a symbol. The time followed not the pixels but the number of calls
that fill a row: 1.3–1.9 µs each, almost whatever the length of the row.

So a piece is drawn as one figure with as few rows as can be: a rectangle
with a flat start and a disc of the line width around its end. It is convex,
so every row of pixels of it is one interval and one call. The disc of the
piece before lies around the start, so the joint is closed whatever the turn
is, and the very first piece is given a disc at its start.

```
        flat start                round end
            |                        .-.
   =========+========================   )
            |                        `-'
      (the disc of the piece before is here)
```

For a row the interval is found without any search:

- the rectangle is four straight lines — its two ends and its two sides — and
  each gives a bound of `x` that is a straight function of the row, so the
  row has a left bound, the larger of two, and a right one, the smaller of
  two;
- the disc gives a chord, kept in whole numbers and stepped from row to row;
- the interval of the row is the two together.

Rows next to each other with the same interval go out as one call, so a
vertical piece is one rectangle. The whole font is drawn in 56–63% of the time
the circles took, with 2–3 times fewer calls.

### Even widths

A line of an odd width has a middle pixel, and the points of the curve are
its centres. An even width has none: one side has to be a pixel wider. DGX
gives that pixel to the right of the direction the line goes in, the same way
`dgx_draw_line_thick()` does, so a figure drawn always in one direction —
clockwise, say — is thicker consistently inwards. For a piece this means its
middle line runs half a pixel to the right of its points, and the disc at its
end is centred between pixels.

### Drawing by `t`

`dgx_bezier_draw_to(&b, t)` draws a prepared curve on until it is `t`
complete, and it has two levels.

The first is the pieces. `t` is a share of the length of the whole polyline,
which is counted once, when it is first needed. The pieces that fit into that
way are drawn whole, as bullets.

The second is the piece the pen is on. It is drawn as far as the pen has got:
the part of its rectangle from where the pen stopped last time to where it is
now, along the exact line of the piece, and a round tip. The tip is a disc two
pixels smaller than the line. A full one, centred on a whole pixel next to
the exact place, could stick out of the finished line by a pixel; the smaller
one never does, and the next call covers it.

Because of that, a curve drawn to 1 in any number of calls is, pixel for
pixel, the same picture as the curve drawn at once, and no call ever draws
outside it. A `t` not greater than the one before draws nothing: a curve is
not drawn back.

`t` goes by the way of the pen, not by the parameter or the pieces. Pieces
differ in length many times over — a straight stroke is one long piece, a curl
many short ones — and a pen led piece by piece would crawl and jump.

## Writing at the pace of a pen

A text appears stroke by stroke when it is drawn not at once but up to some
point that moves on with time. That point is measured in effort.

### Effort

Writing an element takes effort:

```
effort = length + k × pieces
```

`length` is the way of the pen in cells. `pieces` stands for how much the
curve bends: a hand slows down in bends, and a curve that bends more takes
more pieces. With `k = 0` the pen moves at an even speed.

There is a least effort. A dot takes it, and so does every move of the pen
through the air to the start of the next stroke; no element takes less.
`dgx_hw_pace()` takes it from the font itself: it is the effort of its
hyphen, since a dot or a lift takes about as long as a hyphen is written.

The effort is counted in cells of the font, so a text takes the same time at
any size. For a feel, in `font0ss`:

| Symbol | Elements | Length | Pieces | Effort, `k = 0` | `k = 4` | `k = 8` |
| --- | --- | --- | --- | --- | --- | --- |
| - | 1 | 38 | 1 | 38 | 42 | 46 |
| с | 2 | 136 | 17 | 136 | 204 | 272 |
| о | 3 | 208 | 23 | 208 | 300 | 392 |
| 1 | 3 | 271 | 5 | 271 | 291 | 311 |
| ж | 5 | 390 | 32 | 390 | 518 | 646 |
| Щ | 5 | 770 | 58 | 770 | 1002 | 1234 |

At `k = 0` the three nearly straight strokes of «1» take longer than the round
«о»; at `k = 4` they are level, and beyond that «о» takes longer.

### Three levels

The pace of the pen is a number: so much effort a second. From it the place
of the pen is found in three steps.

1. **The text.** The efforts of the strokes, with the least effort for every
   lift, are added up in the order of writing. The effort the pen has gone
   through falls into one of the strokes: the ones before it are finished, and
   that one is complete by the share `t` of its own effort.
2. **The curve.** `t` is turned into a way along its straight pieces: a pen
   led by an even `t` moves at an even speed however long each piece is.
3. **The piece.** The pieces the pen has passed are drawn whole, the one it is
   on as far as it has got, ending with a round tip.

Only what is new is drawn each time; nothing is drawn twice or erased.

### In code

```c
dgx_hw_pace_t    pace = dgx_hw_pace(font, 6);          /* k = 6 */
dgx_hw_writing_t writing;
dgx_hw_writing_begin(&writing, scr, x, y, font, text, scale, pen, color, true, &pace);

int64_t start = esp_timer_get_time();
bool    finished = false;
while (!finished) {
    float seconds = (esp_timer_get_time() - start) * 1e-6f;
    finished = dgx_hw_writing_draw_to(&writing, 1500.0f * seconds);   /* 1500 of effort a second */
    vTaskDelay(1);
}
```

`dgx_hw_writing_draw_to()` takes the effort the pen has gone through since the
start of the text. It may be called as often or as seldom as you like: a call
draws everything between the last effort and the new one.

To lead the writing by a progress of 0 to 1, as a morph is led, take the whole
effort of the text first:

```c
float total = dgx_hw_text_effort(font, text, true, &pace);
dgx_hw_writing_draw_to(&writing, t * total);
```

With `k = 6` and 1500 of effort a second the word «привет» takes 2 seconds.

The same works for a single curve: `dgx_bezier4_begin()` prepares it and
`dgx_bezier_draw_to(&curve, t)` draws it on until it is `t` complete.

### To the display by parts

A pen adds a few pixels a step, and the whole band need not go to the display
for them. Every drawing function the writing uses marks the place it has
changed, and a virtual screen gathers those places into one rectangle while a
batch is open on it. The rectangle is taken and only it is sent:

```c
dgx_screen_progress_up(band);   /* the batch stays open: the changes are gathered, not dropped */
for (;;) {
    bool finished = dgx_hw_writing_draw_to(&writing, tempo * seconds());
    int  left, top, width, height;
    if (dgx_screen_take_dirty(band, &left, &top, &width, &height)) {
        dgx_vscreen8_region_to_screen16(screen, band_x + left, band_y + top, band, left, top, width, height, lut, false);
    }
    if (finished) break;
    vTaskDelay(1);
}
dgx_screen_progress_down(band);
```

A frame of a morph is shown the same way: the rectangle after the band is
cleared and the frame is drawn covers both what was erased and what was
drawn.

Timed on an ESP32 with an ILI9341 at 40 MHz, a band of 320 x 100. The whole
band goes to the display in 16.7 ms. A step of the pen, a word written in 400
steps, changes 24 pixels on average and goes in 0.09 ms, 0.1 ms at most. The
place of one digit, 60 x 100, where a morph goes on: 3.2 ms. The time of a
transfer goes by the number of pixels, about 0.52 us a pixel, and 30 us for
the transfer itself however few they are.

### How it is made

**The strokes.** `dgx_hw_writer_next()` is a small state machine that goes
through the text once and keeps no list of strokes. For a symbol it gives, in
turn, the joint with the symbol before, the rest of its begin connection, its
main part and its end connection without the last element. To know whether
that last element goes into a joint it looks one symbol ahead.

The postponed strokes need what was written some letters ago, and there is no
buffer for them either. The writer remembers where the run of joined letters
began — the place in the text and the state of the line — and when the pen
leaves the paper, at a symbol that is not joined to the one before, at a space
or at the end, it goes through that run once more, places the same symbols in
the same places and gives only their postponed strokes.

**The joint.** The last element of the end connection of one letter and the
first element of the begin connection of the next are both raised to cubic
curves, `a` and `b`. Ideally they would be joined by a curve of five points:
`a0`, `a1`, the middle between `a2` and `b1`, `b2`, `b3`. A font has cubic
curves only, so the curve of four points closest to that one is taken. Its
ends are `a0` and `b3`; its second point lies on the line from `a0` to `a1`
and the third on the line from `b3` to `b2`, so the pen leaves one letter and
comes to the other in the directions the connections are drawn in. How far
along those lines the two points stand is found by least squares at seven
points of the curve, and neither comes closer than a quarter of the way, or
the pen would go backwards. The joint gets its number of pieces from the
bound above, at half a cell, and its length along those pieces.

**The pen.** `dgx_hw_writing_t` holds the writer, the one stroke the pen is
on as a prepared curve, the effort at which that stroke begins and the effort
it takes. `dgx_hw_writing_draw_to(effort)` loops: while the effort covers the
whole stroke it finishes the curve and takes the next stroke from the writer;
when it covers a part, it draws the curve to that share and returns. A stroke
that starts where the pen is not begins the least effort later: that is the
pen moving through the air. A dot is a curve with all its points in one; it
has no length to draw by, so it appears when its effort is over.

A stroke goes to the screen as its points times the scale, rounded to pixels.
A line is a curve of one piece, a dot a disc of the pen.

## Morphing

### The idea

A stroke of any kind is exactly a cubic Bezier curve: a quadratic one and a
line are raised to it, a dot is one with all four points in one place, a joint
is one already. So a text is a list of cubic curves in the order of writing.

A morph is a list of pairs of such curves, one from each text. A frame at `t`
is every pair with each of its four points taken between the two curves:

```
point(t) = from + (to − from) × t
```

The curve through those points is drawn like any other. At `t = 0` the frame
is the first text, at `t = 1` the second one.

Unlike writing, a frame is a new picture every time: the one before has to go.
So frames are drawn into a virtual screen and sent to the display from there.

There are two morphs. They differ only in how the pairs are made.

### A line as a whole

```c
dgx_hw_morph_t *morph = dgx_hw_morph_create(font, "привет", "участникам", true);
```

The whole way of the pen of one text goes into the whole way of the pen of
the other, whatever letters they have. «привет» is 23 curves and «участникам»
46, so the shorter way is made longer: its longest curve is cut in two, again
and again, until both have the same number. Cutting a Bezier curve in two
changes nothing in its shape. Then the curves are paired in the order of
writing.

The word changes all at once and evenly; letters do not keep their identity
on the way.

### Letter by letter

```c
dgx_hw_morph_text_t *text = dgx_hw_morph_text_create(font, "привет", "участникам", true);
```

Here every position of the text is a morph of its own, `text->letters[i]`, and
is led by its own `t`, so the letters can change one after another. The symbol
at a position of one text goes into the symbol at the same position of the
other.

Which stroke goes into which is not taken from the order of writing. A symbol
is drawn as a hand writes it, and that is not the order in which the parts of
two symbols answer one another: «9» is written oval first and tail last, «7»
top first, and paired by that order the top of one flies down to become the
tail of the other. So the pairs are chosen so that all the points together go
the shortest way, and a curve may be taken from its other end, which draws
the same. For the digits of `font0ss` the points go 30% less on average than
by the order of writing, and for «2» into «9» four times less. All the ways of
pairing are gone through, which is cheap for a symbol of a few strokes; one of
more than ten is paired in the order of writing.

A stroke left without a pair, and every stroke of a symbol left without a
symbol, grows from a point or shrinks into one. That point is where the pen of
the other text is after that position. So «привет» into «участникам» changes
six letters in place, and the last four grow out of the end of the word.

A position where the symbol is the same and stands in the same place is marked
as unchanged (`changes` is `false`), and `text->changed` tells where the last
changing position is. In a clock going from `12:34` to `12:35` only the last
digit moves. For a clock whose digits stand in places of their own, each place
is simply a morph of one symbol.

With joined writing the curve joining two letters belongs to the second of
them. While neighbours are at different stages it comes off the first one for
a moment.

### Frames

```c
/* an 8-bit band as wide as the screen; color 0 is paper, 255 is ink */
dgx_screen_t *band = dgx_vscreen_init(screen->width, height, 8, DgxScreenRGB);

for (;;) {
    float t = progress(start, esp_timer_get_time(), duration);
    dgx_fill_rectangle(band, 0, 0, band->width, band->height, 0);
    dgx_hw_morph_draw(morph, t, band, 0, base_y, scale, pen, 255);
    dgx_vscreen8_to_screen16(screen, 0, band_y, band, lut, false);
    if (t >= 1) break;
    vTaskDelay(1);
}
dgx_hw_morph_destroy(&morph);
```

`dgx_hw_morph_draw()` erases nothing. `t` is taken as it is; put it through an
easing first if the motion should start and stop softly.

Letter by letter the loop draws every position with its own progress:

```c
for (size_t i = 0; i < text->length; ++i) {
    float t = progress(start + i * stagger, now, letter_duration);
    dgx_hw_morph_draw(text->letters[i], t, band, 0, base_y, scale, pen, 255);
}
```

All the letters are already in their places in the line, so they are drawn
with the same `x` and `y`. `dgx_hw_morph_text_duration_us()` tells when the
last changing letter finishes.

### Where the texts stand

Both texts of a morph are planned from the start of their first lines. To have
each in the middle of the screen, move each by its own shift, in cells:

```c
dgx_hw_morph_shift(morph, shift_from, 0, shift_to, 0);
```

The frames then go from one place to the other. `dgx_hw_morph_text_shift()`
does the same for a letter by letter morph.

`dgx_hw_morph_box()` gives the box no frame leaves. It is the box of the
points of all the curves, which a Bezier curve never leaves, so it may be
somewhat larger than both texts: on the way a curve can go beyond them. Add
half the pen around it.

## What it costs

Measured on a CYD: ESP32 at 240 MHz, `-O2`, drawing into an 8-bit virtual
screen.

| What | Time |
| --- | --- |
| A text of three lines, 204 strokes, at once, scale 0.3, pen 3 | 11.7 ms |
| The same written in 1000 steps | 20.3 ms in all, 20 µs a step, 164 µs at most |
| Going through its strokes without drawing | 1.0 ms |
| The whole effort of it | 2.2 ms |
| A frame of a morph of two words, 46–55 curves, scale 0.4, pen 4 | 2.7–3.7 ms |
| The same with sending a 320×102 band to an ILI9341 at 40 MHz | 47–49 frames a second |

A morph keeps its pairs in memory: 64 bytes a pair and a byte more, about 3 KB
for two words. Writing keeps nothing but its state, a few hundred bytes, on
the stack or wherever you put it. `font0ss` is 17 KB of flash.

## Limits

- The output is not rotated or mirrored: there is no orientation parameter as
  bitmap fonts have, and a text always goes left to right. Rotate the virtual
  screen on its way to the display.
- `font0ss` has the Latin alphabet with the German letters `ÄÖÜäöüß`, the
  Russian alphabet, digits and punctuation, and nothing else. Other alphabets
  are to be drawn in the [hw-fonts][gh-hw-fonts] editor by those who need
  them; a symbol the font does not have leaves an empty cell.
- Lines are not smoothed. An 8-bit virtual screen leaves room for it.
- A morph of several lines knows nothing special about lines: the texts go as
  one way of the pen.
- At small sizes a thick pen hides small things: a comma is 10 cells long,
  2.5 pixels at a scale of 0.25, and looks like a dot.
- The text passed to `dgx_hw_writing_begin()` and `dgx_hw_writer_begin()` is
  not copied and must live as long as the writing goes.

## Examples and tests

- [`examples/hw_font_demo`](../examples/hw_font_demo) fits three lines to the
  screen and writes them by hand at the pace of a pen, in Russian and in
  English.
- [`examples/hw_morph_demo`](../examples/hw_morph_demo) writes a word and
  morphs it into the next ones, as a whole and letter by letter in turn.

The host tests in `test/host` cover every step: `test_bezier` the curves and
drawing by `t`, `test_hw_font` the glyphs, `test_hw_line` the placing,
`test_hw_write` the order of writing, the effort and writing by pace,
`test_hw_morph` both morphs, `test_region` the transfer of a part and that
writing and a frame of a morph mark all they change.
