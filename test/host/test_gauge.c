/*
 * The arc gauge: its empty ring is there after dgx_gauge_init_with(), and a change of the value is one update of the
 * screen. A pixel of the ring is of one step only: a redraw writes each once, and the picture of a value is the same
 * whichever value the gauge came from.
 */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_gauge.h"
#include "dgx_vscreen.h"

#define W 100
#define H 100
#define BACK 0x2104u
#define LOW 0x07e0u
#define HIGH 0xf800u

static int updates;

static void count_update(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
    ++updates;
}

static uint32_t gauge_color(int value, void *user_data)
{
    (void)user_data;
    return value < 70 ? LOW : HIGH;
}

/* how many times each pixel was written, through the fill of the screen, which the gauge draws by */
static uint8_t                     writes[H][W];
static dgx_scr_fill_rectangle_func plain_fill;

static void counting_fill(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            if (i >= 0 && j >= 0 && i < W && j < H && writes[j][i] < 255) ++writes[j][i];
    plain_fill(scr, x, y, w, h, color);
}

static uint32_t value_color(int value, void *user_data)
{
    (void)user_data;
    return 0x0800u + (uint32_t)value;
}

static int pixels_of(dgx_screen_t *scr, uint32_t color)
{
    int n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) n += dgx_get_pixel(scr, x, y) == color;
    }
    return n;
}

/* the user_data of dgx_gauge_init_with() reaches its color function with the values; the deprecated
 * dgx_gauge_init() with a function of the value alone draws the same ring */
static uint32_t counted_color(int value, void *user_data)
{
    ++*(int *)user_data;
    return gauge_color(value, NULL);
}

static uint32_t value_alone(int value) { return value < 70 ? LOW : HIGH; }

static void test_user_data(void)
{
    dgx_screen_t *a = dgx_vscreen_init(W, H, 16, DgxScreenRGB), *b = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    int           calls = 0;
    dgx_gauge_t   with, before;
    dgx_gauge_init_with(&with, a, W / 2, H / 2, 30, 10, 225.0f, 270, 0, 100, BACK, counted_color, &calls);
    CHECK(calls == 0); /* the empty ring needs no color of a value */
    dgx_gauge_set_value(&with, 80);
    CHECK(calls > 0);
    DGX_DEPRECATED_USE_BEGIN
    dgx_gauge_init(&before, b, W / 2, H / 2, 30, 10, 225.0f, 270, 0, 100, BACK, value_alone);
    DGX_DEPRECATED_USE_END
    dgx_gauge_set_value(&before, 80);
    int differ = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) differ += dgx_get_pixel(a, x, y) != dgx_get_pixel(b, x, y);
    CHECK(differ == 0);
    dgx_screen_destroy(&a), dgx_screen_destroy(&b);
}

int main(void)
{
    test_user_data();
    dgx_screen_t *scr = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    scr->update_screen = count_update;
    dgx_gauge_t gauge;

    /* the ring is drawn by the constructor, in the background color, as one update */
    dgx_gauge_init_with(&gauge, scr, W / 2, H / 2, 30, 10, 225.0f, 270, 0, 100, BACK, gauge_color, NULL);
    const int ring = pixels_of(scr, BACK);
    CHECK(updates == 1);
    CHECK(ring > 1000); /* a ring of 30 to 40 pixels over three quarters of a circle is about 1650 pixels */
    CHECK(pixels_of(scr, LOW) == 0 && pixels_of(scr, HIGH) == 0);
    /* it lies where a ring lies: nothing in the middle, nothing in the quarter the sweep leaves out, at the bottom */
    CHECK(dgx_get_pixel(scr, W / 2, H / 2) == 0);
    CHECK(dgx_get_pixel(scr, W / 2, H / 2 + 35) == 0);
    CHECK(dgx_get_pixel(scr, W / 2, H / 2 - 35) == BACK);
    /* and it is as thick as asked: 10 pixels from the radius of 30 on, up from the middle */
    CHECK(dgx_get_pixel(scr, W / 2, H / 2 - 29) == 0 && dgx_get_pixel(scr, W / 2, H / 2 - 30) == BACK);
    CHECK(dgx_get_pixel(scr, W / 2, H / 2 - 39) == BACK && dgx_get_pixel(scr, W / 2, H / 2 - 40) == 0);

    /* a value lights its steps, the rest of the ring stays; one update for all the steps that changed */
    updates = 0;
    dgx_gauge_set_value(&gauge, 50);
    CHECK(updates == 1);
    const int half = pixels_of(scr, LOW);
    CHECK(half > ring / 3 && half < ring * 2 / 3);
    CHECK(pixels_of(scr, BACK) + half == ring);
    CHECK(pixels_of(scr, HIGH) == 0);

    updates = 0;
    dgx_gauge_set_value(&gauge, 100);
    CHECK(updates == 1);
    CHECK(pixels_of(scr, HIGH) > 0 && pixels_of(scr, BACK) == 0);
    CHECK(pixels_of(scr, LOW) + pixels_of(scr, HIGH) == ring);

    /* back to nothing: the ring is as the constructor left it */
    dgx_gauge_set_value(&gauge, 0);
    CHECK(pixels_of(scr, BACK) == ring && pixels_of(scr, LOW) == 0 && pixels_of(scr, HIGH) == 0);

    /* the same value again draws nothing */
    updates = 0;
    dgx_gauge_set_value(&gauge, 0);
    CHECK(updates == 0);

    /* a redraw on a cleared screen gives the same ring */
    dgx_gauge_set_value(&gauge, 50);
    dgx_fill_rectangle(scr, 0, 0, W, H, 0);
    updates = 0;
    dgx_gauge_redraw(&gauge);
    CHECK(updates == 1);
    /* a pixel is of one step, so the boundary is where it was */
    CHECK(pixels_of(scr, LOW) == half);
    CHECK(pixels_of(scr, BACK) + half == ring);

    /* a range of millions, as of the bytes of PSRAM: a value times the degrees of the sweep is more than an int holds */
    dgx_fill_rectangle(scr, 0, 0, W, H, 0);
    dgx_gauge_init_with(&gauge, scr, W / 2, H / 2, 30, 10, 225.0f, 270, 0, 8388608, BACK, gauge_color, NULL);
    CHECK(pixels_of(scr, BACK) == ring);
    dgx_gauge_set_value(&gauge, 8000000);
    CHECK(gauge.active_steps == 257);
    CHECK(pixels_of(scr, HIGH) > ring * 9 / 10 && pixels_of(scr, BACK) > 0);
    dgx_gauge_set_value(&gauge, 8388608);
    CHECK(gauge.active_steps == 270 && pixels_of(scr, HIGH) == ring && pixels_of(scr, BACK) == 0);

    /* the steps go with the clock, and against it for a negative sweep: from the top over the right side, or over the left one */
    for (int sweep = -180; sweep <= 180; sweep += 360) {
        dgx_fill_rectangle(scr, 0, 0, W, H, 0);
        dgx_gauge_init_with(&gauge, scr, W / 2, H / 2, 30, 10, 90.0f, sweep, 0, 100, BACK, gauge_color, NULL);
        const int empty = pixels_of(scr, BACK);
        CHECK(empty > 900);
        dgx_gauge_set_value(&gauge, 100);
        CHECK(gauge.active_steps == 180 && pixels_of(scr, BACK) == 0 && pixels_of(scr, LOW) + pixels_of(scr, HIGH) == empty);
        int left = 0, right = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                if (!dgx_get_pixel(scr, x, y)) continue;
                left += x < W / 2 - 1;
                right += x > W / 2 + 1;
            }
        }
        CHECK(sweep < 0 ? left > 900 && right == 0 : right > 900 && left == 0);
    }

    /* each pixel of the ring once in a redraw, steps of every color in turn, rings thin and thick, a sweep either way and a whole circle */
    static const int rings[][3] = {{30, 10, 270}, {20, 4, -270}, {10, 14, 360}, {44, 1, 90}, {0, 6, 180}};
    plain_fill = scr->fill_rectangle;
    for (unsigned r = 0; r < sizeof(rings) / sizeof(rings[0]); ++r) {
        const int inner = rings[r][0], width = rings[r][1], sweep = rings[r][2], steps = sweep < 0 ? -sweep : sweep;
        dgx_screen_t *again = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
        dgx_gauge_t   other;
        dgx_fill_rectangle(scr, 0, 0, W, H, 0);
        dgx_gauge_init_with(&gauge, scr, W / 2, H / 2, inner, width, 225.0f, sweep, 0, steps, BACK, value_color, NULL);
        dgx_gauge_set_value(&gauge, steps);
        memset(writes, 0, sizeof(writes));
        scr->fill_rectangle = counting_fill;
        dgx_gauge_redraw(&gauge);
        scr->fill_rectangle = plain_fill;
        int twice = 0, once = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) twice += writes[y][x] > 1, once += writes[y][x] == 1;
        CHECK(twice == 0);
        CHECK(once > 0 && pixels_of(scr, BACK) == 0);
        /* and every value comes out the same stepped up to it, stepped down to it and drawn at once */
        int differ = 0;
        for (int value = 0; value <= steps; value += 7) {
            dgx_gauge_set_value(&gauge, 0);
            for (int v = 0; v <= value; ++v) dgx_gauge_set_value(&gauge, v);
            dgx_fill_rectangle(again, 0, 0, W, H, 0);
            dgx_gauge_init_with(&other, again, W / 2, H / 2, inner, width, 225.0f, sweep, 0, steps, BACK, value_color, NULL);
            dgx_gauge_set_value(&other, value);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) differ += dgx_get_pixel(scr, x, y) != dgx_get_pixel(again, x, y);
            for (int v = steps; v >= value; --v) dgx_gauge_set_value(&gauge, v);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) differ += dgx_get_pixel(scr, x, y) != dgx_get_pixel(again, x, y);
        }
        CHECK(differ == 0);
        dgx_screen_destroy(&again);
    }

    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
