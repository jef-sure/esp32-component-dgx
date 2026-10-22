# The fonts of DGX

Every font here is a C file made by [font2c](../../font2c/README.md) from a
font file under a free license, or drawn for DGX, except one:
`IBMCGALight8x16Light8x1616`, whose origin is unknown; a program that must
not use it denies it in its rules. The table records what each
one was made from and under what license; `font2c/dgx_fonts.py` reads it to
write `fonts-used.txt` of a program and to match `deny` lines of the rules
against these fonts by family. A font added here gets its row.

| Font | Family | Made from | License |
|------|--------|-----------|---------|
| `CasusDotView` | Casus | dots drawn for DGX by Anton Petrusevich | MIT, as DGX |
| `DejaVuSansBook32` | DejaVu Sans | DejaVuSans.ttf 2.37 at 32 px, characters U+0020 to U+007E | Bitstream Vera license; the DejaVu changes are in the public domain |
| `DroidSansRegular9` | Droid Sans | Droid Sans of Google at 9 px | Apache License 2.0 |
| `IBMCGALight8x16Light8x1616` | IBM CGA | unknown: a font file named IBM CGA Light 8x16, its source not kept | unknown |
| `IBMPlexMonoMedium28` | IBM Plex Mono | IBMPlexMono-Medium.ttf 2.3 of IBM at 28 px | SIL Open Font License 1.1 |
| `LiberationSansRegular12` | Liberation Sans | LiberationSans-Regular.ttf 2.1.5 at 12 px, the characters Arial 12 had here before, Arabic aside | SIL Open Font License 1.1 |
| `NotoMonoRegular28` | Noto Mono | NotoMono-Regular.ttf 1.00 of Google at 28 px | SIL Open Font License 1.1 |
| `TerminusTTFMedium12` | Terminus (TTF) | Terminus (TTF) Medium 4.47 at 12 px | SIL Open Font License 1.1 |
| `WeatherDotView25` | Weather Icons | derived from the Weather Icons font of Erik Flowers (weathericons-regular-webfont.ttf), the font of the author's clock | SIL Open Font License 1.1, as a derivative of Weather Icons |
| `WeatherIconsRegular13` | Weather Icons | weathericons-regular-webfont.ttf 1.100 of Erik Flowers at 13 px | SIL Open Font License 1.1 |
| `WeatherIconsRegular27` | Weather Icons | weathericons-regular-webfont.ttf 1.100 of Erik Flowers at 27 px | SIL Open Font License 1.1 |
| `font0ss` | font0-ss | the handwritten font of DGX, drawn by Anton Petrusevich in the hw-fonts editor | MIT, as DGX |
