# The fonts of DGX

Every font here is a C file made by [font2c](../../font2c/README.md) from a
font file under a free license, or drawn for DGX. The table records what each
one was made from and under what license; `font2c/dgx_fonts.py` reads it to
write `fonts-used.txt` of a program and to match `deny` lines of the rules
against these fonts by family. A font added here gets its row.

The top of each `.c` says the same with the copyright notices of the makers
of its font and what was changed, and [licenses](licenses) holds the texts
of the licenses that ask to go with the fonts: the SIL Open Font License,
the Apache License, the license of the DejaVu fonts and that of Cozette. The
fonts drawn for DGX are under its MIT License, `LICENSE` at the top of DGX,
and the Creative Commons license of the IBM CGA font is named by its link,
as that license allows. `font2c` does not write this comment: a font of
DGX made again gets it back at its top by hand.

A `<name>.patch.json` here holds glyphs fixed by hand in the
[font editor](../../font-editor/README.md) for the font of that name: one
face at one size. A font of DGX made again is made with its patch (`font2c
-p`), and a font a program makes at build time from the same face at the same
size takes the patch too: see "Glyphs fixed by hand" in
[docs/fonts-en.md](../../docs/fonts-en.md).

| Font | Family | Made from | License |
|------|--------|-----------|---------|
| `CasusDotView` | Casus | dots drawn for DGX by Anton Petrusevich | MIT, as DGX |
| `CozetteRegular13` | Cozette | cozette.bdf 1.30.0 of Ines (github.com/the-moonwitch/Cozette), a bitmap font drawn at 13 px, used at its own size: ASCII, Latin-1, Cyrillic U+0400 to U+045F, dashes, quotes, the ellipsis, € and №; the Nerd Font icons left out | MIT License, (c) 2020-2025 Ines |
| `DejaVuSansBook32` | DejaVu Sans | DejaVuSans.ttf 2.37 at 32 px, characters U+0020 to U+007E | Bitstream Vera license; the DejaVu changes are in the public domain |
| `DroidSansRegular9` | Droid Sans | DroidSans.ttf 1.00 of Google at 9 px, with the glyphs of `DroidSansRegular9.patch.json` | Apache License 2.0 |
| `IBMCGALight8x16Light8x1616` | IBM CGA Light 8x16 | icl8x16u.bdf of the IBM fonts of farsil (github.com/farsil/ibmfonts), "BDF conversion (c) 2016 farsil", made after the PxPlus fonts of VileR (The Ultimate Oldschool PC Font Pack, int10h.org), made a TTF by bdf2ttf 0.0.5 and rendered at 16 px: 782 glyphs, Latin, Greek, Cyrillic, Hebrew | Creative Commons Attribution-ShareAlike 4.0 |
| `IBMPlexMonoMedium28` | IBM Plex Mono | IBMPlexMono-Medium.ttf 2.3 of IBM at 28 px | SIL Open Font License 1.1 |
| `LiberationSansRegular12` | Liberation Sans | LiberationSans-Regular.ttf 2.1.5 at 12 px, the characters Arial 12 had here before, Arabic aside, with the glyphs of `LiberationSansRegular12.patch.json` | SIL Open Font License 1.1 |
| `NotoMonoRegular28` | Noto Mono | NotoMono-Regular.ttf 1.00 of Google at 28 px | SIL Open Font License 1.1 |
| `TerminusTTFMedium12` | Terminus (TTF) | Terminus (TTF) Medium 4.47 at 12 px | SIL Open Font License 1.1 |
| `WeatherDotView25` | Weather Icons | derived from the Weather Icons font of Erik Flowers (weathericons-regular-webfont.ttf), the font of the author's clock | SIL Open Font License 1.1, as a derivative of Weather Icons |
| `WeatherIconsRegular13` | Weather Icons | weathericons-regular-webfont.ttf 1.100 of Erik Flowers at 13 px | SIL Open Font License 1.1 |
| `WeatherIconsRegular27` | Weather Icons | weathericons-regular-webfont.ttf 1.100 of Erik Flowers at 27 px | SIL Open Font License 1.1 |
| `font0ss` | font0-ss | the handwritten font of DGX, drawn by Anton Petrusevich in the hw-fonts editor | MIT, as DGX |
