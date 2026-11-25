/*
 * Shapes and textures placed between pixels, on the round 466x466 AMOLED of
 * the Waveshare ESP32-S3-Touch-AMOLED-1.32, a CO5300 on QSPI. Eight scenes of
 * eight seconds each, in a loop. In most of them the left half draws a
 * picture with hard edges in whole pixels, and the right half draws the same
 * picture with soft edges, placed in sixteenths of a pixel.
 */
#include <math.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_qspi_esp32.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_gauge.h"
#include "dgx_hw_font.h"
#include "dgx_screen.h"
#include "dgx_vscreen.h"
#include "drivers/co5300.h"
#include "fonts/DroidSansRegular9.h"
#include "fonts/TerminusTTFMedium12.h"
#include "fonts/font0ss.h"

/* shown on the screen and in the log; changed at every flash of a build to be looked at */
#define BUILD 907

static const char *TAG = "dgx_soft_shapes_demo";

/* The display: QSPI on SPI2 at 40 MHz, no backlight line. */
#define EXAMPLE_LCD_HOST SPI2_HOST

static const int EXAMPLE_LCD_PIN_NUM_CS     = 10;
static const int EXAMPLE_LCD_PIN_NUM_CLK    = 11;
static const int EXAMPLE_LCD_PIN_NUM_D0     = 12;
static const int EXAMPLE_LCD_PIN_NUM_D1     = 13;
static const int EXAMPLE_LCD_PIN_NUM_D2     = 14;
static const int EXAMPLE_LCD_PIN_NUM_D3     = 15;
static const int EXAMPLE_LCD_PIN_NUM_RST    = 8;
static const int EXAMPLE_LCD_PIN_NUM_TE     = 9;
static const int EXAMPLE_LCD_QSPI_CLOCK_MHZ = 40;

/* The area the scenes draw in, inside the round glass, and the centres of its two halves. */
#define AREA_X  16
#define AREA_Y  122
#define AREA_W  434
#define AREA_H  250
#define MID_X   233
#define MID_Y   247
#define LEFT_X  125
#define RIGHT_X 341
/* How long each scene runs. */
#define SCENE_US 8000000LL

/* The card of texture_demo, drawn once into a 16-bit texture, and the lens, an 8-bit one. */
#define CARD_W 96
#define CARD_H 64
#define LENS   64

static uint32_t      back;               /* the background, black */
static dgx_screen_t *card, *lens, *veil; /* veil: the opacity of each texel of the card */

/*
 * Converts a position in pixels, where whole numbers are pixel centres as in
 * dgx_fill_circle() and dgx_draw_texture_quad(), to the sixteenths of a pixel
 * that the calls taking a struct use: there (0, 0) is the top-left corner of
 * pixel (0, 0), and its centre is (8, 8).
 */
static int at(float v)
{
    return (int)lroundf((v + 0.5f) * DGX_SUBPIXEL);
}

/* Converts a length in pixels, a radius or a width, to sixteenths of a pixel. */
static int len16(float v)
{
    return (int)lroundf(v * DGX_SUBPIXEL);
}

/* ---- the textures ---- */

/* A text of the handwritten font fitted into a box of the card, in its middle. */
static void write_in(dgx_screen_t *texture, const char *text, int x, int y, int w, int h, int pen, uint32_t color)
{
    const dgx_font_t *font = &font0ss;
    int               left, top, right, bottom;
    if (!dgx_hw_text_box(font, text, &left, &top, &right, &bottom)) return;
    float                  by_width  = (float)(w - pen) / (right - left + 1);
    float                  by_height = (float)(h - pen) / (bottom - top + 1);
    float                  scale     = by_width < by_height ? by_width : by_height;
    int                    tx        = x + (int)lroundf((w - (right - left + 1) * scale) / 2 - left * scale);
    int                    ty        = y + (int)lroundf((h - (bottom - top + 1) * scale) / 2 - top * scale);
    const dgx_hw_context_t hand      = {.scr = texture, .font = font, .scale_x = scale, .width = pen, .color = color, .joined = true};
    dgx_hw_draw(&hand, tx, ty, text);
}

/* A card with a frame, a word written by hand and a red copyright sign in its top-right corner. */
static void draw_card(dgx_screen_t *texture)
{
    uint32_t ground = dgx_rgb_to_16(20, 40, 110), edge = dgx_rgb_to_16(250, 246, 232), ink = dgx_rgb_to_16(255, 210, 60);
    dgx_fill_rectangle(texture, 0, 0, CARD_W, CARD_H, edge);
    dgx_fill_rectangle(texture, 3, 3, CARD_W - 6, CARD_H - 6, ground);
    write_in(texture, "DGX", 8, 10, CARD_W - 32, CARD_H - 18, 3, ink);
    uint32_t           red = dgx_rgb_to_16(240, 70, 70);
    int                cx = CARD_W - 13, cy = 14, r = 6;
    int16_t            advance;
    const dgx_glyph_t *c = dgx_font_glyph(&DroidSansRegular9, 'C', &advance);
    if (c) {
        /* the letter is put by its base line: its box goes around the middle of the circle */
        const dgx_font_context_t sign = {.scr = texture, .font = &DroidSansRegular9, .color = red};
        dgx_font_draw_char(&sign, cx - c->x_offset - c->width / 2, cy - c->y_offset - c->height / 2, 'C');
    }
    dgx_draw_circle(texture, cx, cy, r, red);
}

/* The opacity of each texel of the card: opaque inside an oval in the middle, fading to transparent at the edge. */
static void draw_veil(dgx_screen_t *map)
{
    for (int j = 0; j < CARD_H; ++j)
        for (int i = 0; i < CARD_W; ++i) {
            const float u = (i + 0.5f - CARD_W / 2.0f) / (CARD_W / 2.0f), v = (j + 0.5f - CARD_H / 2.0f) / (CARD_H / 2.0f);
            const float r = sqrtf(u * u + v * v), a = r < 0.6f ? 1 : r > 1 ? 0 : (1 - r) / 0.4f;
            dgx_set_pixel(map, i, j, (uint32_t)lroundf(a * 255));
        }
}

/*
 * A lens made of color indices: a core of index 1 with a cross of 2, a ring
 * of alternating sectors of 3 and 4, which the scene makes translucent, a rim
 * of 5, and 0 outside, which it makes transparent.
 */
static void draw_lens(dgx_screen_t *texture)
{
    for (int j = 0; j < LENS; ++j)
        for (int i = 0; i < LENS; ++i) {
            const float dx = i + 0.5f - LENS / 2.0f, dy = j + 0.5f - LENS / 2.0f, r = sqrtf(dx * dx + dy * dy);
            uint32_t    index = 0;
            if (r < 13) index = fabsf(dx) < 2.5f || fabsf(dy) < 2.5f ? 2 : 1;
            else if (r < 28) index = ((int)((atan2f(dy, dx) + (float)M_PI) * 8 / (float)M_PI) & 1) ? 3 : 4;
            else if (r < 31) index = 5;
            dgx_set_pixel(texture, i, j, index);
        }
}

/* ---- what lies under a frame ---- */

/* A line of 12-pixel Terminus at double size, centred on cx in a field `field` pixels wide. */
static void label(dgx_screen_t *scr, int cx, int y, int field, const char *text)
{
    const dgx_font_context_t ctx = {.scr   = scr,
                                    .font  = &TerminusTTFMedium12,
                                    .color = DGX_WHITE(dgx_rgb_to_16),
                                    .bg    = back,
                                    .scale = 2,
                                    .field = field,
                                    .align = DGX_FONT_ALIGN_CENTER};
    dgx_font_draw(&ctx, cx - field / 2, y, text);
}

static void clear_area(dgx_screen_t *scr)
{
    dgx_fill_rectangle(scr, AREA_X, AREA_Y, AREA_W, AREA_H, back);
}

/* Colored stripes, so that soft edges and translucent paint show what they are mixed with. */
static void stripes(dgx_screen_t *scr)
{
    const uint32_t stripe[4] = {dgx_rgb_to_16(200, 200, 60), dgx_rgb_to_16(30, 30, 30), dgx_rgb_to_16(60, 200, 120), dgx_rgb_to_16(30, 30, 30)};
    for (int x = 0; x < AREA_W; x += 18)
        dgx_fill_rectangle(scr, AREA_X + x, AREA_Y, x + 18 > AREA_W ? AREA_W - x : 18, AREA_H, stripe[(x / 18) & 3]);
}

/* ---- DIAL: a dial whose needle moves slowly ---- */

#define DIAL_FROM 225.0f /* the angle of the value 0, in degrees counter-clockwise from the positive x axis */
#define DIAL_TO   -45.0f /* the angle of the value 100 */

static float dial_angle(float value)
{
    return DIAL_FROM + (DIAL_TO - DIAL_FROM) * value / 100.0f;
}

/* In whole pixels: an arc, thick lines for the ticks and the needle, a circle for the hub. */
static void dial_hard(dgx_screen_t *scr, int cx, int cy, float value)
{
    const uint32_t dim = dgx_rgb_to_16(50, 50, 62), ink = dgx_rgb_to_16(80, 220, 120), white = DGX_WHITE(dgx_rgb_to_16),
                   red = dgx_rgb_to_16(240, 70, 70);
    const float av     = dial_angle(value);
    dgx_draw_arc(scr, cx, cy, 88, 88, DIAL_TO, DIAL_FROM, 14, dim);
    if (av < DIAL_FROM) dgx_draw_arc(scr, cx, cy, 88, 88, av, DIAL_FROM, 14, ink);
    for (int k = 0; k <= 10; ++k) {
        const float a = dial_angle(k * 10.0f) * (float)(M_PI / 180), c = cosf(a), s = sinf(a);
        dgx_draw_thick_line(scr, cx + (int)lroundf(66 * c), cy - (int)lroundf(66 * s), cx + (int)lroundf(76 * c), cy - (int)lroundf(76 * s), 2,
                            white);
    }
    const float a = av * (float)(M_PI / 180);
    dgx_draw_thick_line(scr, cx, cy, cx + (int)lroundf(72 * cosf(a)), cy - (int)lroundf(72 * sinf(a)), 4, red);
    dgx_fill_circle(scr, cx, cy, 7, white);
}

/*
 * Soft, between pixels: a ring with round ends, its lit part in a gradient
 * from green to red; wedges for the ticks, wider at the outer end; a wedge
 * with round ends for the needle, 9 pixels wide at the hub and 1.6 at the
 * tip; the hub is two circles.
 */
static void dial_soft(dgx_screen_t *scr, float cx, float cy, float value)
{
    const uint32_t dim = dgx_rgb_to_16(50, 50, 62), white = DGX_WHITE(dgx_rgb_to_16), red = dgx_rgb_to_16(240, 70, 70);
    const float    av   = dial_angle(value);
    dgx_ring_t     ring = {
            .x       = at(cx),
            .y       = at(cy),
            .r_inner = len16(81),
            .r_outer = len16(95),
            .from    = DIAL_TO,
            .to      = DIAL_FROM,
            .round   = true,
            .paint   = {.color = dim, .flags = DGX_PAINT_SOFT}
    };
    dgx_fill_ring_with(scr, &ring);
    if (av < DIAL_FROM) {
        ring.from  = av;
        ring.paint = (dgx_paint_t){
            .color  = dgx_rgb_to_16(80, 220, 120),
            .color2 = dgx_rgb_to_16(240, 80, 60),
            .gx     = {at(cx - 95), at(cx + 95)},
            .gy     = {at(cy),      at(cy)     },
            .flags  = DGX_PAINT_SOFT | DGX_PAINT_LINEAR
        };
        dgx_fill_ring_with(scr, &ring);
    }
    for (int k = 0; k <= 10; ++k) {
        const float a = dial_angle(k * 10.0f) * (float)(M_PI / 180), c = cosf(a), s = sinf(a);
        dgx_wedge_t tick = {
            .x     = {at(cx + 66 * c), at(cx + 77 * c)        },
            .y     = {at(cy - 66 * s), at(cy - 77 * s)        },
            .width = {len16(1.4f),     len16(3.2f)            },
            .paint = {.color = white,  .flags = DGX_PAINT_SOFT}
        };
        dgx_draw_wedge_with(scr, &tick);
    }
    const float a      = av * (float)(M_PI / 180);
    dgx_wedge_t needle = {
        .x     = {at(cx),       at(cx + 74 * cosf(a))  },
        .y     = {at(cy),       at(cy - 74 * sinf(a))  },
        .width = {len16(9),     len16(1.6f)            },
        .round = true,
        .paint = {.color = red, .flags = DGX_PAINT_SOFT}
    };
    dgx_draw_wedge_with(scr, &needle);
    dgx_ellipse_t hub = {
        .x = at(cx), .y = at(cy), .rx = len16(7.5f), .ry = len16(7.5f), .paint = {.color = white, .flags = DGX_PAINT_SOFT}
    };
    dgx_fill_ellipse_with(scr, &hub);
    hub.rx = hub.ry = len16(3), hub.paint.color = dim;
    dgx_fill_ellipse_with(scr, &hub);
}

static void dial(dgx_screen_t *scr, int half, float s)
{
    const float value = 50 - 48 * cosf(s * 0.45f);
    if (half == 0) dial_hard(scr, LEFT_X, MID_Y + 10, value);
    else dial_soft(scr, RIGHT_X, MID_Y + 10, value);
}

/* ---- SHAPES: the same shapes with hard edges and with soft ones, over stripes ---- */

/*
 * A five-pointed star drawn in one stroke, with a top-to-bottom gradient; a
 * translucent rounded rectangle with a radial gradient; three dots moving
 * slowly along circles, about 5 pixels a second; a translucent arc going
 * round.
 */
static void shapes(dgx_screen_t *scr, int half, float s)
{
    const float    cx = half ? RIGHT_X : LEFT_X, cy = MID_Y;
    const uint32_t soft = half ? DGX_PAINT_SOFT : 0;
    dgx_point_2d_t star[5];
    for (int i = 0; i < 5; ++i) {
        const float a = s * 0.25f + i * 4 * (float)M_PI / 5;
        star[i]       = (dgx_point_2d_t){(int16_t)at(cx + 78 * cosf(a)), (int16_t)at(cy + 78 * sinf(a))};
    }
    const dgx_polygon_t polygon = {
        .points = star,
        .count  = 5,
        .paint  = {.color  = dgx_rgb_to_16(255, 220, 80),
                   .color2 = dgx_rgb_to_16(230, 80, 40),
                   .gx     = {at(cx), at(cx)},
                   .gy     = {at(cy - 78), at(cy + 78)},
                   .flags  = DGX_PAINT_LINEAR | soft}
    };
    dgx_fill_polygon_with(scr, &polygon);

    const float                 rx = cx - 60 + 18 * sinf(s * 0.5f), ry = cy - 28 + 9 * cosf(s * 0.4f);
    const dgx_round_rectangle_t rect = {
        .x     = at(rx),
        .y     = at(ry),
        .w     = len16(120),
        .h     = len16(56),
        .r     = len16(16),
        .paint = {.color   = dgx_rgb_to_16(200, 240, 255),
                  .color2  = dgx_rgb_to_16(30, 60, 200),
                  .gx      = {at(rx + 60), at(rx + 60)},
                  .gy      = {at(ry + 28), at(ry + 98)},
                  .opacity = 170,
                  .flags   = DGX_PAINT_RADIAL | DGX_PAINT_OPACITY | soft}
    };
    dgx_fill_round_rectangle_with(scr, &rect);

    for (int k = 0; k < 3; ++k) {
        const float   a = s * (0.12f + 0.03f * k) + k * 2.1f, r = 30 + 22 * k;
        dgx_ellipse_t dot = {
            .x     = at(cx + r * cosf(a)),
            .y     = at(cy + r * sinf(a)),
            .rx    = len16(4.5f),
            .ry    = len16(4.5f),
            .paint = {.color = DGX_WHITE(dgx_rgb_to_16), .flags = soft}
        };
        dgx_fill_ellipse_with(scr, &dot);
    }

    const dgx_ring_t arc = {
        .x       = at(cx),
        .y       = at(cy),
        .r_inner = len16(92),
        .r_outer = len16(100),
        .from    = s * 20,
        .to      = s * 20 + 120,
        .round   = true,
        .paint   = {.color = dgx_rgb_to_16(255, 80, 200), .opacity = 200, .flags = DGX_PAINT_OPACITY | soft}
    };
    dgx_fill_ring_with(scr, &arc);
}

/* ---- CHART: values that scroll by less than a pixel a frame ---- */

#define CHART_N    14 /* points of the chart; the first and the last lie outside it */
#define CHART_W    190
#define CHART_STEP ((float)CHART_W / (CHART_N - 2))
#define CHART_INK  dgx_rgb_to_16(90, 200, 255)

static float chart_value(float x)
{
    return 45 * sinf(x * 0.031f) * cosf(x * 0.007f) + 18 * sinf(x * 0.11f) + (fmodf(x, 150.0f) < 40 ? 20 : 0);
}

static void chart_ground(dgx_screen_t *scr)
{
    clear_area(scr);
    for (int half = 0; half < 2; ++half)
        for (int g = 0; g <= 4; ++g)
            dgx_fill_rectangle(scr, (half ? RIGHT_X : LEFT_X) - CHART_W / 2, MID_Y - 80 + g * 40, CHART_W, 1, dgx_rgb_to_16(40, 44, 56));
}

/*
 * Left: dgx_draw_polyline(), straight from value to value in whole pixels.
 * Right: DGX_POLYLINE_MONOTONE, a soft curve that never goes above or below
 * both of two neighbouring values, and translucent dots at the values.
 */
static void chart(dgx_screen_t *scr, int half, float s)
{
    const int      left  = (half ? RIGHT_X : LEFT_X) - CHART_W / 2;
    const float    shift = s * 6.0f; /* 6 pixels a second */
    const float    phase = fmodf(shift, CHART_STEP), first = shift - phase;
    dgx_point_2d_t points[CHART_N];
    for (int i = 0; i < CHART_N; ++i) {
        const float x = left + (i - 1) * CHART_STEP - phase, y = MID_Y - chart_value(first + i * CHART_STEP);
        points[i] = half ? (dgx_point_2d_t){(int16_t)at(x), (int16_t)at(y)} : (dgx_point_2d_t){(int16_t)lroundf(x), (int16_t)lroundf(y)};
    }
    dgx_screen_clip(scr, left, MID_Y - 100, CHART_W, 200);
    if (half == 0) {
        dgx_draw_polyline(scr, points, CHART_N, 2, CHART_INK);
    } else {
        const dgx_polyline_t line = {
            .points = points,
            .count  = CHART_N,
            .width  = len16(2.5f),
            .way    = DGX_POLYLINE_MONOTONE,
            .paint  = {.color = CHART_INK, .flags = DGX_PAINT_SOFT}
        };
        dgx_draw_polyline_with(scr, &line);
        for (int i = 0; i < CHART_N; ++i) {
            const dgx_ellipse_t dot = {
                .x     = points[i].x,
                .y     = points[i].y,
                .rx    = len16(4),
                .ry    = len16(4),
                .paint = {.color = DGX_WHITE(dgx_rgb_to_16), .opacity = 140, .flags = DGX_PAINT_SOFT | DGX_PAINT_OPACITY}
            };
            dgx_fill_ellipse_with(scr, &dot);
        }
    }
    dgx_screen_unclip(scr);
}

/* ---- the card on four corners ---- */

/* The corners of the card turned by `angle` and scaled, centred at (cx, cy): top-left, top-right, bottom-right, bottom-left. */
static void turned(float x[4], float y[4], float cx, float cy, float angle, float scale)
{
    static const float side_x[4] = {-1, 1, 1, -1}, side_y[4] = {-1, -1, 1, 1};
    const float        c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 4; ++i) {
        const float px = side_x[i] * CARD_W / 2 * scale, py = side_y[i] * CARD_H / 2 * scale;
        x[i] = cx + px * c - py * s, y[i] = cy + px * s + py * c;
    }
}

/* The corners of the card rotated by `angle` about its vertical axis, seen from 330 pixels in front. */
static void flipped(float x[4], float y[4], float cx, float cy, float angle, float scale)
{
    static const float side_x[4] = {-1, 1, 1, -1}, side_y[4] = {-1, -1, 1, 1};
    const float        eye = 330.0f;
    for (int i = 0; i < 4; ++i) {
        const float px = side_x[i] * CARD_W / 2 * scale, py = side_y[i] * CARD_H / 2 * scale;
        const float k = eye / (eye + px * sinf(angle));
        x[i] = cx + px * cosf(angle) * k, y[i] = cy + py * k;
    }
}

/*
 * Left: dgx_draw_texture_quad(), the corners in whole pixels, the nearest
 * texel; a quad that is not a parallelogram is mapped as two triangles.
 * Right: dgx_draw_texture_quad_with(), the corners in sixteenths of a pixel,
 * bilinear filtering, soft edges; such a quad is mapped in perspective.
 */
static void put_card(dgx_screen_t *scr, int half, const float x[4], const float y[4])
{
    if (half == 0) {
        dgx_point_2d_t quad[4];
        for (int i = 0; i < 4; ++i)
            quad[i] = (dgx_point_2d_t){(int16_t)lroundf(x[i]), (int16_t)lroundf(y[i])};
        dgx_draw_texture_quad(scr, quad, card, 0, 0, CARD_W, CARD_H);
    } else {
        dgx_texture_quad_t quad = {.texture = card, .tw = CARD_W, .th = CARD_H, .flags = DGX_TEXTURE_BILINEAR | DGX_TEXTURE_SOFT};
        for (int i = 0; i < 4; ++i)
            quad.x[i] = at(x[i]), quad.y[i] = at(y[i]);
        dgx_draw_texture_quad_with(scr, &quad);
    }
}

/* TURN: the card turning slowly, growing and shrinking a little. */
static void turn(dgx_screen_t *scr, int half, float s)
{
    float x[4], y[4];
    turned(x, y, half ? RIGHT_X : LEFT_X, MID_Y, s * 0.12f, 1.3f + 0.08f * sinf(s * 0.7f));
    put_card(scr, half, x, y);
}

/* FLIP: the card turning about its vertical axis. */
static void flip(dgx_screen_t *scr, int half, float s)
{
    float x[4], y[4];
    flipped(x, y, half ? RIGHT_X : LEFT_X, MID_Y, s * 0.9f, 1.45f);
    put_card(scr, half, x, y);
}

/* VEIL: the card over stripes, on the right with the opacity of each texel from `veil`. */
static void veil_scene(dgx_screen_t *scr, int half, float s)
{
    float x[4], y[4];
    turned(x, y, half ? RIGHT_X : LEFT_X, MID_Y, s * 0.15f, 1.6f);
    dgx_texture_quad_t quad = {
        .texture = card, .tw = CARD_W, .th = CARD_H, .alpha_map = half ? veil : NULL, .flags = DGX_TEXTURE_BILINEAR | DGX_TEXTURE_SOFT};
    for (int i = 0; i < 4; ++i)
        quad.x[i] = at(x[i]), quad.y[i] = at(y[i]);
    dgx_draw_texture_quad_with(scr, &quad);
}

/*
 * LENSES: three lenses of the 8-bit texture over stripes, each with its own
 * colors and size, circling and turning. The opacity table makes index 0
 * transparent and sectors 3 and 4 translucent.
 */
static uint16_t      lens_colors[3][256];
static const uint8_t lens_opacity[256] = {0, 255, 255, 150, 70, 255};

static void lenses_start(dgx_screen_t *scr)
{
    (void)scr;
    static const uint8_t hue[3][3] = {
        {60,  140, 255},
        {230, 60,  200},
        {255, 150, 30 }
    };
    for (int k = 0; k < 3; ++k) {
        lens_colors[k][1] = (uint16_t)dgx_rgb_to_16(hue[k][0], hue[k][1], hue[k][2]);
        lens_colors[k][2] = (uint16_t)DGX_WHITE(dgx_rgb_to_16);
        lens_colors[k][3] = (uint16_t)dgx_rgb_to_16(hue[k][0] / 2, hue[k][1] / 2, hue[k][2] / 2);
        lens_colors[k][4] = lens_colors[k][1];
        lens_colors[k][5] = (uint16_t)dgx_rgb_to_16(250, 250, 250);
    }
}

static void lenses(dgx_screen_t *scr, int half, float s)
{
    (void)half;
    static const float side_x[4] = {-1, 1, 1, -1}, side_y[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 3; ++k) {
        const float        a = s * 0.7f + k * 2.0944f, half_side = LENS / 2.0f * (1.5f + 0.4f * k);
        const float        cx = MID_X + 95 * cosf(a), cy = MID_Y + 55 * sinf(a), angle = s * (0.5f + 0.3f * k);
        const float        c = cosf(angle), sn = sinf(angle);
        dgx_texture_quad_t quad = {
            .texture = lens, .tw = LENS, .th = LENS, .lut = lens_colors[k], .alpha = lens_opacity, .flags = DGX_TEXTURE_BILINEAR | DGX_TEXTURE_SOFT};
        for (int i = 0; i < 4; ++i) {
            quad.x[i] = at(cx + (side_x[i] * c - side_y[i] * sn) * half_side);
            quad.y[i] = at(cy + (side_x[i] * sn + side_y[i] * c) * half_side);
        }
        dgx_draw_texture_quad_with(scr, &quad);
    }
}

/* ---- GAUGE: the arc gauge with hard edges and with soft ones ---- */

static dgx_gauge_t hard_gauge, soft_gauge;

/* green at 0, red at 100 */
static uint32_t by_value(int value, void *user_data)
{
    (void)user_data;
    const int r = value * 51 / 10, g = (100 - value) * 51 / 10;
    return dgx_rgb_to_16(r > 255 ? 255 : r, g > 255 ? 255 : g, 40);
}

/* The soft gauge mixes the edges of its ring with the color around it, black here. */
static void gauge_start(dgx_screen_t *scr)
{
    const uint32_t ring = dgx_rgb_to_16(55, 55, 65);
    dgx_gauge_init_with(&hard_gauge, scr, LEFT_X, MID_Y, 66, 20, 225.0f, 270, 0, 100, ring, by_value, NULL);
    dgx_gauge_init_soft(&soft_gauge, scr, RIGHT_X, MID_Y, 66, 20, 225.0f, 270, 0, 100, ring, back, by_value, NULL);
}

/* A gauge redraws only the steps its value has changed, so the area is not cleared between frames. */
static void gauge(dgx_screen_t *scr, int half, float s)
{
    const int value = (int)lroundf(50 - 50 * cosf(s * 1.1f));
    char      number[8];
    dgx_gauge_set_value(half ? &soft_gauge : &hard_gauge, value);
    snprintf(number, sizeof(number), "%d", value);
    label(scr, half ? RIGHT_X : LEFT_X, MID_Y - 12, 80, number);
}

/* ---- the scenes ---- */

typedef struct
{
    const char *name;
    const char *left, *right;                           /* what the halves show; NULL for one picture */
    void (*start)(dgx_screen_t *scr);                   /* NULL, or called once before the first frame */
    void (*ground)(dgx_screen_t *scr);                  /* NULL, or drawn under every frame */
    void (*draw)(dgx_screen_t *scr, int half, float s); /* draws one half of a frame, 0 the left, s seconds into the scene */
} scene_t;

static const scene_t scenes[] = {
    {"DIAL",   "HARD",      "SOFT",        NULL,         clear_area,   dial      },
    {"SHAPES", "HARD",      "SOFT",        NULL,         stripes,      shapes    },
    {"CHART",  "HARD",      "SOFT",        NULL,         chart_ground, chart     },
    {"VEIL",   "NO MAP",    "ALPHA MAP",   NULL,         stripes,      veil_scene},
    {"TURN",   "HARD",      "SOFT",        NULL,         clear_area,   turn      },
    {"FLIP",   "TRIANGLES", "PERSPECTIVE", NULL,         clear_area,   flip      },
    {"LENSES", NULL,        NULL,          lenses_start, stripes,      lenses    },
    {"GAUGE",  "HARD",      "SOFT",        gauge_start,  NULL,         gauge     },
};
#define SCENES (int)(sizeof(scenes) / sizeof(scenes[0]))

/*
 * Runs a scene for SCENE_US. Each frame is drawn in one batch: the display
 * receives the changed rectangle once, in step with its scan, and the drawing
 * time does not include sending.
 */
static void play(dgx_screen_t *scr, const scene_t *scene)
{
    char line[32];
    dgx_screen_batch_begin(scr);
    dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, back);
    snprintf(line, sizeof(line), "#%d %s", BUILD, scene->name);
    label(scr, scr->width / 2, 57, 240, line);
    if (scene->right) {
        label(scr, LEFT_X, 92, 150, scene->left);
        label(scr, RIGHT_X, 92, 150, scene->right);
    }
    if (scene->start) scene->start(scr);
    dgx_screen_batch_end(scr);

    const int     halves   = scene->right ? 2 : 1;
    int64_t       drawn[2] = {0, 0}, sent = 0;
    int           frames = 0, counted = 0;
    const int64_t start = esp_timer_get_time();
    int64_t       now = start, counted_at = start;
    do {
        const float s = (now - start) * 1e-6f;
        dgx_screen_batch_begin(scr);
        if (now - counted_at >= 1000000) {
            /* the frame rate of the last second, below the area */
            snprintf(line, sizeof(line), "%.1f FPS", (frames - counted) * 1e6f / (now - counted_at));
            label(scr, scr->width / 2, 400, 200, line);
            counted = frames, counted_at = now;
        }
        if (scene->ground) scene->ground(scr);
        for (int half = 0; half < halves; ++half) {
            const int64_t t0 = esp_timer_get_time();
            scene->draw(scr, half, s);
            drawn[half] += esp_timer_get_time() - t0;
        }
        const int64_t t1 = esp_timer_get_time();
        dgx_screen_batch_end(scr);
        sent += esp_timer_get_time() - t1;
        ++frames;
        now = esp_timer_get_time();
        vTaskDelay(1);
    } while (now - start < SCENE_US);

    const float fps = frames * 1e6f / (esp_timer_get_time() - start);
    if (halves == 2)
        ESP_LOGI(TAG, "build %d, %s: %.1f FPS; a frame drawn in %.2f ms left (%s) and %.2f ms right (%s), sent in %.2f ms", BUILD, scene->name, fps,
                 drawn[0] * 1e-3f / frames, scene->left, drawn[1] * 1e-3f / frames, scene->right, sent * 1e-3f / frames);
    else
        ESP_LOGI(TAG, "build %d, %s: %.1f FPS; a frame drawn in %.2f ms, sent in %.2f ms", BUILD, scene->name, fps, drawn[0] * 1e-3f / frames,
                 sent * 1e-3f / frames);
}

void app_main(void)
{
    ESP_LOGI(TAG, "build %d: shapes and textures between pixels on the CO5300", BUILD);
    dgx_bus_protocols_t *bus =
        dgx_qspi_init(EXAMPLE_LCD_HOST, SPI_DMA_CH_AUTO, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_D0, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_D1,
                      (gpio_num_t)EXAMPLE_LCD_PIN_NUM_D2, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_D3, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CLK,
                      (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CS, EXAMPLE_LCD_QSPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_qspi_init failed");
        return;
    }
    /* each frame is sent in step with the scan of the panel, which its TE line reports, so no frame tears */
    const dgx_co5300_options_t options = {.tear_free = true, .te = (gpio_num_t)EXAMPLE_LCD_PIN_NUM_TE};
    dgx_screen_t *screen = dgx_co5300_init_with(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, 16, DgxScreenRGB, DGX_CO5300_466X466, &options);
    if (!screen) {
        ESP_LOGE(TAG, "dgx_co5300_init_with failed");
        dgx_bus_dispose(&bus);
        return;
    }
    back = DGX_BLACK(dgx_rgb_to_16);

    card = dgx_vscreen_init(CARD_W, CARD_H, 16, DgxScreenRGB);
    veil = dgx_vscreen_init(CARD_W, CARD_H, 8, DgxScreenRGB);
    lens = dgx_vscreen_init(LENS, LENS, 8, DgxScreenRGB);
    if (!card || !veil || !lens) {
        ESP_LOGE(TAG, "No memory for the textures");
        dgx_screen_destroy(&card);
        dgx_screen_destroy(&veil);
        dgx_screen_destroy(&lens);
        dgx_screen_destroy(&screen);
        dgx_bus_dispose(&bus);
        return;
    }
    draw_card(card);
    draw_veil(veil);
    draw_lens(lens);

    for (int k = 0;; k = (k + 1) % SCENES)
        play(screen, &scenes[k]);
}
