/*
 * dgx_image_blit() by colors: pictures of 16 bits with a plane of opacity, pictures of 1, 4 or 8 bits through a table
 * of colors, and the coverage of one color with DGX_IMAGE_SOFT. On a virtual screen, which can be read, a pixel partly
 * covered is mixed with what lies under it, rounded, as soft text mixes; with DGX_IMAGE_OPAQUE with bg, every pixel
 * of the box written once and in one window, on a display behind a bus too, where without it a pixel is put whole
 * from half on; with DGX_IMAGE_BG a pixel partly covered is mixed with bg and one not covered left as it is. A
 * picture drawn by a table of colors lands as the same picture in one color does, in every turn and at every scale;
 * a picture of 16 bits on a screen of 24 bits takes its colors widened by their high bits, white staying white.
 */
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h" /* the waits of the init table, for nothing on the host */
#include "drivers/dgx_lcd_init.c"
#include "drivers/panel.c"

#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_image.h"
#include "dgx_vscreen.h"
#include "dgx_mix_priv.h"

/* ---- a display behind a bus: a 64x64 memory written through windows, every pixel written counted ---- */

static uint8_t  command, params[8], part[3];
static int      params_n, part_n, windows, pixel_bytes = 2; /* 3 for a panel of 18 bits */
static uint16_t sc, ec, sp, ep, column, page;
static uint32_t glass[64][64];
static int      writes[64][64];

/*
 * Pixels sent without waiting are taken off the buffer later, as by DMA: when the next command, data or wait comes.
 * A buffer written over before that shows on the glass.
 */
static const uint8_t *pending;
static uint32_t       pending_bits;
static void           bus_data_now(const uint8_t *data, uint32_t bits);

static void bus_flush(void)
{
    if (!pending) return;
    const uint8_t *data = pending;
    pending             = NULL;
    bus_data_now(data, pending_bits);
}

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value)
{
    (void)bus;
    bus_flush();
    command = value, params_n = 0, part_n = 0;
    if (value == 0x2A) ++windows;
    if (value == 0x2C) column = sc, page = sp;
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    bus_flush();
    bus_data_now(data, bits);
}

static void bus_data_async(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    bus_flush();
    pending = data, pending_bits = bits;
}

static void bus_data_now(const uint8_t *data, uint32_t bits)
{
    for (uint32_t i = 0; i < bits / 8; ++i) {
        if (command == 0x2C) {
            part[part_n++] = data[i];
            if (part_n == pixel_bytes) {
                const uint32_t v = pixel_bytes == 3 ? (uint32_t)part[0] << 16 | (uint32_t)part[1] << 8 | part[2] : (uint32_t)part[0] << 8 | part[1];
                if (column < 64 && page < 64) glass[page][column] = v, ++writes[page][column];
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
    bus_flush();
}

static const dgx_lcd_init_cmd_t no_init[] = {
    {0, 0, 0xff, 0, {0}}
};
static const dgx_panel_t panel64 = {.name = "64", .width = 64, .height = 64, .colmod = {0, 0x55, 0x66}, .init = no_init};

#define RED   0xF800u
#define BLUE  0x001Fu
#define GRAY  0x8410u
#define WHITE 0xFFFFu

static uint16_t px(dgx_screen_t *s, int x, int y) { return (uint16_t)dgx_get_pixel(s, x, y); }

/* drawing on a display behind a bus, and its last pixels off the wire before the glass is looked at */
static esp_err_t blit_now(dgx_screen_t *s, int x, int y, const dgx_image_t *image, const dgx_image_blit_t *how)
{
    const esp_err_t e = dgx_image_blit(s, x, y, image, how);
    bus_flush();
    return e;
}

static void fill_now(dgx_screen_t *s, int x, int y, int w, int h, uint32_t color)
{
    dgx_fill_rectangle(s, x, y, w, h, color);
    bus_flush();
}

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
    CHECK(dgx_get_pixel(s24, 0, 0) == dgx_rgb_to_24(0xFF, 0, 0) && dgx_get_pixel(s24, 1, 0) == dgx_rgb_to_24(0, 0, 0xFF) &&
          dgx_get_pixel(s24, 2, 0) == dgx_rgb_to_24(0xFF, 0xFF, 0xFF));
    /* each channel widened by its high bits again: a gray of 16, 32 and 16 levels is 132, 130 and 132 */
    static const uint8_t mid16[] = {0x84, 0x10};
    const dgx_image_t    mid     = {.width = 1, .height = 1, .format = DGX_IMAGE_16BPP, .data = mid16};
    CHECK(dgx_image_blit(s24, 0, 2, &mid, NULL) == ESP_OK && dgx_get_pixel(s24, 0, 2) == dgx_rgb_to_24(132, 130, 132));

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

    /* a virtual screen of every depth, of an odd width: the pixels written in its memory, the pixels near them kept */
    {
        static const uint8_t vals7[] = {0x12, 0x30, 0x40, 0x56, 0x07, 0x00}; /* 3x3 of 4 bits: 1 2 3 / 0 4 0 / 5 6 7 */
        const dgx_image_t    pic     = {.width = 3, .height = 3, .format = DGX_IMAGE_4BPP, .data = vals7};
        static const uint8_t depths[] = {1, 4, 8, 12, 16, 18, 24, 32};
        for (unsigned d = 0; d < sizeof depths; ++d) {
            const uint8_t  b    = depths[d];
            const uint32_t back = b == 1 ? 1u : b == 4 ? 0x50u : b == 8 ? 0x5Au : b == 12 ? 0x5A50u : b == 16 ? 0x5A5Au : 0x5A5A5Au;
            uint32_t       tab[16];
            for (unsigned i = 0; i < 16; ++i)
                tab[i] = b == 1 ? i & 1u : b == 4 ? (i << 4) & 0xF0u : b == 8 ? i * 17u : b == 12 ? (i * 0x1110u) & 0xFFF0u : b == 16 ? i * 0x1111u
                                                                                                                            : i * 0x111111u;
            for (int x0 = 0; x0 < 2; ++x0) {
                dgx_screen_t *v = dgx_vscreen_init(7, 5, b, DgxScreenRGB), *ref = dgx_vscreen_init(7, 5, b, DgxScreenRGB);
                dgx_fill_rectangle(v, 0, 0, 7, 5, back);
                dgx_fill_rectangle(ref, 0, 0, 7, 5, back);
                CHECK(dgx_image_blit(v, 1 + x0, 1, &pic, &(dgx_image_blit_t){.colors = tab, .alpha = (uint8_t[16]){0, 255, 255, 255, 255, 255, 255, 255}}) ==
                      ESP_OK);
                for (int y = 0; y < 3; ++y)
                    for (int x = 0; x < 3; ++x) {
                        const unsigned i = (unsigned)(vals7[y * 2 + x / 2] >> (x & 1 ? 0 : 4)) & 15u;
                        if (i) dgx_set_pixel(ref, 1 + x0 + x, 1 + y, tab[i]);
                    }
                int same = 1;
                for (int y = 0; y < 5; ++y)
                    for (int x = 0; x < 7; ++x) same &= dgx_get_pixel(v, x, y) == dgx_get_pixel(ref, x, y);
                CHECK(same);
                dgx_screen_destroy(&v);
                dgx_screen_destroy(&ref);
            }
        }
    }

    /* a monochrome screen in pages, which takes its rows through windows: a row of a length not of whole bytes lands
       as on a virtual screen of 1 bit, its window not run over into the next row */
    {
        uint8_t cover[12 * 10];
        for (int i = 0; i < 12 * 10; ++i) cover[i] = (uint8_t)((i * 37 + i / 12 * 91) & 0xFF);
        const dgx_image_t m   = {.width = 12, .height = 10, .format = DGX_IMAGE_8BPP, .data = cover};
        dgx_screen_t     *bw  = dgx_bw_init(32, 16);
        dgx_screen_t     *ref = dgx_vscreen_init(32, 16, 1, DgxScreenRGB);
        CHECK(bw && ref && !dgx_vscreen_is_linear(bw));
        for (int opaque = 0; opaque < 2; ++opaque)
            for (int x = 0; x <= 8; x += 2) {
                dgx_fill_rectangle(bw, 0, 0, 32, 16, 0);
                dgx_fill_rectangle(ref, 0, 0, 32, 16, 0);
                const dgx_image_blit_t how = {.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT | (opaque ? DGX_IMAGE_OPAQUE : 0u), .color = 1, .bg = 0};
                CHECK(dgx_image_blit(bw, x, 3, &m, &how) == ESP_OK && dgx_image_blit(ref, x, 3, &m, &how) == ESP_OK);
                int same = 1;
                for (int yy = 0; yy < 16; ++yy)
                    for (int xx = 0; xx < 32; ++xx) same &= dgx_get_pixel(bw, xx, yy) == dgx_get_pixel(ref, xx, yy);
                CHECK(same);
            }
        dgx_screen_destroy(&bw);
        dgx_screen_destroy(&ref);
    }

    /* DGX_IMAGE_BG on a screen that can be read: the edges mixed with bg, not with what lies under */
    dgx_fill_rectangle(scr, 0, 0, 64, 64, GRAY);
    CHECK(dgx_image_blit(scr, 0, 0, &mask8, &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT | DGX_IMAGE_BG, .color = WHITE, .bg = BLUE}) ==
          ESP_OK);
    CHECK(px(scr, 0, 0) == GRAY && px(scr, 1, 0) == dgx_mix_color(16, BLUE, WHITE, 128) && px(scr, 2, 0) == WHITE &&
          px(scr, 3, 0) == dgx_mix_color(16, BLUE, WHITE, 1));

    /* a display behind a bus, which cannot be read: hard from half on, or with the paper every pixel once */
    {
        static uint8_t      buffer[4096];
        dgx_bus_protocols_t bus = {.bus_type = DGX_BUS_SPI, .write_command = bus_command, .write_data = bus_data, .write_data_async = bus_data_async,
                                   .sync_write = bus_sync, .buffer = buffer, .buffer_len = sizeof(buffer)};
        dgx_screen_t       *lcd = dgx_panel_init(&bus, &panel64, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB);
        CHECK(lcd && !dgx_screen_is_readable(lcd));
        fill_now(lcd, 0, 0, 64, 64, GRAY);
        CHECK(blit_now(lcd, 10, 10, &mask8, &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT, .color = WHITE}) == ESP_OK);
        CHECK(glass[10][10] == GRAY && glass[10][11] == WHITE && glass[10][12] == WHITE && glass[10][13] == GRAY);
        memset(writes, 0, sizeof writes);
        windows = 0;
        CHECK(blit_now(lcd, 10, 20, &mask8,
                             &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT | DGX_IMAGE_OPAQUE, .color = WHITE, .bg = BLUE, .scale = 3}) ==
              ESP_OK);
        int once = 1, outside = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const int in = x >= 10 && x < 22 && y >= 20 && y < 23;
                if (in) once &= writes[y][x] == 1;
                else outside += writes[y][x];
            }
        CHECK(once && outside == 0 && windows == 1);
        CHECK(glass[20][10] == BLUE && glass[22][14] == dgx_mix_color(16, BLUE, WHITE, 128) && glass[21][17] == WHITE &&
              glass[20][20] == dgx_mix_color(16, BLUE, WHITE, 1));
        /* the same by a table of colors, turned and cut by a clip, at every scale, in every turn: with DGX_IMAGE_OPAQUE
           one window, every pixel of what is left once and as a virtual screen has it, through the halves of a buffer
           of two rows and more or through a buffer of one; with DGX_IMAGE_BG by runs, the pixels not covered not
           written and the others as on a virtual screen, where BG mixes with bg too */
        dgx_bus_protocols_t one    = bus;
        static uint8_t      row[20];
        one.buffer = row, one.buffer_len = sizeof row; /* a row of the box, 8 pixels of 16 bits, and not two */
        dgx_screen_t       *narrow = dgx_panel_init(&one, &panel64, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB);
        dgx_screen_t       *ref    = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
        CHECK(narrow && ref);
        for (int screen = 0; screen < 2; ++screen)
            for (int bg = 0; bg < 2; ++bg)
                for (int o = 0; o <= (int)DgxOutputTransverse; ++o)
                    for (int scale = 1; scale <= 4; ++scale) {
                        dgx_screen_t *const    to   = screen ? narrow : lcd;
                        const dgx_image_blit_t how  = {.colors      = colors,
                                                       .alpha       = alpha,
                                                       .flags       = DGX_IMAGE_SOFT | DGX_IMAGE_CLIP | (bg ? DGX_IMAGE_BG : DGX_IMAGE_OPAQUE),
                                                       .bg          = BLUE,
                                                       .scale       = (uint8_t)scale,
                                                       .orientation = (dgx_output_orientation_t)o,
                                                       .clip_x      = 31,
                                                       .clip_y      = 30,
                                                       .clip_w      = 64,
                                                       .clip_h      = 7};
                        const uint16_t         none = 0x1234u; /* no color of the picture, nor a mix of them */
                        dgx_fill_rectangle(ref, 0, 0, 64, 64, none);
                        fill_now(to, 0, 0, 64, 64, none);
                        CHECK(dgx_image_blit(ref, 32, 32, &four, &how) == ESP_OK);
                        memset(writes, 0, sizeof writes);
                        windows = 0;
                        CHECK(blit_now(to, 32, 32, &four, &how) == ESP_OK);
                        int same = 1, boxed = 0;
                        for (int y = 0; y < 64; ++y)
                            for (int x = 0; x < 64; ++x) {
                                const int in = px(ref, x, y) != none;
                                boxed += in;
                                same &= writes[y][x] == in && glass[y][x] == px(ref, x, y);
                            }
                        CHECK(same && boxed > 0 && (bg || windows == 1));
                    }
        dgx_screen_destroy(&ref);

        /* every format, every way of putting, every turn and scale, both buffers: the rows made on a screen of 16 bits
           against the pixels the slow way makes on a virtual screen. There a box with DGX_IMAGE_OPAQUE is drawn
           without it over bg, so every pixel is mixed with what lies under it, or put whole from half on, by the
           loop for any screen. Behind a bus every pixel of the box is written once, and with runs only those put. */
        {
            static uint8_t  pix16[5 * 3 * 2], plane[5 * 3], val8[5 * 3], cov8[5 * 3], val4[3 * 3], val1[3];
            static uint32_t tab256[256];
            static uint8_t  alpha256[256];
            for (int i = 0; i < 15; ++i) {
                const uint16_t color = (uint16_t)(i * 0x1357u + 0x0841u);
                pix16[2 * i] = (uint8_t)(color >> 8), pix16[2 * i + 1] = (uint8_t)color;
                plane[i] = (uint8_t)(i % 3 == 0 ? 255 : i % 3 == 1 ? i * 17 : 0);
                val8[i]  = (uint8_t)(i * 37 + 3);
                cov8[i]  = (uint8_t)(i % 4 == 0 ? 0 : i % 4 == 1 ? 255 : i * 19);
            }
            for (int i = 0; i < 256; ++i) tab256[i] = (uint32_t)(i * 0x0F0Fu + 0x1001u) & 0xFFFFu, alpha256[i] = (uint8_t)(i % 5 == 0 ? 0 : i % 5 == 1 ? 255 : i * 3);
            for (int i = 0; i < 9; ++i) val4[i] = (uint8_t)(i * 0x37u + 0x10u);
            val1[0] = 0xA8, val1[1] = 0x50, val1[2] = 0xF8;
            val8[0] = 0, val8[7] = 0; /* transparent where colors come without opacities */
            const dgx_image_t pictures[] = {
                {.width = 5, .height = 3, .origin_x = 2, .origin_y = 1, .format = DGX_IMAGE_16BPP, .data = pix16},
                {.width = 5, .height = 3, .origin_x = 2, .origin_y = 1, .format = DGX_IMAGE_16BPP, .data = pix16, .alpha = plane},
                {.width = 5, .height = 3, .origin_x = 2, .origin_y = 1, .format = DGX_IMAGE_8BPP, .data = val8},
                {.width = 5, .height = 3, .origin_x = 2, .origin_y = 1, .format = DGX_IMAGE_8BPP, .data = cov8},
                {.width = 5, .height = 3, .origin_x = 2, .origin_y = 1, .format = DGX_IMAGE_4BPP, .data = val4},
                {.width = 5, .height = 3, .origin_x = 2, .origin_y = 1, .format = DGX_IMAGE_1BPP, .data = val1},
                {.width = 5, .height = 3, .origin_x = 2, .origin_y = 1, .format = DGX_IMAGE_8BPP, .data = val8},
                {.width = 5, .height = 3, .origin_x = 2, .origin_y = 1, .format = DGX_IMAGE_4BPP, .data = val4},
            };
            static const uint32_t puts[] = {DGX_IMAGE_SOFT | DGX_IMAGE_OPAQUE, DGX_IMAGE_OPAQUE, DGX_IMAGE_SOFT | DGX_IMAGE_BG, 0};
            const uint16_t        none   = 0x1234u;
            dgx_screen_t         *slow = dgx_vscreen_init(64, 64, 16, DgxScreenRGB), *box = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
            int                   failed = 0;
            for (unsigned pi = 0; pi < sizeof pictures / sizeof *pictures; ++pi)
                for (unsigned wi = 0; wi < sizeof puts / sizeof *puts; ++wi) {
                    const bool coverage = pi == 3;
                    if (coverage && !(puts[wi] & DGX_IMAGE_SOFT)) continue; /* a mask is drawn soft, or in one color */
                    for (int o = 0; o <= (int)DgxOutputTransverse; ++o)
                        for (int scale = 1; scale <= 3; ++scale)
                            for (int screen = 0; screen < 2; ++screen) {
                                dgx_screen_t *const to  = screen ? narrow : lcd;
                                const bool          opq = (puts[wi] & DGX_IMAGE_OPAQUE) != 0;
                                dgx_image_blit_t    how = {.flags       = puts[wi] | DGX_IMAGE_CLIP | (coverage ? DGX_IMAGE_SILHOUETTE : 0u),
                                                           .color       = WHITE,
                                                           .bg          = BLUE,
                                                           .scale       = (uint8_t)scale,
                                                           .orientation = (dgx_output_orientation_t)o,
                                                           .clip_x      = 20,
                                                           .clip_y      = 21,
                                                           .clip_w      = 24,
                                                           .clip_h      = 22};
                                if (pi == 2) how.colors = tab256, how.alpha = alpha256;
                                if (pi == 4) how.colors = tab256, how.alpha = alpha256;
                                static const uint32_t two_colors[2] = {RED, GRAY};
                                if (pi == 5) how.colors = two_colors;
                                if (pi >= 6) how.colors = tab256; /* no opacities: 0 transparent, the others opaque */
                                /* the box: every pixel of it, put by the quick way on a virtual screen */
                                dgx_fill_rectangle(box, 0, 0, 64, 64, none);
                                dgx_image_blit_t whole = how;
                                whole.flags |= DGX_IMAGE_OPAQUE;
                                CHECK(dgx_image_blit(box, 32, 32, &pictures[pi], &whole) == ESP_OK);
                                /* the slow way: without DGX_IMAGE_OPAQUE over bg, or over none */
                                dgx_image_blit_t slow_how = how;
                                slow_how.flags &= ~(uint32_t)(DGX_IMAGE_OPAQUE | DGX_IMAGE_BG);
                                dgx_fill_rectangle(slow, 0, 0, 64, 64, opq ? BLUE : none);
                                if (puts[wi] & DGX_IMAGE_BG) {
                                    /* BG mixes with bg where the slow way mixes with what lies under: under it lies bg,
                                       outside the pixels put none */
                                    dgx_fill_rectangle(slow, 0, 0, 64, 64, BLUE);
                                }
                                CHECK(dgx_image_blit(slow, 32, 32, &pictures[pi], &slow_how) == ESP_OK);
                                fill_now(to, 0, 0, 64, 64, none);
                                memset(writes, 0, sizeof writes);
                                windows = 0;
                                CHECK(blit_now(to, 32, 32, &pictures[pi], &how) == ESP_OK);
                                int same = 1, boxed = 0;
                                for (int y = 0; y < 64; ++y)
                                    for (int x = 0; x < 64; ++x) {
                                        const int in = px(box, x, y) != none;
                                        boxed += in;
                                        if (opq) {
                                            same &= writes[y][x] == in && (!in || glass[y][x] == px(slow, x, y));
                                        } else if (puts[wi] & DGX_IMAGE_BG) {
                                            /* put where the slow way changed bg, or put bg itself: the value then as there */
                                            same &= writes[y][x] <= in && (!writes[y][x] ? glass[y][x] == none : glass[y][x] == px(slow, x, y));
                                        } else {
                                            same &= writes[y][x] == (px(slow, x, y) != none) && glass[y][x] == px(slow, x, y);
                                        }
                                    }
                                if (!(same && boxed > 0 && (!opq || windows == 1))) ++failed;
                            }
                }
            CHECK(failed == 0);
            dgx_screen_destroy(&slow);
            dgx_screen_destroy(&box);
        }
        dgx_screen_destroy(&narrow);

        /* a panel of 18 bits, three bytes a pixel: every pixel of a run reaches the glass, by runs or in one window, as a
           virtual screen of 18 bits has it */
        {
            static uint32_t tab18[256];
            static uint8_t  vals[20 * 37], cover[20 * 37];
            for (int i = 0; i < 256; ++i) tab18[i] = DGX_RGB_18(i * 37 & 255, i * 91 & 255, i * 53 & 255);
            for (int y = 0; y < 37; ++y)
                for (int x = 0; x < 20; ++x) vals[y * 20 + x] = (uint8_t)((x + 3 * y) % 8), cover[y * 20 + x] = (uint8_t)((x * 29 + y * 13) & 255);
            static uint8_t nibbles[10 * 37];
            for (int i = 0; i < 10 * 37; ++i) nibbles[i] = (uint8_t)(vals[2 * i] << 4 | vals[2 * i + 1]);
            const dgx_image_t eight = {.width = 20, .height = 37, .format = DGX_IMAGE_8BPP, .data = vals};
            const dgx_image_t four  = {.width = 20, .height = 37, .format = DGX_IMAGE_4BPP, .data = nibbles};
            const dgx_image_t mask  = {.width = 20, .height = 37, .format = DGX_IMAGE_8BPP, .data = cover};
            const dgx_image_t *const pics[] = {&eight, &four, &mask};
            static const uint32_t ways[] = {0, DGX_IMAGE_OPAQUE, DGX_IMAGE_SOFT | DGX_IMAGE_BG, DGX_IMAGE_SOFT | DGX_IMAGE_OPAQUE};
            pixel_bytes = 3;
            dgx_screen_t *p18 = dgx_panel_init(&bus, &panel64, GPIO_NUM_NC, GPIO_NUM_NC, 18, DgxScreenRGB);
            dgx_screen_t *r18 = dgx_vscreen_init(64, 64, 18, DgxScreenRGB);
            CHECK(p18 && p18->color_bits == 18 && r18);
            const uint32_t none = DGX_RGB_18(16, 32, 48);
            int            failed = 0;
            for (unsigned pi = 0; p18 && pi < 3; ++pi)
                for (unsigned wi = 0; wi < 4; ++wi) {
                    if (pi == 2 && !(ways[wi] & DGX_IMAGE_SOFT)) continue;
                    for (int o = 0; o <= (int)DgxOutputTransverse; ++o)
                        for (int scale = 1; scale <= 2; ++scale) {
                            dgx_image_blit_t how = {.flags = ways[wi] | (pi == 2 ? DGX_IMAGE_SILHOUETTE : 0u), .color = DGX_RGB_18(255, 255, 255),
                                                    .bg = DGX_RGB_18(0, 0, 255), .scale = (uint8_t)scale, .orientation = (dgx_output_orientation_t)o};
                            if (pi < 2) how.colors = tab18;
                            fill_now(p18, 0, 0, 64, 64, none);
                            dgx_fill_rectangle(r18, 0, 0, 64, 64, none);
                            CHECK(blit_now(p18, 22, 13, pics[pi], &how) == ESP_OK && dgx_image_blit(r18, 22, 13, pics[pi], &how) == ESP_OK);
                            int same = 1;
                            for (int y = 0; y < 64; ++y)
                                for (int x = 0; x < 64; ++x) same &= glass[y][x] == dgx_get_pixel(r18, x, y);
                            if (!same) ++failed;
                        }
                }
            CHECK(failed == 0);
            pixel_bytes = 2;
            dgx_screen_destroy(&p18);
            dgx_screen_destroy(&r18);
        }
        /* with DGX_IMAGE_BG: the edges mixed with bg, a pixel not covered not written */
        fill_now(lcd, 0, 0, 64, 64, GRAY);
        memset(writes, 0, sizeof writes);
        CHECK(blit_now(lcd, 10, 40, &mask8,
                             &(dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT | DGX_IMAGE_BG, .color = WHITE, .bg = BLUE}) == ESP_OK);
        CHECK(writes[40][10] == 0 && glass[40][10] == GRAY && glass[40][11] == dgx_mix_color(16, BLUE, WHITE, 128) && glass[40][12] == WHITE &&
              glass[40][13] == dgx_mix_color(16, BLUE, WHITE, 1));
        dgx_screen_destroy(&lcd);
    }

    dgx_screen_destroy(&s24);
    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
