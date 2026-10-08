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
#include "dgx_hw_font.h"
#include "dgx_screen.h"
#include "drivers/ili9341.h"
#include "fonts/font0ss.h"

static const char *TAG = "dgx_hw_font_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;

/* Texts shown in turn. The lines of a text are written one under another, each in the middle of the screen. */
#define LINES 3
static const char *const texts[][LINES] = {
    {"привет", "участникам", "соревнований"},
    {"Greetings", "to all the", "contestants"},
};
#define TEXTS (int)(sizeof(texts) / sizeof(texts[0]))

/* Joined writing; false writes every letter apart. */
#define EXAMPLE_JOINED true
/* The pen goes through so much effort a second: about so many cells of the grid of the font. */
#define EXAMPLE_TEMPO 1500.0f
/* What every straight piece of a curve adds to its length: the pen slows down in bends. */
#define EXAMPLE_BEND_EFFORT 6.0f
/* The pen is this many cells thick, and the screen keeps this many pixels free at its edges. */
#define EXAMPLE_PEN_CELLS 9.0f
#define EXAMPLE_MARGIN 6
/* The finished text stays so long before it is written again. */
#define EXAMPLE_HOLD_MS 4000

typedef struct {
    int left, top, right, bottom; /* the box of the line in cells */
    int x, y;                     /* where it starts on the screen */
} placed_line_t;

/*
 * The largest size at which all the lines fit: the widest one into the width
 * of the screen, all of them, a line height of the font apart, into its
 * height. The pen takes room too, and its thickness follows the size.
 */
static bool fit_lines(const dgx_screen_t *screen, dgx_font_t *font, const char *const *lines, placed_line_t *placed, float *scale,
                      int *pen)
{
    int widest = 0;
    for (int i = 0; i < LINES; ++i) {
        if (!dgx_hw_text_box(font, lines[i], &placed[i].left, &placed[i].top, &placed[i].right, &placed[i].bottom)) return false;
        int width = placed[i].right - placed[i].left + 1;
        if (width > widest) widest = width;
    }
    int pitch = font->hw->symbol_size_y;
    int height = (LINES - 1) * pitch + placed[LINES - 1].bottom - placed[0].top + 1;
    *pen = 1;
    for (int pass = 0; pass < 2; ++pass) {
        float by_width = (float)(screen->width - 2 * EXAMPLE_MARGIN - *pen) / widest;
        float by_height = (float)(screen->height - 2 * EXAMPLE_MARGIN - *pen) / height;
        *scale = by_width < by_height ? by_width : by_height;
        *pen = (int)lroundf(*scale * EXAMPLE_PEN_CELLS);
        if (*pen < 1) *pen = 1;
    }
    if (!(*scale > 0)) return false;
    int first_base = (int)lroundf((screen->height - height * *scale) / 2 - placed[0].top * *scale);
    for (int i = 0; i < LINES; ++i) {
        int width = placed[i].right - placed[i].left + 1;
        placed[i].x = (int)lroundf((screen->width - width * *scale) / 2 - placed[i].left * *scale);
        placed[i].y = first_base + (int)lroundf(i * pitch * *scale);
    }
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting handwritten font demo on CYD");
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
    uint32_t paper = dgx_rgb_to_16(250, 246, 232);
    uint32_t ink = dgx_rgb_to_16(24, 36, 120);

    dgx_font_t   *font = font0ss();
    dgx_hw_pace_t pace = dgx_hw_pace(font, EXAMPLE_BEND_EFFORT);

    for (int round = 0;; ++round) {
        const char *const *lines = texts[round % TEXTS];
        placed_line_t      placed[LINES];
        float              scale;
        int                pen;
        if (!fit_lines(screen, font, lines, placed, &scale, &pen)) {
            ESP_LOGE(TAG, "The font has no symbols of the text");
            return;
        }
        ESP_LOGI(TAG, "Screen %dx%d: %.3f pixels a cell, a line %d pixels high, pen %d", screen->width, screen->height, scale,
                 (int)lroundf(font->hw->symbol_size_y * scale), pen);
        dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, paper);
        int64_t round_us = esp_timer_get_time();
        for (int i = 0; i < LINES; ++i) {
            dgx_hw_writing_t writing;
            dgx_hw_writing_begin(&writing, screen, placed[i].x, placed[i].y, font, lines[i], scale, pen, ink, EXAMPLE_JOINED, &pace);
            /* the pen goes to the next line through the air, as between two strokes */
            int64_t start_us = esp_timer_get_time() + (int64_t)(i ? pace.least / EXAMPLE_TEMPO * 1e6f : 0);
            int64_t drawing_us = 0;
            int     calls = 0;
            bool    finished = false;
            while (!finished) {
                int64_t now_us = esp_timer_get_time();
                if (now_us > start_us) {
                    finished = dgx_hw_writing_draw_to(&writing, EXAMPLE_TEMPO * (now_us - start_us) * 1e-6f);
                    drawing_us += esp_timer_get_time() - now_us;
                    ++calls;
                }
                vTaskDelay(1);
            }
            ESP_LOGI(TAG, "\"%s\": effort %.0f, written in %.2f s, drawing took %lld us in %d calls", lines[i],
                     dgx_hw_text_effort(font, lines[i], EXAMPLE_JOINED, &pace), (esp_timer_get_time() - start_us) * 1e-6f, drawing_us,
                     calls);
        }
        ESP_LOGI(TAG, "All written in %.2f s", (esp_timer_get_time() - round_us) * 1e-6f);
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_HOLD_MS));
    }
}
