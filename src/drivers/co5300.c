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

#include "dgx_vscreen.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "esp_log.h"

#include "../dgx_clip_priv.h"

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
    dgx_bus_protocols_t *bus;
    void (*destroy_vscreen)(struct _dgx_screen_t **pscr);
    dgx_scr_fill_rectangle_func fill_vscreen; /* the fill of the picture alone */
} dgx_co5300_t;

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
    dgx_vscreen_region_to_screen(scr->panel, left, top, _scr, left, top, right - left + 1, bottom - top + 1, 0);
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
 * A turn changes nothing on the glass, as on a panel without memory of its
 * own: the picture in memory is turned with the screen, so that it stays
 * what the glass shows. A point of the screen lies on the glass by the swap
 * of the axes first, then the mirror of x, then that of y; the picture is
 * moved to the glass by the old turn and back from it by the new one, and
 * the moves that undo each other are left out.
 */
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
    panel->set_area(panel, (uint16_t)x, (uint16_t)(x + w - 1), (uint16_t)y, (uint16_t)(y + h - 1));
    dgx_screen_batch_begin(_scr);
    for (int r = 0; r < h; r += band) {
        const int n = h - r < band ? h - r : band;
        panel->write_area(panel, bus->buffer, (uint32_t)n * row_bytes * 8u);
        scr->fill_vscreen(_scr, x, y + r, w, n, color);
    }
    panel->wait(panel);
    /* the glass shows what the picture has now: nothing is left to send */
    int left, top, width, height;
    dgx_screen_take_dirty(_scr, &left, &top, &width, &height);
    dgx_screen_batch_end(_scr);
    dgx_screen_touch(panel, x, x + w - 1, y, y + h - 1);
    dgx_screen_batch_end(panel);
}

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
    dgx_screen_destroy(&scr->panel);
    scr->destroy_vscreen(pscr); /* the pixels and the structure, which begins with the virtual screen */
}

dgx_screen_t *dgx_co5300_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo,
                              dgx_co5300_resolution_t resolution)
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
    /* the panel is reset, set up and cleared black, as the picture starts */
    scr->panel = dgx_panel_init(bus, panel, rst, GPIO_NUM_NC, color_bits, cbo);
    if (!scr->panel) {
        free(scr->base.v_array);
        free(scr);
        return NULL;
    }
    scr->bus                      = bus;
    scr->destroy_vscreen          = scr->base.base.destroy;
    scr->fill_vscreen             = scr->base.base.fill_rectangle;
    scr->base.base.fill_rectangle = dgx_co5300_fill_rectangle;
    scr->base.base.screen_name    = "CO5300";
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.update_screen  = dgx_co5300_update_screen;
    scr->base.base.destroy        = dgx_co5300_destroy;
    scr->base.base.control        = &dgx_co5300_control;
    return (dgx_screen_t *)scr;
}
