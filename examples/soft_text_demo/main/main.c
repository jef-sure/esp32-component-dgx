/*
 * Soft text on a CYD (ESP32-2432S028R: an ESP32 with a 320x240 ILI9341 on SPI).
 *
 * main/CMakeLists.txt asks for DejaVu Sans at 16 and 12 px twice: in 4 bits a pixel (BITS 4), whose edges are soft,
 * and in one bit, for a comparison. The display behind the bus cannot be read, so the soft text is mixed with a paper
 * it is told of: DGX_FONT_BG for the lines, DGX_FONT_FILL for a counter drawn again over itself. Below them a virtual
 * screen with a gradient: there the soft text is mixed with the picture under it, and the screen goes to the display.
 * The times of the ways of drawing a line go to the log once at start.
 */
#include <inttypes.h>
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
#include "dgx_fonts.h"
#include "dgx_screen.h"
#include "drivers/ili9341.h"
#include "drivers/vscreen.h"

static const char *TAG = "soft_text_demo";

/* the build number shown in the top right corner, when not 0: a copy of the example that is flashed many times sets it */
#define EXAMPLE_BUILD 0

/* the display of the CYD: SPI2, and no reset line of its own */
#define LCD_HOST SPI2_HOST

enum
{
    PIN_MOSI      = 13,
    PIN_MISO      = 12,
    PIN_CLK       = 14,
    PIN_CS        = 15,
    PIN_DC        = 2,
    PIN_RST       = GPIO_NUM_NC,
    PIN_BACKLIGHT = 21,
    SPI_CLOCK_MHZ = 40,
};

static const char *const line = "Привет, мир! Quick fox 0123";

/* a band of paper with the line in one bit and in four, at 16 px and at 12 px */
static void draw_band(dgx_screen_t *lcd, int top, uint32_t paper, uint32_t ink, uint32_t note)
{
    dgx_fill_rectangle(lcd, 0, top, lcd->width, 96, paper);
    const dgx_font_context_t hard = {.scr = lcd, .font = dgx_font_hard(), .color = ink};
    const dgx_font_context_t soft = {.scr = lcd, .font = dgx_font_soft(), .color = ink, .bg = paper, .flags = DGX_FONT_BG};
    const dgx_font_context_t small_hard = {.scr = lcd, .font = dgx_font_small_hard(), .color = ink};
    const dgx_font_context_t small = {.scr = lcd, .font = dgx_font_small(), .color = ink, .bg = paper, .flags = DGX_FONT_BG};
    const dgx_font_context_t label = {.scr = lcd, .font = dgx_font_small(), .color = note, .bg = paper, .flags = DGX_FONT_BG};
    dgx_font_draw(&label, 4, top + 18, "1 бит");
    dgx_font_draw(&hard, 52, top + 20, line);
    dgx_font_draw(&label, 4, top + 40, "4 бита");
    dgx_font_draw(&soft, 52, top + 42, line);
    dgx_font_draw(&small_hard, 52, top + 64, line);
    dgx_font_draw(&small, 52, top + 84, line);
}

/* a virtual screen with a gradient and a soft line over it, mixed with the picture, put to the display */
static dgx_screen_t *picture;

static void draw_picture(void)
{
    for (int y = 0; y < picture->height; ++y)
        for (int x = 0; x < picture->width; ++x)
            dgx_set_pixel(picture, x, y, dgx_rgb_to_16(40 + x * 180 / picture->width, 60 + y * 100 / picture->height, 160 - x * 120 / picture->width));
}

static void draw_over_picture(void)
{
    const dgx_font_context_t over = {.scr = picture, .font = dgx_font_soft(), .color = dgx_rgb_to_16(255, 255, 255)};
    dgx_font_draw(&over, 8, 30, "Над картинкой");
}

/* microseconds a call takes, the mean of `times` */
static int64_t mean_us(int times, void (*draw)(void *), void *arg)
{
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < times; ++i) draw(arg);
    return (esp_timer_get_time() - start) / times;
}

typedef struct {
    const dgx_font_context_t *ctx;
    int                       x, y;
} timed_line_t;

static void draw_line(void *arg)
{
    const timed_line_t *t = arg;
    dgx_font_draw(t->ctx, t->x, t->y, line);
}

static void picture_again(void *arg)
{
    (void)arg;
    draw_picture();
}

static void picture_text(void *arg)
{
    (void)arg;
    draw_picture();
    draw_over_picture();
}

/* the ways of drawing the line of 16 px, each over its own band: the times to the log */
static void log_times(dgx_screen_t *lcd)
{
    const uint32_t paper = dgx_rgb_to_16(240, 236, 224), ink = dgx_rgb_to_16(20, 20, 20);
    const dgx_font_context_t ways[] = {
        {.scr = lcd, .font = dgx_font_hard(), .color = ink},
        {.scr = lcd, .font = dgx_font_soft(), .color = ink},
        {.scr = lcd, .font = dgx_font_soft(), .color = ink, .bg = paper, .flags = DGX_FONT_BG},
        {.scr = lcd, .font = dgx_font_soft(), .color = ink, .bg = paper, .flags = DGX_FONT_FILL},
    };
    static const char *const names[] = {"one bit", "4 bits, cut as one bit (no paper)", "4 bits, DGX_FONT_BG", "4 bits, DGX_FONT_FILL"};
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, paper);
    for (size_t i = 0; i < sizeof ways / sizeof ways[0]; ++i) {
        timed_line_t t = {.ctx = &ways[i], .x = 8, .y = 30 + 40 * (int)i};
        ESP_LOGI(TAG, "TIME %-36s %6" PRId64 " us a line on the display", names[i], mean_us(20, draw_line, &t));
    }
    const int64_t picture_alone = mean_us(10, picture_again, NULL);
    const int64_t with_text     = mean_us(10, picture_text, NULL);
    ESP_LOGI(TAG, "TIME %-36s %6" PRId64 " us a line on a 16-bit virtual screen", "4 bits, mixed with the picture", with_text - picture_alone);
}

void app_main(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(LCD_HOST, SPI_DMA_CH_AUTO, (gpio_num_t)PIN_MOSI, (gpio_num_t)PIN_MISO, (gpio_num_t)PIN_CLK,
                                            (gpio_num_t)PIN_CS, (gpio_num_t)PIN_DC, SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return;
    }
    dgx_screen_t *lcd = dgx_ili9341_init(bus, (gpio_num_t)PIN_RST, (gpio_num_t)PIN_BACKLIGHT, 16, DgxScreenRGB);
    if (!lcd) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return;
    }
    dgx_ili9341_orientation(lcd, DgxScreenRightLeft, DgxScreenTopBottom, true); /* landscape, 320x240 */
    picture = dgx_vscreen_init(150, 48, 16, DgxScreenRGB);
    if (!picture) {
        ESP_LOGE(TAG, "no memory for the picture");
        return;
    }

    log_times(lcd);

    draw_band(lcd, 0, dgx_rgb_to_16(240, 236, 224), dgx_rgb_to_16(20, 20, 20), dgx_rgb_to_16(130, 128, 122));
    draw_band(lcd, 96, dgx_rgb_to_16(16, 24, 48), dgx_rgb_to_16(240, 240, 240), dgx_rgb_to_16(128, 132, 144));
    const uint32_t blue = dgx_rgb_to_16(0, 40, 90);
    dgx_fill_rectangle(lcd, 0, 192, 170, 48, blue);
    draw_picture();
    draw_over_picture();
    dgx_vscreen_to_screen(lcd, 170, 192, picture, 0);
    if (EXAMPLE_BUILD) {
        char label[16];
        snprintf(label, sizeof label, "#%d", EXAMPLE_BUILD);
        const dgx_font_context_t mark = {.scr = lcd, .font = dgx_font_small(), .color = dgx_rgb_to_16(200, 60, 60), .bg = dgx_rgb_to_16(240, 236, 224),
                                         .flags = DGX_FONT_BG};
        dgx_font_draw(&mark, lcd->width - 4 - dgx_font_bounds(&mark, label, NULL, NULL), 12, label);
    }

    /* the counter: its cell filled with the paper each time, the old digits gone, no flicker of an erase */
    const dgx_font_context_t count = {.scr = lcd, .font = dgx_font_count(), .color = dgx_rgb_to_16(255, 200, 0), .bg = blue, .flags = DGX_FONT_FILL};
    for (uint32_t tenths = 0;; ++tenths) {
        char text[16];
        snprintf(text, sizeof text, "%" PRIu32 ".%" PRIu32, tenths / 10 % 10000, tenths % 10);
        dgx_font_draw(&count, 8, 232, text);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
