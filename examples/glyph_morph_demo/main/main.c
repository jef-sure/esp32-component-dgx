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
#include "fonts/WeatherIconsRegular27.h"

static const char *TAG = "dgx_glyph_morph_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST
/* Durations are in microseconds. */
#define MORPH_DURATION_US 1000000LL
#define SYMBOL_PAUSE_US 1000000LL
/*
 * What becomes of an old cell nobody took: DGX_MORPH_ORPHANS_FADE melts where
 * it is, DGX_MORPH_ORPHANS_MERGE flies into the nearest cell that stays and
 * goes out there; the bottom bar of "E" is then swept into the stem of "F".
 */
#define MORPH_ORPHANS DGX_MORPH_ORPHANS_MERGE
/* how far such a cell may go, in cells along its figure or across; 0 is no limit */
#define MORPH_MERGE_RADIUS 8

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;

/*
 * Shown one after another, in three sequences: digits of a bitmap font, the
 * first six to compare the orphan modes on, E to F, 8 to 3, 0 to 1; the
 * weather symbols a clock shows, each by day and by night; the letters of the
 * bitmap font. Each sequence has a renderer of its own, since every glyph of a
 * font is rasterized into the same box; it comes from nothing, dots gathering
 * from the center, and goes back into it.
 */
static const uint32_t digits[] = { 'E', 'F', '8', '3', '0', '1', '2', '4', '5', '6', '7', '9' };
static const uint32_t letters[] = {
    'A', 0xc4, 'B', 'C', 'D', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 0xd6, 'P', 'Q', 'R', 'S', 'T', 'U', 0xdc, 'V', 'W', 'X', 'Y', 'Z',
    'a', 0xe4, 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 0xf6, 'p', 'q', 'r', 's', 0xdf, 't', 'u', 0xfc, 'v', 'w', 'x', 'y', 'z',
    0x410, 0x411, 0x412, 0x413, 0x414, 0x415, 0x416, 0x417, 0x418, 0x419, 0x41a, 0x41b, 0x41c, 0x41d, 0x41e, 0x41f,
    0x420, 0x421, 0x422, 0x423, 0x424, 0x425, 0x426, 0x427, 0x428, 0x429, 0x42a, 0x42b, 0x42c, 0x42d, 0x42e, 0x42f,
    0x430, 0x431, 0x432, 0x433, 0x434, 0x435, 0x451, 0x436, 0x437, 0x438, 0x439, 0x43a, 0x43b, 0x43c, 0x43d, 0x43e, 0x43f,
    0x440, 0x441, 0x442, 0x443, 0x444, 0x445, 0x446, 0x447, 0x448, 0x449, 0x44a, 0x44b, 0x44c, 0x44d, 0x44e, 0x44f,
};
/*
 * Each weather by day and then by night, so that one transition is between
 * the two alike symbols of a weather and the next between neighbouring
 * weathers: clear, few clouds, clouds, drizzle, rain, showers, thunderstorm,
 * snow, sleet, fog; then the symbols that are the same by day and by night.
 */
static const uint32_t weather[] = {
    0xf00d, 0xf02e, 0xf00c, 0xf083, 0xf002, 0xf031, 0xf008, 0xf036, 0xf00b, 0xf039, 0xf009, 0xf037, 0xf010, 0xf03b, 0xf00a, 0xf038, 0xf0b2, 0xf0b3, 0xf003, 0xf014,
    0xf062, 0xf0b6, 0xf063, 0xf082, 0xf0c8, 0xf021, 0xf056,
};

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

static const dgx_morph_options_t options = { MORPH_ORPHANS, MORPH_MERGE_RADIUS };

/* one transition, drawn until its time is over; the last frame stays while the symbol is held */
static bool morph_once(dgx_screen_t *screen, const dgx_bit_matrix_t *from, const dgx_bit_matrix_t *to)
{
    /* A new cell takes a free neighbor as its source, otherwise the nearest free cell around. */
    dgx_morph_t *morph = dgx_morph_create_with(from, to, dgx_morph_sources_cells, NULL, &options);
    if (!morph) return false;
    uint32_t frames = 0;
    int64_t start_us = esp_timer_get_time(), fps_start_us = start_us;
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
    vTaskDelay(pdMS_TO_TICKS(SYMBOL_PAUSE_US / 1000));
    return true;
}

/* the symbols of one font one after another, from nothing and back into it */
static void run_sequence(dgx_screen_t *screen, dgx_font_t *font, const uint32_t *codes, size_t count)
{
    dgx_bit_matrix_t *from = dgx_morph_glyph_matrix(font, codes[0]);
    if (!from || !create_renderer(screen, from->width, from->height)) {
        ESP_LOGE(TAG, "Unable to fit the font on screen or allocate the renderer");
        dgx_matrix_destroy(&from);
        return;
    }
    ESP_LOGI(TAG, "Glyph box %dx%d cells, cell %d px, %u symbols; old cells nobody took %s, radius %d", from->width, from->height, cell_width,
             (unsigned)count, options.orphans == DGX_MORPH_ORPHANS_MERGE ? "merge into the nearest cell that stays" : "fade where they are",
             options.merge_radius);
    bool ok = morph_once(screen, NULL, from);
    for (size_t i = 1; ok && i < count; ++i) {
        dgx_bit_matrix_t *to = dgx_morph_glyph_matrix(font, codes[i]);
        ok = to && morph_once(screen, from, to);
        if (!ok) ESP_LOGE(TAG, "Unable to morph U+%04" PRIX32 " -> U+%04" PRIX32, codes[i - 1], codes[i]);
        dgx_matrix_destroy(&from);
        from = to;
    }
    if (ok) morph_once(screen, from, NULL);
    dgx_matrix_destroy(&from);
    dgx_morph_glow_destroy(&glow);
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

    /* digits of a bitmap font, the weather symbols a clock shows, the letters, and again */
    while (true) {
        run_sequence(screen, TerminusTTFMedium12(), digits, sizeof(digits) / sizeof(digits[0]));
        run_sequence(screen, WeatherIconsRegular27(), weather, sizeof(weather) / sizeof(weather[0]));
        run_sequence(screen, TerminusTTFMedium12(), letters, sizeof(letters) / sizeof(letters[0]));
    }
}
