#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "drivers/vscreen.h"

/*
 * Lines and circles are the same code for every screen, made of the pixels
 * and the rectangles a screen draws itself. Here they are checked against
 * the algorithms as the textbooks give them, pixel by pixel: the midpoint
 * circle and Bresenham's line. And against waste: a pixel is not to be sent
 * twice, a solid circle goes by rows.
 */

#define W 700
#define H 700

static uint8_t canvas[H][W], model[H][W];
static int     fills, tall_fills, empty_fills, pixels_set;
static long    written;
/* the box of everything written to canvas or model since the last start(); both are zero outside it */
static int     box_x0 = W, box_y0 = H, box_x1 = -1, box_y1 = -1;

static void touch(int x, int y)
{
    if (x < box_x0) box_x0 = x;
    if (x > box_x1) box_x1 = x;
    if (y < box_y0) box_y0 = y;
    if (y > box_y1) box_y1 = y;
}

static void clear_box(uint8_t (*a)[W])
{
    for (int y = box_y0; y <= box_y1; ++y) memset(&a[y][box_x0], 0, box_x1 - box_x0 + 1);
}

static void put_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    (void)scr;
    ++fills;
    if (h > w) ++tall_fills;
    if (w <= 0 || h <= 0) ++empty_fills;
    for (int j = y; j < y + h; ++j) {
        if (j < 0 || j >= H) continue;
        for (int i = x; i < x + w; ++i) {
            if (i >= 0 && i < W) canvas[j][i] = (uint8_t)color, ++written, touch(i, j);
        }
    }
}

static void put_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr;
    ++pixels_set;
    if (x >= 0 && x < W && y >= 0 && y < H) canvas[y][x] = (uint8_t)color, ++written, touch(x, y);
}

static void dot(int x, int y)
{
    if (x >= 0 && x < W && y >= 0 && y < H) model[y][x] = 7, touch(x, y);
}

static void row(int x, int y, int w)
{
    for (int i = x; i < x + w; ++i) dot(i, y);
}

/* the midpoint circle: an eighth walked, every point put in eight places */
static void midpoint_circle(int x0, int y0, int r)
{
    int f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    dot(x0, y0 + r), dot(x0, y0 - r), dot(x0 + r, y0), dot(x0 - r, y0);
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        dot(x0 + x, y0 + y), dot(x0 - x, y0 + y), dot(x0 + x, y0 - y), dot(x0 - x, y0 - y);
        dot(x0 + y, y0 + x), dot(x0 - y, y0 + x), dot(x0 + y, y0 - x), dot(x0 - y, y0 - x);
    }
}

/* the same walk with the rows between the points filled */
static void midpoint_disc(int x0, int y0, int r)
{
    int f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    row(x0 - r, y0, 2 * r + 1);
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        row(x0 - x, y0 + y, 2 * x + 1), row(x0 - x, y0 - y, 2 * x + 1);
        row(x0 - y, y0 + x, 2 * y + 1), row(x0 - y, y0 - x, 2 * y + 1);
    }
}

/* Bresenham's line */
static void bresenham(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        dot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

static long count(uint8_t (*a)[W])
{
    long n = 0;
    for (int y = box_y0; y <= box_y1; ++y) {
        for (int x = box_x0; x <= box_x1; ++x) n += a[y][x] != 0;
    }
    return n;
}

static void start(void)
{
    clear_box(canvas);
    clear_box(model);
    box_x0 = W, box_y0 = H, box_x1 = -1, box_y1 = -1;
    fills = tall_fills = empty_fills = pixels_set = 0;
    written = 0;
}

static void same(const char *what, int x, int y, int r)
{
    bool differ = false;
    for (int y = box_y0; y <= box_y1 && !differ; ++y) {
        differ = memcmp(&canvas[y][box_x0], &model[y][box_x0], box_x1 - box_x0 + 1) != 0;
    }
    if (differ) {
        fprintf(stderr, "%s at (%d, %d), %d differs\n", what, x, y, r);
        ++check_failures;
    }
    CHECK(empty_fills == 0);
}

static void circles(dgx_screen_t *scr, int x, int y, int r, bool whole)
{
    start();
    midpoint_disc(x, y, r);
    dgx_solid_circle(scr, x, y, r, 7);
    same("solid circle", x, y, r);
    /* by rows, no more pieces than rows, and only the rows at the diagonal sent twice */
    CHECK(tall_fills == 0);
    CHECK(fills + pixels_set <= 2 * r + 3);
    if (whole) CHECK(written <= count(model) + 8 * (r + 1));

    start();
    midpoint_circle(x, y, r);
    dgx_draw_circle(scr, x, y, r, 7);
    same("circle", x, y, r);
    /* every pixel once, but for the eight where the eighths meet */
    if (whole) CHECK(written <= count(model) + 8);
}

static void line(dgx_screen_t *scr, int x1, int y1, int x2, int y2)
{
    start();
    /* the textbook line, walked from the upper end as the library walks it */
    if (y1 < y2 || (y1 == y2 && x1 <= x2)) bresenham(x1, y1, x2, y2);
    else bresenham(x2, y2, x1, y1);
    dgx_draw_line(scr, x1, y1, x2, y2, 7);
    same("line to", x2, y2, x1 * 1000 + y1);
    CHECK(written == count(model));
    /* from the other end it is the same line */
    clear_box(canvas);
    dgx_draw_line(scr, x2, y2, x1, y1, 7);
    same("line back to", x1, y1, x2 * 1000 + y2);
}

int main(void)
{
    /* a screen that only counts what it is asked for */
    static dgx_screen_t screen;
    dgx_screen_t       *scr = &screen;
    scr->width              = W;
    scr->height             = H;
    scr->color_bits         = 8;
    scr->screen_subtype     = DgxVirtualScreen; /* no bus behind it: only the drawing functions are taken */
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->fill_rectangle = put_rectangle;
    scr->set_pixel      = put_pixel;

    for (int r = 0; r <= 340; ++r) circles(scr, W / 2, H / 2, r, true);
    /* cut by the edges of the screen, and off it */
    for (int r = 1; r <= 60; r += 7) {
        circles(scr, 3, 5, r, false);
        circles(scr, W - 2, H - 4, r, false);
        circles(scr, -r / 2, H / 2, r, false);
        circles(scr, W / 2, H + r / 3, r, false);
        circles(scr, -r - 5, 10, r, false);
    }
    for (int x1 = 0; x1 < 96; x1 += 5) {
        for (int y1 = 0; y1 < 96; y1 += 7) {
            for (int x2 = 0; x2 < 96; x2 += 3) {
                for (int y2 = 0; y2 < 96; y2 += 4) line(scr, x1, y1, x2, y2);
            }
        }
    }
    line(scr, -40, 20, 300, 650);
    line(scr, 650, -30, 10, 720);

    /* a color virtual screen draws the same figures with its own pixels and rectangles */
    dgx_screen_t *v = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    for (int r = 0; r <= 120; r += 1) {
        for (int solid = 0; solid < 2; ++solid) {
            memset(model, 0, sizeof(model));
            dgx_fill_rectangle(v, 0, 0, W, H, 0);
            if (solid) midpoint_disc(300, 310, r), dgx_solid_circle(v, 300, 310, r, 7);
            else midpoint_circle(300, 310, r), dgx_draw_circle(v, 300, 310, r, 7);
            int differ = 0;
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) differ += model[y][x] != (uint8_t)dgx_get_pixel(v, x, y);
            }
            CHECK(differ == 0);
        }
    }
    memset(model, 0, sizeof(model));
    dgx_fill_rectangle(v, 0, 0, W, H, 0);
    bresenham(13, 7, 600, 211), dgx_draw_line(v, 600, 211, 13, 7, 7);
    int differ = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) differ += model[y][x] != (uint8_t)dgx_get_pixel(v, x, y);
    }
    CHECK(differ == 0);
    dgx_screen_destroy(&v);
    CHECK_DONE();
}
