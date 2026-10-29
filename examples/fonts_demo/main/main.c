/*
 * Fonts made at build time, on a CYD (ESP32-2432S028R: an ESP32 with a 320x240 ILI9341 on SPI).
 *
 * main/CMakeLists.txt declares three needs. The build renders two of them from fonts installed on the build
 * machine, one picked by ../fonts.rules and one named exactly, and takes the third from DGX as it is. This program
 * includes the generated dgx_fonts.h and draws. The strings below are also in main/title.txt and main/strings.txt,
 * which give the fonts their characters.
 */
#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_spi_esp32.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_fonts.h"
#include "dgx_screen.h"
#include "drivers/ili9341.h"

static const char *TAG = "fonts_demo";

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

static const char *const title   = "Шрифты по запросу";
static const char *const lines[] = {
    "DGX renders the fonts while the",
    "program is built: a face picked by",
    "rules, a size in millimeters, and",
    "only the characters these strings",
    "use. Буквы любого алфавита, если",
    "они есть в текстах.",
};

static void draw_page(dgx_screen_t *scr)
{
    const uint32_t ink  = DGX_WHITE(dgx_rgb_to_16);
    const uint32_t gold = dgx_rgb_to_16(255, 200, 40);
    dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, dgx_rgb_to_16(6, 10, 20));
    int y = DGX_FONT_TITLE_PX + 6;
    dgx_font_string_utf8_screen(scr, 8, y, title, gold, DgxOutputNormal, 1, dgx_font_title(), NULL, NULL);
    y += DGX_FONT_BODY_PX + 12;
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i, y += DGX_FONT_BODY_PX + 4)
        dgx_font_string_utf8_screen(scr, 8, y, lines[i], ink, DgxOutputNormal, 1, dgx_font_body(), NULL, NULL);
}

/* the time since start, in the bundled font: digits and a colon are all the build checked it for */
static void draw_clock(dgx_screen_t *scr, uint32_t seconds)
{
    char text[8];
    snprintf(text, sizeof text, "%02" PRIu32 ":%02" PRIu32, seconds / 60 % 100, seconds % 60);
    dgx_fill_rectangle(scr, 0, scr->height - 44, scr->width, 44, dgx_rgb_to_16(6, 10, 20));
    dgx_font_string_utf8_screen(scr, 8, scr->height - 8, text, DGX_WHITE(dgx_rgb_to_16), DgxOutputNormal, 1, dgx_font_clock(), NULL, NULL);
}

void app_main(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(LCD_HOST, SPI_DMA_CH_AUTO, (gpio_num_t)PIN_MOSI, (gpio_num_t)PIN_MISO, (gpio_num_t)PIN_CLK,
                                            (gpio_num_t)PIN_CS, (gpio_num_t)PIN_DC, SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return;
    }
    dgx_screen_t *screen = dgx_ili9341_init(bus, (gpio_num_t)PIN_RST, (gpio_num_t)PIN_BACKLIGHT, 16, DgxScreenRGB);
    if (!screen) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return;
    }
    dgx_ili9341_orientation(screen, DgxScreenRightLeft, DgxScreenTopBottom, true); /* landscape, 320x240 */

    /* what the build made, from the generated header */
#define NAME(n) " " #n
    ESP_LOGI(TAG, "needs:" DGX_FONT_NEEDS(NAME));
#undef NAME
    ESP_LOGI(TAG, "title: %d px (%g mm), body: %d px (%g mm), clock: %s", DGX_FONT_TITLE_PX, (double)DGX_FONT_TITLE_MM, DGX_FONT_BODY_PX,
             (double)DGX_FONT_BODY_MM, DGX_FONT_CLOCK_MADE ? "made for this board" : "a bundled font, used as is");
    ESP_LOGI(TAG, "the display: %d x %d px, %g x %g mm", DGX_FONTS_BOARD_WIDTH_PX, DGX_FONTS_BOARD_HEIGHT_PX, (double)DGX_FONTS_BOARD_WIDTH_MM,
             (double)DGX_FONTS_BOARD_HEIGHT_MM);

    draw_page(screen);
    for (uint32_t seconds = 0;; ++seconds) {
        draw_clock(screen, seconds);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
