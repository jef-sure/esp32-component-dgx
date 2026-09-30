#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

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

static const char *TAG = "dgx_morph_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST
#define DEMO_MAX_LETTERS 16
#define WORD_COUNT 4
/* Durations are in microseconds. */
#define MORPH_DURATION_US 500000LL
#define LETTER_STAGGER_US 250000LL
#define WORD_PAUSE_US 2000000LL

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;

static const char *const words[WORD_COUNT] = {
    /* The first state is blank; shorter words are padded with spaces. */
    "           ",
    "привет",
    "участникам",
    "соревнований",
};

typedef struct {
    /* Each letter keeps its own timeline and phosphor. */
    dgx_morph_t *morph;
    dgx_morph_glow_t *glow;
} letter_slot_t;

static uint32_t word_codepoints[WORD_COUNT][DEMO_MAX_LETTERS];
static size_t word_lengths[WORD_COUNT];
static size_t letter_count;
static dgx_font_t *font;
static letter_slot_t letters[DEMO_MAX_LETTERS];
static uint32_t current_codepoints[DEMO_MAX_LETTERS];
static uint32_t target_codepoints[DEMO_MAX_LETTERS];
static int glyph_width;
static int glyph_height;
static int cell_width;
static int glow_radius;
static int slot_width;
static int slot_height;
static int line_x;
static int line_y;
static int64_t transition_start_us;
static int64_t transition_finish_us;
static int64_t pause_until_us;
static size_t current_word;
static size_t target_word;
static size_t active_letter_count;
static bool in_pause;

static bool decode_words(void)
{
    letter_count = 0;
    for (size_t word = 0; word < WORD_COUNT; ++word) {
        size_t byte_index = 0;
        size_t count = 0;
        while (words[word][byte_index]) {
            if (count >= DEMO_MAX_LETTERS) return false;
            uint32_t cp = decodeUTF8next(words[word], &byte_index);
            int16_t advance;
            if (!dgx_font_find_glyph(cp, font, &advance)) {
                ESP_LOGE(TAG, "Font is missing U+%04" PRIX32, cp);
                return false;
            }
            word_codepoints[word][count++] = cp;
        }
        word_lengths[word] = count;
        if (count > letter_count) letter_count = count;
    }
    if (!letter_count || letter_count > DEMO_MAX_LETTERS) return false;

    for (size_t word = 0; word < WORD_COUNT; ++word) {
        for (size_t i = word_lengths[word]; i < letter_count; ++i) {
            word_codepoints[word][i] = ' ';
        }
    }
    return true;
}

static bool choose_cell_width(const dgx_screen_t *screen)
{
    glyph_width = font->xRightMax - font->xOffsetLowest;
    glyph_height = font->yBottomMax - font->yOffsetLowest;
    if (glyph_width <= 0 || glyph_height <= 0) return false;

    for (int candidate = screen->width; candidate > 0; --candidate) {
        int radius = candidate / 2 + candidate / 4;
        if (radius < 1) radius = 1;
        /* Keep the glow disk and its margin inside each letter slot. */
        int width = glyph_width * candidate + 2 * radius;
        int height = glyph_height * candidate + 2 * radius;
        if (width * (int)letter_count <= screen->width && height <= screen->height) {
            cell_width = candidate;
            glow_radius = radius;
            slot_width = width;
            slot_height = height;
            line_x = (screen->width - slot_width * (int)letter_count) / 2;
            line_y = (screen->height - slot_height) / 2;
            return true;
        }
    }
    return false;
}

static dgx_bit_matrix_t *matrix_for(uint32_t codepoint)
{
    if (codepoint == ' ') return dgx_matrix_init((uint16_t)glyph_width, (uint16_t)glyph_height);
    return dgx_morph_glyph_matrix(font, codepoint);
}

static void destroy_transition(void)
{
    for (size_t i = 0; i < letter_count; ++i) {
        dgx_morph_destroy(&letters[i].morph);
    }
}

static bool make_transition(size_t next_word, int64_t start_us)
{
    destroy_transition();
    target_word = next_word;
    transition_start_us = start_us;
    active_letter_count = 0;

    for (size_t i = 0; i < letter_count; ++i) {
        target_codepoints[i] = word_codepoints[target_word][i];
        /* Trailing unchanged spaces do not extend the word's transition. */
        if (target_codepoints[i] != current_codepoints[i]) active_letter_count = i + 1;
        dgx_bit_matrix_t *from = matrix_for(current_codepoints[i]);
        dgx_bit_matrix_t *to = matrix_for(target_codepoints[i]);
        if (!from || !to) {
            dgx_matrix_destroy(&from);
            dgx_matrix_destroy(&to);
            destroy_transition();
            return false;
        }
        letters[i].morph = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
        dgx_matrix_destroy(&from);
        dgx_matrix_destroy(&to);
        if (!letters[i].morph) {
            destroy_transition();
            return false;
        }
    }

    transition_finish_us = transition_start_us +
        (int64_t)(active_letter_count ? active_letter_count - 1 : 0) * LETTER_STAGGER_US + MORPH_DURATION_US;
    return true;
}

static void render_frame(dgx_screen_t *screen, int64_t now_us)
{
    for (size_t i = 0; i < letter_count; ++i) {
        /* Offset each start time so the letters morph from left to right. */
        int64_t letter_start_us = transition_start_us + (int64_t)i * LETTER_STAGGER_US;
        float t = dgx_morph_progress(letter_start_us, now_us, MORPH_DURATION_US);
        int slot_x = line_x + (int)i * slot_width;
        dgx_morph_draw(letters[i].morph, t, glow_radius, glow_radius, cell_width,
                       true, dgx_morph_glow_dot, letters[i].glow);
        dgx_morph_glow_present(letters[i].glow, t, screen, slot_x, line_y);
    }
}

static bool create_renderers(void)
{
    uint32_t lut[256];
    for (int i = 0; i < 256; ++i) {
        uint8_t level = (uint8_t)i;
        lut[i] = dgx_rgb_to_16((uint8_t)(level / 3), (uint8_t)(level * 3 / 4), level);
    }

    for (size_t i = 0; i < letter_count; ++i) {
        letters[i].glow = dgx_morph_glow_create(slot_width, slot_height, cell_width, 16);
        if (!letters[i].glow) return false;
        dgx_morph_glow_set_lut(letters[i].glow, lut);
    }
    return true;
}

static void release_resources(void)
{
    destroy_transition();
    for (size_t i = 0; i < letter_count; ++i) {
        dgx_morph_glow_destroy(&letters[i].glow);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting sequential word morph demo on CYD");
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

    font = TerminusTTFMedium12();
    if (!decode_words() || !choose_cell_width(screen)) {
        ESP_LOGE(TAG, "Unable to fit demo words and font on screen");
        return;
    }
    ESP_LOGI(TAG, "Words=%u, glyph=%dx%d cells, cell=%d, line=%dx%d px",
             (unsigned)letter_count, glyph_width, glyph_height, cell_width,
             slot_width * (int)letter_count, slot_height);

    for (size_t i = 0; i < letter_count; ++i) current_codepoints[i] = ' ';
    if (!create_renderers()) {
        ESP_LOGE(TAG, "Unable to allocate morph renderers");
        release_resources();
        return;
    }

    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(dgx_rgb_to_16));
    vTaskDelay(pdMS_TO_TICKS(500));

    current_word = 0;
    if (!make_transition(1, esp_timer_get_time())) {
        ESP_LOGE(TAG, "Unable to create initial word transition");
        release_resources();
        return;
    }

    while (true) {
        int64_t now_us = esp_timer_get_time();
        render_frame(screen, now_us);

        if (!in_pause && now_us >= transition_finish_us) {
            for (size_t i = 0; i < letter_count; ++i) {
                current_codepoints[i] = target_codepoints[i];
            }
            current_word = target_word;
            in_pause = true;
            pause_until_us = transition_finish_us + WORD_PAUSE_US;
        } else if (in_pause && now_us >= pause_until_us) {
            size_t next_word = (current_word + 1) % WORD_COUNT;
            in_pause = false;
            if (!make_transition(next_word, now_us)) {
                ESP_LOGE(TAG, "Unable to create word transition");
                release_resources();
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
