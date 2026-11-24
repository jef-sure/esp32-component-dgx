/*
 * The pictures of the ST7796S demo.
 *
 * The orientation picture is a check by eye. The screen draws its own axes,
 * a square at (0, 0) with arrows along x and y, and a large F. These come
 * out turned or mirrored as the screen is turned. The labels are drawn by
 * their place on the upright panel instead: "x" and "y" past the arrow heads,
 * the test number, the name of the orientation and a small F showing how the
 * large one should look. When the driver turns the screen right, every label
 * reads normally and the large F looks like the small one.
 */
#include "scenes.h"

#include <stdio.h>
#include <stdlib.h>

#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/GLCD5x7.h"

const demo_way_t demo_ways[DEMO_WAYS] = {
    {DgxScreenLeftRight, DgxScreenTopBottom, false, "LT",  1,  0,  0,  1 },
    {DgxScreenRightLeft, DgxScreenTopBottom, false, "RT",  -1, 0,  0,  1 },
    {DgxScreenLeftRight, DgxScreenBottomTop, false, "LB",  1,  0,  0,  -1},
    {DgxScreenRightLeft, DgxScreenBottomTop, false, "RB",  -1, 0,  0,  -1},
    {DgxScreenLeftRight, DgxScreenTopBottom, true,  "LTs", 0,  1,  1,  0 },
    {DgxScreenRightLeft, DgxScreenTopBottom, true,  "RTs", 0,  1,  -1, 0 },
    {DgxScreenLeftRight, DgxScreenBottomTop, true,  "LBs", 0,  -1, 1,  0 },
    {DgxScreenRightLeft, DgxScreenBottomTop, true,  "RBs", 0,  -1, -1, 0 },
};

/* text in the 5x7 font, its top left corner at x, y */
static void text(dgx_screen_t *scr, int x, int y, int scale, uint32_t color, const char *str)
{
    const dgx_font_context_t ctx = {.scr = scr, .font = &GLCD5x7, .color = color, .scale = scale};
    dgx_font_draw(&ctx, x, y + 7 * scale, str);
}

void demo_colors(dgx_screen_t *scr, demo_rgb_t rgb, int bits)
{
    const int      w = scr->width;
    const uint32_t black = rgb(0, 0, 0), white = rgb(255, 255, 255);
    char           title[40];
    dgx_fill_rectangle(scr, 0, 0, w, scr->height, black);
    snprintf(title, sizeof(title), "TEST %d  COLORS %d-BIT", DEMO_TEST, bits);
    text(scr, 8, 12, 2, white, title);
    /* each bar is named after the color it should show */
    static const struct {
        const char *name;
        uint8_t     r, g, b;
        bool        dark;
    } bars[] = {
        {"RED",   255, 0,   0,   false},
        {"GREEN", 0,   255, 0,   false},
        {"BLUE",  0,   0,   255, true },
        {"WHITE", 255, 255, 255, false},
        {"BLACK", 0,   0,   0,   true },
    };
    for (int i = 0; i < 5; ++i) {
        const int y = 40 + i * 56;
        dgx_fill_rectangle(scr, 0, y, w, 52, rgb(bars[i].r, bars[i].g, bars[i].b));
        if (bars[i].r + bars[i].g + bars[i].b == 0) dgx_draw_rectangle(scr, 0, y, w, 52, white);
        text(scr, 16, y + 12, 4, bars[i].dark ? white : black, bars[i].name);
    }
    /* ramps from black to full red, green, blue and white: smooth at 18 bits, in fine steps at 16 */
    for (int r = 0; r < 4; ++r) {
        const int y = 330 + r * 36;
        for (int x = 0; x < w; ++x) {
            const uint8_t v = (uint8_t)(x * 255 / (w - 1));
            dgx_fill_rectangle(scr, x, y, 1, 32, rgb(r == 0 || r == 3 ? v : 0, r == 1 || r == 3 ? v : 0, r == 2 || r == 3 ? v : 0));
        }
    }
}

/* the point of the screen that should show at a point of the upright panel */
static void to_screen(const dgx_screen_t *scr, int px, int py, int *x, int *y)
{
    const int ax = scr->dir_x == DgxScreenRightLeft ? DEMO_PANEL_W - 1 - px : px;
    const int ay = scr->dir_y == DgxScreenBottomTop ? DEMO_PANEL_H - 1 - py : py;
    *x           = scr->swap_xy ? ay : ax;
    *y           = scr->swap_xy ? ax : ay;
}

/* where a point of the screen should show on the upright panel */
static void to_panel(const dgx_screen_t *scr, int x, int y, int *px, int *py)
{
    const int ax = scr->swap_xy ? y : x, ay = scr->swap_xy ? x : y;
    *px          = scr->dir_x == DgxScreenRightLeft ? DEMO_PANEL_W - 1 - ax : ax;
    *py          = scr->dir_y == DgxScreenBottomTop ? DEMO_PANEL_H - 1 - ay : ay;
}

/* a box given in panel coordinates */
static void panel_box(dgx_screen_t *scr, int px, int py, int w, int h, uint32_t color)
{
    int x1, y1, x2, y2;
    to_screen(scr, px, py, &x1, &y1);
    to_screen(scr, px + w - 1, py + h - 1, &x2, &y2);
    const int x = x1 < x2 ? x1 : x2, y = y1 < y2 ? y1 : y2;
    dgx_fill_rectangle(scr, x, y, abs(x2 - x1) + 1, abs(y2 - y1) + 1, color);
}

/* a bar of the small F, in the letter's own axes from its middle, put on the panel as demo_ways[] says it should come out */
static void expected_bar(dgx_screen_t *scr, int way, int cx, int cy, int u, int v, int w, int h, uint32_t color)
{
    const demo_way_t *d  = &demo_ways[way];
    const int         u2 = u + w - 1, v2 = v + h - 1;
    const int         ax = cx + u * d->x_right + v * d->y_right, ay = cy + u * d->x_down + v * d->y_down;
    const int         bx = cx + u2 * d->x_right + v2 * d->y_right, by = cy + u2 * d->x_down + v2 * d->y_down;
    panel_box(scr, ax < bx ? ax : bx, ay < by ? ay : by, abs(bx - ax) + 1, abs(by - ay) + 1, color);
}

/* text drawn on the 1-bit screen aside, then copied to the panel pixel by pixel so it reads normally in every orientation */
static void panel_text(dgx_screen_t *scr, dgx_screen_t *aside, int px, int py, int scale, uint32_t color, const char *str)
{
    dgx_fill_rectangle(aside, 0, 0, aside->width, aside->height, 0);
    text(aside, 0, 0, 1, 1, str);
    for (int j = 0; j < aside->height; ++j)
        for (int i = 0; i < aside->width; ++i)
            if (dgx_get_pixel(aside, i, j)) panel_box(scr, px + i * scale, py + j * scale, scale, scale, color);
}

void demo_orientation(dgx_screen_t *scr, dgx_screen_t *aside, demo_rgb_t rgb, int way)
{
    const int      w = scr->width, h = scr->height;
    const uint32_t black = rgb(0, 0, 0), white = rgb(255, 255, 255), red = rgb(255, 64, 64), green = rgb(64, 255, 64),
                   yellow = rgb(255, 255, 0);
    dgx_fill_rectangle(scr, 0, 0, w, h, black);
    dgx_draw_rectangle(scr, 0, 0, w, h, white);

    /* the axes, drawn by the screen: a square at (0, 0), a red arrow along x and a green one along y */
    const int tip = 160;
    dgx_fill_rectangle(scr, 8, 8, 16, 16, white);
    dgx_fill_rectangle(scr, 30, 14, tip - 40, 5, red);
    dgx_fill_triangle(scr, tip, 16, tip - 16, 6, tip - 16, 26, red);
    dgx_fill_rectangle(scr, 14, 30, 5, tip - 40, green);
    dgx_fill_triangle(scr, 16, tip, 6, tip - 16, 26, tip - 16, green);
    /* the large F, drawn by the screen */
    dgx_fill_rectangle(scr, 40, 40, 8, 64, yellow);
    dgx_fill_rectangle(scr, 40, 40, 48, 8, yellow);
    dgx_fill_rectangle(scr, 40, 68, 36, 8, yellow);

    /* the labels, by their place on the panel: x and y past the arrow heads */
    int px, py;
    to_panel(scr, tip + 16, 16, &px, &py);
    panel_text(scr, aside, px - 10, py - 14, 4, red, "x");
    to_panel(scr, 16, tip + 16, &px, &py);
    panel_text(scr, aside, px - 10, py - 14, 4, green, "y");

    /* the rest in the half of the panel the axes are not in */
    to_panel(scr, 0, 0, &px, &py);
    const int         top = py < DEMO_PANEL_H / 2 ? DEMO_PANEL_H / 2 + 8 : 16;
    const demo_way_t *d   = &demo_ways[way];
    char              line[40];
    snprintf(line, sizeof(line), "TEST %d", DEMO_TEST);
    panel_text(scr, aside, 16, top, 3, white, line);
    snprintf(line, sizeof(line), "%s %dx%d", d->name, w, h);
    panel_text(scr, aside, 16, top + 30, 3, white, line);
    snprintf(line, sizeof(line), "%s %s%s", d->dir_x == DgxScreenLeftRight ? "LeftRight" : "RightLeft",
             d->dir_y == DgxScreenTopBottom ? "TopBottom" : "BottomTop", d->swap_xy ? " swap" : "");
    panel_text(scr, aside, 16, top + 60, 2, white, line);
    if (way == 0) panel_text(scr, aside, 16, top + 80, 2, white, "= DGX_ST7796S_320X480");
    if (way == 5) panel_text(scr, aside, 16, top + 80, 2, white, "= DGX_ST7796S_480X320");
    /* the small F: how the large one should look */
    const int cx = DEMO_PANEL_W / 2, cy = top + 160;
    expected_bar(scr, way, cx, cy, -24, -32, 8, 64, yellow);
    expected_bar(scr, way, cx, cy, -24, -32, 48, 8, yellow);
    expected_bar(scr, way, cx, cy, -24, -4, 36, 8, yellow);
}

void demo_speed(dgx_screen_t *scr, demo_rgb_t rgb, int us_per_fill, int spi_mhz)
{
    const uint32_t black = rgb(0, 0, 0), white = rgb(255, 255, 255);
    char           line[40];
    dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, black);
    snprintf(line, sizeof(line), "TEST %d  SPEED", DEMO_TEST);
    text(scr, 8, 12, 2, white, line);
    text(scr, 16, 80, 3, white, "FULL SCREEN FILL");
    snprintf(line, sizeof(line), "%d.%d MS", us_per_fill / 1000, us_per_fill / 100 % 10);
    text(scr, 16, 120, 5, white, line);
    snprintf(line, sizeof(line), "%dx%d, SPI %d MHZ", scr->width, scr->height, spi_mhz);
    text(scr, 16, 190, 2, white, line);
}
