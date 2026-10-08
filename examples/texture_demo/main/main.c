#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_spi_esp32.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_hw_font.h"
#include "dgx_screen.h"
#include "drivers/ili9341.h"
#include "drivers/vscreen.h"
#include "fonts/DroidSansRegular9.h"
#include "fonts/TerminusTTFMedium12.h"
#include "fonts/font0ss.h"

static const char *TAG = "dgx_texture_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;

/* The card: drawn once into a texture of this size. */
#define CARD_W 96
#define CARD_H 64
/* The frame the card moves in: drawn anew every time and sent to the screen whole. */
#define FRAME_SIDE 200
/* Every kind of motion is shown so long. */
#define EXAMPLE_ACT_US 7000000LL

/* What a frame does to the card. */
typedef struct {
    float angle;  /* radians, clockwise on the screen */
    float scale;
    float wobble; /* how far every corner swings on its own, pixels */
    float phase;
} motion_t;

typedef struct {
    const char *name;
    motion_t (*at)(float seconds);
} act_t;

static motion_t scaling(float s)
{
    return (motion_t){.scale = 1.0f + 0.6f * sinf(s * 2.2f)};
}

static motion_t rotation(float s)
{
    return (motion_t){.angle = s * 1.6f, .scale = 1.2f};
}

static motion_t wobbling(float s)
{
    return (motion_t){.scale = 1.4f, .wobble = 14, .phase = s * 5};
}

static motion_t all_at_once(float s)
{
    return (motion_t){.angle = 0.5f * sinf(s * 1.3f) + s * 0.4f, .scale = 1.1f + 0.4f * sinf(s * 1.9f), .wobble = 9, .phase = s * 4};
}

static const act_t acts[] = {
    {"scaling", scaling},
    {"rotation", rotation},
    {"wobbling", wobbling},
    {"all at once", all_at_once},
};
#define ACTS (int)(sizeof(acts) / sizeof(acts[0]))

/*
 * Where the four corners of the card are: top-left, top-right, bottom-right,
 * bottom-left. Scaling moves them from the centre, wobbling swings each on its
 * own, rotation turns them all around the centre.
 */
static void card_corners(dgx_point_2d_t quad[4], float cx, float cy, const motion_t *m)
{
    static const float side_x[4] = {-1, 1, 1, -1}, side_y[4] = {-1, -1, 1, 1};
    float              c = cosf(m->angle), s = sinf(m->angle);
    for (int i = 0; i < 4; ++i) {
        float x = side_x[i] * CARD_W / 2 * m->scale + m->wobble * sinf(m->phase + i * 1.7f);
        float y = side_y[i] * CARD_H / 2 * m->scale + m->wobble * cosf(m->phase * 1.3f + i * 2.3f);
        quad[i].x = (int16_t)lroundf(cx + x * c - y * s);
        quad[i].y = (int16_t)lroundf(cy + x * s + y * c);
    }
}

/* A text of the handwritten font fitted into a box of the card, in its middle. */
static void write_in(dgx_screen_t *card, const char *text, int x, int y, int w, int h, int pen, uint32_t color)
{
    dgx_font_t *font = font0ss();
    int         left, top, right, bottom;
    if (!dgx_hw_text_box(font, text, &left, &top, &right, &bottom)) return;
    float by_width = (float)(w - pen) / (right - left + 1);
    float by_height = (float)(h - pen) / (bottom - top + 1);
    float scale = by_width < by_height ? by_width : by_height;
    int   tx = x + (int)lroundf((w - (right - left + 1) * scale) / 2 - left * scale);
    int   ty = y + (int)lroundf((h - (bottom - top + 1) * scale) / 2 - top * scale);
    dgx_hw_draw_text(card, tx, ty, font, text, scale, pen, color, true);
}

/*
 * A card with a frame, a word written by hand and a red copyright sign in its
 * top-right corner, which also tells its sides apart when it turns. The sign
 * is a letter of a printed font with a circle around.
 */
static void draw_card(dgx_screen_t *card)
{
    uint32_t ground = dgx_rgb_to_16(20, 40, 110), edge = dgx_rgb_to_16(250, 246, 232), ink = dgx_rgb_to_16(255, 210, 60);
    dgx_fill_rectangle(card, 0, 0, CARD_W, CARD_H, edge);
    dgx_fill_rectangle(card, 3, 3, CARD_W - 6, CARD_H - 6, ground);
    write_in(card, "DGX", 8, 10, CARD_W - 32, CARD_H - 18, 3, ink);
    uint32_t       red = dgx_rgb_to_16(240, 70, 70);
    int            cx = CARD_W - 13, cy = 14, r = 6;
    int16_t        advance;
    const glyph_t *c = dgx_font_find_glyph('C', DroidSansRegular9(), &advance);
    if (c) {
        /* the letter is put by its base line: its box goes around the middle of the circle */
        dgx_font_char_to_screen(card, (int16_t)(cx - c->xOffset - c->width / 2), (int16_t)(cy - c->yOffset - c->height / 2), 'C', red,
                                DgxOutputNormal, 1, DroidSansRegular9(), NULL, NULL);
    }
    dgx_draw_circle(card, cx, cy, r, red);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting texture demo on CYD");
    dgx_bus_protocols_t *bus = dgx_spi_init(
        EXAMPLE_LCD_HOST, SPI_DMA_CH_AUTO,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MOSI, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MISO,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CLK, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CS,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_DC, EXAMPLE_LCD_SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return;
    }
    dgx_screen_t *screen = dgx_ili9341_init(
        bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_BACKLIGHT, 16, DgxScreenRGB);
    if (!screen) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return;
    }
    dgx_ili9341_orientation(screen, DgxScreenRightLeft, DgxScreenTopBottom, true);
    uint32_t back = DGX_BLACK(dgx_rgb_to_16);
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, back);

    /* the texture and the frame have the color depth of the screen */
    dgx_screen_t *card = dgx_vscreen_init(CARD_W, CARD_H, 16, DgxScreenRGB);
    dgx_screen_t *frame = dgx_vscreen_init(FRAME_SIDE, FRAME_SIDE, 16, DgxScreenRGB);
    if (!card || !frame) {
        ESP_LOGE(TAG, "No memory for the texture and the frame");
        return;
    }
    draw_card(card);
    int frame_x = (screen->width - FRAME_SIDE) / 2;
    int frame_y = screen->height - FRAME_SIDE - 4;

    for (int round = 0;; ++round) {
        const act_t *act = &acts[round % ACTS];
        dgx_fill_rectangle(screen, 0, 0, screen->width, frame_y, back);
        dgx_font_string_utf8_screen(screen, 8, 16, act->name, DGX_WHITE(dgx_rgb_to_16), DgxOutputNormal, 2, TerminusTTFMedium12(),
                                    NULL, NULL);

        int64_t start_us = esp_timer_get_time(), now_us, drawing_us = 0;
        int     frames = 0;
        do {
            now_us = esp_timer_get_time();
            motion_t       motion = act->at((now_us - start_us) * 1e-6f);
            dgx_point_2d_t quad[4];
            card_corners(quad, FRAME_SIDE / 2, FRAME_SIDE / 2, &motion);
            dgx_fill_rectangle(frame, 0, 0, FRAME_SIDE, FRAME_SIDE, back);
            dgx_draw_texture_quad(frame, quad, card, 0, 0, CARD_W, CARD_H);
            drawing_us += esp_timer_get_time() - now_us;
            dgx_vscreen_to_screen(screen, frame_x, frame_y, frame);
            ++frames;
            vTaskDelay(1);
        } while (now_us - start_us < EXAMPLE_ACT_US);
        ESP_LOGI(TAG, "%s: %d frames, %.1f FPS, a frame is drawn in %lld us", act->name, frames,
                 frames * 1e6f / (esp_timer_get_time() - start_us), drawing_us / frames);
    }
}
