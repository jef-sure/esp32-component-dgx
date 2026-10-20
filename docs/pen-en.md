# The pen in DGX: what a thick line is made of

[Русская версия](pen-ru.md)

A technical note on how `src/draw/bezier.c` draws a curve with a pen: what figures
it puts on a screen, what it counts with, what it calls and what that costs
on an ESP32. How to use a pen is in [handwriting-en.md](handwriting-en.md);
here is only how it is built.

It describes the code of version 0.5.4. Every number is either
measured on the board or marked as calculated or as counted on the host. A KB
is 1024 bytes.

## Contents

- [What it was measured on](#what-it-was-measured-on)
- [Words](#words)
- [Two pens](#two-pens)
- [The hard pen](#the-hard-pen)
- [The soft pen](#the-soft-pen)
- [What a pen calls](#what-a-pen-calls)
- [The flash cache](#the-flash-cache)
- [What costs what](#what-costs-what)
- [What was tried and did not stay](#what-was-tried-and-did-not-stay)
- [Not done](#not-done)

## What it was measured on

| What | Which |
| --- | --- |
| Board | CYD, ESP32-2432S028R, an ILI9341 display of 320×240 |
| Chip | ESP32-D0WD-V3, revision v3.1 by `esptool`, two cores, a 40 MHz crystal |
| Frequency | 240 MHz |
| Flash | 4 MB, DIO mode, 40 MHz |
| Flash cache | 32 KB, two ways of 16 KB, a line of 32 bytes, one for code and data |
| ESP-IDF | 5.5.4 |
| Compiler | xtensa-esp-elf-gcc 14.2.0 (esp-14.2.0_20260121), `-O2` |
| Where it draws | an 8-bit virtual screen in memory; the display takes no part in the timings |
| What it is timed with | `esp_timer_get_time()` around the call, the mean of 50–300 runs |

There are two texts in the timings. «The name of the place» is the line
«Neustadt an der Weinstraße» in a band of 320×46 at a scale of 0.188 × 0.208,
the way the clock writes it: 89 strokes, 338 pieces. «The text» is «The quick
brown fox» at scales of 0.25, 0.5 and 1.

One timing was repeated on a second board: a Guition JC1060P470, an ESP32-P4
of revision v1.3, 360 MHz, a second-level cache of 128 KB with a line of
64 bytes, as ESP-IDF has it by default. It is in
[«The flash cache»](#the-flash-cache).

## Words

**A curve** is a quadratic or a cubic Bezier curve with whole ends.

**A piece** is a straight bit a curve is replaced with; how many there are is
said by the one who draws, or the library finds it from the points. How, is in
[handwriting-en.md](handwriting-en.md#how-many-pieces).

**A joint** is where one piece of a curve ends and the next begins.

**A figure** is what a pen puts on a screen at one go: each pen has its own.

**A row** is the pixels of a figure in one row of the screen. A convex figure
has one interval there.

**A stroke** is one curve of a handwritten font. A text gives the pen stroke
after stroke; **the layout** is that work, and going through the strokes
without drawing is the layout alone, with a pen of width 0.

**Ink** is the sum of the bytes of an 8-bit screen divided by 255: how many
whole pixels are painted.

## Two pens

A pen is given by a width and a color, and its kind by what is added to the
width: `DGX_PEN_SOFT | 2` asks for a soft edge. The kind of a pen belongs to
the drawing, not to the screen: two pens write on one screen in turn. The
first two versions were different, and both were turned down. At first
softness was switched on for a screen by a call of its own,
`dgx_vscreen8_soft_pen()`; that call is gone. Then a pen was soft on any
8-bit screen by itself. Either way the screen chose the pen
one draws with, and no hard pen was left on an 8-bit screen; it is needed
there for taking lines off, if for nothing else.

How the pens differ in what they are built of:

| | Hard | Soft |
| --- | --- | --- |
| A pixel | the line or not the line | a share of the ink, 0…1 |
| The ends of the pieces | on whole pixels | as they are |
| The figure of a piece | a rectangle with a disc at its end | a bar with its ends cut straight |
| A joint | the disc of the piece before | a corner or the wedge of a disc |
| Writing a pixel | the color over what was there | the larger of the two |
| Counting in a row | three straight lines in `float`, a chord in whole numbers | two additions in whole numbers a pixel |
| Where it draws | any screen | an 8-bit virtual screen |

## The hard pen

### A bullet

The plain way to draw a thick polyline is a thick line for every piece and a
filled circle at every joint, to close the wedge two pieces leave open on the
outer side of a turn. Measured on a CYD, the circles alone took 33–48% of the
time of a symbol. The time followed not the pixels but the number of calls
that fill a row: 1.3–1.9 µs each, almost whatever the length of the row.

So a piece is drawn as one figure with as few rows as can be: a rectangle
with a flat start and a disc of the line width around its end. It is convex,
so every row of pixels of it is one interval. The disc of the piece before
lies around the start, so the joint is closed whatever the turn is, and the
very first piece is given a disc at its start.

```
        flat start                round end
            |                        .-.
   =========+========================   )
            |                        `-'
      (the disc of the piece before is here)
```

For a row the interval is found without a search:

- the rectangle is four straight lines, its two ends and its two sides, and
  each gives a bound of `x` that is a straight function of the row; a row has
  a left bound, the larger of two, and a right one, the smaller of two. It is
  counted in `float`: three products and two roundings down a row;
- the disc gives a chord, kept in whole numbers and stepped from row to row.
  For a disc of up to 23 000 pixels the numbers are of 32 bits, for a larger
  one of 64: a product of 64 bits is several instructions on this chip, and
  every row of every disc needs a chord;
- the interval of the row is the two together.

A pixel is in the rectangle when its centre is no further than 0.01 of a
pixel past its edge.

### Even widths

A line of an odd width has a middle pixel, and the points of the curve are
its centres. An even width has none: one side has to be a pixel wider. That
pixel is given to the right of the direction the line goes in, the same way
`dgx_draw_line_thick()` does, so a figure drawn always in one direction is
thicker consistently inwards. The middle line of a piece runs half a pixel to
the right of its points, and the disc at its end is centred between pixels.

### A curve in parts

`dgx_bezier_draw_to(&b, t)` draws a curve on until `t` of its length. The
length is the sum of the lengths of the pieces, counted once, when it is
first needed. The pieces that fit into the way are drawn whole. The piece the
pen has stopped on is drawn as far as that place: the part of its rectangle
from where the pen stopped last time to where it is now, and a round tip. The
tip is a disc two pixels smaller than the line. A full one, centred on a
whole pixel next to the exact place, could stick out of the finished line by
a pixel; the smaller one does not, and the next call covers it.

The disc of the tip is cut by the straight line its piece starts with: what
lies behind it belongs to the piece before. Before that change, and in 0.5.3
too, a curve of a pen of an even width drawn in parts got a spare pixel now
and then. The axis of an even pen runs half a pixel to the right of the way;
after a turn the axes of two pieces lie on different pixels, and a tip right
at the joint stuck out of the disc of the piece before. Of 40 000 random
curves 39 were so, all of an even width from 4, with 44 spare pixels.

Now a curve drawn to 1 in any number of calls is, pixel for pixel, the curve
drawn at once: on the same 40 000 curves there is no difference.

### Rows into the memory of a screen

In 0.5.3 every row of a figure went to the screen by a call of
`fill_rectangle` through a pointer; rows next to each other with the same
interval were joined into one call, so a vertical piece was one rectangle.
For the name of the place with a pen of 2 that is 427 figures, 1716 rows and
1027 calls (counted on the host): a figure of a small text is four rows of
two pixels, and the call costs more than the filling.

Now, on a virtual screen with linear memory and a pixel of 8 or 16 bits, the
pen writes the rows into the memory of the screen itself and marks the
changed area once a figure. On other screens, and on a display behind a bus,
all is as it was: there a transfer over the bus stands behind a call, and
joining the rows saves it.

A row shorter than 8 pixels is filled by a loop in place, a longer one by
`memset`. The compiler turns such a loop into a call of `memset` by itself;
for a short row the call costs more than the loop, and an empty `__asm__`
statement keeps the loop from it.

The ends of the pieces are rounded in place, without `lroundf`. The result is
the same: a half goes away from zero.

The length of a piece stayed with `sqrtf`. A root found in place, the way the
soft pen does it, with a step of Newton more, differs from that of the
library in the last bit. That was enough for one of 80 000 random curves to
get another pixel: it lay on the very edge of the line. The change gave 2–4%
of the time, the same pixels are worth more.

The pixels of the hard pen after all of this are the ones it had: 80 000
random curves, half on an 8-bit screen and half on a 16-bit one, from 1 to
460 pixels thick, drawn in 1–9 calls, came out byte for byte as before.

What it gave, in one build, the hard pen as it was and the new one,
milliseconds:

| What | As it was | New |
| --- | --- | --- |
| The name of the place, pen 2 | 4.48 | 3.80 |
| The same, pen 3 | 5.16 | 4.28 |
| The same, pen 1 | 3.14 | 2.19 |
| The text, scale 0.25, pen 3 | 4.00 | 3.25 |
| The text, scale 0.25, pen 9 | 6.99 | 5.33 |
| The text, scale 1, pen 3 | 6.74 | 5.26 |
| The text, scale 1, pen 9 | 10.45 | 7.65 |
| The name of the place on a 16-bit screen, pen 2 | 4.19 | 3.58 |

In that build the root was still found in place, and short rows went through
`memset`. The loop in place was timed in another build: the name of the place
with a pen of 2 takes 3.97 ms with `memset` for every row and 3.81 ms with
the loop. Going back to `sqrtf` was not timed apart.

### A pen of one pixel

A line of one pixel is not a figure but a chain of pixels: pixel `k` of `n`
is `k × dx / n` and `k × dy / n` from the start of the piece, each brought to
the nearest whole number. It was counted through `lroundf` for every pixel,
now in whole numbers, a step an addition; the pixels are the same, checked on
9978 random lines. On a virtual screen the pixels are written straight into
its memory.

## The soft pen

### The share of a pixel

A soft pen leaves ink by shares of a pixel. The share is `0.5 + depth`, cut
to 0…1; the depth is the distance from the centre of the pixel to the edge of
the figure, positive inwards. It is the area of a pixel under a straight
edge, and the change takes one pixel across the line.

A pixel keeps the larger of what it has and what it is given. So figures may
be laid over one another, and that is used everywhere: a joint, an end of a
curve and a part of a piece cover their neighbours, and no pixel gets darker
for it. With color 0 the rule is another: every pixel the figure would give
anything to becomes 0.

Hence what it asks of a screen. A pixel has to be an amount that can be
compared with what was there, and that is a byte of an 8-bit virtual screen.
The table the screen is sent through turns ink into the colors of the
display. A 16-bit pixel has no such amount, and a display behind a bus cannot
be read at all: there a pen asked to be soft draws as a hard one of the same
width.

### Points that are not rounded

A soft edge alone gives little. The name of the place with a pen of 2 takes
this much ink, counted on the host. The rows of the table are ways to draw
one and the same line; the last is a measure: a picture 8 times larger,
reduced by averaging.

| How it is drawn | Ink |
| --- | --- |
| A hard edge | 2387 |
| A soft edge, the ends of the pieces on whole pixels | 2286 |
| A soft edge, the ends of the pieces as they are | 2068 |
| The same, and the control points of the curves as they are | 2063 |
| A picture 8 times larger, reduced by averaging | 2101 |

By the table the soft edge alone takes off 4% of the ink, and giving up the
rounding 10% more. The spare ink comes of the rounding: the end of every
piece is moved to the nearest whole pixel, up to half a pixel away from the
curve. A polyline of such pieces goes in a zigzag about the curve, and the
pen paints more pixels than it would along the curve itself. A soft pen finds
the share of a pixel from its distance to the piece and needs no whole
coordinates. So the ends of the pieces of a soft line stay as the formula of
the curve gives them, not whole. Only the start and the end of the curve
itself are whole: they are given as `dgx_point_2d_t`.

With an odd pen a whole point is the centre of the middle pixel. With an even
soft pen it would be there too, and a pen of 2 pixels would cover three: the
middle one whole and half of each beside it. The dot over «i» came out as a
cross, the two dots over «ö» touched. So a soft line of an even width runs
half a pixel to the right and down of its points: whole points fall between
pixels, and the dot of a pen of 2 is a square of 2×2 with 79% of the ink
each, where the hard pen puts it.

### A bar, a disc and a corner

A soft piece is no bullet. It is drawn as a bar: a rectangle as wide as the
pen, soft at its sides only. The ends of the bar are cut straight, and what
lies past them other figures give:

- at the start and at the end of a curve, half a disc, the half past the end;
- at a joint where the line turns sharply, the wedge of a disc between the
  end of one piece and the start of the next;
- at a joint where the turn is little, nothing: both pieces are made longer
  by `(width / 2 + 0.5) × tan(turn / 2)` and meet in a corner. A corner gives
  a pixel no more than 1/16 of its ink over a round joint, and that sets the
  limit of the turn: 23° for a pen of 2, 13° for a pen of 9;
- where the pen has stopped on its way, a soft end and a disc two pixels
  smaller than the line, as the hard pen has it.

To choose between a corner and a disc the pen looks one piece ahead. The end
of that piece is found once and then serves its own piece.

The name of the place has 89 strokes, 338 pieces and 250 joints between them;
108 joints became corners.

The first version made the ends soft. 3–5% of the ink was lost at the joints:
two soft ends put together give less than an unbroken line. Ends cut straight
and a figure of its own for a joint did away with it.

### Counting in whole numbers

A bar counts in fixed point: whole numbers, 13 bits of fraction, 8192 units
to a pixel. A pixel needs two distances from the start of the piece, along it
and across:

```
along  = (x − x0) × ux + (y − y0) × uy
across = (x − x0) × uy − (y − y0) × ux
```

`(ux, uy)` is the direction of the piece, a vector of length 1 in the same
units: 8192 is one. `(x0, y0)` is the pixel the piece starts in; the fraction
of the start goes into both numbers as a constant. There are no products in
the loop: a step a pixel to the right adds `ux` to the first number and `uy`
to the second, a step a row down `uy` and `−ux`. Adding whole numbers is
exact, so a pixel has one number from whatever side it is come to.

The share of a pixel is `width / 2 + 0.5 − |across|` in the same units: from
8192 the pixel is painted whole, from zero down it is not touched. The ink of
a pixel is the share times the color, shifted 13 bits to the right. A pixel
belongs to a part of a piece when `along` lies between its start and its end;
both bounds are checked by one unsigned comparison,
`(uint32_t)(along − start) > length`.

For the bar of a long piece the bounds of a row are found as for a bullet,
from four straight lines. For a short one, and at small sizes nearly all are
short, a row is looked at from the first column of the bar to the last: to
find its bounds costs more than to look at five pixels beside it.

A disc is counted in whole numbers of 1/64 of a pixel. Its cuts are the
straight lines past which the disc is not needed: the end of its own piece
and the start of the next. Only the pixels before the cuts are looked at; the
cuts are counted from the centre as it is, not from the one brought to 1/64,
or between a disc and a bar of a very thick pen some pixels stayed unpainted.
Only the pixels of the soft edge need a root; for a disc of 5 pixels and more
two terms of a series in the difference of squares stand for it, within a
hundredth of a pixel.

Writing in steps keeps what it had: a text written in any number of steps is
byte for byte the text drawn at once. The share of a pixel depends on the
piece and the pixel alone, not on the part of the piece it is drawn with: the
number does not depend on where the count began. Every next part of a piece
begins 2 pixels before the place the pen stopped at and covers the soft end
of the part before; with the rule of the larger of the two the overlap
changes nothing.

## What a pen calls

The processor of an ESP32 has no instruction that divides a `float`: a
division is the function `__divsf3` of libgcc. `sqrtf`, `hypotf`, `lroundf`
and `ceilf` are functions of libm. All of them lie in flash apart from the
code of the pen. What they cost while their code is in the cache, with the
loop of the timing:

| Function | Time, µs |
| --- | --- |
| A product of `float`, to compare with | 0.08 |
| A division of `float` | 0.26 |
| `lroundf` | 0.26 |
| `ceilf` | 0.31 |
| `sqrtf` | 0.38 |
| `hypotf` | 0.79 |

That is little. What costs is another thing: a call of a function that is not
in the cache is a line read from flash, several microseconds; see
[«The flash cache»](#the-flash-cache).

The soft pen calls nothing on the way of a piece. One over the square root is
found in place: a guess from the bits of the number and two steps of Newton,
within 5 parts of a million. The root comes from it, and one over a number,
which stands for a division.

The hard pen calls: `sqrtf` for a piece, a division for a point of a piece,
up to four divisions for the figure of a slanted piece and `memset` for a row
of 8 pixels and more. A division cannot be replaced here with one over the
number for the same reason as the root: the last bit moves pixels on the
edge.

The layout of a text in `hw_font.c` calls a division, `hypotf` and `sqrtf`
for a stroke; nobody has looked into it.

## The flash cache

The code and the constants of a program lie in flash and are read through a
cache: 32 KB, two ways, a line of 32 bytes. An address says which set a line
goes to: two places 16 KB apart share one set of two lines. A third place on
the same set pushes one of the two out, and then every round is a miss.

The first soft pen drew the name of the place in 14.7 ms against 8.5 ms for
the hard one. The same library in two builds that differed in the test
program alone gave 7.7 and 14.4 ms. The check: with the functions of the pen
in IRAM a text drawn beside the screen took 2.4 ms instead of 9.5.

A text is written so: the layout gives a stroke, the pen draws it, and so for
every stroke. There are three hot places: the code of the layout, the code of
the pen and the tables of the font. In the build where the soft pen lost, the
code of the layout and the code of the pen lay 16 984 bytes apart, that is on
the same sets, and the table of the font came to them as the third.

What has been done about it:

- `hw_font.c` and `bezier.c` stand side by side in the list of sources. The
  linker puts the files of an archive in the order they are needed, and the
  code of the layout and of the pen lies in a row, on different sets: the
  name of the place takes 4.05 ms instead of 6.4;
- the soft pen calls nothing, see above.

What that does not solve: where the tables of a font lie against the code.
Five builds of one library with different test programs. In builds 239 and
241 the code of the soft pen is the same; in 242, 244 and 245 two divisions
are gone from it, the rest is the same. The time in milliseconds:

| Build | Going through the name of the place | The name of the place with a soft pen of 2 | Drawing without going through |
| --- | --- | --- | --- |
| 239 | 0.76 | 3.96 | 3.20 |
| 241 | 0.78 | 3.95 | 3.18 |
| 242 | 1.19 | 4.25 | 3.06 |
| 244 | 1.04 | 4.10 | 3.06 |
| 245 | 1.19 | 4.25 | 3.06 |

That it is the place of the font that moves was checked by a firmware of its
own, build 247. `font0ss` lies there in flash 12 times in a row. The tables
of the font take 17.3 KB, 1.3 KB more than a way of the cache, so every copy
is 1.3 KB further in a way than the one before, and 12 copies go over a whole
way. A thirteenth copy lies in RAM, past the cache. The code is the same and
at the same place in every timing; only the copy of the font the layout reads
changes. The time in milliseconds; the best, the mean and the worst are over
the 12 places in flash:

| What | The font in RAM | The best place | The mean | The worst place |
| --- | --- | --- | --- | --- |
| Going through the name of the place | 0.74 | 0.75 | 0.82 | 1.21 |
| Going through the text | 0.52 | 0.53 | 0.58 | 0.76 |
| Going through three Russian lines | 0.96 | 0.97 | 1.12 | 1.51 |
| The name of the place with a soft pen of 2 | 3.80 | 3.80 | 3.99 | 4.58 |
| The name of the place with a hard pen of 2 | 3.47 | 3.48 | 3.66 | 4.42 |
| The text with a soft pen of 3, scale 0.25 | 3.35 | 3.36 | 3.48 | 3.71 |

The best place does not differ from the font in RAM: there are no misses for
data there. The worst costs 1.4–1.6 times as much for going through and
11–27% more for drawing. Every text has a worst place of its own: a text has
its own letters, and they lie in their own lines of the cache. From a quarter
to three quarters of the places give a time at least 5% worse than the best.
The timing repeats: two passes over all the copies differed by no more than
3.7%.

The data of a text is tens of lines of the cache, from 26 to 109 on 11 trial
texts. The name of the place takes 71: 6 lines of the table of ranges, 14 of
glyphs, 17 of symbols and 34 of curves. A miss happens where a line of data falls on a set that already
holds two lines of hot code. Counting such sets by the link map explains a
part of the slow places, but not all: which lines of code are hot the map
does not show.

Three more firmwares repeat the timing with code in IRAM: the layout alone,
the pen alone and both. The code is put there by a linker fragment; `.data`,
`.bss` and the least free heap of a run are the same in all four,
212 212 bytes. The time in milliseconds, the least and the most over 25
places of the font in flash, those in the order of frequency and the font of
the library among them:

| What | All in flash | The layout in IRAM | The pen in IRAM | Both in IRAM |
| --- | --- | --- | --- | --- |
| Going through the name of the place | 0.75–1.21 | 0.74 | 0.75–0.82 | 0.74 |
| Going through the text | 0.53–0.76 | 0.53 | 0.53–0.57 | 0.53 |
| Going through three Russian lines | 0.97–1.92 | 0.96–1.15 | 0.97–1.15 | 0.96–1.08 |
| The name of the place with a soft pen of 2 | 3.80–5.00 | 3.79–4.45 | 3.80–3.87 | 3.79 |
| The name of the place with a hard pen of 2 | 3.48–4.42 | 3.47–3.74 | 3.48–3.55 | 3.47 |
| The text with a soft pen of 3, scale 0.25 | 3.36–4.13 | 3.35–3.77 | 3.36–3.39 | 3.35 |

With both in IRAM the time does not depend on the place of the font and is
the best one: a line of data has nothing to contend with in a set. The code
in IRAM got no faster than the best place in flash. The layout in IRAM nearly
evens the going through and halves the spread of the drawing; the pen in IRAM
evens the drawing within 2%.

Two settings are made of it. Both are off by default and exist for an ESP32
only:

| Setting | What goes to IRAM | IRAM taken |
| --- | --- | --- |
| `CONFIG_DGX_HW_FONT_IN_IRAM` | `hw_font.c` and the two functions of `font.c` that find a glyph | 4.1 KB |
| `CONFIG_DGX_BEZIER_IN_IRAM` | `bezier.c`, both pens | 9.3 KB |

Together 13.3 KB in this firmware and 13.9 KB in the example
`hw_soft_pen_demo`: what goes to IRAM is what a program uses of the two
files. IRAM of an ESP32 is 128 KB and no part of the common heap: in
`memory_layout.c` of ESP-IDF 5.5.4 this memory has no `MALLOC_CAP_DEFAULT`,
and the timing bore it out. But there is little of it. A firmware with Wi-Fi
takes IRAM nearly whole: the clock on a CYD has 5.4 KB of it free, and only
the first setting fits there. When IRAM is short the linker says
`IRAM0 segment data does not fit`.

The settings write the linker fragment at build time: it has to name the
archive of the component, and that name depends on how the component is
brought into a project.

### Other chips

All that is said of the cache is about the ESP32, where code and data have
one cache of two ways. An ESP32-S3 has a cache for code and one for data, of
8 ways by default: 16 KB for code and 32 KB for data, by `Kconfig.cache` of
ESP-IDF 5.5.4. A line of a font does not contend with code there. Nothing was
timed on an S3.

An ESP32-P4 has a cache of two levels. The same firmware of 25 places of the
font on a JC1060P470 board, the time in milliseconds:

| What | The least | The most |
| --- | --- | --- |
| Going through the name of the place | 0.360 | 0.370 |
| Going through the text | 0.254 | 0.256 |
| Going through three Russian lines | 0.471 | 0.474 |
| The name of the place with a soft pen of 2 | 1.985 | 1.992 |
| The name of the place with a hard pen of 2 | 1.424 | 1.428 |
| The text with a soft pen of 3, scale 0.25 | 1.756 | 1.759 |

There is no spread. The ratio of the pens differs too: on a P4 the hard pen
is 28% faster than the soft one, on an ESP32 at the best place of the font
8%. The likely reason is that a P4 has instructions for the division and the
root of a `float`; it was not checked. The numbers of one chip do not carry
to another.

The firmwares of the places of the font were built with the font as it was
before the last change of its connections; the name of the place was not
touched by it.

What follows for the numbers of this note: between builds of the same code
the time of a pen moves by a tenth, the time of going through the strokes by
a half, and the reason is the place of the font. Versions can be compared
only inside one build; every comparison here is made so.

## What costs what

### The work in numbers

Counted on the host: one and the same line with the hard pen and with the
soft one. «Rows» are the rows of all the figures of the hard pen. «Pixels»
are how many times the hard pen wrote a pixel and how many times the soft one
looked at a pixel. «Painted» are the pixels with ink in the finished line.

| Line | Pen | Figures | Rows | Pixels | Painted |
| --- | --- | --- | --- | --- | --- |
| The name of the place, pen 2 | hard | 427 | 1716 | 3722 | 2392 |
| | soft | 338 bars, 314 discs | | 7907 | 2906 |
| The text, scale 0.25, pen 3 | hard | 314 | 1644 | 5330 | 2720 |
| | soft | 251 bars, 254 discs | | 9859 | 3323 |
| The text, scale 0.25, pen 9 | hard | 314 | 3262 | 25 023 | 6869 |
| | soft | 251 bars, 268 discs | | 34 498 | 7141 |
| The text, scale 1, pen 3 | hard | 535 | 4707 | 16 718 | 12 093 |
| | soft | 472 bars, 270 discs | | 48 017 | 15 384 |
| The text, scale 1, pen 9 | hard | 535 | 7368 | 62 403 | 32 561 |
| | soft | 472 bars, 363 discs | | 102 446 | 35 359 |

The soft pen looks at pixels 1.4–2.9 times as often as the hard one writes
them.

### The time

One build, 257, both pens on an 8-bit screen, milliseconds. Going through the
strokes is in both columns: 0.99 ms for the name of the place and 0.70 ms for
the text.

| What | Hard | Soft |
| --- | --- | --- |
| The name of the place, scale 0.188 × 0.208, pen 1 | 2.38 | 3.54 |
| The same, pen 2 | 3.83 | 4.05 |
| The same, pen 3 | 4.32 | 4.54 |
| The text, scale 0.25, pen 3 | 3.23 | 3.54 |
| The text, scale 0.25, pen 9 | 5.45 | 6.32 |
| The text, scale 0.5, pen 3 | 4.17 | 3.45 |
| The text, scale 0.5, pen 9 | 6.57 | 5.54 |
| The text, scale 1, pen 3 | 5.36 | 3.42 |
| The text, scale 1, pen 9 | 7.97 | 4.83 |
| A frame of a morph of «3» into «8», scale 0.33, pen 3 | 0.257 | 0.270 |
| The same, scale 0.6, pen 5 | 0.448 | 0.493 |
| The same, scale 1, pen 9 | 0.720 | 0.876 |

In a small text the hard pen is faster, by 5–14%; in a large one the soft
pen, at a scale of 1 by 1.6 times. The reason is what the time of each goes
with. It shows in what a thick pen adds: the same text, nearly the same
figures, a pen of 9 in place of a pen of 3. The differences by the two tables
above:

| What a pen of 9 added | The text, scale 0.25 | The text, scale 1 |
| --- | --- | --- |
| Hard: rows | 1618 | 2661 |
| Hard: time, ms | 2.22 | 2.61 |
| Hard: for a row added, µs | 1.37 | 0.98 |
| Soft: pixels looked at | 24 639 | 54 429 |
| Soft: time, ms | 2.78 | 1.41 |
| Soft: for a pixel added, ns | 113 | 26 |

The time of the hard pen goes with the number of rows: a row costs about a
microsecond however many pixels it has. At a scale of 1 a row of a pen of 9
is 2.4 times as long as a row of a pen of 3, and a row added costs what a row
costs over the whole text. What that microsecond is made of has not been
taken apart; the bounds of a row in `float` are a small part of it, see
[«What was tried and did not stay»](#what-was-tried-and-did-not-stay).

The time of the soft pen goes with the pixels, and pixels differ. Inside a
long bar a pixel is two additions and a comparison, 26 ns, six cycles. At a
scale of 0.25 a piece is shorter than a pen of 9 is wide: the figure is made
of discs, and a pixel on the edge of a disc needs a root. There a pixel costs
four times as much.

In a small text the rows are few, and the hard pen, which has no share to
find, is faster. At a scale of 1 there are 2.3–2.9 times as many rows as at a
scale of 0.25 and 1.7 times as many figures; the pixels of the soft pen are
cheap there.

A soft pen of one pixel takes 49% longer than a hard one: the hard one needs
two additions a pixel, the soft one a bar and discs.

In 0.5.3 the name of the place took the hard pen 11.8 ms. Most of the
difference from 3.8 now is not the pen: the bend of a curve was found for
every stroke though the font gives the number of pieces, and in that program
the code of the layout and the code of the pen fell on the same sets of the
cache.

Memory. `dgx_bezier_t` is 128 bytes, it was 116: the soft pen keeps the end
of the next piece in it. The code of `bezier.c` is 9.9 KB, it was 3.8 KB:
4.7 KB is the soft pen, 1.3 KB the hard one writing into the memory of a
screen and the cut of its tip. A build without the virtual screen has
4.6 KB. A pen takes no RAM.

## What was tried and did not stay

- **A smoothstep over the share.** With a pen of 2 pixels it makes next to no
  difference, with a thick one still less.
- **A picture 8 times larger, averaged.** It gives 2101 pixels of ink against
  2068 for the soft pen, and its buffer for a band of 320×46 is 942 KB. It
  stayed as a measure.
- **Control points of curves that are not whole.** A quarter of a percent of
  the ink and new functions in the API.
- **Filling the middle of a row of a thick bar by one call.** Tried twice: it
  helped a pen of 9 pixels and more by 3–5% and slowed thin ones.
- **Soft ends of a bar.** They lost 3–5% of the ink at the joints.
- **A root in place in the hard pen.** One other pixel in 80 000 curves.
- **The rows of the hard pen in whole numbers.** The bounds of a row in fixed
  point, 65 536 to a pixel, by additions from row to row; the divisions that
  prepare a figure replaced with one over a number found in place. In one
  build all of it gave from 2% worse to 5% better: a small text from −3% to
  +2%, a text at a scale of 1 faster by 4–5%. And 871 of 40 000 random curves
  get other pixels on the edge, 1027 pixels of 38 million, and 64 trial texts
  3 pixels of 700 thousand. The guess «a good part of the microsecond of a
  row» did not hold.
- **Softness as a property of a screen.** See [«Two pens»](#two-pens).
- **The symbols of a font in the order of the frequency of letters.** A glyph
  is found by the code of its symbol, but its symbol and its curves may lie
  anywhere, and frequent letters can be put side by side. The data of a text
  gets no smaller for it: a symbol takes 30 bytes, the curves of a letter 62
  on the average, a line of the cache 32, and every letter takes its own 5–6
  lines wherever it lies. Counted on 11 texts, the lines of the cache come to
  85%–106% of what they are now. In the same firmware of 12 places a font in
  the order of frequency gave the same spread: going through the name of the
  place 0.80 ms on the average against 0.82, three lines 1.11 against 1.12,
  the name of the place with a soft pen 4.08 against 3.99.

## Not done

- There is no soft pen on 16-bit screens.
- The hard pen puts a disc on the end of every piece. In a text at a scale of
  1 with a pen of 9 two thirds of the rows are rows of discs, and a pixel is
  written 1.9 times on the average. The bar and the corner the soft pen does
  with were not made for the hard one: the pixels of the joints would be
  others.
- Going through the strokes costs 8.5–13 µs a stroke, from a quarter to a
  third of the time of a small text, and calls `hypotf` and a division.
- The library does not choose where its code and a font fall in the cache.
  The settings with IRAM are for an ESP32 only and off. A font in RAM takes
  the misses away as well, for 17.3 KB of RAM; there is no such setting.
- With sending to a display the soft pen was timed on an ESP32-P4 alone, in
  the example `hw_soft_pen_demo`: see
  [handwriting-en.md](handwriting-en.md#to-the-display-by-parts). There is
  no timing on a CYD.
