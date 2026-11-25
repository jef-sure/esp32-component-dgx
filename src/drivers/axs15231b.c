/*
 * axs15231b.c
 *
 *  The AXS15231B on QSPI, the controller of the 320x480 IPS panel of the
 *  Guition JC3248W535. The controller stores its picture by blocks of 4x4
 *  pixels, "4 lines a step" in its datasheet: a window that starts or ends
 *  inside a block spoils the other pixels of the block, which show up as
 *  rows shifted by 4 pixels and stray points, and the panel ripples while
 *  such windows are written (checked on the board, at 20 and at 40 MHz, in
 *  step with the TE line or not). The screen keeps its picture in a 16-bit
 *  virtual screen, as the monochrome drivers do, and sends what changed
 *  widened to whole blocks of the glass, the pixels added taken from the
 *  picture.
 *
 *  The pixels of a window go in whole rows: a piece that ends inside a row,
 *  the next going on with 0x3C, lands in the wrong place. Every piece is a
 *  number of rows of 4 copied into a buffer of its own.
 *
 *  MADCTL mirrors the axes but does not swap them on QSPI: with MV the panel
 *  shows a strip at the top and nothing else. MADCTL stays upright, and the
 *  driver lays the picture onto the glass itself as it copies the rows to
 *  send, in all eight orientations.
 *
 *  The init sequence is that of ESPHome's model of this board: the vendor
 *  registers unlocked, C1h set to 33h and locked again, then sleep out. The
 *  memory is cleared to black before the display is turned on.
 */

#include "drivers/axs15231b.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_backlight_priv.h"
#include "dgx_vscreen.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "dgx_screen_with_bus.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "../dgx_clip_priv.h"

#define AXS15231B_SLPOUT 0x11 // Sleep out
#define AXS15231B_NORON  0x13 // Normal display mode on
#define AXS15231B_INVOFF 0x20 // Display inversion off
#define AXS15231B_DISPON 0x29 // Display on
#define AXS15231B_MADCTL 0x36 // Memory data access control
#define AXS15231B_COLMOD 0x3A // Interface pixel format
#define AXS15231B_UNLOCK 0xBB // Vendor registers: unlocked by 5A A5 in the last two bytes, locked by zeros

/* the side of the blocks the controller stores its picture by */
#define AXS15231B_BLOCK 4
/* the buffers rows are copied into for sending, three of them in turn: the bus holds two transfers at most */
#define AXS15231B_ROWS_BUFFER 8192u

static const char TAG[] = "DGX AXS15231B";

static const dgx_lcd_init_cmd_t axs15231b_init_cmds[] = {
    {AXS15231B_UNLOCK, 0,                    8,    0,   {0, 0, 0, 0, 0, 0, 0x5A, 0xA5}}, // vendor registers unlocked
    {0xC1,             0,                    1,    0,   {0x33}                        }, // as ESPHome sets it for this board
    {AXS15231B_UNLOCK, 0,                    8,    0,   {0}                           }, // and locked again
    {AXS15231B_SLPOUT, 0,                    0,    200, {0}                           }, //
    {AXS15231B_COLMOD, dgx_panel_adj_colmod, 1,    0,   {0x55}                        }, // 16 bits a pixel
    {AXS15231B_MADCTL, dgx_panel_adj_madctl, 1,    0,   {0}                           }, // upright, in the color order of the screen
    {AXS15231B_INVOFF, 0,                    0,    0,   {0}                           }, //
    {AXS15231B_NORON,  0,                    0,    0,   {0}                           }, //
    {0,                0,                    0xff, 0,   {0}                           }  // Stop
};

/* the display is turned on once its memory is cleared, so whatever was left in it is never shown */
static const dgx_lcd_init_cmd_t axs15231b_on_cmds[] = {
    {AXS15231B_DISPON, 0, 0,    200, {0}}, //
    {0,                0, 0xff, 0,   {0}}  // Stop
};

/* the glasses, by dgx_axs15231b_resolution_t from 1 on */
static const dgx_panel_t axs15231b_panels[] = {
    {.name           = "AXS15231B",
     .width          = 320,
     .height         = 480,
     .colmod         = {0, 0x55},
     .madctl         = 0x00,
     .reset_low_ms   = 10,
     .reset_after_ms = 120,
     .init           = axs15231b_init_cmds}, /* Guition JC3248W535 */
};

typedef struct {
    dgx_vscreen_t        base;  /* the picture, as the program draws it */
    dgx_screen_t        *panel; /* the screen of the panel, always upright, it is sent to by windows of whole blocks */
    dgx_bus_protocols_t *bus;
    gpio_num_t           backlight;
    void (*destroy_vscreen)(struct _dgx_screen_t **pscr);
    dgx_scr_fill_rectangle_func fill_vscreen; /* the fill of the picture alone */
    uint8_t                    *rows[3];      /* rows copied for sending */
} dgx_axs15231b_t;

/*
 * A point of the screen lies on the glass by the swap of the axes first,
 * then the mirror of x, then that of y, as on the panels turned by MADCTL.
 */
static void dgx_axs15231b_to_glass(const dgx_screen_t *scr, int gw, int gh, int x, int y, int *gx, int *gy)
{
    int u = scr->swap_xy ? y : x, v = scr->swap_xy ? x : y;
    if (scr->dir_x == DgxScreenRightLeft) u = gw - 1 - u;
    if (scr->dir_y == DgxScreenBottomTop) v = gh - 1 - v;
    *gx = u, *gy = v;
}

/* the rectangle of the screen x0..x1, y0..y1 on the glass, widened to whole blocks */
static void dgx_axs15231b_glass_rect(const dgx_axs15231b_t *scr, int x0, int x1, int y0, int y1, int *g)
{
    const int gw = scr->panel->width, gh = scr->panel->height;
    int       ax, ay, bx, by;
    dgx_axs15231b_to_glass(&scr->base.base, gw, gh, x0, y0, &ax, &ay);
    dgx_axs15231b_to_glass(&scr->base.base, gw, gh, x1, y1, &bx, &by);
    g[0] = ax < bx ? ax : bx, g[1] = ax < bx ? bx : ax;
    g[2] = ay < by ? ay : by, g[3] = ay < by ? by : ay;
}

/*
 * Row gy of the glass, columns gx0..gx1, from the picture into `out`: a
 * row of the picture, or its column when the axes are swapped, read forwards
 * or backwards.
 */
static void dgx_axs15231b_glass_row(const dgx_axs15231b_t *scr, int gy, int gx0, int gx1, uint16_t *out)
{
    const dgx_screen_t *s  = &scr->base.base;
    const int           gw = scr->panel->width, gh = scr->panel->height, pitch = s->width;
    const uint16_t     *p  = (const uint16_t *)scr->base.v_array;
    const int           v  = s->dir_y == DgxScreenBottomTop ? gh - 1 - gy : gy;
    const bool          back = s->dir_x == DgxScreenRightLeft;
    const int           n    = gx1 - gx0 + 1;
    /* the first pixel of the row on the screen and the step to the next one */
    const int u0 = back ? gw - 1 - gx0 : gx0;
    if (!s->swap_xy) {
        const uint16_t *src = p + (size_t)v * (size_t)pitch + u0;
        if (!back) memcpy(out, src, (size_t)n * sizeof(uint16_t));
        else
            for (int i = 0; i < n; ++i) out[i] = src[-i];
    } else {
        /* u is the row of the screen, v its column */
        const uint16_t *src  = p + (size_t)u0 * (size_t)pitch + v;
        const ptrdiff_t step = back ? -(ptrdiff_t)pitch : (ptrdiff_t)pitch;
        for (int i = 0; i < n; ++i, src += step) out[i] = *src;
    }
}

/* the window of the glass gx0..gx1, gy0..gy1, of whole blocks, sent from the picture in pieces of whole rows */
static void dgx_axs15231b_send(dgx_axs15231b_t *scr, int gx0, int gx1, int gy0, int gy1)
{
    dgx_screen_t  *panel = scr->panel;
    const int      w = gx1 - gx0 + 1, h = gy1 - gy0 + 1, row = w * 2;
    const uint32_t room = scr->bus->buffer_len && scr->bus->buffer_len < AXS15231B_ROWS_BUFFER ? scr->bus->buffer_len : AXS15231B_ROWS_BUFFER;
    int            per  = (int)(room / (uint32_t)row) / AXS15231B_BLOCK * AXS15231B_BLOCK;
    if (per < AXS15231B_BLOCK) per = AXS15231B_BLOCK;
    panel->wait(panel);
    dgx_screen_batch_begin(panel);
    panel->set_area(panel, (uint16_t)gx0, (uint16_t)gx1, (uint16_t)gy0, (uint16_t)gy1);
    for (int done = 0, k = 0; done < h; ++k) {
        const int n   = h - done < per ? h - done : per;
        uint8_t  *buf = scr->rows[k % 3];
        for (int i = 0; i < n; ++i) dgx_axs15231b_glass_row(scr, gy0 + done + i, gx0, gx1, (uint16_t *)(buf + i * row));
        panel->write_area(panel, buf, (uint32_t)(n * row) * 8u);
        done += n;
    }
    panel->wait(panel);
    dgx_screen_touch(panel, gx0, gx1, gy0, gy1);
    dgx_screen_batch_end(panel);
}

static void dgx_axs15231b_update_screen(dgx_screen_t *_scr, int left, int right, int top, int bottom)
{
    dgx_axs15231b_t *scr = (dgx_axs15231b_t *)_scr;
    int              g[4];
    dgx_axs15231b_glass_rect(scr, left, right, top, bottom, g);
    /* whole blocks: the glass is a whole number of them */
    g[0] &= ~(AXS15231B_BLOCK - 1);
    g[1] |= AXS15231B_BLOCK - 1;
    g[2] &= ~(AXS15231B_BLOCK - 1);
    g[3] |= AXS15231B_BLOCK - 1;
    dgx_axs15231b_send(scr, g[0], g[1], g[2], g[3]);
}

/*
 * A fill whose rectangle covers whole blocks of the glass needs nothing of
 * the picture around it. The color itself goes to the panel, from the buffer
 * of the bus, in one window of bands of rows, and while a band is on the
 * wire the processor fills a slice of the picture: the DMA does not read the
 * picture, so the two do not wait for each other but at the end. Any other fill goes into
 * the picture and is sent from there.
 */
static void dgx_axs15231b_fill_rectangle(dgx_screen_t *_scr, int x, int y, int w, int h, uint32_t color)
{
    dgx_axs15231b_t *scr = (dgx_axs15231b_t *)_scr;
    if (w < 0) x += w, w = -w;
    if (h < 0) y += h, h = -h;
    const int cl = dgx_clip_left(_scr), cr = dgx_clip_right(_scr), ct = dgx_clip_top(_scr), cb = dgx_clip_bottom(_scr);
    if (x < cl) w -= cl - x, x = cl;
    if (x + w > cr) w = cr - x;
    if (y < ct) h -= ct - y, y = ct;
    if (y + h > cb) h = cb - y;
    if (w <= 0 || h <= 0) return;
    dgx_bus_protocols_t *bus = scr->bus;
    int                  g[4];
    dgx_axs15231b_glass_rect(scr, x, x + w - 1, y, y + h - 1, g);
    const int      gw = g[1] - g[0] + 1, gh = g[3] - g[2] + 1;
    const uint32_t row_bytes = (uint32_t)gw * 2u;
    const bool     blocks    = !((g[0] | gw | g[2] | gh) & (AXS15231B_BLOCK - 1));
    if (_scr->in_progress || !blocks || !bus->buffer || bus->buffer_len < row_bytes * AXS15231B_BLOCK) {
        scr->fill_vscreen(_scr, x, y, w, h, color);
        return;
    }
    /* the color as the wire takes it, as many rows of it as the buffer holds, a whole number of blocks */
    const int     band  = (int)(bus->buffer_len / row_bytes) / AXS15231B_BLOCK * AXS15231B_BLOCK;
    dgx_screen_t *panel = scr->panel;
    panel->wait(panel);
    dgx_fill_pixels_16(bus->buffer, (uint32_t)band * (uint32_t)gw, color);
    /* a batch on the panel as a transfer to it makes one: at its end the bus is let go of, for the other devices on it */
    dgx_screen_batch_begin(panel);
    panel->set_area(panel, (uint16_t)g[0], (uint16_t)g[1], (uint16_t)g[2], (uint16_t)g[3]);
    /*
     * While a band of the glass is on the wire, a slice of the rows of the
     * picture is filled: the fill is one color, so which slice goes with
     * which band does not matter, only that all of them are filled.
     */
    const int bands = (gh + band - 1) / band;
    dgx_screen_batch_begin(_scr);
    for (int k = 0, r = 0; k < bands; ++k, r += band) {
        const int n = gh - r < band ? gh - r : band;
        panel->write_area(panel, bus->buffer, (uint32_t)n * row_bytes * 8u);
        const int from = h * k / bands, to = h * (k + 1) / bands;
        if (to > from) scr->fill_vscreen(_scr, x, y + from, w, to - from, color);
    }
    panel->wait(panel);
    /* the glass shows what the picture has now: nothing is left to send */
    int left, top, width, height;
    dgx_screen_take_dirty(_scr, &left, &top, &width, &height);
    dgx_screen_batch_end(_scr);
    dgx_screen_touch(panel, g[0], g[1], g[2], g[3]);
    dgx_screen_batch_end(panel);
}

/* the moves of the picture of the screen in its memory, each its own inverse */
enum { AXS15231B_SWAP, AXS15231B_MIRROR_X, AXS15231B_MIRROR_Y };

/* false when there was no memory for the swap of the axes */
static bool dgx_axs15231b_move(dgx_axs15231b_t *scr, int move)
{
    const int w = scr->base.base.width, h = scr->base.base.height;
    uint16_t *p = (uint16_t *)scr->base.v_array;
    switch (move) {
    case AXS15231B_SWAP: {
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
    case AXS15231B_MIRROR_X:
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
 * what the glass shows. The picture is moved to the glass by the old turn
 * and back from it by the new one, and the moves that undo each other are
 * left out.
 */
static void dgx_axs15231b_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_axs15231b_t *scr = (dgx_axs15231b_t *)_scr;
    int              moves[6], n = 0;
    const bool       from[3] = {_scr->swap_xy, _scr->dir_x == DgxScreenRightLeft, _scr->dir_y == DgxScreenBottomTop};
    const bool       to[3]   = {swap_xy, dir_x == DgxScreenRightLeft, dir_y == DgxScreenBottomTop};
    for (int m = 0; m < 3; ++m)
        if (from[m]) moves[n++] = m;
    for (int m = 2; m >= 0; --m)
        if (to[m]) {
            if (n && moves[n - 1] == m) --n;
            else moves[n++] = m;
        }
    for (int i = 0; i < n; ++i)
        if (!dgx_axs15231b_move(scr, moves[i])) {
            ESP_LOGE(TAG, "no memory to turn the picture: the screen stays as it lies");
            for (int j = i - 1; j >= 0; --j) dgx_axs15231b_move(scr, moves[j]);
            return;
        }
    dgx_screen_unclip(_scr); /* as every turned screen does */
    _scr->dir_x   = dir_x;
    _scr->dir_y   = dir_y;
    _scr->swap_xy = swap_xy;
}

static esp_err_t dgx_axs15231b_backlight(dgx_screen_t *_scr, bool on)
{
    return dgx_backlight_set(((dgx_axs15231b_t *)_scr)->backlight, on);
}

static esp_err_t dgx_axs15231b_display(dgx_screen_t *_scr, bool on)
{
    return dgx_screen_display(((dgx_axs15231b_t *)_scr)->panel, on);
}

static const dgx_screen_control_t dgx_axs15231b_control = {
    .orientation = dgx_axs15231b_orientation,
    .backlight   = dgx_axs15231b_backlight,
    .display     = dgx_axs15231b_display,
};

static void dgx_axs15231b_destroy(struct _dgx_screen_t **pscr)
{
    dgx_axs15231b_t *scr = (dgx_axs15231b_t *)*pscr;
    if (!scr) return;
    for (int i = 0; i < 3; ++i) heap_caps_free(scr->rows[i]);
    dgx_screen_destroy(&scr->panel);
    scr->destroy_vscreen(pscr); /* the pixels and the structure, which begins with the virtual screen */
}

dgx_screen_t *dgx_axs15231b_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
                                 dgx_axs15231b_resolution_t resolution)
{
    const unsigned which = (unsigned)resolution - (unsigned)DGX_AXS15231B_320X480;
    if (which >= sizeof(axs15231b_panels) / sizeof(axs15231b_panels[0])) {
        ESP_LOGE(TAG, "unsupported resolution %d", (int)resolution);
        return NULL;
    }
    if (color_bits != 16) {
        ESP_LOGE(TAG, "%d-bit color is not supported, only 16", (int)color_bits);
        return NULL;
    }
    const dgx_panel_t *desc    = &axs15231b_panels[which];
    dgx_vscreen_t     *vscreen = (dgx_vscreen_t *)dgx_vscreen_init(desc->width, desc->height, color_bits, cbo);
    if (!vscreen) {
        ESP_LOGE(TAG, "no memory for the picture, %d bytes", desc->width * desc->height * 2);
        return NULL;
    }
    dgx_axs15231b_t *scr = (dgx_axs15231b_t *)calloc(1, sizeof(dgx_axs15231b_t));
    if (!scr) {
        dgx_screen_destroy((dgx_screen_t **)&vscreen);
        return NULL;
    }
    scr->base = *vscreen;
    free(vscreen); /* its pixels now belong to the screen */
    for (int i = 0; i < 3; ++i) scr->rows[i] = (uint8_t *)heap_caps_malloc(AXS15231B_ROWS_BUFFER, MALLOC_CAP_DMA);
    if (!scr->rows[0] || !scr->rows[1] || !scr->rows[2]) {
        ESP_LOGE(TAG, "no DMA memory for the rows sent, 3 x %u bytes", (unsigned)AXS15231B_ROWS_BUFFER);
        for (int i = 0; i < 3; ++i) heap_caps_free(scr->rows[i]);
        free(scr->base.v_array);
        free(scr);
        return NULL;
    }
    /* dark until there is a picture */
    if (backlight >= 0) {
        dgx_gpio_set_direction(backlight, GPIO_MODE_OUTPUT);
        dgx_backlight_set(backlight, false);
    }
    scr->panel = dgx_panel_init(bus, desc, rst, GPIO_NUM_NC, color_bits, cbo);
    if (!scr->panel) {
        for (int i = 0; i < 3; ++i) heap_caps_free(scr->rows[i]);
        free(scr->base.v_array);
        free(scr);
        return NULL;
    }
    scr->bus                      = bus;
    scr->backlight                = backlight;
    scr->destroy_vscreen          = scr->base.base.destroy;
    scr->fill_vscreen             = scr->base.base.fill_rectangle;
    scr->base.base.fill_rectangle = dgx_axs15231b_fill_rectangle;
    scr->base.base.screen_name    = "AXS15231B";
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.update_screen  = dgx_axs15231b_update_screen;
    scr->base.base.destroy        = dgx_axs15231b_destroy;
    scr->base.base.control        = &dgx_axs15231b_control;
    /* the memory of the panel black, as the picture starts, by windows of whole blocks; then the display on and the light */
    dgx_axs15231b_send(scr, 0, desc->width - 1, 0, desc->height - 1);
    dgx_lcd_init((dgx_screen_with_bus_t *)scr->panel, axs15231b_on_cmds);
    if (backlight >= 0) dgx_backlight_set(backlight, true);
    return (dgx_screen_t *)scr;
}
