/*
 * The editor's state: the file read, the font chosen, the glyph edited, the
 * undo of its pixels. A glyph is edited as a grid of booleans and the metrics
 * around it; what is not edited stays as it was read.
 */
const useFontStore = Pinia.defineStore('font', {
    state() {
        return {
            fileName: '',
            head: '',            // the text before the first font of the file
            fonts: [],           // the bitmap fonts of the file, as FontC parsed them
            blocks: [],          // the file as it goes: bitmap fonts and the blocks kept as text
            tail: '',            // the text after the last block of the file
            current: 0,
            codePoint: 0x41,
            dataVersion: 0,      // bumped on every change, the views watch it
            uploadError: '',
            editedBy: [],        // a font: cp -> true, the glyphs changed here or by its patch
            patchName: '',
            patches: [],         // a font: the glyphs of the patch read for it, as font2c -p takes them
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
        },
        loadText(text, name) {
            const { fonts, blocks, tail, error } = FontC.parse(text);
            if (error) {
                this.setUploadError((name || 'the file') + ': ' + error);
                return;
            }
            const headEnd = text.indexOf('static const uint8_t');
            /* what the first font is, font2c's comment, goes with that font when the file is written */
            this.head = (headEnd > 0 ? text.slice(0, headEnd) : '#include "dgx_font.h"\n').replace(/\/\*\s*font2c:[^*]*\*\/\n?/, '');
            this.fileName = name || '';
            this.fonts = fonts;
            this.blocks = blocks;
            this.tail = tail || '';
            this.current = 0;
            const cps = fonts[0].order.slice().sort((a, b) => a - b);
            this.codePoint = cps.includes(0x41) ? 0x41 : cps[0];
            this.editedBy = fonts.map(() => ({}));
            this.undoStack = [];
            this.uploadError = '';
            this.patchName = '';
            this.patches = fonts.map(() => ({}));
            ++this.dataVersion;
        },
        /*
         * A patch file read over the font chosen: its glyphs put in place of
         * the font's, pixels, size and metrics, and marked changed, so that
         * editing goes on from them and a patch saved keeps them, whether or
         * not the .c was made with the patch. A patch that says it is for
         * another font or size is refused; its glyphs the font has not are
         * kept for the patch saved, and told.
         */
        loadPatch(text, name) {
            let p;
            try {
                p = JSON.parse(text);
            } catch (e) {
                this.setUploadError((name || 'the patch') + ': ' + e.message);
                return;
            }
            const f = this.fonts[this.current];
            if (!f) return;
            if (p.format !== 'dgx-font-patch' || (p.formatVersion !== 1 && p.formatVersion !== 2) || !p.glyphs || typeof p.glyphs !== 'object') {
                this.setUploadError((name || 'the patch') + ': not a patch of dgx-font-patch version 1 or 2');
                return;
            }
            const id = f.identity;
            if (id && ((p.font !== undefined && p.font !== id.font) || (p.size !== undefined && p.size !== id.size))) {
                this.setUploadError(`${name || 'the patch'}: the patch is for ${p.font || id.font} at ${p.size || id.size} px, this font is ${id.font} at ${id.size} px`);
                return;
            }
            for (const [k, g] of Object.entries(p.glyphs)) {
                const rows = g && g.rows;
                if (!/^[0-9A-Fa-f]+$/.test(k) || (rows !== undefined && (!Array.isArray(rows) || rows.some(r => typeof r !== 'string' || r.length !== rows[0].length)))) {
                    this.setUploadError(`${name || 'the patch'}: the glyph ${k} of the patch is not a code point with rows of one length`);
                    return;
                }
            }
            const absent = [];
            const edited = this.editedBy[this.current];
            for (const [k, pg] of Object.entries(p.glyphs)) {
                const cp = parseInt(k, 16);
                const g = f.glyphs[cp];
                if (!g) { absent.push('U+' + k.toUpperCase()); continue; }
                if (pg.rows) {
                    g.rows = pg.rows.map(r => r.replace(/[^#]/g, '.'));
                    g.h = g.rows.length;
                    g.w = g.h ? g.rows[0].length : 0;
                    if (!g.w) { g.rows = []; g.h = 0; }
                }
                if (typeof pg.xAdvance === 'number') g.xAdvance = pg.xAdvance;
                if (typeof pg.xOffset === 'number') g.xOffset = pg.xOffset;
                if (typeof pg.yOffset === 'number') g.yOffset = pg.yOffset;
                edited[cp] = true;
            }
            this.patches[this.current] = p.glyphs;
            this.patchName = name || '';
            this.undoStack = [];
            this.uploadError = absent.length ? `${name || 'the patch'}: ${absent.length} of its glyphs are not in this font (${absent.slice(0, 8).join(' ')}${absent.length > 8 ? ' ...' : ''}); they stay in the patch saved` : '';
            ++this.dataVersion;
        },
        /*
         * The patch of the font chosen, as font2c -p takes it: every glyph
         * changed here, with its rows and metrics, and the glyphs of the
         * patch read that this font has not, as they were. It says which font
         * it is for when the .c told that.
         */
        patchOf() {
            const glyphs = {};
            const f = this.fonts[this.current];
            const hex = cp => cp.toString(16).toUpperCase().padStart(4, '0');
            for (const [k, g] of Object.entries(this.patches[this.current] || {})) {
                if (!f.glyphs[parseInt(k, 16)]) glyphs[hex(parseInt(k, 16))] = g;
            }
            for (const cp of Object.keys(this.editedBy[this.current] || {}).map(Number).sort((a, b) => a - b)) {
                const g = f.glyphs[cp];
                if (!g) continue;
                glyphs[hex(cp)] = {
                    rows: g.rows.map(r => r),
                    xAdvance: g.xAdvance, xOffset: g.xOffset, yOffset: g.yOffset,
                };
            }
            const sorted = {};
            for (const k of Object.keys(glyphs).sort((a, b) => parseInt(a, 16) - parseInt(b, 16))) sorted[k] = glyphs[k];
            const out = { format: 'dgx-font-patch', formatVersion: 2 };
            if (f.identity) { out.font = f.identity.font; out.size = f.identity.size; }
            out.glyphs = sorted;
            return out;
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
        /* the pixels, the size and the metrics before a change, to put back */
        pushUndo() {
            const g = this.glyph;
            if (!g) return;
            this.undoStack.push({ cp: this.codePoint, rows: g.rows.map(r => r), w: g.w, h: g.h,
                adv: g.xAdvance, xo: g.xOffset, yo: g.yOffset });
            if (this.undoStack.length > 50) this.undoStack.shift();
        },
        undo() {
            const u = this.undoStack.pop();
            if (!u) return;
            const g = this.fonts[this.current].glyphs[u.cp];
            if (g) {
                g.rows = u.rows;
                g.w = u.w;
                g.h = u.h;
                g.xAdvance = u.adv;
                g.xOffset = u.xo;
                g.yOffset = u.yo;
            }
            ++this.dataVersion;
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
            return FontC.generateFile(this.fonts, this.blocks, this.tail, this.head);
        },
    },
});
