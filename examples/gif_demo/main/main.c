/*
 * Animations of GIF files on a CYD: three GIFs made at build time into files of pictures (dgx_image.h), each frame
 * whole, with its delay and the times the animation is played, and drawn by the time on the clock.
 *
 * - A spinner of 4 bits, its background transparent, twice its size, for ever.
 * - A bouncing ball of 8 bits, twice its size, for ever.
 * - A bar filling, played 3 times as its GIF says, then standing on its last frame; it starts again every 20 seconds.
 *
 * Every frame goes on the paper with DGX_IMAGE_OPAQUE: the transparent pixels of the spinner take the color of the
 * paper, and the box of a frame is written whole, in one window, so a frame drawn over the one before does not
 * blink. A frame is drawn only when the clock comes to it; under each animation stand its frame and the time the
 * frame took to draw.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef DGX_HOST_WINDOW
/* on the computer, in a window: make in this directory */
#include "dgx_host_screen.h"
#else
#include "driver/gpio.h"
#include "driver/spi_common.h"

#include "bus/dgx_spi_esp32.h"
#include "drivers/ili9341.h"
#endif

#include "dgx_assets.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_image.h"
#include "dgx_image_file.h"
#include "dgx_screen.h"
#include "fonts/TerminusTTFMedium12.h"

/* shown on the screen and in the log; changed at every flash of a build to be looked at */
#define BUILD 936

static const char *TAG = "dgx_gif_demo";

#ifndef DGX_HOST_WINDOW
/* The CYD: an ILI9341 on SPI2. */
#define EXAMPLE_LCD_HOST SPI2_HOST
static const int EXAMPLE_LCD_PIN_NUM_MOSI      = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO      = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK       = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS        = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC        = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST       = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ     = 40;
#endif

/* how long the bar waits on its last frame before it starts again */
#define BAR_AGAIN_US 20000000LL

static uint32_t paper, ink, dim;

/* An animation: its file, the colors of its values on the screen, where it stands, when it began, what is shown. */
typedef struct {
    const char      *name;
    const uint8_t   *bytes;
    uint32_t         size;
    int              x, y, scale;
    int              label_y; /* the line under it */
    dgx_image_file_t file;
    uint32_t         colors[256];
    uint8_t          alpha[256];
    uint32_t         length_ms; /* one time round */
    int64_t          start;     /* when it began, in microseconds */
    int              shown;     /* the frame on the screen, -1 for none */
} animation_t;

static animation_t animations[] = {
    {.name = "spinner", .x = 16, .y = 30, .scale = 2, .label_y = 142},
    {.name = "ball", .x = 144, .y = 30, .scale = 2, .label_y = 174},
    {.name = "bar", .x = 16, .y = 194, .scale = 1, .label_y = 232},
};

/* a line of text on the paper, the cell under it filled first: a line drawn over a longer one leaves nothing of it */
static void text(dgx_screen_t *lcd, int x, int y, uint32_t color, const char *s)
{
    const dgx_font_context_t how = {.scr = lcd, .font = &TerminusTTFMedium12, .color = color, .bg = paper, .flags = DGX_FONT_FILL};
    dgx_font_draw(&how, x, y, s);
}

static bool open_animation(dgx_screen_t *lcd, animation_t *a)
{
    if (dgx_image_file_open(a->bytes, a->size, &a->file) != ESP_OK) {
        ESP_LOGE(TAG, "%s: not a file of pictures DGX reads", a->name);
        return false;
    }
    /* the colors of its values on this screen, 0 transparent: DGX_IMAGE_OPAQUE puts the paper there */
    if (dgx_image_file_colors(&a->file, lcd, a->colors, a->alpha) != ESP_OK) {
        ESP_LOGE(TAG, "%s: not an indexed file", a->name);
        return false;
    }
    a->length_ms = 0;
    for (unsigned i = 0; i < a->file.count; ++i) a->length_ms += dgx_image_file_delay(&a->file, i);
    char played[24];
    if (a->file.loops) snprintf(played, sizeof played, "%u times", (unsigned)a->file.loops);
    else snprintf(played, sizeof played, "for ever");
    ESP_LOGI(TAG, "%s: %u frames of %u bits, %u ms a round, played %s, %u bytes in flash", a->name, (unsigned)a->file.count, (unsigned)a->file.bits,
             (unsigned)a->length_ms, played, (unsigned)a->size);
    a->start = esp_timer_get_time();
    a->shown = -1;
    return true;
}

/* the frame of the time now, drawn if it is not the one on the screen; the line under it tells which */
static void step(dgx_screen_t *lcd, animation_t *a, int64_t now)
{
    const uint64_t ms    = (uint64_t)(now - a->start) / 1000u;
    const unsigned frame = dgx_image_file_frame(&a->file, ms);
    if ((int)frame == a->shown) return;
    dgx_image_t image;
    if (dgx_image_file_image(&a->file, frame, &image) != ESP_OK) return;
    const int64_t t0 = esp_timer_get_time();
    dgx_image_blit(lcd, a->x, a->y, &image,
                   &(dgx_image_blit_t){.colors = a->colors, .alpha = a->alpha, .flags = DGX_IMAGE_OPAQUE, .bg = paper, .scale = (uint8_t)a->scale});
    dgx_screen_wait(lcd);
    const int64_t t1 = esp_timer_get_time();
    a->shown         = (int)frame;
    char line[48];
    snprintf(line, sizeof line, "frame %2u/%u %5.2f ms", frame + 1, (unsigned)a->file.count, (double)(t1 - t0) / 1000.0);
    text(lcd, a->x, a->label_y, dim, line);
}

void app_main(void)
{
#ifdef DGX_HOST_WINDOW
    /* the glass of the CYD, 240x320, its SPI at 40 MHz, the screen turned on it as below and the board held so */
    const dgx_host_screen_options_t cyd = {.turn = 270};
    dgx_screen_t                   *lcd = dgx_host_screen_init(240, 320, 16, &cyd);
    if (!lcd) {
        ESP_LOGE(TAG, "dgx_host_screen_init failed");
        return;
    }
#else
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
        dgx_bus_dispose(&bus);
        return;
    }
#endif
    dgx_screen_orientation(lcd, DgxScreenRightLeft, DgxScreenTopBottom, true);
    paper = dgx_screen_rgb(lcd, 30, 32, 40);
    ink   = dgx_screen_rgb(lcd, 220, 224, 232);
    dim   = dgx_screen_rgb(lcd, 140, 146, 158);
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, paper);
    ESP_LOGI(TAG, "build %d, display %dx%d", BUILD, lcd->width, lcd->height);
    char title[48];
    snprintf(title, sizeof title, "GIF animations  b%d", BUILD);
    text(lcd, 16, 16, ink, title);

    animations[0].bytes = dgx_asset_spinner, animations[0].size = dgx_asset_spinner_size;
    animations[1].bytes = dgx_asset_ball, animations[1].size = dgx_asset_ball_size;
    animations[2].bytes = dgx_asset_bar, animations[2].size = dgx_asset_bar_size;
    for (unsigned i = 0; i < sizeof animations / sizeof *animations; ++i)
        if (!open_animation(lcd, &animations[i])) return;

    animation_t *const bar   = &animations[2];
    int                round = -1; /* the round of the bar the line tells */
    for (;;) {
        const int64_t now = esp_timer_get_time();
        /* the bar has played its times and stood long enough on its last frame: again */
        if (now - bar->start >= BAR_AGAIN_US) bar->start = now, bar->shown = -1, round = -1;
        for (unsigned i = 0; i < sizeof animations / sizeof *animations; ++i) step(lcd, &animations[i], now);
        /* the round of the bar, and once it has played them all, that it stands */
        const unsigned played = (unsigned)((uint64_t)(now - bar->start) / 1000u / bar->length_ms);
        const int      now_round = played < bar->file.loops ? (int)played + 1 : 0;
        if (now_round != round) {
            char line[48];
            if (now_round) snprintf(line, sizeof line, "round %d of %u       ", now_round, (unsigned)bar->file.loops);
            else snprintf(line, sizeof line, "played %u times, stays", (unsigned)bar->file.loops);
            text(lcd, bar->x + 136, bar->y + 14, ink, line);
            round = now_round;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
