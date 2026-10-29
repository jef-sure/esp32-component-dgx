/*
 * The glyph edited: its pixels as a grid, the tools and the moves of the
 * pixels over it, the metrics beside, the views of it as the panel shows it
 * under. The mouse paints dragging over the cells; a line or a rectangle is
 * dragged from where it went down. A pointerup anywhere ends a drag.
 */
const GlyphEdit = {
    template: `
    <div v-if="glyph" class="glyph-edit" @pointerup="up" @pointerleave="up">
        <div class="ge-head">
            <span class="ge-char">{{ cpChar }}</span>
            <span class="ge-code">U+{{ cpHex }}</span>
            <span v-if="store.edited[store.codePoint]" class="badge bg-warning text-dark">changed</span>
        </div>
        <div class="ge-tools">
            <div class="btn-group btn-group-sm" role="group" aria-label="the tool">
                <button v-for="t in tools" :key="t.id" class="btn" :class="store.tool === t.id ? 'btn-primary' : 'btn-outline-primary'"
                    :title="t.title" @click="store.tool = t.id">{{ t.name }}</button>
            </div>
            <div class="btn-group btn-group-sm" role="group" aria-label="move the pixels">
                <button class="btn btn-outline-secondary" title="move the pixels left" @click="store.shift(-1, 0)">&larr;</button>
                <button class="btn btn-outline-secondary" title="move the pixels right" @click="store.shift(1, 0)">&rarr;</button>
                <button class="btn btn-outline-secondary" title="move the pixels up" @click="store.shift(0, -1)">&uarr;</button>
                <button class="btn btn-outline-secondary" title="move the pixels down" @click="store.shift(0, 1)">&darr;</button>
            </div>
            <div class="btn-group btn-group-sm" role="group">
                <button class="btn btn-outline-secondary" title="every pixel off" @click="store.fill(false)">clear</button>
                <button class="btn btn-outline-secondary" title="every pixel on" @click="store.fill(true)">fill</button>
            </div>
            <button class="btn btn-sm btn-outline-secondary" :disabled="!store.undoStack.length" @click="store.undo()">Undo</button>
            <span class="small" :class="erasing ? 'text-danger fw-semibold' : 'text-muted'">{{ erasingHint }}</span>
        </div>
        <div class="ge-body">
            <div class="ge-grid">
                <div class="pixel-box" :style="{ '--pixel-size': pixelSize + 'px' }">
                    <div class="pixel-row ruler">
                        <div class="ruler-cell corner"></div>
                        <div v-for="x in view.x1 - view.x0" :key="x" class="ruler-cell" :class="{ zero: x - 1 + view.x0 === 0 }">{{ label(x - 1 + view.x0) }}</div>
                    </div>
                    <div v-for="row in view.rows" :key="store.dataVersion + ':' + row.y" class="pixel-row">
                        <div class="ruler-cell" :class="{ zero: row.y === 0 }">{{ label(row.y) }}</div>
                        <template v-for="c in row.cells" :key="c.x">
                            <button v-if="c.box" class="pixel" :class="c.cls" :data-x="c.bx" :data-y="c.by"
                                @pointerdown.prevent="down(c.bx, c.by, $event)" @pointerenter="enter(c.bx, c.by)" />
                            <div v-else class="pixel ctx" :class="c.cls"></div>
                        </template>
                    </div>
                    <div v-if="!glyph.rows.length" class="small text-muted px-2">no pixels: set the width and the height</div>
                </div>
                <div class="ge-legend">white: the glyph's own cells, drawn on; pale: the line around it, grey the
                    neighbours, red where they meet it; blue lines: the pen, before the glyph (column 0) and at its advance;
                    underlined: the baseline (row 0). The x offset is the column the glyph starts at, the y offset its top
                    row.</div>
                <div class="ge-neighbours">
                    <label>before <input type="text" class="form-control form-control-sm" maxlength="2" v-model="before"
                        :placeholder="autoBefore" title="the letter set before the glyph; empty: chosen by the glyph"></label>
                    <label>after <input type="text" class="form-control form-control-sm" maxlength="2" v-model="after"
                        :placeholder="autoAfter" title="the letter set after the glyph; empty: chosen by the glyph"></label>
                </div>
                <label class="ge-range">cells
                    <input type="range" min="10" max="40" step="2" v-model.number="pixelSize" class="form-range" title="the size of a cell of the grid">
                    {{ pixelSize }} px</label>
            </div>
            <div class="ge-side">
                <section class="ge-section">
                    <h6>Metrics</h6>
                    <div class="ge-fields">
                        <label for="ge-adv">advance</label>
                        <input id="ge-adv" type="number" step="1" class="form-control form-control-sm" :value="glyph.xAdvance" @focus="store.pushUndo()" @input="metric('xAdvance', $event)" @change="metric('xAdvance', $event, true)">
                        <label for="ge-xo">x offset</label>
                        <input id="ge-xo" type="number" step="1" class="form-control form-control-sm" :value="glyph.xOffset" @focus="store.pushUndo()" @input="metric('xOffset', $event)" @change="metric('xOffset', $event, true)">
                        <label for="ge-yo">y offset</label>
                        <input id="ge-yo" type="number" step="1" class="form-control form-control-sm" :value="glyph.yOffset" @focus="store.pushUndo()" @input="metric('yOffset', $event)" @change="metric('yOffset', $event, true)">
                        <label for="ge-w">width</label>
                        <input id="ge-w" type="number" class="form-control form-control-sm" v-model.number="w" @focus="store.pushUndo()" @change="resize()">
                        <label for="ge-h">height</label>
                        <input id="ge-h" type="number" class="form-control form-control-sm" v-model.number="h" @focus="store.pushUndo()" @change="resize()">
                    </div>
                    <button class="btn btn-sm btn-outline-primary mt-2" @click="store.fitMetrics()">Fit to pixels</button>
                    <div class="small text-muted mt-1">trims the empty rows and columns and sets the advance so that {{ gap }} px
                        stay before the next glyph: the gap the glyph had before it was changed, or the one the advance typed in
                        left<span v-if="digit">; a digit keeps its advance, so that numbers stand in columns</span></div>
                </section>
                <section class="ge-section">
                    <h6>Meets</h6>
                    <div class="small text-muted mb-1">letters whose pixels touch the glyph's, side by side or at a corner;
                        red: it did not touch them when the .c was read. A letter clicked is set beside the glyph.</div>
                    <div class="ge-meets"><span class="ge-meets-side">before it</span>
                        <button v-for="m in meetsList.left" :key="m.cp" class="ge-meet" :class="{ fresh: m.isNew }" @click="before = ch(m.cp)">{{ ch(m.cp) }}</button>
                        <span v-if="!meetsList.left.length" class="text-muted small">none</span></div>
                    <div class="ge-meets"><span class="ge-meets-side">after it</span>
                        <button v-for="m in meetsList.right" :key="m.cp" class="ge-meet" :class="{ fresh: m.isNew }" @click="after = ch(m.cp)">{{ ch(m.cp) }}</button>
                        <span v-if="!meetsList.right.length" class="text-muted small">none</span></div>
                </section>
            </div>
        </div>
        <section class="ge-section">
            <h6>As the panel shows it <span class="small text-muted fw-normal">the red line is the baseline</span></h6>
            <div class="ge-previews">
                <div>
                    <label class="ge-range">alone, zoom
                        <input type="range" min="1" max="8" step="1" v-model.number="realScale" class="form-range">
                        {{ realScale }}&times;</label>
                    <div class="real-size"><canvas ref="real" class="d-block"></canvas></div>
                </div>
                <div class="ge-line">
                    <label class="ge-range">in a line, zoom
                        <input type="range" min="1" max="8" step="1" v-model.number="sampleScale" class="form-range">
                        {{ sampleScale }}&times;</label>
                    <input type="text" class="form-control form-control-sm mb-1" v-model="sampleText" spellcheck="false" title="the text of the line; the glyph edited is blue">
                    <div class="real-size line-scroll"><canvas ref="sample" class="d-block"></canvas></div>
                </div>
            </div>
        </section>
    </div>
    <div v-else class="text-muted">no glyph</div>
    `,
    setup() {
        const store = useFontStore();
        const real = VueDemi.ref(null);
        const state = VueDemi.reactive({ w: 0, h: 0, painting: false, erasing: false, downX: 0, downY: 0, pixelSize: 16, realScale: 4, sampleScale: 2, sampleText: 'Hamburgefonstiv 0123',
            before: '', after: '' });
        /* the neighbour set beside a glyph when none is typed: a letter with straight stems of its alphabet and case */
        const autoNeighbour = cp => {
            if (cp >= 0x30 && cp <= 0x39) return '0';
            if ((cp >= 0x410 && cp <= 0x42F) || cp === 0x401) return 'Н';
            if (cp >= 0x400 && cp <= 0x4FF) return 'н';
            if (cp >= 0x41 && cp <= 0x5A) return 'H';
            return 'n';
        };
        /* the glyph of the font chosen for a typed letter or the automatic one */
        const neighbour = (typed, auto) => {
            const f = store.font;
            const t = typed ? f && f.glyphs[typed.codePointAt(0)] : null;
            return t || (f && f.glyphs[auto.codePointAt(0)]) || null;
        };
        /*
         * The grid: the glyph's own cells, editable, and around them the
         * neighbours as the panel sets them, by the pen, so that the gaps on
         * both sides and what they touch are seen. A cell is in pen columns
         * (0 where the glyph's pen starts) and rows from the baseline.
         */
        const view = VueDemi.computed(() => {
            const g = store.glyph, f = store.font;
            if (!g || !f || store.dataVersion < 0) return { rows: [], x0: 0, x1: 0 };
            const auto = autoNeighbour(store.codePoint);
            const left = neighbour(state.before, auto), right = neighbour(state.after, auto);
            const ghost = new Set(), key = (x, y) => x + ',' + y;
            const put = (n, penX) => n.rows.forEach((row, ry) => {
                for (let rx = 0; rx < row.length; ++rx) if (row[rx] === '#') ghost.add(key(penX + n.xOffset + rx, n.yOffset + ry));
            });
            const on = (x, y) => {
                const bx = x - g.xOffset, by = y - g.yOffset;
                return by >= 0 && by < g.h && bx >= 0 && bx < g.w && g.rows[by][bx] === '#';
            };
            /* the canvas spans the line of the font, its tallest glyph to its deepest, and the glyph whatever it became */
            let x0 = Math.min(0, g.xOffset), x1 = Math.max(g.xAdvance, g.xOffset + g.w);
            let y0 = Math.min(f.yOffsetLowest, 0, g.yOffset), y1 = Math.max(f.yBottomMax, 1, g.yOffset + g.h);
            for (const [n, penX] of [[left, left ? -left.xAdvance : 0], [right, g.xAdvance]]) {
                if (!n) continue;
                put(n, penX);
                x0 = Math.min(x0, penX + n.xOffset);
                x1 = Math.max(x1, penX + n.xOffset + n.w);
                if (n.h) { y0 = Math.min(y0, n.yOffset); y1 = Math.max(y1, n.yOffset + n.h); }
            }
            const touches = (x, y) => {
                for (let dy = -1; dy <= 1; ++dy) for (let dx = -1; dx <= 1; ++dx) if (on(x + dx, y + dy)) return true;
                return false;
            };
            const rows = [];
            for (let y = y0; y < y1; ++y) {
                const cells = [];
                for (let x = x0; x < x1; ++x) {
                    const bx = x - g.xOffset, by = y - g.yOffset, box = bx >= 0 && bx < g.w && by >= 0 && by < g.h;
                    const isGhost = ghost.has(key(x, y)), lit = box && g.rows[by][bx] === '#';
                    const cls = {
                        on: lit, ghost: isGhost && !lit, touch: isGhost && touches(x, y),
                        pen: x === 0 || x === g.xAdvance, base: y === 0,
                    };
                    cells.push({ x, box, bx, by, cls });
                }
                rows.push({ y, cells });
            }
            return { rows, x0, x1 };
        });
        /* the numbers of the rulers: every one when the cells are large enough to hold them, every fifth else */
        const rulerLabel = n => state.pixelSize >= 16 || n % 5 === 0 ? String(n) : '';
        const meetsList = VueDemi.computed(() => store.dataVersion >= 0 && store.glyph ? store.meetsOf(store.codePoint) : { left: [], right: [] });
        VueDemi.watch(() => store.dataVersion, () => {
            if (store.glyph) { state.w = store.glyph.w; state.h = store.glyph.h; }
        }, { immediate: true });
        /* cells that make a line of the font about 320 px tall on the grid,
         * set when a font is read or chosen; the slider changes them after */
        VueDemi.watch(() => store.font, font => {
            if (font) state.pixelSize = Math.max(10, Math.min(40, 2 * Math.round(160 / Math.max(1, font.yAdvance))));
        }, { immediate: true });
        /* the glyph the way the panel shows it, scaled by the slider: at 1x
         * one font pixel is one screen pixel, the size the panel draws. The
         * canvas spans the advance of the glyph and the line box of the font
         * (yAdvance tall, the baseline inside it), so the glyph is seen at
         * its true place over the baseline - a comma hangs below it, a
         * period sits on it - and the side bearings are seen too. */
        const drawReal = () => {
            const cv = real.value;
            if (!cv) return;
            const g = store.glyph;
            const font = store.font;
            if (!g || !font) { cv.width = cv.height = 0; return; }
            const s = state.realScale;
            const adv = Math.max(1, g.xAdvance);
            const left = Math.min(0, g.xOffset);
            const right = Math.max(adv, g.xOffset + g.w);
            /* the line box: the top of the tallest glyph of the font
             * (-yOffsetLowest) above the baseline, the bottom of the
             * deepest one (yBottomMax) below it, and the glyph itself
             * inside it however it was changed */
            const ascent = Math.max(-font.yOffsetLowest, -g.yOffset, 1);
            const descent = Math.max(font.yBottomMax, g.yOffset + g.h, 0);
            const dpr = window.devicePixelRatio || 1;
            cv.width = Math.round((right - left) * s * dpr);
            cv.height = Math.round((ascent + descent) * s * dpr);
            cv.style.width = (right - left) * s + 'px';
            cv.style.height = (ascent + descent) * s + 'px';
            const ctx = cv.getContext('2d');
            ctx.scale(dpr * s, dpr * s);
            ctx.fillStyle = '#fff';
            ctx.fillRect(0, 0, right - left, ascent + descent);
            /* the baseline first, at the top edge of the row y=0: the ink of
             * the glyphs that sit on it is drawn over it, so the line tells
             * the place without covering the pixels */
            ctx.fillStyle = 'rgba(220,53,69,.6)';
            ctx.fillRect(0, ascent, right - left, 1);
            ctx.fillStyle = '#212529';
            const gx = g.xOffset - left, gy = ascent + g.yOffset;
            for (let py = 0; py < g.h; ++py)
                for (let px = 0; px < g.w; ++px)
                    if (g.rows[py][px] === '#') ctx.fillRect(gx + px, gy + py, 1, 1);
        };
        VueDemi.watch(() => [store.dataVersion, store.codePoint, state.realScale], drawReal, { immediate: true });
        VueDemi.onMounted(drawReal);
        /* a line of text with the current glyph in it, the way the panel
         * shows it: the neighbours tell the shape better than the glyph
         * alone. Scaled by its own slider, 1x the real size. */
        const sample = VueDemi.ref(null);
        const drawSample = () => {
            const cv = sample.value;
            if (!cv) return;
            const font = store.font;
            const g = store.glyph;
            if (!font || !g) { cv.width = cv.height = 0; return; }
            const s = state.sampleScale;
            const cps = [];
            for (const ch of state.sampleText) {
                const cp = ch.codePointAt(0);
                if (font.glyphs[cp]) cps.push(cp);
            }
            const cur = store.codePoint;
            if (!cps.includes(cur)) cps.push(cur);
            let width = 0, top = 0, bottom = 0, first = true;
            for (const cp of cps) {
                const gg = font.glyphs[cp];
                width += gg.xAdvance;
                const t = gg.yOffset, b = gg.yOffset + gg.h;
                if (first || t < top) top = t;
                if (first || b > bottom) bottom = b;
                first = false;
            }
            if (first) { top = 0; bottom = Math.max(1, Math.round(font.yAdvance)); }
            const pad = 1;
            const baseline = pad - top;
            const dpr = window.devicePixelRatio || 1;
            cv.width = Math.round((width + 2 * pad) * s * dpr);
            cv.height = Math.round((bottom - top + 2 * pad) * s * dpr);
            cv.style.width = (width + 2 * pad) * s + 'px';
            cv.style.height = (bottom - top + 2 * pad) * s + 'px';
            const ctx = cv.getContext('2d');
            ctx.scale(dpr * s, dpr * s);
            ctx.fillStyle = '#fff';
            ctx.fillRect(0, 0, width + 2 * pad, bottom - top + 2 * pad);
            /* the baseline under the ink, same as in the glyph preview */
            ctx.fillStyle = 'rgba(220,53,69,.6)';
            ctx.fillRect(0, baseline, width + 2 * pad, 1);
            let x = pad;
            for (const cp of cps) {
                const gg = font.glyphs[cp];
                /* the glyph being edited stands out; the rest as the panel */
                ctx.fillStyle = cp === cur ? '#0d6efd' : '#212529';
                const gx = x + gg.xOffset, gy = baseline + gg.yOffset;
                for (let py = 0; py < gg.h; ++py)
                    for (let px = 0; px < gg.w; ++px)
                        if (gg.rows[py][px] === '#') ctx.fillRect(gx + px, gy + py, 1, 1);
                x += gg.xAdvance;
            }
        };
        VueDemi.watch(() => [store.dataVersion, store.codePoint, state.sampleText, state.sampleScale], drawSample, { immediate: true });
        VueDemi.onMounted(drawSample);
        /* Ctrl or Shift held means erase, for every tool: paint, line, rect.
         * Watched on the window, so the hint by the tools tells it even
         * before the pointer touches the grid. */
        const tools = [
            { id: 'draw', name: 'Draw', title: 'pixels follow the mouse while it is down; Ctrl or Shift erases' },
            { id: 'line', name: 'Line', title: 'dragged from the first cell to the last one; Ctrl or Shift erases' },
            { id: 'rect', name: 'Rect', title: 'a filled rectangle dragged; Ctrl or Shift erases' },
        ];
        const modDown = (ev) => { if (ev.ctrlKey || ev.shiftKey || ev.altKey || ev.metaKey) state.erasing = true; else state.erasing = false; };
        window.addEventListener('keydown', modDown);
        window.addEventListener('keyup', modDown);
        window.addEventListener('blur', () => { state.erasing = false; });
        const paint = (x, y, erasing) => {
            store.setPixel(x, y, !erasing);
        };
        return {
            store, tools, real, sample,
            glyph: VueDemi.computed(() => store.glyph),
            get w() { return state.w; }, set w(v) { state.w = v; },
            get h() { return state.h; }, set h(v) { state.h = v; },
            get pixelSize() { return state.pixelSize; }, set pixelSize(v) { state.pixelSize = v; },
            get realScale() { return state.realScale; }, set realScale(v) { state.realScale = v; },
            get sampleScale() { return state.sampleScale; }, set sampleScale(v) { state.sampleScale = v; },
            get sampleText() { return state.sampleText; }, set sampleText(v) { state.sampleText = v; },
            get erasing() { return state.erasing; }, set erasing(v) { state.erasing = v; },
            view, meetsList,
            get before() { return state.before; }, set before(v) { state.before = v; },
            get after() { return state.after; }, set after(v) { state.after = v; },
            autoBefore: VueDemi.computed(() => autoNeighbour(store.codePoint)),
            autoAfter: VueDemi.computed(() => autoNeighbour(store.codePoint)),
            ch: cp => String.fromCodePoint(cp),
            label: rulerLabel,
            erasingHint: VueDemi.computed(() => state.erasing ? 'erasing (Ctrl/Shift held)' : 'Ctrl/Shift erases'),
            gap: VueDemi.computed(() => store.dataVersion >= 0 ? store.gapFor(store.codePoint) : 0),
            digit: VueDemi.computed(() => store.codePoint >= 0x30 && store.codePoint <= 0x39),
            cpHex: VueDemi.computed(() => store.codePoint.toString(16).toUpperCase().padStart(4, '0')),
            cpChar: VueDemi.computed(() => (store.codePoint >= 0x20 && store.codePoint < 0xD800 ? String.fromCodePoint(store.codePoint) : '')),
            down(x, y, ev) {
                state.painting = true;
                state.erasing = !!(ev.ctrlKey || ev.shiftKey || ev.altKey || ev.metaKey);
                state.downX = x; state.downY = y;
                store.pushUndo();
                if (store.tool === 'line' || store.tool === 'rect') return;
                paint(x, y, state.erasing);
            },
            enter(x, y, ev) {
                if (!state.painting) return;
                if (store.tool === 'draw') paint(x, y, state.erasing);
            },
            up(ev) {
                if (!state.painting) return;
                state.painting = false;
                if (store.tool === 'line' || store.tool === 'rect') {
                    /* the cell of the glyph the pointer went up over; the neighbours' cells are not drawn on */
                    const cell = ev.target.closest && ev.target.closest('.pixel[data-x]');
                    if (!cell) return;
                    const x = +cell.dataset.x, y = +cell.dataset.y;
                    /* the modifiers at the release decide: the figure can be
                     * started plain and erased by pressing Ctrl before the
                     * pointer goes up, and back */
                    const erasing = ev.ctrlKey || ev.shiftKey || ev.altKey || ev.metaKey;
                    if (store.tool === 'line') store.line(state.downX, state.downY, x, y, !erasing);
                    else store.rect(state.downX, state.downY, x, y, !erasing);
                }
            },
            resize() {
                if (store.glyph && state.w > 0 && state.h > 0) store.resize(state.w, state.h);
            },
            /* a metric typed in: a whole number goes to the glyph as it is typed. Anything else, an empty field
             * too, is not taken, and the field shows the glyph's number again when it is left: an empty metric
             * made a .c that did not compile */
            metric(key, ev, done) {
                const g = store.glyph, text = ev.target.value.trim(), v = Number(text);
                if (!g) return;
                if (text !== '' && Number.isInteger(v)) g[key] = v;
                else if (done) ev.target.value = g[key];
                if (!done) return;
                if (key === 'xAdvance') store.gapFromAdvance();
                store.markEdited();
            },
        };
    },
};
