#include <math.h>
#include <stdbool.h>
#include <stddef.h>
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
#include "dgx_screen.h"
#include "drivers/ili9341.h"
#include "drivers/vscreen.h"
#include "fonts/CasusDotView.h"

static const char *TAG = "dgx_flip_clock_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST
#define CLOCK_DIGITS 4
/* Every digit is drawn once into a small texture and stretched onto the screen from it. */
#define DIGIT_TEX_W 54
#define DIGIT_TEX_H 80
#define DIGIT_W 55
#define DIGIT_H 140
#define DIGIT_GAP 24
#define GROUP_GAP 36
/* The falling flap gets this much wider on each side at mid-flip, which reads as perspective. */
#define FLIP_WIDEN_PX DIGIT_GAP
#define FLIP_DURATION_US 1000000LL
/* The clock runs fast to show the flips: a minute passes every five seconds. */
#define TICK_PERIOD_US 1000000LL
#define CLOCK_SECONDS_PER_TICK 12
#define FRAME_PERIOD_MS 16

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;

typedef struct {
    uint8_t current;
    uint8_t next;
    bool animating;
    int64_t start_us;
    dgx_screen_t *current_tex;
    dgx_screen_t *next_tex;
    int16_t x;
} digit_t;

static digit_t digits[CLOCK_DIGITS];
static int16_t digits_y;
static uint32_t band_color;

/* One dot of a dot-font glyph: a soft 8-bit point stamped through a black-to-ink LUT. */
static void draw_dot(dgx_screen_t *dst, int x, int y, dgx_font_sym8_params_t *params)
{
    dgx_vscreen8_to_screen16(dst, x, y, params->vpoint, params->lut, true);
}

/* Draw the digit card at its natural size, then scale it into the texture. */
static bool draw_digit_texture(dgx_screen_t *tex, uint8_t digit)
{
    const uint32_t card = dgx_rgb_to_16(9, 14, 20);
    const uint32_t bezel = dgx_rgb_to_16(26, 36, 54);
    const int scale = 5;
    dgx_font_t *font = CasusDotView();
    char str[2] = {(char)('0' + digit % 10), '\0'};

    int16_t ycorner, glyph_h;
    int glyph_w = dgx_font_string_bounds(str, font, &ycorner, &glyph_h);
    int panel_w = DGX_MAX(glyph_w * scale + 14, 24);
    int panel_h = DGX_MAX(glyph_h * scale + 18, 32);

    dgx_screen_t *panel = dgx_vscreen_init(panel_w, panel_h, 16, DgxScreenRGB);
    dgx_screen_t *point = dgx_vscreen_init(scale, scale, 8, DgxScreenRGB);
    if (!panel || !point) {
        dgx_screen_destroy(&panel);
        dgx_screen_destroy(&point);
        return false;
    }
    dgx_fill_rectangle(panel, 0, 0, panel_w, panel_h, bezel);
    dgx_fill_rectangle(panel, 3, 3, panel_w - 6, panel_h - 6, card);

    dgx_font_make_point8(point);
    /* Index 0 is transparent; the rest fade from black to the ink color. */
    uint16_t lut[256];
    lut[0] = 0;
    for (int i = 1; i < 256; ++i) {
        lut[i] = (uint16_t)dgx_rgb_to_16((uint8_t)(238 * i / 255), (uint8_t)(242 * i / 255), (uint8_t)i);
    }
    dgx_font_sym8_params_t params = {
        .vpoint = point,
        .lut = lut,
        .dot_func = draw_dot,
    };
    int x = (panel_w - glyph_w * scale) / 2;
    int y = (panel_h - glyph_h * scale) / 2 - ycorner * scale;
    dgx_font_string_utf8_screen(panel, (int16_t)x, (int16_t)y, str, 0, DgxOutputNormal, scale, font, NULL, &params);

    dgx_draw_texture_rect(tex, 0, 0, tex->width, tex->height, panel, 0, 0, panel_w, panel_h);
    dgx_screen_destroy(&point);
    dgx_screen_destroy(&panel);
    return true;
}

static void draw_digit(dgx_screen_t *screen, const digit_t *d)
{
    dgx_draw_texture_rect(screen, d->x, digits_y, DIGIT_W, DIGIT_H, d->current_tex, 0, 0, DIGIT_TEX_W, DIGIT_TEX_H);
}

/*
 * A split-flap flip. First half: the top of the new digit is already in
 * place, and the top flap of the old digit falls towards the hinge, getting
 * shorter and wider. Second half: the same flap, now showing the bottom of
 * the new digit, comes down over the bottom of the old one.
 */
static void draw_flip(dgx_screen_t *screen, const digit_t *d, float t)
{
    const int16_t x = d->x;
    const int16_t y = digits_y;
    const int16_t right = (int16_t)(x + DIGIT_W - 1);
    const int16_t half = DIGIT_H / 2;
    const int16_t hinge = (int16_t)(y + half);
    const int tex_half = DIGIT_TEX_H / 2;

    /* The widened flap paints over the gaps next to the digit: wipe what the previous frame left there. */
    dgx_fill_rectangle(screen, x - FLIP_WIDEN_PX, y, FLIP_WIDEN_PX, DIGIT_H, band_color);
    dgx_fill_rectangle(screen, right + 1, y, FLIP_WIDEN_PX, DIGIT_H, band_color);

    if (t < 0.5f) {
        dgx_draw_texture_rect(screen, x, y, DIGIT_W, half, d->next_tex, 0, 0, DIGIT_TEX_W, tex_half);
        int16_t widen = (int16_t)lroundf(FLIP_WIDEN_PX * t);
        int16_t flap_top = (int16_t)lroundf(hinge - half * (1.0f - t * 2.0f));
        const dgx_point_2d_t flap[4] = {
            {(int16_t)(x - widen), flap_top},
            {(int16_t)(right + widen), flap_top},
            {right, (int16_t)(hinge - 1)},
            {x, (int16_t)(hinge - 1)},
        };
        dgx_draw_texture_quad(screen, flap, d->current_tex, 0, 0, DIGIT_TEX_W, tex_half);
    } else {
        int16_t widen = (int16_t)lroundf(FLIP_WIDEN_PX * (1.0f - t));
        int16_t flap_bottom = (int16_t)lroundf(hinge + half * (t - 0.5f) * 2.0f);
        flap_bottom = (int16_t)DGX_MAX(flap_bottom, hinge + 1);
        flap_bottom = (int16_t)DGX_MIN(flap_bottom, y + DIGIT_H);
        const dgx_point_2d_t flap[4] = {
            {x, hinge},
            {right, hinge},
            {(int16_t)(right + widen), (int16_t)(flap_bottom - 1)},
            {(int16_t)(x - widen), (int16_t)(flap_bottom - 1)},
        };
        dgx_draw_texture_quad(screen, flap, d->next_tex, 0, tex_half, DIGIT_TEX_W, tex_half);
    }
    dgx_draw_line(screen, x, hinge, right, hinge, dgx_rgb_to_16(16, 22, 34));
}

static void split_time(int32_t seconds, uint8_t out[CLOCK_DIGITS])
{
    int hours = seconds / 3600 % 24;
    int minutes = seconds / 60 % 60;
    out[0] = (uint8_t)(hours / 10);
    out[1] = (uint8_t)(hours % 10);
    out[2] = (uint8_t)(minutes / 10);
    out[3] = (uint8_t)(minutes % 10);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting flip clock demo on CYD");
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

    int32_t clock_seconds = 12 * 3600 + 34 * 60;
    uint8_t values[CLOCK_DIGITS];
    split_time(clock_seconds, values);

    const int total_w = DIGIT_W * CLOCK_DIGITS + DIGIT_GAP * 2 + GROUP_GAP;
    digits_y = (int16_t)((screen->height - DIGIT_H) / 2);
    digits[0].x = (int16_t)((screen->width - total_w) / 2);
    digits[1].x = (int16_t)(digits[0].x + DIGIT_W + DIGIT_GAP);
    digits[2].x = (int16_t)(digits[1].x + DIGIT_W + GROUP_GAP);
    digits[3].x = (int16_t)(digits[2].x + DIGIT_W + DIGIT_GAP);
    for (int i = 0; i < CLOCK_DIGITS; ++i) {
        digits[i].current = digits[i].next = values[i];
        digits[i].current_tex = dgx_vscreen_init(DIGIT_TEX_W, DIGIT_TEX_H, 16, DgxScreenRGB);
        digits[i].next_tex = dgx_vscreen_init(DIGIT_TEX_W, DIGIT_TEX_H, 16, DgxScreenRGB);
        if (!digits[i].current_tex || !digits[i].next_tex || !draw_digit_texture(digits[i].current_tex, values[i])) {
            ESP_LOGE(TAG, "Unable to allocate digit textures");
            return;
        }
    }

    band_color = dgx_rgb_to_16(8, 12, 20);
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, dgx_rgb_to_16(5, 8, 14));
    dgx_fill_rectangle(screen, 0, digits_y - 8, screen->width, DIGIT_H + 16, band_color);
    for (int i = 0; i < CLOCK_DIGITS; ++i) {
        draw_digit(screen, &digits[i]);
    }

    /* Only the digits that are flipping are redrawn; the rest of the screen is left alone. */
    int64_t next_tick_us = esp_timer_get_time() + TICK_PERIOD_US;
    int64_t stats_start_us = esp_timer_get_time();
    int64_t draw_us = 0;
    uint32_t flip_frames = 0;
    while (true) {
        int64_t now_us = esp_timer_get_time();
        while (now_us >= next_tick_us) {
            clock_seconds = (clock_seconds + CLOCK_SECONDS_PER_TICK) % (24 * 3600);
            split_time(clock_seconds, values);
            for (int i = 0; i < CLOCK_DIGITS; ++i) {
                /* A flip in progress is left to finish; the digit catches up on a later tick. */
                if (!digits[i].animating && values[i] != digits[i].current) {
                    if (!draw_digit_texture(digits[i].next_tex, values[i])) {
                        ESP_LOGW(TAG, "No memory to draw digit %d", values[i]);
                        continue;
                    }
                    digits[i].next = values[i];
                    digits[i].animating = true;
                    digits[i].start_us = now_us;
                }
            }
            next_tick_us += TICK_PERIOD_US;
        }

        bool flipping = false;
        for (int i = 0; i < CLOCK_DIGITS; ++i) {
            digit_t *d = &digits[i];
            if (!d->animating) continue;
            flipping = true;
            float t = (float)(now_us - d->start_us) / (float)FLIP_DURATION_US;
            if (t < 1.0f) {
                draw_flip(screen, d, t);
                continue;
            }
            dgx_screen_t *tex = d->current_tex;
            d->current_tex = d->next_tex;
            d->next_tex = tex;
            d->current = d->next;
            d->animating = false;
            dgx_fill_rectangle(screen, d->x - FLIP_WIDEN_PX, digits_y, FLIP_WIDEN_PX, DIGIT_H, band_color);
            dgx_fill_rectangle(screen, d->x + DIGIT_W, digits_y, FLIP_WIDEN_PX, DIGIT_H, band_color);
            draw_digit(screen, d);
        }

        int64_t done_us = esp_timer_get_time();
        if (flipping) {
            draw_us += done_us - now_us;
            ++flip_frames;
        }
        if (done_us - stats_start_us >= 5000000) {
            if (flip_frames) {
                ESP_LOGI(TAG, "%u flip frames, %.1f ms to draw one", (unsigned)flip_frames, (float)draw_us / 1000.0f / (float)flip_frames);
            }
            stats_start_us = done_us;
            draw_us = 0;
            flip_frames = 0;
        }

        int32_t spent_ms = (int32_t)((done_us - now_us) / 1000);
        vTaskDelay(pdMS_TO_TICKS(DGX_MAX(FRAME_PERIOD_MS - spent_ms, 1)));
    }
}
