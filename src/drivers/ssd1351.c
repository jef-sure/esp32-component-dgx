#include "drivers/ssd1351.h"

#include <string.h>

#include "dgx_arch.h"
#include "dgx_screen_functions.h"
#include "drivers/dgx_lcd_init.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SSD1351_CMD_SETCOLUMN 0x15       ///< See datasheet
#define SSD1351_CMD_SETROW 0x75          ///< See datasheet
#define SSD1351_CMD_WRITERAM 0x5C        ///< See datasheet
#define SSD1351_CMD_READRAM 0x5D         ///< Not currently used
#define SSD1351_CMD_SETREMAP 0xA0        ///< See datasheet
#define SSD1351_CMD_STARTLINE 0xA1       ///< See datasheet
#define SSD1351_CMD_DISPLAYOFFSET 0xA2   ///< See datasheet
#define SSD1351_CMD_DISPLAYALLOFF 0xA4   ///< Not currently used
#define SSD1351_CMD_DISPLAYALLON 0xA5    ///< Not currently used
#define SSD1351_CMD_NORMALDISPLAY 0xA6   ///< See datasheet
#define SSD1351_CMD_INVERTDISPLAY 0xA7   ///< See datasheet
#define SSD1351_CMD_FUNCTIONSELECT 0xAB  ///< See datasheet
#define SSD1351_CMD_DISPLAYOFF 0xAE      ///< See datasheet
#define SSD1351_CMD_DISPLAYON 0xAF       ///< See datasheet
#define SSD1351_CMD_PRECHARGE 0xB1       ///< See datasheet
#define SSD1351_CMD_DISPLAYENHANCE 0xB2  ///< Not currently used
#define SSD1351_CMD_CLOCKDIV 0xB3        ///< See datasheet
#define SSD1351_CMD_SETVSL 0xB4          ///< See datasheet
#define SSD1351_CMD_SETGPIO 0xB5         ///< See datasheet
#define SSD1351_CMD_PRECHARGE2 0xB6      ///< See datasheet
#define SSD1351_CMD_SETGRAY 0xB8         ///< Not currently used
#define SSD1351_CMD_USELUT 0xB9          ///< Not currently used
#define SSD1351_CMD_PRECHARGELEVEL 0xBB  ///< Not currently used
#define SSD1351_CMD_VCOMH 0xBE           ///< See datasheet
#define SSD1351_CMD_CONTRASTABC 0xC1     ///< See datasheet
#define SSD1351_CMD_CONTRASTMASTER 0xC7  ///< See datasheet
#define SSD1351_CMD_MUXRATIO 0xCA        ///< See datasheet
#define SSD1351_CMD_COMMANDLOCK 0xFD     ///< See datasheet
#define SSD1351_CMD_HORIZSCROLL 0x96     ///< Not currently used
#define SSD1351_CMD_STOPSCROLL 0x9E      ///< Not currently used
#define SSD1351_CMD_STARTSCROLL 0x9F     ///< Not currently used

typedef struct _dgx_ssd1351_t {
    dgx_screen_with_bus_t base;
    gpio_num_t rst;
} dgx_ssd1351_t;

static const char TAG[] = "DGX SSD 1351";

static uint8_t *dgx_ssd1351_adj_madctl(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd) {
    static uint8_t adj_data[1];
    if (scr->scr.rgb_order == DgxScreenBGR)
        adj_data[0] = init_cmd->data[0] & ~0x4;
    else
        adj_data[0] = init_cmd->data[0];
    if (scr->scr.color_bits == 18) {
        adj_data[0] |= 0x80;
        adj_data[0] &= 0x40;
    }  // else 16 bits
    return adj_data;
}

DRAM_ATTR static const dgx_lcd_init_cmd_t st_init_cmds[] = {
    //
    {SSD1351_CMD_COMMANDLOCK, 0, 1, 0, {0x12}},                          //
    {SSD1351_CMD_COMMANDLOCK, 0, 1, 0, {0xB1}},                          //
    {SSD1351_CMD_DISPLAYOFF, 0, 0, 0, {0x00}},                           // Display off, no args
    {SSD1351_CMD_CLOCKDIV, 0, 1, 0, {0xF1}},                             // 7:4 = Oscillator Freq, 3:0 = CLK Div Ratio (A[3:0]+1 = 1..16)
    {SSD1351_CMD_MUXRATIO, 0, 1, 0, {127}},                              //
    {SSD1351_CMD_DISPLAYOFFSET, 0, 1, 0, {0x0}},                         //
    {SSD1351_CMD_SETGPIO, 0, 1, 0, {0x00}},                              //
    {SSD1351_CMD_FUNCTIONSELECT, 0, 1, 0, {0x01}},                       // internal (diode drop)
    {SSD1351_CMD_PRECHARGE, 0, 1, 0, {0x32}},                            //
    {SSD1351_CMD_VCOMH, 0, 1, 0, {0x05}},                                //
    {SSD1351_CMD_NORMALDISPLAY, 0, 0, 0, {0x00}},                        //
    {SSD1351_CMD_CONTRASTABC, 0, 3, 0, {0xD0, 0xD0, 0xD0}},              //
    {SSD1351_CMD_CONTRASTMASTER, 0, 1, 0, {0x0F}},                       //
    {SSD1351_CMD_SETVSL, 0, 3, 0, {0xA0, 0xB5, 0x55}},                   //
    {SSD1351_CMD_PRECHARGE2, 0, 1, 0, {0x01}},                           //
    {SSD1351_CMD_DISPLAYON, 0, 0, 255, {0x00}},                          // Main screen turn on
    {SSD1351_CMD_SETREMAP, dgx_ssd1351_adj_madctl, 1, 0, {0b01100100}},  // 64K, enable split, CBA (RGB)
    {SSD1351_CMD_STARTLINE, 0, 1, 0, {0x00}},                            //
    {0, 0, 0xff, 0, {0}}                                                 // Stop
};

// END OF COMMAND LIST

// Initialize the display
dgx_screen_t *dgx_ssd1351_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo) {
    // Attach the LCD to the SPI bus
    dgx_ssd1351_t *scr = (dgx_ssd1351_t *)calloc(1, sizeof(dgx_ssd1351_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }
    scr->rst = rst;
    scr->base.bus = bus;
    scr->base.scr.cg_row_shift = 0;
    scr->base.scr.cg_col_shift = 0;
    scr->base.scr.width = 128;
    scr->base.scr.height = 128;
    scr->base.scr.color_bits = color_bits;
    scr->base.scr.rgb_order = cbo;
    scr->base.scr.screen_name = "SSD 1351";
    scr->base.scr.screen_submodel = 0;
    scr->base.scr.screen_subtype = DgxPhysicalScreenWithBus;
    dgx_screen_with_bus_init_area(&scr->base, SSD1351_CMD_SETCOLUMN, SSD1351_CMD_SETROW, SSD1351_CMD_WRITERAM, 0,
                                  DGX_SCREEN_AREA_PROTO_STD8);
    // Initialize non-SPI GPIOs
    if (rst >= 0) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        // Reset the display
        dgx_gpio_set_level(rst, 1);
        dgx_delay(100);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(100);
        dgx_gpio_set_level(rst, 1);
    }
    // Send all the commands
    dgx_lcd_init(&scr->base, st_init_cmds);
    dgx_scr_init_slow_bus_optimized_funcs((dgx_screen_t *)scr);
    return (dgx_screen_t *)scr;
}

void dgx_ssd1351_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy) {
    dgx_ssd1351_t *scr = (dgx_ssd1351_t *)_scr;
    uint8_t cmd = SSD1351_CMD_SETREMAP;
    uint8_t adj_data[1] = {
        0b01100100  // 64K, enable split, CBA (RGB)
    };
    _scr->dir_x = dir_x;
    _scr->dir_y = dir_y;
    _scr->swap_xy = swap_xy;
    if (scr->base.scr.rgb_order == DgxScreenBGR) {
        adj_data[0] &= ~0x4;
    }
    if (_scr->color_bits == 18) {
        adj_data[0] |= 0x80;
        adj_data[0] &= 0x40;
    }  // else 16 bits
    if (dir_x == DgxScreenRightLeft) {
        adj_data[0] |= 0x02;
    }
    if (dir_y == DgxScreenBottomTop) {
        adj_data[0] |= 0x10;
    }
    if (swap_xy) {
        adj_data[0] |= 0x01;
    }
    scr->base.bus->write_command(scr->base.bus, cmd);
    scr->base.bus->write_data(scr->base.bus, adj_data, 8);
    cmd = SSD1351_CMD_STARTLINE;
    if (dir_y == DgxScreenBottomTop) {
        adj_data[0] = 0x80;
    } else {
        adj_data[0] = 0x00;
    }
    scr->base.bus->write_command(scr->base.bus, cmd);
    scr->base.bus->write_data(scr->base.bus, adj_data, 8);
}

void dgx_ssd1351_brightness(dgx_screen_t *_scr, uint8_t brightness) {
    dgx_ssd1351_t *scr = (dgx_ssd1351_t *)_scr;
    uint8_t cmd = SSD1351_CMD_CONTRASTMASTER;
    uint8_t adj_data[1] = {brightness >> 4};
    scr->base.bus->write_command(scr->base.bus, cmd);
    scr->base.bus->write_data(scr->base.bus, adj_data, 8);
}