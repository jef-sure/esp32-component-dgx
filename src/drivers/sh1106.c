/*
 * sh1106.c
 *
 * An SH1106 monochrome OLED: the commands of the SSD1306, 132 segments and
 * the page addressing mode only. The values of the init commands are those
 * of the u8g2 driver of the controller (csrc/u8x8_d_ssd1306_128x64_noname.c
 * of https://github.com/olikraus/u8g2, BSD-2-Clause, Copyright (c) 2016,
 * olikraus@gmail.com): the "winstar" sequence written for the SH1106 after
 * its datasheet (u8g2 issue 316), with the segment remap and the scan
 * direction of the "noname" sequence, so that the picture lies as on an
 * SSD1306 module. The segment the glass starts at is u8g2's too.
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 */

#include <string.h>

#include "bus/dgx_i2c_esp32.h"
#include "dgx_bw_screen.h"
#include "drivers/sh1106.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Control byte of the I2C protocol, the same as the SSD1306
#define SH1106_CONTROL_BYTE_CMD_SINGLE  0x80
#define SH1106_CONTROL_BYTE_CMD_STREAM  0x00
#define SH1106_CONTROL_BYTE_DATA_STREAM 0x40

#define SH1106_SEGMENTS 132

// Fundamental commands
#define SH1106_CMD_SET_CONTRAST     0x81 // follow with 0x00..0xFF
#define SH1106_CMD_DISPLAY_RAM      0xA4
#define SH1106_CMD_DISPLAY_ALLON    0xA5
#define SH1106_CMD_DISPLAY_NORMAL   0xA6
#define SH1106_CMD_DISPLAY_INVERTED 0xA7
#define SH1106_CMD_DISPLAY_OFF      0xAE
#define SH1106_CMD_DISPLAY_ON       0xAF

// Addressing commands: a page, then the column in two halves
#define SH1106_CMD_SET_PAGE       0xB0 // | page
#define SH1106_CMD_SET_COLUMN_LO  0x00 // | low nibble of the column
#define SH1106_CMD_SET_COLUMN_HI  0x10 // | high nibble of the column

// Hardware configuration
#define SH1106_CMD_SET_DISPLAY_START_LINE 0x40
#define SH1106_CMD_SET_SEGMENT_REMAP      0xA1 // column 131 is segment 0
#define SH1106_CMD_SET_MUX_RATIO          0xA8 // follow with height - 1
#define SH1106_CMD_SET_COM_SCAN_MODE      0xC8 // from COM[N-1] to COM0
#define SH1106_CMD_SET_DISPLAY_OFFSET     0xD3 // follow with 0x00
#define SH1106_CMD_SET_COM_PIN_MAP        0xDA // follow with 0x12

// Timing and driving scheme
#define SH1106_CMD_SET_DISPLAY_CLK_DIV 0xD5 // follow with 0x50
#define SH1106_CMD_SET_PRECHARGE       0xD9 // follow with 0x22
#define SH1106_CMD_SET_VCOMH_DESELCT   0xDB // follow with 0x35

// Power
#define SH1106_CMD_SET_DCDC         0xAD // follow with 0x8B: the built-in DC-DC converter on
#define SH1106_CMD_SET_PUMP_VOLTAGE 0x32 // 0x30..0x33: 6.4, 7.4, 8.0, 9.0 V

typedef struct
{
    uint8_t cmd;
    uint8_t databytes; // No of data in data; 0xFF = end of cmds.
    uint8_t delay;
    uint8_t data[2];
} lcd_init_cmd_t;

typedef struct _dgx_sh1106_t
{
    dgx_bw_vscreen_t     base;
    dgx_bus_protocols_t *bus;
    gpio_num_t           rst;
    uint8_t              col_offset; // the segment the first column of the panel is wired to
} dgx_sh1106_t;

// u8x8_d_sh1106_128x64_winstar_init_seq of u8g2, with the segment remap and the scan direction of the noname one
static const lcd_init_cmd_t st_init_cmds[] = {
    {SH1106_CMD_DISPLAY_OFF,            0,    0, {0}   }, //
    {SH1106_CMD_DISPLAY_RAM,            0,    0, {0}   }, //
    {SH1106_CMD_SET_DISPLAY_CLK_DIV,    1,    0, {0x50}}, // divide ratio 1, oscillator frequency +0%
    {SH1106_CMD_SET_MUX_RATIO,          1,    0, {0x3F}}, // replaced by height - 1
    {SH1106_CMD_SET_DISPLAY_OFFSET,     1,    0, {0}   }, //
    {SH1106_CMD_SET_DISPLAY_START_LINE, 0,    0, {0}   }, //
    {SH1106_CMD_SET_DCDC,               1,    0, {0x8B}}, // the built-in DC-DC converter is used
    {SH1106_CMD_SET_PRECHARGE,          1,    0, {0x22}}, // pre-charge 2 DCLKs, dis-charge 2 DCLKs
    {SH1106_CMD_SET_VCOMH_DESELCT,      1,    0, {0x35}}, // 0.770 V
    {SH1106_CMD_SET_PUMP_VOLTAGE,       0,    0, {0}   }, // 8.0 V
    {SH1106_CMD_SET_CONTRAST,           1,    0, {0xFF}}, //
    {SH1106_CMD_DISPLAY_NORMAL,         0,    0, {0}   }, //
    {SH1106_CMD_SET_COM_PIN_MAP,        1,    0, {0x12}}, // alternative COM pins, no left/right remap
    {SH1106_CMD_SET_SEGMENT_REMAP,      0,    0, {0}   }, //
    {SH1106_CMD_SET_COM_SCAN_MODE,      0,    0, {0}   }, //
    {SH1106_CMD_DISPLAY_ON,             0,    0, {0}   }, //
    {0,                                 0xff, 0, {0}   }  // Stop
};

static const char TAG[] = "DGX SH1106";

void dgx_sh1106_update_screen(dgx_screen_t *scr_, int left, int right, int top, int bottom)
{
    dgx_sh1106_t *scr = (dgx_sh1106_t *)scr_;
    ESP_LOGD(TAG, "Updating screen: left=%d, top=%d, right=%d, bottom=%d", left, top, right, bottom);
    if (left > right) {
        int t = left;
        left  = right;
        right = t;
    }
    if (top > bottom) {
        int t  = top;
        top    = bottom;
        bottom = t;
    }
    if (right < 0 || bottom < 0 || left >= scr_->width || top >= scr_->height) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= scr_->width) right = scr_->width - 1;
    if (bottom >= scr_->height) bottom = scr_->height - 1;
    // what the offset pushes past the last segment is not shown
    int clo = left + scr->col_offset;
    if (clo >= SH1106_SEGMENTS) return;
    if (right + scr->col_offset >= SH1106_SEGMENTS) right = SH1106_SEGMENTS - 1 - scr->col_offset;
    uint8_t page_s = (uint16_t)top >> 3;
    uint8_t page_e = (uint16_t)bottom >> 3;
    // the page addressing mode: the column goes back to where it was set only with the next page command
    for (; page_s <= page_e; ++page_s) {
        uint8_t prset[3] = {
            (uint8_t)(SH1106_CMD_SET_PAGE | page_s),              //
            (uint8_t)(SH1106_CMD_SET_COLUMN_LO | (clo & 0x0F)),   //
            (uint8_t)(SH1106_CMD_SET_COLUMN_HI | (clo >> 4))      //
        };
        scr->bus->write_commands(scr->bus, prset, 3 * 8u);
        uint8_t *pb = scr->base.v_array + page_s * scr_->width + left;
        scr->bus->write_data(scr->bus, pb, (right - left + 1) * 8u);
    }
}

void dgx_sh1106_contrast(dgx_screen_t *scr_, uint8_t contrast)
{
    dgx_sh1106_t *scr     = (dgx_sh1106_t *)scr_;
    uint8_t       cset[2] = {
        SH1106_CMD_SET_CONTRAST, //
        contrast                 //
    };
    scr->bus->write_commands(scr->bus, cset, 2 * 8u);
}

void dgx_sh1106_column_offset(dgx_screen_t *scr_, uint8_t offset)
{
    dgx_sh1106_t *scr = (dgx_sh1106_t *)scr_;
    if (offset > SH1106_SEGMENTS - 1) offset = SH1106_SEGMENTS - 1;
    scr->col_offset = offset;
    dgx_sh1106_update_screen(scr_, 0, scr_->width - 1, 0, scr_->height - 1);
}

static void dgx_sh1106_destroy_screen(dgx_screen_t **pscr)
{
    dgx_bw_destroy_screen(pscr);
}

dgx_screen_t *dgx_sh1106_init(dgx_bus_protocols_t *bus, sh1106_resolution_t resolution, gpio_num_t rst)
{
    int16_t width;
    int16_t height;
    uint8_t col_offset;
    switch (resolution) {
    case SH1106_128X64:
        width      = 128;
        height     = 64;
        col_offset = 2;
        break;
    case SH1106_64X32:
        width      = 64;
        height     = 32;
        col_offset = 32;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported SH1106 resolution");
        return NULL;
    }
    dgx_bw_vscreen_t *bw_scr = (dgx_bw_vscreen_t *)dgx_bw_init(width, height);
    if (bw_scr == NULL) {
        ESP_LOGE(TAG, "BW screen initialization failed");
        return NULL;
    }
    dgx_sh1106_t *scr = (dgx_sh1106_t *)calloc(1, sizeof(dgx_sh1106_t));
    if (scr == NULL) {
        ESP_LOGE(TAG, "SH1106 screen structure memory allocation failed");
        bw_scr->base.destroy((dgx_screen_t **)&bw_scr);
        return NULL;
    }
    scr->base = *bw_scr;
    free(bw_scr);
    scr->base.base.screen_name    = "SH1106";
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.update_screen  = dgx_sh1106_update_screen;
    scr->base.base.destroy        = dgx_sh1106_destroy_screen;
    scr->rst                      = rst;
    scr->bus                      = bus;
    scr->col_offset               = col_offset;
    if (bus->bus_type == DGX_BUS_I2C) {
        dgx_i2c_bus_t *i2c_bus = (dgx_i2c_bus_t *)bus;
        i2c_bus->data_stream   = SH1106_CONTROL_BYTE_DATA_STREAM;
        i2c_bus->cmd_stream    = SH1106_CONTROL_BYTE_CMD_STREAM;
        i2c_bus->cmd_single    = SH1106_CONTROL_BYTE_CMD_SINGLE;
        if (bus->buffer_len == 0) {
            bus->buffer_len = 1 + width * ((height + 7) / 8);
            bus->buffer     = (uint8_t *)heap_caps_calloc(1, bus->buffer_len, MALLOC_CAP_DMA);
            if (bus->buffer == NULL) {
                ESP_LOGE(TAG, "SH1106 I2C buffer allocation failed");
                bus->buffer_len = 0;
                dgx_sh1106_destroy_screen((dgx_screen_t **)&scr);
                return NULL;
            }
        }
    }

    if (rst != GPIO_NUM_NC) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 1);
        // u8g2 waits 100 ms after the reset: "far east OLEDs need much longer setup time"
        dgx_delay(100);
    }
    // Send all the commands
    uint8_t adj_data[1];
    for (uint8_t cidx = 0; st_init_cmds[cidx].databytes != 0xff; ++cidx) {
        uint8_t cmd = st_init_cmds[cidx].cmd;
        ESP_LOGD(TAG, "Sending init command: 0x%02x", cmd);
        bus->write_command(bus, cmd);
        uint8_t const *data = st_init_cmds[cidx].data;
        if (cmd == SH1106_CMD_SET_MUX_RATIO) {
            *adj_data = height - 1;
            data      = adj_data;
        }
        if (st_init_cmds[cidx].databytes != 0) {
            bus->write_commands(bus, data, st_init_cmds[cidx].databytes * 8u);
        }
        if (st_init_cmds[cidx].delay) {
            dgx_delay(st_init_cmds[cidx].delay);
        }
    }
    return (dgx_screen_t *)scr;
}
