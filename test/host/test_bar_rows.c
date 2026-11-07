/*
 * The rows of a long soft bar of a segment: where a row begins and ends is found from the counts of its
 * pixels, and the bar has the pixels the whole box around it gives, byte for byte. segment.c is built in
 * here with the margin of DGX_STROKE_BESIDE a variable, so the same segments are drawn by the rows always,
 * by the whole box always, and as the library chooses. The curves have their own bar in bezier.c.
 */
static float beside = 5.0f;
#define DGX_STROKE_BESIDE beside
#include "draw/segment.c"

#include <stdlib.h>

#include "check.h"

#define W 200
#define H 160

static int rnd(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

int main(void)
{
    /* the whole box, the rows always, the choice of the library */
    static const float margins[3] = {1e9f, -1e9f, 5.0f};
    dgx_screen_t      *s[3];
    for (int m = 0; m < 3; ++m) s[m] = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    CHECK(s[0] && s[1] && s[2]);
    int unlike = 0;
    srand(11);
    for (int i = 0; i < 20000; ++i) {
        const int      pen = DGX_PEN_SOFT | rnd(1, 14) | (i & 1 ? DGX_PEN_START_DISC : 0) | (i & 2 ? DGX_PEN_END_DISC : 0);
        const uint32_t color = i & 4 ? 0 : 255;
        /* ends on whole pixels, on halves and anywhere, far out of the screen too */
        const int      grid = i & 8 ? 1 : i & 16 ? 2 : 64;
        float          c[4];
        for (int k = 0; k < 4; ++k) c[k] = rnd(-100 * grid, ((k & 1 ? H : W) + 100) * grid) / (float)grid;
        const float t = i & 32 ? 1 : rnd(1, 100) / 100.0f;
        for (int m = 0; m < 3; ++m) {
            beside = margins[m];
            /* something to take off for a pen of color 0 */
            memset(((dgx_vscreen_t *)s[m])->v_array, color ? 0 : 200, W * H);
            /* a part, and the rest of it after: what a call draws on from where the call before has stopped */
            dgx_segment_part_t part = {.t = t};
            dgx_draw_thick_segment_part(s[m], c[0], c[1], c[2], c[3], pen, color, &part);
            if (i & 64) part.t = 1, dgx_draw_thick_segment_part(s[m], c[0], c[1], c[2], c[3], pen, color, &part);
        }
        for (int m = 1; m < 3; ++m) unlike += memcmp(((dgx_vscreen_t *)s[m])->v_array, ((dgx_vscreen_t *)s[0])->v_array, W * H) != 0;
    }
    CHECK(unlike == 0);
    if (unlike) fprintf(stderr, "%d segments by rows unlike those by the whole box\n", unlike);
    for (int m = 0; m < 3; ++m) dgx_screen_destroy(&s[m]);
    CHECK_DONE();
}
