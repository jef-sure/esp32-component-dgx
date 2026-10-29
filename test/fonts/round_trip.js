/*
 * The C of a bitmap font of DGX read and written back by the font editor's
 * own code, font-editor/FontC.js, for test_fonts.py:
 *
 *   node round_trip.js <dgx> read <file.c>
 *     JSON: `same`, the file written back is byte for byte the file; `stable`,
 *     what is written back is read and written again unchanged; `glyphs`, every
 *     glyph as [w, h, xAdvance, xOffset, yOffset, rows joined by '|'].
 *   node round_trip.js <dgx> stream <file.c> <out.c>
 *     the fonts of the file written as DGX_FONT_BITMAP_STREAM.
 *
 * A file the editor does not edit gives {"error": ...}.
 */
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const [dgx, command, file, out] = process.argv.slice(2);
vm.runInThisContext(fs.readFileSync(path.join(dgx, 'font-editor', 'FontC.js'), 'utf8'));

const text = fs.readFileSync(file, 'utf8');
const parsed = FontC.parse(text);
if (parsed.error) {
    console.log(JSON.stringify({ error: parsed.error }));
    process.exit(0);
}
if (command === 'stream') {
    for (const block of parsed.blocks) if (block.type === 'bitmap') block.font.f_type = 'DGX_FONT_BITMAP_STREAM';
    fs.writeFileSync(out, FontC.generateFile(parsed.blocks, parsed.tail));
    process.exit(0);
}
const back = FontC.generateFile(parsed.blocks, parsed.tail);
const again = FontC.parse(back);
const glyphs = {};
const font = parsed.fonts[0];
for (const cp of Object.keys(font.glyphs)) {
    const g = font.glyphs[cp];
    glyphs[cp] = [g.w, g.h, g.xAdvance, g.xOffset, g.yOffset, g.rows.join('|')];
}
console.log(JSON.stringify({
    same: back === text,
    stable: !again.error && FontC.generateFile(again.blocks, again.tail) === back,
    glyphs,
}));
