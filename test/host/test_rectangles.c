#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "drivers/vscreen.h"

/*
 * The outline of a rectangle and the rectangles with round corners, checked
 * pixel by pixel against a model made without the library: the corners are
 * the quarters of the midpoint circle of the textbooks put at four centres,
 * the sides are rows and columns between them, and a solid one is four discs
 * and the two rectangles that join them. And against waste, as the circles
 * are: a pixel is not sent twice but where the eighths of a corner meet, a
 * solid one goes by rows.
 */

#define W 400
#define H 400

static uint8_t canvas[H][W], model[H][W];
static int     fills, tall_fills, empty_fills, pixels_set;
static long    written;

static void put_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    (void)scr;
    ++fills;
    if (h > w) ++tall_fills;
    if (w <= 0 || h <= 0) ++empty_fills;
    for (int j = y; j < y + h; ++j) {
        if (j < 0 || j >= H) continue;
        for (int i = x; i < x + w; ++i) {
            if (i >= 0 && i < W) canvas[j][i] = (uint8_t)color, ++written;
        }
    }
}

static void put_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr;
    ++pixels_set;
    if (x >= 0 && x < W && y >= 0 && y < H) canvas[y][x] = (uint8_t)color, ++written;
}

static void dot(int x, int y)
{
    if (x >= 0 && x < W && y >= 0 && y < H) model[y][x] = 7;
}

static void block(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) dot(x, y);
    }
}

/* the points of the midpoint circle of radius r about (0, 0), each given to put() */
static void midpoint_points(int r, void (*put)(int dx, int dy, void *ctx), void *ctx)
{
    int f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    put(0, r, ctx), put(0, -r, ctx), put(r, 0, ctx), put(-r, 0, ctx);
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        put(x, y, ctx), put(-x, y, ctx), put(x, -y, ctx), put(-x, -y, ctx);
        put(y, x, ctx), put(-y, x, ctx), put(y, -x, ctx), put(-y, -x, ctx);
    }
}

typedef struct {
    int left, top, right, bottom;
} corners_t;

/* a point of the circle goes to the corner of its quarter */
static void corner_dot(int dx, int dy, void *ctx)
{
    const corners_t *c = ctx;
    dot(dx < 0 ? c->left + dx : c->right + dx, dy < 0 ? c->top + dy : c->bottom + dy);
}

/* a point of the circle and the row between it and its mirror across the corners */
static void corner_row(int dx, int dy, void *ctx)
{
    const corners_t *c = ctx;
    if (dx < 0) dx = -dx;
    const int y = dy < 0 ? c->top + dy : c->bottom + dy;
    for (int x = c->left - dx; x <= c->right + dx; ++x) dot(x, y);
}

static int clamp_radius(int w, int h, int r)
{
    const int most = ((w < h ? w : h) - 1) / 2;
    return r < 0 ? 0 : r > most ? most : r;
}

static void model_outline(int x, int y, int w, int h, int r)
{
    r = clamp_radius(w, h, r);
    corners_t c = {x + r, y + r, x + w - 1 - r, y + h - 1 - r};
    midpoint_points(r, corner_dot, &c);
    block(c.left, y, c.right, y), block(c.left, y + h - 1, c.right, y + h - 1);
    block(x, c.top, x, c.bottom), block(x + w - 1, c.top, x + w - 1, c.bottom);
}

static void model_solid(int x, int y, int w, int h, int r)
{
    r = clamp_radius(w, h, r);
    corners_t c = {x + r, y + r, x + w - 1 - r, y + h - 1 - r};
    midpoint_points(r, corner_row, &c);
    block(x, c.top, x + w - 1, c.bottom);
}

static long count(uint8_t (*a)[W])
{
    long n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) n += a[y][x] != 0;
    }
    return n;
}

static void start(void)
{
    memset(canvas, 0, sizeof(canvas));
    memset(model, 0, sizeof(model));
    fills = tall_fills = empty_fills = pixels_set = 0;
    written = 0;
}

static void same(const char *what, int x, int y, int w, int h, int r)
{
    if (memcmp(canvas, model, sizeof(canvas)) != 0) {
        fprintf(stderr, "%s (%d, %d, %d, %d, r %d) differs\n", what, x, y, w, h, r);
        ++check_failures;
    }
    CHECK(empty_fills == 0);
}

static void figures(dgx_screen_t *scr, int x, int y, int w, int h, int r, bool whole)
{
    const int rr = clamp_radius(w, h, r);

    start();
    model_outline(x, y, w, h, 0);
    dgx_draw_rectangle(scr, x, y, w, h, 7);
    same("rectangle", x, y, w, h, 0);
    if (whole) CHECK(written == count(model));

    start();
    model_outline(x, y, w, h, r);
    dgx_draw_round_rectangle(scr, x, y, w, h, r, 7);
    same("round rectangle", x, y, w, h, r);
    /* every pixel once, but for the eight where the eighths of the corners meet */
    if (whole) CHECK(written <= count(model) + 8);

    start();
    model_solid(x, y, w, h, r);
    dgx_fill_round_rectangle(scr, x, y, w, h, r, 7);
    same("solid round rectangle", x, y, w, h, r);
    /* by rows, but for the one piece between the corners; no more pieces than rows of the corners; every pixel once */
    CHECK(tall_fills <= 1);
    CHECK(fills + pixels_set <= 2 * rr + 3);
    if (whole) CHECK(written == count(model));

    /* a negative width or height is the rectangle on the other side */
    start();
    dgx_draw_round_rectangle(scr, x + w, y, -w, h, r, 7);
    model_outline(x, y, w, h, r);
    same("round rectangle to the left", x + w, y, -w, h, r);
    start();
    dgx_fill_round_rectangle(scr, x, y + h, w, -h, r, 7);
    model_solid(x, y, w, h, r);
    same("solid round rectangle upwards", x, y + h, w, -h, r);
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

    for (int w = 1; w <= 41; w += 2) {
        for (int h = 1; h <= 37; h += 3) {
            for (int r = -1; r <= 22; ++r) figures(scr, 100, 120, w, h, r, true);
        }
    }
    for (int w = 2; w <= 40; w += 6) {
        for (int h = 2; h <= 40; h += 5) {
            for (int r = 0; r <= 21; r += 3) figures(scr, 31, 17, w, h, r, true);
        }
    }
    figures(scr, 10, 20, 320, 240, 40, true);
    figures(scr, 5, 5, 390, 380, 1000, true);

    /* a square of 2r + 1 is the circle; a radius of 0 is the rectangle */
    for (int r = 0; r <= 60; ++r) {
        static uint8_t circle[H][W];
        for (int solid = 0; solid < 2; ++solid) {
            start();
            if (solid) dgx_solid_circle(scr, 200, 190, r, 7);
            else dgx_draw_circle(scr, 200, 190, r, 7);
            memcpy(circle, canvas, sizeof(canvas));
            start();
            if (solid) dgx_fill_round_rectangle(scr, 200 - r, 190 - r, 2 * r + 1, 2 * r + 1, r, 7);
            else dgx_draw_round_rectangle(scr, 200 - r, 190 - r, 2 * r + 1, 2 * r + 1, r, 7);
            CHECK(memcmp(circle, canvas, sizeof(canvas)) == 0);
        }
    }
    start();
    dgx_fill_rectangle(scr, 33, 44, 55, 66, 7);
    memcpy(model, canvas, sizeof(canvas));
    memset(canvas, 0, sizeof(canvas));
    dgx_fill_round_rectangle(scr, 33, 44, 55, 66, 0, 7);
    CHECK(memcmp(canvas, model, sizeof(canvas)) == 0);

    /* nothing at all when the width or the height is 0 */
    start();
    dgx_draw_rectangle(scr, 10, 10, 0, 5, 7);
    dgx_draw_round_rectangle(scr, 10, 10, 5, 0, 2, 7);
    dgx_fill_round_rectangle(scr, 10, 10, 0, 0, 2, 7);
    CHECK(fills == 0 && pixels_set == 0);

    /* cut by the edges of the screen, and off it */
    for (int r = 0; r <= 30; r += 5) {
        figures(scr, -20, 50, 70, 60, r, false);
        figures(scr, W - 30, H - 25, 80, 70, r, false);
        figures(scr, 100, -40, 60, 50, r, false);
        figures(scr, -100, -100, 50, 50, r, false);
        figures(scr, W + 5, 10, 50, 50, r, false);
    }

    /* a color virtual screen draws the same figures with its own pixels and rectangles */
    dgx_screen_t *v = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    for (int r = 0; r <= 40; r += 3) {
        for (int kind = 0; kind < 3; ++kind) {
            memset(model, 0, sizeof(model));
            dgx_fill_rectangle(v, 0, 0, W, H, 0);
            if (kind == 0) model_outline(17, 23, 150, 90, 0), dgx_draw_rectangle(v, 17, 23, 150, 90, 7);
            else if (kind == 1) model_outline(17, 23, 150, 90, r), dgx_draw_round_rectangle(v, 17, 23, 150, 90, r, 7);
            else model_solid(17, 23, 150, 90, r), dgx_fill_round_rectangle(v, 17, 23, 150, 90, r, 7);
            int differ = 0;
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) differ += model[y][x] != (uint8_t)dgx_get_pixel(v, x, y);
            }
            CHECK(differ == 0);
        }
    }
    dgx_screen_destroy(&v);
    CHECK_DONE();
}
