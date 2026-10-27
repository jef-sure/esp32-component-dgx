/*
 * A DGX bitmap font as C (what font2c writes), read and written back.
 *
 * The editor edits pixels; everything else in the file is kept as it was
 * found, and written the way font2c writes it: bitmaps[] with a comment of
 * the glyph and its rows as '#' and ' ', the glyph_t table, glyph_ranges[]
 * and the dgx_font_t at the end. Only the rows of a glyph are re-encoded
 * when its pixels change; its width and height are the pixels'.
 */

var FontC = (() => {

    function stripComments(text) {
        return text.replace(/\/\*.*?\*\//gs, '').replace(/\/\/[^\n]*/g, '');
    }

    /* the blocks of the fonts of a file: [{name, from, to}] around every dgx_font_t *name() */
    function fontBlocks(text) {
        const ends = [...text.matchAll(/dgx_font_t\s*\*\s*(\w+)\s*\(\s*(?:void)?\s*\)\s*\{/g)].map(m => ({ name: m[1], end: m.index + m[0].length }));
        const blocks = [];
        let start = 0;
        for (const { name, end } of ends) {
            let close = text.indexOf('\n}', end);
            if (close < 0) close = text.length; else close += 2;
            blocks.push({ name, from: start, to: close });
            start = close;
        }
        return blocks;
    }

    /* rows of '#' and '.' back into bytes, MSB first, rows padded to a byte */
    function encodeRows(rows) {
        if (!rows.length) return [];
        const bpr = (rows[0].length + 7) >> 3;
        const out = new Array(bpr * rows.length).fill(0);
        rows.forEach((row, y) => {
            for (let x = 0; x < row.length; ++x)
                if (row[x] === '#') out[y * bpr + (x >> 3)] |= 0x80 >> (x & 7);
        });
        return out;
    }

    function bytesOf(rows) { return encodeRows(rows); }

    /* the ranges a sorted list of code points makes, as font2c makes them */
    function rangesOf(points) {
        const ranges = [];
        for (const cp of points) {
            const last = ranges[ranges.length - 1];
            if (last && cp <= last.first + last.number) last.number = cp - last.first + 1;
            else ranges.push({ first: cp, number: 1 });
        }
        return ranges;
    }

    /*
     * Which font a block is, so that a patch saved for it says so and font2c
     * refuses it for another: {font, size, family, style}, the name font2c
     * gives the font (family, style and size) and its size in pixels. font2c
     * writes it in a comment; a font of a -jo list that predates the comment
     * has its name in the prefix of its arrays and its size in the comment
     * of the list. Null when the block does not tell.
     */
    function identityOf(body, prefix) {
        const m = /\/\*\s*font2c:\s*(\w+),\s*"([^"]*)"\s*"([^"]*)"\s*at\s*(\d+)\s*px\s*\*\//.exec(body);
        if (m) return { font: m[1], family: m[2], style: m[3], size: +m[4] };
        const jo = /\/\*\s*\w+:\s*.*?\s+at\s+(\d+)\s*px\s*\*\//.exec(body);
        if (prefix && jo) return { font: prefix.replace(/_$/, ''), size: +jo[1] };
        return null;
    }

    /*
     * Read one font of the text of a .c file: {font, at} with `at` the place
     * of its bitmaps[] in the text, or null when the block has no bitmaps (a
     * handwritten or a dots font), which the caller keeps as it was found.
     * The arrays of a file of several fonts (font2c -jo) carry a prefix; it
     * is kept, so the file written has the names the one read had.
     */
    function readBlock(text, block) {
        const body = text.slice(block.from, block.to);
        const m = /static\s+const\s+uint8_t\s+(\w*)bitmaps\s*\[\s*\]\s*=\s*\{/.exec(body);
        if (!m) return null; // not a bitmap font
        const prefix = m[1];
        if (body.indexOf('};', m.index + m[0].length) < 0) return null;
        const glyphRe = /static\s+const\s+glyph_t\s+\w*glyphs\s*\[\s*\]\s*=\s*\{/g;
        const gm = glyphRe.exec(body);
        if (!gm) return null;
        if (body.indexOf('};', gm.index) < 0) return null;
        const rm = /static\s+const\s+glyph_array_t\s+\w*glyph_ranges\s*\[\s*\]\s*=\s*\{/g.exec(body);
        if (!rm) return null;
        if (body.indexOf('};', rm.index) < 0) return null;

        const clean = stripComments(body);
        const cm = /\w*bitmaps\s*\[\s*\]\s*=\s*\{(.*?)\};/s.exec(clean);
        const bitmaps = [...cm[1].matchAll(/0x([0-9A-Fa-f]+)/g)].map(x => parseInt(x[1], 16));
        const gcl = /glyph_t\s+\w*glyphs\s*\[\s*\]\s*=\s*\{(.*?)\};/s.exec(clean);
        const glyphs = [...gcl[1].matchAll(/\{\s*\{\s*(?:\.bitmap\s*=\s*)?\w*bitmaps\s*\+\s*(\d+)\s*\}\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}/g)]
            .map(g => ({ at: +g[1], w: +g[2], h: +g[3], adv: +g[4], xo: +g[5], yo: +g[6] }));
        const rcl = /glyph_array_t\s+\w*glyph_ranges\s*\[\s*\]\s*=\s*\{(.*?)\};/s.exec(clean);
        const ranges = [...rcl[1].matchAll(/\{\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*\w*glyphs\s*\+\s*(\d+)\s*\}/g)]
            .filter(r => !/^0+$/.test(r[1]) || +r[2] > 0) // the {0,0,0} stop
            .filter(r => !(parseInt(r[1], 0) === 0 && parseInt(r[2], 0) === 0))
            .map(r => ({ first: parseInt(r[1], 0), number: parseInt(r[2], 0), k: +r[3] }));

        const head = body.slice(body.lastIndexOf('dgx_font_t'));
        const fields = {};
        for (const f of head.matchAll(/\.(\w+)\s*=\s*([-\w.]+)/g)) fields[f[1]] = f[2];
        const stream = (fields.f_type || 'DGX_FONT_BITMAP_LINES') === 'DGX_FONT_BITMAP_STREAM';

        const font = {
            name: block.name,
            prefix,
            identity: identityOf(body.slice(0, m.index + m[0].length), prefix),
            f_type: fields.f_type || 'DGX_FONT_BITMAP_LINES',
            yAdvance: +fields.yAdvance || 0,
            yOffsetLowest: +fields.yOffsetLowest || 0,
            xWidest: +fields.xWidest || 0,
            xWidthAverage: fields.xWidthAverage || '0',
            yBottomMax: +fields.yBottomMax || 0,
            xOffsetLowest: +fields.xOffsetLowest || 0,
            xRightMax: +fields.xRightMax || 0,
            number_of_ranges: ranges.length,
            glyphs: {},
            order: [],
        };
        for (const r of ranges) {
            for (let i = 0; i < r.number; ++i) {
                const cp = r.first + i, g = glyphs[r.k + i];
                if (!g) continue;
                const rows = [];
                for (let y = 0; y < g.h; ++y) {
                    let row = '';
                    for (let x = 0; x < g.w; ++x) {
                        const bit = stream ? y * g.w + x : y * 8 * ((g.w + 7) >> 3) + x;
                        const b = bitmaps[g.at + (bit >> 3)] || 0;
                        row += (b >> (7 - (bit & 7)) & 1) ? '#' : '.';
                    }
                    rows.push(row);
                }
                font.glyphs[cp] = { w: g.w, h: g.h, xAdvance: g.adv, xOffset: g.xo, yOffset: g.yo, rows };
                font.order.push(cp);
            }
        }
        return { font, at: m.index };
    }

    /* every font of a .c text: {fonts, blocks, error}; `blocks` are the spans
     * of the text as they go: a bitmap font as its parsed state, a block that
     * is not edited (handwritten, dots) as its text, kept as it was found */
    function parse(text) {
        const spans = fontBlocks(text);
        if (!spans.length) return { fonts: [], blocks: [], error: 'no dgx_font_t *<name>() in it: not a font of font2c' };
        const fonts = [];
        const blocks = [];
        let tail = text; // the text after the last dgx_font_t block
        for (const b of spans) {
            const r = readBlock(text, b);
            if (r) {
                fonts.push(r.font);
                blocks.push({ type: 'bitmap', at: r.at, font: r.font });
            } else {
                blocks.push({ type: 'text', at: b.from, text: text.slice(b.from, b.to) });
            }
            tail = text.slice(b.to);
        }
        if (!fonts.length) return { fonts: [], blocks: [], error: 'no bitmap font in it (handwritten and dots fonts are not edited here)' };
        return { fonts, blocks, tail, error: null };
    }

    /* the measures of the whole font over its glyphs as they are now, as font2c takes them */
    function measures(font, cps) {
        const r = { yOffsetLowest: 0, yBottomMax: 0, xWidest: 0, xOffsetLowest: 0, xRightMax: 0, xWidthAverage: '0.000000' };
        let sum = 0;
        cps.forEach((cp, i) => {
            const g = font.glyphs[cp];
            const take = (key, v, less) => { if (!i || (less ? v < r[key] : v > r[key])) r[key] = v; };
            take('yOffsetLowest', g.yOffset, true);
            take('yBottomMax', g.yOffset + g.h, false);
            take('xWidest', g.w, false);
            take('xOffsetLowest', g.xOffset, true);
            take('xRightMax', g.xOffset + g.w, false);
            sum += g.w;
        });
        if (cps.length) r.xWidthAverage = (sum / cps.length).toFixed(6);
        return r;
    }

    /*
     * One font as the .c font2c writes: the header of the block as it was,
     * then bitmaps[] with the art comment of every glyph, the tables and the
     * dgx_font_t. `head` is the text before the arrays (the #include and what
     * else there was); the arrays carry the prefix the file had (font2c -jo).
     */
    function generate(font, head) {
        const out = [];
        const p = font.prefix || '';
        const cps = font.order.slice().sort((a, b) => a - b);
        const ranges = rangesOf(cps);
        const m = measures(font, cps);
        const id = font.identity;
        out.push(head.replace(/\s*$/, '\n'));
        if (id && id.family !== undefined) out.push(`/* font2c: ${id.font}, "${id.family}" "${id.style}" at ${id.size} px */\n`);
        out.push(`static const uint8_t ${p}bitmaps[] = {\n`);
        let at = 0;
        const glyphRows = {};
        for (const cp of cps) {
            const g = font.glyphs[cp];
            const bytes = bytesOf(g.rows);
            out.push(`  /* ${cp.toString(16).toUpperCase().padStart(4, '0')} */`);
            for (const row of g.rows) out.push('/* ' + row.replace(/\./g, ' ') + ' */');
            out.push(bytes.map((b, i) => (i % 12 === 0 ? '\n' : '') + (at + i ? ', ' : '') + '0x' + b.toString(16).toUpperCase().padStart(2, '0')).join('') + '\n');
            glyphRows[cp] = { at, len: bytes.length };
            at += bytes.length;
        }
        out.push('};\n\n');
        out.push(`static const glyph_t ${p}glyphs[] = {\n  `);
        let gi = 0;
        for (const r of ranges) {
            for (let i = 0; i < r.number; ++i) {
                const cp = r.first + i, g = font.glyphs[cp], gr = glyphRows[cp];
                out.push(`${gi ? ', ' : ''}  { {.bitmap = ${p}bitmaps + ${String(gr.at).padStart(5)} }, ${String(g.w).padStart(3)}, ${String(g.h).padStart(3)}, ${String(g.xAdvance).padStart(3)}, ${String(g.xOffset).padStart(4)}, ${String(g.yOffset).padStart(4)} } /* ${cp.toString(16).toUpperCase().padStart(4, '0')} */\n`);
                ++gi;
            }
        }
        out.push('};\n');
        out.push(`static const glyph_array_t ${p}glyph_ranges[] = {\n  `);
        let k = 0;
        ranges.forEach((r, ri) => {
            out.push(`${ri ? ',' : ''} {0x${r.first.toString(16).padEnd(4)}, 0x${r.number.toString(16).padEnd(4)}, ${p}glyphs + ${k} }\n`);
            k += r.number;
        });
        out.push(', {0x0   , 0x0   , 0 }\n');
        out.push('};\n');
        out.push(`// Bitmap size: ${at}\n`);
        out.push(`dgx_font_t* ${font.name}() {\n`);
        out.push(`\tstatic dgx_font_t rval = { \n`);
        out.push(`    .glyph_ranges = ${p}glyph_ranges,\n`);
        out.push(`    .yAdvance = ${font.yAdvance},\n`);
        out.push(`    .yOffsetLowest = ${m.yOffsetLowest},\n`);
        out.push(`    .xWidest = ${m.xWidest},\n`);
        out.push(`    .xWidthAverage = ${m.xWidthAverage},\n`);
        out.push(`    .f_type = ${font.f_type},\n`);
        out.push(`    .yBottomMax = ${m.yBottomMax},\n`);
        out.push(`    .xOffsetLowest = ${m.xOffsetLowest},\n`);
        out.push(`    .xRightMax = ${m.xRightMax},\n`);
        out.push(`    .number_of_ranges = ${ranges.length}\n`);
        out.push(`  };\n`);
        out.push(`\treturn &rval;\n}\n`);
        return out.join('');
    }

    /*
     * The whole file as it is written back: `head` before everything, then
     * every block as it goes — a bitmap font generated from its parsed state,
     * a block that is not edited (handwritten, dots) as its text, kept as it
     * was found — and the text after the last block.
     */
    function generateFile(fonts, blocks, tail, head) {
        let out = head || '';
        let first = true;
        for (const b of blocks) {
            if (b.type === 'text') { out += b.text; continue; }
            out += generate(b.font, first ? '' : '\n');
            first = false;
        }
        return out + (tail || '');
    }

    return { parse, generate, generateFile, rangesOf, encodeRows, measures };
})();
