/*
 * ssd1680.c
 *
 * SSD1680, the controller of e-paper panels of three colors: two planes of
 * memory of a bit a pixel, black-and-white (24h, 1 white, 0 black) and red
 * (26h, 1 red), a byte for 8 pixels along a gate line with the first one in
 * its high bit. The glass is changed all at once by a refresh that takes
 * about 20 s, while the BUSY line is high; the controller takes no command
 * meanwhile.
 *
 * The driver keeps the picture in the two planes as the controller takes
 * them and draws into them, each pixel turned to its place on the glass as
 * it is drawn; dgx_ssd1680_refresh() sends both and starts the refresh.
 * The commands are those of WeAct Studio for its modules, and of GxEPD2:
 * sources from S8 (21h), the memory written left to right and top to bottom
 * (11h = 03h), the built-in temperature sensor (18h), and the display
 * update F7h, which loads the temperature, drives the glass by the
 * waveform of the OTP for it and turns the analog part off. After a
 * refresh the controller goes into deep sleep 1, from which only a
 * hardware reset wakes it.
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 */

#include "drivers/ssd1680.h"

#include <stdlib.h>
#include <string.h>

#include "dgx_arch.h"
#include "dgx_bits.h"
#include "dgx_screen_functions.h"
#include "esp_log.h"

#include "../vscreen/dgx_soft_turn_priv.h"

#define SSD1680_CMD_DRIVER_OUTPUT  0x01 ///< gate lines - 1, low byte and bit 8; scan direction
#define SSD1680_CMD_DEEP_SLEEP     0x10
#define SSD1680_CMD_DATA_ENTRY     0x11
#define SSD1680_CMD_SW_RESET       0x12
#define SSD1680_CMD_TEMP_SENSOR    0x18
#define SSD1680_CMD_MASTER_ACT     0x20 ///< runs the sequence of 22h
#define SSD1680_CMD_UPDATE_CTRL1   0x21
#define SSD1680_CMD_UPDATE_CTRL2   0x22
#define SSD1680_CMD_WRITE_BW       0x24
#define SSD1680_CMD_WRITE_RED      0x26
#define SSD1680_CMD_BORDER         0x3C
#define SSD1680_CMD_RAM_X_RANGE    0x44 ///< in bytes of 8 pixels
#define SSD1680_CMD_RAM_Y_RANGE    0x45
#define SSD1680_CMD_RAM_X_COUNTER  0x4E
#define SSD1680_CMD_RAM_Y_COUNTER  0x4F

typedef struct {
    uint16_t width, height; /* the glass: sources, gates */
} ssd1680_module_t;

static const ssd1680_module_t ssd1680_modules[] = {
    [SSD1680_128X296_BWR] = {128, 296},
    [SSD1680_122X250_BWR] = {122, 250},
};

/*
 * A color of the screen is its ink, DGX_BWR_BLACK, DGX_BWR_WHITE or DGX_BWR_RED, any value above them red too. On the
 * planes: black is 0 and 0, white 1 and 0, red 1 and 1 (the black-and-white bit, the red bit), as GxEPD2 has it.
 */
static inline bool dgx_ssd1680_bw_bit(uint32_t color)
{
    return color != DGX_BWR_BLACK;
}

static inline bool dgx_ssd1680_red_bit(uint32_t color)
{
    return color > DGX_BWR_WHITE;
}

/* where the controller is */
enum {
    SSD1680_AWAKE,      /* takes commands */
    SSD1680_REFRESHING, /* changing the glass, BUSY high until it is done */
    SSD1680_ASLEEP,     /* in deep sleep: a reset and the init commands again */
};

typedef struct _dgx_ssd1680_t {
    dgx_screen_t            base;
    dgx_bus_protocols_t    *bus;
    const ssd1680_module_t *module;
    gpio_num_t              rst, busy;
    int                     pitch; /* bytes a gate line of the planes */
    uint8_t                *bw, *red;
    int                     state;
    /* the window of write_area(): the rectangle of the screen, and the pixel the next one goes to */
    struct {
        int left, right, top, bottom, x, y;
    } area;
} dgx_ssd1680_t;

static const char TAG[] = "DGX SSD1680";

/* the pixel (px, py) of the glass */
static inline void dgx_ssd1680_put(dgx_ssd1680_t *scr, int px, int py, uint32_t color)
{
    const size_t  at   = (size_t)py * (size_t)scr->pitch + (size_t)(px >> 3);
    const uint8_t mask = (uint8_t)(0x80u >> (px & 7));
    if (dgx_ssd1680_bw_bit(color)) scr->bw[at] |= mask;
    else scr->bw[at] &= (uint8_t)~mask;
    if (dgx_ssd1680_red_bit(color)) scr->red[at] |= mask;
    else scr->red[at] &= (uint8_t)~mask;
}

/* pixels x0..x1 of a gate line of a plane set to 1 or to 0 */
static void dgx_ssd1680_run(uint8_t *line, int x0, int x1, bool on)
{
    const int     b0 = x0 >> 3, b1 = x1 >> 3;
    const uint8_t m0 = (uint8_t)(0xffu >> (x0 & 7)), m1 = (uint8_t)(0xff00u >> ((x1 & 7) + 1));
    if (b0 == b1) {
        const uint8_t m = m0 & m1;
        line[b0]        = on ? (uint8_t)(line[b0] | m) : (uint8_t)(line[b0] & ~m);
        return;
    }
    line[b0] = on ? (uint8_t)(line[b0] | m0) : (uint8_t)(line[b0] & ~m0);
    if (b1 > b0 + 1) memset(line + b0 + 1, on ? 0xff : 0x00, (size_t)(b1 - b0 - 1));
    line[b1] = on ? (uint8_t)(line[b1] | m1) : (uint8_t)(line[b1] & ~m1);
}

/* the pixel of the glass a pixel of the screen lies on */
static inline void dgx_ssd1680_to_glass(const dgx_ssd1680_t *scr, int x, int y, int *px, int *py)
{
    const int ax = scr->base.swap_xy ? y : x, ay = scr->base.swap_xy ? x : y;
    *px          = scr->base.dir_x == DgxScreenRightLeft ? scr->module->width - 1 - ax : ax;
    *py          = scr->base.dir_y == DgxScreenBottomTop ? scr->module->height - 1 - ay : ay;
}

static void dgx_ssd1680_set_pixel(dgx_screen_t *_scr, int x, int y, uint32_t color)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)_scr;
    if (x < 0 || y < 0 || x >= _scr->width || y >= _scr->height) return;
    int px, py;
    dgx_ssd1680_to_glass(scr, x, y, &px, &py);
    dgx_ssd1680_put(scr, px, py, color);
    dgx_screen_touch(_scr, x, x, y, y);
}

static uint32_t dgx_ssd1680_get_pixel(dgx_screen_t *_scr, int x, int y)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)_scr;
    if (x < 0 || y < 0 || x >= _scr->width || y >= _scr->height) return 0;
    int px, py;
    dgx_ssd1680_to_glass(scr, x, y, &px, &py);
    const size_t  at   = (size_t)py * (size_t)scr->pitch + (size_t)(px >> 3);
    const uint8_t mask = (uint8_t)(0x80u >> (px & 7));
    if (scr->red[at] & mask) return DGX_BWR_RED;
    return (scr->bw[at] & mask) ? DGX_BWR_WHITE : DGX_BWR_BLACK;
}

static void dgx_ssd1680_fill_rectangle(dgx_screen_t *_scr, int x, int y, int w, int h, uint32_t color)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)_scr;
    /* a negative width or height goes to the left of x or up from y, as on a display behind a bus */
    if (w < 0) x += w, w = -w;
    if (h < 0) y += h, h = -h;
    /* clipped to the screen; the far corner in 64 bits, for x + w of a huge w */
    const int sl = x < 0 ? 0 : x, st = y < 0 ? 0 : y;
    const int sr = (int64_t)x + w > _scr->width ? _scr->width - 1 : x + w - 1;
    const int sb = (int64_t)y + h > _scr->height ? _scr->height - 1 : y + h - 1;
    int       left = sl, right = sr, top = st, bottom = sb;
    if (!dgx_soft_turn_rect(_scr, scr->module->width, scr->module->height, &left, &right, &top, &bottom)) return;
    for (int py = top; py <= bottom; ++py) {
        dgx_ssd1680_run(scr->bw + (size_t)py * (size_t)scr->pitch, left, right, dgx_ssd1680_bw_bit(color));
        dgx_ssd1680_run(scr->red + (size_t)py * (size_t)scr->pitch, left, right, dgx_ssd1680_red_bit(color));
    }
    dgx_screen_touch(_scr, sl, sr, st, sb);
}

static void dgx_ssd1680_set_area(dgx_screen_t *_scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)_scr;
    if (left > right) DGX_INT_SWAP(left, right);
    if (top > bottom) DGX_INT_SWAP(top, bottom);
    /* a window off the screen takes nothing, rather than the pixels going into the window before it */
    if (left >= _scr->width || top >= _scr->height) {
        scr->area.left = 1, scr->area.right = 0;
        return;
    }
    scr->area.left   = left;
    scr->area.right  = right >= _scr->width ? _scr->width - 1 : right;
    scr->area.top    = top;
    scr->area.bottom = bottom >= _scr->height ? _scr->height - 1 : bottom;
    scr->area.x      = left;
    scr->area.y      = top;
}

/* pixels of a byte into the window left to right and top to bottom, wrapping inside it */
static void dgx_ssd1680_write_area(dgx_screen_t *_scr, uint8_t *data, uint32_t lenbits)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)_scr;
    if (scr->area.left > scr->area.right) return; /* the window lies off the screen */
    int x = scr->area.x, y = scr->area.y;
    for (uint32_t n = lenbits / 8u; n; --n, ++data) {
        if (x > scr->area.right) {
            x = scr->area.left;
            y = y >= scr->area.bottom ? scr->area.top : y + 1;
        }
        int px, py;
        dgx_ssd1680_to_glass(scr, x, y, &px, &py);
        dgx_ssd1680_put(scr, px, py, *data);
        ++x;
    }
    scr->area.x = x;
    scr->area.y = y;
}

static void dgx_ssd1680_wait_buffer(dgx_screen_t *_scr)
{
    (void)_scr;
}

/* drawing changes the planes only: the panel is changed by dgx_ssd1680_refresh() */
static void dgx_ssd1680_update_screen(dgx_screen_t *_scr, int left, int right, int top, int bottom)
{
    (void)_scr, (void)left, (void)right, (void)top, (void)bottom;
}

static void dgx_ssd1680_command(dgx_ssd1680_t *scr, uint8_t cmd, const uint8_t *data, uint32_t len)
{
    dgx_bus_protocols_t *bus = scr->bus;
    bus->write_command(bus, cmd);
    if (len)
        bus->write_data(bus, data, len * 8u);
    else
        bus->sync_write(bus); /* a bus that holds a command back until its data sends it now */
}

/* until BUSY is low; a line that stays high, not wired or wired to another pin, is told of every minute */
static void dgx_ssd1680_wait(dgx_ssd1680_t *scr)
{
    scr->bus->sync_write(scr->bus);
    for (uint32_t polls = 1; dgx_gpio_get_level(scr->busy); ++polls) {
        dgx_delay(10);
        if (polls % 6000u == 0) ESP_LOGE(TAG, "BUSY (GPIO %d) has been high for %u min at least", (int)scr->busy, (unsigned)(polls / 6000u));
    }
}

/* a reset when the line is wired, and the commands that set the controller up for this module */
static void dgx_ssd1680_start(dgx_ssd1680_t *scr)
{
    if (scr->rst != GPIO_NUM_NC) {
        dgx_gpio_set_level(scr->rst, 0);
        dgx_delay(10);
        dgx_gpio_set_level(scr->rst, 1);
        dgx_delay(10);
    }
    dgx_ssd1680_wait(scr);
    dgx_ssd1680_command(scr, SSD1680_CMD_SW_RESET, NULL, 0);
    dgx_delay(10);
    dgx_ssd1680_wait(scr);
    const int     gates     = scr->module->height - 1;
    const uint8_t output[3] = {(uint8_t)(gates & 0xff), (uint8_t)(gates >> 8), 0x00}; /* G0 first, no interlace */
    const uint8_t entry     = 0x03;                                                 /* X then Y, both rising */
    const uint8_t border    = 0x05;                                                 /* the border kept white */
    const uint8_t sensor    = 0x80;                                                 /* the built-in one */
    const uint8_t ctrl1[2]  = {0x00, 0x80};                                         /* planes as they are; S8 to S167 */
    dgx_ssd1680_command(scr, SSD1680_CMD_DRIVER_OUTPUT, output, 3);
    dgx_ssd1680_command(scr, SSD1680_CMD_DATA_ENTRY, &entry, 1);
    dgx_ssd1680_command(scr, SSD1680_CMD_BORDER, &border, 1);
    dgx_ssd1680_command(scr, SSD1680_CMD_TEMP_SENSOR, &sensor, 1);
    dgx_ssd1680_command(scr, SSD1680_CMD_UPDATE_CTRL1, ctrl1, 2);
    scr->state = SSD1680_AWAKE;
}

/* the refresh under way waited for, and the controller sent to sleep, when a reset can wake it */
static void dgx_ssd1680_finish(dgx_ssd1680_t *scr)
{
    if (scr->state != SSD1680_REFRESHING) return;
    dgx_ssd1680_wait(scr);
    if (scr->rst == GPIO_NUM_NC) {
        scr->state = SSD1680_AWAKE;
        return;
    }
    const uint8_t mode = 0x01; /* deep sleep 1: the memory kept */
    dgx_ssd1680_command(scr, SSD1680_CMD_DEEP_SLEEP, &mode, 1);
    scr->state = SSD1680_ASLEEP;
}

/* the whole memory as the window, the counters at its start */
static void dgx_ssd1680_window(dgx_ssd1680_t *scr)
{
    const int     gates   = scr->module->height - 1;
    const uint8_t xs[2]   = {0x00, (uint8_t)(scr->pitch - 1)};
    const uint8_t ys[4]   = {0x00, 0x00, (uint8_t)(gates & 0xff), (uint8_t)(gates >> 8)};
    const uint8_t x0      = 0x00;
    const uint8_t y0[2]   = {0x00, 0x00};
    dgx_ssd1680_command(scr, SSD1680_CMD_RAM_X_RANGE, xs, 2);
    dgx_ssd1680_command(scr, SSD1680_CMD_RAM_Y_RANGE, ys, 4);
    dgx_ssd1680_command(scr, SSD1680_CMD_RAM_X_COUNTER, &x0, 1);
    dgx_ssd1680_command(scr, SSD1680_CMD_RAM_Y_COUNTER, y0, 2);
}

void dgx_ssd1680_refresh(dgx_screen_t *_scr)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)_scr;
    dgx_ssd1680_finish(scr);
    if (scr->state == SSD1680_ASLEEP) dgx_ssd1680_start(scr);
    const uint32_t bytes = (uint32_t)scr->pitch * scr->module->height;
    dgx_ssd1680_window(scr);
    dgx_ssd1680_command(scr, SSD1680_CMD_WRITE_BW, scr->bw, bytes);
    dgx_ssd1680_window(scr);
    dgx_ssd1680_command(scr, SSD1680_CMD_WRITE_RED, scr->red, bytes);
    const uint8_t sequence = 0xf7; /* clock and analog on, the temperature, display mode 1, analog and clock off */
    dgx_ssd1680_command(scr, SSD1680_CMD_UPDATE_CTRL2, &sequence, 1);
    dgx_ssd1680_command(scr, SSD1680_CMD_MASTER_ACT, NULL, 0);
    scr->state = SSD1680_REFRESHING;
}

bool dgx_ssd1680_busy(dgx_screen_t *_scr)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)_scr;
    if (scr->state != SSD1680_REFRESHING) return false;
    if (dgx_gpio_get_level(scr->busy)) return true;
    dgx_ssd1680_finish(scr);
    return false;
}

void dgx_ssd1680_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)_scr;
    _scr->dir_x        = dir_x;
    _scr->dir_y        = dir_y;
    _scr->swap_xy      = swap_xy;
    _scr->width        = swap_xy ? scr->module->height : scr->module->width;
    _scr->height       = swap_xy ? scr->module->width : scr->module->height;
    _scr->dirty_left = _scr->dirty_right = _scr->dirty_top = _scr->dirty_bottom = 0;
    dgx_ssd1680_set_area(_scr, 0, (uint16_t)(_scr->width - 1), 0, (uint16_t)(_scr->height - 1));
}

static void dgx_ssd1680_destroy(dgx_screen_t **_pscr)
{
    dgx_ssd1680_t *scr = (dgx_ssd1680_t *)*_pscr;
    if (!scr) return;
    if (scr->bus) dgx_ssd1680_finish(scr); /* a refresh is not cut short: the glass would keep what it had got to */
    free(scr->bw);
    free(scr->red);
    free(scr);
    *_pscr = NULL;
}

dgx_screen_t *dgx_ssd1680_init(dgx_bus_protocols_t *bus, ssd1680_resolution_t resolution, gpio_num_t rst, gpio_num_t busy)
{
    if ((unsigned)resolution >= sizeof(ssd1680_modules) / sizeof(ssd1680_modules[0])) {
        ESP_LOGE(TAG, "unsupported resolution %d", (int)resolution);
        return NULL;
    }
    if (!bus || busy == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "no bus, or no BUSY line");
        return NULL;
    }
    const ssd1680_module_t *m   = &ssd1680_modules[resolution];
    dgx_ssd1680_t          *scr = (dgx_ssd1680_t *)calloc(1, sizeof(dgx_ssd1680_t));
    if (!scr) {
        ESP_LOGE(TAG, "SSD1680 screen structure memory allocation failed");
        return NULL;
    }
    scr->pitch      = (m->width + 7) / 8;
    const size_t sz = (size_t)scr->pitch * m->height;
    /* sent as they are: memory the bus sends from */
    scr->bw  = bus->memory_allocator((uint32_t)sz);
    scr->red = bus->memory_allocator((uint32_t)sz);
    if (!scr->bw || !scr->red) {
        ESP_LOGE(TAG, "no memory for the planes, 2 x %u bytes", (unsigned)sz);
        dgx_ssd1680_destroy((dgx_screen_t **)&scr);
        return NULL;
    }
    memset(scr->bw, 0xff, sz); /* white */
    memset(scr->red, 0x00, sz);
    scr->module     = m;
    scr->rst        = rst;
    scr->busy       = busy;
    /* the functions of a screen; set_area and the rest are this driver's */
    scr->base.screen_name    = "SSD1680";
    scr->base.screen_subtype = DgxVirtualBackScreen;
    scr->base.color_bits     = 8;
    scr->base.rgb_order      = DgxScreenRGB;
    dgx_scr_init_slow_bus_optimized_funcs(&scr->base);
    scr->base.set_area       = dgx_ssd1680_set_area;
    scr->base.write_area     = dgx_ssd1680_write_area;
    scr->base.wait_buffer    = dgx_ssd1680_wait_buffer;
    scr->base.set_pixel      = dgx_ssd1680_set_pixel;
    scr->base.get_pixel      = dgx_ssd1680_get_pixel;
    scr->base.fill_rectangle = dgx_ssd1680_fill_rectangle;
    scr->base.update_screen  = dgx_ssd1680_update_screen;
    scr->base.destroy        = dgx_ssd1680_destroy;
    dgx_ssd1680_orientation(&scr->base, DgxScreenLeftRight, DgxScreenTopBottom, false);
    if (rst != GPIO_NUM_NC) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        dgx_gpio_set_level(rst, 1);
    }
    dgx_gpio_set_direction(busy, GPIO_MODE_INPUT);
    scr->bus = bus;
    dgx_ssd1680_start(scr);
    return (dgx_screen_t *)scr;
}
