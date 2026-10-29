# Generated fonts editor

Fixes the glyphs of the fonts font2c makes: a .c read here is edited pixel
by pixel, and the fixes are kept as a **patch**, so a font made again keeps
them.

Open `index.html` in a browser. No build: Vue and Pinia come from a CDN, as
in the hw-fonts editor.

The page has the files on top, the glyph edited on the left and the glyphs
of the font on the right. A file of several fonts (font2c -jo) has a tab for
each font over the glyphs; the number on a tab is how many of its glyphs are
changed. Changed glyphs are yellow in the list, and `changed` shows them
alone.

## How it works

1. **Open .c**: a font font2c wrote (one of `src/fonts/*.c`, or one of a
   program). Handwritten (`DGX_FONT_HW`) and dots (`DGX_FONT_DOTS`) fonts
   are not edited here.
2. **Fix glyphs**: pixels with Draw/Line/Rect, moved with the arrows over
   the grid, the metrics (advance, offsets, width and height) beside it;
   **Ctrl or Shift held erases**, for every tool, told beside the tools. The
   white cells are the glyph's own, the ones drawn on. Around them the grid
   goes on, pale, over the whole height of the font: the rulers count the
   columns from the pen and the rows from the baseline, blue lines mark the
   pen (column 0) and the advance, and the baseline row (row 0) is
   underlined, so the x offset is the column the glyph starts at and the y
   offset its top row. The letters set before and after the glyph are drawn
   there in grey, red where they touch it: `n`, `H`, `н`, `Н` or `0` by its
   alphabet and case, or any typed in under the grid. `Meets` beside the
   metrics lists the letters the glyph touches on each side, side by side or
   at a corner, in red those it did not touch when the .c was read; one
   clicked is set beside the glyph in the grid. The `cells` slider makes the
   grid bigger or smaller. Under it the glyph is shown the way the panel
   draws it: alone at its advance, and in a line of text you type with the
   current glyph marked; both have a `zoom` slider from 1x, the size the
   panel will show, up.
3. **Fit the metrics** to the pixels: `Fit to pixels` under the metrics
   trims the empty rows and columns of the glyph, its pixels staying where
   they are, and sets its advance so that the glyph keeps its gap before
   the next glyph. That gap is the one the glyph had when the .c was read,
   which is the one FreeType drew when the .c was made without the patch;
   the patch saved remembers it for each glyph as `rightGap`, so a .c made
   with the patch does not lose it. An advance typed in sets the gap the
   glyph keeps from then on. A digit keeps its advance: a font gives all its
   digits one advance, so that numbers stand in columns. Before saving,
   `Fit changed glyphs` on the top bar does the same for every changed
   glyph of the file whose metrics do not fit its pixels; it tells how many
   there are, and one Undo puts them all back.
4. **Save patch**: one file for every font of the .c that has changes.
   A .c of one font gives `<name>.patch.json`, `<name>` being the name
   font2c gives the font (family, style and size: `DroidSansRegular9`); a .c
   of several (font2c -jo) gives one named after the .c, `dgx_fonts.patch.json`
   for `dgx_fonts.c`, with a part for each font. This is the source of the
   fixes. **Save .c** writes the .c as font2c makes it with the patch, byte
   for byte, but the patch is what is kept: font2c makes the .c from it
   again whenever the font changes. Each part records
   which font and size it is for when the .c says so (a .c font2c makes now
   has a comment for it; an older one does not, and the editor tells you
   above the glyphs), and font2c then gives it to that font only.
5. **Make the font again with the patch**:

```
font2c -p <name>.patch.json font.ttf 12 ...
# fonts of a list: -P finds each font the part that names it
font2c -jo fonts.list -o fonts -P <the directory of the patch>
# or in a list of -jo, on the line of the font:
#    font.ttf 12 0x20 0x7f -p <name>.patch.json -n MyFont
```

Every glyph of the patch replaces the rendered one whole, with its own size
and metrics, so a glyph made wider or taller here stays so; rows of
different lengths stop font2c with a message. A font made at build time
finds its patch by itself, by the part that names it, when the patch lies
beside the CMakeLists that asks for it or in `src/fonts/` of DGX: see
"Glyphs fixed by hand" in [docs/fonts-en.md](../docs/fonts-en.md).

A patch read back into the editor (**Open patch**) puts its glyphs
in place of those of the .c, pixels, size and metrics, and marks them, so
editing goes on from them and the patch saved keeps them, whether or not the
.c was made with it. Each part goes to the font of the file it names; a
patch of one font that names none goes to the font chosen. Fonts of the
patch the file has not, and glyphs a font has not (a font made at build time
has only the characters asked for), are told and kept in the patch saved,
also when another patch is read after it: what that one has goes over them.
A patch of one font that names another font or size is refused.

## Notes

- The comment art in a .c may be reformatted by clang-format; the editor
  reads the bytes, not the art, and writes both in step.
- A patch fits the face *and the size* it was made for: the rows are the
  pixels of the whole glyph. A font made at another size needs its own
  fixes.
- What was changed in each font of a file of several (font2c -jo) stays
  with that font when another is chosen. The glyph list shows the font
  chosen only; `Fit changed glyphs` fits every font of the file, as the
  patch saved holds them all.
- A patch of an older editor, version 1 or 2, is one font; it is read as
  before, and saved again as version 3.
