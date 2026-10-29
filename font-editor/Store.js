/*
 * The editor's state: the file read, the font chosen, the glyph edited, the
 * undo of its pixels. A glyph is edited as a grid of booleans and the metrics
 * around it; what is not edited stays as it was read.
 */
/* the box of the pixels of rows of '#' and '.': {left, right, top, bottom}, or null when there are none */
function inkBox(rows) {
    let left = Infinity, right = -1, top = Infinity, bottom = -1;
    rows.forEach((row, y) => {
        const l = row.indexOf('#');
        if (l < 0) return;
        left = Math.min(left, l);
        right = Math.max(right, row.lastIndexOf('#'));
        top = Math.min(top, y);
        bottom = y;
    });
    return right < 0 ? null : { left, right, top, bottom };
}

/* the gap on the right of the pixels of a glyph, up to its advance; null when it has no pixels */
function rightGap(g) {
    const box = inkBox(g.rows);
    return box ? g.xAdvance - (g.xOffset + box.right + 1) : null;
}

/* the first and the last pixel of each row of a glyph, by the row from the baseline: {y: {l, r}} in pen columns */
function inkRows(g, penX = 0) {
    const m = {};
    g.rows.forEach((row, y) => {
        const l = row.indexOf('#');
        if (l >= 0) m[g.yOffset + y] = { l: penX + g.xOffset + l, r: penX + g.xOffset + row.lastIndexOf('#') };
    });
    return m;
}

/* whether glyph `b` set after glyph `a` meets it: pixels side by side in a row, or at a corner a row apart */
function meets(a, b) {
    const A = inkRows(a), B = inkRows(b, a.xAdvance);
    for (const y in A) {
        for (const dy of [-1, 0, 1]) {
            const row = B[+y + dy];
            if (row && row.l - A[y].r <= 1) return true;
        }
    }
    return false;
}

/* the letters a glyph is set against: digits, the Latin and the Cyrillic alphabets */
function isLetter(cp) {
    return (cp >= 0x30 && cp <= 0x39) || (cp >= 0x41 && cp <= 0x5A) || (cp >= 0x61 && cp <= 0x7A) || (cp >= 0x410 && cp <= 0x44F) ||
        cp === 0x401 || cp === 0x451;
}

/* a digit keeps its advance when fitted: a font gives all its digits one advance, so that numbers stand in columns */
function keepsAdvance(cp) {
    return cp >= 0x30 && cp <= 0x39;
}

/*
 * The metrics of a glyph fitted to its pixels: the glyph cut to them, the
 * pixels staying where they are (the offsets take the rows and columns cut),
 * and its advance set so that `gap` pixels are left on the right of them,
 * unless `keepAdvance`. A glyph of no pixels becomes 0 x 0 and keeps its
 * advance. True when anything changed.
 */
function fitGlyph(g, gap, keepAdvance) {
    const box = inkBox(g.rows);
    const was = JSON.stringify([g.rows, g.xAdvance, g.xOffset, g.yOffset]);
    if (!box) {
        g.rows = [];
        g.w = g.h = 0;
    } else {
        g.rows = g.rows.slice(box.top, box.bottom + 1).map(r => r.slice(box.left, box.right + 1));
        g.w = box.right - box.left + 1;
        g.h = box.bottom - box.top + 1;
        g.xOffset += box.left;
        g.yOffset += box.top;
        if (!keepAdvance) g.xAdvance = g.xOffset + g.w + gap;
    }
    return JSON.stringify([g.rows, g.xAdvance, g.xOffset, g.yOffset]) !== was;
}

const useFontStore = Pinia.defineStore('font', {
    state() {
        return {
            fileName: '',
            fonts: [],           // the bitmap fonts of the file, as FontC parsed them
            blocks: [],          // the file as it goes: bitmap fonts and the blocks kept as text
            tail: '',            // the text after the last block of the file
            current: 0,
            codePoint: 0x41,
            dataVersion: 0,      // bumped on every change, the views watch it
            uploadError: '',
            uploadNote: '',      // what a patch read could not place, kept: told, not an error
            editedBy: [],        // a font: cp -> true, the glyphs changed here or by its patch
            readBy: [],          // a font: cp -> the glyph as the .c had it, to tell what it meets that it did not
            gapsBy: [],          // a font: cp -> the gap on the right a glyph keeps when fitted: as the .c had it,
                                 // as its patch remembers it, or as the advance typed in left it
            patchName: '',
            patches: [],         // a font: the glyphs of the patch read for it, as font2c -p takes them
            patchOthers: [],     // the parts of the patch read for fonts this file has not, kept for the patch saved
            undoStack: [],
            tool: 'draw',        // draw, line, rect; Ctrl/Shift erases
            drag: null,          // what a line or a rectangle is drawn from
        };
    },
    getters: {
        font(state) {
            return state.fonts[state.current] || null;
        },
        glyph(state) {
            const f = state.fonts[state.current];
            return f ? f.glyphs[state.codePoint] : null;
        },
        codePoints(state) {
            const f = state.fonts[state.current];
            return f ? f.order.slice().sort((a, b) => a - b) : [];
        },
        /* the glyphs changed in the font chosen */
        edited(state) {
            return state.editedBy[state.current] || {};
        },
        editedCount(state) {
            return Object.keys(state.editedBy[state.current] || {}).length;
        },
    },
    actions: {
        setUploadError(message) {
            this.uploadError = message;
            this.uploadNote = '';
        },
        loadText(text, name) {
            const { fonts, blocks, tail, error } = FontC.parse(text);
            if (error) {
                this.setUploadError((name || 'the file') + ': ' + error);
                return;
            }
            this.fileName = name || '';
            this.fonts = fonts;
            this.blocks = blocks;
            this.tail = tail || '';
            this.current = 0;
            const cps = fonts[0].order.slice().sort((a, b) => a - b);
            this.codePoint = cps.includes(0x41) ? 0x41 : cps[0];
            this.editedBy = fonts.map(() => ({}));
            this.readBy = fonts.map(f => {
                const read = {};
                for (const cp of f.order) {
                    const g = f.glyphs[cp];
                    read[cp] = { rows: g.rows.slice(), xAdvance: g.xAdvance, xOffset: g.xOffset, yOffset: g.yOffset };
                }
                return read;
            });
            /* the gaps as the .c has them: those FreeType drew, when the .c was made without a patch */
            this.gapsBy = fonts.map(f => {
                const gaps = {};
                for (const cp of f.order) {
                    const gap = rightGap(f.glyphs[cp]);
                    if (gap !== null) gaps[cp] = gap;
                }
                return gaps;
            });
            this.undoStack = [];
            this.uploadError = '';
            this.uploadNote = '';
            this.patchName = '';
            this.patches = fonts.map(() => ({}));
            this.patchOthers = [];
            ++this.dataVersion;
        },
        /*
         * A patch file read over the file: every part of it goes to the font
         * of the file it names, its glyphs put in place of the font's,
         * pixels, size and metrics, and marked changed, so that editing goes
         * on from them and the patch saved keeps them, whether or not the .c
         * was made with the patch. A patch of one font that names none, or
         * names it while the font chosen does not tell which it is, goes to
         * the font chosen; one that names another font is refused. Parts and
         * glyphs for what this file has not are kept for the patch saved,
         * with those of the patches read before, and told.
         */
        loadPatch(text, name) {
            const told = name || 'the patch';
            let p;
            try {
                p = JSON.parse(text);
            } catch (e) {
                this.setUploadError(told + ': ' + e.message);
                return;
            }
            if (!this.fonts.length) return;
            const v = p && p.format === 'dgx-font-patch' ? p.formatVersion : 0;
            const parts = v === 3 && Array.isArray(p.fonts) ? p.fonts : v === 1 || v === 2 ? [p] : null;
            if (!parts || parts.some(part => !part || !part.glyphs || typeof part.glyphs !== 'object')) {
                this.setUploadError(told + ': not a patch of dgx-font-patch version 1, 2 or 3');
                return;
            }
            for (const part of parts) {
                for (const [k, g] of Object.entries(part.glyphs)) {
                    const rows = g && g.rows;
                    if (!/^[0-9A-Fa-f]+$/.test(k) || (rows !== undefined && (!Array.isArray(rows) || rows.some(r => typeof r !== 'string' || r.length !== rows[0].length)))) {
                        this.setUploadError(`${told}: the glyph ${k} of the patch is not a code point with rows of one length`);
                        return;
                    }
                }
            }
            const fontText = (font, size) => `${font} at ${size} px`;
            const targets = parts.map(part => part.font === undefined ? -1 : this.fonts.findIndex(f => f.identity &&
                f.identity.font === part.font && (part.size === undefined || f.identity.size === part.size)));
            if (parts.length === 1 && targets[0] < 0) {
                const id = this.font.identity, part = parts[0];
                if (id && (part.font !== undefined || (part.size !== undefined && part.size !== id.size))) {
                    this.setUploadError(this.fonts.length === 1
                        ? `${told}: the patch is for ${fontText(part.font || id.font, part.size || id.size)}, this font is ${fontText(id.font, id.size)}`
                        : `${told}: the patch is for ${fontText(part.font || id.font, part.size || id.size)}, which is not a font of this file`);
                    return;
                }
                targets[0] = this.current;
            }
            if (parts.length > 1 && targets.every(t => t < 0)) {
                this.setUploadError(`${told}: none of its fonts is in this file (${parts.map(part => fontText(part.font, part.size)).join(', ')})`);
                return;
            }
            const absent = [];
            parts.forEach((part, i) => {
                const t = targets[i];
                if (t < 0) return;
                const f = this.fonts[t], edited = this.editedBy[t], gaps = this.gapsBy[t];
                for (const [k, pg] of Object.entries(part.glyphs)) {
                    const cp = parseInt(k, 16);
                    const g = f.glyphs[cp];
                    if (!g) { absent.push((this.fonts.length > 1 ? f.name + ' ' : '') + 'U+' + k.toUpperCase()); continue; }
                    if (pg.rows) {
                        g.rows = pg.rows.map(r => r.replace(/[^#]/g, '.'));
                        g.h = g.rows.length;
                        g.w = g.h ? g.rows[0].length : 0;
                        if (!g.w) { g.rows = []; g.h = 0; }
                    }
                    if (typeof pg.xAdvance === 'number') g.xAdvance = pg.xAdvance;
                    if (typeof pg.xOffset === 'number') g.xOffset = pg.xOffset;
                    if (typeof pg.yOffset === 'number') g.yOffset = pg.yOffset;
                    /* the gap the glyph had before it was first changed, or as it was set by hand */
                    if (typeof pg.rightGap === 'number') gaps[cp] = pg.rightGap;
                    edited[cp] = true;
                }
                /* the glyphs of a patch read before for this font stay, those of this one go over them */
                this.patches[t] = Object.assign({}, this.patches[t], part.glyphs);
            });
            /* the parts for fonts this file has not stay too, those of the patches read before as well: one part a
             * font, the glyphs of this one over theirs */
            const otherParts = parts.filter((_, i) => targets[i] < 0);
            for (const part of otherParts) {
                const same = this.patchOthers.find(o => o.font === part.font && o.size === part.size);
                if (same) same.glyphs = Object.assign({}, same.glyphs, part.glyphs);
                else this.patchOthers.push(part);
            }
            this.patchName = name || '';
            this.undoStack = [];
            const notes = [];
            if (absent.length) notes.push(`${absent.length} of its glyphs are not in this file (${absent.slice(0, 8).join(' ')}${absent.length > 8 ? ' ...' : ''})`);
            const others = otherParts.map(part => fontText(part.font, part.size));
            if (others.length === 1) notes.push(`its font ${others[0]} is not in this file`);
            else if (others.length) notes.push(`its fonts ${others.join(', ')} are not in this file`);
            const one = absent.length + others.length === 1;
            this.uploadError = '';
            this.uploadNote = notes.length ? `${told}: ${notes.join('; ')}; ${one ? 'it stays' : 'they stay'} in the patch saved` : '';
            ++this.dataVersion;
        },
        /* the glyphs of the patch of font `i`: every glyph changed here, with its rows, its metrics and the gap it keeps,
         * and the glyphs of the patch read for it that the font has not, as they were; keyed by the code point in hex,
         * in order */
        patchGlyphsOf(i) {
            const glyphs = {};
            const f = this.fonts[i], gaps = this.gapsBy[i] || {};
            const hex = cp => cp.toString(16).toUpperCase().padStart(4, '0');
            for (const [k, g] of Object.entries(this.patches[i] || {})) {
                if (!f.glyphs[parseInt(k, 16)]) glyphs[hex(parseInt(k, 16))] = g;
            }
            for (const cp of Object.keys(this.editedBy[i] || {}).map(Number)) {
                const g = f.glyphs[cp];
                if (!g) continue;
                glyphs[hex(cp)] = {
                    rows: g.rows.map(r => r),
                    xAdvance: g.xAdvance, xOffset: g.xOffset, yOffset: g.yOffset,
                };
                if (typeof gaps[cp] === 'number') glyphs[hex(cp)].rightGap = gaps[cp];
            }
            const sorted = {};
            for (const k of Object.keys(glyphs).sort((a, b) => parseInt(a, 16) - parseInt(b, 16))) sorted[k] = glyphs[k];
            return sorted;
        },
        /*
         * The patch of the file, as font2c takes it: a part for every font of
         * it that has fixes, saying which font it is for when the .c told
         * that, and the parts of the patch read for fonts the file has not,
         * as they were. font2c gives every font it makes its own part.
         */
        patchOf() {
            const fonts = [];
            this.fonts.forEach((f, i) => {
                const glyphs = this.patchGlyphsOf(i);
                if (!Object.keys(glyphs).length) return;
                const part = {};
                if (f.identity) { part.font = f.identity.font; part.size = f.identity.size; }
                part.glyphs = glyphs;
                fonts.push(part);
            });
            return { format: 'dgx-font-patch', formatVersion: 3, fonts: fonts.concat(this.patchOthers) };
        },
        /* another font of the file; what was changed in each stays with it */
        chooseFont(i) {
            this.current = i;
            const cps = this.fonts[i].order.slice().sort((a, b) => a - b);
            this.codePoint = cps.includes(0x41) ? 0x41 : cps[0];
            this.undoStack = [];
            ++this.dataVersion;
        },
        choose(cp) {
            this.codePoint = cp;
            this.undoStack = [];
            ++this.dataVersion;
        },
        /* the pixels, the size, the metrics and the gap kept of glyph `cp` of font `i`, the one chosen by default, to put
         * back */
        undoRecord(cp, i = this.current) {
            const g = this.fonts[i].glyphs[cp];
            return { font: i, cp, rows: g.rows.map(r => r), w: g.w, h: g.h, adv: g.xAdvance, xo: g.xOffset, yo: g.yOffset,
                gap: this.gapsBy[i][cp] };
        },
        /* the glyph chosen before a change; or `records`, of several glyphs changed at once, undone at once */
        pushUndo(records) {
            if (!records && !this.glyph) return;
            this.undoStack.push(records || this.undoRecord(this.codePoint));
            if (this.undoStack.length > 50) this.undoStack.shift();
        },
        undo() {
            const u = this.undoStack.pop();
            if (!u) return;
            for (const r of [].concat(u)) {
                const g = this.fonts[r.font].glyphs[r.cp];
                if (!g) continue;
                g.rows = r.rows;
                g.w = r.w;
                g.h = r.h;
                g.xAdvance = r.adv;
                g.xOffset = r.xo;
                g.yOffset = r.yo;
                if (r.gap === undefined) delete this.gapsBy[r.font][r.cp];
                else this.gapsBy[r.font][r.cp] = r.gap;
            }
            ++this.dataVersion;
        },
        /*
         * The gap on the right of the glyphs of font `i`, the one chosen by
         * default, as the face spaces them: the most common one over its
         * glyphs with pixels that were not changed here, the smaller of two
         * as common; 1 when there are none.
         */
        usualGap(i = this.current) {
            const f = this.fonts[i], edited = this.editedBy[i] || {}, count = new Map();
            for (const cp of f.order) {
                const g = f.glyphs[cp], box = edited[cp] ? null : inkBox(g.rows);
                if (!box) continue;
                const gap = g.xAdvance - (g.xOffset + box.right + 1);
                count.set(gap, (count.get(gap) || 0) + 1);
            }
            let best = 1, most = 0;
            for (const [gap, n] of count) {
                if (n > most || (n === most && gap < best)) { best = gap; most = n; }
            }
            return best;
        },
        /* the gap on the right glyph `cp` of font `i` keeps when fitted: its own, or the usual one of the font for a
         * glyph that had no pixels; `usual` saves counting it again */
        gapFor(cp, i = this.current, usual) {
            const gap = this.gapsBy[i][cp];
            return typeof gap === 'number' ? gap : usual !== undefined ? usual : this.usualGap(i);
        },
        /*
         * The letters glyph `cp` of the font chosen meets, set before it and
         * after it: {left: [{cp, isNew}], right: [...]}, `isNew` when the two
         * did not meet as the .c had them.
         */
        meetsOf(cp) {
            const f = this.fonts[this.current], read = this.readBy[this.current] || {}, g = f.glyphs[cp];
            const out = { left: [], right: [] };
            if (!g) return out;
            for (const n of f.order) {
                if (!isLetter(n)) continue;
                const o = f.glyphs[n];
                if (meets(o, g)) out.left.push({ cp: n, isNew: !(read[n] && read[cp] && meets(read[n], read[cp])) });
                if (meets(g, o)) out.right.push({ cp: n, isNew: !(read[n] && read[cp] && meets(read[cp], read[n])) });
            }
            return out;
        },
        /* the advance typed in: the gap it leaves is the one the glyph keeps from now on */
        gapFromAdvance() {
            const gap = this.glyph ? rightGap(this.glyph) : null;
            if (gap !== null) this.gapsBy[this.current][this.codePoint] = gap;
        },
        /* the metrics of the glyph chosen fitted to its pixels, with the gap it keeps on its right */
        fitMetrics() {
            const g = this.glyph, cp = this.codePoint;
            if (!g) return;
            const before = this.undoRecord(cp);
            if (!fitGlyph(g, this.gapFor(cp), keepsAdvance(cp))) return;
            this.pushUndo(before);
            this.markEdited();
        },
        /* the changed glyphs of every font of the file that fitting would change: a list of code points a font */
        unfitted() {
            return this.fonts.map((f, i) => {
                const usual = this.usualGap(i);
                return Object.keys(this.editedBy[i] || {}).map(Number).filter(cp => {
                    const g = f.glyphs[cp];
                    return fitGlyph({ rows: g.rows, w: g.w, h: g.h, xAdvance: g.xAdvance, xOffset: g.xOffset, yOffset: g.yOffset },
                        this.gapFor(cp, i, usual), keepsAdvance(cp));
                });
            });
        },
        /* every glyph changed in every font of the file fitted, each with the gap it keeps, one undo for all; the
         * number of glyphs it changed */
        fitChanged() {
            const records = [];
            this.fonts.forEach((f, i) => {
                const usual = this.usualGap(i);
                for (const cp of Object.keys(this.editedBy[i] || {}).map(Number)) {
                    const before = this.undoRecord(cp, i);
                    if (fitGlyph(f.glyphs[cp], this.gapFor(cp, i, usual), keepsAdvance(cp))) records.push(before);
                }
            });
            if (records.length) this.pushUndo(records);
            ++this.dataVersion;
            return records.length;
        },
        markEdited() {
            this.editedBy[this.current][this.codePoint] = true;
            ++this.dataVersion;
        },
        setPixel(x, y, on) {
            const g = this.glyph;
            if (!g || x < 0 || y < 0 || x >= g.w || y >= g.h) return;
            const row = g.rows[y];
            const was = row[x] === '#';
            if (was === on) return;
            g.rows[y] = row.slice(0, x) + (on ? '#' : '.') + row.slice(x + 1);
            this.markEdited();
        },
        getPixel(x, y) {
            const g = this.glyph;
            return g && x >= 0 && y >= 0 && x < g.w && y < g.h && g.rows[y][x] === '#';
        },
        /* a straight line, for stems and diagonals the renderer broke;
         * `on` false draws it in eraser */
        line(x0, y0, x1, y1, on = true) {
            const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
            const dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
            let err = dx + dy;
            for (;;) {
                this.setPixel(x0, y0, on);
                if (x0 === x1 && y0 === y1) break;
                const e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        },
        rect(x0, y0, x1, y1, on) {
            for (let y = Math.min(y0, y1); y <= Math.max(y0, y1); ++y)
                for (let x = Math.min(x0, x1); x <= Math.max(x0, x1); ++x)
                    this.setPixel(x, y, on);
        },
        /* all pixels of the glyph, for a start over */
        fill(on) {
            const g = this.glyph;
            if (!g) return;
            this.pushUndo();
            g.rows = g.rows.map(r => on ? '#'.repeat(r.length) : '.'.repeat(r.length));
            this.markEdited();
        },
        /* a glyph wider or taller, the pixels keep their place */
        resize(w, h) {
            const g = this.glyph;
            if (!g || w < 0 || h < 0 || w > 64 || h > 64) return;
            /* the undo record is pushed by the w/h inputs at focus; pushing
             * here too would undo one step at a time */
            /* a glyph of no pixels, a space, is 0 x 0: no rows of no width */
            if (!w || !h) w = h = 0;
            const rows = [];
            for (let y = 0; y < h; ++y) {
                const old = y < g.h ? g.rows[y] : '';
                rows.push((old + '.'.repeat(w)).slice(0, w));
            }
            g.rows = rows;
            g.w = w;
            g.h = h;
            this.markEdited();
        },
        shift(dx, dy) {
            const g = this.glyph;
            if (!g) return;
            this.pushUndo();
            const rows = [];
            for (let y = 0; y < g.h; ++y) {
                const srcY = y - dy;
                const src = srcY >= 0 && srcY < g.h ? g.rows[srcY] : '';
                /* the columns of the source that stay: dx cut from the left
                 * for a shift left, from the right for a shift right */
                const cut = dx < 0 ? src.slice(-dx, -dx + g.w) : src.slice(0, g.w - dx);
                const pad = '.'.repeat(g.w - cut.length);
                rows.push(dx < 0 ? cut + pad : pad + cut);
            }
            g.rows = rows;
            this.markEdited();
        },
        /* the file as text, every font of it, the edited one with its changes */
        toText() {
            return FontC.generateFile(this.blocks, this.tail);
        },
    },
});
