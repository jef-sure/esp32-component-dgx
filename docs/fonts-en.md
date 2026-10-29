# Fonts made at build time

[Русская версия](fonts-ru.md)

A program built on DGX, and the components it uses, declare the fonts they
need in their CMakeLists files. DGX renders all of them during the build, in
the right sizes and with exactly the characters the texts use, and nothing
has to be run by hand. This guide starts from scratch; how the mechanism
works inside is at the end.

## The problem

DGX draws text with bitmap fonts: each character is a picture of pixels,
rendered in advance for one pixel size. The converter `font2c` makes such a
font from a TTF or OTF file on your computer and writes it as a pair of C
files, `.c` and `.h`. DGX ships a few fonts of its own (`src/fonts/`), but a
real program usually needs more: another size, Cyrillic, umlauts.

The program is not the only one that needs fonts. The on-screen keyboard of
DGX-DI draws key labels, and a keyboard layout comes with its own
characters and its own size in millimeters. The game engine DGX-GE shows
dialogs in a font that must be a different pixel size on each display. If
every component made its own fonts, you would get two fonts of the same
face with different character sets, the same function name in two files,
and a build that depends on which component happens to be configured
first.

So DGX takes the job, in one place:

- whoever needs a font declares a **need** in its CMakeLists: a name, the
  characters, and where the font comes from;
- at the end of the configure step DGX gathers every need into one build
  rule;
- when the project is built, one script makes all the fonts at once: one
  font per face and size, with the combined characters of every need that uses it;
- the program gets a header, `dgx_fonts.h`, with one function per need.

Nothing runs outside CMake. A project without needs generates nothing and
requires nothing extra on the build machine.

## What it looks like

In the program's `main/CMakeLists.txt`, after `idf_component_register`:

```cmake
# the board: a 1280x800 display of 217x136 mm; the game shows its frame four times larger
dgx_fonts_board(PIXELS 1280 800 MM 217 136)
dgx_fonts_scale(4)
# the rules that pick a face when a need does not name one (the file is described below)
dgx_fonts_rules(../fonts.rules)

# the game's text: 12 points of the frame, monospaced, with every character of the game's strings
dgx_font_need(text SIZE 12pt MONO TEXTS assets/ru.po assets/pop.pot)
# DGX's handwritten font, used as it comes; it has no braces, which only mark placeholders like {0}
dgx_font_need(hand DGX font0ss TEXTS assets/ru.po EXCEPT "{}")
# a font of the program's own, used as it comes; only digits and a colon are needed from it
dgx_font_need(time FILE fonts/PxCgaThin8.c LETTERS "0123456789:")
# the keyboard labels: DGX-DI reads its own layout and declares the need on the program's behalf
dgx_di_keyboard_font(keys keys.json)
```

A component writes the same in its own CMakeLists, for example the titles
of its widgets, 4.5 mm high:

```cmake
idf_component_register(SRCS "widgets.c" INCLUDE_DIRS "." REQUIRES dgx)
dgx_font_need(title SIZE 4.5mm TEXTS title.txt)
```

In the code, include the header and call the function named after the
need:

```c
#include "dgx_fonts.h"

dgx_font_string_utf8_screen(scr, 10, 40, "Привет", ink, DgxOutputNormal, 1, dgx_font_text(), NULL, NULL);
dgx_di_keyboard_open(&kb, scr, DGX_FONTS_BOARD_WIDTH_MM, DGX_FONTS_BOARD_HEIGHT_MM, keys, keys_size, dgx_font_keys(), &field);
```

`idf.py build` prints one line per need:

```
title: DejaVu Sans Mono Book 27 px (4.5 mm), 24 glyphs
text: DejaVu Sans Mono Book 48 px (12 pt x 4), 97 glyphs
hand: font0ss of DGX, used as is
time: PxCgaThin8.c of the program, used as is
keys: DejaVu Sans Book 24 px (4 mm), 148 glyphs
```

## A need

A need is one line, `dgx_font_need(<name> ...)`. The name is a C
identifier: it names the function, `dgx_font_<name>()`. A name that
collides with a function of DGX itself (`dgx_font_string_bounds` and the
like) is rejected, and so are two names that differ only in case, since
their macros would not. The same name declared in two places, a component
and the program, with the same keywords is one need: the characters of both
are put together before the face is picked. With different keywords the two
must resolve to the same font; two different fonts under one name stop the
build, and the message names both places and both fonts. A keyword given
twice in one declaration is an error, not a silent second value.

The keywords of a need:

| Keyword | Meaning |
| --- | --- |
| `TEXTS <files...>` | Files whose characters the font must contain. Paths are relative to the CMakeLists that declares the need. How each kind of file is read is described below. |
| `LETTERS "<characters>"` | Characters given directly, in addition to the texts or instead of them; any characters, brackets, semicolons and spaces included. |
| `EXCEPT "<characters>"` | Characters of the texts the font does not need: the braces of placeholders like `{0}` in a game's strings, for example. |
| `SIZE <n>pt`, `<n>mm`, `<n>px` | The size: in points of the frame, in millimeters, or in pixels (see below). A bare number means points. |
| `MONO` | Only a monospaced face. |
| `FAMILY "<pattern>"` | Only this family; a pattern with `*`, as in the rules. |
| `POSTSCRIPT <name>` | Exactly this face, by its PostScript name (`DejaVuSans`, `DejaVuSans-Bold`); no rules needed. |
| `SOURCE <file>` | A font from this file: any font file `font2c` reads, a TTF, an OTF or a BDF, with a `SIZE` (a font of bitmaps only at a size it has), or the JSON of a handwritten font from the hw-fonts editor, which has no size. |
| `DGX <name>` | One of DGX's own fonts, used as is: `font0ss`, `TerminusTTFMedium12` and the others in `src/fonts/`. |
| `FILE <file.c>` | A font the program already has as a `.c` file, used as is; the program compiles it itself, DGX only checks its characters and declares the function. |
| `PATCH <file>` | The glyphs fixed by hand for this font in the font editor (see [Glyphs fixed by hand](#glyphs-fixed-by-hand)); without it a patch is looked up by the name of the font. |

Exactly one of these says where the font comes from: `SIZE` with
`MONO`/`FAMILY` (picked by the rules), `SIZE` with `POSTSCRIPT`, `SOURCE`,
`DGX`, `FILE`. An existing font (`DGX`, `FILE`) has a size of its own, so it
takes no `SIZE`. `POSTSCRIPT` and `SOURCE` name the face themselves, so they
take no `MONO` or `FAMILY`; a need that has both is an error.

Fonts come in two kinds:

- **made fonts**: DGX renders them for this board from a TTF, an OTF or a
  JSON (`SIZE`, `SOURCE`);
- **existing fonts**: `.c` files that are already there (`DGX`, `FILE`).

## Needs in a file

Usually the needs go straight into the CMakeLists, and that is all. A file
is for one case: a program with two builds. A game engine, for instance, is
built not only for the board with ESP-IDF but also on the development
machine with a plain Makefile, for its own tests. CMake does not run there, so needs
written in a CMakeLists are invisible to that build, yet it needs the same
fonts. Such a program keeps its needs in a file that both builds read.

The file uses the same keywords as `dgx_font_need`, one per line and in
lower case; a need starts with the line `need <name>`:

| In the CMakeLists | In the file |
| --- | --- |
| `dgx_font_need(text SIZE 12pt MONO TEXTS a.po b.po)` | `need text` / `size 12pt` / `mono 1` / `text a.po` / `text b.po` |
| `FAMILY "DejaVu*"`, `POSTSCRIPT DejaVuSans` | `family DejaVu*`, `postscript DejaVuSans` |
| `DGX font0ss`, `FILE f.c`, `SOURCE f.ttf` | `dgx font0ss`, `file f.c`, `source f.ttf` |
| `LETTERS "0123456789:"`, `EXCEPT "{}"` | `letters 0123456789:`, `except {}` |

An example, `main/fonts.needs` of a game:

```
# the game's fonts: the same for the board and for the host
need text
size 12pt
mono 1
text assets/pop.letters
need hand
dgx font0ss
text assets/pop.letters
```

The board build: one line in the CMakeLists replaces the `dgx_font_need`
calls; the board, the scale and the rules are declared as usual:

```cmake
dgx_font_needs_file(fonts.needs)
```

The host build: the same file and the same script, without CMake. The scale,
the display and the rules, which the CMakeLists declares for the board
build, are passed as arguments here. The result is the same `dgx_fonts.c`
and `dgx_fonts.h` in the directory you name; compile them into the host
program with `-I` pointing at that directory:

```sh
python3 components/dgx/font2c/dgx_fonts.py build main/fonts.needs build-host/dgx_fonts \
        --scale 4 --pixels 1280 800 --mm 217 136 --rules fonts.rules
cc ... -Ibuild-host/dgx_fonts build-host/dgx_fonts/dgx_fonts.c ...
```

Paths in the file are relative to the file's directory. A program built for
a single board may also put `rules <file>`, `scale <n>`, `board pixels <w>
<h>` and `board mm <w> <h>` lines into the file; command-line arguments
override them. Editing the file reconfigures the project, just like editing
a CMakeLists.

## Size and board

The size of a need is converted to display pixels once, at build time:

| Unit | Pixels | What the board must provide |
| --- | --- | --- |
| `12pt` | 12 × the frame scale: the game shows its frame enlarged | `dgx_fonts_scale`, 1 if not given |
| `4.5mm` | 4.5 × (pixels per millimetre of the display) | `PIXELS` and `MM` |
| `30px` | 30 | nothing |

The scale is a single number: how many times larger the board shows the
program's frame. A game draws a 320x200 frame in points; a 10-inch display
shows it as 1280x800, one point is four pixels, the scale is 4; on a 7-inch
display the same game uses 3; a program without a frame declares no scale
and a point is a pixel. The scale draws nothing and enlarges nothing: it
only converts points to pixels when the fonts are made, and the program
gets it back as the number `DGX_FONTS_SCALE`. With a scale of 4, the need
`text SIZE 12pt` yields a font rendered from the TTF directly at 48 px, real
48-pixel characters rather than 12-pixel ones blown up into squares, and
the program draws with it at text `scale` 1 (`DGX_FONT_TEXT_MADE` is 1). An
existing font, `time FILE PxCgaThin8.c`, stays in points of the frame
(`DGX_FONT_TIME_MADE` is 0), and the program draws with it through the text
API with an integer `scale` equal to the frame scale: 8 × 4 = 32 pixels,
blocky on purpose, as a pixel font should be.

The display and the scale are declared separately: `dgx_fonts_board(PIXELS
1280 800 MM 217 136)` is a fact about the board, `dgx_fonts_scale(4)` is the
game's choice on that board. Pixels per millimetre are computed from the
width; if the height gives a noticeably different figure (more than 5%) the
build stops, because the numbers cannot all be right. The result is
rounded to whole pixels, halves up: 4 mm on a display with 5.9 px/mm is
24 px.

A need in millimeters without `PIXELS`/`MM` is a build error with a hint
about what is missing. Declaring the display or the scale twice with the
same numbers is fine; different numbers are an error that quotes both. So a
board description file and the program may both declare them, as long as
they agree.

## Characters

A font gets only the characters that were asked for: those of the need's
texts and its `LETTERS`, minus `EXCEPT`. ASCII is not added on its own: a
clock font declared with `LETTERS "0123456789:"` has no use for 85 extra
glyphs. The files given in `TEXTS` are read according to their kind, not as
raw bytes:

| File | What is taken |
| --- | --- |
| `.po`, `.pot` | The `msgid`, `msgid_plural` and `msgstr` strings, with line continuations and escapes as in gettext; the header entry (the one with an empty `msgid`) is skipped. |
| `.json` | Every string value. |
| anything else | The whole file, as UTF-8 text. |

Strings that the program assembles at run time, such as a level number or
the minutes of a clock, are not in the texts; the texts hold only
placeholders like `{level}`. Request their characters with `LETTERS
"0123456789:-"`, or through a character file that the program writes
itself, as a game's packer does.

A text file with a byte order mark is read without it. Control and format
characters (newline, tab, a soft hyphen, a zero-width joiner) do not count
as characters; everything else does, the icons of an icon font in the
Private Use Area included. A space goes
into a made font if the texts contain one, but is never required of an existing
font: every DGX font handles spaces, and a handwritten font defines a space
as a width, not a glyph.

If a font lacks some of the characters, the build stops and names them:

```
dgx_fonts.py: needs.txt:15: font0ss of DGX: lacks the characters { U+007B } U+007D
```

This is where the `EXCEPT "{}"` in the example above comes from: the game's
strings contain placeholders like `{0}`, which the handwritten font does not
need. There is no fallback font for missing characters: a silent gap in a
text is worse than a stopped build.

## Selection rules

When a need does not name an exact face, the **rules** pick one: a text
file of the program, declared with `dgx_fonts_rules(fonts.rules)`. Which
TTF converts cleanly to a monochrome bitmap at which size is a judgment
call, and the rules file records it. Without rules, a
need by family or `MONO` is an error.

```
# where the fonts come from
source system                  # the fonts installed on the computer (fontconfig)
# source dir  ~/Fonts          # every .ttf, .otf, .ttc in the directory, recursively
# source list ~/fonts.txt      # a file listing font files, one per line

# what is allowed, at which sizes, in order of preference
allow "Terminus (TTF)*"   12, 14, 16, 18, 20, 22, 24, 28, 32
allow "DejaVu Sans Mono"  16..
allow "IBM Plex Mono"     28..
deny  "Comic*"
allow *                   40..
```

- `allow "<family>" <sizes>`: the family is a pattern with `*` (the family
  name, or "family style"; case does not matter). Sizes are in display
  pixels: numbers separated by commas, ranges `16..24`, `16..`, `..24`,
  comparisons `> 12`, `>= 12`, `< 40`, `<= 40`; `&` means "and", a comma or a
  space between conditions means "or".
- `deny "<family>" [<sizes>]` forbids a family; without sizes, at every
  size. A deny applies to every font of the program, not only to those the
  rules pick: see [Fonts and their licenses](#fonts-and-their-licenses).
- `strict`, on a line of its own, admits only the fonts the `allow` lines
  name, however they are asked for. It is off unless the rules say so.
- The first `allow` line, in order, that has a face of the required size
  **with all the characters of the need** wins; among its faces a regular
  style comes first, then Medium, then bold, italic and the rest, and then
  the order of the sources decides. A face without the characters is
  skipped, like a denied one.
- A relative path in a `source dir` or `source list` line is resolved from
  the rules file, and a relative path in a source list from that list.
- If nothing matches, the build stops and says which size and which
  characters it looked for.

Fonts come from the machine the project is built on: another machine may
pick differently, and without a suitable font the build stops. An exact
face by PostScript name is looked up with `fc-list` and must match exactly; `fc-match` is not used, because it silently returns a different
font for a name it does not know.

## Fonts and their licenses

DGX takes whatever fonts the program and its components ask for and does
not judge their licenses. Whether you may ship a font is your decision,
and the build puts what you need for it in plain sight.

**What is in the program.** Every time the fonts are made, the build
writes `build/dgx_fonts/fonts-used.txt`: each font the program uses, the
file it came from, what that file says about its copyright and license
(the name table of a TTF or OTF), and which needs and lists use it. The
configure step prints its path, the build prints it again, and
`dgx_fonts.h` points to it. Read it before you ship, and again after
adding a component that asks for fonts.

```
DejaVu Sans Book
  file: /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf, PostScript name DejaVuSans
  version: Version 2.37
  copyright: Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. ...
  license: Fonts are (c) Bitstream (see below). DejaVu changes are in public domain. ... (shortened; the whole text is in the font file)
  license URL: http://dejavu.sourceforge.net/wiki/index.php/License
  used by: keys at 24 px, named by its PostScript name in /work/components/kbd/CMakeLists.txt

TerminusTTFMedium12, a font of DGX
  file: .../src/fonts/TerminusTTFMedium12.c
  made from: Terminus (TTF) Medium 4.47 at 12 px
  license: SIL Open Font License 1.1
  used by: clock, used as is, in /work/main/CMakeLists.txt
```

A font file that states nothing about its license is listed with "the file
says nothing of its copyright or license". A font the program brings as C
(`FILE`) is listed without a license: only the program knows where it came
from.

**Keeping a font out.** Deny it in the program's rules:

```
deny "Arial*"
```

The rules never pick a denied face. A need or a list that names a denied
font exactly, by `POSTSCRIPT`, `SOURCE`, `DGX`, `FILE` or a line of a
`dgx_font_list`, stops the build, and the message names both sides:

```
dgx_fonts.py: /work/components/kbd/CMakeLists.txt: keys asks for DejaVu Sans Book
(/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf) at 24 px, and /work/fonts.rules:7 denies it:
the request and the rules disagree, one of them must change
```

DGX does not substitute another face: a component that asked for a font
by name either gets that font or the build stops. Only the program has
rules, so only the program decides; a component cannot override them.

- A deny with sizes, `deny "DejaVu Sans" < 20`, keeps a face out at those
  sizes only. A font used as is (`DGX`, `FILE`) has no size to compare, so
  only a deny without sizes applies to it.
- A font used as is is matched by its name, with spaces and punctuation
  ignored as font2c ignores them: `deny "DejaVu Sans*"` also matches
  `DejaVuSansBook32`, and `deny "Terminus (TTF)*"` matches
  `TerminusTTFMedium12`. A font of DGX is
  matched by its family as well, as recorded in `src/fonts/README.md`.
- Rules with only `deny` lines and no `source` are enough when every need
  names its face exactly. A need that the rules must pick a face for still
  requires a source.

**Strict rules.** If you have checked the licenses of a set of fonts and
want the build to stop on anything else, add `strict` to the rules. Then a
font is admitted only if an `allow` line names it, whether the rules pick
it or a need names it exactly:

```
strict
source dir ~/fonts-checked
allow "Terminus (TTF)*"  12..32
allow "DejaVu Sans*"     8..
allow "Weather Icons"    1..
```

The sizes of the `allow` lines still choose faces for the needs the rules
pick for; a font named exactly only needs an `allow` line for its family,
at any size. `allow *` admits every font, so it defeats `strict`. Without
the word `strict`, nothing changes from the rules described above.

**The fonts of DGX.** The fonts in `src/fonts/` are made from fonts under
free licenses, or were drawn for DGX. `src/fonts/README.md` lists what each
one was made from and under what license.

## One font per face and size

The needs are passed to `font2c` as one list, one line per need, and font2c
itself groups them by (face, pixel size): one font is made per group, with
the characters of all its needs merged, and every need gets its own function
pointing to it. If the game asks for "DejaVu Sans Mono at 48 px with the
characters of the dialogs" and the keyboard asks for "the same face at
4 mm", which is also 48 px on this board, flash holds one set of glyphs, and
`dgx_font_text()` and `dgx_font_keys()` return the same font.

A handwritten font from a JSON has no size (the size is chosen when text is
drawn): one font per file.

## Glyphs fixed by hand

At small sizes a rendered glyph sometimes comes out wrong on the panel: a
stem broken, a letter off its line, an accent in the wrong place. The font
editor of DGX, `font-editor/index.html`, opens a `.c` written by `font2c`,
lets you redraw the glyphs pixel by pixel, change their size, advance and
offsets or fit them to the pixels, and saves the result as a **patch**. The
`.c` is never edited by hand: `font2c` makes it again from the face and the
patch, and every glyph of the patch replaces the rendered one completely,
with its own size and metrics. A font made again keeps the fixes.

The editor saves one patch for the whole `.c`, with a part for every font of
it that has changes. A part names its font, by the name `font2c` gives it
(the family, the style and the size with spaces and punctuation removed, as
in `DroidSansRegular9`), and its size; `font2c` gives each font it makes the
part that names it. The patch of a `.c` of one font is named after that
font, `DroidSansRegular9.patch.json`; the patch of a `.c` of several, such as
`build/dgx_fonts/dgx_fonts.c`, is named after the file, `dgx_fonts.patch.json`.
A patch named for a font, by `PATCH` or by its file name, that has no part for
it is refused.

A font rendered at build time takes its patch without being asked:

1. the file named by `PATCH` (or `patch` in a needs file), if any;
2. otherwise a `.patch.json` with a part for the font next to the CMakeLists
   or the needs file that declares the need;
3. then next to the `SOURCE` file, for a need with one;
4. then in `src/fonts/` of DGX, where DGX keeps the fixes of the faces its
   own fonts come from.

The first one found wins. Two patches of one directory with a part for the
same font stop the build, with both named. A patch added or changed in one
of these places makes the fonts again on the next build. A font used as is (`DGX`, `FILE`)
is not patched: it is a `.c` already, made with its patch if it had one.
A patch may have glyphs the font does not need; they are skipped, so one
patch serves every need of that face and size, whatever characters each
asks for.

`font2c` takes the same patches directly: `-p <file>` names one, `-P <dir>`
(repeatable) looks in a directory for a patch with a part for the font. Both
can also be used on a line of a `-jo` list.

## What you get

The build produces `build/dgx_fonts/dgx_fonts.c` and `dgx_fonts.h`.
The first is compiled into the DGX library; the second is available to
every component that depends on DGX. Next to them, `fonts-used.txt` lists
the fonts and their licenses (see
[Fonts and their licenses](#fonts-and-their-licenses)). In the header, for each need:

```c
/* text: DejaVu Sans Mono Book at 48 px (12 pt x 4), made for this board */
dgx_font_t *dgx_font_text(void);
#define DGX_FONT_TEXT_MADE 1
#define DGX_FONT_TEXT_PX 48

/* keys: DejaVu Sans Book at 24 px (4 mm), made for this board */
dgx_font_t *dgx_font_keys(void);
#define DGX_FONT_KEYS_MADE 1
#define DGX_FONT_KEYS_PX 24
#define DGX_FONT_KEYS_MM 4

/* hand: font0ss of DGX, used as is: its own function */
#define dgx_font_hand font0ss
#define DGX_FONT_HAND_MADE 0

/* every need, in declaration order: X(name) for each */
#define DGX_FONT_NEEDS(X) X(title) X(text) X(hand) X(time) X(keys)

/* the frame scale and the display the fonts were made for, as declared in the project's CMakeLists */
#define DGX_FONTS_SCALE 4
#define DGX_FONTS_BOARD_WIDTH_PX 1280
#define DGX_FONTS_BOARD_HEIGHT_PX 800
#define DGX_FONTS_BOARD_WIDTH_MM 217
#define DGX_FONTS_BOARD_HEIGHT_MM 136
```

- `dgx_font_<need>()` returns the need's font, an ordinary `dgx_font_t *`.
  For an existing font it is not a wrapper function but a macro for the font's
  own function: `dgx_font_hand()` *is* `font0ss()`. That way the DGX library
  never references code of the program, and link order does not matter; the
  header declares a program's own font (`FILE`) itself.
- `DGX_FONT_<NEED>_MADE` is 1 if the font was made for this board (its size
  is already in display pixels; draw text with `scale` 1) and 0 if it is an
  existing font (the program scales it itself: the DGX-GE engine, for
  instance, draws the pixel font of its status bar at ×4 and sizes its
  handwritten font by line height).
- `DGX_FONT_<NEED>_PX` and, for a need in millimeters, `_MM`: the pixel size
  of a made font and the millimeters that were asked for, so a keyboard can
  check that it was given the font its layout was designed with.
- `DGX_FONT_NEEDS(X)` lists every need in declaration order, for the
  program's own purposes: enumerating its fonts, printing their names:

```c
#define NAME(n) #n,
static const char *const names[] = {DGX_FONT_NEEDS(NAME)};
```

  The order spans every component, so do not build a table indexed by
  number from it, as a game or a library keeps one. Build such a table by
  name; a missing need then fails to compile instead of silently yielding
  the wrong font:

```c
static dgx_font_t *(*const fonts[POP_FONT_COUNT])(void) = {
    [POP_FONT_TEXT] = dgx_font_text, [POP_FONT_HAND] = dgx_font_hand, [POP_FONT_TIME] = dgx_font_time,
};
```

- `DGX_FONTS_SCALE` and `DGX_FONTS_BOARD_*` repeat the numbers from
  `dgx_fonts_scale` and `dgx_fonts_board`, so the scale and the display's
  millimeters are written once in the project: the same numbers go to
  `dgx_di_touch_display()` and `dgx_di_keyboard_open()`.

## What the build machine needs

ESP-IDF already provides Python. A project with needs also requires:

| What | For | If missing |
| --- | --- | --- |
| CMake 3.19 or later | gathering the needs at the end of the configure step (`cmake_language(DEFER)`); the CMake of ESP-IDF's tools is newer | "the fonts made at build time need CMake 3.19 or later" at the first need; a project without needs is not affected |
| a host C compiler and the FreeType development headers (`libfreetype-dev`, `pkg-config`) | building `font2c` from DGX's source; it is built into `build/dgx_fonts/` and rebuilt when its source changes | "the FreeType development files (libfreetype-dev) are not installed" |
| fontconfig (`fc-list`, `fc-scan`) | finding faces: the installed ones, and those in directories named by the rules | "fontconfig is required to find fonts" |
| the fonts the rules allow, or the one a need names | rendering the glyphs | "no face allowed by the rules at 48 px has all the characters", "no installed face has the PostScript name …" |

A project without a single need requires none of this.

## Needs from another component

A component declares needs the same way the program does: `dgx_font_need`
in its own CMakeLists after `idf_component_register`, as the widget titles
in the example above. Paths are relative to that CMakeLists, the component
calls `dgx_font_<name>()` in its own code, and the program has nothing to
do: its build generates these fonts along with its own. Things to keep in
mind:

- The component depends on DGX (`REQUIRES dgx`, or `jef-sure__dgx` from the
  registry): `dgx_fonts.h` is on the include path of the DGX library.
- Need names are global to the project. A component's need with the same
  name as one of the program's merges with it if both resolve to the same
  font, and stops the build if they do not. Give a component's own needs a
  prefix (`widgets_title`); a name the program chooses, like `keys` for the
  keyboard, is taken as an argument.
- The board, the scale and the rules are declared by the program, not by a
  component: the board is the program's. A need in millimeters without a
  board stops the build with a hint about what to declare.
- To build against a checkout of DGX instead of the registry's copy, the
  checkout must sit in a directory named `jef-sure__dgx` (ESP-IDF names a
  local component after its directory, and the registry components require
  exactly that name), and the program's `idf_component.yml` must point
  `override_path` at it. Without `override_path` a copy under `components/`
  loses: the component manager downloads `jef-sure/dgx` from the registry
  anyway, the local `project_include.cmake` never runs, and
  `dgx_fonts_board` is an unknown command.

A component with a file format of its own, from which a need follows, knows
that format itself; DGX knows only its own keywords. Such a component
provides a wrapper function in its own `project_include.cmake`, which the
program calls instead of `dgx_font_need`: at configure time the wrapper
reads its file with CMake's `string(JSON)`, writes the characters it needs
to a file in the build directory, only when they change, and calls a plain
`dgx_font_need` with plain keywords. The wrapper adds its file to
`CMAKE_CONFIGURE_DEPENDS`, so that editing it reconfigures the project, and
stops with a clear message when `dgx_font_need` is not defined (a DGX older
than 0.7.2). This is how DGX-DI makes the font for its keyboard labels:

```cmake
dgx_di_keyboard_font(keys main/keys.json)
```

The wrapper takes the layout's `font` field (`postscript` and `mm`, or
`dgx`) and the key labels (`label`, or else `char` and `upper`), writes them
to `build/dgx_di/keys.letters` and calls `dgx_font_need(keys SIZE 4.5mm
POSTSCRIPT DejaVuSans TEXTS build/dgx_di/keys.letters)`. The program hands
the keyboard `dgx_font_keys()`, and the display size in millimeters is the
one from `dgx_fonts_board`: `DGX_FONTS_BOARD_WIDTH_MM`, `_HEIGHT_MM`.

A component that is also built on the host, as a game engine is, keeps its
needs in a file in DGX's format (see "Needs in a file"): its wrapper hands
the file to `dgx_font_needs_file`, and the host build runs `dgx_fonts.py
build` on the same file.

## Lists for font2c

A list is the second way, beside needs, for a component or a program that
knows its font files and characters itself: a text file with the arguments
of one ordinary `font2c` run per line, for any font file `font2c` reads, a
BDF among them (a font of bitmaps at a size it has), paths relative to the list,
`#` for comments:

```
# fonts.list: the fonts of the widgets
-n widgets_title DejaVuSans.ttf 24 0x20 0x7e 0xc4 0xdc
-n widgets_note -f note.letters DejaVuSans.ttf 18
DejaVuSans.ttf 30 0x30 0x39
```

In the component's CMakeLists, `dgx_font_list(fonts.list)`: the fonts of the
list go into the same `dgx_fonts.c` and their functions are declared in the
same `dgx_fonts.h`. A line without `-n` gets a name from the family and the
size, prefixed with the name of the output: `dgx_fonts_DejaVuSansBook30`.
One face at one size is made once, whoever asked for it, a list or a need.

Without CMake and without needs, `font2c` processes the same list on its
own:

```sh
font2c -jo fonts.list -jo more.list -o my_fonts     # my_fonts.c and my_fonts.h
```

## Using the tool by hand

The same script, `font2c/dgx_fonts.py`, answers questions without a build:

```sh
python3 font2c/dgx_fonts.py pick fonts.rules 48 --mono --letters "Привет"   # which face would be picked
python3 font2c/dgx_fonts.py list fonts.rules 48                            # every allowed face, in order
python3 font2c/dgx_fonts.py letters assets/ru.po main/keys.json            # the characters the texts yield
python3 font2c/dgx_fonts.py json build/dgx_fonts/dgx_fonts.c --font dgx_font_keys  # a font as JSON with its glyph pixels, for editors
```

`font2c` has these options for it. `-jo <list>` (repeatable) and `-o <name>`
put every font of the lists into one `<name>.c` with a `<name>.h`, merging
duplicates and naming the arrays after the fonts so nothing collides. `-n
<name>` is for a single run: the function and the files take that name, and
the static arrays get it as a prefix. `-p <file>` and `-P <dir>` apply a
patch of the font editor (see [Glyphs fixed by hand](#glyphs-fixed-by-hand)).
Without these options the output is unchanged.

## How it works

For the curious, and for whoever has to fix it.

1. **`project_include.cmake` of the DGX component.** ESP-IDF includes this
   file of every component before the CMakeLists of any of them. It defines
   `dgx_font_need`, `dgx_font_needs_file`, `dgx_font_list`,
   `dgx_fonts_scale`, `dgx_fonts_board` and `dgx_fonts_rules`. They run
   nothing; they only record their arguments in global CMake properties,
   with paths made absolute relative to the CMakeLists that declared the
   need.
2. **A deferred call.** The same file asks CMake to call `_dgx_fonts_finish`
   at the very end of the top-level CMakeLists (`cmake_language(DEFER CALL
   ...)`), when every component and `main` have declared their needs. That
   is why the order of components does not matter.
3. **One rule.** `_dgx_fonts_finish` writes `build/dgx_fonts/needs.txt` (only
   if it changed) and creates one custom command: the command is `python
   dgx_fonts.py build needs.txt build/dgx_fonts`, the inputs are the needs'
   texts, the font files, the rules, the script itself and `font2c.c`, the
   outputs are `dgx_fonts.c` and `dgx_fonts.h`. The outputs are added to the
   DGX library (`target_sources`), their directory to its public include
   path, and the library depends on the rule.
4. **Fixed output names.** CMake must know the list of sources at configure
   time, but the fonts are known only at build time. That is what used to
   make a "dynamic list of fonts" impossible. Now the list never changes: it
   is always these two files, and only their contents change.
5. **The script.** At build time `dgx_fonts.py` reads the needs, gathers the
   characters, finds the faces (`fc-list`, the rules), converts sizes to
   pixels, builds `font2c` from DGX's source if it is not built yet, writes a
   list with one line per need and runs `font2c -jo` once, together with the
   lists from `dgx_font_list`; `font2c` itself merges identical fonts and
   writes one `.c`. For existing fonts the script parses `glyph_ranges[]` from
   their `.c` and checks the characters. Then it writes `dgx_fonts.c` and
   `dgx_fonts.h`, only if they changed.
6. **What gets rebuilt.** When a text changes, the rule runs, and if the font
   changed, the single file `dgx_fonts.c` is recompiled; the header is the
   same for the same needs, so files that include it are left alone. If a
   file is merely touched, the rule runs, rewrites nothing and recompiles
   nothing. A new need in a CMakeLists reconfigures the project by itself,
   as any edit of a CMakeLists does, and so does an edit of a list given to
   `dgx_font_list`. The faces that fontconfig and the rules found are named
   in a depfile (CMake 3.20 or later), so a changed face makes the fonts
   again too. A face installed anew, or put into a directory the rules take
   faces from, is not seen until one of those files changes: touch the
   rules to make the fonts again.

Verified with ESP-IDF 5.5 and 6.0 (CMake 3.30 and 4.0): needs from a
component and from `main` in one rule, one file recompiled when a text
changes, nothing when a file is touched.
