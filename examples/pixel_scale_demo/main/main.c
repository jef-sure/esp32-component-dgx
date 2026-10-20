/*
 * Pixel art made larger: a small 8-bit picture shown two, three and four
 * times as large through a table of colors, by squares, by Scale2x and
 * Scale3x, and by their SFX variants, one after another. Every way is timed, and
 * the time is shown above the picture and written to the log, so the cost
 * of smoothing on a display behind SPI and on a DSI display can be compared.
 *
 * The picture has what tells the ways apart: thin lines at many slopes,
 * where plain Scale2x grows lumps and Scale2xSFX does not, circles, steps of
 * one pixel and of two, a checkerboard, which none of them changes, and a
 * line of text.
 *
 * On a display whose pixels can be read back, a DSI one, every way is first
 * checked in every turn, with holes and without: put on the display and into
 * a virtual screen, which goes another way through the library, and the two
 * compared pixel by pixel.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_lut.h"
#include "dgx_screen.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

/* shown on the screen and in the log; changed at every flash of a build to be looked at */
#define BUILD 347

static const char *TAG = "dgx_pixel_scale_demo";

#if CONFIG_IDF_TARGET_ESP32P4

#include "bus/dgx_dsi_esp32.h"

#ifdef CONFIG_EXAMPLE_BOARD_JC8012P4A1C
#include "drivers/jd9365.h"
/* Guition JC8012P4A1C: a 10.1-inch 800x1280 display with a JD9365 on two DSI lanes, turned to 1280x800 */
enum
{
    EXAMPLE_LCD_DSI_LANES     = 2,
    EXAMPLE_LCD_DSI_LANE_MBPS = 1000,
    EXAMPLE_LCD_PHY_LDO_CHAN  = 3,    /* the regulator of the chip that powers the DSI PHY */
    EXAMPLE_LCD_PHY_LDO_MV    = 2500,
    EXAMPLE_LCD_PIN_NUM_RST   = 27,
    EXAMPLE_LCD_PIN_NUM_BL    = 23,
};
#define example_lcd_init        dgx_jd9365_init
#define example_lcd_orientation dgx_jd9365_orientation
/* lying: x of the screen along the rows of the panel; one of the two axes counted the other way, or the picture is mirrored */
#define EXAMPLE_LCD_DIR_X   DgxScreenRightLeft
#define EXAMPLE_LCD_DIR_Y   DgxScreenTopBottom
#define EXAMPLE_LCD_SWAP_XY true
#else
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
};
#define example_lcd_init        dgx_jd9165_init
#define example_lcd_orientation dgx_jd9165_orientation
#define EXAMPLE_LCD_DIR_X   DgxScreenLeftRight
#define EXAMPLE_LCD_DIR_Y   DgxScreenTopBottom
#define EXAMPLE_LCD_SWAP_XY false
#endif

/* 16 bits a pixel and two frame buffers: a picture is shown whole, as a game shows its frames */
static dgx_screen_t *display_init(void)
{
    gpio_set_direction((gpio_num_t)EXAMPLE_LCD_PIN_NUM_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)EXAMPLE_LCD_PIN_NUM_BL, 1);
    dgx_bus_protocols_t *bus =
        dgx_dsi_init(0, EXAMPLE_LCD_DSI_LANES, EXAMPLE_LCD_DSI_LANE_MBPS, EXAMPLE_LCD_PHY_LDO_CHAN, EXAMPLE_LCD_PHY_LDO_MV);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_dsi_init failed");
        return NULL;
    }
    dgx_screen_t *display = example_lcd_init(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, 16, DgxScreenRGB, DgxDsiTwoFrames);
    if (!display) {
        ESP_LOGE(TAG, "the driver of the display failed");
        return NULL;
    }
    example_lcd_orientation(display, EXAMPLE_LCD_DIR_X, EXAMPLE_LCD_DIR_Y, EXAMPLE_LCD_SWAP_XY);
    return display;
}

#else

#include "bus/dgx_spi_esp32.h"
#include "driver/spi_common.h"
#include "drivers/ili9341.h"

/* CYD, ESP32-2432S028R: a 320x240 display with an ILI9341 on SPI */
#define EXAMPLE_LCD_HOST SPI2_HOST

static const int EXAMPLE_LCD_PIN_NUM_MOSI      = 13;
static const int EXAMPLE_LCD_PIN_NUM_MISO      = 12;
static const int EXAMPLE_LCD_PIN_NUM_CLK       = 14;
static const int EXAMPLE_LCD_PIN_NUM_CS        = 15;
static const int EXAMPLE_LCD_PIN_NUM_DC        = 2;
static const int EXAMPLE_LCD_PIN_NUM_RST       = GPIO_NUM_NC;
static const int EXAMPLE_LCD_PIN_NUM_BACKLIGHT = 21;
static const int EXAMPLE_LCD_SPI_CLOCK_MHZ     = 40;

static dgx_screen_t *display_init(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(EXAMPLE_LCD_HOST, SPI_DMA_CH_AUTO, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MOSI, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_MISO,
                                            (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CLK, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_CS, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_DC,
                                            EXAMPLE_LCD_SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return NULL;
    }
    dgx_screen_t *display =
        dgx_ili9341_init(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_BACKLIGHT, 16, DgxScreenRGB);
    if (!display) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return NULL;
    }
    dgx_ili9341_orientation(display, DgxScreenRightLeft, DgxScreenTopBottom, true);
    return display;
}

#endif

/* the band above the picture with the name of the way and its time, drawn straight on the display */
#define BAND 16
/* every way is timed by so many transfers, with a pause before each: with two frame buffers a call right after another waits for the frame to be shown */
#define TIMES    8
#define PAUSE_MS 60
/* and then shown for so long */
#define HOLD_MS 2500

/* the indices of the picture */
enum {
    PAPER = 1,
    LINE_FIRST,      /* 4 colors of the lines */
    RING = LINE_FIRST + 4,
    DISC,
    STEP,
    CHECK,
    INK,
    SHADE,
    COLORS
};

static const uint8_t palette[COLORS][3] = {
    {0, 0, 0},       {16, 24, 48},   {250, 220, 90}, {120, 230, 140}, {250, 120, 110}, {130, 170, 255},
    {240, 240, 240}, {200, 80, 200}, {90, 200, 220}, {255, 255, 255}, {255, 250, 230}, {60, 70, 110},
};

static uint16_t lut[256];

/* the picture: the same things at any size, laid out by its width and height, on `paper` */
static void draw_picture(dgx_screen_t *pic, uint32_t paper)
{
    const int w = pic->width, h = pic->height;
    dgx_fill_rectangle(pic, 0, 0, w, h, paper);
    /* a fan of thin lines at every slope from a corner: where Scale2x grows lumps on a slant of one to one */
    const int fx = 4, fy = h - 4, reach = (w < h ? w : h) * 3 / 5;
    for (int i = 0; i <= 12; ++i) {
        const float a = (float)M_PI / 2 * (float)i / 12;
        dgx_draw_line(pic, fx, fy, fx + (int)lroundf(reach * cosf(a)), fy - (int)lroundf(reach * sinf(a)), (uint32_t)(LINE_FIRST + i % 4));
    }
    /* rings and a disc */
    const int cx = w * 3 / 4, cy = h / 3, r = h / 5;
    for (int k = 0; k < 3; ++k) dgx_draw_circle(pic, cx, cy, r - 3 * k, RING);
    dgx_solid_circle(pic, cx, cy, r / 3, DISC);
    dgx_draw_ellipse(pic, w / 2, h * 3 / 4, w / 6, h / 10, RING);
    /* steps of one pixel and of two, going up to the right */
    for (int i = 0; i < 12; ++i) {
        dgx_set_pixel(pic, w / 2 + i, h / 6 - i / 2 + 8, STEP);
        dgx_fill_rectangle(pic, w / 2 + 2 * i, h / 3 + 10 - i, 2, 1, STEP);
    }
    /* a checkerboard of single pixels: no way of smoothing changes it */
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) dgx_set_pixel(pic, w - 16 + x, h - 16 + y, (x + y) & 1 ? CHECK : paper);
    /* a solid triangle with slanted edges, and a word */
    dgx_draw_triangle_solid(pic, w / 4, h / 6, w / 4 + h / 6, h / 3, w / 4 - h / 8, h / 3 + 4, SHADE);
    dgx_font_string_utf8_screen(pic, w / 3, h - 6, "DGX", INK, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
}

typedef struct {
    uint32_t    flags;
    const char *name[3]; /* at 2, 3 and 4 */
} way_t;

static const way_t ways[] = {
    {0, {"squares", "squares", "squares"}},
    {DGX_VSCREEN_SMOOTH, {"Scale2x", "Scale3x", "Scale2x twice"}},
    {DGX_VSCREEN_SMOOTH_SFX, {"Scale2xSFX", "Scale3xSFX", "Scale2xSFX twice"}},
};
#define WAYS (sizeof(ways) / sizeof(ways[0]))

/*
 * Every way at 1 to 4, in all eight turns, with and without holes, of 8 bits
 * through the table and of 16 bits as they are, put on the display and into
 * a virtual screen, and the two compared. Large enough for the faster ways of
 * a DSI display to take it. Returns the pixels that differ.
 */
static int check(dgx_screen_t *display, int *transfers)
{
    enum { PART = 56 };
    static uint16_t lut0[256];
    dgx_screen_t   *pic = dgx_vscreen_init(PART, PART, 8, DgxScreenRGB), *pic16 = dgx_vscreen_init(PART, PART, 16, DgxScreenRGB);
    dgx_screen_t   *ref = dgx_vscreen_init(4 * PART, 4 * PART, 16, DgxScreenRGB);
    if (!pic || !pic16 || !ref) {
        dgx_screen_destroy(&pic);
        dgx_screen_destroy(&pic16);
        dgx_screen_destroy(&ref);
        return -1;
    }
    int wrong = 0;
    *transfers = 0;
    for (int holes = 0; holes < 2; ++holes) {
        /* the paper is index 0 for the holes; in 16 bits it is the value 0 */
        draw_picture(pic, holes ? 0 : PAPER);
        for (int i = 0; i < 256; ++i) lut0[i] = i ? lut[i] : 0;
        dgx_vscreen8_to_screen16(pic16, 0, 0, pic, lut0, 0);
        for (int n = 1; n <= 4; ++n) {
            for (size_t k = 0; k < (n == 1 ? 1 : WAYS); ++k) {
                for (int turn = 0; turn < 8; ++turn) {
                    for (int deep = 0; deep < 2; ++deep) {
                        const uint32_t flags = DGX_VSCREEN_SCALE(n) | ways[k].flags | DGX_VSCREEN_ORIENTED(turn) | (holes ? DGX_VSCREEN_TRANSPARENT : 0);
                        const int      side  = n * PART, under = dgx_rgb_to_16(40, 0, 40);
                        dgx_fill_rectangle(display, 0, BAND, side, side, under);
                        dgx_fill_rectangle(ref, 0, 0, side, side, under);
                        if (deep) {
                            dgx_vscreen_to_screen(display, 0, BAND, pic16, flags);
                            dgx_vscreen_to_screen(ref, 0, 0, pic16, flags);
                        } else {
                            dgx_vscreen8_to_screen16(display, 0, BAND, pic, lut, flags);
                            dgx_vscreen8_to_screen16(ref, 0, 0, pic, lut, flags);
                        }
                        for (int y = 0; y < side; ++y)
                            for (int x = 0; x < side; ++x) wrong += dgx_get_pixel(display, x, BAND + y) != dgx_get_pixel(ref, x, y);
                        ++*transfers;
                    }
                }
            }
        }
    }
    dgx_screen_destroy(&pic);
    dgx_screen_destroy(&pic16);
    dgx_screen_destroy(&ref);
    return wrong;
}

void app_main(void)
{
    dgx_screen_t *display = display_init();
    if (!display) return;
    const int width = display->width, height = display->height - BAND;
    /* the picture fills the display at 2; at 3 and 4 the part of it that fits */
    dgx_screen_t *pic = dgx_vscreen_init(width / 2, height / 2, 8, DgxScreenRGB);
    if (!pic) {
        ESP_LOGE(TAG, "no memory for a picture of %dx%d", width / 2, height / 2);
        return;
    }
    ESP_LOGI(TAG, "build %d: display %dx%d, picture %dx%d", BUILD, display->width, display->height, pic->width, pic->height);
    dgx_lut_from_rgb(lut, 0, COLORS, palette);
    if (dgx_screen_is_readable(display)) {
        int        transfers = 0;
        const int  wrong     = check(display, &transfers);
        char       line[64];
        snprintf(line, sizeof line, "b%d check: %d transfers, %d pixels differ", BUILD, transfers, wrong);
        dgx_fill_rectangle(display, 0, 0, display->width, display->height, dgx_rgb_to_16(0, 0, 0));
        dgx_font_string_utf8_screen(display, 4, BAND - 4, line, wrong ? dgx_rgb_to_16(255, 80, 80) : dgx_rgb_to_16(120, 255, 120), DgxOutputNormal, 1,
                                    TerminusTTFMedium12(), NULL, NULL);
        ESP_LOGI(TAG, "%s", line);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    draw_picture(pic, PAPER);
    float ms[3][WAYS];
    for (;;) {
        for (int n = 2; n <= 4; ++n) {
            /* the part of the picture that fits the display at this scale */
            const int part_w = width / n < pic->width ? width / n : pic->width, part_h = height / n < pic->height ? height / n : pic->height;
            for (size_t k = 0; k < WAYS; ++k) {
                const uint32_t flags = DGX_VSCREEN_SCALE(n) | ways[k].flags;
                int64_t        sum = 0, most = 0;
                for (int i = 0; i < TIMES; ++i) {
                    vTaskDelay(pdMS_TO_TICKS(PAUSE_MS));
                    const int64_t start = esp_timer_get_time();
                    dgx_vscreen8_region_to_screen16(display, 0, BAND, pic, 0, 0, part_w, part_h, lut, flags);
                    const int64_t took = esp_timer_get_time() - start;
                    sum += took;
                    if (took > most) most = took;
                }
                ms[n - 2][k] = sum / (float)TIMES / 1000.0f;
                char line[64];
                snprintf(line, sizeof line, "b%d x%d %s: %.1f ms, at most %.1f", BUILD, n, ways[k].name[n - 2], (double)ms[n - 2][k], most / 1000.0);
                dgx_fill_rectangle(display, 0, 0, display->width, BAND, dgx_rgb_to_16(0, 0, 0));
                dgx_font_string_utf8_screen(display, 4, BAND - 4, line, dgx_rgb_to_16(255, 255, 255), DgxOutputNormal, 1, TerminusTTFMedium12(), NULL,
                                            NULL);
                ESP_LOGI(TAG, "%s; %dx%d of the picture to %dx%d", line, part_w, part_h, n * part_w, n * part_h);
                vTaskDelay(pdMS_TO_TICKS(HOLD_MS));
            }
        }
        ESP_LOGI(TAG, "build %d, ms a call: squares, plain, SFX: x2 %.1f %.1f %.1f, x3 %.1f %.1f %.1f, x4 %.1f %.1f %.1f", BUILD, (double)ms[0][0],
                 (double)ms[0][1], (double)ms[0][2], (double)ms[1][0], (double)ms[1][1], (double)ms[1][2], (double)ms[2][0], (double)ms[2][1],
                 (double)ms[2][2]);
    }
}
