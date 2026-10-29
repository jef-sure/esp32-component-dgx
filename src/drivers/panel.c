/*
 * panel.c
 *
 *  A panel of the MIPI display command set made from its description, see
 *  drivers/dgx_panel.h.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include "drivers/dgx_panel.h"

#include <stdio.h>
#include <stdlib.h>

#include "dgx_arch.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "dgx_spi_area_protocol.h"
#include "esp_log.h"

static const char TAG[] = "DGX PANEL";

#define DGX_PANEL_CASET  0x2Au
#define DGX_PANEL_RASET  0x2Bu
#define DGX_PANEL_RAMWR  0x2Cu
#define DGX_PANEL_MADCTL 0x36u

#define DGX_PANEL_MY  0x80u
#define DGX_PANEL_MX  0x40u
#define DGX_PANEL_MV  0x20u
#define DGX_PANEL_BGR 0x08u

typedef struct dgx_panel_screen_t {
    dgx_screen_with_bus_t base;
    const dgx_panel_t    *panel;
    gpio_num_t            rst;
    gpio_num_t            backlight;
} dgx_panel_screen_t;

/* the place of a depth in colmod[]: 12, 16 and 18 bits, -1 for any other */
static int dgx_panel_depth(uint8_t color_bits)
{
    switch (color_bits) {
    case 12:
        return 0;
    case 16:
        return 1;
    case 18:
        return 2;
    default:
        return -1;
    }
}

/* MADCTL of the screen as it lies: the upright one with its axes turned and the order of its colors */
static uint8_t dgx_panel_madctl(const dgx_screen_with_bus_t *scr)
{
    uint8_t madctl = ((const dgx_panel_screen_t *)scr)->panel->madctl;
    if (scr->scr.dir_x == DgxScreenRightLeft) madctl ^= DGX_PANEL_MX;
    if (scr->scr.dir_y == DgxScreenBottomTop) madctl ^= DGX_PANEL_MY;
    if (scr->scr.swap_xy) madctl |= DGX_PANEL_MV;
    if (scr->scr.rgb_order == DgxScreenBGR) madctl ^= DGX_PANEL_BGR;
    return madctl;
}

/*
 * Where the glass lies in the memory of the controller as the screen lies: a
 * mirrored axis counts the memory from its other end, so a glass that does
 * not fill it moves along it. With the axes swapped the columns of the
 * screen go along the rows of the memory.
 */
static void dgx_panel_shifts(dgx_screen_with_bus_t *scr)
{
    const dgx_panel_t *panel = ((dgx_panel_screen_t *)scr)->panel;
    const int          mw    = panel->memory_width ? panel->memory_width : panel->width;
    const int          mh    = panel->memory_height ? panel->memory_height : panel->height;
    const int          col   = scr->scr.dir_x == DgxScreenRightLeft ? mw - panel->width - panel->x_offset : panel->x_offset;
    const int          row   = scr->scr.dir_y == DgxScreenBottomTop ? mh - panel->height - panel->y_offset : panel->y_offset;
    scr->scr.cg_col_shift    = scr->scr.swap_xy ? row : col;
    scr->scr.cg_row_shift    = scr->scr.swap_xy ? col : row;
}

uint8_t *dgx_panel_adj_madctl(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t data[1];
    (void)init_cmd;
    data[0] = dgx_panel_madctl(scr);
    return data;
}

uint8_t *dgx_panel_adj_colmod(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t data[1];
    const int      depth = dgx_panel_depth(scr->scr.color_bits);
    data[0]              = depth < 0 ? init_cmd->data[0] : ((const dgx_panel_screen_t *)scr)->panel->colmod[depth];
    return data;
}

/* the set_area of every display behind a bus, but with both bounds of the window sent every time: DGX_PANEL_BOTH_BOUNDS */
static void dgx_panel_set_area_both(dgx_screen_t *_scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    dgx_screen_with_bus_t *scr = (dgx_screen_with_bus_t *)_scr;
    if (left > right) {
        uint16_t tmp = left;
        left         = right;
        right        = tmp;
    }
    if (top > bottom) {
        uint16_t tmp = top;
        top          = bottom;
        bottom       = tmp;
    }
    /* a window off the screen is no window: what is sent into it must not land in the one before */
    scr->area_void = left >= _scr->width || top >= _scr->height;
    if (scr->area_void) return;
    if (right > _scr->width - 1) right = _scr->width - 1;
    if (bottom > _scr->height - 1) bottom = _scr->height - 1;
    left += _scr->cg_col_shift;
    right += _scr->cg_col_shift;
    top += _scr->cg_row_shift;
    bottom += _scr->cg_row_shift;
    dgx_screen_with_bus_set_area_window_full(scr, left, right, top, bottom);
}

/* the depths a panel takes, for the log: "16 and 18" */
static void dgx_panel_depths(const dgx_panel_t *panel, char *text, size_t size)
{
    static const int bits[3] = {12, 16, 18};
    int              taken[3], n = 0;
    for (int i = 0; i < 3; ++i) {
        if (panel->colmod[i]) taken[n++] = bits[i];
    }
    if (n == 1) snprintf(text, size, "%d", taken[0]);
    else if (n == 2) snprintf(text, size, "%d and %d", taken[0], taken[1]);
    else if (n == 3) snprintf(text, size, "%d, %d and %d", taken[0], taken[1], taken[2]);
    else snprintf(text, size, "none");
}

dgx_screen_t *dgx_panel_init(dgx_bus_protocols_t *bus, const dgx_panel_t *panel, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits,
                             dgx_color_order_t cbo)
{
    if (!bus) {
        ESP_LOGE(TAG, "%s: the bus of the display is not initialized", panel->name);
        return NULL;
    }
    const int depth = dgx_panel_depth(color_bits);
    if (depth < 0 || !panel->colmod[depth]) {
        char depths[24];
        dgx_panel_depths(panel, depths, sizeof(depths));
        ESP_LOGE(TAG, "%s: %u-bit color is not set up for this panel, only %s", panel->name, (unsigned)color_bits, depths);
        return NULL;
    }
    dgx_panel_screen_t *scr = (dgx_panel_screen_t *)calloc(1, sizeof(dgx_panel_screen_t));
    if (!scr) {
        ESP_LOGE(TAG, "%s: screen structure memory allocation failed", panel->name);
        return NULL;
    }
    dgx_screen_t *s       = &scr->base.scr;
    scr->panel            = panel;
    scr->rst              = rst;
    scr->backlight        = backlight;
    scr->base.bus         = bus;
    s->width              = panel->width;
    s->height             = panel->height;
    s->dir_x              = DgxScreenLeftRight;
    s->dir_y              = DgxScreenTopBottom;
    s->swap_xy            = false;
    s->color_bits         = color_bits;
    s->rgb_order          = cbo;
    s->screen_name        = panel->name;
    s->screen_submodel    = 0;
    s->screen_subtype     = DgxPhysicalScreenWithBus;
    dgx_panel_shifts(&scr->base);
    dgx_screen_with_bus_init_area(&scr->base, DGX_PANEL_CASET, DGX_PANEL_RASET, DGX_PANEL_RAMWR, panel->ramrd, DGX_SCREEN_AREA_PROTO_STD16);
    if (panel->scroll_lines) dgx_screen_with_bus_scroll_init(&scr->base, panel->scroll_lines, panel->scroll_area_cmd, panel->scroll_start_cmd);
    const bool clear_first = panel->flags & DGX_PANEL_CLEAR_FIRST;
    if (backlight >= 0 && clear_first) {
        /* dark until there is a picture */
        dgx_gpio_set_direction(backlight, GPIO_MODE_OUTPUT);
        dgx_panel_backlight(s, false);
    }
    if (rst >= 0) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        if (panel->reset_high_ms) {
            dgx_gpio_set_level(rst, 1);
            dgx_delay(panel->reset_high_ms);
        }
        dgx_gpio_set_level(rst, 0);
        dgx_delay(panel->reset_low_ms);
        dgx_gpio_set_level(rst, 1);
        if (panel->reset_after_ms) dgx_delay(panel->reset_after_ms);
    }
    dgx_lcd_init(&scr->base, panel->init);
    if (backlight >= 0 && !clear_first) {
        dgx_gpio_set_direction(backlight, GPIO_MODE_OUTPUT);
        dgx_panel_backlight(s, true);
    }
    dgx_scr_init_slow_bus_optimized_funcs(s);
    if (panel->flags & DGX_PANEL_BOTH_BOUNDS) s->set_area = dgx_panel_set_area_both;
    if (clear_first) {
        dgx_fill_rectangle(s, 0, 0, panel->width, panel->height, 0);
        s->wait_buffer(s);
        if (panel->on) dgx_lcd_init(&scr->base, panel->on);
        if (backlight >= 0) dgx_panel_backlight(s, true);
    }
    return s;
}

void dgx_panel_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_panel_screen_t *scr   = (dgx_panel_screen_t *)_scr;
    const dgx_panel_t  *panel = scr->panel;
    _scr->dir_x               = dir_x;
    _scr->dir_y               = dir_y;
    _scr->swap_xy             = swap_xy;
    _scr->width               = swap_xy ? panel->height : panel->width;
    _scr->height              = swap_xy ? panel->width : panel->height;
    dgx_panel_shifts(&scr->base);
    const uint8_t madctl[1] = {dgx_panel_madctl(&scr->base)};
    scr->base.bus->write_command(scr->base.bus, DGX_PANEL_MADCTL);
    scr->base.bus->write_data(scr->base.bus, madctl, 8);
    /*
     * The rows of memory are the rows of the screen, or its columns when the
     * axes are swapped; MY turns the addresses along them round whichever
     * they are.
     */
    if (panel->scroll_lines) dgx_screen_with_bus_scroll_way(&scr->base, swap_xy, dir_y == DgxScreenBottomTop);
}

esp_err_t dgx_panel_backlight(dgx_screen_t *_scr, bool on)
{
    dgx_panel_screen_t *scr = (dgx_panel_screen_t *)_scr;
    if (scr->backlight < 0) return ESP_ERR_NOT_SUPPORTED;
    dgx_gpio_set_level(scr->backlight, on ? 1 : 0);
    return ESP_OK;
}
