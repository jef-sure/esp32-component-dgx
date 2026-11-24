/*
 * A check of the ST7796S driver on a 320x480 SPI module.
 *
 * The demo runs four scenes in a loop and prints to the log what each one
 * should look like:
 *   1. colors at 16 bits a pixel: named color bars and color ramps;
 *   2. the eight orientations of the screen, five seconds each;
 *   3. speed: the whole screen filled 10 times in a row (the screen flashes),
 *      then the time of one fill;
 *   4. colors at 18 bits a pixel: the same bars, the ramps smoother.
 *
 * Change the pins and the SPI clock below to match your wiring.
 */
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_spi_esp32.h"
#include "dgx_bits.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_screen.h"
#include "drivers/st7796s.h"
#include "scenes.h"

static const char *TAG = "st7796s_demo";

#define DEMO_LCD_HOST SPI3_HOST

enum {
    DEMO_PIN_MOSI      = 23,
    DEMO_PIN_SCLK      = 18,
    DEMO_PIN_CS        = 5,
    DEMO_PIN_DC        = 2,
    DEMO_PIN_RST       = 4,  /* GPIO_NUM_NC if RESET is tied to 3.3V or EN */
    DEMO_PIN_BACKLIGHT = -1, /* GPIO_NUM_NC: LED tied to 3.3V; or the GPIO it is wired to */
    DEMO_SPI_MHZ       = 40, /* try 20 if the picture is broken */
};

/* the color order the driver is told; DgxScreenBGR if red and blue come out swapped */
#define DEMO_COLOR_ORDER DgxScreenRGB

static dgx_screen_t *make_screen(dgx_bus_protocols_t *bus, int bits)
{
    dgx_screen_t *scr = dgx_st7796s_init(bus, (gpio_num_t)DEMO_PIN_RST, (gpio_num_t)DEMO_PIN_BACKLIGHT, (uint8_t)bits, DEMO_COLOR_ORDER,
                                         DGX_ST7796S_320X480);
    if (!scr) ESP_LOGE(TAG, "dgx_st7796s_init() failed at %d bits", bits);
    return scr;
}

static void colors(dgx_screen_t *scr, int bits)
{
    ESP_LOGI(TAG, "test %d, colors %d-bit: bars RED, GREEN, BLUE, WHITE, BLACK in these colors; "
                  "below them ramps from black to red, green, blue and white",
             DEMO_TEST, bits);
    demo_colors(scr, bits == 18 ? dgx_rgb_to_18 : dgx_rgb_to_16, bits);
    vTaskDelay(pdMS_TO_TICKS(8000));
}

static void orientations(dgx_screen_t *scr, dgx_screen_t *aside)
{
    for (int way = 0; way < DEMO_WAYS; ++way) {
        const demo_way_t *d = &demo_ways[way];
        dgx_screen_orientation(scr, d->dir_x, d->dir_y, d->swap_xy);
        ESP_LOGI(TAG, "test %d, orientation %d of 8, %s, %dx%d: all text reads normally, the large F looks like the small one", DEMO_TEST,
                 way + 1, d->name, scr->width, scr->height);
        demo_orientation(scr, aside, dgx_rgb_to_16, way);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    dgx_screen_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false);
}

static void speed(dgx_screen_t *scr)
{
    ESP_LOGI(TAG, "test %d, speed: the screen flashes 10 times", DEMO_TEST);
    static const uint8_t fills[][3] = {
        {255, 0,   0  },
        {0,   255, 0  },
        {0,   0,   255},
        {255, 255, 255},
        {0,   0,   0  },
    };
    const int     n  = 10;
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < n; ++i) {
        const uint8_t *c = fills[i % 5];
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, dgx_rgb_to_16(c[0], c[1], c[2]));
    }
    dgx_screen_wait(scr);
    const int us = (int)((esp_timer_get_time() - t0) / n);
    ESP_LOGI(TAG, "test %d, speed: one fill of %dx%d takes %d us at SPI %d MHz", DEMO_TEST, scr->width, scr->height, us, DEMO_SPI_MHZ);
    demo_speed(scr, dgx_rgb_to_16, us, DEMO_SPI_MHZ);
    vTaskDelay(pdMS_TO_TICKS(6000));
}

void app_main(void)
{
    ESP_LOGI(TAG, "ST7796S demo, test %d: MOSI %d, SCLK %d, CS %d, DC %d, RST %d, backlight %d, SPI %d MHz", DEMO_TEST, DEMO_PIN_MOSI,
             DEMO_PIN_SCLK, DEMO_PIN_CS, DEMO_PIN_DC, DEMO_PIN_RST, DEMO_PIN_BACKLIGHT, DEMO_SPI_MHZ);
    dgx_bus_protocols_t *bus = dgx_spi_init(DEMO_LCD_HOST, SPI_DMA_CH_AUTO, (gpio_num_t)DEMO_PIN_MOSI, GPIO_NUM_NC, (gpio_num_t)DEMO_PIN_SCLK,
                                            (gpio_num_t)DEMO_PIN_CS, (gpio_num_t)DEMO_PIN_DC, DEMO_SPI_MHZ * 1000000, 0);
    dgx_screen_t        *aside = dgx_bw_init(160, 10);
    if (!bus || !aside) {
        ESP_LOGE(TAG, "dgx_spi_init() or dgx_bw_init() failed");
        return;
    }
    for (;;) {
        dgx_screen_t *scr = make_screen(bus, 16);
        if (!scr) return;
        colors(scr, 16);
        orientations(scr, aside);
        speed(scr);
        dgx_screen_destroy(&scr);
        scr = make_screen(bus, 18);
        if (!scr) return;
        colors(scr, 18);
        dgx_screen_destroy(&scr);
    }
}
