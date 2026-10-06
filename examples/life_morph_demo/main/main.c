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
#include "dgx_matrix_morph.h"
#include "dgx_morph.h"
#include "dgx_morph_render.h"
#include "dgx_screen.h"
#include "drivers/ili9341.h"

static const char *TAG = "dgx_life_morph_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST
/* One generation morphs into the next over this time, in microseconds. */
#define GENERATION_US 1000000LL
#define MIN_CELL_WIDTH 3

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;
/* BOOT button of the CYD: switches to the next pattern. */
static const int EXAMPLE_BUTTON_PIN_NUM = 0;

/* ------------------------------------------------------------------ */
/* Seeds                                                                */
/* ------------------------------------------------------------------ */

typedef dgx_bit_matrix_t *(*seed_func_t)(void);

static dgx_bit_matrix_t *seed_from_cells(int width, int height, const int8_t cells[][2], size_t count)
{
    dgx_bit_matrix_t *life = dgx_matrix_init((uint16_t)width, (uint16_t)height);
    if (!life) return NULL;
    for (size_t i = 0; i < count; ++i) {
        dgx_matrix_set_point(life, cells[i][0], cells[i][1], true);
    }
    return life;
}

#define SEED(width, height, cells) seed_from_cells(width, height, cells, sizeof(cells) / sizeof((cells)[0]))

/* Four gliders fly in from the corners and collide into four blocks. */
static dgx_bit_matrix_t *seed_gliders(void)
{
    static const int8_t cells[][2] = {
        {1, 0}, {2, 1}, {0, 2}, {1, 2},  {2, 2}, {8, 0}, {7, 1}, {7, 2}, {8, 2}, {9, 2},
        {1, 10}, {2, 9}, {0, 8}, {1, 8}, {2, 8}, {7, 8}, {8, 8}, {9, 8}, {7, 9}, {8, 10},
    };
    return SEED(10, 11, cells);
}

/* T-tetromino: grows, oscillates and settles. */
static dgx_bit_matrix_t *seed_navy(void)
{
    static const int8_t cells[][2] = {{4, 3}, {3, 4}, {4, 4}, {5, 4}};
    return SEED(9, 9, cells);
}

/* Period 2: two blocks blinking at their touching corners. */
static dgx_bit_matrix_t *seed_beacon(void)
{
    static const int8_t cells[][2] = {{1, 1}, {2, 1}, {1, 2}, {4, 3}, {3, 4}, {4, 4}};
    return SEED(6, 6, cells);
}

/* Period 2: two shifted rows of three. */
static dgx_bit_matrix_t *seed_toad(void)
{
    static const int8_t cells[][2] = {{2, 1}, {3, 1}, {4, 1}, {1, 2}, {2, 2}, {3, 2}};
    return SEED(6, 4, cells);
}

/* Period 3. Two 3-cell arm segments, four rotations and a diagonal mirror give the whole shape. */
static dgx_bit_matrix_t *seed_pulsar(void)
{
    static const int8_t arms[2][3][2] = {
        {{-4, -6}, {-3, -6}, {-2, -6}},
        {{-1, -4}, {-1, -3}, {-1, -2}},
    };
    const int side = 15;
    const int center = side / 2;
    dgx_bit_matrix_t *life = dgx_matrix_init(side, side);
    if (!life) return NULL;
    for (int arm = 0; arm < 2; ++arm) {
        for (int i = 0; i < 3; ++i) {
            int x = arms[arm][i][0];
            int y = arms[arm][i][1];
            for (int rotation = 0; rotation < 4; ++rotation) {
                dgx_matrix_set_point(life, center + x, center + y, true);
                dgx_matrix_set_point(life, center + y, center + x, true);
                int t = x;
                x = -y;
                y = t;
            }
        }
    }
    return life;
}

/* Five cells that take a long time to settle. */
static dgx_bit_matrix_t *seed_rpentomino(void)
{
    static const int8_t cells[][2] = {{13, 9}, {14, 9}, {12, 10}, {13, 10}, {13, 11}};
    return SEED(30, 25, cells);
}

static const struct {
    const char *name;
    seed_func_t seed;
} patterns[] = {
    {"gliders", seed_gliders},
    {"navy", seed_navy},
    {"beacon", seed_beacon},
    {"toad", seed_toad},
    {"pulsar", seed_pulsar},
    {"r-pentomino", seed_rpentomino},
};

#define PATTERN_COUNT (sizeof(patterns) / sizeof(patterns[0]))

/* ------------------------------------------------------------------ */
/* Rules                                                                */
/* ------------------------------------------------------------------ */

/* Conway's rules on a finite field: cells outside it are dead. */
static dgx_bit_matrix_t *next_generation(const dgx_bit_matrix_t *now)
{
    dgx_bit_matrix_t *next = dgx_matrix_init(now->width, now->height);
    if (!next) return NULL;
    for (int y = 0; y < now->height; ++y) {
        for (int x = 0; x < now->width; ++x) {
            int neighbors = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    /* dgx_matrix_get_point() returns false outside the matrix */
                    if ((dx || dy) && dgx_matrix_get_point(now, x + dx, y + dy)) ++neighbors;
                }
            }
            bool alive = dgx_matrix_get_point(now, x, y);
            dgx_matrix_set_point(next, x, y, neighbors == 3 || (alive && neighbors == 2));
        }
    }
    return next;
}

/* ------------------------------------------------------------------ */
/* Scene                                                                */
/* ------------------------------------------------------------------ */

static dgx_bit_matrix_t *now;
static dgx_morph_glow_t *glow;
static size_t pattern;
static int cell_width;
static int origin_x;
static int origin_y;

static void release_scene(void)
{
    dgx_matrix_destroy(&now);
    dgx_morph_glow_destroy(&glow);
}

/* Seed the pattern and create a glow renderer with the largest cell that fits the screen and the heap. */
static bool start_pattern(dgx_screen_t *screen, size_t index)
{
    release_scene();
    pattern = index;
    now = patterns[pattern].seed();
    if (!now) return false;

    int cell = DGX_MIN(screen->width / now->width, screen->height / now->height);
    /* An odd cell keeps the dot centered on a pixel. */
    if (cell % 2 == 0) --cell;
    for (; cell >= MIN_CELL_WIDTH; cell -= 2) {
        glow = dgx_morph_glow_create(now->width * cell, now->height * cell, cell, screen->color_bits);
        if (glow) break;
        ESP_LOGW(TAG, "No memory for cell %d px, trying a smaller one", cell);
    }
    if (!glow) {
        dgx_matrix_destroy(&now);
        return false;
    }
    cell_width = cell;
    origin_x = (screen->width - now->width * cell) / 2;
    origin_y = (screen->height - now->height * cell) / 2;
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(dgx_rgb_to_16));
    ESP_LOGI(TAG, "Pattern %s: %dx%d cells, cell %d px", patterns[pattern].name, now->width, now->height, cell_width);
    return true;
}

/* True once per press: the button has to be released before the next one counts. */
static bool button_pressed(void)
{
    static bool was_down;
    bool down = gpio_get_level((gpio_num_t)EXAMPLE_BUTTON_PIN_NUM) == 0;
    bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Game of Life morph demo on CYD");
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

    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << EXAMPLE_BUTTON_PIN_NUM,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&button_config);
    button_pressed(); /* a button held at boot is not a press */

    if (!start_pattern(screen, 0)) {
        ESP_LOGE(TAG, "Unable to allocate the first pattern");
        return;
    }

    uint32_t generation = 0;
    uint32_t frames = 0;
    int64_t fps_start_us = esp_timer_get_time();
    while (true) {
        dgx_bit_matrix_t *next = next_generation(now);
        if (next && (next->number_of_set_cells == 0 || dgx_matrix_equals(now, next))) {
            /* Extinct or a still life: morph back to the seed. */
            ESP_LOGI(TAG, "%s after %" PRIu32 " generations, restarting",
                     next->number_of_set_cells == 0 ? "Extinct" : "Still life", generation);
            dgx_matrix_destroy(&next);
            next = patterns[pattern].seed();
            generation = 0;
        }
        /* The plan is made once per generation; every live neighbor of a newborn cell is its parent. */
        dgx_morph_t *morph = next ? dgx_morph_create(now, next, dgx_morph_sources_life, NULL) : NULL;
        if (!morph) {
            ESP_LOGE(TAG, "Unable to allocate the next generation");
            dgx_matrix_destroy(&next);
            break;
        }
        ++generation;

        bool switch_pattern = false;
        int64_t start_us = esp_timer_get_time();
        float t;
        do {
            int64_t now_us = esp_timer_get_time();
            t = dgx_morph_progress(start_us, now_us, GENERATION_US);
            dgx_morph_draw(morph, t, 0, 0, cell_width, true, dgx_morph_glow_dot, glow);
            dgx_morph_glow_present(glow, t, screen, origin_x, origin_y);
            ++frames;
            if (now_us - fps_start_us >= 1000000) {
                ESP_LOGI(TAG, "FPS: %.1f, generation %" PRIu32, frames * 1000000.0f / (float)(now_us - fps_start_us), generation);
                fps_start_us = now_us;
                frames = 0;
            }
            switch_pattern = button_pressed();
        } while (t < 1.0f && !switch_pattern);
        dgx_morph_destroy(&morph);
        vTaskDelay(1); /* let the idle task run */

        if (switch_pattern) {
            dgx_matrix_destroy(&next);
            generation = 0;
            if (!start_pattern(screen, (pattern + 1) % PATTERN_COUNT)) {
                ESP_LOGE(TAG, "Unable to allocate the next pattern");
                break;
            }
            continue;
        }
        dgx_matrix_destroy(&now);
        now = next;
    }
    release_scene();
}
