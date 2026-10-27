# Generated fonts editor

Fixes the glyphs of the fonts font2c makes: a .c read here is edited pixel
by pixel, and the fixes are kept as a **patch**, so a font made again keeps
them.

Open `index.html` in a browser. No build: Vue and Pinia come from a CDN, as
in the hw-fonts editor.

## How it works

1. **Read the .c** of a font font2c wrote (one of `src/fonts/*.c`, or one of
   a program). Handwritten (`DGX_FONT_HW`) and dots (`DGX_FONT_DOTS`) fonts
   are not edited here.
2. **Fix glyphs**: pixels with Draw/Line/Rect, the metrics (advance,
   offsets, size), shifts; **Ctrl or Shift held erases**, for every tool,
   told beside the tools. The `zoom` slider makes the grid bigger for fine
   work. Beside the metrics the glyph is shown the way the panel draws it:
   alone at its advance (the side bearings and the baseline seen), and in a
   line of text you type with the current glyph marked; both have a `zoom`
   slider from 1x, the size the panel will show, up.
3. **Save the patch** — `<name>.patch.json`, `<name>` being the name font2c
   gives the font (family, style and size: `DroidSansRegular9`). This is the
   source of the fixes; the .c is written by font2c, not by the editor (the
   "Save the .c" button is for a look at the result, not for keeping).
   The patch records which font and size it is for when the .c says so (a
   .c font2c makes now has a comment for it; an older one does not, and the
   editor tells you above the glyphs), and font2c then refuses it for any
   other.
4. **Make the font again with the patch**:

```
font2c -p <name>.patch.json font.ttf 12 ...
# or in a list of -jo, on the line of the font:
#    font.ttf 12 0x20 0x7f -p <name>.patch.json -n MyFont
```

Every glyph of the patch replaces the rendered one whole, with its own size
and metrics, so a glyph made wider or taller here stays so; rows of
different lengths stop font2c with a message. A font made at build time
finds its patch by itself, by the name of the font, when the patch lies
beside the CMakeLists that asks for it or in `src/fonts/` of DGX: see
"Glyphs fixed by hand" in [docs/fonts-en.md](../docs/fonts-en.md).

A patch read back into the editor (the second Choose File) puts its glyphs
in place of those of the .c, pixels, size and metrics, and marks them, so
editing goes on from them and the patch saved keeps them, whether or not the
.c was made with it. Glyphs of the patch the font has not (a font made at
build time has only the characters asked for) are told, and kept in the
patch saved. A patch that says it is for another font or size is refused.

## Notes

- The comment art in a .c may be reformatted by clang-format; the editor
  reads the bytes, not the art, and writes both in step.
- A patch fits the face *and the size* it was made for: the rows are the
  pixels of the whole glyph. A font made at another size needs its own
  fixes.
- What was changed in each font of a file of several (font2c -jo) stays
  with that font when another is chosen; the patch saved is that of the
  font chosen.
