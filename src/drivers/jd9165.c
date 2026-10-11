#include <stdlib.h>

#include "bus/dgx_dsi_esp32.h"
#include "dgx_arch.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/jd9165.h"

// JD9165 commands used here; the window and the memory are those of the MIPI display command set
#define JD9165_SLPOUT 0x11
#define JD9165_DISPON 0x29
#define JD9165_CASET  0x2A
#define JD9165_RASET  0x2B
#define JD9165_RAMWR  0x2C
#define JD9165_RAMRD  0x2E
#define JD9165_PAGE   0x30 // the page of registers the commands that follow belong to
#define JD9165_MADCTL 0x36
#define JD9165_COLMOD 0x3A

#define JD9165_MADCTL_BGR 0x08

#define JD9165_WIDTH  1024
#define JD9165_HEIGHT 600

typedef struct _dgx_jd9165_t {
    dgx_screen_with_bus_t base;
    gpio_num_t            rst;
} dgx_jd9165_t;

static const char TAG[] = "DGX JD9165";

// 1024x600 at 60 frames a second
static const dgx_dsi_video_t jd9165_video = {
    .width             = JD9165_WIDTH,
    .height            = JD9165_HEIGHT,
    .pixel_clock_khz   = 52000,
    .hsync_pulse_width = 24,
    .hsync_back_porch  = 136,
    .hsync_front_porch = 160,
    .vsync_pulse_width = 2,
    .vsync_back_porch  = 21,
    .vsync_front_porch = 12,
};

static uint8_t *dgx_jd9165_adj_madctl(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];
    adj_data[0] = scr->scr.rgb_order == DgxScreenBGR ? JD9165_MADCTL_BGR : 0x00;
    return adj_data;
}

static uint8_t *dgx_jd9165_adj_colmod(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];
    adj_data[0] = scr->scr.color_bits == 24 ? 0x77 : 0x55;
    return adj_data;
}

/*
 * The commands of the 7-inch 1024x600 panel of the Guition JC1060P470 board,
 * as the firmware the board comes with sends them. Command 0x30 chooses a
 * page of registers; a command of the display command set means itself only
 * on page 0, so the last ones come after the page is set back.
 */
static const dgx_lcd_init_cmd_t jd9165_init_cmds[] = {
    {JD9165_MADCTL, dgx_jd9165_adj_madctl, 1, 0, {0x00}}, // 00 - RGB, 08 - BGR
    {JD9165_PAGE, 0, 1, 0, {0x00}}, //
    {0xF7, 0, 4, 0, {0x49, 0x61, 0x02, 0x00}}, //
    {JD9165_PAGE, 0, 1, 0, {0x01}}, //
    {0x04, 0, 1, 0, {0x0C}}, //
    {0x05, 0, 1, 0, {0x08}}, //
    {0x0B, 0, 1, 0, {0x11}}, //
    {0x20, 0, 1, 0, {0x04}}, //
    {0x1F, 0, 1, 0, {0x05}}, //
    {0x23, 0, 1, 0, {0x38}}, //
    {0x28, 0, 1, 0, {0x18}}, //
    {0x29, 0, 1, 0, {0x29}}, //
    {0x2A, 0, 1, 0, {0x01}}, //
    {0x2B, 0, 1, 0, {0x29}}, //
    {0x2C, 0, 1, 0, {0x01}}, //
    {JD9165_PAGE, 0, 1, 0, {0x02}}, //
    {0x00, 0, 1, 0, {0x05}}, //
    {0x01, 0, 1, 0, {0x22}}, //
    {0x02, 0, 1, 0, {0x08}}, //
    {0x03, 0, 1, 0, {0x12}}, //
    {0x04, 0, 1, 0, {0x16}}, //
    {0x05, 0, 1, 0, {0x64}}, //
    {0x06, 0, 1, 0, {0x00}}, //
    {0x07, 0, 1, 0, {0x00}}, //
    {0x08, 0, 1, 0, {0x78}}, //
    {0x09, 0, 1, 0, {0x00}}, //
    {0x0A, 0, 1, 0, {0x04}}, //
    {0x0B, 0, 11, 0, {0x16, 0x17, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x07, 0x09}}, //
    {0x0C, 0, 11, 0, {0x09, 0x1E, 0x1E, 0x1C, 0x1C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}}, //
    {0x0D, 0, 11, 0, {0x0A, 0x05, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x06, 0x08}}, //
    {0x0E, 0, 11, 0, {0x08, 0x1F, 0x1F, 0x1D, 0x1D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}}, //
    {0x0F, 0, 11, 0, {0x0A, 0x05, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1D, 0x1D, 0x1F}}, //
    {0x10, 0, 11, 0, {0x1F, 0x08, 0x08, 0x06, 0x06, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}}, //
    {0x11, 0, 11, 0, {0x16, 0x17, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1C, 0x1C, 0x1E}}, //
    {0x12, 0, 11, 0, {0x1E, 0x09, 0x09, 0x07, 0x07, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}}, //
    {0x13, 0, 4, 0, {0x00, 0x00, 0x00, 0x00}}, //
    {0x14, 0, 4, 0, {0x00, 0x00, 0x41, 0x41}}, //
    {0x15, 0, 4, 0, {0x00, 0x00, 0x00, 0x00}}, //
    {0x17, 0, 1, 0, {0x00}}, //
    {0x18, 0, 1, 0, {0x85}}, //
    {0x19, 0, 2, 0, {0x06, 0x09}}, //
    {0x1A, 0, 2, 0, {0x05, 0x08}}, //
    {0x1B, 0, 2, 0, {0x0A, 0x04}}, //
    {0x26, 0, 1, 0, {0x00}}, //
    {0x27, 0, 1, 0, {0x00}}, //
    {JD9165_PAGE, 0, 1, 0, {0x06}}, //
    {0x12, 0, 14, 0, {0x3F, 0x25, 0x27, 0x35, 0x1D, 0x1B, 0x1B, 0x1A, 0x18, 0x0A, 0x2A, 0x21, 0x19, 0x30}}, //
    {0x13, 0, 14, 0, {0x3F, 0x26, 0x27, 0x35, 0x1E, 0x1C, 0x1C, 0x1A, 0x18, 0x0B, 0x2A, 0x21, 0x19, 0x30}}, //
    {JD9165_PAGE, 0, 1, 0, {0x0A}}, //
    {0x02, 0, 1, 0, {0x4F}}, //
    {0x0B, 0, 1, 0, {0x40}}, //
    {JD9165_PAGE, 0, 1, 0, {0x0D}}, //
    {0x0D, 0, 1, 0, {0x04}}, //
    {0x10, 0, 1, 0, {0x05}}, //
    {0x11, 0, 1, 0, {0x0C}}, //
    {0x12, 0, 1, 0, {0x05}}, //
    {0x13, 0, 1, 0, {0x0C}}, //
    {JD9165_PAGE, 0, 1, 0, {0x00}}, //
    {JD9165_COLMOD, dgx_jd9165_adj_colmod, 1, 0, {0x55}}, // 0x55 - 16 bits a pixel, 0x77 - 24
    {JD9165_SLPOUT, 0, 1, 120, {0x00}}, //
    {JD9165_DISPON, 0, 1, 20, {0x00}}, //
    {0, 0, 0xff, 0, {0}} // Stop
};

// Initialize the display
dgx_screen_t *dgx_jd9165_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo, dgx_dsi_frames_t frames)
{
    if (!bus || bus->bus_type != DGX_BUS_DSI) {
        ESP_LOGE(TAG, "the display needs a bus made by dgx_dsi_init()");
        return NULL;
    }
    dgx_jd9165_t *scr = (dgx_jd9165_t *)calloc(1, sizeof(dgx_jd9165_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }
    scr->rst                      = rst;
    scr->base.bus                 = bus;
    scr->base.scr.cg_row_shift    = 0;
    scr->base.scr.cg_col_shift    = 0;
    scr->base.scr.width           = JD9165_WIDTH;
    scr->base.scr.height          = JD9165_HEIGHT;
    scr->base.scr.color_bits      = color_bits;
    scr->base.scr.rgb_order       = cbo;
    scr->base.scr.screen_name     = "JD9165";
    scr->base.scr.screen_submodel = 0;
    scr->base.scr.screen_subtype  = DgxPhysicalScreenWithBus;
    dgx_screen_with_bus_init_area(&scr->base, JD9165_CASET, JD9165_RASET, JD9165_RAMWR, JD9165_RAMRD, DGX_SCREEN_AREA_PROTO_STD16);
    // the frame buffer first: it is black until something is drawn. The display itself knows the order of colors.
    if (!dgx_dsi_video_setup(bus, &jd9165_video, color_bits, false, frames)) {
        free(scr);
        return NULL;
    }
    if (rst >= 0) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        // Reset the display
        dgx_gpio_set_level(rst, 1);
        dgx_delay(5);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(120);
    }
    // Send all the commands, then the picture
    dgx_lcd_init(&scr->base, jd9165_init_cmds);
    if (!dgx_dsi_video_start(bus)) {
        free(scr);
        return NULL;
    }
    // the drawing functions of a screen that has its pixels in memory
    dgx_dsi_screen_functions((dgx_screen_t *)scr);
    return (dgx_screen_t *)scr;
}

void dgx_jd9165_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_jd9165_t *scr = (dgx_jd9165_t *)_scr;
    _scr->dir_x       = dir_x;
    _scr->dir_y       = dir_y;
    _scr->swap_xy     = swap_xy;
    _scr->width       = swap_xy ? JD9165_HEIGHT : JD9165_WIDTH;
    _scr->height      = swap_xy ? JD9165_WIDTH : JD9165_HEIGHT;
    dgx_dsi_orientation(scr->base.bus, dir_x == DgxScreenRightLeft, dir_y == DgxScreenBottomTop, swap_xy);
    // the bus has set the window to the whole screen
    scr->base.cached_area.left = scr->base.cached_area.right = 0xffff;
    scr->base.cached_area.top = scr->base.cached_area.bottom = 0xffff;
}
