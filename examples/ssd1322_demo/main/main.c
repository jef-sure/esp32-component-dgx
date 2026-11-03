/*
 * The SSD1322 256x64 OLED of 16 grays on the 8-bit parallel bus of an ESP32.
 * The driver keeps a 4-bit RAM buffer and sends what has changed, so the
 * drawing is as on any screen of DGX; the panel shows 16 levels of gray.
 * Every scene carries the build number in its top left corner: upright and
 * readable there, the panel is mounted the way the driver expects.
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_p8_esp32.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_screen.h"
#include "drivers/ssd1322.h"
#include "fonts/DejaVuSansBook32.h"
#include "fonts/GLCD5x7.h"

static const char *TAG = "dgx_ssd1322_demo";

/* the wiring of the test board */
enum
{
    PIN_D0  = 13, // IO13
    PIN_D1  = 14, // IO14
    PIN_D2  = 16, // RX2
    PIN_D3  = 17, // TX2
    PIN_D4  = 18, // IO18
    PIN_D5  = 19, // IO19
    PIN_D6  = 21, // IO21
    PIN_D7  = 22, // IO22
    PIN_WR  = 25, // IO25
    PIN_RD  = 26, // IO26
    PIN_CS  = 27, // IO27
    PIN_DC  = 32, // IO32
    PIN_RST = 33, // IO33
};

/* the SSD1322 takes a write cycle of 300 ns at the shortest; the I2S bus of an ESP32 runs at 40 MHz / n */
#define P8_CLOCK_HZ (2500 * 1000)

#define BUILD 671

static dgx_screen_t *scr;

/* the 16 levels of gray the panel shows, 0 (dark) to 15 (bright) */
static inline uint32_t gray(uint32_t level)
{
    return level << 4;
}

/* the build number in the top left corner, on black; y of a string is its baseline */
static void label(void)
{
    char text[16];
    snprintf(text, sizeof(text), "b%d", BUILD);
    dgx_fill_rectangle(scr, 0, 0, 6 * (int)strlen(text) + 2, 10, 0);
    dgx_font_string_utf8_screen(scr, 1, 8, text, gray(15), DgxOutputNormal, 1, GLCD5x7(), NULL, NULL);
}

static void scene_grays(void)
{
    /* 16 bars, one for each gray */
    const int w = scr->width / 16;
    dgx_screen_progress_up(scr);
    for (int i = 0; i < 16; ++i)
        dgx_fill_rectangle(scr, i * w, 0, w, scr->height, gray(i));
    label();
    dgx_screen_progress_down(scr);
}

static void scene_text(void)
{
    dgx_screen_progress_up(scr);
    dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0);
    label();
    dgx_font_string_utf8_screen(scr, 40, 8, "SSD1322 256x64, 8-bit bus", gray(15), DgxOutputNormal, 1, GLCD5x7(), NULL, NULL);
    dgx_font_string_utf8_screen(scr, 3, 40, "DGX", gray(15), DgxOutputNormal, 3, GLCD5x7(), NULL, NULL);
    dgx_font_string_utf8_screen(scr, 70, 52, "16 grays", gray(10), DgxOutputNormal, 1, DejaVuSansBook32(), NULL, NULL);
    dgx_screen_progress_down(scr);
}

static void scene_lines(void)
{
    dgx_screen_progress_up(scr);
    dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0);
    for (int i = 0; i < scr->width; i += 4)
        dgx_draw_line(scr, i, 0, scr->width - 1 - i, scr->height - 1, gray((i / 4) % 16));
    dgx_draw_circle(scr, scr->width / 2, scr->height / 2, scr->height / 2 - 2, gray(15));
    dgx_solid_circle(scr, scr->width / 2, scr->height / 2, scr->height / 4 - 2, gray(8));
    label();
    dgx_screen_progress_down(scr);
}

static void scene_ramp(void)
{
    /* a smooth ramp: every pixel of a row its own gray, the rows below a dimmer copy of it */
    dgx_screen_progress_up(scr);
    for (int y = 0; y < scr->height; ++y) {
        const uint32_t dim = 15u - (uint32_t)y * 15u / (uint32_t)(scr->height - 1);
        for (int x = 0; x < scr->width; ++x)
            dgx_set_pixel(scr, x, y, gray((uint32_t)x * dim / (uint32_t)(scr->width - 1)));
    }
    label();
    dgx_screen_progress_down(scr);
}

typedef void (*scene_fn_t)(void);

static const scene_fn_t scenes[] = {scene_grays, scene_text, scene_lines, scene_ramp};

void app_main(void)
{
    dgx_bus_protocols_t *bus = dgx_p8_init(PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7,
            PIN_WR, PIN_RD, PIN_CS, PIN_DC, P8_CLOCK_HZ);
    if (!bus) {
        ESP_LOGE(TAG, "the P8 bus did not come up");
        return;
    }
    scr = dgx_ssd1322_init(bus, SSD1322_256X64, PIN_RST);
    if (!scr) {
        ESP_LOGE(TAG, "the SSD1322 screen did not come up");
        return;
    }
    ESP_LOGI(TAG, "build %d, SSD1322 up: %dx%d, %u bits a pixel", BUILD, scr->width, scr->height, scr->color_bits);
    dgx_ssd1322_contrast(scr, 0x9f);
    dgx_ssd1322_brightness(scr, 15);

    for (int i = 0;; ++i) {
        const int64_t t0 = esp_timer_get_time();
        scenes[i % 4]();
        scr->wait_buffer(scr);
        ESP_LOGI(TAG, "scene %d: %lld ms", i % 4, (long long)((esp_timer_get_time() - t0) / 1000));
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}
