/*
 * dgx_image_blit() by colors: pictures of 16 bits with a plane of opacity, pictures of 1, 4 or 8 bits through a table
 * of colors, and the coverage of one color with DGX_IMAGE_SOFT. On a virtual screen, which can be read, a pixel partly
 * covered is mixed with what lies under it, rounded, as soft text mixes; with DGX_IMAGE_OPAQUE with bg, every pixel
 * of the box written once, on a display behind a bus too, where without it a pixel is put whole from half on. A
 * picture drawn by a table of colors lands as the same picture in one color does, in every turn and at every scale;
 * a picture of 16 bits on a screen of 24 bits takes its colors converted.
 */
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h" /* the waits of the init table, for nothing on the host */
#include "drivers/dgx_lcd_init.c"
#include "drivers/panel.c"

#include "dgx_draw.h"
#include "dgx_image.h"
#include "dgx_vscreen.h"
#include "dgx_mix_priv.h"

/* ---- a display behind a bus: a 64x64 memory written through windows, every pixel written counted ---- */

static uint8_t  command, params[8], part[2];
static int      params_n, part_n;
static uint16_t sc, ec, sp, ep, column, page, glass[64][64];
static int      writes[64][64];

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
                if (column < 64 && page < 64) glass[page][column] = (uint16_t)(part[0] << 8 | part[1]), ++writes[page][column];
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

static void bus_sync(struct _dgx_bus_protocols_t *bus) { (void)bus; }

static const dgx_lcd_init_cmd_t no_init[] = {
    {0, 0, 0xff, 0, {0}}
};
static const dgx_panel_t panel64 = {.name = "64", .width = 64, .height = 64, .colmod = {0, 0x55}, .init = no_init};

#define RED   0xF800u
#define BLUE  0x001Fu
#define GRAY  0x8410u
#define WHITE 0xFFFFu

static uint16_t px(dgx_screen_t *s, int x, int y) { return (uint16_t)dgx_get_pixel(s, x, y); }

int main(void)
{
    dgx_screen_t *scr = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);

    /* the coverage of one color, 4 bits: 0 leaves the screen, 15 the color whole, between mixed with what lies under */
    static const uint8_t cover4[] = {0x08, 0xF4}; /* 2x2: 0 8 / 15 4 */
    const dgx_image_t    mask     = {.width = 2, .height = 2, .format = DGX_IMAGE_4BPP, .data = cover4};
    dgx_fill_rectangle(scr, 0, 0, 64, 64, GRAY);
    CHECK(dgx_image_blit(scr, 0, 0, &mask, &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT, .color = RED}) == ESP_OK);
    CHECK(px(scr, 0, 0) == GRAY && px(scr, 0, 1) == RED);
    CHECK(px(scr, 1, 0) == dgx_mix_color(16, GRAY, RED, 8 * 17) && px(scr, 1, 1) == dgx_mix_color(16, GRAY, RED, 4 * 17));
    /* with the paper: the box whole, mixed with bg */
    CHECK(dgx_image_blit(scr, 4, 0, &mask, &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT | DGX_IMAGE_OPAQUE, .color = RED, .bg = BLUE}) ==
          ESP_OK);
    CHECK(px(scr, 4, 0) == BLUE && px(scr, 4, 1) == RED && px(scr, 5, 0) == dgx_mix_color(16, BLUE, RED, 136));
    /* without SOFT a silhouette of 4 bits stays as it was: every pixel not 0 the color whole */
    dgx_fill_rectangle(scr, 0, 0, 64, 64, GRAY);
    CHECK(dgx_image_blit(scr, 0, 0, &mask, &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE, .color = RED}) == ESP_OK);
    CHECK(px(scr, 0, 0) == GRAY && px(scr, 1, 0) == RED && px(scr, 1, 1) == RED);
    /* 8 bits of coverage */
    static const uint8_t cover8[] = {0, 128, 255, 1};
    const dgx_image_t    mask8    = {.width = 4, .height = 1, .format = DGX_IMAGE_8BPP, .data = cover8};
    dgx_fill_rectangle(scr, 0, 0, 64, 64, GRAY);
    CHECK(dgx_image_blit(scr, 0, 0, &mask8, &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT, .color = WHITE}) == ESP_OK);
    CHECK(px(scr, 0, 0) == GRAY && px(scr, 1, 0) == dgx_mix_color(16, GRAY, WHITE, 128) && px(scr, 2, 0) == WHITE &&
          px(scr, 3, 0) == dgx_mix_color(16, GRAY, WHITE, 1));

    /* a picture of 16 bits with a plane: soft, and hard from half on without SOFT */
    static const uint8_t pic16[] = {0xF8, 0x00, 0x00, 0x1F, 0xFF, 0xFF}; /* red, blue, white */
    static const uint8_t plane[] = {255, 100, 200};
    const dgx_image_t    rgb     = {.width = 3, .height = 1, .format = DGX_IMAGE_16BPP, .data = pic16, .alpha = plane};
    dgx_fill_rectangle(scr, 0, 0, 64, 64, GRAY);
    CHECK(dgx_image_blit(scr, 0, 0, &rgb, &(dgx_image_blit_t){.flags = DGX_IMAGE_SOFT}) == ESP_OK);
    CHECK(px(scr, 0, 0) == RED && px(scr, 1, 0) == dgx_mix_color(16, GRAY, BLUE, 100) && px(scr, 2, 0) == dgx_mix_color(16, GRAY, WHITE, 200));
    dgx_fill_rectangle(scr, 0, 0, 64, 64, GRAY);
    CHECK(dgx_image_blit(scr, 0, 0, &rgb, NULL) == ESP_OK);
    CHECK(px(scr, 0, 0) == RED && px(scr, 1, 0) == GRAY && px(scr, 2, 0) == WHITE);
    const dgx_image_t solid = {.width = 3, .height = 1, .format = DGX_IMAGE_16BPP, .data = pic16};
    CHECK(dgx_image_blit(scr, 0, 1, &solid, NULL) == ESP_OK);
    CHECK(px(scr, 0, 1) == RED && px(scr, 1, 1) == BLUE && px(scr, 2, 1) == WHITE);

    /* a picture of 16 bits on a screen of 24 bits: its colors converted */
    dgx_screen_t *s24 = dgx_vscreen_init(8, 8, 24, DgxScreenRGB);
    CHECK(dgx_image_blit(s24, 0, 0, &solid, NULL) == ESP_OK);
    CHECK(dgx_get_pixel(s24, 0, 0) == dgx_rgb_to_24(0xF8, 0, 0) && dgx_get_pixel(s24, 2, 0) == dgx_rgb_to_24(0xF8, 0xFC, 0xF8));

    /* values through a table of colors and opacities: on any screen, 0 transparent when alpha says so */
    static const uint8_t vals[] = {0x01, 0x23}; /* 2x2 of 4 bits: 0 1 / 2 3 */
    const dgx_image_t    four   = {.width = 2, .height = 2, .format = DGX_IMAGE_4BPP, .data = vals};
    uint32_t             colors[16] = {0, RED, BLUE, WHITE};
    uint8_t              alpha[16]  = {0, 255, 255, 64};
    dgx_fill_rectangle(scr, 0, 0, 64, 64, GRAY);
    CHECK(dgx_image_blit(scr, 0, 0, &four, &(dgx_image_blit_t){.colors = colors, .alpha = alpha, .flags = DGX_IMAGE_SOFT}) == ESP_OK);
    CHECK(px(scr, 0, 0) == GRAY && px(scr, 1, 0) == RED && px(scr, 0, 1) == BLUE && px(scr, 1, 1) == dgx_mix_color(16, GRAY, WHITE, 64));
    CHECK(dgx_image_blit(s24, 4, 4, &four, &(dgx_image_blit_t){.colors = (uint32_t[16]){0, 0x123456, 0x654321, 0xabcdef}}) == ESP_OK);
    CHECK(dgx_get_pixel(s24, 5, 4) == 0x123456 && dgx_get_pixel(s24, 4, 5) == 0x654321 && dgx_get_pixel(s24, 5, 5) == 0xabcdef);

    /* a 1-bit picture through a table lands where the same picture in one color does: every turn, scale, clip */
    {
        static const uint8_t bits[] = {0xE0, 0x40, 0x60, 0x80, 0x20}; /* 3x5, an uneven shape */
        const dgx_image_t    one    = {.width = 3, .height = 5, .origin_x = 1, .origin_y = 3, .format = DGX_IMAGE_1BPP, .data = bits};
        dgx_screen_t        *ref    = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
        const uint32_t       two[2] = {0, RED};
        for (int o = 0; o <= (int)DgxOutputTransverse; ++o)
            for (int scale = 1; scale <= 3; ++scale)
                for (int clip = 0; clip < 2; ++clip) {
                    dgx_fill_rectangle(scr, 0, 0, 64, 64, GRAY);
                    dgx_fill_rectangle(ref, 0, 0, 64, 64, GRAY);
                    const dgx_image_blit_t a = {.color = RED, .orientation = (dgx_output_orientation_t)o, .scale = (uint8_t)scale,
                                                .flags = clip ? DGX_IMAGE_CLIP : 0u, .clip_x = 20, .clip_y = 19, .clip_w = 5, .clip_h = 6};
                    dgx_image_blit_t b = a;
                    b.colors           = two;
                    CHECK(dgx_image_blit(ref, 21, 22, &one, &a) == ESP_OK && dgx_image_blit(scr, 21, 22, &one, &b) == ESP_OK);
                    int same = 1;
                    for (int y = 0; y < 64; ++y)
                        for (int x = 0; x < 64; ++x) same &= px(scr, x, y) == px(ref, x, y);
                    CHECK(same);
                }
        dgx_screen_destroy(&ref);
    }

    /* a display behind a bus, which cannot be read: hard from half on, or with the paper every pixel once */
    {
        static uint8_t      buffer[4096];
        dgx_bus_protocols_t bus = {.bus_type = DGX_BUS_SPI, .write_command = bus_command, .write_data = bus_data, .write_data_async = bus_data,
                                   .sync_write = bus_sync, .buffer = buffer, .buffer_len = sizeof(buffer)};
        dgx_screen_t       *lcd = dgx_panel_init(&bus, &panel64, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB);
        CHECK(lcd && !dgx_screen_is_readable(lcd));
        dgx_fill_rectangle(lcd, 0, 0, 64, 64, GRAY);
        CHECK(dgx_image_blit(lcd, 10, 10, &mask8, &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT, .color = WHITE}) == ESP_OK);
        CHECK(glass[10][10] == GRAY && glass[10][11] == WHITE && glass[10][12] == WHITE && glass[10][13] == GRAY);
        memset(writes, 0, sizeof writes);
        CHECK(dgx_image_blit(lcd, 10, 20, &mask8,
                             &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT | DGX_IMAGE_OPAQUE, .color = WHITE, .bg = BLUE, .scale = 3}) ==
              ESP_OK);
        int once = 1, outside = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const int in = x >= 10 && x < 22 && y >= 20 && y < 23;
                if (in) once &= writes[y][x] == 1;
                else outside += writes[y][x];
            }
        CHECK(once && outside == 0);
        CHECK(glass[20][10] == BLUE && glass[22][14] == dgx_mix_color(16, BLUE, WHITE, 128) && glass[21][17] == WHITE &&
              glass[20][20] == dgx_mix_color(16, BLUE, WHITE, 1));
        dgx_screen_destroy(&lcd);
    }

    dgx_screen_destroy(&s24);
    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
