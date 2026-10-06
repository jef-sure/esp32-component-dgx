#include <inttypes.h>
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
#include "dgx_matrix_morph.h"
#include "dgx_morph.h"
#include "dgx_morph_render.h"
#include "dgx_morph_sources.h"
#include "dgx_screen.h"
#include "drivers/ili9341.h"
#include "fonts/TerminusTTFMedium12.h"

static const char *TAG = "dgx_glyph_morph_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST
/* Durations are in microseconds. */
#define MORPH_DURATION_US 1000000LL
#define SYMBOL_PAUSE_US 1000000LL

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;

/* Shown one after another in a loop. A symbol the font lacks is an empty matrix: dots gather in the center. */
static const char symbols[] = "0123456789 AÄBCDEFGHIJKLMNOÖPQRSTUÜVWXYZaäbcdefghijklmnoöpqrsßtuüvwxyz "
                              "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдеёжзийклмнопрстуфхцчшщъыьэюя";

static dgx_morph_glow_t *glow;
static int cell_width;
static int glow_radius;
static int origin_x;
static int origin_y;

/*
 * Every glyph of a font is rasterized into the same box, so one renderer
 * serves all symbols. Take the largest cell that fits the screen together
 * with the glow margin, then shrink it until the renderer fits the heap.
 */
static bool create_renderer(const dgx_screen_t *screen, int columns, int rows)
{
    for (int cell = DGX_MIN(screen->width / columns, screen->height / rows); cell > 0; --cell) {
        /* Same radius as the glow renderer uses for its discs. */
        int radius = DGX_MAX(cell / 2 + cell / 4, 2);
        int width = columns * cell + 2 * radius;
        int height = rows * cell + 2 * radius;
        if (width > screen->width || height > screen->height) continue;
        glow = dgx_morph_glow_create(width, height, cell, screen->color_bits);
        if (!glow) {
            ESP_LOGW(TAG, "No memory for cell %d px, trying a smaller one", cell);
            continue;
        }
        cell_width = cell;
        glow_radius = radius;
        origin_x = (screen->width - width) / 2;
        origin_y = (screen->height - height) / 2;
        return true;
    }
    return false;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting glyph morph demo on CYD");
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
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(dgx_rgb_to_16));

    /* Any other DGX font works: dot fonts and bitmap fonts both become matrices. */
    dgx_font_t *font = TerminusTTFMedium12();
    size_t idx = 0;
    uint32_t code_point = decodeUTF8next(symbols, &idx);
    dgx_bit_matrix_t *from = dgx_morph_glyph_matrix(font, code_point);
    if (!from || !create_renderer(screen, from->width, from->height)) {
        ESP_LOGE(TAG, "Unable to fit the font on screen or allocate the renderer");
        dgx_matrix_destroy(&from);
        return;
    }
    ESP_LOGI(TAG, "Glyph box %dx%d cells, cell %d px", from->width, from->height, cell_width);

    uint32_t frames = 0;
    int64_t fps_start_us = esp_timer_get_time();
    while (true) {
        if (symbols[idx] == '\0') idx = 0;
        uint32_t next_code_point = decodeUTF8next(symbols, &idx);
        dgx_bit_matrix_t *to = dgx_morph_glyph_matrix(font, next_code_point);
        /* A new cell takes a free neighbor as its source, otherwise the nearest free cell around. */
        dgx_morph_t *morph = to ? dgx_morph_create(from, to, dgx_morph_sources_cells, NULL) : NULL;
        if (!morph) {
            ESP_LOGE(TAG, "Unable to create morph U+%04" PRIX32 " -> U+%04" PRIX32, code_point, next_code_point);
            dgx_matrix_destroy(&to);
            break;
        }

        int64_t start_us = esp_timer_get_time();
        float t;
        do {
            int64_t now_us = esp_timer_get_time();
            t = dgx_morph_progress(start_us, now_us, MORPH_DURATION_US);
            dgx_morph_draw(morph, t, glow_radius, glow_radius, cell_width, true, dgx_morph_glow_dot, glow);
            dgx_morph_glow_present(glow, t, screen, origin_x, origin_y);
            ++frames;
            if (now_us - fps_start_us >= 1000000) {
                ESP_LOGI(TAG, "FPS: %.1f", frames * 1000000.0f / (float)(now_us - fps_start_us));
                fps_start_us = now_us;
                frames = 0;
            }
            vTaskDelay(1);
        } while (t < 1.0f);
        dgx_morph_destroy(&morph);

        /* The last frame stays on screen while the symbol is held. */
        vTaskDelay(pdMS_TO_TICKS(SYMBOL_PAUSE_US / 1000));
        frames = 0;
        fps_start_us = esp_timer_get_time();

        dgx_matrix_destroy(&from);
        from = to;
        code_point = next_code_point;
    }
    dgx_matrix_destroy(&from);
    dgx_morph_glow_destroy(&glow);
}
