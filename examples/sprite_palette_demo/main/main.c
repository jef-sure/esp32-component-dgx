/*
 * Sprites and palettes on a CYD: an 8-bit frame of 320 x 240 drawn anew every
 * time and sent through a table of colors.
 *
 * - Three walkers are the same 4-bit pictures with three maps of colors. A
 *   walker that turns round is mirrored around the origin at its feet, so it
 *   turns where it stands. The one on the shore has a reflection: the same
 *   picture as a silhouette, mirrored down from the same point and clipped
 *   to the water.
 * - A fourth walker crosses a window, clipped to it.
 * - A 1-bit arrow goes through the eight orientations around its centre.
 * - 8-bit coins use their values as indices of the frame.
 * - The day turns into night and back by mixing two palettes, lightning and
 *   a fade to black mix a palette with one color. The pixels of the frame do
 *   not change for any of it: only the table does. The text keeps its own
 *   entries of the table and stays readable.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_spi_esp32.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_image.h"
#include "dgx_lut.h"
#include "dgx_screen.h"
#include "drivers/ili9341.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

#include "sprites.h"

/* shown on the screen and in the log; changed at every flash of a build to be looked at */
#define BUILD 299

static const char TAG[] = "sprite_palette_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST
static const int EXAMPLE_LCD_PIN_NUM_MOSI      = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO      = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK       = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS        = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC        = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST       = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ     = 40;

#define W 320
#define H 240
#define BAND 32 /* the dark band of the text at the top */
#define HORIZON 160
#define SHORE 206

/*
 * The indices of the frame. 0 .. SCENE - 1 change with the time of day and
 * are mixed from two RGB888 palettes; the text has the entries from SCENE on,
 * set once.
 */
enum {
    SKY_FIRST = 1, /* 64 entries, from the top of the sky to the horizon */
    SKY_COUNT = 64,
    GRASS     = 65,
    DIRT      = 66,
    FURROW    = 67,
    FRAME     = 68,
    GLASS     = 69,
    WATER     = 70,
    RIPPLE    = 71,
    REFLECT   = 72,
    ARROW     = 73,
    SET_A     = 80, /* 15 entries each: the values 1..15 of a walker */
    SET_B     = 96,
    SET_C     = 112,
    SCENE     = 240,
    TEXT_BAND = 240,
    TEXT      = 255,
};

static uint16_t lut[256];
static uint8_t  day[SCENE][3], night[SCENE][3];
static uint8_t  map_a[15], map_b[15], map_c[15];

static void put(uint8_t (*pal)[3], int index, int r, int g, int b)
{
    pal[index][0] = (uint8_t)r, pal[index][1] = (uint8_t)g, pal[index][2] = (uint8_t)b;
}

/* the colors of a walker: the values 1..15 at base .. base + 14, and its map */
static void walker_colors(uint8_t (*pal)[3], int base, uint8_t *map, const uint8_t helmet[3], const uint8_t tunic[3])
{
    put(pal, base + 0, 20, 20, 30);    /* 1 outline */
    put(pal, base + 1, 240, 190, 150); /* 2 skin */
    put(pal, base + 2, helmet[0], helmet[1], helmet[2]);
    put(pal, base + 3, 255, 255, 230); /* 4 a shine on the helmet */
    put(pal, base + 4, tunic[0], tunic[1], tunic[2]);
    put(pal, base + 5, tunic[0] * 3 / 5, tunic[1] * 3 / 5, tunic[2] * 3 / 5);
    put(pal, base + 6, 110, 70, 30); /* 7 belt */
    put(pal, base + 7, 60, 60, 80);  /* 8 legs */
    put(pal, base + 8, 50, 30, 20);  /* 9 boots */
    put(pal, base + 9, 20, 20, 20);  /* 10 eye */
    for (int p = 1; p <= 15; ++p) map[p - 1] = (uint8_t)(base + p - 1);
}

static void make_palettes(void)
{
    for (int i = 0; i < SKY_COUNT; ++i) {
        const float k = (float)i / (SKY_COUNT - 1);
        put(day, SKY_FIRST + i, (int)(60 + 140 * k), (int)(120 + 110 * k), (int)(215 + 40 * k));
        put(night, SKY_FIRST + i, (int)(4 + 30 * k), (int)(6 + 36 * k), (int)(24 + 60 * k));
    }
    put(day, GRASS, 70, 170, 60);
    put(day, DIRT, 150, 110, 60);
    put(day, FURROW, 125, 90, 48);
    put(day, FRAME, 90, 60, 40);
    put(day, GLASS, 30, 40, 60);
    put(day, WATER, 50, 110, 170);
    put(day, RIPPLE, 90, 150, 200);
    put(day, REFLECT, 30, 70, 120);
    put(day, ARROW, 250, 220, 60);
    walker_colors(day, SET_A, map_a, (const uint8_t[3]){150, 160, 180}, (const uint8_t[3]){40, 90, 210});
    walker_colors(day, SET_B, map_b, (const uint8_t[3]){220, 180, 60}, (const uint8_t[3]){210, 40, 40});
    walker_colors(day, SET_C, map_c, (const uint8_t[3]){120, 120, 120}, (const uint8_t[3]){40, 160, 60});
    for (int i = 0; i < 6; ++i) put(day, COIN_FIRST + i, 120 + 27 * i, 80 + 30 * i, 10 + 20 * i);
    /* by night everything but the sky is darker and bluer, the coins less so */
    for (int i = GRASS; i < SCENE; ++i) {
        const float keep = i >= COIN_FIRST && i < COIN_FIRST + 6 ? 0.6f : 0.3f;
        put(night, i, (int)(day[i][0] * keep), (int)(day[i][1] * keep), (int)(day[i][2] * keep + 20));
    }
    /* the text: entries of its own, the same by day and by night */
    dgx_lut_gradient(lut, SCENE, 256 - SCENE, 0x202020, 0xffffff);
}

/*
 * The table at second s of a lap of 20: what a palette does to the frame.
 * Returns what is being done, to be written on the screen.
 */
static const char *palette_at(float s)
{
    if (s < 5) {
        dgx_lut_from_rgb(lut, 0, SCENE, day);
        return "day: dgx_lut_from_rgb";
    }
    if (s < 8) {
        dgx_lut_mix(lut, 0, SCENE, day, night, (uint8_t)((s - 5) / 3 * 255));
        return "dusk: dgx_lut_mix(day, night)";
    }
    if (s < 12) {
        /* three strokes of lightning, each gone in a quarter of a second */
        static const float strokes[] = {9.0f, 9.35f, 11.0f};
        float              flash     = 0;
        for (int i = 0; i < 3; ++i) {
            if (s >= strokes[i] && s < strokes[i] + 0.25f) flash = fmaxf(flash, 1 - (s - strokes[i]) / 0.25f);
        }
        dgx_lut_mix_color(lut, 0, SCENE, night, 0xffffff, (uint8_t)(flash * 160));
        return "night: lightning, dgx_lut_mix_color(white)";
    }
    if (s < 15) {
        dgx_lut_mix(lut, 0, SCENE, night, day, (uint8_t)((s - 12) / 3 * 255));
        return "dawn: dgx_lut_mix(night, day)";
    }
    if (s < 19) {
        const float dark = s < 17 ? (s - 15) / 2 : (19 - s) / 2;
        dgx_lut_mix_color(lut, 0, SCENE, day, 0x000000, (uint8_t)(dark * 255));
        return "fade: dgx_lut_mix_color(black)";
    }
    dgx_lut_from_rgb(lut, 0, SCENE, day);
    return "day: dgx_lut_from_rgb";
}

static const dgx_image_t walker_stand  = {.width = 16, .height = 24, .origin_x = 7, .origin_y = 22, .format = DGX_IMAGE_4BPP, .data = walker_stand_data};
static const dgx_image_t walker_stride = {.width = 16, .height = 24, .origin_x = 7, .origin_y = 22, .format = DGX_IMAGE_4BPP, .data = walker_stride_data};
static const dgx_image_t arrow         = {.width = 16, .height = 16, .origin_x = 7, .origin_y = 7, .format = DGX_IMAGE_1BPP, .data = arrow_data};
static const dgx_image_t coin          = {.width = 12, .height = 12, .origin_x = 6, .origin_y = 6, .format = DGX_IMAGE_8BPP, .data = coin_data};

/* where a walker going to and fro between two columns is at second s, and whether it goes left */
static int walk(float s, float speed, int from, int to, bool *left)
{
    const float lap = 2.0f * (float)(to - from), d = fmodf(s * speed, lap);
    *left           = d >= lap / 2;
    return *left ? to - (int)(d - lap / 2) : from + (int)d;
}

/* a walker with its feet at (x, y), and its reflection in the water under them */
static void draw_walker(dgx_screen_t *frame, int x, int y, bool left, const uint8_t *map, bool reflected)
{
    /* a stride every 6 pixels of the way */
    const dgx_image_t *picture = (x / 6) & 1 ? &walker_stride : &walker_stand;
    if (reflected) {
        /* the same picture upside down around the feet, mirrored as the walker is, in one color, only on the water */
        const dgx_image_blit_t water = {.flags       = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_CLIP,
                                        .color       = REFLECT,
                                        .orientation = left ? DgxOutputRotate180 : DgxOutputMirrorY,
                                        .clip_x      = 0,
                                        .clip_y      = SHORE,
                                        .clip_w      = W,
                                        .clip_h      = H - SHORE};
        dgx_image_blit(frame, x, y + 1, picture, &water);
    }
    const dgx_image_blit_t how = {.map = map, .orientation = left ? DgxOutputMirrorX : DgxOutputNormal};
    dgx_image_blit(frame, x, y, picture, &how);
}

static const char *const turn_names[8] = {"Normal", "MirrorX", "MirrorY", "Rotate180", "Transpose", "Rotate90CCW", "Rotate90CW", "Transverse"};

static void text(dgx_screen_t *frame, int x, int y, const char *s)
{
    dgx_font_string_utf8_screen(frame, (int16_t)x, (int16_t)y, s, TEXT, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
}

static void draw_scene(dgx_screen_t *frame, float s, float fps, float out_ms, const char *doing)
{
    /* the band of the text, the sky in rows of its ramp, the ground with furrows, the water with ripples */
    dgx_fill_rectangle(frame, 0, 0, W, BAND, TEXT_BAND);
    for (int y = BAND; y < HORIZON; ++y) dgx_fill_rectangle(frame, 0, y, W, 1, SKY_FIRST + (y - BAND) * (SKY_COUNT - 1) / (HORIZON - 1 - BAND));
    dgx_fill_rectangle(frame, 0, HORIZON, W, 8, GRASS);
    dgx_fill_rectangle(frame, 0, HORIZON + 8, W, SHORE - HORIZON - 8, DIRT);
    for (int y = HORIZON + 14; y < SHORE - 4; y += 12) dgx_fill_rectangle(frame, 0, y, W, 2, FURROW);
    dgx_fill_rectangle(frame, 0, SHORE, W, H - SHORE, WATER);
    for (int i = 0; i < 9; ++i) {
        const int y = SHORE + 4 + (i * 7) % (H - SHORE - 6), x = (int)(fmodf(s * 12 + i * 53, W + 40)) - 40;
        dgx_fill_rectangle(frame, x, y, 30 + (i % 3) * 12, 1, RIPPLE);
    }

    /* a window: a walker crosses it and is seen only through it */
    const int wx = 196, wy = 46, ww = 112, wh = 64;
    dgx_fill_rectangle(frame, wx - 4, wy - 4, ww + 8, wh + 8, FRAME);
    dgx_fill_rectangle(frame, wx, wy, ww, wh, GLASS);
    {
        bool                   left;
        const int              x   = walk(s, 40, wx - 30, wx + ww + 30, &left);
        const dgx_image_t     *pic = (x / 6) & 1 ? &walker_stride : &walker_stand;
        const dgx_image_blit_t how = {.map = map_b, .orientation = left ? DgxOutputMirrorX : DgxOutputNormal, .flags = DGX_IMAGE_CLIP,
                                      .clip_x = (int16_t)wx, .clip_y = (int16_t)wy, .clip_w = (int16_t)ww, .clip_h = (int16_t)wh};
        dgx_image_blit(frame, x, wy + wh - 6, pic, &how);
    }
    text(frame, wx + 6, wy + wh + 18, "DGX_IMAGE_CLIP");

    /* the arrow of 1 bit in the eight orientations around its centre, a new one twice a second */
    const int turn = (int)(s * 2) % 8;
    {
        const dgx_image_blit_t how = {.color = ARROW, .orientation = (dgx_output_orientation_t)turn};
        dgx_image_blit(frame, 28, 64, &arrow, &how);
        text(frame, 48, 61, "1 bit, turned:");
        text(frame, 48, 74, turn_names[turn]);
    }

    /* coins of 8 bits, their values the indices of the frame */
    for (int i = 0; i < 4; ++i) dgx_image_blit(frame, 28 + 24 * i, 116 + (int)(5 * sinf(s * 4 + i)), &coin, NULL);
    text(frame, 20, 146, "8 bits: values are indices");

    /* three walkers of 4 bits, the same pictures in three maps of colors; the last on the shore */
    bool left;
    int  x;
    x = walk(s, 34, 20, W - 20, &left);
    draw_walker(frame, x, 182, left, map_a, false);
    x = walk(s + 3.1f, 26, 20, W - 20, &left);
    draw_walker(frame, x, 194, left, map_c, false);
    x = walk(s + 7.7f, 47, 20, W - 20, &left);
    draw_walker(frame, x, SHORE - 1, left, map_b, true);
    text(frame, 4, H - 4, "4 bits: one picture, three maps");

    char line[64];
    snprintf(line, sizeof line, "b%d  %.1f fps  frame out %.1f ms", BUILD, (double)fps, (double)out_ms);
    text(frame, 4, 12, line);
    text(frame, 4, 27, doing);
}

void app_main(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(EXAMPLE_LCD_HOST, SPI_DMA_CH_AUTO, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MOSI,
                                            (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MISO, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CLK,
                                            (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CS, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_DC,
                                            EXAMPLE_LCD_SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return;
    }
    dgx_screen_t *lcd = dgx_ili9341_init(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_BACKLIGHT, 16, DgxScreenRGB);
    if (!lcd) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return;
    }
    dgx_ili9341_orientation(lcd, DgxScreenRightLeft, DgxScreenTopBottom, true);
    dgx_screen_t *frame = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    if (!frame) {
        ESP_LOGE(TAG, "No memory for the frame");
        return;
    }
    make_palettes();
    ESP_LOGI(TAG, "build %d, display %dx%d", BUILD, lcd->width, lcd->height);

    const int64_t start = esp_timer_get_time();
    int64_t       last = start, logged = start;
    float         fps = 0, out_ms = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const float   s   = fmodf((float)(now - start) / 1e6f, 20.0f);
        const char   *doing = palette_at(s);
        draw_scene(frame, s, fps, out_ms, doing);
        const int64_t t0 = esp_timer_get_time();
        dgx_vscreen8_to_screen16(lcd, 0, 0, frame, lut, 0);
        const int64_t t1 = esp_timer_get_time();
        out_ms = (float)(t1 - t0) / 1000.0f;
        fps    = fps * 0.9f + 0.1f * (1e6f / (float)(t1 - last));
        last   = t1;
        if (t1 - logged > 2000000) {
            ESP_LOGI(TAG, "build %d: %.1f fps, the frame goes out in %.1f ms", BUILD, (double)fps, (double)out_ms);
            logged = t1;
        }
        vTaskDelay(1); /* the idle task runs, the watchdog is quiet */
    }
}
