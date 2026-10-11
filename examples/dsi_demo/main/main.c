#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_dsi_esp32.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_screen.h"
#include "drivers/jd9165.h"
#include "drivers/vscreen.h"
#include "fonts/ArialRegular12.h"
#include "fonts/VerdanaRegular32.h"

static const char *TAG = "dgx_dsi_demo";

/* Guition JC1060P470: a 7-inch 1024x600 display with a JD9165 on two DSI lanes */
enum
{
    EXAMPLE_LCD_DSI_LANES     = 2,
    EXAMPLE_LCD_DSI_LANE_MBPS = 750,
    EXAMPLE_LCD_PHY_LDO_CHAN  = 3,    /* the regulator of the chip that powers the DSI PHY */
    EXAMPLE_LCD_PHY_LDO_MV    = 2500,
    EXAMPLE_LCD_PIN_NUM_RST   = 0,
    EXAMPLE_LCD_PIN_NUM_BL    = 23,
    EXAMPLE_LCD_BL_ON_LEVEL   = 1,
};

/* How the screen lies on the panel. If "top left" is not at the top left, change these. */
#define EXAMPLE_LCD_DIR_X   DgxScreenLeftRight
#define EXAMPLE_LCD_DIR_Y   DgxScreenTopBottom
#define EXAMPLE_LCD_SWAP_XY false

typedef void (*demo_step_fn_t)(dgx_screen_t *screen);

#define EXAMPLE_LCD_COLOR_BITS CONFIG_EXAMPLE_LCD_COLOR_BITS

/* What the display may spend to show a picture whole: nothing, or one more frame buffer in PSRAM */
#ifdef CONFIG_EXAMPLE_LCD_TWO_FRAMES
#define EXAMPLE_LCD_FRAMES DgxDsiTwoFrames
#else
#define EXAMPLE_LCD_FRAMES DgxDsiOneFrame
#endif

static inline uint32_t demo_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return EXAMPLE_LCD_COLOR_BITS == 24 ? dgx_rgb_to_24(r, g, b) : dgx_rgb_to_16(r, g, b);
}

static void demo_text(dgx_screen_t *screen, int x, int y, const char *text, uint32_t color)
{
    dgx_font_string_utf8_screen(screen, x, y, text, color, DgxOutputNormal, 1, VerdanaRegular32(), NULL, NULL);
}

/* The corners named and the three colors named: what tells a wrong orientation or color order at a glance */
static void demo_test_card(dgx_screen_t *screen)
{
    const uint32_t white = DGX_WHITE(demo_rgb);
    int            w = screen->width, h = screen->height;

    dgx_fill_rectangle(screen, 0, 0, w, h, demo_rgb(6, 10, 20));
    dgx_fill_rectangle(screen, 0, 0, w, 1, white);
    dgx_fill_rectangle(screen, 0, h - 1, w, 1, white);
    dgx_fill_rectangle(screen, 0, 0, 1, h, white);
    dgx_fill_rectangle(screen, w - 1, 0, 1, h, white);
    demo_text(screen, 16, 44, "top left", white);
    demo_text(screen, w - 250, h - 20, "bottom right", white);

    int bar = w / 5, top = h / 2 - 70;
    dgx_fill_rectangle(screen, bar, top, bar, 140, DGX_RED(demo_rgb));
    dgx_fill_rectangle(screen, 2 * bar, top, bar, 140, DGX_GREEN(demo_rgb));
    dgx_fill_rectangle(screen, 3 * bar, top, bar, 140, DGX_BLUE(demo_rgb));
    demo_text(screen, bar + 16, top + 82, "red", white);
    demo_text(screen, 2 * bar + 16, top + 82, "green", DGX_BLACK(demo_rgb));
    demo_text(screen, 3 * bar + 16, top + 82, "blue", white);

    char size[48];
    snprintf(size, sizeof(size), "DGX on MIPI-DSI, %dx%d", w, h);
    demo_text(screen, bar, top - 30, size, demo_rgb(255, 200, 40));
    dgx_font_string_utf8_screen(screen, bar, top + 170, EXAMPLE_LCD_COLOR_BITS == 24 ? "JD9165, 24-bit color" : "JD9165, 16-bit color",
                                demo_rgb(60, 190, 255), DgxOutputNormal, 1, ArialRegular12(), NULL, NULL);
}

/* a color of the rainbow, hue from 0 to 1535 */
static uint32_t demo_hue(int hue)
{
    int part = hue / 256, up = hue % 256, down = 255 - up;
    switch (part) {
    case 0: return demo_rgb(255, up, 0);
    case 1: return demo_rgb(down, 255, 0);
    case 2: return demo_rgb(0, 255, up);
    case 3: return demo_rgb(0, down, 255);
    case 4: return demo_rgb(up, 0, 255);
    default: return demo_rgb(255, 0, down);
    }
}

/*
 * Ramps from dark to full: 16-bit color shows steps in them, 24-bit color does not.
 * A row is made in a virtual screen one row high and sent to the display row
 * after row: a row is one piece of the memory the display is sent from.
 */
static void demo_ramps(dgx_screen_t *screen)
{
    int           w = screen->width, band = screen->height / 6;
    dgx_screen_t *row = dgx_vscreen_init(w, 1, screen->color_bits, DgxScreenRGB);
    if (!row) return;
    for (int ramp = 0; ramp < 5; ++ramp) {
        for (int x = 0; x < w; ++x) {
            uint8_t  level = (uint8_t)(x * 255 / (w - 1));
            uint32_t color = ramp == 0   ? demo_rgb(level, 0, 0)
                             : ramp == 1 ? demo_rgb(0, level, 0)
                             : ramp == 2 ? demo_rgb(0, 0, level)
                             : ramp == 3 ? demo_rgb(level, level, level)
                                         : demo_hue(x * 1535 / (w - 1));
            dgx_set_pixel(row, x, 0, color);
        }
        for (int y = ramp * band; y < (ramp + 1) * band; ++y) dgx_vscreen_to_screen(screen, 0, y, row);
    }
    /* the rainbow going down to black */
    int top = 5 * band, rows = screen->height - top;
    for (int y = 0; y < rows; ++y) {
        int light = 255 - y * 255 / (rows - 1);
        for (int x = 0; x < w; ++x) {
            int hue = x * 1535 / (w - 1), part = hue / 256, up = hue % 256 * light / 255, down = light - up;
            static const uint8_t order[6][3] = {{0, 1, 2}, {3, 0, 2}, {2, 0, 1}, {2, 3, 0}, {1, 2, 0}, {0, 2, 3}};
            int                  value[4]    = {light, up, 0, down};
            dgx_set_pixel(row, x, 0, demo_rgb((uint8_t)value[order[part][0]], (uint8_t)value[order[part][1]], (uint8_t)value[order[part][2]]));
        }
        dgx_vscreen_to_screen(screen, 0, top + y, row);
    }
    dgx_screen_destroy(&row);
    demo_text(screen, 16, band - 20, EXAMPLE_LCD_COLOR_BITS == 24 ? "24 bits" : "16 bits", DGX_WHITE(demo_rgb));
}

static void demo_lines(dgx_screen_t *screen)
{
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(demo_rgb));
    for (int x = 0; x < screen->width; x += 24) {
        dgx_draw_line(screen, 0, 0, x, screen->height - 1, demo_rgb(255, 64, 64));
        dgx_draw_line(screen, screen->width - 1, 0, x, screen->height - 1, demo_rgb(64, 200, 255));
    }
    for (int y = 0; y < screen->height; y += 24) {
        dgx_draw_line(screen, 0, 0, screen->width - 1, y, demo_rgb(255, 200, 40));
        dgx_draw_line(screen, screen->width - 1, 0, 0, y, demo_rgb(80, 255, 120));
    }
}

static void demo_circles(dgx_screen_t *screen)
{
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, demo_rgb(0, 14, 24));
    for (int y = 60; y < screen->height - 40; y += 96) {
        for (int x = 60; x < screen->width - 40; x += 96) {
            dgx_draw_circle(screen, x, y, 40, demo_rgb(60, 190, 255));
            dgx_solid_circle(screen, x, y, 16, demo_rgb(255, 180, 32));
        }
    }
}

static void demo_triangles(dgx_screen_t *screen)
{
    int cx = screen->width / 2, bottom = screen->height - 30;
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, demo_rgb(10, 0, 18));
    for (int i = 0; i < 9; ++i) {
        int      span  = screen->width / 2 - 40 - i * 48;
        uint32_t color = demo_rgb(255 - i * 22, 40 + i * 24, 90 + i * 18);
        dgx_draw_triangle_solid(screen, cx, 30 + i * 30, cx - span, bottom - i * 18, cx + span, bottom - i * 18, color);
    }
}

static void demo_color_bars(dgx_screen_t *screen)
{
    const uint32_t bars[] = {
        DGX_RED(demo_rgb),  DGX_ORANGE(demo_rgb), DGX_YELLOW(demo_rgb),  DGX_GREEN(demo_rgb),
        DGX_CYAN(demo_rgb), DGX_BLUE(demo_rgb),   DGX_MAGENTA(demo_rgb), DGX_WHITE(demo_rgb),
    };
    int bar_width = screen->width / (int)(sizeof(bars) / sizeof(bars[0]));
    for (size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); ++i) {
        dgx_fill_rectangle(screen, (int)i * bar_width, 0, bar_width + 1, screen->height, bars[i]);
    }
}

/*
 * Bars moving sideways in a band of the screen: a picture that tears if it does. With one frame buffer a bar is
 * seen broken now and then, where the display was reading the band while it was sent; with two it never is.
 */
static void demo_moving_bars(dgx_screen_t *screen, uint32_t run_ms)
{
    const int     top = screen->height / 3, height = screen->height / 3, pitch = 128, width = 40, step = 4;
    dgx_screen_t *band = dgx_vscreen_init(screen->width, height, screen->color_bits, DgxScreenRGB);
    if (!band) return;
    dgx_screen_progress_up(screen);
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(demo_rgb));
    demo_text(screen, 16, 44, EXAMPLE_LCD_FRAMES == DgxDsiTwoFrames ? "two frame buffers" : "one frame buffer", DGX_WHITE(demo_rgb));
    dgx_screen_progress_down(screen);
    int64_t start = esp_timer_get_time();
    int     frames = 0, offset = 0;
    while (esp_timer_get_time() - start < (int64_t)run_ms * 1000) {
        for (int x = offset - pitch; x < band->width; x += pitch) dgx_fill_rectangle(band, x, 0, width, height, DGX_BLACK(demo_rgb));
        offset = (offset + step) % pitch;
        for (int x = offset - pitch; x < band->width; x += pitch) dgx_fill_rectangle(band, x, 0, width, height, DGX_CYAN(demo_rgb));
        dgx_vscreen_to_screen(screen, 0, top, band);
        ++frames;
        /* one frame buffer: not faster than the display shows; with two the bus waits for the display itself */
        if (EXAMPLE_LCD_FRAMES == DgxDsiOneFrame) vTaskDelay(1);
    }
    ESP_LOGI(TAG, "moving bars: %d frames a second", (int)(frames * 1000LL / run_ms));
    dgx_screen_destroy(&band);
}

static void demo_run_step(dgx_screen_t *screen, const char *name, demo_step_fn_t step, uint32_t hold_ms)
{
    int64_t start_us = esp_timer_get_time();
    /* one batch: what a scene has drawn is brought out to the display once, at its end */
    dgx_screen_progress_up(screen);
    step(screen);
    dgx_screen_progress_down(screen);
    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG, "%s rendered in %" PRId64 " ms", name, elapsed_ms);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
}

void app_main(void)
{
    gpio_set_direction((gpio_num_t)EXAMPLE_LCD_PIN_NUM_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)EXAMPLE_LCD_PIN_NUM_BL, EXAMPLE_LCD_BL_ON_LEVEL);

    dgx_bus_protocols_t *bus =
        dgx_dsi_init(0, EXAMPLE_LCD_DSI_LANES, EXAMPLE_LCD_DSI_LANE_MBPS, EXAMPLE_LCD_PHY_LDO_CHAN, EXAMPLE_LCD_PHY_LDO_MV);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_dsi_init failed");
        return;
    }

    dgx_screen_t *screen = dgx_jd9165_init(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, EXAMPLE_LCD_COLOR_BITS, DgxScreenRGB, EXAMPLE_LCD_FRAMES);
    if (!screen) {
        ESP_LOGE(TAG, "dgx_jd9165_init failed");
        return;
    }
    dgx_jd9165_orientation(screen, EXAMPLE_LCD_DIR_X, EXAMPLE_LCD_DIR_Y, EXAMPLE_LCD_SWAP_XY);
    ESP_LOGI(TAG, "Display initialized: %dx%d", screen->width, screen->height);

    while (1) {
        demo_run_step(screen, "test card", demo_test_card, 5000);
        demo_run_step(screen, "ramps", demo_ramps, 6000);
        demo_run_step(screen, "lines", demo_lines, 2500);
        demo_run_step(screen, "circles", demo_circles, 2500);
        demo_run_step(screen, "triangles", demo_triangles, 2500);
        demo_run_step(screen, "color bars", demo_color_bars, 2500);
        demo_moving_bars(screen, 6000);
    }
}
