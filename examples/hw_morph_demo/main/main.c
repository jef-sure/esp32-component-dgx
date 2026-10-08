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
#include "drivers/vscreen.h"
#include "fonts/font0ss.h"

static const char *TAG = "dgx_hw_morph_demo";

#define EXAMPLE_LCD_HOST SPI2_HOST

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;

/* Shown one after another in a loop, each morphing into the next. */
static const char *const words[] = {"привет", "участникам", "соревнований", "Greetings", "to all the", "contestants"};
#define WORDS (int)(sizeof(words) / sizeof(words[0]))

#define EXAMPLE_JOINED true
/* A line as a whole takes so long to become the next one. */
#define EXAMPLE_LINE_MORPH_US 1500000LL
/* Letter by letter: each letter takes so long and starts so much after the one before. */
#define EXAMPLE_LETTER_MORPH_US 800000LL
#define EXAMPLE_LETTER_STAGGER_US 120000LL
/* A finished word stays so long. */
#define EXAMPLE_HOLD_MS 1500
/* The pen is this many cells thick, and the screen keeps this many pixels free at its sides. */
#define EXAMPLE_PEN_CELLS 9.0f
#define EXAMPLE_MARGIN 6

static dgx_screen_t *screen;
static dgx_screen_t *band;    /* the frame is drawn here, 8 bits a pixel, and sent to the screen whole */
static uint16_t      lut[256];
static int           band_y;  /* where the band is on the screen */
static int           base_y;  /* the base line of the text in the band */
static float         scale;
static int           pen;
static float         shift[WORDS]; /* what puts each word in the middle, in cells */

static float progress(int64_t start_us, int64_t now_us, int64_t duration_us)
{
    if (now_us <= start_us) return 0;
    if (now_us - start_us >= duration_us) return 1;
    float t = (float)(now_us - start_us) / (float)duration_us;
    return t * t * (3 - 2 * t);
}

/* The size at which the widest word fits the width of the screen, and a band as high as the tallest one. */
static bool prepare(dgx_font_t *font)
{
    int left[WORDS], right[WORDS], top = 0, bottom = 0, widest = 0;
    for (int i = 0; i < WORDS; ++i) {
        int t, b;
        if (!dgx_hw_text_box(font, words[i], &left[i], &t, &right[i], &b)) return false;
        if (i == 0 || t < top) top = t;
        if (i == 0 || b > bottom) bottom = b;
        if (right[i] - left[i] + 1 > widest) widest = right[i] - left[i] + 1;
    }
    pen = 1;
    for (int pass = 0; pass < 2; ++pass) {
        scale = (float)(screen->width - 2 * EXAMPLE_MARGIN - pen) / widest;
        pen = (int)lroundf(scale * EXAMPLE_PEN_CELLS);
        if (pen < 1) pen = 1;
    }
    for (int i = 0; i < WORDS; ++i) shift[i] = (screen->width / scale - (right[i] - left[i] + 1)) / 2 - left[i];
    /* between two words a curve may go somewhat beyond both: some room above and below */
    int room = (int)lroundf(font->hw->x_height * scale / 2) + pen;
    int height = (int)lroundf((bottom - top + 1) * scale) + 2 * room;
    if (height > screen->height) height = screen->height;
    band = dgx_vscreen_init(screen->width, height, 8, DgxScreenRGB);
    if (!band) return false;
    band_y = (screen->height - height) / 2;
    base_y = room - (int)lroundf(top * scale);
    return true;
}

static void present(void)
{
    dgx_vscreen8_to_screen16(screen, 0, band_y, band, lut, false);
}

static void clear(void)
{
    dgx_fill_rectangle(band, 0, 0, band->width, band->height, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting handwritten morph demo on CYD");
    dgx_bus_protocols_t *bus = dgx_spi_init(
        EXAMPLE_LCD_HOST, SPI_DMA_CH_AUTO,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MOSI, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MISO,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CLK, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CS,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_DC, EXAMPLE_LCD_SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return;
    }
    screen = dgx_ili9341_init(
        bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_BACKLIGHT, 16, DgxScreenRGB);
    if (!screen) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return;
    }
    dgx_ili9341_orientation(screen, DgxScreenRightLeft, DgxScreenTopBottom, true);
    /* paper is color 0 of the band and ink is color 255 */
    for (int i = 0; i < 256; ++i) {
        lut[i] = (uint16_t)dgx_rgb_to_16((uint8_t)(250 - (250 - 24) * i / 255), (uint8_t)(246 - (246 - 36) * i / 255),
                                        (uint8_t)(232 - (232 - 120) * i / 255));
    }
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, lut[0]);

    dgx_font_t *font = font0ss();
    if (!prepare(font)) {
        ESP_LOGE(TAG, "The font has no symbols of the words, or no memory for the band");
        return;
    }
    ESP_LOGI(TAG, "Screen %dx%d: %.3f pixels a cell, pen %d, band %dx%d at row %d", screen->width, screen->height, scale, pen,
             band->width, band->height, band_y);

    /* the first word is written by a pen, then the words morph one into another */
    {
        dgx_hw_pace_t    pace = dgx_hw_pace(font, 6);
        dgx_hw_writing_t writing;
        int64_t          start_us = esp_timer_get_time();
        clear();
        dgx_hw_writing_begin(&writing, band, (int)lroundf(shift[0] * scale), base_y, font, words[0], scale, pen, 255, EXAMPLE_JOINED, &pace);
        bool finished = false;
        while (!finished) {
            finished = dgx_hw_writing_draw_to(&writing, 1500.0f * (esp_timer_get_time() - start_us) * 1e-6f);
            present();
            vTaskDelay(1);
        }
    }

    for (int round = 0;; ++round) {
        int  from = round % WORDS, to = (round + 1) % WORDS;
        bool by_letters = round % 2;
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_HOLD_MS));

        dgx_hw_morph_t      *line = NULL;
        dgx_hw_morph_text_t *letters = NULL;
        int64_t              duration_us;
        int                  curves = 0;
        if (by_letters) {
            /* every position is a morph of its own with a progress of its own */
            letters = dgx_hw_morph_text_create(font, words[from], words[to], EXAMPLE_JOINED);
            if (!letters) break;
            dgx_hw_morph_text_shift(letters, shift[from], 0, shift[to], 0);
            duration_us = dgx_hw_morph_text_duration_us(letters, EXAMPLE_LETTER_MORPH_US, EXAMPLE_LETTER_STAGGER_US);
            for (size_t i = 0; i < letters->length; ++i) curves += letters->letters[i] ? letters->letters[i]->number : 0;
        } else {
            /* the whole way of the pen of one word goes into that of the other one */
            line = dgx_hw_morph_create(font, words[from], words[to], EXAMPLE_JOINED);
            if (!line) break;
            dgx_hw_morph_shift(line, shift[from], 0, shift[to], 0);
            duration_us = EXAMPLE_LINE_MORPH_US;
            curves = line->number;
        }

        int64_t start_us = esp_timer_get_time(), now_us, drawing_us = 0;
        int     frames = 0;
        do {
            now_us = esp_timer_get_time();
            clear();
            if (by_letters) {
                for (size_t i = 0; i < letters->length; ++i) {
                    float t = progress(start_us + (int64_t)i * EXAMPLE_LETTER_STAGGER_US, now_us, EXAMPLE_LETTER_MORPH_US);
                    dgx_hw_morph_draw(letters->letters[i], t, band, 0, base_y, scale, pen, 255);
                }
            } else {
                dgx_hw_morph_draw(line, progress(start_us, now_us, duration_us), band, 0, base_y, scale, pen, 255);
            }
            drawing_us += esp_timer_get_time() - now_us;
            present();
            ++frames;
            vTaskDelay(1);
        } while (now_us - start_us < duration_us);

        int64_t took_us = esp_timer_get_time() - start_us;
        ESP_LOGI(TAG, "\"%s\" -> \"%s\" %s: %d curves, %d frames, %.1f FPS, a frame is drawn in %lld us", words[from], words[to],
                 by_letters ? "letter by letter" : "as a whole", curves, frames, frames * 1e6f / took_us, drawing_us / frames);
        dgx_hw_morph_destroy(&line);
        dgx_hw_morph_text_destroy(&letters);
    }
    ESP_LOGE(TAG, "No memory for a morph");
}
