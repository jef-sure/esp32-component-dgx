/* The arc gauge: its empty ring is there after dgx_gauge_init(), and a change of the value is one update of the screen. */
#include <stdint.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_gauge.h"
#include "drivers/vscreen.h"

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

static uint32_t gauge_color(int value)
{
    return value < 70 ? LOW : HIGH;
}

static int pixels_of(dgx_screen_t *scr, uint32_t color)
{
    int n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) n += dgx_get_pixel(scr, x, y) == color;
    }
    return n;
}

int main(void)
{
    dgx_screen_t *scr = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    scr->update_screen = count_update;
    dgx_gauge_t gauge;

    /* the ring is drawn by the constructor, in the background color, as one update */
    dgx_gauge_init(&gauge, scr, W / 2, H / 2, 30, 10, 225.0f, 270, 0, 100, BACK, gauge_color);
    const int ring = pixels_of(scr, BACK);
    CHECK(updates == 1);
    CHECK(ring > 1000); /* a ring of 30 to 40 pixels over three quarters of a circle is about 1650 pixels */
    CHECK(pixels_of(scr, LOW) == 0 && pixels_of(scr, HIGH) == 0);
    /* it lies where a ring lies: nothing in the middle, nothing in the quarter the sweep leaves out, at the bottom */
    CHECK(dgx_get_pixel(scr, W / 2, H / 2) == 0);
    CHECK(dgx_get_pixel(scr, W / 2, H / 2 + 35) == 0);
    CHECK(dgx_get_pixel(scr, W / 2, H / 2 - 35) == BACK);

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
    /* neighbouring steps share the pixels of their common edge, and the one drawn later has them: the boundary may move by that edge */
    const int again = pixels_of(scr, LOW);
    CHECK(again > half - 15 && again < half + 15);
    CHECK(pixels_of(scr, BACK) + again == ring);

    /* a range of millions, as of the bytes of PSRAM: a value times the degrees of the sweep is more than an int holds */
    dgx_fill_rectangle(scr, 0, 0, W, H, 0);
    dgx_gauge_init(&gauge, scr, W / 2, H / 2, 30, 10, 225.0f, 270, 0, 8388608, BACK, gauge_color);
    CHECK(pixels_of(scr, BACK) == ring);
    dgx_gauge_set_value(&gauge, 8000000);
    CHECK(gauge.active_steps == 257);
    CHECK(pixels_of(scr, HIGH) > ring * 9 / 10 && pixels_of(scr, BACK) > 0);
    dgx_gauge_set_value(&gauge, 8388608);
    CHECK(gauge.active_steps == 270 && pixels_of(scr, HIGH) == ring && pixels_of(scr, BACK) == 0);

    /* the steps go with the clock, and against it for a negative sweep: from the top over the right side, or over the left one */
    for (int sweep = -180; sweep <= 180; sweep += 360) {
        dgx_fill_rectangle(scr, 0, 0, W, H, 0);
        dgx_gauge_init(&gauge, scr, W / 2, H / 2, 30, 10, 90.0f, sweep, 0, 100, BACK, gauge_color);
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

    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
