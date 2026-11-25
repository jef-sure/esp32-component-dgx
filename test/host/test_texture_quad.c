/*
 * dgx_draw_texture_quad_with(): a texture put on a quad whose corners lie
 * between pixels. On virtual screens of 16 bits: a rectangle at whole pixels
 * takes the texels one for one; one moved by half a pixel and sampled
 * bilinearly takes the mean of two; a quarter turn takes them turned; a soft
 * edge half over a pixel mixes it half; the key color and an opacity of 0
 * leave the screen as it was, an opacity of half mixes; paper takes the place
 * of what lies on the screen; a quad that is not a parallelogram maps the
 * meeting of its diagonals to the middle of the texture; nothing is drawn
 * outside the clip. On a display behind a bus, which cannot be read, a soft
 * edge is whole from half on, or mixed with paper.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h" /* the waits of the init table, for nothing on the host */
#include "drivers/dgx_lcd_init.c"
#include "drivers/panel.c"

#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_vscreen.h"
#include "vscreen/dgx_alpha_priv.h"

#define WHITE 0xFFFFu
#define BLACK 0x0000u
#define RED   0xF800u
#define BLUE  0x001Fu

static uint16_t px(dgx_screen_t *s, int x, int y)
{
    return (uint16_t)dgx_get_pixel(s, x, y);
}

/* ---- a display behind a bus: a 64x64 memory written through windows ---- */

static uint8_t  command, params[8], part[2];
static int      params_n, part_n;
static uint16_t sc, ec, sp, ep, column, page, glass[64][64];

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value)
{
    (void)bus;
    command = value, params_n = 0, part_n = 0;
    if (value == 0x2C) column = sc, page = sp;
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    for (uint32_t i = 0; i < bits / 8; ++i) {
        if (command == 0x2C) {
            part[part_n++] = data[i];
            if (part_n == 2) {
                if (column < 64 && page < 64) glass[page][column] = (uint16_t)(part[0] << 8 | part[1]);
                part_n = 0;
                if (++column > ec) column = sc, ++page;
            }
            continue;
        }
        if (params_n < (int)sizeof(params)) params[params_n] = data[i];
        const int n = ++params_n;
        if (command == 0x2A && n == 4) sc = (uint16_t)(params[0] << 8 | params[1]), ec = (uint16_t)(params[2] << 8 | params[3]);
        if (command == 0x2B && n == 4) sp = (uint16_t)(params[0] << 8 | params[1]), ep = (uint16_t)(params[2] << 8 | params[3]);
    }
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

static const dgx_lcd_init_cmd_t no_init[] = {
    {0, 0, 0xff, 0, {0}}
};
static const dgx_panel_t panel64 = {.name = "64", .width = 64, .height = 64, .colmod = {0, 0x55}, .init = no_init};

int main(void)
{
    /* the texture: 16x16, each texel its own color from its place */
    dgx_screen_t *tex = dgx_vscreen_init(16, 16, 16, DgxScreenRGB);
    for (int j = 0; j < 16; ++j)
        for (int i = 0; i < 16; ++i) dgx_set_pixel(tex, i, j, (uint32_t)(i << 11 | j << 5));
    dgx_screen_t *scr = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);

    /* a rectangle at whole pixels takes the texels one for one */
    dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
    dgx_texture_quad_t q = {.x = {10 * 16, 26 * 16, 26 * 16, 10 * 16}, .y = {20 * 16, 20 * 16, 36 * 16, 36 * 16}, .texture = tex, .tw = 16, .th = 16};
    CHECK(dgx_draw_texture_quad_with(scr, &q) == ESP_OK);
    bool same = true;
    for (int j = 0; j < 16; ++j)
        for (int i = 0; i < 16; ++i) same = same && px(scr, 10 + i, 20 + j) == (uint16_t)(i << 11 | j << 5);
    CHECK(same);
    CHECK(px(scr, 9, 20) == BLACK && px(scr, 26, 20) == BLACK && px(scr, 10, 19) == BLACK && px(scr, 10, 36) == BLACK);

    /* moved by half a pixel and sampled bilinearly: a pixel between two texels takes their mean */
    dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
    q.x[0] = q.x[3] = 10 * 16 + 8, q.x[1] = q.x[2] = 26 * 16 + 8, q.flags = DGX_TEXTURE_BILINEAR;
    CHECK(dgx_draw_texture_quad_with(scr, &q) == ESP_OK);
    /* pixel 15's centre is between texels 4 and 5: red 4.5 of 31 steps, rounded down by the mixing of 32 steps */
    const uint16_t mid = px(scr, 15, 25);
    CHECK((mid >> 11) == 4 && ((mid >> 5) & 63) == 5);

    /* a quarter turn clockwise: the top row of the texture goes down the right side */
    dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
    dgx_texture_quad_t r = {.x = {26 * 16, 26 * 16, 10 * 16, 10 * 16}, .y = {20 * 16, 36 * 16, 36 * 16, 20 * 16}, .texture = tex, .tw = 16, .th = 16};
    CHECK(dgx_draw_texture_quad_with(scr, &r) == ESP_OK);
    same = true;
    for (int j = 0; j < 16; ++j)
        for (int i = 0; i < 16; ++i) same = same && px(scr, 25 - j, 20 + i) == (uint16_t)(i << 11 | j << 5);
    CHECK(same);

    /* a soft edge half over a pixel mixes it half: white on black is gray, 16 of 32 */
    dgx_screen_t *white = dgx_vscreen_init(4, 4, 16, DgxScreenRGB);
    dgx_fill_rectangle(white, 0, 0, 4, 4, WHITE);
    dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
    dgx_texture_quad_t w = {.x = {10 * 16 + 8, 20 * 16 + 8, 20 * 16 + 8, 10 * 16 + 8},
                            .y = {10 * 16, 10 * 16, 20 * 16, 20 * 16},
                            .texture = white, .tw = 4, .th = 4, .flags = DGX_TEXTURE_SOFT};
    CHECK(dgx_draw_texture_quad_with(scr, &w) == ESP_OK);
    CHECK(px(scr, 10, 15) == dgx_alpha_mix16(BLACK, WHITE, 16) && px(scr, 20, 15) == dgx_alpha_mix16(BLACK, WHITE, 16));
    CHECK(px(scr, 11, 15) == WHITE && px(scr, 19, 15) == WHITE && px(scr, 9, 15) == BLACK && px(scr, 21, 15) == BLACK);
    /* and with paper, the half is mixed with the paper, not with the screen */
    w.flags = DGX_TEXTURE_SOFT | DGX_TEXTURE_PAPER, w.paper = BLUE;
    CHECK(dgx_draw_texture_quad_with(scr, &w) == ESP_OK);
    CHECK(px(scr, 10, 15) == dgx_alpha_mix16(BLUE, WHITE, 16));

    /* the key color is not drawn */
    dgx_set_pixel(white, 1, 1, RED);
    dgx_fill_rectangle(scr, 0, 0, 64, 64, BLUE);
    dgx_texture_quad_t k = {
        .x = {0, 64 * 16, 64 * 16, 0}, .y = {0, 0, 64 * 16, 64 * 16}, .texture = white, .tw = 4, .th = 4, .key = RED, .flags = DGX_TEXTURE_KEY};
    CHECK(dgx_draw_texture_quad_with(scr, &k) == ESP_OK);
    /* texel (1, 1) is pixels 16 to 31 both ways */
    CHECK(px(scr, 0, 0) == WHITE && px(scr, 16, 16) == BLUE && px(scr, 31, 31) == BLUE && px(scr, 32, 32) == WHITE && px(scr, 15, 20) == WHITE);

    /* an 8-bit texture: index 0 transparent, 1 opaque red, 2 red half over what lies there */
    dgx_screen_t *idx = dgx_vscreen_init(3, 1, 8, DgxScreenRGB);
    for (int i = 0; i < 3; ++i) dgx_set_pixel(idx, i, 0, (uint32_t)i);
    static uint16_t lut[256];
    static uint8_t  alpha[256];
    lut[1] = lut[2] = RED, alpha[1] = 255, alpha[2] = 128;
    dgx_fill_rectangle(scr, 0, 0, 64, 64, BLUE);
    dgx_texture_quad_t ix = {
        .x = {0, 48 * 16, 48 * 16, 0}, .y = {0, 0, 16 * 16, 16 * 16}, .texture = idx, .tw = 3, .th = 1, .lut = lut, .alpha = alpha};
    CHECK(dgx_draw_texture_quad_with(scr, &ix) == ESP_OK);
    CHECK(px(scr, 8, 8) == BLUE && px(scr, 24, 8) == RED && px(scr, 40, 8) == dgx_alpha_mix16(BLUE, RED, 16));
    ix.lut = NULL;
    CHECK(dgx_draw_texture_quad_with(scr, &ix) == ESP_ERR_INVALID_ARG);

    /* not a parallelogram: where the diagonals meet takes the middle of the texture */
    dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
    dgx_texture_quad_t p = {.x = {20 * 16, 44 * 16, 60 * 16, 4 * 16}, .y = {4 * 16, 4 * 16, 60 * 16, 60 * 16}, .texture = tex, .tw = 16, .th = 16};
    CHECK(dgx_draw_texture_quad_with(scr, &p) == ESP_OK);
    /* the diagonals (20,4)-(60,60) and (44,4)-(4,60) meet at (32, 20.8): pixel 31,20, texel 8,8 or a neighbour */
    const uint16_t centre = px(scr, 31, 20);
    CHECK((centre >> 11) >= 7 && (centre >> 11) <= 8 && ((centre >> 5) & 63) >= 7 && ((centre >> 5) & 63) <= 8);

    /* nothing outside the clip */
    dgx_set_pixel(white, 1, 1, WHITE);
    dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
    dgx_screen_clip(scr, 16, 16, 16, 16);
    CHECK(dgx_draw_texture_quad_with(scr, &k) == ESP_OK);
    CHECK(px(scr, 15, 20) == BLACK && px(scr, 32, 20) == BLACK && px(scr, 20, 15) == BLACK && px(scr, 20, 32) == BLACK && px(scr, 20, 20) == WHITE);
    dgx_screen_unclip(scr);

    /* depths it does not take */
    dgx_screen_t *s18 = dgx_vscreen_init(8, 8, 18, DgxScreenRGB);
    CHECK(dgx_draw_texture_quad_with(s18, &q) == ESP_ERR_NOT_SUPPORTED);

    /* a display behind a bus: a soft edge whole from half on, or mixed with paper */
    static uint8_t      buffer[512];
    dgx_bus_protocols_t bus = {.bus_type = DGX_BUS_SPI, .write_command = bus_command, .write_data = bus_data, .write_data_async = bus_data,
                               .sync_write = bus_sync, .buffer = buffer, .buffer_len = sizeof(buffer)};
    dgx_screen_t       *lcd = dgx_panel_init(&bus, &panel64, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB);
    CHECK(lcd && !dgx_screen_is_readable(lcd));
    memset(glass, 0, sizeof(glass));
    w.flags = DGX_TEXTURE_SOFT;
    w.x[0] = w.x[3] = 10 * 16 + 9, w.x[1] = w.x[2] = 20 * 16 + 9; /* the left edge covers pixel 10 a bit less than half, the right one pixel 20 a bit more */
    CHECK(dgx_draw_texture_quad_with(lcd, &w) == ESP_OK);
    CHECK(glass[15][10] == BLACK && glass[15][20] == WHITE && glass[15][11] == WHITE);
    w.flags = DGX_TEXTURE_SOFT | DGX_TEXTURE_PAPER, w.paper = BLUE;
    CHECK(dgx_draw_texture_quad_with(lcd, &w) == ESP_OK);
    CHECK(glass[15][10] != BLACK && glass[15][10] != WHITE && glass[15][10] != BLUE);

    /*
     * The inside of a quad of a texture with no transparent texel goes by a
     * loop of its own; with a key no texel has, the same texture goes pixel by
     * pixel. Both give the same pixels: turned, in perspective, every way of
     * sampling and of edges, over a picture of every color.
     */
    {
        dgx_screen_t *noise = dgx_vscreen_init(23, 17, 16, DgxScreenRGB);
        dgx_screen_t *a = dgx_vscreen_init(96, 96, 16, DgxScreenRGB), *b = dgx_vscreen_init(96, 96, 16, DgxScreenRGB);
        uint32_t      seed = 12345;
        for (int y = 0; y < 17; ++y)
            for (int x = 0; x < 23; ++x) seed = seed * 1103515245u + 12345u, dgx_set_pixel(noise, x, y, ((seed >> 8) & 0xffff) | 1u);
        static const int quads[][8] = {
            {130, 70, 1290, 310, 1090, 1350, -70, 1110}, /* turned, a parallelogram */
            {200, 260, 1400, 120, 1300, 1420, 300, 1180}, /* in perspective */
            {-300, -200, 900, 40, 700, 1100, -500, 860},  /* turned, over the edges of the screen */
        };
        int differ = 0, drawn = 0;
        for (int k = 0; k < 3; ++k)
            for (uint32_t flags = 0; flags < 4; ++flags) {
                dgx_fill_rectangle(a, 0, 0, 96, 96, 0x1234), dgx_fill_rectangle(b, 0, 0, 96, 96, 0x1234);
                dgx_texture_quad_t q = {.texture = noise, .tx = 2, .ty = 1, .tw = 19, .th = 15, .flags = flags};
                for (int i = 0; i < 4; ++i) q.x[i] = quads[k][2 * i], q.y[i] = quads[k][2 * i + 1];
                CHECK(dgx_draw_texture_quad_with(a, &q) == ESP_OK);
                /* a key of a color none of the texels has */
                q.flags |= DGX_TEXTURE_KEY, q.key = 0;
                CHECK(dgx_draw_texture_quad_with(b, &q) == ESP_OK);
                for (int y = 0; y < 96; ++y)
                    for (int x = 0; x < 96; ++x) differ += px(a, x, y) != px(b, x, y), drawn += px(a, x, y) != 0x1234;
            }
        CHECK(differ == 0 && drawn > 12 * 2000);
        /*
         * A display behind a bus has no memory to write to: the inside of an
         * opaque texture goes to it by runs, any other pixel by itself. It shows
         * what a virtual screen has, soft edges mixed with paper on both.
         */
        dgx_screen_t *m = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
        differ = 0, drawn = 0;
        for (int k = 0; k < 3; ++k)
            for (uint32_t flags = 0; flags < 8; ++flags) {
                dgx_texture_quad_t q = {.texture = noise, .tx = 2, .ty = 1, .tw = 19, .th = 15, .paper = 0x1234,
                                        .flags = (flags & 3) | (flags & DGX_TEXTURE_SOFT ? DGX_TEXTURE_PAPER : 0) | (flags & 4 ? DGX_TEXTURE_KEY : 0)};
                for (int i = 0; i < 4; ++i) q.x[i] = quads[k][2 * i] * 2 / 3, q.y[i] = quads[k][2 * i + 1] * 2 / 3;
                dgx_fill_rectangle(m, 0, 0, 64, 64, 0x1234);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) glass[y][x] = 0x1234;
                CHECK(dgx_draw_texture_quad_with(m, &q) == ESP_OK && dgx_draw_texture_quad_with(lcd, &q) == ESP_OK);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) differ += px(m, x, y) != glass[y][x], drawn += glass[y][x] != 0x1234;
            }
        CHECK(differ == 0 && drawn > 24 * 800);
        /* the same with an 8-bit texture of every opacity, mixed with paper on both */
        dgx_screen_t   *ink = dgx_vscreen_init(23, 17, 8, DgxScreenRGB);
        static uint16_t inks[256];
        static uint8_t  opacity[256];
        for (int i = 0; i < 256; ++i) seed = seed * 1103515245u + 12345u, inks[i] = (uint16_t)(seed >> 8), opacity[i] = (uint8_t)(i < 40 ? 0 : i > 200 ? 255 : seed >> 24);
        for (int y = 0; y < 17; ++y)
            for (int x = 0; x < 23; ++x) seed = seed * 1103515245u + 12345u, dgx_set_pixel(ink, x, y, (seed >> 16) & 255);
        differ = 0;
        for (int k = 0; k < 3; ++k)
            for (uint32_t flags = 0; flags < 4; ++flags) {
                dgx_texture_quad_t q = {.texture = ink, .tx = 2, .ty = 1, .tw = 19, .th = 15, .lut = inks, .alpha = opacity, .paper = 0x1234,
                                        .flags = flags | DGX_TEXTURE_PAPER};
                for (int i = 0; i < 4; ++i) q.x[i] = quads[k][2 * i] * 2 / 3, q.y[i] = quads[k][2 * i + 1] * 2 / 3;
                dgx_fill_rectangle(m, 0, 0, 64, 64, 0x1234);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) glass[y][x] = 0x1234;
                CHECK(dgx_draw_texture_quad_with(m, &q) == ESP_OK && dgx_draw_texture_quad_with(lcd, &q) == ESP_OK);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) differ += px(m, x, y) != glass[y][x];
            }
        CHECK(differ == 0);
        /*
         * An opacity for every texel, alpha_map: on a 16-bit texture and on an
         * 8-bit one with opacities of its own, the two multiplied; a virtual
         * screen and the display behind a bus give the same pixels with paper.
         */
        dgx_screen_t *map = dgx_vscreen_init(23, 17, 8, DgxScreenRGB);
        for (int y = 0; y < 17; ++y)
            for (int x = 0; x < 23; ++x) seed = seed * 1103515245u + 12345u, dgx_set_pixel(map, x, y, x < 4 ? 0 : x > 18 ? 255 : (seed >> 16) & 255);
        differ = 0, drawn = 0;
        for (int k = 0; k < 3; ++k)
            for (uint32_t flags = 0; flags < 8; ++flags) {
                dgx_texture_quad_t q = {.texture = flags & 4 ? ink : noise, .tx = 2, .ty = 1, .tw = 19, .th = 15, .lut = inks, .alpha = opacity,
                                        .alpha_map = map, .paper = 0x1234, .flags = (flags & 3) | DGX_TEXTURE_PAPER};
                for (int i = 0; i < 4; ++i) q.x[i] = quads[k][2 * i] * 2 / 3, q.y[i] = quads[k][2 * i + 1] * 2 / 3;
                dgx_fill_rectangle(m, 0, 0, 64, 64, 0x1234);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) glass[y][x] = 0x1234;
                CHECK(dgx_draw_texture_quad_with(m, &q) == ESP_OK && dgx_draw_texture_quad_with(lcd, &q) == ESP_OK);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) differ += px(m, x, y) != glass[y][x], drawn += glass[y][x] != 0x1234;
            }
        CHECK(differ == 0 && drawn > 24 * 400);
        /* a map of 255 is no map; a map of 0 draws nothing; a map of 128 over an opacity of 255 is an opacity of 128 */
        {
            dgx_screen_t      *full = dgx_vscreen_init(23, 17, 8, DgxScreenRGB);
            dgx_texture_quad_t q    = {.texture = noise, .tx = 2, .ty = 1, .tw = 19, .th = 15, .flags = DGX_TEXTURE_BILINEAR | DGX_TEXTURE_SOFT};
            for (int i = 0; i < 4; ++i) q.x[i] = quads[0][2 * i] * 2 / 3, q.y[i] = quads[0][2 * i + 1] * 2 / 3;
            dgx_fill_rectangle(full, 0, 0, 23, 17, 255);
            dgx_fill_rectangle(m, 0, 0, 64, 64, 0x1234);
            CHECK(dgx_draw_texture_quad_with(m, &q) == ESP_OK);
            dgx_screen_t *plain = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
            dgx_fill_rectangle(plain, 0, 0, 64, 64, 0x1234);
            q.alpha_map = full;
            CHECK(dgx_draw_texture_quad_with(plain, &q) == ESP_OK);
            int same = 0, none = 0;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) same += px(m, x, y) == px(plain, x, y);
            dgx_fill_rectangle(full, 0, 0, 23, 17, 0);
            dgx_fill_rectangle(plain, 0, 0, 64, 64, 0x1234);
            CHECK(dgx_draw_texture_quad_with(plain, &q) == ESP_OK);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) none += px(plain, x, y) == 0x1234;
            CHECK(same == 64 * 64 && none == 64 * 64);
            /* an index of opacity 128 and a map of 255, an index of 255 and a map of 128: the same */
            static uint8_t half[256], whole[256];
            memset(half, 128, sizeof(half)), memset(whole, 255, sizeof(whole));
            dgx_texture_quad_t h = {.texture = ink, .tx = 2, .ty = 1, .tw = 19, .th = 15, .lut = inks, .alpha = half, .alpha_map = full};
            for (int i = 0; i < 4; ++i) h.x[i] = quads[0][2 * i] * 2 / 3, h.y[i] = quads[0][2 * i + 1] * 2 / 3;
            dgx_fill_rectangle(full, 0, 0, 23, 17, 255);
            dgx_fill_rectangle(m, 0, 0, 64, 64, 0x1234);
            CHECK(dgx_draw_texture_quad_with(m, &h) == ESP_OK);
            dgx_fill_rectangle(full, 0, 0, 23, 17, 128);
            h.alpha = whole;
            dgx_fill_rectangle(plain, 0, 0, 64, 64, 0x1234);
            CHECK(dgx_draw_texture_quad_with(plain, &h) == ESP_OK);
            same = 0, none = 0;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) same += px(m, x, y) == px(plain, x, y), none += px(m, x, y) == 0x1234;
            CHECK(same == 64 * 64 && none < 64 * 64 - 300);
            /* both: an index of 128 and a map of 128 are an index of 64 */
            static uint8_t quarter[256];
            memset(quarter, 64, sizeof(quarter));
            h.alpha = half;
            dgx_fill_rectangle(m, 0, 0, 64, 64, 0x1234);
            CHECK(dgx_draw_texture_quad_with(m, &h) == ESP_OK);
            dgx_fill_rectangle(full, 0, 0, 23, 17, 255);
            h.alpha = quarter;
            dgx_fill_rectangle(plain, 0, 0, 64, 64, 0x1234);
            CHECK(dgx_draw_texture_quad_with(plain, &h) == ESP_OK);
            same = 0;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) same += px(m, x, y) == px(plain, x, y);
            CHECK(same == 64 * 64);
            /* a map of another depth, or smaller than the texture */
            dgx_screen_t *wide = dgx_vscreen_init(23, 17, 16, DgxScreenRGB), *small = dgx_vscreen_init(22, 17, 8, DgxScreenRGB);
            h.alpha_map = wide;
            CHECK(dgx_draw_texture_quad_with(m, &h) == ESP_ERR_NOT_SUPPORTED);
            h.alpha_map = small;
            CHECK(dgx_draw_texture_quad_with(m, &h) == ESP_ERR_INVALID_ARG);
            dgx_screen_destroy(&wide), dgx_screen_destroy(&small), dgx_screen_destroy(&plain), dgx_screen_destroy(&full);
        }
        dgx_screen_destroy(&map);
        /*
         * Moved by whole pixels the quad is the same pixels, moved: drawn
         * whole, drawn into strips of 16 rows each with the quad at its place
         * in the strip, drawn 7 pixels right and 5 up, and drawn under a clip
         * that cuts every row and column of it. Every way of sampling and of
         * edges, in perspective and turned, the 8-bit texture and the 16-bit.
         */
        dgx_screen_t *whole = dgx_vscreen_init(96, 96, 16, DgxScreenRGB), *moved = dgx_vscreen_init(96, 96, 16, DgxScreenRGB);
        dgx_screen_t *strip = dgx_vscreen_init(96, 16, 16, DgxScreenRGB);
        differ = 0, drawn = 0;
        for (int k = 0; k < 3; ++k)
            for (uint32_t flags = 0; flags < 8; ++flags) {
                dgx_texture_quad_t q = {.tx = 2, .ty = 1, .tw = 19, .th = 15, .lut = inks, .alpha = opacity, .flags = flags & 3};
                q.texture            = flags & 4 ? ink : noise;
                for (int i = 0; i < 4; ++i) q.x[i] = quads[k][2 * i] + 5 * 16 + 3, q.y[i] = quads[k][2 * i + 1] + 7 * 16 + 5;
                dgx_fill_rectangle(whole, 0, 0, 96, 96, 0x1234);
                CHECK(dgx_draw_texture_quad_with(whole, &q) == ESP_OK);
                for (int y = 0; y < 96; ++y)
                    for (int x = 0; x < 96; ++x) drawn += px(whole, x, y) != 0x1234;
                for (int r = 0; r < 96; r += 16) {
                    dgx_texture_quad_t part = q;
                    for (int i = 0; i < 4; ++i) part.y[i] -= r * 16;
                    dgx_fill_rectangle(strip, 0, 0, 96, 16, 0x1234);
                    CHECK(dgx_draw_texture_quad_with(strip, &part) == ESP_OK);
                    for (int y = 0; y < 16; ++y)
                        for (int x = 0; x < 96; ++x) differ += px(strip, x, y) != px(whole, x, r + y);
                }
                dgx_texture_quad_t far = q;
                for (int i = 0; i < 4; ++i) far.x[i] += 7 * 16, far.y[i] -= 5 * 16;
                dgx_fill_rectangle(moved, 0, 0, 96, 96, 0x1234);
                CHECK(dgx_draw_texture_quad_with(moved, &far) == ESP_OK);
                for (int y = 0; y < 96; ++y)
                    for (int x = 0; x < 96; ++x)
                        if (x + 7 < 96 && y - 5 >= 0) differ += px(moved, x + 7, y - 5) != px(whole, x, y);
                dgx_fill_rectangle(moved, 0, 0, 96, 96, 0x1234);
                dgx_screen_clip(moved, 37, 41, 23, 19);
                CHECK(dgx_draw_texture_quad_with(moved, &q) == ESP_OK);
                dgx_screen_unclip(moved);
                for (int y = 0; y < 96; ++y)
                    for (int x = 0; x < 96; ++x) {
                        const bool in = x >= 37 && x < 60 && y >= 41 && y < 60;
                        differ += px(moved, x, y) != (in ? px(whole, x, y) : 0x1234);
                    }
            }
        CHECK(differ == 0 && drawn > 24 * 1500);
        dgx_screen_destroy(&whole), dgx_screen_destroy(&moved), dgx_screen_destroy(&strip);
        dgx_screen_destroy(&ink);
        dgx_screen_destroy(&m);
        dgx_screen_destroy(&noise), dgx_screen_destroy(&a), dgx_screen_destroy(&b);
    }

    /*
     * A soft edge covers a pixel by the share of its area inside the quad, at
     * its corners too: white on black, the weight of every pixel against the
     * share counted by 32 x 32 points of it tested against the quad. A square
     * with corners a quarter of a pixel off the grid, a turned rectangle, a
     * thin parallelogram with sharp tips, a quad in perspective.
     */
    {
        float quads[4][8] = {
            {10.25f, 10.25f, 30.25f, 10.25f, 30.25f, 30.25f, 10.25f, 30.25f},
            {0},
            {4.2f, 58.3f, 20.1f, 58.3f, 60.3f, 52.1f, 44.4f, 52.1f},
            {12.3f, 8.6f, 50.2f, 14.1f, 46.7f, 40.9f, 9.1f, 33.3f},
        };
        static const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
        for (int i = 0; i < 4; ++i) {
            const float c = 0.8660254f, s = 0.5f, u = sx[i] * 15.0f, v = sy[i] * 7.0f;
            quads[1][2 * i] = 32.3f + u * c - v * s, quads[1][2 * i + 1] = 31.7f + u * s + v * c;
        }
        int   bad = 0, edge = 0;
        float worst = 0;
        for (int k = 0; k < 4; ++k) {
            dgx_texture_quad_t c = {.texture = white, .tw = 4, .th = 4, .flags = DGX_TEXTURE_SOFT};
            float              X[4], Y[4];
            for (int i = 0; i < 4; ++i) {
                c.x[i] = (int)lroundf(quads[k][2 * i] * 16), c.y[i] = (int)lroundf(quads[k][2 * i + 1] * 16);
                X[i] = (float)c.x[i] / 16, Y[i] = (float)c.y[i] / 16;
            }
            dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
            CHECK(dgx_draw_texture_quad_with(scr, &c) == ESP_OK);
            float sign = 0;
            for (int e = 0; e < 4; ++e) sign += X[e] * Y[(e + 1) & 3] - X[(e + 1) & 3] * Y[e];
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    /* a pixel whose corners are all inside is covered whole, one with all of them outside a side not at all */
                    int whole = 1, none = 0;
                    for (int e = 0; e < 4; ++e) {
                        const int f   = (e + 1) & 3;
                        int       out = 0;
                        for (int k2 = 0; k2 < 4; ++k2) {
                            const float qx = (float)(x + (k2 & 1)), qy = (float)(y + (k2 >> 1));
                            out += ((X[f] - X[e]) * (qy - Y[e]) - (Y[f] - Y[e]) * (qx - X[e])) * sign < 0;
                        }
                        whole &= out == 0, none |= out == 4;
                    }
                    int in = whole ? 1024 : 0;
                    if (!whole && !none)
                    for (int j = 0; j < 32; ++j)
                        for (int i = 0; i < 32; ++i) {
                            const float qx = (float)x + ((float)i + 0.5f) / 32, qy = (float)y + ((float)j + 0.5f) / 32;
                            int         pos = 0, neg = 0;
                            for (int e = 0; e < 4; ++e) {
                                const int   f     = (e + 1) & 3;
                                const float cross = (X[f] - X[e]) * (qy - Y[e]) - (Y[f] - Y[e]) * (qx - X[e]);
                                pos += cross > 0, neg += cross < 0;
                            }
                            in += !pos || !neg;
                        }
                    const uint16_t got = px(scr, x, y);
                    int            w   = -1;
                    for (int m = 0; m <= 32 && w < 0; ++m)
                        if (dgx_alpha_mix16(BLACK, WHITE, (uint32_t)m) == got) w = m;
                    const float err = w < 0 ? 1.0f : fabsf((float)w / 32 - (float)in / 1024);
                    if (err > worst) worst = err;
                    bad += err > 1.5f / 32;
                    edge += in > 0 && in < 1024;
                }
        }
        if (bad) printf("soft edges by area: %d pixels off by more than 1.5/32, the worst by %.3f\n", bad, worst);
        CHECK(bad == 0 && edge > 300);
    }

    /*
     * A quad that is not convex is refused by both calls, and nothing is
     * drawn: one whose sides cross, one with a corner bent inward. A corner
     * given twice, a triangle, has no projective map: the quad of corners
     * between pixels draws nothing and says nothing, as for a quad of no
     * area; the quad of whole pixels, mapped as two triangles, draws it,
     * whichever corner it is.
     */
    {
        static const int16_t shapes[6][8] = {
            {10, 10, 40, 40, 40, 10, 10, 40}, /* sides crossing */
            {10, 10, 40, 10, 20, 20, 10, 40}, /* a corner bent inward */
            {10, 10, 10, 10, 40, 10, 10, 40}, /* a triangle, corner 0 twice */
            {10, 10, 40, 10, 40, 10, 10, 40}, /* corner 1 twice */
            {10, 10, 40, 10, 10, 40, 10, 40}, /* corner 2 twice */
            {10, 10, 40, 10, 10, 40, 10, 10}, /* corner 3 twice */
        };
        for (int k = 0; k < 6; ++k) {
            dgx_texture_quad_t c = {.texture = white, .tw = 4, .th = 4};
            dgx_point_2d_t     p[4];
            for (int i = 0; i < 4; ++i) {
                c.x[i] = shapes[k][2 * i] * DGX_SUBPIXEL, c.y[i] = shapes[k][2 * i + 1] * DGX_SUBPIXEL;
                p[i] = (dgx_point_2d_t){shapes[k][2 * i], shapes[k][2 * i + 1]};
            }
            const esp_err_t want = k < 2 ? ESP_ERR_INVALID_ARG : ESP_OK;
            dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
            CHECK(dgx_draw_texture_quad_with(scr, &c) == want);
            CHECK(px(scr, 15, 15) == BLACK && px(scr, 30, 12) == BLACK);
            dgx_fill_rectangle(scr, 0, 0, 64, 64, BLACK);
            CHECK(dgx_draw_texture_quad(scr, p, white, 0, 0, 4, 4) == want);
            CHECK(px(scr, 15, 15) == (k < 2 ? BLACK : WHITE) && px(scr, 30, 12) == (k < 2 ? BLACK : WHITE));
        }
    }

    dgx_screen_destroy(&lcd);
    dgx_screen_destroy(&s18);
    dgx_screen_destroy(&idx);
    dgx_screen_destroy(&white);
    dgx_screen_destroy(&scr);
    dgx_screen_destroy(&tex);
    CHECK_DONE();
}
