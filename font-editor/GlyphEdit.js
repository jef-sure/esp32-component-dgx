/*
 * The glyph edited: its pixels as a grid, the metrics beside, the tools over
 * it. The mouse paints dragging over the cells; a line or a rectangle is
 * dragged from where it went down. A pointerup anywhere ends a drag.
 */
const GlyphEdit = {
    template: `
    <div v-if="glyph" class="glyph-edit" @pointerup="up" @pointerleave="up">
        <div class="mb-2">
            <div class="btn-group btn-group-sm" role="group">
                <button v-for="t in tools" :key="t.id" class="btn" :class="store.tool === t.id ? 'btn-primary' : 'btn-outline-secondary'"
                    :title="t.title" @click="store.tool = t.id">{{ t.name }}</button>
            </div>
            <button class="btn btn-sm btn-outline-secondary ms-2" @click="store.undo()">Undo</button>
            <span class="ms-2 small" :class="erasing ? 'text-danger' : 'text-muted'">{{ erasingHint }}</span>
            <span class="ms-2 text-muted small">U+{{ cpHex }} {{ cpChar }}</span>
            <span v-if="store.editedCount" class="ms-2 badge bg-warning text-dark">{{ store.editedCount }} changed</span>
            <span class="ms-3 small text-muted">zoom</span>
            <input type="range" min="10" max="40" step="2" v-model.number="pixelSize" class="form-range d-inline-block align-middle"
                style="width: 8em" title="the size of a pixel of the grid">
            <span class="small text-muted">{{ pixelSize }}</span>
        </div>
        <div class="d-flex">
            <div class="pixel-box" :style="{ '--pixel-size': pixelSize + 'px' }">
                <div v-for="(row, y) in glyph.rows" :key="store.dataVersion + ':' + y" class="pixel-row">
                    <button v-for="(px, x) in row" :key="x" class="pixel"
                        :class="{ on: px === '#' }"
                        @pointerdown.prevent="down(x, y, $event)" @pointerenter="enter(x, y)" />
                </div>
            </div>
            <div class="ms-3" style="min-width: 13em">
                <div class="mb-2 small text-muted">metrics of the glyph; the red line of the views below is the baseline</div>
                <div class="input-group input-group-sm mb-2">
                    <span class="input-group-text">advance</span>
                    <input type="number" class="form-control" v-model.number="glyph.xAdvance" @focus="store.pushUndo()" @change="store.markEdited()">
                </div>
                <div class="input-group input-group-sm mb-2">
                    <span class="input-group-text">x offset</span>
                    <input type="number" class="form-control" v-model.number="glyph.xOffset" @focus="store.pushUndo()" @change="store.markEdited()">
                </div>
                <div class="input-group input-group-sm mb-3">
                    <span class="input-group-text">y offset</span>
                    <input type="number" class="form-control" v-model.number="glyph.yOffset" @focus="store.pushUndo()" @change="store.markEdited()">
                </div>
                <div class="input-group input-group-sm mb-3">
                    <span class="input-group-text">w &times; h</span>
                    <input type="number" class="form-control" v-model.number="w" @focus="store.pushUndo()" @change="resize()">
                    <input type="number" class="form-control" v-model.number="h" @focus="store.pushUndo()" @change="resize()">
                </div>
                <div class="btn-group btn-group-sm mb-2">
                    <button class="btn btn-outline-secondary" @click="store.shift(-1, 0)">&larr;</button>
                    <button class="btn btn-outline-secondary" @click="store.shift(1, 0)">&rarr;</button>
                    <button class="btn btn-outline-secondary" @click="store.shift(0, -1)">&uarr;</button>
                    <button class="btn btn-outline-secondary" @click="store.shift(0, 1)">&darr;</button>
                </div>
                <div class="btn-group btn-group-sm">
                    <button class="btn btn-outline-secondary" @click="store.fill(false)">clear</button>
                    <button class="btn btn-outline-secondary" @click="store.fill(true)">all</button>
                </div>
                <div class="mt-3 small text-muted">the glyph, zoom
                    <input type="range" min="1" max="8" step="1" v-model.number="realScale" class="form-range d-inline-block align-middle" style="width: 6em">
                    <span class="small text-muted">{{ realScale }}&times;</span>
                </div>
                <div class="real-size border rounded bg-white mt-1">
                    <canvas ref="real" class="d-block"></canvas>
                </div>
                <div class="mt-3 small text-muted">a line of it, zoom
                    <input type="range" min="1" max="8" step="1" v-model.number="sampleScale" class="form-range d-inline-block align-middle" style="width: 6em">
                    <span class="small text-muted">{{ sampleScale }}&times;</span>
                </div>
                <input type="text" class="form-control form-control-sm mb-1" v-model="sampleText" spellcheck="false" style="max-width: 13em">
                <div class="real-size border rounded bg-white">
                    <canvas ref="sample" class="d-block"></canvas>
                </div>
            </div>
        </div>
    </div>
    <div v-else class="text-muted">no glyph</div>
    `,
    setup() {
        const store = useFontStore();
        const real = VueDemi.ref(null);
        const state = VueDemi.reactive({ w: 0, h: 0, painting: false, erasing: false, downX: 0, downY: 0, pixelSize: 16, realScale: 4, sampleScale: 2, sampleText: 'Hamburgefonstiv 0123' });
        VueDemi.watch(() => store.dataVersion, () => {
            if (store.glyph) { state.w = store.glyph.w; state.h = store.glyph.h; }
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
            erasingHint: VueDemi.computed(() => state.erasing ? 'erasing (Ctrl/Shift held)' : 'Ctrl/Shift erases'),
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
                    const box = ev.currentTarget.closest('.glyph-edit').querySelector('.pixel-box');
                    if (!box) return;
                    const rows = [...box.querySelectorAll('.pixel-row')];
                    const y = rows.findIndex(r => r.contains(ev.target));
                    if (y < 0) return;
                    const x = [...rows[y].querySelectorAll('.pixel')].indexOf(ev.target.closest('.pixel'));
                    if (x < 0) return;
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
        };
    },
};
