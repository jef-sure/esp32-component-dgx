# Fonts made at build time

A CYD (ESP32-2432S028R: an ESP32 with a 320x240 ILI9341 on SPI) shows a
title, a paragraph and a running clock in three fonts that nobody converted
by hand. `main/CMakeLists.txt` declares them:

```cmake
dgx_fonts_board(PIXELS 320 240 MM 57.6 43.2)
dgx_fonts_rules(../fonts.rules)
dgx_font_need(body SIZE 2.5mm MONO TEXTS strings.txt)
dgx_font_need(title SIZE 4.5mm POSTSCRIPT DejaVuSans-Bold TEXTS title.txt)
dgx_font_need(clock DGX DejaVuSansBook32 LETTERS "0123456789:")
```

- `body` is a monospaced face picked by `fonts.rules` from the fonts
  installed on the build machine, 2.5 mm high on this display (14 px), with
  the characters of `main/strings.txt` and no others;
- `title` is DejaVu Sans Bold at 4.5 mm (25 px), with the characters of
  `main/title.txt`;
- `clock` is a font bundled with DGX, used as is; the build checks that it
  has the digits and the colon.

The build renders the first two with `font2c`, writes `dgx_fonts.c` and
`dgx_fonts.h` into the build directory and prints what it made:

```
body: DejaVu Sans Mono Book 14 px (2.5 mm), 48 glyphs
title: DejaVu Sans Bold 25 px (4.5 mm), 13 glyphs
clock: DejaVuSansBook32 of DGX, used as is
```

`main/main.c` includes `dgx_fonts.h` and draws with `dgx_font_body()`,
`dgx_font_title()` and `dgx_font_clock()`. The constants of the header
(`DGX_FONT_BODY_PX`, `DGX_FONT_TITLE_MM`, `DGX_FONT_CLOCK_MADE`,
`DGX_FONT_NEEDS(X)` and the display's numbers) go to the log at start.
Change a string in `main/strings.txt` and build again: only `dgx_fonts.c`
is recompiled.

## What the build machine needs

A C compiler with the FreeType development files (`libfreetype-dev` and
`pkg-config` on Debian and Ubuntu), fontconfig (`fc-list`) and the DejaVu
fonts (`fonts-dejavu`). The build says what is missing. Edit `fonts.rules`
to use other fonts; this shows which face the rules would pick:

```sh
python3 ../../font2c/dgx_fonts.py pick fonts.rules 14 --mono
```

## Wiring

The pins of the CYD: MOSI 23, MISO 19, SCLK 18, CS 5, DC 16, RST 17,
backlight 21. Change the constants at the top of `main/main.c` for another
board.

## Build and flash

```sh
idf.py set-target esp32
idf.py flash monitor
```

The guide: [docs/fonts-en.md](../../docs/fonts-en.md)
([Russian](../../docs/fonts-ru.md)).
