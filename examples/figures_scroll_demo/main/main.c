/*
 * The ellipse, the arc, the sector and the scroll on a CYD (ESP32-2432S028R, ILI9341 on SPI).
 *
 * A round begins with timings, which go to the log, and shows pages A..G one after another. Every page has
 * the number of the build, main/build.h, and its letter, and says what moves its picture: nothing, the memory
 * of a virtual screen, or the panel itself. On the pages the panel scrolls, the number of moves it has made
 * is counted on the screen: the display is not read, so a move that was made was made by the panel.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
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
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

#include "build.h"

static dgx_screen_t *lcd;
static uint16_t      lut[256];
static uint32_t      paper, ink, red, green, blue, yellow, grey;

#define TEXT(scr, x, y, color, ...)                                                                            \
    do {                                                                                                       \
        char text_[96];                                                                                        \
        snprintf(text_, sizeof(text_), __VA_ARGS__);                                                           \
        dgx_font_string_utf8_screen(scr, x, y, text_, color, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL); \
    } while (0)

/* so many microseconds a call, the least of a few runs of `n` calls */
#define TIME(what, n, code)                                                     \
    do {                                                                        \
        int64_t best_ = INT64_MAX;                                              \
        for (int run_ = 0; run_ < 3; ++run_) {                                  \
            const int64_t t_ = esp_timer_get_time();                            \
            for (int i_ = 0; i_ < (n); ++i_) {                                  \
                code;                                                           \
            }                                                                   \
            if (lcd->wait_buffer) lcd->wait_buffer(lcd);                        \
            const int64_t d_ = esp_timer_get_time() - t_;                       \
            if (d_ < best_) best_ = d_;                                         \
            vTaskDelay(1);                                                      \
        }                                                                       \
        printf("T %-58s %9.1f us\n", what, (double)best_ / (n));                \
        vTaskDelay(2);                                                          \
    } while (0)

/* what moves the picture on a page, in a box that stays: the panel itself, or the memory of a virtual screen */
static uint32_t white;

static void badge_panel(int x, int y, int w, char letter, int moves)
{
    dgx_fill_rectangle(lcd, x, y, w, 58, red);
    TEXT(lcd, x + 4, y + 12, white, "#%d %c", BUILD, letter);
    TEXT(lcd, x + 4, y + 26, white, "АППАРАТНЫЙ СКРОЛЛ");
    TEXT(lcd, x + 4, y + 40, white, "двигает панель");
    TEXT(lcd, x + 4, y + 54, white, "сдвигов: %d", moves);
}

static void badge_moves(int x, int y, int w, int moves)
{
    dgx_fill_rectangle(lcd, x, y + 44, w, 14, red);
    TEXT(lcd, x + 4, y + 54, white, "сдвигов: %d", moves);
}

static void pause_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

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

/* A: figures drawn straight on the display, through its window */
static void page_figures(void)
{
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, paper);
    TEXT(lcd, 4, 12, red, "#%d A  фигуры прямо на дисплей, без скролла", BUILD);
    dgx_draw_ellipse(lcd, 60, 70, 50, 30, ink);
    dgx_draw_ellipse(lcd, 60, 70, 30, 50, blue);
    dgx_draw_ellipse(lcd, 60, 132, 50, 3, ink);
    dgx_draw_ellipse(lcd, 125, 70, 3, 45, ink);
    dgx_draw_ellipse(lcd, 60, 132, 50, 0, red);
    /* a circle and the ellipse of equal radii over it: none of the red is left */
    dgx_draw_circle(lcd, 175, 60, 28, red);
    dgx_draw_ellipse(lcd, 175, 60, 28, 28, green);
    dgx_draw_circle(lcd, 175, 60, 4, red);
    dgx_draw_ellipse(lcd, 175, 60, 4, 4, green);
    /* arcs by a hard pen */
    dgx_draw_arc(lcd, 262, 62, 45, 45, 0, 270, 1, ink);
    dgx_draw_arc(lcd, 262, 62, 34, 34, 45, 135, 3, red);
    dgx_draw_arc(lcd, 262, 62, 22, 22, 200, 340, 6, blue);
    dgx_draw_arc(lcd, 262, 62, 50, 26, 180, 360, 2, green);
    /* sectors: a pie, a sector over a half turn, sectors of an ellipse */
    dgx_solid_sector(lcd, 62, 185, 45, 45, 0, 100, red);
    dgx_solid_sector(lcd, 62, 185, 45, 45, 100, 190, green);
    dgx_solid_sector(lcd, 62, 185, 45, 45, 190, 300, blue);
    dgx_solid_sector(lcd, 62, 185, 45, 45, 300, 360, yellow);
    dgx_draw_ellipse(lcd, 62, 185, 45, 45, ink);
    dgx_solid_sector(lcd, 170, 185, 42, 42, 30, 330, yellow);
    dgx_draw_arc(lcd, 170, 185, 42, 42, 30, 330, 1, ink);
    dgx_solid_sector(lcd, 265, 185, 50, 26, 20, 160, grey);
    dgx_solid_sector(lcd, 265, 185, 50, 26, 200, 340, blue);
    dgx_draw_ellipse(lcd, 265, 185, 50, 26, ink);
    pause_ms(9000);
}

/* B: arcs by a soft pen, on a page of amounts of ink shown through a table */
static void page_soft(void)
{
    dgx_screen_t *page = dgx_vscreen_init(lcd->width, lcd->height, 8, DgxScreenRGB);
    if (!page) {
        printf("no memory for the page of ink\n");
        return;
    }
    for (int turn = 0; turn < 10; ++turn) {
        const int arcs = turn % 5 > 3 ? 3 : turn % 5;
        dgx_fill_rectangle(page, 0, 0, page->width, page->height, 0);
        TEXT(page, 4, 12, 255, "#%d B  мягкое перо, без скролла", BUILD);
        for (int pen = 1; pen <= 5; ++pen) dgx_draw_arc(page, 30 + (pen - 1) * 54, 50, 20, 20, 0, 360, DGX_PEN_SOFT | pen, 255);
        dgx_draw_arc(page, 285, 50, 26, 14, 0, 360, DGX_PEN_SOFT | 3, 255);
        dgx_draw_arc(page, 285, 50, 10, 26, 20, 300, DGX_PEN_SOFT | 2, 255);
        wifi_sign(page, 70, 200, 30, 9, arcs, 255);
        wifi_sign(page, 190, 200, 16, 5, arcs, 255);
        wifi_sign(page, 270, 200, 9, 3, arcs, 255);
        wifi_sign(page, 310, 200, 5, 1, arcs, 255);
        /* the same by curves of three points, as it was done before: they rise over the circle */
        wifi_sign_curves(page, 190, 110, 16, 5, 3, 255);
        TEXT(page, 215, 100, 255, "кривыми");
        dgx_vscreen8_to_screen16(lcd, 0, 0, page, lut, 0);
        pause_ms(turn % 5 == 4 ? 1500 : 450);
    }
    dgx_screen_destroy(&page);
}

/* C: a virtual screen scrolled in its memory and sent to the display */
static void page_memory(void)
{
    dgx_screen_t *band = dgx_vscreen_init(lcd->width, 112, 16, DgxScreenRGB);
    if (!band) {
        printf("no memory for the band\n");
        return;
    }
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, paper);
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, 46, blue);
    TEXT(lcd, 4, 12, white, "#%d C", BUILD);
    TEXT(lcd, 4, 26, white, "СКРОЛЛ В ПАМЯТИ, не аппаратный");
    TEXT(lcd, 4, 40, white, "виртуальный экран двигаем и отправляем целиком");
    dgx_fill_rectangle(band, 0, 0, band->width, band->height, yellow);
    for (int i = 0; i < 26; ++i) {
        dgx_scroll(band, 0, 0, band->width, band->height, 0, -14, yellow);
        TEXT(band, 4, band->height - 4, ink, "строка %d уходит вверх на высоту строки", i + 1);
        dgx_vscreen_to_screen(lcd, 0, 60, band);
        pause_ms(110);
    }
    for (int i = 0; i < 40; ++i) {
        dgx_scroll(band, 0, 0, band->width, band->height, 6, 0, i & 1 ? green : yellow);
        dgx_vscreen_to_screen(lcd, 0, 60, band);
        pause_ms(40);
    }
    for (int i = 0; i < 26; ++i) {
        dgx_scroll(band, 0, 0, band->width, band->height, 0, 14, yellow);
        TEXT(band, 4, 10, ink, "строка %d приходит сверху", i + 1);
        dgx_vscreen_to_screen(lcd, 0, 60, band);
        pause_ms(60);
    }
    /* a part of the band, aslant */
    for (int i = 0; i < 30; ++i) {
        dgx_scroll(band, 40, 10, 200, 90, -5, 3, blue);
        dgx_vscreen_to_screen(lcd, 0, 60, band);
        pause_ms(50);
    }
    dgx_screen_destroy(&band);
    pause_ms(1200);
}

/*
 * D and E: the panel scrolls along its long side, which lies along x here. A chart: the picture goes left by
 * the panel, a new piece is drawn at the right. Columns 40..279 are scrolled, what is beside them stays.
 */
static void page_chart(char letter, dgx_orientation_t dir_x, dgx_orientation_t dir_y)
{
    dgx_ili9341_orientation(lcd, dir_x, dir_y, true);
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, paper);
    const int left = 112, width = 200, right = left + width - 1;
    dgx_fill_rectangle(lcd, 0, 0, left, lcd->height, grey);
    dgx_fill_rectangle(lcd, right + 1, 0, lcd->width - right - 1, lcd->height, grey);
    badge_panel(0, 0, left, letter, 0);
    TEXT(lcd, 4, 80, ink, "серое стоит,");
    TEXT(lcd, 4, 94, ink, "график едет");
    TEXT(lcd, 4, 108, ink, "влево, новое");
    TEXT(lcd, 4, 122, ink, "рисуем справа");
    int refused = 0, moves = 0, before = 120;
    for (int i = 0; i < 330; ++i) {
        const int step = 3;
        /* a display that is not read: when the picture has moved, the panel has moved it */
        if (dgx_scroll(lcd, left, 0, width, lcd->height, -step, 0, paper)) ++moves;
        else ++refused;
        if (i % 10 == 9) badge_moves(0, 0, left, moves);
        const int now = 120 + (int)lroundf(70 * sinf(i * 0.11f) + 25 * sinf(i * 0.37f));
        dgx_draw_line(lcd, right - step, before, right, now, blue);
        dgx_draw_line(lcd, right - step, before + 1, right, now + 1, blue);
        dgx_set_pixel(lcd, right, 120, grey);
        before = now;
        /* things drawn in the middle of the area and across the place where its rows turn round in memory */
        if (i % 40 == 20) {
            dgx_solid_circle(lcd, left + 100, 215, 12, yellow);
            dgx_draw_ellipse(lcd, left + 100, 215, 12, 12, ink);
            TEXT(lcd, left + 70, 200, ink, "шаг %d", i);
            dgx_fill_rectangle(lcd, left + 10, 228, 180, 3, green);
        }
        if (i % 40 == 0) TEXT(lcd, right - 30, 36, ink, "%d", i);
        pause_ms(18);
    }
    if (refused) printf("chart %c: %d moves refused\n", letter, refused);
    /* a frame at the very edges of the area, drawn where it is seen whatever the shift */
    dgx_fill_rectangle(lcd, left, 0, width, 2, red);
    dgx_fill_rectangle(lcd, left, lcd->height - 2, width, 2, red);
    dgx_fill_rectangle(lcd, left, 0, 2, lcd->height, red);
    dgx_fill_rectangle(lcd, right - 1, 0, 2, lcd->height, red);
    TEXT(lcd, left + 40, 130, red, "рамка по краям области");
    pause_ms(2500);
    /* the area is let go: a move as large as it only fills it */
    dgx_scroll(lcd, left, 0, width, lcd->height, width, 0, paper);
}

/* F and G: the screen upright, the long side of the panel along y. A terminal: a head and a foot stay, lines go up. */
static void page_terminal(char letter, dgx_orientation_t dir_x, dgx_orientation_t dir_y)
{
    dgx_ili9341_orientation(lcd, dir_x, dir_y, false);
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, paper);
    const int top = 58, height = 244, line = 14;
    dgx_fill_rectangle(lcd, 0, top + height, lcd->width, lcd->height - top - height, grey);
    badge_panel(0, 0, lcd->width, letter, 0);
    TEXT(lcd, 130, 12, white, "шапка стоит");
    TEXT(lcd, 4, top + height + 13, ink, "подвал стоит");
    int refused = 0, moves = 0;
    for (int i = 0; i < 46; ++i) {
        if (dgx_scroll(lcd, 0, top, lcd->width, height, 0, -line, paper)) ++moves;
        else ++refused;
        badge_moves(0, 0, lcd->width, moves);
        TEXT(lcd, 4, top + height - 4, i % 5 ? ink : blue, "%2d: панель двигает строки вверх", i + 1);
        if (i % 9 == 4) dgx_solid_sector(lcd, 200, top + height / 2, 24, 24, 20, 340, yellow);
        pause_ms(90);
    }
    for (int i = 0; i < 20; ++i) {
        if (dgx_scroll(lcd, 0, top, lcd->width, height, 0, line, paper)) ++moves;
        else ++refused;
        badge_moves(0, 0, lcd->width, moves);
        TEXT(lcd, 4, top + 11, red, "%2d: и вниз", i + 1);
        pause_ms(90);
    }
    if (refused) printf("terminal %c: %d moves refused\n", letter, refused);
    dgx_draw_line(lcd, 0, top, lcd->width - 1, top + height - 1, red);
    dgx_draw_line(lcd, 0, top + height - 1, lcd->width - 1, top, red);
    pause_ms(2500);
    dgx_scroll(lcd, 0, top, lcd->width, height, 0, height, paper);
}

static void timings(void)
{
    dgx_ili9341_orientation(lcd, DgxScreenRightLeft, DgxScreenTopBottom, true);
    dgx_fill_rectangle(lcd, 0, 0, lcd->width, lcd->height, paper);
    TEXT(lcd, 4, 12, red, "#%d  замеры, смотри лог", BUILD);
    printf("T build %d, free heap %u, largest block %u\n", BUILD, (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    /* straight on the display */
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
        /* the same picture across the place where the rows of a scrolled area turn round: every row of it in two pieces */
        dgx_scroll(lcd, 0, 0, 320, 240, -100, 0, paper);
        TIME("the same, the panel scrolled by 100 columns", 10, dgx_vscreen_to_screen(lcd, 0, 60, band));
        TIME("display: dgx_scroll left by 3, the panel", 50, dgx_scroll(lcd, 0, 0, 320, 240, -3, 0, paper));
        dgx_scroll(lcd, 0, 0, 320, 240, 320, 0, paper);
        dgx_screen_destroy(&band);
    }
    dgx_screen_t *twelve = dgx_vscreen_init(320, 112, 12, DgxScreenRGB);
    if (twelve) {
        dgx_screen_progress_up(twelve);
        TIME("12 bits 320x112: dgx_scroll up by 14, pixel by pixel", 5, dgx_scroll(twelve, 0, 0, 320, 112, 0, -14, 0));
        dgx_screen_destroy(&twelve);
    }
    /* upright: the long side along y, a terminal line */
    dgx_ili9341_orientation(lcd, DgxScreenLeftRight, DgxScreenTopBottom, false);
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
    printf("T done\n");
}

void app_main(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_2, 40 * 1000 * 1000, 0);
    if (!bus) return;
    lcd = dgx_ili9341_init(bus, GPIO_NUM_NC, GPIO_NUM_21, 16, DgxScreenRGB);
    if (!lcd) return;
    paper  = dgx_rgb_to_16(250, 246, 232);
    ink    = dgx_rgb_to_16(24, 36, 120);
    red    = dgx_rgb_to_16(200, 40, 30);
    green  = dgx_rgb_to_16(30, 150, 60);
    blue   = dgx_rgb_to_16(40, 90, 220);
    yellow = dgx_rgb_to_16(240, 200, 40);
    grey   = dgx_rgb_to_16(170, 170, 170);
    white  = dgx_rgb_to_16(255, 255, 255);
    for (int i = 0; i < 256; ++i) {
        lut[i] = (uint16_t)dgx_rgb_to_16((uint8_t)(250 - (250 - 24) * i / 255), (uint8_t)(246 - (246 - 36) * i / 255),
                                        (uint8_t)(232 - (232 - 120) * i / 255));
    }
    printf("build %d, readable %d\n", BUILD, dgx_screen_is_readable(lcd));
    for (int round = 0;; ++round) {
        timings();
        dgx_ili9341_orientation(lcd, DgxScreenRightLeft, DgxScreenTopBottom, true);
        page_figures();
        page_soft();
        page_memory();
        page_chart('D', DgxScreenRightLeft, DgxScreenTopBottom);
        page_chart('E', DgxScreenLeftRight, DgxScreenBottomTop);
        page_terminal('F', DgxScreenLeftRight, DgxScreenTopBottom);
        page_terminal('G', DgxScreenRightLeft, DgxScreenBottomTop);
        printf("round %d done\n", round);
    }
}
