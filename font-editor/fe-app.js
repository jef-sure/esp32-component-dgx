var app = VueDemi.createApp({
    template: `
    <div class="container-fluid">
        <div class="row my-2 align-items-center">
            <div class="col-auto text-muted" v-if="store.fileName">{{ store.fileName }}</div>
            <div class="col-auto small" v-if="store.font" :class="store.font.identity ? 'text-muted' : 'text-warning'">{{ identityText }}</div>
            <div class="col"></div>
            <div class="col-auto">
                <label class="form-label mb-0 small text-muted me-1">font .c</label>
                <input type="file" class="form-control form-control-sm d-inline-block" style="width: 14em" accept=".c,.h" title="the .c of a font font2c wrote" @change="upload">
            </div>
            <div class="col-auto">
                <label class="form-label mb-0 small text-muted me-1">patch .json</label>
                <input type="file" class="form-control form-control-sm d-inline-block" style="width: 12em" accept=".json" title="a patch of font2c (-p)" @change="uploadPatch">
            </div>
            <div class="col-auto">
                <a class="btn btn-sm btn-outline-secondary" :class="{ disabled: !store.font }"
                    :download="patchDownloadName" :href="patchDownloadUrl">Save the patch</a>
            </div>
            <div class="col-auto">
                <a class="btn btn-sm btn-primary" :class="{ disabled: !store.font }"
                    :download="downloadName" :href="downloadUrl" ref="download">Save the .c</a>
            </div>
        </div>
        <div v-if="store.uploadError" class="alert alert-danger py-1 px-3">{{ store.uploadError }}</div>
        <div v-if="store.fonts.length">
            <div class="row">
                <div class="col-auto">
                    <GlyphEdit></GlyphEdit>
                </div>
                <div class="col">
                    <div v-if="store.fonts.length > 1" class="mb-2">
                        <div class="btn-group btn-group-sm">
                            <button v-for="(f, i) in store.fonts" :key="f.name" class="btn"
                                :class="store.current === i ? 'btn-primary' : 'btn-outline-secondary'"
                                @click="store.chooseFont(i)">{{ f.name }}</button>
                        </div>
                    </div>
                    <div class="glyph-list">
                        <button v-for="cp in store.codePoints" :key="cp" class="btn btn-sm glyph-pick"
                            :class="store.codePoint === cp ? 'btn-dark' : 'btn-outline-secondary'"
                            @click="store.choose(cp)">{{ label(cp) }}</button>
                    </div>
                </div>
            </div>
        </div>
        <div v-if="!store.fonts.length" class="text-muted">read a .c of font2c to edit its glyphs</div>
    </div>
    `,
    setup() {
        const store = useFontStore();
        const download = VueDemi.ref(null);
        const state = VueDemi.reactive({ url: '', patchUrl: '' });
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
            store, download,
            get downloadUrl() { return state.url; },
            get patchDownloadUrl() { return state.patchUrl; },
            downloadName: VueDemi.computed(() => store.fileName || 'font.c'),
            /* the patch read keeps its name; a new one is named by the font, as font2c and the build look for it */
            patchDownloadName: VueDemi.computed(() => store.patchName ||
                ((store.font ? (store.font.identity ? store.font.identity.font : store.font.name) : 'font') + '.patch.json')),
            upload(ev) {
                const file = ev.target.files[0];
                if (!file) return;
                const reader = new FileReader();
                reader.onload = () => store.loadText(String(reader.result), file.name);
                reader.readAsText(file);
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
                return id ? `${id.font}, ${id.size} px: the patch saved is for this font only`
                    : 'the .c does not say which font it is: the patch saved does not either, font2c cannot check it';
            }),
            label(cp) {
                return (cp >= 0x20 && cp < 0xD800 ? String.fromCodePoint(cp) : '?') + ' ' + cp.toString(16).toUpperCase();
            },
        };
    },
});
app.component('GlyphEdit', GlyphEdit);
app.use(Pinia.createPinia());
app.mount('#vue-app');
