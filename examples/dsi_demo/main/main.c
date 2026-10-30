#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "bus/dgx_dsi_esp32.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_screen.h"
#include "drivers/jd9165.h"
#include "drivers/jd9365.h"
#include "drivers/vscreen.h"
#include "fonts/LiberationSansRegular12.h"
#include "fonts/DejaVuSansBook32.h"

static const char *TAG = "dgx_dsi_demo";

static dgx_bus_protocols_t *demo_bus;

/* a number of its own for every build that is looked at on a display */
#define BUILD 335

#ifdef CONFIG_EXAMPLE_BOARD_JC8012P4A1C
/* Guition JC8012P4A1C: a 10.1-inch 800x1280 display with a JD9365 on two DSI lanes */
enum
{
    EXAMPLE_LCD_DSI_LANES     = 2,
    EXAMPLE_LCD_DSI_LANE_MBPS = 1000,
    EXAMPLE_LCD_PHY_LDO_CHAN  = 3,    /* the regulator of the chip that powers the DSI PHY */
    EXAMPLE_LCD_PHY_LDO_MV    = 2500,
    EXAMPLE_LCD_PIN_NUM_RST   = 27,
    EXAMPLE_LCD_PIN_NUM_BL    = 23,
    EXAMPLE_LCD_BL_ON_LEVEL   = 1,
    EXAMPLE_LCD_PANEL_WIDTH   = 800, /* of the panel, before any orientation */
    EXAMPLE_LCD_PANEL_HEIGHT  = 1280,
};
#define example_lcd_init        dgx_jd9365_init
#define example_lcd_orientation dgx_jd9365_orientation
#else
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
    EXAMPLE_LCD_PANEL_WIDTH   = 1024, /* of the panel, before any orientation */
    EXAMPLE_LCD_PANEL_HEIGHT  = 600,
};
#define example_lcd_init        dgx_jd9165_init
#define example_lcd_orientation dgx_jd9165_orientation
#endif

/* How the screen lies on the panel. If "top left" is not at the top left, change these. */
#define EXAMPLE_LCD_DIR_X   DgxScreenLeftRight
#define EXAMPLE_LCD_DIR_Y   DgxScreenTopBottom
#define EXAMPLE_LCD_SWAP_XY false

typedef void (*demo_step_fn_t)(dgx_screen_t *screen);

#define EXAMPLE_LCD_COLOR_BITS CONFIG_EXAMPLE_LCD_COLOR_BITS

/*
 * What the display may spend to show a picture whole: nothing, or one more
 * frame buffer in PSRAM. The menu may ask for two; otherwise the numbers
 * measured on the board decide, see demo_tune().
 */
#ifdef CONFIG_EXAMPLE_LCD_TWO_FRAMES
#define EXAMPLE_LCD_FRAMES DgxDsiTwoFrames
#else
#define EXAMPLE_LCD_FRAMES DgxDsiOneFrame
#endif

/* the numbers of the board, where they came from and what they say; shown by demo_tuning_card() */
typedef struct {
    dgx_dsi_tuning_t built_in;    /* the numbers the bus starts with */
    dgx_dsi_tuning_t tuning;      /* the numbers it works with now */
    bool             from_nvs;    /* read at this start, measured at an earlier one */
    bool             measured;    /* measured at this start */
    bool             kept;        /* and written to NVS */
    uint32_t         bench_ms;    /* how long dgx_dsi_bench() took */
    uint8_t          advice;      /* dgx_dsi_advise() of the numbers in use */
    dgx_dsi_frames_t frames;      /* what the display was made with */
    bool             by_advice;   /* and that was chosen by the advice, not by the menu */
    bool             restart;     /* the advice asks for two frame buffers and the display has one: the next start makes it so */
    uint32_t         piece_built_in_us, piece_measured_us; /* a 60x100 piece of a virtual screen to the display, with either set of numbers */
} demo_tuning_t;

static demo_tuning_t demo_numbers;

static inline uint32_t demo_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return EXAMPLE_LCD_COLOR_BITS == 24 ? dgx_rgb_to_24(r, g, b) : dgx_rgb_to_16(r, g, b);
}

static void demo_build_label(dgx_screen_t *screen);

static void demo_text(dgx_screen_t *screen, int x, int y, const char *text, uint32_t color)
{
    dgx_font_string_utf8_screen(screen, x, y, text, color, DgxOutputNormal, 1, DejaVuSansBook32(), NULL, NULL);
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
    snprintf(size, sizeof(size), "%s, %d-bit color, build %d", screen->screen_name, EXAMPLE_LCD_COLOR_BITS, BUILD);
    dgx_font_string_utf8_screen(screen, bar, top + 170, size, demo_rgb(60, 190, 255), DgxOutputNormal, 1, LiberationSansRegular12(), NULL, NULL);
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
        for (int y = ramp * band; y < (ramp + 1) * band; ++y) dgx_vscreen_to_screen(screen, 0, y, row, 0);
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
        dgx_vscreen_to_screen(screen, 0, top + y, row, 0);
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
/*
 * How a program keeps the pace of the display. A display does not hold a
 * program back, so one that would draw more often than frames are shown
 * holds itself. Sleeping for the time of a frame is not enough: where in a
 * frame the picture is finished then is a matter of chance, and one that is
 * finished near the end of a frame gets now into this frame, now into the
 * next, and moves unevenly.
 *
 * So a picture is aimed: it is to be finished a little before a frame ends.
 * The bus tells how long a frame takes and when the last one ended, the
 * program knows how long its last pictures took, and it starts the next one
 * that much, and 4 ms of room, before the end of the frame it wants to be
 * in. The bus does not wait for anything: the program sleeps as it likes.
 * With one frame buffer the bus does not count frames, and the pace is kept
 * by the clock alone.
 */
typedef struct {
    int64_t  aim;     /* the end of a frame the last picture was to be in time for */
    uint32_t work_us; /* the longest of the last pictures, forgotten by an eighth a picture */
} demo_pace_t;

#define DEMO_PACE_ROOM_US 4000

/* sleep until the next picture is to be started; `began_us` is when the last one was */
static void demo_pace(demo_pace_t *pace, int64_t began_us)
{
    const int64_t  now = esp_timer_get_time(), period = dgx_dsi_frame_us(demo_bus);
    const uint32_t took = (uint32_t)(now - began_us), less = pace->work_us - (pace->work_us >> 3);
    int64_t        ended, from;
    pace->work_us = took > less ? took : less;
    if (dgx_dsi_frame_ended(demo_bus, &ended)) {
        /* the frame after the one aimed at last, on the ends of frames as they really are */
        int64_t aim = pace->aim ? pace->aim + period : ended + period;
        aim         = ended + (aim - ended + period / 2) / period * period;
        from        = aim - DEMO_PACE_ROOM_US - pace->work_us;
        /* too late for that frame: the one after it */
        while (from < now) aim += period, from += period;
        pace->aim = aim;
    } else {
        from = began_us + period;
    }
    while (esp_timer_get_time() < from) vTaskDelay(1);
}

static void demo_moving_bars(dgx_screen_t *screen, uint32_t run_ms)
{
    const int     top = screen->height / 3, height = screen->height / 3, pitch = 128, width = 40, step = 4;
    dgx_screen_t *band = dgx_vscreen_init(screen->width, height, screen->color_bits, DgxScreenRGB);
    if (!band) return;
    dgx_screen_progress_up(screen);
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, DGX_BLACK(demo_rgb));
    demo_text(screen, 16, 44, demo_numbers.frames == DgxDsiTwoFrames ? "two frame buffers" : "one frame buffer", DGX_WHITE(demo_rgb));
    demo_build_label(screen);
    dgx_screen_progress_down(screen);
    int64_t     start = esp_timer_get_time();
    int         frames = 0, offset = 0;
    demo_pace_t pace = {0};
    while (esp_timer_get_time() - start < (int64_t)run_ms * 1000) {
        const int64_t began = esp_timer_get_time();
        for (int x = offset - pitch; x < band->width; x += pitch) dgx_fill_rectangle(band, x, 0, width, height, DGX_BLACK(demo_rgb));
        offset = (offset + step) % pitch;
        for (int x = offset - pitch; x < band->width; x += pitch) dgx_fill_rectangle(band, x, 0, width, height, DGX_CYAN(demo_rgb));
        dgx_vscreen_to_screen(screen, 0, top, band, 0);
        ++frames;
        demo_pace(&pace, began);
    }
    ESP_LOGI(TAG, "moving bars: %d frames a second, a frame of the display takes %u us", (int)(frames * 1000LL / run_ms),
             (unsigned)dgx_dsi_frame_us(demo_bus));
    dgx_screen_destroy(&band);
}

/* a number of the tuning, or a dash where the bus does not have it */
static const char *demo_number(char *buffer, size_t size, uint32_t value, uint32_t none)
{
    if (value == none) return "-";
    snprintf(buffer, size, "%u", (unsigned)value);
    return buffer;
}

/*
 * The numbers of the board on the screen: those the bus is built with
 * beside those measured, where they came from, what they say and what was
 * done about it.
 */
static void demo_tuning_card(dgx_screen_t *screen)
{
    const demo_tuning_t    *n = &demo_numbers;
    const dgx_dsi_tuning_t *a = &n->built_in, *b = &n->tuning;
    const uint32_t          white = DGX_WHITE(demo_rgb), yellow = demo_rgb(255, 200, 40), cyan = demo_rgb(60, 190, 255), grey = demo_rgb(150, 160, 180);
    /* 16 lines of DejaVu Sans 32 at most, above the build label: 34 pixels a line fit the 600 rows of the 7-inch panel */
    const int               left = 16, col_a = screen->width * 58 / 100, col_b = screen->width * 79 / 100, step = 34;
    char                    line[80], va[16], vb[16];
    int                     y = 36;

    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, demo_rgb(6, 10, 20));
    demo_text(screen, left, y, "dgx_dsi_bench(): numbers of this board", yellow);
    y += step;
    if (n->from_nvs) snprintf(line, sizeof(line), "from NVS, measured at an earlier start");
    else if (n->measured) snprintf(line, sizeof(line), "measured now in %u ms, %s", (unsigned)n->bench_ms, n->kept ? "kept in NVS" : "NVS failed");
    else snprintf(line, sizeof(line), "not measured: the built-in numbers");
    demo_text(screen, left, y, line, grey);
    y += step;
    demo_text(screen, col_a, y, "built-in", grey);
    demo_text(screen, col_b, y, n->measured || n->from_nvs ? "measured" : "in use", grey);
    y += step;

    struct {
        const char *name;
        uint32_t    a, b, none;
    } rows[] = {
        {"window, KB/ms", a->window_kb_a_ms, b->window_kb_a_ms, 0},
        {"out of cache, KB/ms", a->out_kb_a_ms, b->out_kb_a_ms, 0},
        {"2D DMA, KB/ms", a->dma_kb_a_ms, b->dma_kb_a_ms, 0},
        {"rows by DMA from, B", a->rows_from, b->rows_from, UINT32_MAX},
        {"copy by DMA from", a->copy_from, b->copy_from, UINT32_MAX},
        {"fill by PPA from", a->fill_from, b->fill_from, 65535},
        {"lanes taken, %", a->lanes_percent, b->lanes_percent, 0},
        {"FIFO drops a frame", a->fifo_drops, b->fifo_drops, DGX_DSI_NOT_MEASURED},
        {"PSRAM, MHz", a->psram_mhz, b->psram_mhz, 0},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        demo_text(screen, left, y, rows[i].name, white);
        demo_text(screen, col_a, y, demo_number(va, sizeof(va), rows[i].a, rows[i].none), grey);
        demo_text(screen, col_b, y, demo_number(vb, sizeof(vb), rows[i].b, rows[i].none), cyan);
        y += step;
    }
    y += step / 2;
    snprintf(line, sizeof(line), "advice: %s", n->advice & DGX_DSI_ADVISE_TWO_FRAMES ? "two frame buffers" : "margin enough");
    demo_text(screen, left, y, line, yellow);
    y += step;
    snprintf(line, sizeof(line), "display: %s, by the %s", n->frames == DgxDsiTwoFrames ? "two frame buffers" : "one frame buffer", n->by_advice ? "advice" : "menu");
    demo_text(screen, left, y, line, white);
    y += step;
    if (n->piece_built_in_us) {
        snprintf(line, sizeof(line), "60x100 piece: %u us, then %u us", (unsigned)n->piece_built_in_us, (unsigned)n->piece_measured_us);
        demo_text(screen, left, y, line, white);
        y += step;
    }
    if (n->restart) demo_text(screen, left, y, "restarting with two frame buffers...", yellow);
}

/* the number of the build, large, in the lower left corner of every scene */
static void demo_build_label(dgx_screen_t *screen)
{
    char label[16];
    snprintf(label, sizeof(label), "b%d", BUILD);
    dgx_fill_rectangle(screen, 0, screen->height - 48, 120, 48, DGX_BLACK(demo_rgb));
    demo_text(screen, 12, screen->height - 12, label, demo_rgb(255, 200, 40));
}

static void demo_run_step(dgx_screen_t *screen, const char *name, demo_step_fn_t step, uint32_t hold_ms)
{
    int64_t start_us = esp_timer_get_time();
    /* one batch: what a scene has drawn is brought out to the display once, at its end */
    dgx_screen_progress_up(screen);
    step(screen);
    demo_build_label(screen);
    dgx_screen_progress_down(screen);
    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG, "%s rendered in %" PRId64 " ms", name, elapsed_ms);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
}

/*
 * Tuning the bus to the board.
 *
 * When a rectangle goes by the 2D DMA or the accelerator of the chip and
 * when by the processor, and how much room the bus leaves before the scan
 * of the display, is decided by numbers that depend on the chip, its
 * memory and the display. The bus starts with those of the board DGX was
 * written on. dgx_dsi_bench() measures them on the board at hand, in
 * memory of its own, so nothing shows on the screen. It takes about 0.3 s,
 * so it is done once and the numbers are kept in NVS, namespace "dgx", key
 * "dsi_tuning": at the next start the bus gets them from there,
 * dgx_dsi_tune(). Numbers kept for another panel, another color depth or
 * another PSRAM clock are not taken, and the board is measured again.
 *
 * The numbers also say how to use the display, dgx_dsi_advise(): whether one
 * frame buffer will run the display short of data under heavy drawing. The
 * advice needs no display, so at a start that has the numbers in NVS the
 * display is made by it: two frame buffers when it says so. At the first
 * start there are no numbers yet; the display is made as the menu says,
 * measured, and when the advice asks for two frame buffers it does not have,
 * the example restarts once, with the numbers in NVS.
 */

/* the numbers of an earlier start, when NVS has them and they are for this panel and this build */
static bool demo_tuning_kept(dgx_dsi_tuning_t *tuning)
{
    nvs_handle_t nvs;
    size_t       size = sizeof(*tuning);
    esp_err_t    err  = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK || nvs_open("dgx", NVS_READONLY, &nvs) != ESP_OK) return false;
    const bool found = nvs_get_blob(nvs, "dsi_tuning", tuning, &size) == ESP_OK && size == sizeof(*tuning);
    nvs_close(nvs);
    /* what dgx_dsi_tune() will check as well, but before the display is made: the panel, the color depth, the PSRAM clock */
    return found && tuning->version == DGX_DSI_TUNING_VERSION && tuning->size == sizeof(*tuning) && tuning->width == EXAMPLE_LCD_PANEL_WIDTH &&
           tuning->height == EXAMPLE_LCD_PANEL_HEIGHT && tuning->pixel_bytes == EXAMPLE_LCD_COLOR_BITS / 8
#ifdef CONFIG_SPIRAM_SPEED
           && tuning->psram_mhz == CONFIG_SPIRAM_SPEED
#endif
        ;
}

static bool demo_tuning_keep(const dgx_dsi_tuning_t *tuning)
{
    nvs_handle_t nvs;
    if (nvs_open("dgx", NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool kept = nvs_set_blob(nvs, "dsi_tuning", tuning, sizeof(*tuning)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return kept;
}

/*
 * What the numbers change: a 60x100 piece of a virtual screen sent to the
 * display 50 times in one batch, microseconds a piece. At 16 bits the piece
 * is 12 KB: with the built-in numbers it goes through the window, with the
 * measured ones by the 2D DMA.
 */
static uint32_t demo_time_piece(dgx_screen_t *screen)
{
    dgx_screen_t *piece = dgx_vscreen_init(60, 100, screen->color_bits, DgxScreenRGB);
    if (!piece) return 0;
    dgx_fill_rectangle(piece, 0, 0, piece->width, piece->height, demo_rgb(40, 40, 60));
    dgx_screen_progress_up(screen);
    dgx_vscreen_to_screen(screen, 0, 0, piece, 0); /* the first one may wait for the display */
    const int64_t from = esp_timer_get_time();
    for (int i = 0; i < 50; ++i) dgx_vscreen_to_screen(screen, 0, 0, piece, 0);
    const uint32_t us = (uint32_t)((esp_timer_get_time() - from) / 50);
    dgx_screen_progress_down(screen);
    dgx_screen_destroy(&piece);
    return us;
}

/* after the display is made: the numbers from NVS, or the board measured; kept is what demo_tuning_kept() found */
static void demo_tune(dgx_bus_protocols_t *bus, dgx_screen_t *screen, const dgx_dsi_tuning_t *kept)
{
    demo_tuning_t *n = &demo_numbers;
    dgx_dsi_tuning(bus, &n->built_in);
    n->tuning = n->built_in;
    if (kept && dgx_dsi_tune(bus, kept)) {
        n->tuning   = *kept;
        n->from_nvs = true;
        ESP_LOGI(TAG, "the bus has the numbers kept in NVS");
    } else {
        const int64_t from = esp_timer_get_time();
        if (dgx_dsi_bench(bus, &n->tuning)) {
            n->bench_ms = (uint32_t)((esp_timer_get_time() - from) / 1000);
            n->measured = true;
            /* the same piece with the built-in numbers, then with the measured ones */
            n->piece_built_in_us = demo_time_piece(screen);
            dgx_dsi_tune(bus, &n->tuning);
            n->piece_measured_us = demo_time_piece(screen);
            n->kept = demo_tuning_keep(&n->tuning);
            ESP_LOGI(TAG, "the board was measured in %u ms, the numbers are %s; a 60x100 piece of a virtual screen took %u us, now %u us", (unsigned)n->bench_ms,
                     n->kept ? "kept in NVS" : "NOT kept, NVS failed", (unsigned)n->piece_built_in_us, (unsigned)n->piece_measured_us);
        } else {
            n->tuning = n->built_in;
            ESP_LOGW(TAG, "the board could not be measured: the bus keeps the numbers it has");
        }
    }
    const dgx_dsi_tuning_t *t = &n->tuning;
    ESP_LOGI(TAG, "the bus puts %u KB/ms through a window, the cache brings out %u KB/ms, the 2D DMA copies %u KB/ms; rows by the 2D DMA from %u bytes",
             (unsigned)t->window_kb_a_ms, (unsigned)t->out_kb_a_ms, (unsigned)t->dma_kb_a_ms, (unsigned)t->rows_from);
    /* what the numbers say about this board: the one thing the program acts on, and the whole story in words for the developer */
    n->advice = dgx_dsi_advise(t);
    char text[400];
    dgx_dsi_tuning_text(t, text, sizeof(text));
    ESP_LOGI(TAG, "%s", text);
    /* measured only now, and the advice asks for two frame buffers the display was not given: the next start reads the numbers and makes it so */
    n->restart = n->measured && n->kept && n->frames == DgxDsiOneFrame && (n->advice & DGX_DSI_ADVISE_TWO_FRAMES);
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

    /* the numbers of an earlier start, if there are any, say how to make the display; the menu may ask for two frame buffers anyway */
    dgx_dsi_tuning_t kept;
    const bool       have_kept = demo_tuning_kept(&kept);
    demo_numbers.frames        = EXAMPLE_LCD_FRAMES;
    if (have_kept && demo_numbers.frames == DgxDsiOneFrame && (dgx_dsi_advise(&kept) & DGX_DSI_ADVISE_TWO_FRAMES)) {
        demo_numbers.frames    = DgxDsiTwoFrames;
        demo_numbers.by_advice = true;
        ESP_LOGI(TAG, "the numbers kept in NVS ask for two frame buffers: the display gets them");
    }

    dgx_screen_t *screen = example_lcd_init(bus, (gpio_num_t)EXAMPLE_LCD_PIN_NUM_RST, EXAMPLE_LCD_COLOR_BITS, DgxScreenRGB, demo_numbers.frames);
    if (!screen) {
        ESP_LOGE(TAG, "the display could not be set up");
        return;
    }
    example_lcd_orientation(screen, EXAMPLE_LCD_DIR_X, EXAMPLE_LCD_DIR_Y, EXAMPLE_LCD_SWAP_XY);
    ESP_LOGI(TAG, "build %d: %s, %dx%d, %s", BUILD, screen->screen_name, screen->width, screen->height,
             demo_numbers.frames == DgxDsiTwoFrames ? "two frame buffers" : "one frame buffer");
    demo_bus = bus;
    demo_tune(bus, screen, have_kept ? &kept : NULL);
    if (demo_numbers.restart) {
        ESP_LOGW(TAG, "the advice asks for two frame buffers: restarting to make the display with them");
        demo_run_step(screen, "tuning", demo_tuning_card, 6000);
        esp_restart();
    }

    while (1) {
        demo_run_step(screen, "test card", demo_test_card, 5000);
        demo_run_step(screen, "tuning", demo_tuning_card, 8000);
        demo_run_step(screen, "ramps", demo_ramps, 6000);
        demo_run_step(screen, "lines", demo_lines, 2500);
        demo_run_step(screen, "circles", demo_circles, 2500);
        demo_run_step(screen, "triangles", demo_triangles, 2500);
        demo_run_step(screen, "color bars", demo_color_bars, 2500);
        demo_moving_bars(screen, 6000);
    }
}
