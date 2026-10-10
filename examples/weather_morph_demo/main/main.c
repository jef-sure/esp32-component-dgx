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
#include "fonts/WeatherIconsRegular27.h"

static const char *TAG = "dgx_weather_morph_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST
/* Durations are in microseconds. */
#define MORPH_DURATION_US 1000000LL
#define SYMBOL_PAUSE_US 1000000LL
/* the screen stays empty for this long between two laps */
#define LAP_PAUSE_US 1000000LL
/*
 * What becomes of an old cell nobody took: DGX_MORPH_ORPHANS_FADE melts where
 * it is, DGX_MORPH_ORPHANS_MERGE flies into the nearest cell that stays and
 * goes out there.
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

typedef struct {
    uint32_t    code;
    const char *name;
} symbol_t;

/*
 * The weather symbols a clock shows, in the order weather itself may take.
 * Every step is one a clock really makes: either the weather turns into a
 * neighbouring one while the time of day stays, or the day turns into the
 * night, or back, while the weather stays. A day gathers clouds up to a
 * thunderstorm; in the night the rain cools down to snow; the next day it
 * thaws into fog; the next night the fog thins out to a clear sky.
 */
static const symbol_t by_weather[] = {
    { 0xf00d, "clear sky, day" },
    { 0xf00c, "few clouds, day" },
    { 0xf002, "clouds, day" },
    { 0xf008, "drizzle, day" },
    { 0xf00b, "heavy drizzle, day" },
    { 0xf009, "rain, day" },
    { 0xf010, "thunderstorm, day" },
    { 0xf03b, "thunderstorm, night" },
    { 0xf037, "rain, night" },
    { 0xf0b3, "sleet, night" },
    { 0xf038, "snow, night" },
    { 0xf00a, "snow, day" },
    { 0xf0b2, "sleet, day" },
    { 0xf003, "fog, day" },
    { 0xf014, "fog, night" },
    { 0xf039, "heavy drizzle, night" },
    { 0xf036, "drizzle, night" },
    { 0xf031, "clouds, night" },
    { 0xf083, "few clouds, night" },
    { 0xf02e, "clear sky, night" },
};

/*
 * The same symbols, each weather by day and then by night: every second
 * transition is between the two alike symbols of one weather, where the sun
 * turns into the moon and the rest stays, and the one after it goes on to the
 * next weather.
 */
static const symbol_t day_and_night[] = {
    { 0xf00d, "clear sky, day" },
    { 0xf02e, "clear sky, night" },
    { 0xf00c, "few clouds, day" },
    { 0xf083, "few clouds, night" },
    { 0xf002, "clouds, day" },
    { 0xf031, "clouds, night" },
    { 0xf003, "fog, day" },
    { 0xf014, "fog, night" },
    { 0xf008, "drizzle, day" },
    { 0xf036, "drizzle, night" },
    { 0xf00b, "heavy drizzle, day" },
    { 0xf039, "heavy drizzle, night" },
    { 0xf010, "thunderstorm, day" },
    { 0xf03b, "thunderstorm, night" },
    { 0xf009, "rain, day" },
    { 0xf037, "rain, night" },
    { 0xf0b2, "sleet, day" },
    { 0xf0b3, "sleet, night" },
    { 0xf00a, "snow, day" },
    { 0xf038, "snow, night" },
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
static bool morph_once(dgx_screen_t *screen, const dgx_bit_matrix_t *from, const dgx_bit_matrix_t *to, int64_t pause_us)
{
    /* A new cell takes a free neighbor as its source, otherwise the nearest free cell around. */
    dgx_morph_t *morph = dgx_morph_create_with(from, to, dgx_morph_sources_cells, NULL, &options);
    if (!morph) return false;
    int64_t start_us = esp_timer_get_time();
    float t;
    do {
        t = dgx_morph_progress(start_us, esp_timer_get_time(), MORPH_DURATION_US);
        dgx_morph_draw(morph, t, glow_radius, glow_radius, cell_width, true, dgx_morph_glow_dot, glow);
        dgx_morph_glow_present(glow, t, screen, origin_x, origin_y);
        vTaskDelay(1);
    } while (t < 1.0f);
    dgx_morph_destroy(&morph);
    vTaskDelay(pdMS_TO_TICKS(pause_us / 1000));
    return true;
}

/* one lap: the first symbol gathers from nothing, the last one goes back into it */
static bool run_lap(dgx_screen_t *screen, dgx_font_t *font, const char *title, const symbol_t *symbols, size_t count)
{
    dgx_bit_matrix_t *from = NULL;
    bool ok = true;
    ESP_LOGI(TAG, "A lap %s, %u symbols", title, (unsigned)count);
    for (size_t i = 0; ok && i < count; ++i) {
        dgx_bit_matrix_t *to = dgx_morph_glyph_matrix(font, symbols[i].code);
        ESP_LOGI(TAG, "U+%04" PRIX32 " %s", symbols[i].code, symbols[i].name);
        ok = to && morph_once(screen, from, to, SYMBOL_PAUSE_US);
        if (!ok) ESP_LOGE(TAG, "Unable to morph into U+%04" PRIX32, symbols[i].code);
        dgx_matrix_destroy(&from);
        from = to;
    }
    if (ok) ok = morph_once(screen, from, NULL, LAP_PAUSE_US);
    dgx_matrix_destroy(&from);
    return ok;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting weather morph demo on CYD");
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

    dgx_font_t *font = WeatherIconsRegular27();
    dgx_bit_matrix_t *box = dgx_morph_glyph_matrix(font, by_weather[0].code);
    if (!box || !create_renderer(screen, box->width, box->height)) {
        ESP_LOGE(TAG, "Unable to fit the font on screen or allocate the renderer");
        dgx_matrix_destroy(&box);
        return;
    }
    ESP_LOGI(TAG, "Glyph box %dx%d cells, cell %d px; old cells nobody took %s, radius %d", box->width, box->height, cell_width,
             options.orphans == DGX_MORPH_ORPHANS_MERGE ? "merge into the nearest cell that stays" : "fade where they are", options.merge_radius);
    dgx_matrix_destroy(&box);

    /* the two orders in turn */
    while (run_lap(screen, font, "by the way of the weather", by_weather, sizeof(by_weather) / sizeof(by_weather[0])) &&
           run_lap(screen, font, "by day and by night", day_and_night, sizeof(day_and_night) / sizeof(day_and_night[0]))) {
    }
}
