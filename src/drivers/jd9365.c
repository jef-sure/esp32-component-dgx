#include <stdlib.h>

#include "bus/dgx_dsi_esp32.h"
#include "dgx_arch.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/jd9365.h"

// JD9365 commands used here; the window and the memory are those of the MIPI display command set
#define JD9365_SLPOUT   0x11
#define JD9365_DISPON   0x29
#define JD9365_CASET    0x2A
#define JD9365_RASET    0x2B
#define JD9365_RAMWR    0x2C
#define JD9365_RAMRD    0x2E
#define JD9365_MADCTL   0x36
#define JD9365_COLMOD   0x3A
#define JD9365_DSI_INT0 0x80 // the number of DSI lanes, on page 0
#define JD9365_PAGE     0xE0 // the page of registers the commands that follow belong to

#define JD9365_MADCTL_BGR 0x08

#define JD9365_WIDTH  800
#define JD9365_HEIGHT 1280

typedef struct _dgx_jd9365_t {
    dgx_screen_with_bus_t base;
    gpio_num_t            rst;
} dgx_jd9365_t;

static const char TAG[] = "DGX JD9365";

/*
 * 800x1280 at 60 frames a second. The firmware of the board asks for 70 MHz
 * with a front porch of 40; IDF divides its 240 MHz clock by a whole number
 * and gets 80, then stretches the row for the bridge but not for the DSI
 * host, so the host ends every row 7 ns early (esp-idf issue 19188). 80 MHz
 * and a row of 1008 pixels, which is a whole number of byte clocks of a lane
 * at 1000 Mbit/s, give both the same row: 12.6 us, 59.94 frames a second.
 */
static const dgx_dsi_video_t jd9365_video = {
    .width             = JD9365_WIDTH,
    .height            = JD9365_HEIGHT,
    .pixel_clock_khz   = 80000,
    .hsync_pulse_width = 20,
    .hsync_back_porch  = 20,
    .hsync_front_porch = 168,
    .vsync_pulse_width = 4,
    .vsync_back_porch  = 10,
    .vsync_front_porch = 30,
};

static uint8_t *dgx_jd9365_adj_madctl(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];
    adj_data[0] = scr->scr.rgb_order == DgxScreenBGR ? JD9365_MADCTL_BGR : 0x00;
    return adj_data;
}

static uint8_t *dgx_jd9365_adj_colmod(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];
    adj_data[0] = scr->scr.color_bits == 24 ? 0x77 : 0x55;
    return adj_data;
}

/*
 * Bits 1..0 of the register hold the number of lanes minus one: 0x01 for two
 * lanes, 0x03 for four. The Linux driver panel-jadard-jd9365da-h3 writes it
 * so, and so did the init table of the board's own driver before it was
 * edited for two lanes ("03->01"). That driver defines three and four lanes
 * as 0x10 and 0x11, binary numbers written as hex. Only two lanes have been
 * tried on a panel.
 */
static uint8_t *dgx_jd9365_adj_lanes(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static const uint8_t lanes[] = {0x00, 0x01, 0x02, 0x03};
    static uint8_t       adj_data[1];
    uint8_t              n = dgx_dsi_data_lanes(scr->bus);
    adj_data[0]            = n >= 1 && n <= 4 ? lanes[n - 1] : 0x01;
    return adj_data;
}

/*
 * The commands of the 10.1-inch 800x1280 panel of the Guition JC8012P4A1C
 * board, the newer of its two panels, as the firmware the board comes with
 * sends them. Command 0xE0 chooses a page of registers; a command of the
 * display command set means itself only on page 0, so the order of colors,
 * the pixel format and the lanes are set there first.
 */
static const dgx_lcd_init_cmd_t jd9365_init_cmds[] = {
    {JD9365_PAGE, 0, 1, 0, {0x00}}, //
    {JD9365_MADCTL, dgx_jd9365_adj_madctl, 1, 0, {0x00}}, // 00 - RGB, 08 - BGR
    {JD9365_COLMOD, dgx_jd9365_adj_colmod, 1, 0, {0x55}}, // 0x55 - 16 bits a pixel, 0x77 - 24
    {JD9365_DSI_INT0, dgx_jd9365_adj_lanes, 1, 0, {0x01}}, //
    // page 0, then the password that opens the other pages
    {JD9365_PAGE, 0, 1, 0, {0x00}}, //
    {0xE1, 0, 1, 0, {0x93}}, //
    {0xE2, 0, 1, 0, {0x65}}, //
    {0xE3, 0, 1, 0, {0xF8}}, //
    {JD9365_DSI_INT0, dgx_jd9365_adj_lanes, 1, 0, {0x01}}, // the number of lanes less one: 0x01 two lanes, 0x03 four

    // Page 1
    {JD9365_PAGE, 0, 1, 0, {0x01}}, //
    {0x00, 0, 1, 0, {0x00}}, //
    {0x01, 0, 1, 0, {0x44}}, //
    {0x03, 0, 1, 0, {0x10}}, //
    {0x04, 0, 1, 0, {0x38}}, //

    {0x0C, 0, 1, 0, {0x74}}, //

    {0x17, 0, 1, 0, {0x00}}, //
    {0x18, 0, 1, 0, {0xAF}}, //
    {0x19, 0, 1, 0, {0x00}}, //
    {0x1A, 0, 1, 0, {0x00}}, //
    {0x1B, 0, 1, 0, {0xAF}}, //
    {0x1C, 0, 1, 0, {0x00}}, //

    {0x35, 0, 1, 0, {0x26}}, //

    {0x37, 0, 1, 0, {0x09}}, //

    {0x38, 0, 1, 0, {0x04}}, //
    {0x39, 0, 1, 0, {0x00}}, //
    {0x3A, 0, 1, 0, {0x01}}, //
    {0x3C, 0, 1, 0, {0x78}}, //
    {0x3D, 0, 1, 0, {0xFF}}, //
    {0x3E, 0, 1, 0, {0xFF}}, //
    {0x3F, 0, 1, 0, {0x7F}}, //

    {0x40, 0, 1, 0, {0x06}}, //
    {0x41, 0, 1, 0, {0xA0}}, //
    {0x42, 0, 1, 0, {0x81}}, //
    {0x43, 0, 1, 0, {0x1E}}, //
    {0x44, 0, 1, 0, {0x0D}}, //
    {0x45, 0, 1, 0, {0x28}}, //

    {0x55, 0, 1, 0, {0x02}}, //
    {0x57, 0, 1, 0, {0x69}}, //
    {0x59, 0, 1, 0, {0x0A}}, //
    {0x5A, 0, 1, 0, {0x2A}}, //
    {0x5B, 0, 1, 0, {0x17}}, //

    // G2.2 / G2.5 - Positive Gamma
    {0x5D, 0, 1, 0, {0x7F}}, //
    {0x5E, 0, 1, 0, {0x6B}}, //
    {0x5F, 0, 1, 0, {0x5C}}, //
    {0x60, 0, 1, 0, {0x50}}, //
    {0x61, 0, 1, 0, {0x4C}}, //
    {0x62, 0, 1, 0, {0x3E}}, //
    {0x63, 0, 1, 0, {0x41}}, //
    {0x64, 0, 1, 0, {0x2B}}, //
    {0x65, 0, 1, 0, {0x43}}, //
    {0x66, 0, 1, 0, {0x42}}, //
    {0x67, 0, 1, 0, {0x43}}, //
    {0x68, 0, 1, 0, {0x62}}, //
    {0x69, 0, 1, 0, {0x52}}, //
    {0x6A, 0, 1, 0, {0x5A}}, //
    {0x6B, 0, 1, 0, {0x4C}}, //
    {0x6C, 0, 1, 0, {0x48}}, //
    {0x6D, 0, 1, 0, {0x3A}}, //
    {0x6E, 0, 1, 0, {0x28}}, //
    {0x6F, 0, 1, 0, {0x10}}, //

    // Negative Gamma
    {0x70, 0, 1, 0, {0x7F}}, //
    {0x71, 0, 1, 0, {0x6B}}, //
    {0x72, 0, 1, 0, {0x5C}}, //
    {0x73, 0, 1, 0, {0x50}}, //
    {0x74, 0, 1, 0, {0x4C}}, //
    {0x75, 0, 1, 0, {0x3E}}, //
    {0x76, 0, 1, 0, {0x41}}, //
    {0x77, 0, 1, 0, {0x2B}}, //
    {0x78, 0, 1, 0, {0x43}}, //
    {0x79, 0, 1, 0, {0x42}}, //
    {0x7A, 0, 1, 0, {0x43}}, //
    {0x7B, 0, 1, 0, {0x62}}, //
    {0x7C, 0, 1, 0, {0x52}}, //
    {0x7D, 0, 1, 0, {0x5A}}, //
    {0x7E, 0, 1, 0, {0x4C}}, //
    {0x7F, 0, 1, 0, {0x48}}, //
    {0x80, 0, 1, 0, {0x3A}}, //
    {0x81, 0, 1, 0, {0x28}}, //
    {0x82, 0, 1, 0, {0x10}}, //

    // Page 2
    {JD9365_PAGE, 0, 1, 0, {0x02}}, //
    {0x00, 0, 1, 0, {0x42}}, //
    {0x01, 0, 1, 0, {0x42}}, //
    {0x02, 0, 1, 0, {0x40}}, //
    {0x03, 0, 1, 0, {0x40}}, //
    {0x04, 0, 1, 0, {0x5E}}, //
    {0x05, 0, 1, 0, {0x5E}}, //
    {0x06, 0, 1, 0, {0x5F}}, //
    {0x07, 0, 1, 0, {0x5F}}, //
    {0x08, 0, 1, 0, {0x5F}}, //
    {0x09, 0, 1, 0, {0x57}}, //
    {0x0A, 0, 1, 0, {0x57}}, //
    {0x0B, 0, 1, 0, {0x77}}, //
    {0x0C, 0, 1, 0, {0x77}}, //
    {0x0D, 0, 1, 0, {0x47}}, //
    {0x0E, 0, 1, 0, {0x47}}, //
    {0x0F, 0, 1, 0, {0x45}}, //
    {0x10, 0, 1, 0, {0x45}}, //
    {0x11, 0, 1, 0, {0x4B}}, //
    {0x12, 0, 1, 0, {0x4B}}, //
    {0x13, 0, 1, 0, {0x49}}, //
    {0x14, 0, 1, 0, {0x49}}, //
    {0x15, 0, 1, 0, {0x5F}}, //

    {0x16, 0, 1, 0, {0x41}}, //
    {0x17, 0, 1, 0, {0x41}}, //
    {0x18, 0, 1, 0, {0x40}}, //
    {0x19, 0, 1, 0, {0x40}}, //
    {0x1A, 0, 1, 0, {0x5E}}, //
    {0x1B, 0, 1, 0, {0x5E}}, //
    {0x1C, 0, 1, 0, {0x5F}}, //
    {0x1D, 0, 1, 0, {0x5F}}, //
    {0x1E, 0, 1, 0, {0x5F}}, //
    {0x1F, 0, 1, 0, {0x57}}, //
    {0x20, 0, 1, 0, {0x57}}, //
    {0x21, 0, 1, 0, {0x77}}, //
    {0x22, 0, 1, 0, {0x77}}, //
    {0x23, 0, 1, 0, {0x46}}, //
    {0x24, 0, 1, 0, {0x46}}, //
    {0x25, 0, 1, 0, {0x44}}, //
    {0x26, 0, 1, 0, {0x44}}, //
    {0x27, 0, 1, 0, {0x4A}}, //
    {0x28, 0, 1, 0, {0x4A}}, //
    {0x29, 0, 1, 0, {0x48}}, //
    {0x2A, 0, 1, 0, {0x48}}, //
    {0x2B, 0, 1, 0, {0x5F}}, //

    {0x2C, 0, 1, 0, {0x01}}, //
    {0x2D, 0, 1, 0, {0x01}}, //
    {0x2E, 0, 1, 0, {0x00}}, //
    {0x2F, 0, 1, 0, {0x00}}, //
    {0x30, 0, 1, 0, {0x1F}}, //
    {0x31, 0, 1, 0, {0x1F}}, //
    {0x32, 0, 1, 0, {0x1E}}, //
    {0x33, 0, 1, 0, {0x1E}}, //
    {0x34, 0, 1, 0, {0x1F}}, //
    {0x35, 0, 1, 0, {0x17}}, //
    {0x36, 0, 1, 0, {0x17}}, //
    {0x37, 0, 1, 0, {0x37}}, //
    {0x38, 0, 1, 0, {0x37}}, //
    {0x39, 0, 1, 0, {0x08}}, //
    {0x3A, 0, 1, 0, {0x08}}, //
    {0x3B, 0, 1, 0, {0x0A}}, //
    {0x3C, 0, 1, 0, {0x0A}}, //
    {0x3D, 0, 1, 0, {0x04}}, //
    {0x3E, 0, 1, 0, {0x04}}, //
    {0x3F, 0, 1, 0, {0x06}}, //
    {0x40, 0, 1, 0, {0x06}}, //
    {0x41, 0, 1, 0, {0x1F}}, //

    {0x42, 0, 1, 0, {0x02}}, //
    {0x43, 0, 1, 0, {0x02}}, //
    {0x44, 0, 1, 0, {0x00}}, //
    {0x45, 0, 1, 0, {0x00}}, //
    {0x46, 0, 1, 0, {0x1F}}, //
    {0x47, 0, 1, 0, {0x1F}}, //
    {0x48, 0, 1, 0, {0x1E}}, //
    {0x49, 0, 1, 0, {0x1E}}, //
    {0x4A, 0, 1, 0, {0x1F}}, //
    {0x4B, 0, 1, 0, {0x17}}, //
    {0x4C, 0, 1, 0, {0x17}}, //
    {0x4D, 0, 1, 0, {0x37}}, //
    {0x4E, 0, 1, 0, {0x37}}, //
    {0x4F, 0, 1, 0, {0x09}}, //
    {0x50, 0, 1, 0, {0x09}}, //
    {0x51, 0, 1, 0, {0x0B}}, //
    {0x52, 0, 1, 0, {0x0B}}, //
    {0x53, 0, 1, 0, {0x05}}, //
    {0x54, 0, 1, 0, {0x05}}, //
    {0x55, 0, 1, 0, {0x07}}, //
    {0x56, 0, 1, 0, {0x07}}, //
    {0x57, 0, 1, 0, {0x1F}}, //

    {0x58, 0, 1, 0, {0x40}}, //
    {0x5B, 0, 1, 0, {0x30}}, //
    {0x5C, 0, 1, 0, {0x00}}, //
    {0x5D, 0, 1, 0, {0x34}}, //
    {0x5E, 0, 1, 0, {0x05}}, //
    {0x5F, 0, 1, 0, {0x02}}, //
    {0x63, 0, 1, 0, {0x00}}, //
    {0x64, 0, 1, 0, {0x6A}}, //
    {0x67, 0, 1, 0, {0x73}}, //
    {0x68, 0, 1, 0, {0x07}}, //
    {0x69, 0, 1, 0, {0x08}}, //
    {0x6A, 0, 1, 0, {0x6A}}, //
    {0x6B, 0, 1, 0, {0x08}}, //

    {0x6C, 0, 1, 0, {0x00}}, //
    {0x6D, 0, 1, 0, {0x00}}, //
    {0x6E, 0, 1, 0, {0x00}}, //
    {0x6F, 0, 1, 0, {0x88}}, //

    {0x75, 0, 1, 0, {0xFF}}, //
    {0x77, 0, 1, 0, {0xDD}}, //
    {0x78, 0, 1, 0, {0x2C}}, //
    {0x79, 0, 1, 0, {0x15}}, //
    {0x7A, 0, 1, 0, {0x17}}, //
    {0x7D, 0, 1, 0, {0x14}}, //
    {0x7E, 0, 1, 0, {0x82}}, //

    // Page 4
    {JD9365_PAGE, 0, 1, 0, {0x04}}, //
    {0x00, 0, 1, 0, {0x0E}}, //
    {0x02, 0, 1, 0, {0xB3}}, //
    {0x09, 0, 1, 0, {0x60}}, //
    {0x0E, 0, 1, 0, {0x48}}, //
    {0x37, 0, 1, 0, {0x58}}, //
    {0x2B, 0, 1, 0, {0x0F}}, //

    // Page 5
    {JD9365_PAGE, 0, 1, 0, {0x05}}, //
    {0x15, 0, 1, 0, {0x1D}}, //

    // Back to page 0
    {JD9365_PAGE, 0, 1, 0, {0x00}}, //
    {0xE6, 0, 1, 0, {0x02}}, //
    {0xE7, 0, 1, 0, {0x0C}}, //

    // Sleep out
    {JD9365_SLPOUT, 0, 1, 120, {0x00}}, //

    // Display on
    {JD9365_DISPON, 0, 1, 20, {0x00}}, //
    {0, 0, 0xff, 0, {0}} // Stop
};

// Initialize the display
dgx_screen_t *dgx_jd9365_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo, dgx_dsi_frames_t frames)
{
    if (!bus || bus->bus_type != DGX_BUS_DSI) {
        ESP_LOGE(TAG, "the display needs a bus made by dgx_dsi_init()");
        return NULL;
    }
    dgx_jd9365_t *scr = (dgx_jd9365_t *)calloc(1, sizeof(dgx_jd9365_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }
    scr->rst                      = rst;
    scr->base.bus                 = bus;
    scr->base.scr.cg_row_shift    = 0;
    scr->base.scr.cg_col_shift    = 0;
    scr->base.scr.width           = JD9365_WIDTH;
    scr->base.scr.height          = JD9365_HEIGHT;
    scr->base.scr.color_bits      = color_bits;
    scr->base.scr.rgb_order       = cbo;
    scr->base.scr.screen_name     = "JD9365";
    scr->base.scr.screen_submodel = 0;
    scr->base.scr.screen_subtype  = DgxPhysicalScreenWithBus;
    dgx_screen_with_bus_init_area(&scr->base, JD9365_CASET, JD9365_RASET, JD9365_RAMWR, JD9365_RAMRD, DGX_SCREEN_AREA_PROTO_STD16);
    // the frame buffer first: it is black until something is drawn. The display itself knows the order of colors.
    if (!dgx_dsi_video_setup(bus, &jd9365_video, color_bits, false, frames)) {
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
    dgx_lcd_init(&scr->base, jd9365_init_cmds);
    if (!dgx_dsi_video_start(bus)) {
        free(scr);
        return NULL;
    }
    // the drawing functions of a screen that has its pixels in memory
    dgx_dsi_screen_functions((dgx_screen_t *)scr);
    return (dgx_screen_t *)scr;
}

void dgx_jd9365_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_jd9365_t *scr = (dgx_jd9365_t *)_scr;
    _scr->dir_x       = dir_x;
    _scr->dir_y       = dir_y;
    _scr->swap_xy     = swap_xy;
    _scr->width       = swap_xy ? JD9365_HEIGHT : JD9365_WIDTH;
    _scr->height      = swap_xy ? JD9365_WIDTH : JD9365_HEIGHT;
    dgx_dsi_orientation(scr->base.bus, dir_x == DgxScreenRightLeft, dir_y == DgxScreenBottomTop, swap_xy);
    // the bus has set the window to the whole screen
    scr->base.cached_area.left = scr->base.cached_area.right = 0xffff;
    scr->base.cached_area.top = scr->base.cached_area.bottom = 0xffff;
}
