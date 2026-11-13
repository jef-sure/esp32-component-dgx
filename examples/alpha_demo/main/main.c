/*
 * Alpha in DGX, in three scenes shown one after another again and again,
 * every one timed into the log.
 *
 * 1. A crossfade. Two pictures fade into each other by DGX_VSCREEN_ALPHA(a).
 *    A DSI display reads its frame buffer: the second picture is mixed
 *    straight over the first there. A display behind SPI cannot be read, so
 *    the two are mixed in a 16-bit virtual screen, which then goes to the
 *    display; a picture fades to black and back straight on the display,
 *    which needs nothing read, as black is what lies under a picture there;
 *    and the second picture, as large as the display, is the overlay layer
 *    over the first, mixed into it on the way to the display as fast as the
 *    bus sends it.
 * 2. A soft shape put many times. A disc with a soft edge and a word in a
 *    soft font are drawn once into an 8-bit screen of amounts of ink, and
 *    DGX_VSCREEN_MASK puts the shape in one color at many places over a
 *    picture: larger, turned, fainter by DGX_VSCREEN_ALPHA. Behind SPI into
 *    the virtual screen again.
 * 3. A translucent palette. An overlay layer of buttons lies over a picture
 *    that moves: the face of a button solid and its shadow faint, by
 *    dgx_overlay_set_alphas(), and the alpha of the layer fades all of them
 *    in and out.
 *
 * A band of 16 rows at the top holds the number of the build, the scene and
 * its time; the pictures lie under it, made larger: twice on the CYD, four
 * times on a DSI display.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_fonts.h"
#include "dgx_lut.h"
#include "dgx_overlay.h"
#include "dgx_screen.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

/* shown on the screen and in the log; changed at every flash of a build to be looked at */
#define BUILD 766

static const char *TAG = "dgx_alpha_demo";

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
    dgx_bus_protocols_t *bus =
        dgx_dsi_init(0, EXAMPLE_LCD_DSI_LANES, EXAMPLE_LCD_DSI_LANE_MBPS, EXAMPLE_LCD_PHY_LDO_CHAN, EXAMPLE_LCD_PHY_LDO_MV);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_dsi_init failed");
        return NULL;
    }
    dgx_screen_t *display =
        example_lcd_init(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_BL, 16, DgxScreenRGB, DgxDsiTwoFrames);
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

/* how long a scene runs, and the band of the label at the top */
#define SCENE_MS 6000
#define BAND     16

static dgx_screen_t *display;
static bool          readable;   /* a DSI display: what it holds is read and mixed with */
static int           scale;      /* of the pictures on the display */
static int           pw, ph;     /* the pictures */
static dgx_screen_t *pic_a, *pic_b, *mix, *shape;
static dgx_screen_t *layer_b; /* behind SPI: picture B as large as the display, for the overlay layer */
static uint16_t      lut_a[256], lut_b[256];

static uint16_t c16(uint32_t rgb)
{
    return (uint16_t)dgx_rgb_to_16((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* the label: the build, the scene and what it has measured */
static void label(const char *text)
{
    char line[96];
    snprintf(line, sizeof(line), "#%d  %s", BUILD, text);
    const dgx_font_context_t ctx = {.scr = display, .font = TerminusTTFMedium12(), .color = c16(0xf0f0f0), .bg = 0, .flags = DGX_FONT_FILL};
    dgx_fill_rectangle(display, 0, 0, display->width, BAND, 0);
    dgx_font_draw(&ctx, 4, 12, line);
}

/* picture A: a sky from deep blue to pale, a sun and two hills */
static void draw_landscape(dgx_screen_t *pic)
{
    dgx_lut_gradient(lut_a, 0, 64, 0x102a6a, 0xb4dcff);
    lut_a[64] = c16(0xffd84a), lut_a[65] = c16(0x2f6b2a), lut_a[66] = c16(0x4f9a3c);
    for (int y = 0; y < ph; ++y) dgx_fill_rectangle(pic, 0, y, pw, 1, (uint32_t)(y * 63 / (ph - 1)));
    dgx_solid_circle(pic, pw * 3 / 4, ph / 3, ph / 7, 64);
    dgx_solid_circle(pic, pw / 4, ph + ph / 3, ph * 3 / 5, 65);
    dgx_solid_circle(pic, pw * 4 / 5, ph + ph / 2, ph * 3 / 4, 66);
}

/* picture B: tiles of two colors under rings of warm ones */
static void draw_tiles(dgx_screen_t *pic)
{
    lut_b[1] = c16(0x3c1450), lut_b[2] = c16(0x5a2878);
    lut_b[3] = c16(0xff8a5c), lut_b[4] = c16(0xffc06a), lut_b[5] = c16(0xff5c8a);
    const int tile = ph / 7 > 4 ? ph / 7 : 4;
    for (int y = 0; y < ph; y += tile)
        for (int x = 0; x < pw; x += tile) dgx_fill_rectangle(pic, x, y, tile, tile, (uint32_t)(((x / tile) ^ (y / tile)) & 1 ? 1 : 2));
    for (int i = 0; i < 9; ++i)
        for (int k = 0; k < 3; ++k) dgx_draw_circle(pic, pw * (1 + i % 3) / 4, ph * (1 + i / 3) / 4, ph / 9 - k, (uint32_t)(3 + i % 3));
}

/* the soft shape: a disc with a soft edge and a word, amounts of ink from 0 to 255 */
static void draw_shape(dgx_screen_t *m)
{
    dgx_fill_rectangle(m, 0, 0, m->width, m->height, 0);
    dgx_solid_circle(m, 20, 20, DGX_PEN_SOFT | 17, 255);
    const dgx_font_context_t word = {.scr = m, .font = dgx_font_word(), .color = 255};
    dgx_font_draw(&word, 42, 28, "DGX");
}

/* a picture under the band, through its table, larger and mixed as `flags` say */
static void put(dgx_screen_t *dst, dgx_screen_t *pic, const uint16_t *lut, uint32_t flags)
{
    if (dst == display) dgx_vscreen8_to_screen16(display, 0, BAND, pic, lut, DGX_VSCREEN_SCALE(scale) | flags);
    else dgx_vscreen8_to_screen16(dst, 0, 0, pic, lut, flags);
}

/* 0 .. 255 .. 0 over `period` steps */
static int wave(int step, int period)
{
    const int t = step % period, half = period / 2;
    return (t < half ? t : period - t) * 255 / half;
}

static void scene_crossfade(void)
{
    label("1/3 crossfade");
    int64_t busy = 0;
    int     frames = 0;
    for (const int64_t end = now_us() + SCENE_MS * 1000; now_us() < end; ++frames) {
        const int     a  = wave(frames, 64);
        const int64_t t0 = now_us();
        if (readable) {
            /* the frame buffer is read: the second picture mixed straight over the first */
            dgx_screen_progress_up(display);
            put(display, pic_a, lut_a, 0);
            put(display, pic_b, lut_b, DGX_VSCREEN_ALPHA(a));
            dgx_screen_progress_down(display);
        } else {
            /* mixed in memory, then sent whole */
            put(mix, pic_a, lut_a, 0);
            put(mix, pic_b, lut_b, DGX_VSCREEN_ALPHA(a));
            dgx_vscreen_to_screen(display, 0, BAND, mix, DGX_VSCREEN_SCALE(scale));
        }
        busy += now_us() - t0;
        vTaskDelay(1);
    }
    char text[80];
    snprintf(text, sizeof(text), "1/3 crossfade: %.1f ms a frame", (double)busy / frames / 1000.0);
    ESP_LOGI(TAG, "%s, %s", text, readable ? "mixed on the display" : "mixed in a virtual screen and sent whole");
    label(text);
    if (readable) return;
    /* behind SPI: black is under a picture, so it fades to black and back straight on the display */
    busy = 0, frames = 0;
    for (const int64_t end = now_us() + SCENE_MS * 1000 / 2; now_us() < end; ++frames) {
        const int64_t t0 = now_us();
        put(display, pic_a, lut_a, DGX_VSCREEN_ALPHA(255 - wave(frames, 32)));
        busy += now_us() - t0;
        vTaskDelay(1);
    }
    snprintf(text, sizeof(text), "1/3 fade to black: %.1f ms a frame", (double)busy / frames / 1000.0);
    ESP_LOGI(TAG, "%s, straight on the display", text);
    label(text);
    /*
     * B the overlay layer at the alpha of the frame, A put under it: the
     * layer is mixed into A as it goes, so a frame takes as long as the bus
     * takes to send A, with no 16-bit screen in memory. The layer is not
     * made larger, so B is drawn as large as the display; it has no index 0,
     * which a layer leaves empty.
     */
    busy = 0, frames = 0;
    for (const int64_t end = now_us() + SCENE_MS * 1000; now_us() < end; ++frames) {
        const int64_t t0 = now_us();
        dgx_overlay_set(display, layer_b, 0, BAND, lut_b, (uint8_t)wave(frames, 64));
        put(display, pic_a, lut_a, 0);
        int x, y, w, h;
        dgx_overlay_take_dirty(display, &x, &y, &w, &h); /* the whole picture is put every frame */
        busy += now_us() - t0;
        vTaskDelay(1);
    }
    dgx_overlay_clear(display);
    int x, y, w, h;
    dgx_overlay_take_dirty(display, &x, &y, &w, &h);
    snprintf(text, sizeof(text), "1/3 crossfade by a layer: %.1f ms", (double)busy / frames / 1000.0);
    ESP_LOGI(TAG, "%s a frame, picture B the overlay layer", text);
    label(text);
}

static void scene_shapes(void)
{
    label("2/3 a soft shape put many times");
    static const uint32_t inks[4] = {0xffffff, 0xffe066, 0x66e0ff, 0xff7090};
    static uint16_t       lut_ink[4][256];
    for (int i = 0; i < 4; ++i) lut_ink[i][255] = c16(inks[i]);
    dgx_screen_t *dst = readable ? display : mix;
    /* where the shapes go: on the display under the band, or in the virtual screen */
    const int     top = readable ? BAND : 0, wide = readable ? display->width : pw, high = readable ? display->height - BAND : ph;
    int64_t       busy = 0;
    int           frames = 0, puts = 0;
    for (const int64_t end = now_us() + SCENE_MS * 1000; now_us() < end; ++frames) {
        const int64_t t0 = now_us();
        dgx_screen_progress_up(dst);
        put(dst, frames / 40 % 2 ? pic_b : pic_a, frames / 40 % 2 ? lut_b : lut_a, 0);
        for (int i = 0; i < 12; ++i) {
            /* around a circle that turns, every shape turned its own way, some twice as large, some fainter */
            const float    angle = (float)(frames * 0.04 + i * 0.5236);
            const int      n     = readable && i % 3 == 0 ? 2 : 1;
            const int      x = wide / 2 + (int)(cosf(angle) * wide * 0.33f) - shape->width * n / 2;
            const int      y = top + high / 2 + (int)(sinf(angle) * high * 0.33f) - shape->height * n / 2;
            const uint32_t flags = DGX_VSCREEN_MASK | DGX_VSCREEN_SCALE(n) | DGX_VSCREEN_ORIENTED(i % 8) | DGX_VSCREEN_ALPHA(i % 4 == 3 ? 110 : 255);
            dgx_vscreen8_to_screen16(dst, x, y, shape, lut_ink[i % 4], flags);
            ++puts;
        }
        dgx_screen_progress_down(dst);
        if (!readable) dgx_vscreen_to_screen(display, 0, BAND, mix, DGX_VSCREEN_SCALE(scale));
        busy += now_us() - t0;
        vTaskDelay(1);
    }
    char text[80];
    snprintf(text, sizeof(text), "2/3 soft shapes: %.1f ms a frame of 12", (double)busy / frames / 1000.0);
    ESP_LOGI(TAG, "%s, %d shapes put", text, puts);
    label(text);
}

static void scene_overlay(void)
{
    label("3/3 a translucent palette");
    /* a game pad: three buttons, each a shadow, a face and a sign */
    const int     lw = display->width * 3 / 8, lh = (display->height - BAND) / 4, r = lh / 3;
    dgx_screen_t *layer = dgx_vscreen_init(lw, lh, 8, DgxScreenRGB);
    if (!layer) {
        ESP_LOGE(TAG, "no memory for the layer");
        return;
    }
    dgx_fill_rectangle(layer, 0, 0, lw, lh, 0);
    for (int i = 0; i < 3; ++i) {
        const int cx = lw * (1 + 2 * i) / 6, cy = lh / 2;
        dgx_solid_circle(layer, cx + r / 5, cy + r / 5, r, 2);
        dgx_solid_circle(layer, cx, cy, r, 1);
        dgx_draw_circle(layer, cx, cy, r / 2, 3);
    }
    static uint16_t lut_l[256];
    static uint8_t  shares[256];
    lut_l[1] = c16(0xe8e8e8), lut_l[2] = c16(0x000000), lut_l[3] = c16(0xd03030);
    memset(shares, 255, sizeof(shares));
    shares[2] = 90; /* the shadow faint */
    const int lx = (display->width - lw) / 2, ly = display->height - lh - 4;
    int64_t   busy = 0;
    int       frames = 0;
    for (const int64_t end = now_us() + SCENE_MS * 1000; now_us() < end; ++frames) {
        const int64_t t0 = now_us();
        /* the alpha of the layer fades it all; the shares of the indices stay */
        dgx_overlay_set(display, layer, lx, ly, lut_l, (uint8_t)(80 + wave(frames, 48) * 175 / 255));
        if (!frames) dgx_overlay_set_alphas(display, shares);
        /* the picture moves along: its two parts, and the layer mixed into what goes under it */
        const int dx = frames * 2 % pw;
        dgx_screen_progress_up(display);
        dgx_vscreen8_region_to_screen16(display, 0, BAND, pic_b, dx, 0, pw - dx, ph, lut_b, DGX_VSCREEN_SCALE(scale));
        if (dx) dgx_vscreen8_region_to_screen16(display, (pw - dx) * scale, BAND, pic_b, 0, 0, dx, ph, lut_b, DGX_VSCREEN_SCALE(scale));
        dgx_screen_progress_down(display);
        int x, y, w, h;
        dgx_overlay_take_dirty(display, &x, &y, &w, &h); /* the whole picture is put every frame */
        busy += now_us() - t0;
        vTaskDelay(1);
    }
    dgx_overlay_clear(display);
    int x, y, w, h;
    dgx_overlay_take_dirty(display, &x, &y, &w, &h);
    dgx_screen_destroy(&layer);
    char text[80];
    snprintf(text, sizeof(text), "3/3 overlay: %.1f ms a frame", (double)busy / frames / 1000.0);
    ESP_LOGI(TAG, "%s", text);
    label(text);
}

void app_main(void)
{
    display = display_init();
    if (!display) return;
    readable = dgx_screen_is_readable(display);
    scale    = readable ? 4 : 2;
    pw = display->width / scale, ph = (display->height - BAND) / scale;
    pic_a = dgx_vscreen_init(pw, ph, 8, DgxScreenRGB);
    pic_b = dgx_vscreen_init(pw, ph, 8, DgxScreenRGB);
    shape = dgx_vscreen_init(96, 40, 8, DgxScreenRGB);
    mix   = readable ? NULL : dgx_vscreen_init(pw, ph, 16, DgxScreenRGB);
    layer_b = readable ? NULL : dgx_vscreen_init(display->width, display->height - BAND, 8, DgxScreenRGB);
    if (!pic_a || !pic_b || !shape || (!readable && (!mix || !layer_b))) {
        ESP_LOGE(TAG, "no memory for the pictures");
        return;
    }
    ESP_LOGI(TAG, "build %d: %s, %dx%d, pictures %dx%d at x%d, %s", BUILD, display->screen_name, display->width, display->height, pw, ph, scale,
             readable ? "the display is read" : "the display is not read");
    draw_landscape(pic_a);
    draw_tiles(pic_b);
    if (layer_b) draw_tiles(layer_b);
    draw_shape(shape);
    dgx_fill_rectangle(display, 0, 0, display->width, display->height, 0);
    for (;;) {
        scene_crossfade();
        scene_shapes();
        scene_overlay();
    }
}
