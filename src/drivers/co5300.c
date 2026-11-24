/*
 * co5300.c
 *
 *  The CO5300, the controller of round AMOLED panels on QSPI. It does not
 *  draw every window right. In a window whose first and last columns are
 *  both odd the rows slip; a window one column wide draws nothing, at an even
 *  column too, and so does a window one row high (checked on the Waveshare
 *  ESP32-S3-Touch-AMOLED-1.32). The vendor's examples send every window from
 *  an even column and row to an odd one, and so does this driver: the screen
 *  keeps its picture in a 16-bit virtual screen, as the monochrome drivers
 *  do, and sends what changed widened to whole pairs of columns and rows, the
 *  pixels added taken from the picture. The panel turns the picture itself,
 *  by MADCTL: the screen in memory lies as the program draws it.
 *
 *  The init sequence is that of Waveshare's ESP-IDF code for its CO5300
 *  boards: the user command page, the SPI mode, the pixel format, the tearing
 *  effect line on, the brightness control on and the brightness at its top,
 *  sleep out, and the display on once the memory is cleared. The 480x480
 *  glass of the 2.16" board takes three registers of command page 20h first,
 *  as Waveshare's code for it does.
 *
 *  The memory has 480 columns whatever the glass: MX counts them from the
 *  other end of the 480, so a glass at column 6 lies at column 480 - 6 - width
 *  with MX the other way (measured on the 466x466 glass, and what Arduino_GFX
 *  has for the 280x456 one: 20 and 180). The rows are those of the glass. The
 *  offsets of each glass and its upright MADCTL are those of the vendors'
 *  code; only the 466x466 has been checked on a module.
 */

#include "drivers/co5300.h"

#include <stdlib.h>
#include <string.h>

#include "dgx_vscreen.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "dgx_screen_with_bus.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "../dgx_clip_priv.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define CO5300_SLPOUT 0x11 // Sleep out
#define CO5300_DISPON 0x29 // Display on
#define CO5300_TEON   0x35 // Tearing effect line on
#define CO5300_MADCTL 0x36 // Memory data access control
#define CO5300_COLMOD 0x3A // Interface pixel format
#define CO5300_WRDISBV 0x51 // Write display brightness
#define CO5300_WRCTRLD 0x53 // Write CTRL display
#define CO5300_WRHBMDISBV 0x63 // Write display brightness in high brightness mode
#define CO5300_SPIMODE 0xC4 // SPI mode control
#define CO5300_CMDMODE 0xFE // Command page
#define CO5300_CASET  0x2A // Column address set
#define CO5300_RASET  0x2B // Row address set
#define CO5300_RAMWR  0x2C // Memory write

#define CO5300_MY  0x80
#define CO5300_MX  0x40
#define CO5300_MV  0x20
#define CO5300_BGR 0x08

/* the buffers rows are copied into for sending in the order of the scan, three of them in turn */
#define CO5300_ROWS_BUFFER 8192u

static const char TAG[] = "DGX CO5300";

static const dgx_lcd_init_cmd_t co5300_init_cmds[] = {
    {CO5300_CMDMODE,    0,                    1,    0,   {0x00}}, // user command page
    {CO5300_SPIMODE,    0,                    1,    0,   {0x80}}, // as the vendor sets it
    {CO5300_COLMOD,     dgx_panel_adj_colmod, 1,    0,   {0x55}}, // 16 bits a pixel
    {CO5300_TEON,       0,                    1,    0,   {0x00}}, //
    {CO5300_WRCTRLD,    0,                    1,    0,   {0x20}}, // brightness control on
    {CO5300_WRDISBV,    0,                    1,    0,   {0xFF}}, //
    {CO5300_WRHBMDISBV, 0,                    1,    0,   {0xFF}}, //
    {CO5300_MADCTL,     dgx_panel_adj_madctl, 1,    0,   {0}   }, // upright, in the color order of the screen
    {CO5300_SLPOUT,     0,                    0,    120, {0}   }, //
    {0,                 0,                    0xff, 0,   {0}   }  // Stop
};

/* the 480x480 glass: three registers of command page 20h, then the same */
static const dgx_lcd_init_cmd_t co5300_init_480_cmds[] = {
    {CO5300_CMDMODE,    0,                    1,    0,   {0x20}}, // command page 20h
    {0x19,              0,                    1,    0,   {0x10}}, // as Waveshare sets them for the 2.16" board
    {0x1C,              0,                    1,    0,   {0xA0}}, //
    {CO5300_CMDMODE,    0,                    1,    0,   {0x00}}, // user command page
    {CO5300_SPIMODE,    0,                    1,    0,   {0x80}}, //
    {CO5300_COLMOD,     dgx_panel_adj_colmod, 1,    0,   {0x55}}, //
    {CO5300_TEON,       0,                    1,    0,   {0x00}}, //
    {CO5300_WRCTRLD,    0,                    1,    0,   {0x20}}, //
    {CO5300_WRDISBV,    0,                    1,    0,   {0xFF}}, //
    {CO5300_WRHBMDISBV, 0,                    1,    0,   {0xFF}}, //
    {CO5300_MADCTL,     dgx_panel_adj_madctl, 1,    0,   {0}   }, //
    {CO5300_SLPOUT,     0,                    0,    120, {0}   }, //
    {0,                 0,                    0xff, 0,   {0}   }  // Stop
};

/* the display is turned on once its memory is cleared, so whatever was left in it is never shown */
static const dgx_lcd_init_cmd_t co5300_on_cmds[] = {
    {CO5300_DISPON, 0, 0,    20, {0}}, //
    {0,             0, 0xff, 0,  {0}}  // Stop
};

/* the glasses, by dgx_co5300_resolution_t from 1 on */
#define CO5300_PANEL(w, h, x, mad, table)                                                                                                    \
    {.name = "CO5300", .width = (w), .height = (h), .memory_width = 480, .x_offset = (x), .colmod = {0, 0x55}, .madctl = (mad),                \
     .reset_low_ms = 10, .reset_after_ms = 120, .flags = DGX_PANEL_CLEAR_FIRST, .init = (table), .on = co5300_on_cmds}
static const dgx_panel_t co5300_panels[] = {
    CO5300_PANEL(466, 466, 6, 0xC0, co5300_init_cmds),      /* Waveshare 1.32", upright as its examples turn it */
    CO5300_PANEL(480, 480, 0, 0xA0, co5300_init_480_cmds),  /* Waveshare 2.16" */
    CO5300_PANEL(368, 448, 16, 0x00, co5300_init_cmds),     /* Waveshare 1.8" of the second revision */
    CO5300_PANEL(280, 456, 20, 0x00, co5300_init_cmds),     /* Waveshare and LilyGO 1.64" */
};

typedef struct {
    dgx_vscreen_t        base;  /* the picture, as the program draws it */
    dgx_screen_t        *panel; /* the screen of the panel it is sent to, by windows of whole pairs of columns and rows */
    const dgx_panel_t   *desc;  /* the glass */
    dgx_bus_protocols_t *bus;
    void (*destroy_vscreen)(struct _dgx_screen_t **pscr);
    dgx_scr_fill_rectangle_func fill_vscreen; /* the fill of the picture alone */
    /* tear_free: what is sent goes in the order of the scan of the panel, from a time the scan allows */
    bool             tear_free;
    gpio_num_t       te;
    int              scan_rows;   /* the rows of memory the panel scans, from address 0 up when MY is off */
    bool             across;      /* the axes are swapped: the rows of the screen lie across the scan, no order helps */
    bool             reversed;    /* the rows of the screen run against the scan: MY is off, and they go from the last */
    uint8_t         *rows[3];     /* rows copied for sending; the bus holds two transfers at most, so the third is free */
    uint32_t         ns_per_byte; /* what the wire takes, measured on what was sent */
    volatile int64_t te_end_us;   /* the last end of a TE pulse: the panel starts a scan */
    volatile int32_t te_period_us;
} dgx_co5300_t;

/*
 * The TE line: the end of each of its pulses with its time, from an
 * interrupt, and waits until a time. On the computer there is no line: the
 * order of the rows is kept, nothing waits.
 */
#ifdef ESP_PLATFORM
static void IRAM_ATTR dgx_co5300_te_isr(void *arg)
{
    dgx_co5300_t *scr    = (dgx_co5300_t *)arg;
    const int64_t now    = esp_timer_get_time();
    const int64_t period = now - scr->te_end_us;
    if (period > 5000 && period < 50000) scr->te_period_us = (int32_t)period;
    scr->te_end_us = now;
}

static int64_t dgx_co5300_now(void)
{
    return esp_timer_get_time();
}

static bool dgx_co5300_te_start(dgx_co5300_t *scr)
{
    /* the service of GPIO interrupts is installed once: IDF logs an error at a second install, which serves the same */
    static bool         service_up;
    const gpio_config_t io = {.pin_bit_mask = 1ull << scr->te, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_NEGEDGE};
    if (gpio_config(&io) != ESP_OK) return false;
    if (!service_up) {
        const esp_err_t service = gpio_install_isr_service(0);
        if (service != ESP_OK && service != ESP_ERR_INVALID_STATE) return false; /* one the program has installed serves too */
        service_up = true;
    }
    return gpio_isr_handler_add(scr->te, dgx_co5300_te_isr, scr) == ESP_OK;
}

static void dgx_co5300_te_stop(dgx_co5300_t *scr)
{
    gpio_isr_handler_remove(scr->te);
}

static void dgx_co5300_wait_until(int64_t at_us)
{
    for (int64_t now = esp_timer_get_time(); at_us - now > (int64_t)portTICK_PERIOD_MS * 1000 + 1000; now = esp_timer_get_time()) vTaskDelay(1);
    while (esp_timer_get_time() < at_us) {
    }
}
#else
static int64_t dgx_co5300_now(void)
{
    return 0;
}

static bool dgx_co5300_te_start(dgx_co5300_t *scr)
{
    (void)scr;
    return true;
}

static void dgx_co5300_te_stop(dgx_co5300_t *scr)
{
    (void)scr;
}

static void dgx_co5300_wait_until(int64_t at_us)
{
    (void)at_us;
}
#endif

/* the MADCTL of the screen as it lies, as dgx_panel_init() sets it */
static uint8_t dgx_co5300_madctl(const dgx_co5300_t *scr)
{
    const dgx_screen_t *panel  = scr->panel;
    uint8_t             m      = scr->desc->madctl;
    const bool          across = (m & CO5300_MV) != 0;
    if (panel->dir_x == DgxScreenRightLeft) m ^= across ? CO5300_MY : CO5300_MX;
    if (panel->dir_y == DgxScreenBottomTop) m ^= across ? CO5300_MX : CO5300_MY;
    if (panel->swap_xy) m ^= CO5300_MV;
    if (panel->rgb_order == DgxScreenBGR) m ^= CO5300_BGR;
    return m;
}

/*
 * The panel scans the rows of its memory from address 0 up, from the end of
 * a TE pulse; the 466x466 glass of the Waveshare board from its bottom edge
 * up. With MY on the rows of the screen run against the scan: MY goes off,
 * and the rows are sent from the last, to their addresses counted from the
 * other end, so the picture stays as it is. With the axes swapped a row of
 * the screen lies across the scan, and no order of the rows keeps the write
 * behind it: the screen is sent as without the line.
 */
static void dgx_co5300_scan_order(dgx_co5300_t *scr)
{
    if (!scr->tear_free) return;
    const uint8_t m = dgx_co5300_madctl(scr);
    scr->across     = (m & CO5300_MV) != 0;
    scr->reversed   = !scr->across && (m & CO5300_MY);
    if (scr->reversed) {
        const uint8_t v = (uint8_t)(m & ~CO5300_MY);
        scr->bus->write_command(scr->bus, CO5300_MADCTL);
        scr->bus->write_data(scr->bus, &v, 8);
    }
}

/*
 * When rows g0..g1 of the memory, in the order of the scan, can be written
 * in duration_us without meeting the scan: all the time behind the scan that
 * passes them, and each row written before the next scan comes to it. In rows
 * of the scan s from the end of the last pulse, the write starts at s0 with
 * s0 >= g0 and s0 + d >= g1, and s0 < g0 + H and s0 + d < g1 + H, d the rows
 * the scan passes while the write lasts; both are straight lines, so their
 * ends are enough. A margin of 8 rows, 0.3 ms, on each side. Without pulses,
 * or for a write too long for any time, it starts now.
 */
static int64_t dgx_co5300_start_time(const dgx_co5300_t *scr, int g0, int g1, int64_t duration_us)
{
    const int64_t te = scr->te_end_us, now = dgx_co5300_now();
    const int32_t period = scr->te_period_us;
    if (!te || period <= 0 || now - te > 4 * (int64_t)period) return now;
    const int64_t H = scr->scan_rows, margin = 8;
    /* in rows times the period, to stay in integers */
    const int64_t d     = duration_us * H;
    const int64_t s_min = (g0 * (int64_t)period > g1 * (int64_t)period - d ? g0 * (int64_t)period : g1 * (int64_t)period - d) + margin * period;
    const int64_t s_max = ((g0 + H) * (int64_t)period < (g1 + H) * (int64_t)period - d ? (g0 + H) * (int64_t)period : (g1 + H) * (int64_t)period - d) -
                          margin * period;
    if (s_max < s_min) return now;
    /* rows times the period over H is microseconds */
    const int64_t u_min = s_min / H, u_max = s_max / H;
    for (int k = 0; k < 4; ++k) {
        const int64_t a = te + k * (int64_t)period + u_min, b = te + k * (int64_t)period + u_max;
        if (now <= b) return now > a ? now : a;
    }
    return now;
}

/*
 * The window of rows top..bottom, columns left..right of the screen in the
 * order of the scan, sent when the scan allows; the pixels follow by
 * write_data_async(). Returns the time it was sent at.
 */
static int64_t dgx_co5300_scan_window(dgx_co5300_t *scr, int left, int right, int top, int bottom)
{
    dgx_bus_protocols_t *bus = scr->bus;
    const dgx_screen_t  *panel = scr->panel;
    const int            c0 = left + panel->cg_col_shift, c1 = right + panel->cg_col_shift;
    int                  a0 = top + panel->cg_row_shift, a1 = bottom + panel->cg_row_shift;
    if (scr->reversed) {
        const int t = a0;
        a0          = scr->scan_rows - 1 - a1;
        a1          = scr->scan_rows - 1 - t;
    }
    const uint32_t bytes = (uint32_t)(right - left + 1) * (uint32_t)(bottom - top + 1) * 2u;
    dgx_co5300_wait_until(dgx_co5300_start_time(scr, a0, a1, (int64_t)bytes * scr->ns_per_byte / 1000));
    const int64_t at      = dgx_co5300_now();
    const uint8_t cols[4] = {(uint8_t)(c0 >> 8), (uint8_t)c0, (uint8_t)(c1 >> 8), (uint8_t)c1};
    const uint8_t rows[4] = {(uint8_t)(a0 >> 8), (uint8_t)a0, (uint8_t)(a1 >> 8), (uint8_t)a1};
    if (bus->hold) bus->hold(bus);
    bus->write_command(bus, CO5300_CASET);
    bus->write_data(bus, cols, 32);
    bus->write_command(bus, CO5300_RASET);
    bus->write_data(bus, rows, 32);
    bus->write_command(bus, CO5300_RAMWR); /* goes out with the pixels */
    /* the window of the panel's own screen is another now: it sends its bounds anew */
    dgx_screen_with_bus_t *sbus = (dgx_screen_with_bus_t *)scr->panel;
    sbus->cached_area.left = sbus->cached_area.right = sbus->cached_area.top = sbus->cached_area.bottom = 0xffff;
    return at;
}

/* what the wire took for `bytes`, into the measure the next start is reckoned by */
static void dgx_co5300_scan_done(dgx_co5300_t *scr, int64_t since_us, uint32_t bytes)
{
    const int64_t took = dgx_co5300_now() - since_us;
    if (took <= 0 || bytes < 4096u) return;
    const uint32_t ns = (uint32_t)(took * 1000 / bytes);
    scr->ns_per_byte  = (scr->ns_per_byte * 7u + ns) / 8u;
}

/* the rows of a rectangle of the picture in the order of the scan, copied in turn into the three buffers */
static void dgx_co5300_send_scanned(dgx_co5300_t *scr, int left, int right, int top, int bottom)
{
    dgx_bus_protocols_t *bus = scr->bus;
    dgx_screen_t        *panel = scr->panel;
    const int            w = right - left + 1, h = bottom - top + 1, row = w * 2, per = (int)(CO5300_ROWS_BUFFER / (uint32_t)row);
    const uint8_t       *picture = scr->base.v_array;
    const int            pitch   = scr->base.base.width * 2;
    panel->wait(panel);
    dgx_screen_batch_begin(panel);
    const int64_t at = dgx_co5300_scan_window(scr, left, right, top, bottom);
    for (int done = 0, k = 0; done < h; ++k) {
        const int n   = h - done < per ? h - done : per;
        uint8_t  *buf = scr->rows[k % 3];
        for (int i = 0; i < n; ++i) {
            const int y = scr->reversed ? bottom - done - i : top + done + i;
            memcpy(buf + i * row, picture + (size_t)y * (size_t)pitch + (size_t)left * 2u, (size_t)row);
        }
        bus->write_data_async(bus, buf, (uint32_t)(n * row) * 8u);
        done += n;
    }
    bus->sync_write(bus);
    dgx_co5300_scan_done(scr, at, (uint32_t)(w * h * 2));
    dgx_screen_touch(panel, left, right, top, bottom);
    dgx_screen_batch_end(panel);
}

static void dgx_co5300_update_screen(dgx_screen_t *_scr, int left, int right, int top, int bottom)
{
    dgx_co5300_t *scr = (dgx_co5300_t *)_scr;
    /* whole pairs of columns and rows: the window starts on an even column and row and ends on odd ones; the glass is an even number of both */
    left &= ~1;
    right |= 1;
    top &= ~1;
    bottom |= 1;
    if (right > _scr->width - 1) right = _scr->width - 1;
    if (bottom > _scr->height - 1) bottom = _scr->height - 1;
    if (scr->tear_free && !scr->across) dgx_co5300_send_scanned(scr, left, right, top, bottom);
    else dgx_vscreen_region_to_screen(scr->panel, left, top, _scr, left, top, right - left + 1, bottom - top + 1, 0);
}

/* the moves of the picture of the screen in its memory, each its own inverse */
enum { CO5300_SWAP, CO5300_MIRROR_X, CO5300_MIRROR_Y };

/* false when there was no memory for the swap of the axes of a glass that is not square */
static bool dgx_co5300_move(dgx_co5300_t *scr, int move)
{
    const int w = scr->base.base.width, h = scr->base.base.height;
    uint16_t *p = (uint16_t *)scr->base.v_array;
    switch (move) {
    case CO5300_SWAP:
        if (w == h) {
            for (int y = 0; y < h; ++y)
                for (int x = y + 1; x < w; ++x) {
                    const uint16_t t = p[y * w + x];
                    p[y * w + x]     = p[x * w + y];
                    p[x * w + y]     = t;
                }
            return true;
        } else {
            /* the rows become the columns: a copy, which the heap gives as it gave the first */
            uint16_t *q = (uint16_t *)malloc((size_t)w * (size_t)h * sizeof(uint16_t));
            if (!q) return false;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) q[x * h + y] = p[y * w + x];
            free(p);
            scr->base.v_array     = (uint8_t *)q;
            scr->base.base.width  = h;
            scr->base.base.height = w;
            return true;
        }
    case CO5300_MIRROR_X:
        for (int y = 0; y < h; ++y)
            for (uint16_t *a = p + y * w, *b = a + w - 1; a < b; ++a, --b) {
                const uint16_t t = *a;
                *a = *b, *b = t;
            }
        return true;
    default:
        for (int y = 0; y < h / 2; ++y)
            for (uint16_t *a = p + y * w, *b = p + (h - 1 - y) * w, *end = a + w; a < end; ++a, ++b) {
                const uint16_t t = *a;
                *a = *b, *b = t;
            }
        return true;
    }
}

/*
 * A fill whose rectangle starts and ends on pairs of columns and rows needs
 * nothing of the picture around it. The color itself goes to the panel, from
 * the buffer of the bus in internal memory, in one window, and while a band
 * of rows is on the wire the processor fills the same band of the picture in
 * PSRAM: the DMA does not read the picture, so the two do not share its
 * memory and wait for each other only at the end. Any other fill goes into
 * the picture and is sent from there.
 */
static void dgx_co5300_fill_rectangle(dgx_screen_t *_scr, int x, int y, int w, int h, uint32_t color)
{
    dgx_co5300_t *scr = (dgx_co5300_t *)_scr;
    if (w < 0) x += w, w = -w;
    if (h < 0) y += h, h = -h;
    const int cl = dgx_clip_left(_scr), cr = dgx_clip_right(_scr), ct = dgx_clip_top(_scr), cb = dgx_clip_bottom(_scr);
    if (x < cl) w -= cl - x, x = cl;
    if (x + w > cr) w = cr - x;
    if (y < ct) h -= ct - y, y = ct;
    if (y + h > cb) h = cb - y;
    if (w <= 0 || h <= 0) return;
    dgx_bus_protocols_t *bus       = scr->bus;
    const uint32_t       row_bytes = (uint32_t)w * 2u;
    if (_scr->in_progress || ((x | y | w | h) & 1) || !bus->buffer || bus->buffer_len < row_bytes) {
        scr->fill_vscreen(_scr, x, y, w, h, color);
        return;
    }
    /* the color as the wire takes it, as many whole rows of it as the buffer holds */
    const int     band  = (int)(bus->buffer_len / row_bytes);
    dgx_screen_t *panel = scr->panel;
    panel->wait(panel);
    dgx_fill_pixels_16(bus->buffer, (uint32_t)band * (uint32_t)w, color);
    /* a batch on the panel as a transfer to it makes one: at its end the bus is let go of, for the other devices on it */
    dgx_screen_batch_begin(panel);
    /* tear_free: the window in the order of the scan and when the scan allows; one color reads the same either way */
    const bool    scanned = scr->tear_free && !scr->across;
    const int64_t at      = scanned ? dgx_co5300_scan_window(scr, x, x + w - 1, y, y + h - 1) : 0;
    if (!scanned) panel->set_area(panel, (uint16_t)x, (uint16_t)(x + w - 1), (uint16_t)y, (uint16_t)(y + h - 1));
    dgx_screen_batch_begin(_scr);
    for (int r = 0; r < h; r += band) {
        const int n = h - r < band ? h - r : band;
        if (scanned) bus->write_data_async(bus, bus->buffer, (uint32_t)n * row_bytes * 8u);
        else panel->write_area(panel, bus->buffer, (uint32_t)n * row_bytes * 8u);
        scr->fill_vscreen(_scr, x, y + r, w, n, color);
    }
    if (scanned) {
        bus->sync_write(bus);
        dgx_co5300_scan_done(scr, at, (uint32_t)w * (uint32_t)h * 2u);
    }
    panel->wait(panel);
    /* the glass shows what the picture has now: nothing is left to send */
    int left, top, width, height;
    dgx_screen_take_dirty(_scr, &left, &top, &width, &height);
    dgx_screen_batch_end(_scr);
    dgx_screen_touch(panel, x, x + w - 1, y, y + h - 1);
    dgx_screen_batch_end(panel);
}

/*
 * A turn changes nothing on the glass, as on a panel without memory of its
 * own: the picture in memory is turned with the screen, so that it stays
 * what the glass shows. A point of the screen lies on the glass by the swap
 * of the axes first, then the mirror of x, then that of y; the picture is
 * moved to the glass by the old turn and back from it by the new one, and
 * the moves that undo each other are left out.
 */
static void dgx_co5300_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_co5300_t *scr = (dgx_co5300_t *)_scr;
    int           moves[6], n = 0;
    const bool    from[3] = {_scr->swap_xy, _scr->dir_x == DgxScreenRightLeft, _scr->dir_y == DgxScreenBottomTop};
    const bool    to[3]   = {swap_xy, dir_x == DgxScreenRightLeft, dir_y == DgxScreenBottomTop};
    for (int m = 0; m < 3; ++m)
        if (from[m]) moves[n++] = m;
    for (int m = 2; m >= 0; --m)
        if (to[m]) {
            if (n && moves[n - 1] == m) --n;
            else moves[n++] = m;
        }
    /* a glass that is not square swaps the axes by a copy; without memory for it the screen stays as it lies */
    for (int i = 0; i < n; ++i)
        if (!dgx_co5300_move(scr, moves[i])) {
            ESP_LOGE(TAG, "no memory to turn the picture: the screen stays as it lies");
            for (int j = i - 1; j >= 0; --j) dgx_co5300_move(scr, moves[j]);
            return;
        }
    dgx_screen_orientation(scr->panel, dir_x, dir_y, swap_xy);
    dgx_co5300_scan_order(scr);
    dgx_screen_unclip(_scr); /* as every turned screen does */
    _scr->dir_x   = dir_x;
    _scr->dir_y   = dir_y;
    _scr->swap_xy = swap_xy;
}

static esp_err_t dgx_co5300_display(dgx_screen_t *_scr, bool on)
{
    return dgx_screen_display(((dgx_co5300_t *)_scr)->panel, on);
}

static esp_err_t dgx_co5300_brightness(dgx_screen_t *_scr, uint8_t level)
{
    dgx_bus_protocols_t *bus = ((dgx_co5300_t *)_scr)->bus;
    bus->write_command(bus, CO5300_WRDISBV);
    bus->write_data(bus, &level, 8);
    return ESP_OK;
}

static const dgx_screen_control_t dgx_co5300_control = {
    .orientation = dgx_co5300_orientation,
    .display     = dgx_co5300_display,
    .brightness  = dgx_co5300_brightness,
};

static void dgx_co5300_destroy(struct _dgx_screen_t **pscr)
{
    dgx_co5300_t *scr = (dgx_co5300_t *)*pscr;
    if (!scr) return;
    if (scr->tear_free) dgx_co5300_te_stop(scr);
    for (int i = 0; i < 3; ++i) heap_caps_free(scr->rows[i]);
    dgx_screen_destroy(&scr->panel);
    scr->destroy_vscreen(pscr); /* the pixels and the structure, which begins with the virtual screen */
}

dgx_screen_t *dgx_co5300_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo,
                              dgx_co5300_resolution_t resolution)
{
    return dgx_co5300_init_with(bus, rst, color_bits, cbo, resolution, NULL);
}

dgx_screen_t *dgx_co5300_init_with(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo,
                                   dgx_co5300_resolution_t resolution, const dgx_co5300_options_t *options)
{
    const unsigned which = (unsigned)resolution - (unsigned)DGX_CO5300_466X466;
    if (which >= sizeof(co5300_panels) / sizeof(co5300_panels[0])) {
        ESP_LOGE(TAG, "unsupported resolution %d", (int)resolution);
        return NULL;
    }
    if (color_bits != 16) {
        ESP_LOGE(TAG, "%d-bit color is not supported, only 16", (int)color_bits);
        return NULL;
    }
    const dgx_panel_t *panel   = &co5300_panels[which];
    dgx_vscreen_t     *vscreen = (dgx_vscreen_t *)dgx_vscreen_init(panel->width, panel->height, color_bits, cbo);
    if (!vscreen) {
        ESP_LOGE(TAG, "no memory for the picture, %d bytes", panel->width * panel->height * 2);
        return NULL;
    }
    dgx_co5300_t *scr = (dgx_co5300_t *)calloc(1, sizeof(dgx_co5300_t));
    if (!scr) {
        dgx_screen_destroy((dgx_screen_t **)&vscreen);
        return NULL;
    }
    scr->base = *vscreen;
    free(vscreen); /* its pixels now belong to the screen */
    if (options && options->tear_free) {
        for (int i = 0; i < 3; ++i) scr->rows[i] = (uint8_t *)heap_caps_malloc(CO5300_ROWS_BUFFER, MALLOC_CAP_DMA);
        if (!scr->rows[0] || !scr->rows[1] || !scr->rows[2]) {
            ESP_LOGE(TAG, "no DMA memory for the rows sent in the order of the scan, 3 x %u bytes", (unsigned)CO5300_ROWS_BUFFER);
            for (int i = 0; i < 3; ++i) heap_caps_free(scr->rows[i]);
            free(scr->base.v_array);
            free(scr);
            return NULL;
        }
    }
    /* the panel is reset, set up and cleared black, as the picture starts */
    scr->panel = dgx_panel_init(bus, panel, rst, GPIO_NUM_NC, color_bits, cbo);
    if (!scr->panel) {
        free(scr->base.v_array);
        free(scr);
        return NULL;
    }
    scr->bus                      = bus;
    scr->desc                     = panel;
    scr->scan_rows                = panel->memory_height ? panel->memory_height : panel->height;
    scr->ns_per_byte              = 55; /* 40 MHz on four lines, and what a transfer costs besides; measured from then on */
    scr->destroy_vscreen          = scr->base.base.destroy;
    scr->fill_vscreen             = scr->base.base.fill_rectangle;
    scr->base.base.fill_rectangle = dgx_co5300_fill_rectangle;
    scr->base.base.screen_name    = "CO5300";
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.update_screen  = dgx_co5300_update_screen;
    scr->base.base.destroy        = dgx_co5300_destroy;
    scr->base.base.control        = &dgx_co5300_control;
    if (options && options->tear_free) {
        scr->te        = options->te;
        scr->tear_free = dgx_co5300_te_start(scr);
        if (!scr->tear_free) {
            ESP_LOGE(TAG, "the TE line on GPIO %d cannot be listened to", (int)options->te);
            dgx_co5300_destroy((dgx_screen_t **)&scr);
            return NULL;
        }
        dgx_co5300_scan_order(scr);
    }
    return (dgx_screen_t *)scr;
}
