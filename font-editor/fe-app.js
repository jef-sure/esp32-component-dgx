var app = VueDemi.createApp({
    template: `
    <div class="fe">
        <header class="fe-bar">
            <span class="fe-title">Font editor</span>
            <div class="btn-group btn-group-sm">
                <button class="btn" :class="store.font ? 'btn-outline-primary' : 'btn-primary'" title="the .c of a font font2c made"
                    @click="pick(fontInput)">Open .c&hellip;</button>
                <button class="btn btn-outline-primary" :disabled="!store.font" title="a patch saved before: its glyphs go over those of the .c"
                    @click="pick(patchInput)">Open patch&hellip;</button>
            </div>
            <button class="btn btn-sm" :class="unfittedCount ? 'btn-warning' : 'btn-outline-secondary'" :disabled="!unfittedCount"
                :title="fitTitle" @click="fitChanged()">Fit changed glyphs: {{ unfittedCount }}</button>
            <div class="btn-group btn-group-sm">
                <a class="btn btn-primary" :class="{ disabled: !store.font }" :download="patchDownloadName" :href="patchDownloadUrl"
                    title="the changes, for font2c -p: the source of the fixes" @click="follow($event, 'patch')">Save patch</a>
                <a class="btn btn-outline-primary" :class="{ disabled: !store.font }" :download="downloadName" :href="downloadUrl"
                    title="the .c as font2c makes it with the patch" @click="follow($event, 'c')">Save .c</a>
            </div>
            <input type="file" ref="fontInput" class="d-none" accept=".c,.h" @change="upload">
            <input type="file" ref="patchInput" class="d-none" accept=".json" @change="uploadPatch">
            <span v-if="fitTold" class="small text-success">{{ fitTold }}</span>
            <div v-if="store.fileName" class="fe-status">
                <span class="fw-semibold">{{ store.fileName }}</span>
                <span v-if="store.patchName" class="text-muted"> + {{ store.patchName }}</span>
                <span :class="store.font && store.font.identity ? 'text-muted' : 'text-warning'"> &middot; {{ identityText }}</span>
            </div>
        </header>
        <div v-if="store.font && !store.font.identity" class="alert alert-warning fe-alert">
            This .c does not say which font {{ store.font.name }} is: a font2c older than 0.9.0 made it. The patch saved
            will not name the font, so font2c cannot check that it is given to the right one, and cannot find it among
            other patches by what it holds. Make the .c again with the font2c of now, or add the line font2c writes
            after the #include, <code>/* font2c: &lt;name&gt;, "&lt;family&gt;" "&lt;style&gt;" at &lt;size&gt; px */</code>,
            and open it again.
        </div>
        <div v-if="store.uploadError" class="alert alert-danger fe-alert">{{ store.uploadError }}</div>
        <div v-if="store.uploadNote" class="alert alert-info fe-alert">{{ store.uploadNote }}</div>
        <main v-if="store.fonts.length" class="fe-main">
            <GlyphEdit></GlyphEdit>
            <section class="fe-glyphs">
                <ul v-if="store.fonts.length > 1" class="nav nav-tabs">
                    <li v-for="(f, i) in store.fonts" :key="f.name" class="nav-item">
                        <a class="nav-link" :class="{ active: store.current === i }" href="#" @click.prevent="store.chooseFont(i)"
                            :title="fixedIn(i) + ' of its glyphs changed'">{{ f.name }}
                            <span v-if="fixedIn(i)" class="badge bg-warning text-dark">{{ fixedIn(i) }}</span></a>
                    </li>
                </ul>
                <div class="fe-glyphs-head">
                    <span v-if="store.fonts.length === 1" class="fw-semibold">{{ store.font.name }}</span>
                    <div class="btn-group btn-group-sm" role="group">
                        <button class="btn" :class="!onlyChanged ? 'btn-secondary' : 'btn-outline-secondary'" @click="onlyChanged = false">all {{ store.codePoints.length }}</button>
                        <button class="btn" :class="onlyChanged ? 'btn-secondary' : 'btn-outline-secondary'" :disabled="!store.editedCount"
                            @click="onlyChanged = true">changed {{ store.editedCount }}</button>
                    </div>
                    <span class="small text-muted">changed glyphs are yellow</span>
                </div>
                <div class="glyph-list">
                    <button v-for="cp in shown" :key="cp" class="glyph-tile"
                        :class="{ current: store.codePoint === cp, changed: store.edited[cp] }"
                        :title="'U+' + hex(cp) + (store.edited[cp] ? ', changed' : '')" @click="store.choose(cp)">
                        <span class="glyph-char">{{ char(cp) }}</span><span class="glyph-code">{{ hex(cp) }}</span>
                    </button>
                </div>
            </section>
        </main>
        <div v-else class="fe-empty text-muted">
            Open the .c of a font that font2c made. Its glyphs are fixed here pixel by pixel, and the fixes are saved as a
            patch, from which font2c makes the font again.
        </div>
    </div>
    `,
    setup() {
        const store = useFontStore();
        const download = VueDemi.ref(null);
        const state = VueDemi.reactive({ url: '', patchUrl: '', onlyChanged: false, fitted: 0, fittedAt: -1 });
        /* the changed glyphs fitting would change, a list a font */
        const unfitted = VueDemi.computed(() => store.dataVersion >= 0 && store.fonts.length ? store.unfitted() : []);
        const fontInput = VueDemi.ref(null), patchInput = VueDemi.ref(null);
        const save = () => {
            if (!store.font) return;
            const blob = new Blob([store.toText()], { type: 'text/plain' });
            if (state.url) URL.revokeObjectURL(state.url);
            state.url = URL.createObjectURL(blob);
            const patch = new Blob([JSON.stringify(store.patchOf(), null, 4)], { type: 'application/json' });
            if (state.patchUrl) URL.revokeObjectURL(state.patchUrl);
            state.patchUrl = URL.createObjectURL(patch);
        };
        window.addEventListener('hashchange', () => { location.hash = ''; history.replaceState(null, '', location.pathname + location.search); });
        /* the file to save is made when the pointer comes up, after an edit:
         * the link asks for it only then, and what was drawn is in it */
        window.addEventListener('pointerup', () => VueDemi.nextTick(save));
        return {
            store, download, fontInput, patchInput,
            pick(input) { if (input) input.click(); },
            /* a link to save followed, by the pointer or from the keyboard, which gives no pointerup: the file is made
             * now, with what the editor has, and the link takes it before the browser follows it */
            follow(ev, what) {
                if (!store.font) {
                    ev.preventDefault();
                    return;
                }
                save();
                ev.currentTarget.href = what === 'c' ? state.url : state.patchUrl;
            },
            unfittedCount: VueDemi.computed(() => unfitted.value.reduce((n, cps) => n + cps.length, 0)),
            fitTitle: VueDemi.computed(() => {
                const per = store.fonts.length > 1 ? ' (' + store.fonts.map((f, i) => `${f.name} ${unfitted.value[i].length}`).join(', ') + ')' : '';
                return 'before saving the patch: every changed glyph whose metrics do not fit its pixels' + per +
                    ' is trimmed to them and gets the advance that keeps its gap before the next glyph, the one it had' +
                    ' before it was changed or the one its advance typed in left; digits keep their advance;' +
                    ' one Undo puts them all back';
            }),
            /* what fitting did, told until the next change */
            fitTold: VueDemi.computed(() => state.fittedAt === store.dataVersion ? `${state.fitted} glyph${state.fitted === 1 ? "" : "s"} fitted, Undo puts ${state.fitted === 1 ? "it" : "them"} back` : ''),
            fitChanged() {
                state.fitted = store.fitChanged();
                state.fittedAt = store.dataVersion;
            },
            get onlyChanged() { return state.onlyChanged && store.editedCount > 0; }, set onlyChanged(v) { state.onlyChanged = v; },
            /* the glyphs of the list: all of the font chosen, or those changed */
            shown: VueDemi.computed(() => state.onlyChanged && store.editedCount ? store.codePoints.filter(cp => store.edited[cp]) : store.codePoints),
            get downloadUrl() { return state.url; },
            get patchDownloadUrl() { return state.patchUrl; },
            downloadName: VueDemi.computed(() => store.fileName || 'font.c'),
            /* the patch read keeps its name; a new one is named by the font, as font2c and the build look for it,
             * or by the file when it has several fonts: font2c finds those by what they hold */
            patchDownloadName: VueDemi.computed(() => {
                if (store.patchName) return store.patchName;
                if (store.fonts.length > 1) return (store.fileName.replace(/\.[ch]$/, '') || 'fonts') + '.patch.json';
                return (store.font ? (store.font.identity ? store.font.identity.font : store.font.name) : 'font') + '.patch.json';
            }),
            upload(ev) {
                const file = ev.target.files[0];
                if (!file) return;
                const reader = new FileReader();
                reader.onload = () => store.loadText(String(reader.result), file.name);
                reader.readAsText(file);
                ev.target.value = ''; // the same file read again is read again
            },
            uploadPatch(ev) {
                const file = ev.target.files[0];
                if (!file) return;
                const reader = new FileReader();
                reader.onload = () => store.loadPatch(String(reader.result), file.name);
                reader.readAsText(file);
                ev.target.value = ''; // the same patch read again tells of it again
            },
            identityText: VueDemi.computed(() => {
                const id = store.font && store.font.identity;
                if (!id) return 'the font is not named in the .c';
                return store.fonts.length > 1 ? `${id.font}, ${id.size} px: the patch saved holds the changes of every font of the file`
                    : `${id.font}, ${id.size} px: the patch saved is for this font only`;
            }),
            fixedIn(i) {
                return Object.keys(store.editedBy[i] || {}).length;
            },
            hex(cp) {
                return cp.toString(16).toUpperCase().padStart(4, '0');
            },
            /* the character of a tile: a mark on a dotted circle, a space or a control as a sign */
            char(cp) {
                if (cp >= 0xD800 && cp < 0xE000) return '?';
                const ch = String.fromCodePoint(cp);
                if (/\p{M}/u.test(ch)) return '\u25CC' + ch;
                if (/[\p{Z}]/u.test(ch)) return '\u2423';
                if (/\p{C}/u.test(ch)) return '\u00B7';
                return ch;
            },
        };
    },
});
app.component('GlyphEdit', GlyphEdit);
app.use(Pinia.createPinia());
app.mount('#vue-app');
