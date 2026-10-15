/*
 * The ellipse, the arc, the sector and the scroll, on one of two boards: a CYD (ESP32-2432S028R, an ILI9341
 * of 320x240 on SPI) or a Guition JC1060P470 (ESP32-P4, a JD9165 of 1024x600 on MIPI-DSI).
 *
 * A round begins with timings, which go to the log, and shows pages A..G one after another. Every page has
 * the number of the build, main/build.h, and its letter, and says what moves its picture: nothing, the memory
 * of a virtual screen, the panel itself, or the frame buffer of the display.
 *
 * The pictures are laid out for 320x240, the screen lying, and 240x320, upright. A larger display shows them
 * so many times larger, in its middle: S() is a size, X() and Y() a place on the display.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
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
#include "dgx_screen.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

#include "build.h"

static dgx_screen_t *lcd;

#if CONFIG_IDF_TARGET_ESP32P4

#include "bus/dgx_dsi_esp32.h"
#include "drivers/jd9165.h"

/* Guition JC1060P470: 16 bits a pixel and two frame buffers, what is drawn between two frames is shown whole */
static bool display_init(void)
{
    gpio_set_direction(GPIO_NUM_23, GPIO_MODE_OUTPUT); /* the backlight */
    gpio_set_level(GPIO_NUM_23, 1);
    /* two lanes of 750 Mbit/s; the regulator 3 of the chip powers the DSI PHY with 2.5 V */
    dgx_bus_protocols_t *bus = dgx_dsi_init(0, 2, 750, 3, 2500);
    if (!bus) return false;
    lcd = dgx_jd9165_init(bus, GPIO_NUM_0, 16, DgxScreenRGB, DgxDsiTwoFrames);
    return lcd != NULL;
}

/* the panel lies as it is made; `turned` is half a turn more */
static void display_orientation(bool upright, bool turned)
{
    if (upright) dgx_jd9165_orientation(lcd, turned ? DgxScreenLeftRight : DgxScreenRightLeft, turned ? DgxScreenBottomTop : DgxScreenTopBottom, true);
    else dgx_jd9165_orientation(lcd, turned ? DgxScreenRightLeft : DgxScreenLeftRight, turned ? DgxScreenBottomTop : DgxScreenTopBottom, false);
}

#else

#include "bus/dgx_spi_esp32.h"
#include "driver/spi_common.h"
#include "drivers/ili9341.h"

/* CYD: MOSI 13, MISO 12, CLK 14, CS 15, DC 2, the backlight 21, SPI at 40 MHz */
static bool display_init(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_2, 40 * 1000 * 1000, 0);
    if (!bus) return false;
    lcd = dgx_ili9341_init(bus, GPIO_NUM_NC, GPIO_NUM_21, 16, DgxScreenRGB);
    return lcd != NULL;
}

/* the panel is upright as it is made, and lies with its axes swapped; `turned` is half a turn more */
static void display_orientation(bool upright, bool turned)
{
    if (upright) dgx_ili9341_orientation(lcd, turned ? DgxScreenRightLeft : DgxScreenLeftRight, turned ? DgxScreenBottomTop : DgxScreenTopBottom, false);
    else dgx_ili9341_orientation(lcd, turned ? DgxScreenLeftRight : DgxScreenRightLeft, turned ? DgxScreenBottomTop : DgxScreenTopBottom, true);
}

#endif

static uint16_t lut[256];
static uint32_t paper, ink, red, green, blue, yellow, grey, white, dark;

/* how many times larger a picture is than it is laid out, and where its corner is on the display */
static int scale = 1, corner_x, corner_y;
#define S(v) ((v) * scale)
#define X(v) (corner_x + (v) * scale)
#define Y(v) (corner_y + (v) * scale)

/* the display turned, and cleared for a picture laid out for 320x240 or, upright, 240x320 */
static void stage(bool upright, bool turned)
{
    display_orientation(upright, turned);
    const int w = upright ? 240 : 320, h = upright ? 320 : 240;
    scale = lcd->width / w < lcd->height / h ? lcd->width / w : lcd->height / h;
    if (scale < 1) scale = 1;
    corner_x = (lcd->width - w * scale) / 2;
    corner_y = (lcd->height - h * scale) / 2;
    dgx_screen_progress_up(lcd);
    if (corner_x || corner_y) dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, dark);
    dgx_fill_rectangle(lcd, X(0), Y(0), S(w), S(h), paper);
    dgx_screen_progress_down(lcd);
}

/* a text, its letters as many times larger as the picture is; the place is that of its base line */
#define TEXT(scr, x, y, color, ...)                                                                                \
    do {                                                                                                           \
        char text_[96];                                                                                            \
        snprintf(text_, sizeof(text_), __VA_ARGS__);                                                               \
        dgx_font_string_utf8_screen(scr, x, y, text_, color, DgxOutputNormal, scale, TerminusTTFMedium12(), NULL, NULL); \
    } while (0)

/*
 * So many microseconds a call, the least of three runs of `n` calls. A run is one batch: a display with two
 * frame buffers shows it after the run, and the time is that of the drawing.
 */
#define TIME(what, n, code)                                                     \
    do {                                                                        \
        int64_t best_ = INT64_MAX;                                              \
        for (int run_ = 0; run_ < 3; ++run_) {                                  \
            dgx_screen_progress_up(lcd);                                        \
            const int64_t t_ = esp_timer_get_time();                            \
            for (int i_ = 0; i_ < (n); ++i_) {                                  \
                code;                                                           \
            }                                                                   \
            if (lcd->wait_buffer) lcd->wait_buffer(lcd);                        \
            const int64_t d_ = esp_timer_get_time() - t_;                       \
            dgx_screen_progress_down(lcd);                                      \
            if (d_ < best_) best_ = d_;                                         \
            vTaskDelay(1);                                                      \
        }                                                                       \
        printf("T %-58s %9.1f us\n", what, (double)best_ / (n));                \
        vTaskDelay(2);                                                          \
    } while (0)

static void pause_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/*
 * What moves the picture on a page, in a box that stays. A display that is not read is moved by its panel,
 * or not at all; one that is read, a DSI display, is moved in its frame buffer pixel by pixel.
 */
static void badge(int x, int y, int w, char letter)
{
    const bool by_panel = !dgx_screen_is_readable(lcd);
    dgx_fill_rectangle(lcd, x, y, w, S(58), by_panel ? red : blue);
    TEXT(lcd, x + S(4), y + S(12), white, "#%d %c", BUILD, letter);
    TEXT(lcd, x + S(4), y + S(26), white, by_panel ? "АППАРАТНЫЙ СКРОЛЛ" : "СКРОЛЛ В КАДРЕ");
    TEXT(lcd, x + S(4), y + S(40), white, by_panel ? "двигает панель" : "строки в памяти");
}

/* so many moves were made, a move took so long */
static void badge_moves(int x, int y, int w, int moves, int64_t us)
{
    dgx_fill_rectangle(lcd, x, y + S(44), w, S(14), dgx_screen_is_readable(lcd) ? blue : red);
    TEXT(lcd, x + S(4), y + S(54), white, "%d по %.1f мс", moves, moves ? (double)us / moves / 1000 : 0.0);
}

/* the sign of Wi-Fi: a dot, and so many arcs of a quarter of a turn over it */
static void wifi_sign(dgx_screen_t *scr, int cx, int cy, int step, int pen, int arcs, uint32_t color)
{
    dgx_solid_circle(scr, cx, cy, pen > 1 ? pen / 2 + 1 : 1, color);
    for (int i = 1; i <= arcs; ++i) dgx_draw_arc(scr, cx, cy, i * step, i * step, 45, 135, DGX_PEN_SOFT | pen, color);
}

/* the sign as it was drawn before there was an arc: a curve of three points for a quarter of a circle */
static void wifi_sign_curves(dgx_screen_t *scr, int cx, int cy, int step, int pen, int arcs, uint32_t color)
{
    dgx_solid_circle(scr, cx, cy, pen > 1 ? pen / 2 + 1 : 1, color);
    for (int i = 1; i <= arcs; ++i) {
        const float    r = (float)(i * step);
        dgx_point_2d_t arc[3] = {
            {(int16_t)lroundf(cx - r * 0.7071f), (int16_t)lroundf(cy - r * 0.7071f)},
            {(int16_t)cx, (int16_t)lroundf(cy - r * 1.4142f)},
            {(int16_t)lroundf(cx + r * 0.7071f), (int16_t)lroundf(cy - r * 0.7071f)},
        };
        dgx_draw_bezier3(scr, arc, 0, DGX_PEN_SOFT | pen, color);
    }
}

/* A: figures drawn straight on the display */
static void page_figures(void)
{
    stage(false, false);
    dgx_screen_progress_up(lcd);
    TEXT(lcd, X(4), Y(12), red, "#%d A  фигуры прямо на дисплей, без скролла", BUILD);
    dgx_draw_ellipse(lcd, X(60), Y(70), S(50), S(30), ink);
    dgx_draw_ellipse(lcd, X(60), Y(70), S(30), S(50), blue);
    dgx_draw_ellipse(lcd, X(60), Y(132), S(50), S(3), ink);
    dgx_draw_ellipse(lcd, X(125), Y(70), S(3), S(45), ink);
    dgx_draw_ellipse(lcd, X(60), Y(132), S(50), 0, red);
    /* a circle and the ellipse of equal radii over it: none of the red is left */
    dgx_draw_circle(lcd, X(175), Y(60), S(28), red);
    dgx_draw_ellipse(lcd, X(175), Y(60), S(28), S(28), green);
    dgx_draw_circle(lcd, X(175), Y(60), S(4), red);
    dgx_draw_ellipse(lcd, X(175), Y(60), S(4), S(4), green);
    /* arcs by a hard pen */
    dgx_draw_arc(lcd, X(262), Y(62), S(45), S(45), 0, 270, 1, ink);
    dgx_draw_arc(lcd, X(262), Y(62), S(34), S(34), 45, 135, S(3), red);
    dgx_draw_arc(lcd, X(262), Y(62), S(22), S(22), 200, 340, S(6), blue);
    dgx_draw_arc(lcd, X(262), Y(62), S(50), S(26), 180, 360, S(2), green);
    /* sectors: a pie with an outline around it, a sector over a half turn with its arc, sectors of an ellipse */
    dgx_solid_sector(lcd, X(62), Y(185), S(45), S(45), 0, 100, red);
    dgx_solid_sector(lcd, X(62), Y(185), S(45), S(45), 100, 190, green);
    dgx_solid_sector(lcd, X(62), Y(185), S(45), S(45), 190, 300, blue);
    dgx_solid_sector(lcd, X(62), Y(185), S(45), S(45), 300, 360, yellow);
    dgx_draw_ellipse(lcd, X(62), Y(185), S(45), S(45), ink);
    dgx_solid_sector(lcd, X(170), Y(185), S(42), S(42), 30, 330, yellow);
    dgx_draw_arc(lcd, X(170), Y(185), S(42), S(42), 30, 330, 1, ink);
    dgx_solid_sector(lcd, X(265), Y(185), S(50), S(26), 20, 160, grey);
    dgx_solid_sector(lcd, X(265), Y(185), S(50), S(26), 200, 340, blue);
    dgx_draw_ellipse(lcd, X(265), Y(185), S(50), S(26), ink);
    dgx_screen_progress_down(lcd);
    pause_ms(9000);
}

/* B: arcs by a soft pen, on a page of amounts of ink shown through a table */
static void page_soft(void)
{
    stage(false, false);
    dgx_screen_t *page = dgx_vscreen_init(S(320), S(240), 8, DgxScreenRGB);
    if (!page) {
        printf("no memory for the page of ink\n");
        return;
    }
    for (int turn = 0; turn < 10; ++turn) {
        const int arcs = turn % 5 > 3 ? 3 : turn % 5;
        dgx_fill_rectangle(page, 0, 0, page->width, page->height, 0);
        TEXT(page, S(4), S(12), 255, "#%d B  мягкое перо, без скролла", BUILD);
        for (int pen = 1; pen <= 5; ++pen) dgx_draw_arc(page, S(30 + (pen - 1) * 54), S(50), S(20), S(20), 0, 360, DGX_PEN_SOFT | S(pen), 255);
        dgx_draw_arc(page, S(285), S(50), S(26), S(14), 0, 360, DGX_PEN_SOFT | S(3), 255);
        dgx_draw_arc(page, S(285), S(50), S(10), S(26), 20, 300, DGX_PEN_SOFT | S(2), 255);
        wifi_sign(page, S(70), S(200), S(30), S(9), arcs, 255);
        wifi_sign(page, S(190), S(200), S(16), S(5), arcs, 255);
        wifi_sign(page, S(270), S(200), S(9), S(3), arcs, 255);
        wifi_sign(page, S(310), S(200), S(5), S(1), arcs, 255);
        /* the same by curves of three points, as it was done before: they rise over the circle */
        wifi_sign_curves(page, S(190), S(110), S(16), S(5), 3, 255);
        TEXT(page, S(215), S(100), 255, "кривыми");
        dgx_vscreen8_to_screen16(lcd, X(0), Y(0), page, lut, 0);
        pause_ms(turn % 5 == 4 ? 1500 : 450);
    }
    dgx_screen_destroy(&page);
}

/* C: a virtual screen scrolled in its memory and sent to the display */
static void page_memory(void)
{
    stage(false, false);
    dgx_screen_t *band = dgx_vscreen_init(S(320), S(112), 16, DgxScreenRGB);
    if (!band) {
        printf("no memory for the band\n");
        return;
    }
    dgx_screen_progress_up(lcd);
    dgx_fill_rectangle(lcd, X(0), Y(0), S(320), S(46), blue);
    TEXT(lcd, X(4), Y(12), white, "#%d C", BUILD);
    TEXT(lcd, X(4), Y(26), white, "СКРОЛЛ В ПАМЯТИ, не аппаратный");
    TEXT(lcd, X(4), Y(40), white, "виртуальный экран двигаем и отправляем целиком");
    dgx_screen_progress_down(lcd);
    dgx_fill_rectangle(band, 0, 0, band->width, band->height, yellow);
    for (int i = 0; i < 26; ++i) {
        /* a line of text more: the picture goes up by the height of a line, the new line is drawn at the bottom */
        dgx_scroll(band, 0, 0, band->width, band->height, 0, -S(14), yellow);
        TEXT(band, S(4), band->height - S(4), ink, "строка %d уходит вверх на высоту строки", i + 1);
        dgx_vscreen_to_screen(lcd, X(0), Y(60), band);
        pause_ms(110);
    }
    for (int i = 0; i < 40; ++i) {
        dgx_scroll(band, 0, 0, band->width, band->height, S(6), 0, i & 1 ? green : yellow);
        dgx_vscreen_to_screen(lcd, X(0), Y(60), band);
        pause_ms(40);
    }
    for (int i = 0; i < 26; ++i) {
        dgx_scroll(band, 0, 0, band->width, band->height, 0, S(14), yellow);
        TEXT(band, S(4), S(10), ink, "строка %d приходит сверху", i + 1);
        dgx_vscreen_to_screen(lcd, X(0), Y(60), band);
        pause_ms(60);
    }
    /* a part of the band, aslant */
    for (int i = 0; i < 30; ++i) {
        dgx_scroll(band, S(40), S(10), S(200), S(90), -S(5), S(3), blue);
        dgx_vscreen_to_screen(lcd, X(0), Y(60), band);
        pause_ms(50);
    }
    dgx_screen_destroy(&band);
    pause_ms(1200);
}

/*
 * D and E: a chart, the screen lying. The picture goes left and a new piece of it is drawn at the right
 * edge; what is beside the chart stays. An ILI9341 scrolls along its long side, which lies along x here, a
 * rectangle from the top of the screen to its bottom: the panel moves the chart itself. A DSI display is
 * moved in its frame buffer.
 */
static void page_chart(char letter, bool turned)
{
    stage(false, turned);
    const int left = 112, width = 200, right = left + width - 1, step = 3;
    dgx_screen_progress_up(lcd);
    dgx_fill_rectangle(lcd, X(0), Y(0), S(left), S(240), grey);
    dgx_fill_rectangle(lcd, X(right + 1), Y(0), S(320 - right - 1), S(240), grey);
    badge(X(0), Y(0), S(left), letter);
    TEXT(lcd, X(4), Y(80), ink, "серое стоит,");
    TEXT(lcd, X(4), Y(94), ink, "график едет");
    TEXT(lcd, X(4), Y(108), ink, "влево, новое");
    TEXT(lcd, X(4), Y(122), ink, "рисуем справа");
    dgx_screen_progress_down(lcd);
    int           refused = 0, moves = 0, before = 120;
    int64_t       spent = 0;
    const int64_t until = esp_timer_get_time() + 12 * 1000000LL;
    for (int i = 0; i < 330 && esp_timer_get_time() < until; ++i) {
        /* a step is one picture: a display that shows by batches shows it whole */
        dgx_screen_progress_up(lcd);
        const int64_t t = esp_timer_get_time();
        if (dgx_scroll(lcd, X(left), Y(0), S(width), S(240), -S(step), 0, paper) == ESP_OK) ++moves;
        else ++refused;
        spent += esp_timer_get_time() - t;
        if (i % 10 == 9) badge_moves(X(0), Y(0), S(left), moves, spent);
        const int now = 120 + (int)lroundf(70 * sinf(i * 0.11f) + 25 * sinf(i * 0.37f));
        for (int k = 0; k < 2 * scale; ++k) dgx_draw_line(lcd, X(right - step), Y(before) + k, X(right) + scale - 1, Y(now) + k, blue);
        dgx_fill_rectangle(lcd, X(right), Y(120), S(1), S(1), grey);
        before = now;
        /* things drawn in the middle of the chart: where the rows of a panel that scrolls turn round in its memory, sooner or later */
        if (i % 40 == 20) {
            dgx_solid_circle(lcd, X(left + 100), Y(215), S(12), yellow);
            dgx_draw_ellipse(lcd, X(left + 100), Y(215), S(12), S(12), ink);
            TEXT(lcd, X(left + 70), Y(200), ink, "шаг %d", i);
            dgx_fill_rectangle(lcd, X(left + 10), Y(228), S(180), S(3), green);
        }
        if (i % 40 == 0) TEXT(lcd, X(right - 30), Y(36), ink, "%d", i);
        dgx_screen_progress_down(lcd);
        pause_ms(18);
    }
    printf("chart %c: %d moves, %.2f ms a move, %d refused\n", letter, moves, moves ? (double)spent / moves / 1000 : 0.0, refused);
    /* a frame at the very edges of the chart, drawn where it is seen whatever has been scrolled */
    dgx_screen_progress_up(lcd);
    badge_moves(X(0), Y(0), S(left), moves, spent);
    dgx_fill_rectangle(lcd, X(left), Y(0), S(width), S(2), red);
    dgx_fill_rectangle(lcd, X(left), Y(238), S(width), S(2), red);
    dgx_fill_rectangle(lcd, X(left), Y(0), S(2), S(240), red);
    dgx_fill_rectangle(lcd, X(right - 1), Y(0), S(2), S(240), red);
    TEXT(lcd, X(left + 40), Y(130), red, "рамка по краям области");
    dgx_screen_progress_down(lcd);
    pause_ms(2500);
    /* a move as large as the rectangle only fills it; a panel that was scrolling lets its area go */
    dgx_scroll(lcd, X(left), Y(0), S(width), S(240), S(width), 0, paper);
}

/*
 * F and G: a terminal, the screen upright. A head and a foot stay, the lines between them go up, and then
 * down. On an ILI9341 the long side of the panel is along y here, and the panel moves the lines itself.
 */
static void page_terminal(char letter, bool turned)
{
    stage(true, turned);
    const int top = 58, height = 244, line = 14;
    dgx_screen_progress_up(lcd);
    dgx_fill_rectangle(lcd, X(0), Y(top + height), S(240), S(320 - top - height), grey);
    badge(X(0), Y(0), S(240), letter);
    TEXT(lcd, X(130), Y(12), white, "шапка стоит");
    TEXT(lcd, X(4), Y(top + height + 13), ink, "подвал стоит");
    dgx_screen_progress_down(lcd);
    int           refused = 0, moves = 0;
    int64_t       spent = 0;
    const int64_t until = esp_timer_get_time() + 14 * 1000000LL;
    for (int i = 0; i < 66 && esp_timer_get_time() < until; ++i) {
        const bool up = i < 46;
        dgx_screen_progress_up(lcd);
        const int64_t t = esp_timer_get_time();
        if (dgx_scroll(lcd, X(0), Y(top), S(240), S(height), 0, up ? -S(line) : S(line), paper) == ESP_OK) ++moves;
        else ++refused;
        spent += esp_timer_get_time() - t;
        badge_moves(X(0), Y(0), S(240), moves, spent);
        if (up) TEXT(lcd, X(4), Y(top + height - 4), i % 5 ? ink : blue, "%2d: строки идут вверх", i + 1);
        else TEXT(lcd, X(4), Y(top + 11), red, "%2d: и вниз", i - 45);
        if (up && i % 9 == 4) dgx_solid_sector(lcd, X(200), Y(top + height / 2), S(24), S(24), 20, 340, yellow);
        dgx_screen_progress_down(lcd);
        pause_ms(90);
    }
    printf("terminal %c: %d moves, %.2f ms a move, %d refused\n", letter, moves, moves ? (double)spent / moves / 1000 : 0.0, refused);
    dgx_screen_progress_up(lcd);
    dgx_draw_line(lcd, X(0), Y(top), X(239), Y(top + height - 1), red);
    dgx_draw_line(lcd, X(0), Y(top + height - 1), X(239), Y(top), red);
    dgx_screen_progress_down(lcd);
    pause_ms(2500);
    dgx_scroll(lcd, X(0), Y(top), S(240), S(height), 0, S(height), paper);
}

static uint32_t rnd(void)
{
    static uint32_t seed = 2463534242u;
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return seed;
}

static int between(int from, int to)
{
    return from + (int)(rnd() % (uint32_t)(to - from + 1));
}

/*
 * A display that is read: the same things are drawn and the same moves are made on it and on a virtual
 * screen, and then every pixel of the two is compared. Half of the moves are each a batch of its own, shown
 * before the next one, the rest are one batch. Returns the number of pixels that differ, below 0 for no memory.
 */
static int check_scroll(const char *name)
{
    const int     w = 400 < lcd->width - 80 ? 400 : lcd->width - 80, h = 300 < lcd->height - 60 ? 300 : lcd->height - 60, x0 = 40, y0 = 30;
    dgx_screen_t *ref = dgx_vscreen_init(w, h, 16, DgxScreenRGB);
    if (!ref) return -1;
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, dark);
    dgx_fill_rectangle(lcd, x0, y0, w, h, paper);
    dgx_fill_rectangle(ref, 0, 0, w, h, paper);
    int moves = 0;
    for (int i = 0; i < 160; ++i) {
        if (i == 80) dgx_screen_progress_up(lcd);
        const uint32_t color = dgx_rgb_to_16((uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd());
        const int      x = between(-20, w - 1), y = between(-20, h - 1), rw = between(1, w), rh = between(1, h);
        switch (rnd() % 5) {
        case 0:
            dgx_fill_rectangle(lcd, x0 + (x < 0 ? 0 : x), y0 + (y < 0 ? 0 : y), rw / 3 < w - (x < 0 ? 0 : x) ? rw / 3 : w - (x < 0 ? 0 : x),
                               rh / 3 < h - (y < 0 ? 0 : y) ? rh / 3 : h - (y < 0 ? 0 : y), color);
            dgx_fill_rectangle(ref, x < 0 ? 0 : x, y < 0 ? 0 : y, rw / 3, rh / 3, color);
            break;
        case 1: {
            const int   cx = between(60, w - 60), cy = between(60, h - 60), r = between(3, 50);
            const float from = (float)between(0, 360), to = (float)between(0, 720);
            dgx_solid_sector(lcd, x0 + cx, y0 + cy, r, r / 2 + 2, from, to, color);
            dgx_solid_sector(ref, cx, cy, r, r / 2 + 2, from, to, color);
            break;
        }
        default: {
            /* a rectangle within the region, moved any way, at times further than it is large */
            const int sx = x < 0 ? 0 : x, sy = y < 0 ? 0 : y, sw = rw < w - sx ? rw : w - sx, sh = rh < h - sy ? rh : h - sy;
            const int dx = between(-40, 40) * (rnd() % 7 ? 1 : 12), dy = between(-30, 30) * (rnd() % 7 ? 1 : 12);
            const esp_err_t a = dgx_scroll(lcd, x0 + sx, y0 + sy, sw, sh, dx, dy, color);
            const esp_err_t b = dgx_scroll(ref, sx, sy, sw, sh, dx, dy, color);
            if (a != b) printf("CHECK %s: a move refused on one screen only\n", name);
            ++moves;
            break;
        }
        }
    }
    dgx_screen_progress_down(lcd);
    int wrong = 0, around = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) wrong += dgx_get_pixel(lcd, x0 + x, y0 + y) != dgx_get_pixel(ref, x, y);
    }
    /* nothing is to be touched beside the region */
    for (int x = 0; x < lcd->width; ++x) around += dgx_get_pixel(lcd, x, y0 - 1) != dark, around += dgx_get_pixel(lcd, x, y0 + h) != dark;
    for (int y = 0; y < lcd->height; ++y) around += dgx_get_pixel(lcd, x0 - 1, y) != dark, around += dgx_get_pixel(lcd, x0 + w, y) != dark;
    printf("CHECK %s: %d moves and other drawing on %dx%d: %d pixels differ, %d touched beside it\n", name, moves, w, h, wrong, around);
    dgx_screen_destroy(&ref);
    return wrong + around;
}

static void timings(void)
{
    stage(false, false);
    TEXT(lcd, X(4), Y(12), red, "#%d  замеры, смотри лог", BUILD);
    printf("T build %d, display %dx%d, read %d, free heap %u, largest block %u\n", BUILD, lcd->width, lcd->height, dgx_screen_is_readable(lcd),
           (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    /* straight on the display; the sizes are in pixels on every board */
    TIME("display: dgx_draw_circle r 50", 50, dgx_draw_circle(lcd, 160, 130, 50, ink));
    TIME("display: dgx_draw_ellipse 50 x 50", 50, dgx_draw_ellipse(lcd, 160, 130, 50, 50, blue));
    TIME("display: dgx_draw_ellipse 80 x 40", 50, dgx_draw_ellipse(lcd, 160, 130, 80, 40, blue));
    TIME("display: dgx_draw_arc 50 x 50, 0..360, pen 1", 50, dgx_draw_arc(lcd, 160, 130, 50, 50, 0, 360, 1, red));
    TIME("display: dgx_draw_arc 50 x 50, 45..135, pen 1", 50, dgx_draw_arc(lcd, 160, 130, 50, 50, 45, 135, 1, ink));
    TIME("display: dgx_draw_arc 50 x 50, 45..135, pen 5", 20, dgx_draw_arc(lcd, 160, 130, 50, 50, 45, 135, 5, green));
    TIME("display: dgx_solid_circle r 50", 20, dgx_solid_circle(lcd, 160, 130, 50, yellow));
    TIME("display: dgx_solid_sector 50 x 50, 0..360", 20, dgx_solid_sector(lcd, 160, 130, 50, 50, 0, 360, grey));
    TIME("display: dgx_solid_sector 50 x 50, 30..300", 20, dgx_solid_sector(lcd, 160, 130, 50, 50, 30, 300, blue));
    TIME("display: dgx_solid_sector 80 x 40, 30..120", 20, dgx_solid_sector(lcd, 160, 130, 80, 40, 30, 120, red));
    /* on virtual screens */
    dgx_screen_t *page = dgx_vscreen_init(320, 240, 8, DgxScreenRGB);
    if (page) {
        dgx_screen_progress_up(page);
        TIME("8 bits: dgx_draw_circle r 50", 200, dgx_draw_circle(page, 160, 120, 50, 255));
        TIME("8 bits: dgx_draw_ellipse 50 x 50", 200, dgx_draw_ellipse(page, 160, 120, 50, 50, 255));
        TIME("8 bits: dgx_draw_ellipse 80 x 40", 200, dgx_draw_ellipse(page, 160, 120, 80, 40, 255));
        TIME("8 bits: dgx_solid_circle r 50", 200, dgx_solid_circle(page, 160, 120, 50, 255));
        TIME("8 bits: dgx_solid_sector 50 x 50, 0..360", 200, dgx_solid_sector(page, 160, 120, 50, 50, 0, 360, 255));
        TIME("8 bits: dgx_solid_sector 50 x 50, 30..300", 200, dgx_solid_sector(page, 160, 120, 50, 50, 30, 300, 255));
        TIME("8 bits: Wi-Fi sign, 3 arcs of 16 a step, soft pen 5", 50, wifi_sign(page, 160, 200, 16, 5, 3, 255));
        TIME("8 bits: the same by curves of three points", 50, wifi_sign_curves(page, 160, 200, 16, 5, 3, 255));
        TIME("8 bits: Wi-Fi sign, 3 arcs of 30 a step, soft pen 9", 20, wifi_sign(page, 160, 200, 30, 9, 3, 255));
        TIME("8 bits: the same by curves of three points", 20, wifi_sign_curves(page, 160, 200, 30, 9, 3, 255));
        TIME("8 bits: soft arc 40 x 40, 0..360, pen 3", 50, dgx_draw_arc(page, 160, 120, 40, 40, 0, 360, DGX_PEN_SOFT | 3, 255));
        TIME("8 bits: hard arc 40 x 40, 0..360, pen 3", 50, dgx_draw_arc(page, 160, 120, 40, 40, 0, 360, 3, 255));
        TIME("8 bits 320x240: dgx_scroll up by 14", 50, dgx_scroll(page, 0, 0, 320, 240, 0, -14, 0));
        TIME("8 bits 320x240: dgx_scroll left by 4", 50, dgx_scroll(page, 0, 0, 320, 240, -4, 0, 0));
        TIME("8 bits 320x240: dgx_scroll aslant by 3, -2", 50, dgx_scroll(page, 0, 0, 320, 240, 3, -2, 0));
        dgx_screen_destroy(&page);
    }
    dgx_screen_t *band = dgx_vscreen_init(320, 112, 16, DgxScreenRGB);
    if (band) {
        dgx_screen_progress_up(band);
        TIME("16 bits 320x112: dgx_scroll up by 14", 50, dgx_scroll(band, 0, 0, 320, 112, 0, -14, 0));
        TIME("16 bits 320x112: dgx_scroll left by 4", 50, dgx_scroll(band, 0, 0, 320, 112, -4, 0, 0));
        TIME("16 bits 320x112: dgx_vscreen_to_screen to the display", 10, dgx_vscreen_to_screen(lcd, 0, 60, band));
        dgx_screen_destroy(&band);
    }
    dgx_screen_t *twelve = dgx_vscreen_init(320, 112, 12, DgxScreenRGB);
    if (twelve) {
        dgx_screen_progress_up(twelve);
        TIME("12 bits 320x112: dgx_scroll up by 14, pixel by pixel", 5, dgx_scroll(twelve, 0, 0, 320, 112, 0, -14, 0));
        dgx_screen_destroy(&twelve);
    }
    if (dgx_screen_is_readable(lcd)) {
        /* a display that is read is moved in its frame buffer, pixel by pixel */
        TIME("display: dgx_scroll 320x240 up by 14, in the frame", 5, dgx_scroll(lcd, 0, 0, 320, 240, 0, -14, paper));
        TIME("display: dgx_scroll 320x240 left by 4, in the frame", 5, dgx_scroll(lcd, 0, 0, 320, 240, -4, 0, paper));
        TIME("display: dgx_scroll 320x240 aslant by 3, -2, in the frame", 5, dgx_scroll(lcd, 0, 0, 320, 240, 3, -2, paper));
        TIME("display: dgx_scroll the whole screen up by 28, in the frame", 3, dgx_scroll(lcd, 0, 0, lcd->width, lcd->height, 0, -28, paper));
        TIME("display: dgx_scroll the whole screen left by 6", 3, dgx_scroll(lcd, 0, 0, lcd->width, lcd->height, -6, 0, paper));
        int wrong = check_scroll("lying");
        display_orientation(true, false);
        wrong += check_scroll("upright");
        display_orientation(false, true);
        wrong += check_scroll("lying, turned");
        stage(false, false);
        TEXT(lcd, X(4), Y(12), red, "#%d  проверка скролла в кадре", BUILD);
        if (wrong) TEXT(lcd, X(4), Y(40), red, "пикселей не сошлось: %d", wrong);
        else TEXT(lcd, X(4), Y(40), green, "все пиксели сошлись с виртуальным экраном");
        pause_ms(4000);
    } else {
        /* a display that is not read: the panel moves the picture, and a window across the turn of its rows goes in pieces */
        band = dgx_vscreen_init(320, 112, 16, DgxScreenRGB);
        if (band) {
            dgx_scroll(lcd, 0, 0, 320, 240, -100, 0, paper);
            TIME("16 bits 320x112 to the display, the panel scrolled by 100", 10, dgx_vscreen_to_screen(lcd, 0, 60, band));
            TIME("display: dgx_scroll left by 3, the panel", 50, dgx_scroll(lcd, 0, 0, 320, 240, -3, 0, paper));
            dgx_scroll(lcd, 0, 0, 320, 240, 320, 0, paper);
            dgx_screen_destroy(&band);
        }
        display_orientation(true, false);
        dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, paper);
        TIME("display upright: dgx_scroll up by 14, the panel", 50, dgx_scroll(lcd, 0, 24, 240, 280, 0, -14, paper));
        dgx_scroll(lcd, 0, 24, 240, 280, 0, 280, paper);
        dgx_scroll(lcd, 0, 24, 240, 280, 0, -100, paper);
        dgx_screen_t *lines = dgx_vscreen_init(240, 112, 16, DgxScreenRGB);
        if (lines) {
            TIME("display upright: 240x112 sent across the turn of the rows", 10, dgx_vscreen_to_screen(lcd, 0, 150, lines));
            TIME("display upright: 240x112 sent beside it", 10, dgx_vscreen_to_screen(lcd, 0, 30, lines));
            dgx_screen_destroy(&lines);
        }
        TIME("display upright: fill of 240 x 14", 50, dgx_fill_rectangle(lcd, 0, 100, 240, 14, yellow));
        dgx_scroll(lcd, 0, 24, 240, 280, 0, 280, paper);
    }
    printf("T done\n");
}

void app_main(void)
{
    if (!display_init()) {
        printf("no display\n");
        return;
    }
    paper  = dgx_rgb_to_16(250, 246, 232);
    ink    = dgx_rgb_to_16(24, 36, 120);
    red    = dgx_rgb_to_16(200, 40, 30);
    green  = dgx_rgb_to_16(30, 150, 60);
    blue   = dgx_rgb_to_16(40, 90, 220);
    yellow = dgx_rgb_to_16(240, 200, 40);
    grey   = dgx_rgb_to_16(170, 170, 170);
    white  = dgx_rgb_to_16(255, 255, 255);
    dark   = dgx_rgb_to_16(40, 40, 48);
    /* the table of the page of ink: from the paper at 0 to the ink at 255 */
    dgx_lut_gradient(lut, 0, 256, dgx_rgb_to_24(250, 246, 232), dgx_rgb_to_24(24, 36, 120));
    printf("build %d, display %dx%d, read %d\n", BUILD, lcd->width, lcd->height, dgx_screen_is_readable(lcd));
    for (int round = 0;; ++round) {
        timings();
        page_figures();
        page_soft();
        page_memory();
        page_chart('D', false);
        page_chart('E', true);
        page_terminal('F', false);
        page_terminal('G', true);
        printf("round %d done\n", round);
    }
}
