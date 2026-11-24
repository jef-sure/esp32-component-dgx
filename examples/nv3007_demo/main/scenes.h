#pragma once
/*
 * The pictures of the NV3007 demo, drawn by DGX alone.
 */
#include <stdbool.h>
#include <stdint.h>

#include "dgx_screen.h"

/* The number of this test: shown on every picture and printed in the log. It changes whenever the demo changes. */
#define DEMO_TEST 1

/* The panel upright: portrait, 142x428. */
#define DEMO_PANEL_W 142
#define DEMO_PANEL_H 428

/* A color in the pixel format of the screen: dgx_rgb_to_16(). */
typedef uint32_t (*demo_rgb_t)(uint8_t r, uint8_t g, uint8_t b);

/*
 * One of the eight orientations of dgx_screen_orientation(), with the
 * expected result written out by hand: one step along the screen's x and
 * along its y as steps right and down on the upright panel.
 */
typedef struct {
    dgx_orientation_t dir_x, dir_y;
    bool              swap_xy;
    const char       *name;
    int8_t            x_right, x_down, y_right, y_down;
} demo_way_t;

#define DEMO_WAYS 8
extern const demo_way_t demo_ways[DEMO_WAYS];

/* Color bars and ramps on the upright screen. */
void demo_colors(dgx_screen_t *scr, demo_rgb_t rgb);

/* The orientation picture of demo_ways[way] on a screen already turned that way. aside is a 1-bit screen of at least 96x10. */
void demo_orientation(dgx_screen_t *scr, dgx_screen_t *aside, demo_rgb_t rgb, int way);

/* The result of the fill timing on the upright screen. */
void demo_speed(dgx_screen_t *scr, demo_rgb_t rgb, int us_per_fill, int spi_mhz);

/* Lines one pixel wide at even and odd x, and lines of single points, on the upright screen. */
void demo_pixels(dgx_screen_t *scr, demo_rgb_t rgb);
