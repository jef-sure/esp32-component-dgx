#include <stdint.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

static int calls;
static int last_l, last_r, last_t, last_b;

static void record(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr;
    ++calls;
    last_l = left, last_r = right, last_t = top, last_b = bottom;
}

static bool last_is(int l, int r, int t, int b)
{
    return last_l == l && last_r == r && last_t == t && last_b == b;
}

/* user_data: the program's own, NULL on a new screen, given back to its update_screen as it was put */
typedef struct {
    int calls;
    int left, right, top, bottom;
} shown_t;

static void show(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    shown_t *s = scr->user_data;
    ++s->calls;
    s->left = left, s->right = right, s->top = top, s->bottom = bottom;
}

static void test_user_data(void)
{
    dgx_screen_t *a = dgx_vscreen_init(30, 20, 8, DgxScreenRGB), *b = dgx_vscreen_init(30, 20, 16, DgxScreenRGB);
    CHECK(a && b && a->user_data == NULL && b->user_data == NULL);
    shown_t sa = {0}, sb = {0};
    a->user_data = &sa, a->update_screen = show;
    b->user_data = &sb, b->update_screen = show;
    dgx_fill_rectangle(a, 3, 4, 5, 2, 9);
    CHECK(sa.calls == 1 && sa.left == 3 && sa.right == 7 && sa.top == 4 && sa.bottom == 5 && sb.calls == 0);
    /* a batch of its own, as code that draws makes one, sends by itself at its end */
    dgx_screen_progress_up(b);
    dgx_set_pixel(b, 1, 2, 0xffff);
    dgx_set_pixel(b, 20, 15, 0xffff);
    dgx_screen_progress_down(b);
    CHECK(sb.calls == 1 && sb.left == 1 && sb.right == 20 && sb.top == 2 && sb.bottom == 15 && sa.calls == 1);
    CHECK(a->user_data == &sa && b->user_data == &sb);
    dgx_screen_destroy(&a), dgx_screen_destroy(&b);
}

int main(void)
{
    dgx_screen_t *s = dgx_vscreen_init(20, 10, 16, DgxScreenRGB);
    s->update_screen = record;

    dgx_fill_rectangle(s, 2, 3, 4, 2, 0xffff);
    CHECK(calls == 1 && last_is(2, 5, 3, 4));

    /* changes at opposite corners are merged into one update */
    calls = 0;
    dgx_screen_progress_up(s);
    dgx_set_pixel(s, 1, 1, 0xffff);
    dgx_fill_rectangle(s, 15, 8, 3, 2, 0xffff);
    dgx_screen_progress_up(s);
    dgx_draw_line(s, 4, 6, 9, 6, 0xffff);
    CHECK(dgx_screen_progress_down(s) == 1);
    CHECK(calls == 0);
    CHECK(dgx_screen_progress_down(s) == 0);
    CHECK(calls == 1 && last_is(1, 17, 1, 9));

    /* empty batch does not flush */
    calls = 0;
    dgx_screen_progress_up(s);
    dgx_screen_progress_down(s);
    CHECK(calls == 0);

    /* an extra down does not leave the screen deferred forever */
    CHECK(dgx_screen_progress_down(s) == 0 && s->in_progress == 0);
    dgx_set_pixel(s, 3, 3, 0xffff);
    CHECK(calls == 1 && last_is(3, 3, 3, 3));
    calls = 0;

    /* clipping */
    dgx_screen_touch(s, -5, 3, -2, 100);
    CHECK(calls == 1 && last_is(0, 3, 0, 9));
    calls = 0;
    dgx_screen_touch(s, 30, 40, 0, 1);
    CHECK(calls == 0);

    /* composite primitives and text flush once */
    calls = 0;
    dgx_draw_line_thick(s, 2, 2, 15, 7, 3, 0xffff);
    CHECK(calls == 1);
    calls = 0;
    dgx_draw_triangle_solid(s, 1, 1, 18, 2, 9, 8, 0xffff);
    CHECK(calls == 1 && last_t == 1 && last_b == 8);
    calls = 0;
    dgx_font_string_utf8_screen(s, 0, 9, "ab", 0xffff, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
    CHECK(calls == 1);

    /* a dashed line: the pattern is the lowest bits of the mask, what lies above them never comes into it */
    dgx_fill_rectangle(s, 0, 0, 20, 10, 0);
    uint32_t turned = dgx_draw_line_mask(s, 0, 0, 11, 0, 0xffff, 0, 0xfffffff5u, 4);
    for (int x = 0; x < 12; ++x) CHECK((dgx_get_pixel(s, x, 0) != 0) == !(x & 1));
    CHECK(turned == 0x5u);

    dgx_screen_destroy(&s);
    test_user_data();
    CHECK_DONE();
}
