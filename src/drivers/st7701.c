#include <stdlib.h>

#include "bus/dgx_dsi_esp32.h"
#include "dgx_arch.h"
#include "drivers/dgx_lcd_init.h"
#include "dgx_backlight_priv.h"
#include "drivers/st7701.h"

// ST7701S commands used here; the window and the memory are those of the MIPI display command set
#define ST7701_SLPOUT  0x11
#define ST7701_DISPOFF 0x28
#define ST7701_DISPON  0x29
#define ST7701_CASET   0x2A
#define ST7701_RASET   0x2B
#define ST7701_RAMWR   0x2C
#define ST7701_MADCTL  0x36
#define ST7701_COLMOD  0x3A
#define ST7701_BKSEL   0xFF // 77 01 00 00 and the bank: 0x00 the display command set, 0x10 bank 0, 0x11 bank 1, 0x13 bank 3

#define ST7701_MADCTL_BGR 0x08

#define ST7701_WIDTH  480
#define ST7701_HEIGHT 800

typedef struct _dgx_st7701_t {
    dgx_screen_with_bus_t base;
    gpio_num_t            rst;
    gpio_num_t            backlight;
} dgx_st7701_t;

static const char TAG[] = "DGX ST7701";

/*
 * 480x800 with the porches of the board's firmware: 576 pixels a row, 976
 * rows a frame, the long front porch of 166 rows theirs. The firmware asks
 * for 28 MHz, 49.8 frames a second. IDF divides its 240 MHz clock by 9 and
 * gets 26.67; it shortens the front porch at the bridge to keep the rate,
 * and times the row of the DSI host by the clock asked for: 20.568 us at
 * the host against 20.588 at the bridge, the host 20 ns early every row
 * (esp-idf issue 19188). 240/7 MHz is a whole divider: the host and the
 * bridge then have the same row, 16.8 us, 2100 byte clocks of a lane at
 * 1000 Mbit/s or 1575 at 750, and the frame is 16.4 ms, 61 frames a
 * second. The board's video demo asks for 34 MHz and gets this clock.
 */
static const dgx_dsi_video_t st7701_video = {
    .width             = ST7701_WIDTH,
    .height            = ST7701_HEIGHT,
    .pixel_clock_khz   = 34286,
    .hsync_pulse_width = 12,
    .hsync_back_porch  = 42,
    .hsync_front_porch = 42,
    .vsync_pulse_width = 2,
    .vsync_back_porch  = 8,
    .vsync_front_porch = 166,
};

static uint8_t *dgx_st7701_adj_madctl(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];
    adj_data[0] = scr->scr.rgb_order == DgxScreenBGR ? ST7701_MADCTL_BGR : 0x00;
    return adj_data;
}

static uint8_t *dgx_st7701_adj_colmod(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];
    adj_data[0] = scr->scr.color_bits == 24 ? 0x77 : 0x55;
    return adj_data;
}

/*
 * The commands of the 4.3-inch 480x800 panel of the Guition JC4880P443C
 * board, as the firmware the board comes with sends them: the order of
 * colors and the pixel format in the display command set first, then the
 * table of esp_lcd_st7701 1.1.3 for MIPI-DSI, which the firmware keeps as it
 * is. Command 0xFF chooses a bank of registers; the commands of the display
 * command set mean themselves only with no bank chosen.
 */
static const dgx_lcd_init_cmd_t st7701_init_cmds[] = {
    {ST7701_BKSEL, 0, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}}, //
    {ST7701_MADCTL, dgx_st7701_adj_madctl, 1, 0, {0x00}},    // 00 - RGB, 08 - BGR
    {ST7701_COLMOD, dgx_st7701_adj_colmod, 1, 0, {0x55}},    // 0x55 - 16 bits a pixel, 0x77 - 24

    // Bank 3
    {ST7701_BKSEL, 0, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}}, //
    {0xEF, 0, 1, 0, {0x08}},                                 //

    // Bank 0: the display line, porch control, inversion, gamma
    {ST7701_BKSEL, 0, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}}, //
    {0xC0, 0, 2, 0, {0x63, 0x00}},                           //
    {0xC1, 0, 2, 0, {0x0D, 0x02}},                           //
    {0xC2, 0, 2, 0, {0x10, 0x08}},                           //
    {0xCC, 0, 1, 0, {0x10}},                                 //
    {0xB0, 0, 16, 0, {0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}}, // positive gamma
    {0xB1, 0, 16, 0, {0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}}, // negative gamma

    // Bank 1: voltages
    {ST7701_BKSEL, 0, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}}, //
    {0xB0, 0, 1, 0, {0x5D}},                                 //
    {0xB1, 0, 1, 0, {0x58}},                                 //
    {0xB2, 0, 1, 0, {0x87}},                                 //
    {0xB3, 0, 1, 0, {0x80}},                                 //
    {0xB5, 0, 1, 0, {0x4E}},                                 //
    {0xB7, 0, 1, 0, {0x85}},                                 //
    {0xB8, 0, 1, 0, {0x21}},                                 //
    {0xB9, 0, 2, 0, {0x10, 0x1F}},                           //
    {0xBB, 0, 1, 0, {0x03}},                                 //
    {0xBC, 0, 1, 0, {0x00}},                                 //
    {0xC1, 0, 1, 0, {0x78}},                                 //
    {0xC2, 0, 1, 0, {0x78}},                                 //
    {0xD0, 0, 1, 0, {0x88}},                                 //

    // Bank 1: the timing of the gate driver
    {0xE0, 0, 3, 0, {0x00, 0x3A, 0x02}},                                                                           //
    {0xE1, 0, 11, 0, {0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}},                           //
    {0xE2, 0, 13, 0, {0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}},               //
    {0xE3, 0, 4, 0, {0x00, 0x00, 0x33, 0x33}},                                                                     //
    {0xE4, 0, 2, 0, {0x44, 0x44}},                                                                                 //
    {0xE5, 0, 16, 0, {0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}}, //
    {0xE6, 0, 4, 0, {0x00, 0x00, 0x33, 0x33}},                                                                     //
    {0xE7, 0, 2, 0, {0x44, 0x44}},                                                                                 //
    {0xE8, 0, 16, 0, {0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}}, //
    {0xEB, 0, 7, 0, {0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}},                                                   //
    {0xEC, 0, 2, 0, {0x08, 0x01}},                                                                                 //
    {0xED, 0, 16, 0, {0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}}, //
    {0xEF, 0, 6, 0, {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}},                                                         //

    // Back to the display command set
    {ST7701_BKSEL, 0, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}}, //

    // Sleep out
    {ST7701_SLPOUT, 0, 1, 120, {0x00}}, //

    // Display on
    {ST7701_DISPON, 0, 1, 20, {0x00}}, //
    {0, 0, 0xff, 0, {0}}               // Stop
};

static void dgx_st7701_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_st7701_t *scr = (dgx_st7701_t *)_scr;
    _scr->dir_x       = dir_x;
    _scr->dir_y       = dir_y;
    _scr->swap_xy     = swap_xy;
    _scr->width       = swap_xy ? ST7701_HEIGHT : ST7701_WIDTH;
    _scr->height      = swap_xy ? ST7701_WIDTH : ST7701_HEIGHT;
    dgx_screen_unclip(_scr);
    dgx_dsi_orientation(scr->base.bus, dir_x == DgxScreenRightLeft, dir_y == DgxScreenBottomTop, swap_xy);
    // the bus has set the window to the whole screen
    scr->base.cached_area.left = scr->base.cached_area.right = 0xffff;
    scr->base.cached_area.top = scr->base.cached_area.bottom = 0xffff;
}

static esp_err_t dgx_st7701_backlight(dgx_screen_t *scr, bool on)
{
    return dgx_backlight_set(((dgx_st7701_t *)scr)->backlight, on);
}

/*
 * DISPOFF and DISPON go to the controller beside the video, with the one byte the commands of the start have: the
 * glass shows nothing, the frame buffer is kept and is sent on, and the picture is there again at DISPON.
 */
static esp_err_t dgx_st7701_display(dgx_screen_t *_scr, bool on)
{
    dgx_st7701_t *scr  = (dgx_st7701_t *)_scr;
    const uint8_t none = 0;
    scr->base.bus->write_command(scr->base.bus, on ? ST7701_DISPON : ST7701_DISPOFF);
    scr->base.bus->write_data(scr->base.bus, &none, 8);
    return ESP_OK;
}

/* the picture of a DSI display is turned by the bus, in its frame buffer */
static const dgx_screen_control_t dgx_st7701_control = {
    .orientation = dgx_st7701_orientation,
    .backlight   = dgx_st7701_backlight,
    .display     = dgx_st7701_display,
};

// Initialize the display
dgx_screen_t *dgx_st7701_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
                              dgx_dsi_frames_t frames)
{
    if (!bus || bus->bus_type != DGX_BUS_DSI) {
        ESP_LOGE(TAG, "the display needs a bus made by dgx_dsi_init()");
        return NULL;
    }
    dgx_st7701_t *scr = (dgx_st7701_t *)calloc(1, sizeof(dgx_st7701_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }
    scr->rst                      = rst;
    scr->backlight                = backlight;
    scr->base.bus                 = bus;
    scr->base.scr.cg_row_shift    = 0;
    scr->base.scr.cg_col_shift    = 0;
    scr->base.scr.width           = ST7701_WIDTH;
    scr->base.scr.height          = ST7701_HEIGHT;
    scr->base.scr.color_bits      = color_bits;
    scr->base.scr.rgb_order       = cbo;
    scr->base.scr.screen_name     = "ST7701";
    scr->base.scr.screen_submodel = 0;
    scr->base.scr.screen_subtype  = DgxPhysicalScreenWithBus;
    scr->base.scr.control         = &dgx_st7701_control;
    dgx_screen_with_bus_init_area(&scr->base, ST7701_CASET, ST7701_RASET, ST7701_RAMWR, DGX_SCREEN_AREA_PROTO_STD16);
    // the frame buffer first: it is black until something is drawn. The display itself knows the order of colors.
    if (!dgx_dsi_video_setup(bus, &st7701_video, color_bits, false, frames)) {
        free(scr);
        return NULL;
    }
    if (rst >= 0) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        // Reset the display: low 10 ms, then 120 ms before the first command, as esp_lcd_st7701 does
        dgx_gpio_set_level(rst, 1);
        dgx_delay(5);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(120);
    }
    // Send all the commands, then the picture
    dgx_lcd_init(&scr->base, st7701_init_cmds);
    if (!dgx_dsi_video_start(bus)) {
        free(scr);
        return NULL;
    }
    // the drawing functions of a screen that has its pixels in memory
    dgx_dsi_screen_functions((dgx_screen_t *)scr);
    // the frame buffer is black and on the glass
    dgx_backlight_on_start(backlight);
    return (dgx_screen_t *)scr;
}
