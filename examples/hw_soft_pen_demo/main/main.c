#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_lut.h"
#include "dgx_font.h"
#include "dgx_hw_font.h"
#include "dgx_screen.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"
#include "fonts/font0ss.h"

static const char *TAG = "dgx_hw_soft_pen_demo";

#if CONFIG_IDF_TARGET_ESP32P4

#include "bus/dgx_dsi_esp32.h"
#include "drivers/jd9165.h"

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

/* 16 bits a pixel and two frame buffers: what is drawn between two frames is shown whole, the moving lines do not tear. */
static dgx_screen_t *display_init(void)
{
    ESP_LOGI(TAG, "Starting soft pen demo on a JC1060P470");
    gpio_set_direction((gpio_num_t)EXAMPLE_LCD_PIN_NUM_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)EXAMPLE_LCD_PIN_NUM_BL, EXAMPLE_LCD_BL_ON_LEVEL);
    dgx_bus_protocols_t *bus =
        dgx_dsi_init(0, EXAMPLE_LCD_DSI_LANES, EXAMPLE_LCD_DSI_LANE_MBPS, EXAMPLE_LCD_PHY_LDO_CHAN, EXAMPLE_LCD_PHY_LDO_MV);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_dsi_init failed");
        return NULL;
    }
    dgx_screen_t *display = dgx_jd9165_init(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, 16, DgxScreenRGB, DgxDsiTwoFrames);
    if (!display) {
        ESP_LOGE(TAG, "dgx_jd9165_init failed");
        return NULL;
    }
    dgx_jd9165_orientation(display, DgxScreenLeftRight, DgxScreenTopBottom, false);
    return display;
}

#else

#include "bus/dgx_spi_esp32.h"
#include "driver/spi_common.h"
#include "drivers/ili9341.h"

/* CYD, ESP32-2432S028R: a 320x240 display with an ILI9341 on SPI */
#define EXAMPLE_LCD_HOST SPI2_HOST

static const int EXAMPLE_LCD_PIN_NUM_MOSI = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ = 40;

static dgx_screen_t *display_init(void)
{
    ESP_LOGI(TAG, "Starting soft pen demo on CYD");
    dgx_bus_protocols_t *bus = dgx_spi_init(
        EXAMPLE_LCD_HOST, SPI_DMA_CH_AUTO,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MOSI, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MISO,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CLK, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CS,
        (gpio_num_t)EXAMPLE_LCD_PIN_NUM_DC, EXAMPLE_LCD_SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return NULL;
    }
    dgx_screen_t *display = dgx_ili9341_init(
        bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_BACKLIGHT, 16, DgxScreenRGB);
    if (!display) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return NULL;
    }
    dgx_ili9341_orientation(display, DgxScreenRightLeft, DgxScreenTopBottom, true);
    return display;
}

#endif

/* Two lines are written as large as the screen takes them; then they get lower, as wide as they were, and make room for two more. */
#define LINES 4
static const char *const lines[LINES] = {"Привет,", "Perl!", "&", "Modern::Perl"};
/* How large the third line, the sign, is beside the others when all four are on the screen: larger, it takes too much empty room. */
#define EXAMPLE_SIGN_SHARE 0.5f

/* Joined writing; false writes every letter apart. */
#define EXAMPLE_JOINED true
/* The pen goes through so much effort a second: about so many cells of the grid of the font. */
#define EXAMPLE_TEMPO 1500.0f
/* Ink is taken off so many times faster than it is put. */
#define EXAMPLE_OFF_FASTER 4.0f
/* What every straight piece of a curve adds to its length: the pen slows down in bends. */
#define EXAMPLE_BEND_EFFORT 6.0f
/* The pen is this many cells thick and not thinner than this many pixels; the screen keeps so many pixels free at its edges. */
#define EXAMPLE_PEN_CELLS 9.0f
#define EXAMPLE_PEN_LEAST 2
#define EXAMPLE_MARGIN 6
/* So many cells of the upper lines are left between the bottom of a line and the top of the next. */
#define EXAMPLE_GAP_CELLS 24
/* The two lines stay so long before they move, they take so long to move, and the four stay so long. */
#define EXAMPLE_PAUSE_MS 1200
#define EXAMPLE_MOVE_US 1400000LL
#define EXAMPLE_HOLD_MS 6000
/* Not 0: shown in a corner until the writing starts, to tell one firmware from another. */
#define EXAMPLE_BUILD 0

typedef struct {
    float scale_x, scale_y; /* pixels a cell of the font along a line and up it */
    int   pen;
    int   x, y;             /* where the line starts on the screen: its left end on the base line */
} place_t;

static dgx_screen_t *screen;
static dgx_screen_t *page; /* all is drawn here, 8 bits a pixel: a byte is an amount of ink */
static uint16_t      lut[256];
static dgx_font_t   *font;
static dgx_hw_pace_t pace;
static int           box_left[LINES], box_top[LINES], box_right[LINES], box_bottom[LINES]; /* of a line, in cells */

/* The pen stays round whatever the two sizes are: it is as thick as a size between them asks. */
static int pen_of(float scale_x, float scale_y)
{
    int pen = (int)lroundf(sqrtf(scale_x * scale_y) * EXAMPLE_PEN_CELLS);
    return pen < EXAMPLE_PEN_LEAST ? EXAMPLE_PEN_LEAST : pen;
}

static int width_of(int line)
{
    return box_right[line] - box_left[line] + 1;
}

static int wider_of(int one, int other)
{
    return width_of(one) > width_of(other) ? width_of(one) : width_of(other);
}

static int height_of(int line)
{
    return box_bottom[line] - box_top[line];
}

/*
 * The lines `first` .. `last` one under another, each in the middle of the screen. A line is
 * `wide[i]` pixels a cell along and `tall * part[i]` up. The top of a line is EXAMPLE_GAP_CELLS under
 * the bottom of the one above it, by what the lines themselves take and not by the line height of
 * the font, which keeps room over the capitals. `high` is how high all of them are when `tall` is 1,
 * in cells.
 */
static void put_lines(place_t *placed, int first, int last, const float *wide, const float *part, float tall, float high)
{
    float top = (page->height - high * tall) / 2;
    for (int i = first; i <= last; ++i) {
        placed[i].scale_x = wide[i];
        placed[i].scale_y = tall * part[i];
        placed[i].pen = pen_of(placed[i].scale_x, placed[i].scale_y);
        placed[i].x = (int)lroundf((page->width - width_of(i) * placed[i].scale_x) / 2 - box_left[i] * placed[i].scale_x);
        placed[i].y = (int)lroundf(top - box_top[i] * placed[i].scale_y);
        top += height_of(i) * placed[i].scale_y + EXAMPLE_GAP_CELLS * tall;
    }
}

/* The largest size at which the two upper lines fill the screen alone, as wide as they are tall. */
static void fit_two(place_t *placed)
{
    const float high = height_of(0) + EXAMPLE_GAP_CELLS + height_of(1) + 1;
    float       scale = 0;
    int         pen = 1;
    for (int pass = 0; pass < 2; ++pass) {
        float by_width = (float)(page->width - 2 * EXAMPLE_MARGIN - pen) / wider_of(0, 1);
        float by_height = (float)(page->height - 2 * EXAMPLE_MARGIN - pen) / high;
        scale = by_width < by_height ? by_width : by_height;
        pen = pen_of(scale, scale);
    }
    const float wide[2] = {scale, scale}, part[2] = {1, 1};
    put_lines(placed, 0, 1, wide, part, scale, high);
}

/*
 * All four on the screen. The two upper lines stay as wide as they were alone, `wide`, and only get
 * lower. The last line is as large as the first one: as high, and as wide on the screen, its letters
 * the narrower for it. The line between, a sign alone, is `lower` of their height and as wide as it
 * is tall, as it is drawn in the font.
 */
static void fit_four(place_t *placed, float wide, float lower)
{
    const float part[LINES] = {1, 1, lower, 1};
    const float high = height_of(0) + height_of(1) + height_of(2) * lower + height_of(3) + 3 * EXAMPLE_GAP_CELLS + 1;
    float       tall = 0;
    int         pen = 1;
    for (int pass = 0; pass < 2; ++pass) {
        tall = (float)(page->height - 2 * EXAMPLE_MARGIN - pen) / high;
        if (tall > wide) tall = wide;
        pen = pen_of(wide, tall);
    }
    const float wide_of[LINES] = {wide, wide, tall * lower, wide * width_of(0) / width_of(3)};
    put_lines(placed, 0, 3, wide_of, part, tall, high);
    /* written by the pen of the first line, not by a thinner one for its narrower letters */
    placed[3].pen = placed[0].pen;
}

/* What has changed on the page goes to the display, and only that. */
static void show(void)
{
    int left, top, width, height;
    if (dgx_screen_take_dirty(page, &left, &top, &width, &height)) {
        dgx_vscreen8_region_to_screen16(screen, left, top, page, left, top, width, height, lut, false);
    }
}

/* A line is written by a soft pen with so much ink, at the pace of the pen; with no ink it is taken off the same way. */
static void write_line(int line, const place_t *at, uint32_t ink, float tempo)
{
    dgx_hw_writing_t writing;
    dgx_hw_writing_begin_xy(&writing, page, at->x, at->y, font, lines[line], at->scale_x, at->scale_y, DGX_PEN_SOFT | at->pen, ink,
                            EXAMPLE_JOINED, &pace);
    /* the pen comes to the line through the air, as between two strokes */
    int64_t start_us = esp_timer_get_time() + (int64_t)(pace.least / tempo * 1e6f);
    bool    finished = false;
    while (!finished) {
        int64_t now_us = esp_timer_get_time();
        if (now_us > start_us) {
            finished = dgx_hw_writing_draw_to(&writing, tempo * (now_us - start_us) * 1e-6f);
            show();
        }
        vTaskDelay(1);
    }
}

/* The part of the page a line takes at a place, the pen counted in. */
static void wipe_line(int line, const place_t *at)
{
    int left = at->x + (int)floorf(box_left[line] * at->scale_x) - at->pen - 1;
    int top = at->y + (int)floorf(box_top[line] * at->scale_y) - at->pen - 1;
    int right = at->x + (int)ceilf(box_right[line] * at->scale_x) + at->pen + 1;
    int bottom = at->y + (int)ceilf(box_bottom[line] * at->scale_y) + at->pen + 1;
    dgx_fill_rectangle(page, left, top, right - left + 1, bottom - top + 1, 0);
}

/*
 * The two upper lines go from one place and size to another: they get lower and go up, as wide as they
 * were. A frame is the two lines drawn anew, at a size and a place between the two; the pen follows the
 * size by whole pixels.
 */
static void move_lines(const place_t *from, const place_t *to)
{
    place_t now[2] = {from[0], from[1]};
    int64_t start_us = esp_timer_get_time(), drawn_us = 0;
    int     frames = 0;
    for (bool last = false; !last;) {
        float t = (float)(esp_timer_get_time() - start_us) / (float)EXAMPLE_MOVE_US;
        if (t >= 1) t = 1, last = true;
        t = t * t * (3 - 2 * t);
        int64_t frame_us = esp_timer_get_time();
        for (int i = 0; i < 2; ++i) wipe_line(i, &now[i]);
        for (int i = 0; i < 2; ++i) {
            now[i].scale_x = from[i].scale_x + (to[i].scale_x - from[i].scale_x) * t;
            now[i].scale_y = from[i].scale_y + (to[i].scale_y - from[i].scale_y) * t;
            now[i].pen = last ? to[i].pen : pen_of(now[i].scale_x, now[i].scale_y);
            now[i].x = (int)lroundf(from[i].x + (to[i].x - from[i].x) * t);
            now[i].y = (int)lroundf(from[i].y + (to[i].y - from[i].y) * t);
            dgx_hw_draw_text_xy(page, now[i].x, now[i].y, font, lines[i], now[i].scale_x, now[i].scale_y, DGX_PEN_SOFT | now[i].pen, 255,
                                EXAMPLE_JOINED);
        }
        drawn_us += esp_timer_get_time() - frame_us;
        show();
        ++frames;
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "Moved in %d frames, %.1f frames a second, a frame drawn in %.2f ms", frames,
             frames * 1e6f / (float)(esp_timer_get_time() - start_us), drawn_us * 1e-3f / frames);
}

void app_main(void)
{
    screen = display_init();
    if (!screen) return;
    ESP_LOGI(TAG, "Display initialized: %dx%d", screen->width, screen->height);
    page = dgx_vscreen_init(screen->width, screen->height, 8, DgxScreenRGB);
    if (!page) {
        ESP_LOGE(TAG, "No memory for the page");
        return;
    }
    /* paper is 0 on the page and all the ink is 255: the table runs from the one to the other, for the soft edge of the pen */
    dgx_lut_gradient(lut, 0, 256, dgx_rgb_to_24(250, 246, 232), dgx_rgb_to_24(24, 36, 120));
    font = font0ss();
    pace = dgx_hw_pace(font, EXAMPLE_BEND_EFFORT);
    for (int i = 0; i < LINES; ++i) {
        if (!dgx_hw_text_box(font, lines[i], &box_left[i], &box_top[i], &box_right[i], &box_bottom[i])) {
            ESP_LOGE(TAG, "The font has no symbols of \"%s\"", lines[i]);
            return;
        }
    }
    /* what is drawn is gathered until it is shown */
    dgx_screen_progress_up(page);

    for (;;) {
        const float lower = EXAMPLE_SIGN_SHARE;
        place_t     two[2], four[LINES];
        fit_two(two);
        fit_four(four, two[0].scale_x, lower);
        ESP_LOGI(TAG, "Two lines: %.3f pixels a cell, pen %d. Four, the sign %.2f of the rest: %.3f x %.3f pen %d, %.3f x %.3f pen %d, %.3f x %.3f pen %d",
                 two[0].scale_x, two[0].pen, lower, four[0].scale_x, four[0].scale_y, four[0].pen, four[2].scale_x, four[2].scale_y, four[2].pen,
                 four[3].scale_x, four[3].scale_y, four[3].pen);
        dgx_fill_rectangle(page, 0, 0, page->width, page->height, 0);
        show();
        if (EXAMPLE_BUILD) {
            char label[32];
            snprintf(label, sizeof(label), "#%d", EXAMPLE_BUILD);
            dgx_font_string_utf8_screen(screen, 4, 12, label, dgx_rgb_to_16(150, 60, 40), DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
            vTaskDelay(pdMS_TO_TICKS(1500));
            dgx_vscreen8_to_screen16(screen, 0, 0, page, lut, false);
        }

        for (int i = 0; i < 2; ++i) write_line(i, &two[i], 255, EXAMPLE_TEMPO);
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_PAUSE_MS));
        move_lines(two, four);
        for (int i = 2; i < LINES; ++i) write_line(i, &four[i], 255, EXAMPLE_TEMPO);
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_HOLD_MS));
        /* the same pen with no ink goes the same way: nothing is left */
        for (int i = 0; i < LINES; ++i) write_line(i, &four[i], 0, EXAMPLE_TEMPO * EXAMPLE_OFF_FASTER);
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_PAUSE_MS));
    }
}
