/*
 * The pictures of the demo, apart from the board so that they can be drawn
 * on the computer through the driver too.
 *
 * The eight ways the screen can lie on the 128x296 glass, as the ST7920 and
 * the OLEDs were checked: the axes of the screen and a large F drawn by the
 * screen, so they come out turned or mirrored as the screen lies; the
 * letters x and y past the heads of the arrows, the build, the way and a
 * small F as it is expected to look, drawn by their place on the GLASS, in
 * the half the axes are not in. The labels read the usual way in every way
 * and the large F looks like the small one when the driver turns the screen
 * as it is expected to. The axes and the labels are black, both letters F
 * red.
 *
 * Then a picture in landscape, the way the module lies with its pins on the
 * right.
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>

#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/ssd1680.h"
#include "fonts/DejaVuSansBook32.h"
#include "fonts/GLCD5x7.h"
#include "fonts/TerminusTTFMedium12.h"

#define BUILD   786
#define PANEL_W 128
#define PANEL_H 296

#define INK   DGX_BWR_BLACK
#define PAPER DGX_BWR_WHITE
#define RED   DGX_BWR_RED

/* where the axes of the screen are expected to point on the glass, written out by hand: a step along x and along y as steps on the glass (right, down) */
static const struct {
    dgx_orientation_t dir_x, dir_y;
    bool              swap_xy;
    const char       *name;
    int8_t            x_right, x_down, y_right, y_down;
} ways[] = {
    {DgxScreenLeftRight, DgxScreenTopBottom, false, "LT", 1, 0, 0, 1},
    {DgxScreenRightLeft, DgxScreenTopBottom, false, "RT", -1, 0, 0, 1},
    {DgxScreenLeftRight, DgxScreenBottomTop, false, "LB", 1, 0, 0, -1},
    {DgxScreenRightLeft, DgxScreenBottomTop, false, "RB", -1, 0, 0, -1},
    {DgxScreenLeftRight, DgxScreenTopBottom, true, "LTs", 0, 1, 1, 0},
    {DgxScreenRightLeft, DgxScreenTopBottom, true, "RTs", 0, 1, -1, 0},
    {DgxScreenLeftRight, DgxScreenBottomTop, true, "LBs", 0, -1, 1, 0},
    {DgxScreenRightLeft, DgxScreenBottomTop, true, "RBs", 0, -1, -1, 0},
};
#define WAYS ((int)(sizeof(ways) / sizeof(ways[0])))

/* the way of the module in landscape with its pins on the right, as on its photo: seen so on the glass */
#define LANDSCAPE 6

/* the point of the screen the driver is to show at a point of the glass */
static void to_screen(const dgx_screen_t *scr, int px, int py, int *x, int *y)
{
    const int ax = scr->dir_x == DgxScreenRightLeft ? PANEL_W - 1 - px : px;
    const int ay = scr->dir_y == DgxScreenBottomTop ? PANEL_H - 1 - py : py;
    *x           = scr->swap_xy ? ay : ax;
    *y           = scr->swap_xy ? ax : ay;
}

/* where a point of the screen is expected on the glass */
static void to_panel(const dgx_screen_t *scr, int x, int y, int *px, int *py)
{
    const int ax = scr->swap_xy ? y : x, ay = scr->swap_xy ? x : y;
    *px          = scr->dir_x == DgxScreenRightLeft ? PANEL_W - 1 - ax : ax;
    *py          = scr->dir_y == DgxScreenBottomTop ? PANEL_H - 1 - ay : ay;
}

static void panel_box(dgx_screen_t *scr, int px, int py, int w, int h, uint32_t color)
{
    int x1, y1, x2, y2;
    to_screen(scr, px, py, &x1, &y1);
    to_screen(scr, px + w - 1, py + h - 1, &x2, &y2);
    const int x = x1 < x2 ? x1 : x2, y = y1 < y2 ? y1 : y2;
    dgx_fill_rectangle(scr, x, y, abs(x2 - x1) + 1, abs(y2 - y1) + 1, color);
}

/* a bar of the small F, in the letter's own axes from its middle, put on the glass as the table says */
static void expected_bar(dgx_screen_t *scr, int way, int cx, int cy, int u, int v, int w, int h)
{
    const int u2 = u + w - 1, v2 = v + h - 1;
    const int ax = cx + u * ways[way].x_right + v * ways[way].y_right, ay = cy + u * ways[way].x_down + v * ways[way].y_down;
    const int bx = cx + u2 * ways[way].x_right + v2 * ways[way].y_right, by = cy + u2 * ways[way].x_down + v2 * ways[way].y_down;
    panel_box(scr, ax < bx ? ax : bx, ay < by ? ay : by, abs(bx - ax) + 1, abs(by - ay) + 1, RED);
}

/* a text drawn aside and moved to the glass pixel by pixel, twice as large, to read the usual way in every way; its cell at px, py */
static void panel_text(dgx_screen_t *scr, dgx_screen_t *aside, int px, int py, const char *text)
{
    dgx_fill_rectangle(aside, 0, 0, aside->width, aside->height, 0);
    dgx_font_string_utf8_screen(aside, 0, 7, text, 1, DgxOutputNormal, 1, GLCD5x7(), NULL, NULL);
    for (int j = 0; j < aside->height; ++j) {
        for (int i = 0; i < aside->width; ++i) {
            if (dgx_get_pixel(aside, i, j)) panel_box(scr, px + 2 * i, py + 2 * j, 2, 2, INK);
        }
    }
}

/* the F: a stem and two arms, 26 by 40, from its middle */
static const int8_t f_bars[3][4] = {
    {-13, -20, 4, 40},
    {-13, -20, 26, 4},
    {-13, -4, 18, 4},
};

/* the picture of one way */
static void picture_way(dgx_screen_t *scr, dgx_screen_t *aside, int way)
{
    const int w = scr->width, h = scr->height;
    dgx_fill_rectangle(scr, 0, 0, w, h, PAPER);
    dgx_draw_line(scr, 0, 0, w - 1, 0, INK);
    dgx_draw_line(scr, 0, h - 1, w - 1, h - 1, INK);
    dgx_draw_line(scr, 0, 0, 0, h - 1, INK);
    dgx_draw_line(scr, w - 1, 0, w - 1, h - 1, INK);

    /* the axes, by the screen: a square at (0, 0), an arrow along x and an arrow along y */
    const int tip = 70;
    dgx_fill_rectangle(scr, 3, 3, 8, 8, INK);
    dgx_fill_rectangle(scr, 13, 6, tip - 13, 2, INK);
    dgx_fill_rectangle(scr, 6, 13, 2, tip - 13, INK);
    for (int k = 0; k < 2; ++k) {
        dgx_draw_line(scr, tip, 6 + k, tip - 6, 0 + k, INK);
        dgx_draw_line(scr, tip, 6 + k, tip - 6, 12 + k, INK);
        dgx_draw_line(scr, 6 + k, tip, 0 + k, tip - 6, INK);
        dgx_draw_line(scr, 6 + k, tip, 12 + k, tip - 6, INK);
    }
    /* the letter, by the screen, its middle at (36, 42) */
    for (int b = 0; b < 3; ++b) dgx_fill_rectangle(scr, 36 + f_bars[b][0], 42 + f_bars[b][1], f_bars[b][2], f_bars[b][3], RED);

    /* what is expected, by the glass: the names of the axes past their heads, and in the half the axes are not in the build, the way and the small F */
    int px, py;
    to_panel(scr, tip + 9, 7, &px, &py);
    panel_text(scr, aside, px - 5, py - 7, "x");
    to_panel(scr, 7, tip + 9, &px, &py);
    panel_text(scr, aside, px - 5, py - 7, "y");
    to_panel(scr, 0, 0, &px, &py);
    const int top = py < PANEL_H / 2 ? 190 : 30;
    char      text[16];
    snprintf(text, sizeof(text), "b%d", BUILD);
    panel_text(scr, aside, 12, top, text);
    panel_text(scr, aside, 12, top + 22, ways[way].name);
    for (int b = 0; b < 3; ++b) expected_bar(scr, way, 92, top + 30, f_bars[b][0], f_bars[b][1], f_bars[b][2], f_bars[b][3]);
}

/* the picture in landscape: text in two fonts, the three colors, figures */
static void picture_landscape(dgx_screen_t *scr)
{
    const int w = scr->width, h = scr->height;
    char      text[32];
    dgx_fill_rectangle(scr, 0, 0, w, h, PAPER);
    dgx_draw_line(scr, 0, 0, w - 1, 0, INK);
    dgx_draw_line(scr, 0, h - 1, w - 1, h - 1, INK);
    dgx_draw_line(scr, 0, 0, 0, h - 1, INK);
    dgx_draw_line(scr, w - 1, 0, w - 1, h - 1, INK);
    /* y of a text is its base line */
    const dgx_font_context_t big   = {.scr = scr, .font = DejaVuSansBook32(), .color = INK};
    dgx_font_context_t       small = {.scr = scr, .font = TerminusTTFMedium12(), .color = INK};
    dgx_font_draw(&big, 8, 34, "DGX SSD1680");
    const int along = dgx_font_draw(&small, 8, 56, "black, white and ");
    small.color     = RED;
    dgx_font_draw(&small, 8 + along, 56, "red");
    dgx_fill_rectangle(scr, 8, 64, w - 16, 6, RED);
    /* the three colors, and the colors of dgx_colors.h as the nearest of them */
    const uint32_t swatches[] = {
        DGX_BLACK(dgx_rgb_to_bwr), DGX_RED(dgx_rgb_to_bwr), DGX_ORANGE(dgx_rgb_to_bwr), DGX_NAVY(dgx_rgb_to_bwr), DGX_SILVER(dgx_rgb_to_bwr),
    };
    for (int i = 0; i < (int)(sizeof(swatches) / sizeof(swatches[0])); ++i) {
        dgx_fill_rectangle(scr, 8 + i * 22, 78, 18, 18, swatches[i]);
        dgx_draw_rectangle(scr, 8 + i * 22, 78, 18, 18, INK);
    }
    dgx_draw_circle(scr, 160, 96, 22, INK);
    dgx_solid_circle(scr, 160, 96, 14, RED);
    dgx_draw_line(scr, 196, 76, 280, 116, INK);
    dgx_draw_line(scr, 196, 116, 280, 76, RED);
    snprintf(text, sizeof(text), "b%d", BUILD);
    dgx_font_string_utf8_screen(scr, 8, h - 8, text, INK, DgxOutputNormal, 1, GLCD5x7(), NULL, NULL);
}
