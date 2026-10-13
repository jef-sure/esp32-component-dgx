#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "drivers/vscreen.h"

/*
 * What a rectangle shows is moved within it: every pixel of the screen is
 * told beforehand, the rectangle and the move are drawn at random. On
 * screens of every depth, those moved in memory by rows and those moved
 * pixel by pixel, and on one that cannot be read.
 */

#define W 61
#define H 47

static uint32_t before[H][W];

static uint32_t rnd(void)
{
    static uint32_t seed = 2463534242u;
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return seed;
}

static int between(int from, int to)
{
    return from + (int)(rnd() % (uint32_t)(to - from + 1));
}

static void scrolls(dgx_screen_t *scr, const char *name)
{
    /* a color of 12 bits is the upper 12 of 16: the lower four are not a color and are not to be given */
    const uint32_t colors = scr->color_bits == 12 ? 0xfff0u : 0xffffffffu;
    CHECK(dgx_screen_is_readable(scr));
    int wrong = 0;
    for (int n = 0; n < 4000; ++n) {
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                dgx_set_pixel(scr, x, y, rnd() & colors);
                before[y][x] = dgx_get_pixel(scr, x, y);
            }
        }
        /* the color of the strip as the screen keeps it */
        const uint32_t was = before[0][0];
        dgx_set_pixel(scr, 0, 0, rnd() & colors);
        const uint32_t color = dgx_get_pixel(scr, 0, 0);
        dgx_set_pixel(scr, 0, 0, was);
        /* rectangles that are cut by the screen and off it among them, and moves larger than they are */
        const int x = between(-8, W - 1), y = between(-8, H - 1), w = between(-2, W + 8), h = between(-2, H + 8);
        const int dx = n % 7 == 0 ? 0 : between(-W - 2, W + 2) / (n % 3 + 1), dy = n % 5 == 0 ? 0 : between(-H - 2, H + 2) / (n % 4 + 1);
        /* a whole picture and a batch around it: one piece is shown, and it has all that has changed */
        int left, top, width, height;
        dgx_screen_progress_up(scr);
        CHECK(dgx_scroll(scr, x, y, w, h, dx, dy, color) == ESP_OK);
        const bool dirty = dgx_screen_take_dirty(scr, &left, &top, &width, &height);
        dgx_screen_progress_down(scr);
        /* the rectangle as the screen has it */
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w - 1, y1 = y + h - 1;
        if (x1 >= W) x1 = W - 1;
        if (y1 >= H) y1 = H - 1;
        for (int j = 0; j < H; ++j) {
            for (int i = 0; i < W; ++i) {
                uint32_t want = before[j][i];
                if (i >= x0 && i <= x1 && j >= y0 && j <= y1) {
                    const int si = i - dx, sj = j - dy;
                    want = si >= x0 && si <= x1 && sj >= y0 && sj <= y1 ? before[sj][si] : color;
                }
                const uint32_t got = dgx_get_pixel(scr, i, j);
                wrong += got != want;
                if (got != before[j][i]) CHECK(dirty && i >= left && i < left + width && j >= top && j < top + height);
            }
        }
    }
    if (wrong) fprintf(stderr, "%s: %d pixels wrong\n", name, wrong);
    CHECK(wrong == 0);
}

static int  fills, pixels_set;
static long filled;

static void put_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    (void)scr, (void)x, (void)y, (void)color;
    ++fills, filled += (long)w * h;
}

static void put_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr, (void)x, (void)y, (void)color;
    ++pixels_set;
}

int main(void)
{
    static const uint8_t depths[] = {1, 4, 8, 12, 16, 18, 24, 32};
    for (unsigned i = 0; i < sizeof(depths); ++i) {
        char          name[32];
        dgx_screen_t *v = dgx_vscreen_init(W, H, depths[i], DgxScreenRGB);
        snprintf(name, sizeof(name), "a virtual screen of %d bits", depths[i]);
        CHECK(v != NULL);
        if (v) scrolls(v, name);
        dgx_screen_destroy(&v);
    }
    /* a black-and-white screen kept by pages, as an SSD1306 has it */
    dgx_screen_t *bw = dgx_bw_init(W, H);
    scrolls(bw, "a black-and-white screen");
    dgx_screen_destroy(&bw);

    /* a display that keeps its pixels behind a bus: it is not read, and nothing is drawn but a move that only fills */
    static dgx_screen_t screen;
    dgx_screen_t       *scr = &screen;
    scr->width              = W;
    scr->height             = H;
    scr->color_bits         = 16;
    scr->screen_subtype     = DgxVirtualScreen; /* no bus behind it: only the drawing functions are taken */
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->fill_rectangle = put_rectangle;
    scr->set_pixel      = put_pixel;
    CHECK(!dgx_screen_is_readable(scr));
    CHECK(dgx_scroll(scr, 5, 5, 20, 10, 1, 0, 3) == ESP_ERR_NOT_SUPPORTED);
    CHECK(dgx_scroll(scr, 5, 5, 20, 10, -19, 9, 3) == ESP_ERR_NOT_SUPPORTED);
    CHECK(fills == 0 && pixels_set == 0);
    /* nothing to move */
    CHECK(dgx_scroll(scr, 5, 5, 20, 10, 0, 0, 3) == ESP_OK);
    CHECK(dgx_scroll(scr, 5, 5, 0, 10, 3, 3, 3) == ESP_OK);
    CHECK(dgx_scroll(scr, W, 5, 20, 10, 3, 3, 3) == ESP_OK);
    CHECK(fills == 0 && pixels_set == 0);
    /* the whole picture leaves: the rectangle is filled, as much of it as the screen has */
    CHECK(dgx_scroll(scr, 5, 5, 20, 10, 20, 0, 3) == ESP_OK);
    CHECK(fills == 1 && filled == 200);
    CHECK(dgx_scroll(scr, -5, 40, 20, 10, 3, -70, 3) == ESP_OK);
    CHECK(fills == 2 && filled == 200 + 15 * 7);
    CHECK_DONE();
}
