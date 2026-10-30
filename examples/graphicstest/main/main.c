/*
 * The PDQ graphics test of Adafruit_GFX, TFT_eSPI and LovyanGFX
 * (TFT_graphicstest_PDQ) made with DGX: the same figures, sizes, colors and
 * order, timed the same way, so that its numbers can be put beside theirs.
 * The board is a CYD (ESP32-2432S028R: an ILI9341 of 240x320 on SPI at
 * 40 MHz), a Guition JC4827W543 (ESP32-S3: an NV3041A of 480x272 on QSPI
 * at 40 MHz) or a Guition JC1060P470 (ESP32-P4: a JD9165 of 1024x600 on
 * MIPI-DSI, one frame buffer, so that a figure is in the memory the panel
 * shows when its call returns, as in the memory of an SPI panel), upright as
 * their sketches draw it.
 *
 * Every timed part ends when the display has its last pixel: DGX may still
 * be sending when a call returns, so the bus is waited for before the clock
 * is read. A test that DGX has no single call for is made of the calls a
 * program would use: a "Fast" line is a rectangle a pixel high or wide, the
 * outline of a triangle three lines, the text of the test is drawn in the
 * classic 5x7 font of Adafruit_GFX (GLCD5x7 of DGX), and the "HaD pushColor"
 * picture is sent by set_area() and write_area() of the screen.
 *
 * Every round runs twice. First on the display, as their sketches draw:
 * every call goes to the panel. Then in memory, as programs of DGX draw to
 * show no flicker: each test draws its picture into an 8-bit virtual screen
 * of the size of the display, its time taken as before, and one transfer of
 * the whole of it to the display through a table of 256 colors, RGB332 as
 * the 8-bit sprites of TFT_eSPI keep them, is added to that time. Where
 * there is no memory for the virtual screen the second run is left out.
 *
 * The log has the table of their sketch for each run; the screen shows both
 * at the end of the round.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "build.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_lut.h"
#include "driver/spi_common.h"
#include "drivers/vscreen.h"
#include "fonts/GLCD5x7.h"
#include "had_240x320.h"

static const char *TAG = "graphicstest";

static dgx_screen_t *lcd;       /* the display */
static dgx_screen_t *mem;       /* the 8-bit virtual screen of the run in memory, NULL without memory for it */
static dgx_screen_t *tft;       /* what the tests draw on: one of the two */
static bool          in_memory; /* tft is mem */
static uint16_t      lut[256];  /* RGB332 as RGB565 */
static int           W, H;

/* color565() of their sketches, the arguments bytes: an index of RGB332 in memory */
static uint32_t color565(int r, int g, int b)
{
    if (in_memory) return ((uint32_t)r & 0xE0u) | (((uint32_t)g & 0xE0u) >> 3) | (((uint32_t)b & 0xFFu) >> 6);
    return dgx_rgb_to_16((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* a color of TFT_eSPI.h, RGB565, as the screen drawn on takes it */
static uint32_t tft_color(uint16_t c565)
{
    return in_memory ? color565((c565 >> 8) & 0xF8, (c565 >> 3) & 0xFC, (c565 << 3) & 0xF8) : c565;
}

/* the colors of TFT_eSPI.h */
#define BLACK tft_color(0x0000)
#define WHITE tft_color(0xFFFF)
#define RED tft_color(0xF800)
#define GREEN tft_color(0x07E0)
#define BLUE tft_color(0x001F)
#define YELLOW tft_color(0xFFE0)
#define MAGENTA tft_color(0xF81F)
#define CYAN tft_color(0x07FF)

static void use(bool memory)
{
    in_memory = memory;
    tft       = memory ? mem : lcd;
}

/* the virtual screen to the display, what the run in memory adds to the time of every test */
static uint32_t push(void)
{
    const int64_t start = esp_timer_get_time();
    dgx_vscreen8_to_screen16(lcd, 0, 0, mem, lut, 0);
    if (lcd->wait_buffer) lcd->wait_buffer(lcd);
    return (uint32_t)(esp_timer_get_time() - start);
}

static int64_t micros(void)
{
    return esp_timer_get_time();
}

/* the clock is read when the display has everything */
static uint32_t since(int64_t start)
{
    if (tft->wait_buffer) tft->wait_buffer(tft);
    return (uint32_t)(micros() - start);
}

static void fill_screen(uint32_t color)
{
    dgx_fill_rectangle(tft, 0, 0, W, H, color);
}

/* the cursor of Adafruit_GFX, at the top of a line: println() goes to the left edge and a row of 8 x size down */
static struct {
    int      x, y, size;
    uint32_t color;
} cursor;

static void print(const char *s)
{
    /* the baseline of the font is 7 rows below the top of its cell */
    dgx_font_string_utf8_screen(tft, cursor.x, cursor.y + 7 * cursor.size, s, cursor.color, DgxOutputNormal, cursor.size, GLCD5x7(), NULL, NULL);
    cursor.x += 6 * cursor.size * (int)strlen(s);
}

static void println(const char *s)
{
    print(s);
    cursor.x = 0;
    cursor.y += 8 * cursor.size;
}

static uint32_t test_had(void)
{
    enum { PIXELS = 2048 };
    uint8_t *half[2] = {heap_caps_malloc(2 * PIXELS, MALLOC_CAP_DMA), heap_caps_malloc(2 * PIXELS, MALLOC_CAP_DMA)};
    if (!half[0] || !half[1]) {
        free(half[0]), free(half[1]);
        return 0;
    }
    const unsigned bytes = tft->color_bits / 8u;
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    int64_t start = micros();
    for (int i = 0; i < 0x10; i++) {
        const uint32_t color = color565((i << 4) | i, (i << 4) | i, (i << 4) | i);
        uint32_t       now = 0;
        tft->set_area(tft, 0, 239, 0, 319);
        /* the runs go into one half while the other one is sent */
        int            k = 0, n = 0;
        const uint8_t *cmp = had_240x320;
        while (cmp < had_240x320 + sizeof(had_240x320)) {
            unsigned cnt = *cmp++;
            if (cnt & 0x80) cnt = ((cnt & 0x7f) << 8) | *cmp++;
            while (cnt) {
                const unsigned take = cnt < (unsigned)(PIXELS - n) ? cnt : (unsigned)(PIXELS - n);
                if (bytes == 1) {
                    memset(half[k] + n, (uint8_t)now, take);
                    n += (int)take;
                } else {
                    for (unsigned j = 0; j < take; ++j, ++n) half[k][2 * n] = (uint8_t)(now >> 8), half[k][2 * n + 1] = (uint8_t)now;
                }
                cnt -= take;
                if (n == PIXELS) {
                    tft->write_area(tft, half[k], 8 * bytes * PIXELS);
                    k ^= 1, n = 0;
                    if (tft->wait_buffer) tft->wait_buffer(tft); /* the half to be filled next is no longer sent */
                }
            }
            now ^= color;
        }
        if (n) tft->write_area(tft, half[k], 8 * bytes * n);
        if (tft->wait_buffer) tft->wait_buffer(tft);
    }
    uint32_t t = since(start);
    free(half[0]), free(half[1]);
    cursor.color = YELLOW, cursor.size = 2;
    cursor.x = 8, cursor.y = 285, print("http://hackaday.io/");
    cursor.x = 96, cursor.y = 302, print("Xark");
    vTaskDelay(pdMS_TO_TICKS(3000));
    return t;
}

static uint32_t test_fill_screen(void)
{
    int64_t start = micros();
    fill_screen(WHITE);
    fill_screen(RED);
    fill_screen(GREEN);
    fill_screen(BLUE);
    fill_screen(BLACK);
    return since(start) / 5;
}

static uint32_t test_text(void)
{
    char line[24];
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    int64_t start = micros();
    cursor.x = cursor.y = 0;
    /* setTextColor(TFT_WHITE, TFT_BLACK): the cells of the first line are filled first */
    cursor.color = WHITE, cursor.size = 1;
    dgx_fill_rectangle(tft, 0, 0, 6 * 12, 8, BLACK);
    println("Hello World!");
    cursor.size = 2;
    cursor.color = color565(0xff, 0x00, 0x00), print("RED ");
    cursor.color = color565(0x00, 0xff, 0x00), print("GREEN ");
    cursor.color = color565(0x00, 0x00, 0xff), println("BLUE");
    cursor.color = YELLOW, cursor.size = 2;
    snprintf(line, sizeof(line), "%.2f", 1234.56), println(line);
    cursor.color = RED, cursor.size = 3;
    snprintf(line, sizeof(line), "%X", 0xDEADBEEFu), println(line);
    println("");
    cursor.color = GREEN, cursor.size = 5;
    println("Groop");
    cursor.size = 2;
    println("I implore thee,");
    cursor.color = GREEN, cursor.size = 1;
    println("my foonting turlingdromes.");
    println("And hooptiously drangle me");
    println("with crinkly bindlewurdles,");
    println("Or I will rend thee");
    println("in the gobberwarts");
    println("with my blurglecruncheon,");
    println("see if I don't!");
    println("");
    println("");
    cursor.color = MAGENTA, cursor.size = 6;
    println("Woot!");
    uint32_t t = since(start);
    vTaskDelay(pdMS_TO_TICKS(1000));
    return t;
}

static uint32_t test_pixels(void)
{
    int64_t start = micros();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) dgx_set_pixel(tft, x, y, color565(x << 3, y << 3, x * y));
    }
    return since(start);
}

static uint32_t test_lines(uint32_t color)
{
    uint32_t t = 0;
    int      x1, y1, x2, y2;
    for (int corner = 0; corner < 4; ++corner) {
        fill_screen(BLACK);
        if (tft->wait_buffer) tft->wait_buffer(tft);
        x1 = corner & 1 ? W - 1 : 0;
        y1 = corner & 2 ? H - 1 : 0;
        y2 = corner & 2 ? 0 : H - 1;
        int64_t start = micros();
        for (x2 = 0; x2 < W; x2 += 6) dgx_draw_line(tft, x1, y1, x2, y2, color);
        x2 = corner & 1 ? 0 : W - 1;
        for (y2 = 0; y2 < H; y2 += 6) dgx_draw_line(tft, x1, y1, x2, y2, color);
        t += since(start);
    }
    return t;
}

static uint32_t test_fast_lines(uint32_t color1, uint32_t color2)
{
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    int64_t start = micros();
    for (int y = 0; y < H; y += 5) dgx_fill_rectangle(tft, 0, y, W, 1, color1);
    for (int x = 0; x < W; x += 5) dgx_fill_rectangle(tft, x, 0, 1, H, color2);
    return since(start);
}

static uint32_t test_rects(uint32_t color)
{
    const int cx = W / 2, cy = H / 2, n = W < H ? W : H;
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    int64_t start = micros();
    for (int i = 2; i < n; i += 6) dgx_draw_rectangle(tft, cx - i / 2, cy - i / 2, i, i, color);
    return since(start);
}

static uint32_t test_filled_rects(uint32_t color1, uint32_t color2)
{
    const int cx = W / 2 - 1, cy = H / 2 - 1, n = W < H ? W : H;
    uint32_t  t = 0;
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    for (int i = n; i > 0; i -= 6) {
        const int i2    = i / 2;
        int64_t   start = micros();
        dgx_fill_rectangle(tft, cx - i2, cy - i2, i, i, color1);
        t += since(start);
        /* outlines are not included in timing results */
        dgx_draw_rectangle(tft, cx - i2, cy - i2, i, i, color2);
    }
    return t;
}

static uint32_t test_filled_circles(int radius, uint32_t color)
{
    const int r2 = radius * 2;
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    int64_t start = micros();
    for (int x = radius; x < W; x += r2) {
        for (int y = radius; y < H; y += r2) dgx_solid_circle(tft, x, y, radius, color);
    }
    return since(start);
}

static uint32_t test_circles(int radius, uint32_t color)
{
    const int r2 = radius * 2, w = W + radius, h = H + radius;
    /* the screen is not cleared for this one, as in their sketch */
    int64_t start = micros();
    for (int x = 0; x < w; x += r2) {
        for (int y = 0; y < h; y += r2) dgx_draw_circle(tft, x, y, radius, color);
    }
    return since(start);
}

static void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color)
{
    dgx_draw_line(tft, x0, y0, x1, y1, color);
    dgx_draw_line(tft, x1, y1, x2, y2, color);
    dgx_draw_line(tft, x2, y2, x0, y0, color);
}

static uint32_t test_triangles(void)
{
    const int cx = W / 2 - 1, cy = H / 2 - 1, n = cx < cy ? cx : cy;
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    int64_t start = micros();
    for (int i = 0; i < n; i += 5) draw_triangle(cx, cy - i, cx - i, cy + i, cx + i, cy + i, color565(0, 0, i));
    return since(start);
}

static uint32_t test_filled_triangles(void)
{
    const int cx = W / 2 - 1, cy = H / 2 - 1;
    uint32_t  t = 0;
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    for (int i = cx < cy ? cx : cy; i > 10; i -= 5) {
        int64_t start = micros();
        dgx_draw_triangle_solid(tft, cx, cy - i, cx - i, cy + i, cx + i, cy + i, color565(0, i, i));
        t += since(start);
        draw_triangle(cx, cy - i, cx - i, cy + i, cx + i, cy + i, color565(i, i, 0));
    }
    return t;
}

static uint32_t test_round_rects(void)
{
    const int cx = W / 2 - 1, cy = H / 2 - 1, w = W < H ? W : H;
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    int64_t start = micros();
    for (int i = 0; i < w; i += 6) dgx_draw_round_rectangle(tft, cx - i / 2, cy - i / 2, i, i, i / 8, color565(i, 0, 0));
    return since(start);
}

static uint32_t test_filled_round_rects(void)
{
    const int cx = W / 2 - 1, cy = H / 2 - 1;
    fill_screen(BLACK);
    if (tft->wait_buffer) tft->wait_buffer(tft);
    int64_t start = micros();
    for (int i = W < H ? W : H; i > 20; i -= 6) dgx_fill_round_rectangle(tft, cx - i / 2, cy - i / 2, i, i, i / 8, color565(0, i, 0));
    return since(start);
}

typedef struct {
    const char *name;
    uint32_t    us;
} result_t;

static int run(result_t *r)
{
    int n = 0;
#define TEST(label, call)                                            \
    do {                                                             \
        r[n].name = label;                                           \
        r[n].us   = call;                                            \
        if (in_memory) r[n].us += push();                            \
        ESP_LOGI(TAG, "%-24s %10lu", label, (unsigned long)r[n].us); \
        ++n;                                                         \
        vTaskDelay(pdMS_TO_TICKS(100));                              \
    } while (0)
    TEST("HaD pushColor", test_had());
    TEST("Screen fill", test_fill_screen());
    TEST("Text", test_text());
    TEST("Pixels", test_pixels());
    TEST("Lines", test_lines(BLUE));
    TEST("Horiz/Vert Lines", test_fast_lines(RED, BLUE));
    TEST("Rectangles (outline)", test_rects(GREEN));
    TEST("Rectangles (filled)", test_filled_rects(YELLOW, MAGENTA));
    TEST("Circles (filled)", test_filled_circles(10, MAGENTA));
    TEST("Circles (outline)", test_circles(10, WHITE));
    TEST("Triangles (outline)", test_triangles());
    TEST("Triangles (filled)", test_filled_triangles());
    TEST("Rounded rects (outline)", test_round_rects());
    TEST("Rounded rects (filled)", test_filled_round_rects());
#undef TEST
    return n;
}

static uint32_t total_of(const result_t *r, int n)
{
    uint32_t total = 0;
    for (int i = 0; i < n; ++i) total += r[i].us;
    return total;
}

/* the results of both runs on the display, as their sketch shows its table; `m` 0 without the run in memory */
static void show(const result_t *d, const result_t *m, int n, int round)
{
    char line[48];
    use(false);
    for (int i = 0; i < H; i++) dgx_fill_rectangle(tft, 0, i, W, 1, color565(0, 0, 32 + (i * 64) / H));
    cursor.x = 0, cursor.y = 0, cursor.size = 2, cursor.color = MAGENTA;
    println("   DGX test");
    cursor.size = 1, cursor.color = WHITE;
    snprintf(line, sizeof(line), " build %d, round %d", BUILD, round), println(line);
    println("");
    cursor.color = GREEN;
    println(" Benchmark, us        display    memory");
    println("");
    cursor.color = YELLOW;
    for (int i = 0; i < n; ++i) {
        snprintf(line, sizeof(line), " %-19.19s %9lu %9lu", d[i].name, (unsigned long)d[i].us, m ? (unsigned long)m[i].us : 0ul);
        println(line);
    }
    println("");
    cursor.color = CYAN;
    snprintf(line, sizeof(line), " %-19s %9lu %9lu", "Total", (unsigned long)total_of(d, n), m ? (unsigned long)total_of(m, n) : 0ul), println(line);
}

#if CONFIG_IDF_TARGET_ESP32P4
#include "bus/dgx_dsi_esp32.h"
#include "driver/gpio.h"
#include "drivers/jd9165.h"

/* JC1060P470: two lanes of 750 Mbit/s, the DSI PHY powered by the regulator 3 with 2.5 V, the backlight 23 */
static dgx_screen_t *display_init(void)
{
    gpio_set_direction(GPIO_NUM_23, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_23, 1);
    dgx_bus_protocols_t *bus = dgx_dsi_init(0, 2, 750, 3, 2500);
    dgx_screen_t        *scr = bus ? dgx_jd9165_init(bus, GPIO_NUM_0, 16, DgxScreenRGB, DgxDsiOneFrame) : NULL;
    if (scr) dgx_jd9165_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true); /* upright, 600x1024 */
    return scr;
}
#elif CONFIG_IDF_TARGET_ESP32S3
#include "bus/dgx_qspi_esp32.h"
#include "drivers/nv3041a.h"

/* JC4827W543: D0 21, D1 48, D2 40, D3 39, CLK 47, CS 45, the backlight 1, QSPI at 40 MHz */
static dgx_screen_t *display_init(void)
{
    dgx_bus_protocols_t *bus = dgx_qspi_init(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_21, GPIO_NUM_48, GPIO_NUM_40, GPIO_NUM_39, GPIO_NUM_47, GPIO_NUM_45,
                                             40 * 1000 * 1000, 0);
    dgx_screen_t *scr = bus ? dgx_nv3041a_init(bus, GPIO_NUM_NC, GPIO_NUM_1, 16, DgxScreenRGB) : NULL;
    if (scr) dgx_nv3041a_orientation(scr, DgxScreenLeftRight, DgxScreenBottomTop, true); /* upright, 272x480 */
    return scr;
}
#else
#include "bus/dgx_spi_esp32.h"
#include "drivers/ili9341.h"

/* CYD: MOSI 13, MISO 12, CLK 14, CS 15, DC 2, the backlight 21, SPI at 40 MHz */
static dgx_screen_t *display_init(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_2, 40 * 1000 * 1000, 0);
    dgx_screen_t        *scr = bus ? dgx_ili9341_init(bus, GPIO_NUM_NC, GPIO_NUM_21, 16, DgxScreenRGB) : NULL;
    if (scr) dgx_ili9341_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false); /* upright, 240x320 */
    return scr;
}
#endif

void app_main(void)
{
    lcd = display_init();
    if (!lcd) {
        ESP_LOGE(TAG, "no display");
        return;
    }
    W = lcd->width, H = lcd->height;
    /* the run in memory: an 8-bit virtual screen of the display, its colors RGB332 */
    mem = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    static uint8_t rgb[256][3];
    for (int i = 0; i < 256; ++i) {
        const uint8_t r = i & 0xE0, g = (i << 3) & 0xE0, b = (i << 6) & 0xC0;
        rgb[i][0] = r | r >> 3 | r >> 6, rgb[i][1] = g | g >> 3 | g >> 6, rgb[i][2] = b | b >> 2 | b >> 4 | b >> 6;
    }
    dgx_lut_from_rgb(lut, 0, 256, (const uint8_t(*)[3])rgb);
    ESP_LOGI(TAG, "build %d, %dx%d%s", BUILD, W, H, mem ? "" : ", no memory for the run in memory");
    result_t d[16], m[16];
    for (int round = 1;; ++round) {
        ESP_LOGI(TAG, "round %d", round);
        use(false);
        ESP_LOGI(TAG, "Benchmark                Time (microseconds), on the display");
        const int n = run(d);
        ESP_LOGI(TAG, "%-24s %10lu", "Total", (unsigned long)total_of(d, n));
        if (mem) {
            use(true);
            ESP_LOGI(TAG, "Benchmark                Time (microseconds), in memory and sent");
            run(m);
            ESP_LOGI(TAG, "%-24s %10lu", "Total", (unsigned long)total_of(m, n));
        }
        show(d, mem ? m : NULL, n, round);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
