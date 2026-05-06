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
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_screen.h"
#include "drivers/ili9341.h"
#include "fonts/ArialRegular12.h"
#include "fonts/VerdanaRegular32.h"

static const char *TAG = "dgx_ili9341_demo";

#define EXAMPLE_LCD_HOST SPI3_HOST

enum
{
    EXAMPLE_LCD_PIN_NUM_MOSI  = 23,
    EXAMPLE_LCD_PIN_NUM_MISO  = 19,
    EXAMPLE_LCD_PIN_NUM_CLK   = 18,
    EXAMPLE_LCD_PIN_NUM_CS    = 5,
    EXAMPLE_LCD_PIN_NUM_DC    = 16,
    EXAMPLE_LCD_PIN_NUM_RST   = 17,
    EXAMPLE_LCD_SPI_CLOCK_MHZ = 40,
};

typedef void (*demo_step_fn_t)(dgx_screen_t *screen);

static inline uint32_t demo_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return dgx_rgb_to_16(r, g, b);
}

static void demo_draw_footer(dgx_screen_t *screen, const char *title)
{
    const uint32_t footer_bg = demo_rgb(8, 8, 18);
    const uint32_t footer_fg = DGX_WHITE(dgx_rgb_to_16);

    dgx_fill_rectangle(screen, 0, screen->height - 28, screen->width, 28, footer_bg);
    dgx_draw_line(screen, 0, screen->height - 28, screen->width - 1, screen->height - 28, demo_rgb(48, 140, 255));
    dgx_font_string_utf8_screen(screen, 8, screen->height - 10, title, footer_fg, DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
}

static void demo_splash(dgx_screen_t *screen)
{
    const uint32_t ink  = DGX_WHITE(dgx_rgb_to_16);
    const uint32_t cyan = demo_rgb(0, 180, 220);
    const uint32_t gold = demo_rgb(255, 200, 40);
    const uint32_t rose = demo_rgb(255, 90, 120);

    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, demo_rgb(6, 10, 20));
    dgx_fill_rectangle(screen, 0, 0, screen->width, 48, demo_rgb(10, 28, 60));
    dgx_fill_rectangle(screen, 0, 48, screen->width, 4, cyan);

    dgx_font_string_utf8_screen(screen, 14, 42, "DGX", ink, DgxOutputNormal, 1, VerdanaRegular32(), NULL, NULL);
    dgx_font_string_utf8_screen(screen, 138, 40, "ILI9341", gold, DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
    dgx_font_string_utf8_screen(screen, 138, 64, "ESP32 SPI demo", ink, DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
    dgx_font_string_utf8_screen(screen, 304, 170, "graphics", cyan, DgxOutputRotate90CW, 1, ArialRegular12(), NULL, NULL);

    for (int x = 18; x < screen->width - 18; x += 32) {
        uint32_t color = (x / 32) % 3 == 0 ? cyan : ((x / 32) % 3 == 1 ? gold : rose);
        dgx_fill_rectangle(screen, x, 92, 20, 78, color);
        dgx_draw_circle(screen, x + 10, 182, 10, color);
    }

    demo_draw_footer(screen, "Landscape mode, 16-bit color");
}

static void demo_text(dgx_screen_t *screen)
{
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(dgx_rgb_to_16));
    dgx_font_string_utf8_screen(screen, 12, 26, "Adafruit-style graphics test", demo_rgb(255, 214, 10), DgxOutputNormal, 1, ArialRegular12(), NULL,
                                NULL);
    dgx_font_string_utf8_screen(screen, 12, 56, "DGX draw + font primitives", demo_rgb(60, 190, 255), DgxOutputNormal, 1, ArialRegular12(), NULL,
                                NULL);
    dgx_font_string_utf8_screen(screen, 12, 86, "ILI9341 over SPI @ configurable clock", demo_rgb(220, 220, 220), DgxOutputNormal, 1,
                                ArialRegular12(), NULL, NULL);
    dgx_font_string_utf8_screen(screen, 12, 126, "RED", DGX_RED(dgx_rgb_to_16), DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
    dgx_font_string_utf8_screen(screen, 62, 126, "GREEN", DGX_GREEN(dgx_rgb_to_16), DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
    dgx_font_string_utf8_screen(screen, 132, 126, "BLUE", DGX_BLUE(dgx_rgb_to_16), DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
    dgx_font_string_utf8_screen(screen, 12, 176, "rotated", demo_rgb(255, 128, 32), DgxOutputRotate90CW, 1, ArialRegular12(), NULL, NULL);
    dgx_font_string_utf8_screen(screen, 292, 176, "text", demo_rgb(255, 128, 32), DgxOutputRotate90CCW, 1, ArialRegular12(), NULL, NULL);
    demo_draw_footer(screen, "Text, colors, and font orientation");
}

static void demo_lines(dgx_screen_t *screen)
{
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(dgx_rgb_to_16));

    for (int x = 0; x < screen->width; x += 12) {
        dgx_draw_line(screen, 0, 0, x, screen->height - 1, demo_rgb(255, 64, 64));
        dgx_draw_line(screen, screen->width - 1, 0, x, screen->height - 1, demo_rgb(64, 200, 255));
    }
    for (int y = 0; y < screen->height; y += 12) {
        dgx_draw_line(screen, 0, 0, screen->width - 1, y, demo_rgb(255, 200, 40));
        dgx_draw_line(screen, screen->width - 1, 0, 0, y, demo_rgb(80, 255, 120));
    }

    demo_draw_footer(screen, "Corner-to-corner line sweep");
}

static void demo_rectangles(dgx_screen_t *screen)
{
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, demo_rgb(8, 8, 18));

    for (int i = 0; i < 9; ++i) {
        int      inset = 10 + i * 14;
        uint32_t color = demo_rgb(30 + i * 22, 180 - i * 14, 255 - i * 18);
        dgx_fill_rectangle(screen, inset, inset, screen->width - inset * 2, screen->height - inset * 2, color);
    }

    dgx_fill_rectangle(screen, 54, 46, 212, 116, demo_rgb(12, 12, 12));
    dgx_font_string_utf8_screen(screen, 84, 112, "filled rectangles", DGX_WHITE(dgx_rgb_to_16), DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
    demo_draw_footer(screen, "Nested fill test");
}

static void demo_circles(dgx_screen_t *screen)
{
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, demo_rgb(0, 14, 24));

    for (int y = 28; y < 180; y += 38) {
        for (int x = 30; x < screen->width - 20; x += 44) {
            dgx_draw_circle(screen, x, y, 16, demo_rgb(60, 190, 255));
            dgx_solid_circle(screen, x, y, 6, demo_rgb(255, 180, 32));
        }
    }

    dgx_solid_circle(screen, 56, 198, 18, demo_rgb(255, 80, 80));
    dgx_solid_circle(screen, 112, 198, 18, demo_rgb(80, 255, 120));
    dgx_solid_circle(screen, 168, 198, 18, demo_rgb(80, 180, 255));
    dgx_solid_circle(screen, 224, 198, 18, demo_rgb(255, 220, 64));
    dgx_solid_circle(screen, 280, 198, 18, demo_rgb(255, 120, 220));

    demo_draw_footer(screen, "Circle outlines and fills");
}

static void demo_triangles(dgx_screen_t *screen)
{
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, demo_rgb(10, 0, 18));

    for (int i = 0; i < 7; ++i) {
        int      top   = 14 + i * 12;
        int      span  = 132 - i * 14;
        uint32_t color = demo_rgb(255 - i * 22, 40 + i * 24, 90 + i * 20);
        dgx_draw_triangle_solid(screen, screen->width / 2, top, screen->width / 2 - span, 198 - i * 8, screen->width / 2 + span, 198 - i * 8, color);
    }

    dgx_fill_rectangle(screen, 102, 90, 116, 46, demo_rgb(8, 8, 14));
    dgx_font_string_utf8_screen(screen, 116, 118, "triangles", DGX_WHITE(dgx_rgb_to_16), DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
    demo_draw_footer(screen, "Filled triangle fan");
}

static void demo_color_bars(dgx_screen_t *screen)
{
    const uint32_t bars[] = {
        DGX_RED(dgx_rgb_to_16),  DGX_ORANGE(dgx_rgb_to_16), DGX_YELLOW(dgx_rgb_to_16),  DGX_GREEN(dgx_rgb_to_16),
        DGX_CYAN(dgx_rgb_to_16), DGX_BLUE(dgx_rgb_to_16),   DGX_MAGENTA(dgx_rgb_to_16), DGX_WHITE(dgx_rgb_to_16),
    };

    int bar_width = screen->width / (int)(sizeof(bars) / sizeof(bars[0]));
    for (size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); ++i) {
        dgx_fill_rectangle(screen, (int)i * bar_width, 0, bar_width + 1, screen->height, bars[i]);
    }

    dgx_fill_rectangle(screen, 48, 72, 224, 84, demo_rgb(8, 8, 8));
    dgx_font_string_utf8_screen(screen, 74, 102, "RGB565 color bars", DGX_WHITE(dgx_rgb_to_16), DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
    demo_draw_footer(screen, "Panel sanity check");
}

static void demo_run_step(dgx_screen_t *screen, const char *name, demo_step_fn_t step, uint32_t hold_ms)
{
    int64_t start_us = esp_timer_get_time();
    step(screen);
    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG, "%s rendered in %" PRId64 " ms", name, elapsed_ms);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ILI9341 demo on ESP32: MOSI=%d MISO=%d SCLK=%d CS=%d DC=%d RST=%d CLK=%dMHz", EXAMPLE_LCD_PIN_NUM_MOSI,
             EXAMPLE_LCD_PIN_NUM_MISO, EXAMPLE_LCD_PIN_NUM_CLK, EXAMPLE_LCD_PIN_NUM_CS, EXAMPLE_LCD_PIN_NUM_DC, EXAMPLE_LCD_PIN_NUM_RST,
             EXAMPLE_LCD_SPI_CLOCK_MHZ);

    dgx_bus_protocols_t *bus =
        dgx_spi_init(EXAMPLE_LCD_HOST, SPI_DMA_CH_AUTO, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MOSI, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MISO,
                     (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CLK, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CS, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_DC,
                     EXAMPLE_LCD_SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return;
    }

    dgx_screen_t *screen = dgx_ili9341_init(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, 16, DgxScreenRGB);
    if (!screen) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return;
    }

    dgx_ili9341_orientation(screen, DgxScreenLeftRight, DgxScreenTopBottom, false);

    while (1) {
        demo_run_step(screen, "splash", demo_splash, 1800);
        demo_run_step(screen, "color-bars", demo_color_bars, 1400);
        demo_run_step(screen, "text", demo_text, 1700);
        demo_run_step(screen, "lines", demo_lines, 1400);
        demo_run_step(screen, "rectangles", demo_rectangles, 1400);
        demo_run_step(screen, "circles", demo_circles, 1400);
        demo_run_step(screen, "triangles", demo_triangles, 1600);
    }
}