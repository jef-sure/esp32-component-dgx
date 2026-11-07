/*
 * The rows of a long soft bar of a curve, as test_bar_rows.c has them for a segment: bezier.c is built in
 * here with the margin of DGX_STROKE_BESIDE a variable, and the same curves are drawn by the rows always,
 * by the whole box always, and as the library chooses, at once and in steps.
 */
static float beside = 5.0f;
#define DGX_STROKE_BESIDE beside
#include "draw/bezier.c"

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
    srand(13);
    for (int i = 0; i < 12000; ++i) {
        const int      pen = DGX_PEN_SOFT | rnd(1, 14);
        const uint32_t color = i & 4 ? 0 : 255;
        dgx_point_2d_t p[4];
        /* curves as large as the screen and larger, of few pieces and so of long ones too */
        for (int k = 0; k < 4; ++k) p[k].x = (int16_t)rnd(-200, W + 200), p[k].y = (int16_t)rnd(-200, H + 200);
        const int   pieces = i & 8 ? rnd(1, 4) : 0;
        const float t = rnd(1, 100) / 100.0f;
        for (int m = 0; m < 3; ++m) {
            beside = margins[m];
            /* something to take off for a pen of color 0 */
            memset(((dgx_vscreen_t *)s[m])->v_array, color ? 0 : 200, W * H);
            if (i & 1) {
                dgx_draw_bezier4(s[m], p, pieces, pen, color);
            } else {
                dgx_bezier_t b;
                dgx_bezier3_begin(&b, s[m], p, pieces, pen, color);
                for (int k = 1; k <= 7; ++k) dgx_bezier_draw_to(&b, k / 7.0f * t);
            }
        }
        for (int m = 1; m < 3; ++m) unlike += memcmp(((dgx_vscreen_t *)s[m])->v_array, ((dgx_vscreen_t *)s[0])->v_array, W * H) != 0;
    }
    CHECK(unlike == 0);
    if (unlike) fprintf(stderr, "%d curves by rows unlike those by the whole box\n", unlike);
    for (int m = 0; m < 3; ++m) dgx_screen_destroy(&s[m]);
    CHECK_DONE();
}
