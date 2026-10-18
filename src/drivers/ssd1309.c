/*
 * ssd1309.c
 *
 * An SSD1309 monochrome OLED: the command set of the SSD1306 without the
 * charge pump, so there is nothing to tell the driver about the supply. The
 * values of the init commands are those of the u8g2 driver of the controller
 * (csrc/u8x8_d_ssd1309.c of https://github.com/olikraus/u8g2, BSD-2-Clause,
 * Copyright (c) 2017, olikraus@gmail.com), apart from the addressing mode:
 * u8g2 writes page by page, this driver sets a column and a page range and
 * sends the rectangle in one transfer, as the SSD1306 driver does.
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 */

#include <string.h>

#include "bus/dgx_i2c_esp32.h"
#include "dgx_bw_screen.h"
#include "drivers/ssd1309.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Control byte of the I2C protocol, the same as the SSD1306
#define SSD1309_CONTROL_BYTE_CMD_SINGLE  0x80
#define SSD1309_CONTROL_BYTE_CMD_STREAM  0x00
#define SSD1309_CONTROL_BYTE_DATA_STREAM 0x40

// Fundamental commands
#define SSD1309_CMD_SET_CONTRAST     0x81 // follow with 0x00..0xFF
#define SSD1309_CMD_DISPLAY_RAM      0xA4
#define SSD1309_CMD_DISPLAY_ALLON    0xA5
#define SSD1309_CMD_DISPLAY_NORMAL   0xA6
#define SSD1309_CMD_DISPLAY_INVERTED 0xA7
#define SSD1309_CMD_DISPLAY_OFF      0xAE
#define SSD1309_CMD_DISPLAY_ON       0xAF

// Addressing commands
#define SSD1309_CMD_SET_MEMORY_ADDR_MODE 0x20 // follow with 0x00 = horizontal mode
#define SSD1309_CMD_SET_COLUMN_RANGE     0x21 // horizontal/vertical mode only, follow with the first and the last column
#define SSD1309_CMD_SET_PAGE_RANGE       0x22 // horizontal/vertical mode only, follow with the first and the last page

#define SSD1309_CMD_DEACTIVATE_SCROLL 0x2E

// Hardware configuration
#define SSD1309_CMD_SET_DISPLAY_START_LINE 0x40
#define SSD1309_CMD_SET_SEGMENT_REMAP      0xA1 // column 127 is segment 0
#define SSD1309_CMD_SET_MUX_RATIO          0xA8 // follow with height - 1
#define SSD1309_CMD_SET_COM_SCAN_MODE      0xC8 // from COM[N-1] to COM0
#define SSD1309_CMD_SET_DISPLAY_OFFSET     0xD3 // follow with 0x00
#define SSD1309_CMD_SET_COM_PIN_MAP        0xDA // follow with 0x12 for 64 rows, 0x02 for 32

// Timing and driving scheme
#define SSD1309_CMD_SET_DISPLAY_CLK_DIV 0xD5 // follow with 0xA0
#define SSD1309_CMD_SET_PRECHARGE       0xD9 // follow with 0xD3
#define SSD1309_CMD_SET_VCOMH_DESELCT   0xDB // follow with 0x20

typedef struct
{
    uint8_t cmd;
    uint8_t databytes; // No of data in data; 0xFF = end of cmds.
    uint8_t delay;
    uint8_t data[2];
} lcd_init_cmd_t;

typedef struct _dgx_ssd1309_t
{
    dgx_bw_vscreen_t     base;
    dgx_bus_protocols_t *bus;
    gpio_num_t           rst;
    uint8_t              col_offset; // the segment the first column of the panel is wired to
} dgx_ssd1309_t;

// u8x8_d_ssd1309_128x64_noname_init_seq of u8g2, with the horizontal addressing mode and the display on at the end
static const lcd_init_cmd_t st_init_cmds[] = {
    {SSD1309_CMD_DISPLAY_OFF,            0,    0, {0}   }, //
    {SSD1309_CMD_SET_DISPLAY_CLK_DIV,    1,    0, {0xA0}}, // clock divide ratio 1, oscillator frequency 0xA
    {SSD1309_CMD_SET_MUX_RATIO,          1,    0, {0x3F}}, // replaced by height - 1
    {SSD1309_CMD_SET_DISPLAY_OFFSET,     1,    0, {0}   }, //
    {SSD1309_CMD_SET_DISPLAY_START_LINE, 0,    0, {0}   }, //
    {SSD1309_CMD_SET_MEMORY_ADDR_MODE,   1,    0, {0}   }, // horizontal: the column wraps to the next page of the range
    {SSD1309_CMD_SET_SEGMENT_REMAP,      0,    0, {0}   }, //
    {SSD1309_CMD_SET_COM_SCAN_MODE,      0,    0, {0}   }, //
    {SSD1309_CMD_SET_COM_PIN_MAP,        1,    0, {0x12}}, // alternative COM pins, no left/right remap; 0x02 for 32 rows
    {SSD1309_CMD_SET_CONTRAST,           1,    0, {0x6F}}, //
    {SSD1309_CMD_SET_PRECHARGE,          1,    0, {0xD3}}, //
    {SSD1309_CMD_SET_VCOMH_DESELCT,      1,    0, {0x20}}, // vcomh=0 gives the biggest range of the contrast but not every OLED takes it (u8g2 #98, #116)
    {SSD1309_CMD_DEACTIVATE_SCROLL,      0,    0, {0}   }, //
    {SSD1309_CMD_DISPLAY_RAM,            0,    0, {0}   }, //
    {SSD1309_CMD_DISPLAY_NORMAL,         0,    0, {0}   }, //
    {SSD1309_CMD_DISPLAY_ON,             0,    0, {0}   }, //
    {0,                                  0xff, 0, {0}   }  // Stop
};

static const char TAG[] = "DGX SSD1309";

void dgx_ssd1309_update_screen(dgx_screen_t *scr_, int left, int right, int top, int bottom)
{
    dgx_ssd1309_t *scr = (dgx_ssd1309_t *)scr_;
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
    // the controller has 128 segments; what the offset pushes past the last one is not shown
    int clo = left + scr->col_offset;
    int chi = right + scr->col_offset;
    if (clo > 127) return;
    if (chi > 127) {
        chi   = 127;
        right = chi - scr->col_offset;
    }
    uint8_t page_s   = (uint16_t)top >> 3;
    uint8_t page_e   = (uint16_t)bottom >> 3;
    uint8_t crset[6] = {
        SSD1309_CMD_SET_COLUMN_RANGE, //
        (uint8_t)clo,                 //
        (uint8_t)chi,                 //
        SSD1309_CMD_SET_PAGE_RANGE,   //
        page_s,                       //
        page_e                        //
    };
    scr->bus->write_commands(scr->bus, crset, 6 * 8u);
    if (left == 0 && right == (scr_->width - 1) && page_s == 0 && page_e == ((uint16_t)(scr_->height - 1) >> 3)) {
        uint8_t *pb    = scr->base.v_array;
        uint32_t bsize = scr_->width * (page_e + 1);
        scr->bus->write_data(scr->bus, pb, bsize * 8u);
    } else {
        for (; page_s <= page_e; ++page_s) {
            uint8_t *pb = scr->base.v_array + page_s * scr_->width + left;
            scr->bus->write_data(scr->bus, pb, (right - left + 1) * 8u);
        }
    }
}

void dgx_ssd1309_contrast(dgx_screen_t *scr_, uint8_t contrast)
{
    dgx_ssd1309_t *scr     = (dgx_ssd1309_t *)scr_;
    uint8_t        cset[2] = {
        SSD1309_CMD_SET_CONTRAST, //
        contrast                  //
    };
    scr->bus->write_commands(scr->bus, cset, 2 * 8u);
}

void dgx_ssd1309_column_offset(dgx_screen_t *scr_, uint8_t offset)
{
    dgx_ssd1309_t *scr = (dgx_ssd1309_t *)scr_;
    if (offset > 127) offset = 127;
    scr->col_offset = offset;
    dgx_ssd1309_update_screen(scr_, 0, scr_->width - 1, 0, scr_->height - 1);
}

static void dgx_ssd1309_destroy_screen(dgx_screen_t **pscr)
{
    dgx_bw_destroy_screen(pscr);
}

dgx_screen_t *dgx_ssd1309_init(dgx_bus_protocols_t *bus, ssd1309_resolution_t resolution, gpio_num_t rst)
{
    int16_t width;
    int16_t height;
    switch (resolution) {
    case SSD1309_128X64:
        width  = 128;
        height = 64;
        break;
    case SSD1309_128X32:
        width  = 128;
        height = 32;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported SSD1309 resolution");
        return NULL;
    }
    dgx_bw_vscreen_t *bw_scr = (dgx_bw_vscreen_t *)dgx_bw_init(width, height);
    if (bw_scr == NULL) {
        ESP_LOGE(TAG, "BW screen initialization failed");
        return NULL;
    }
    dgx_ssd1309_t *scr = (dgx_ssd1309_t *)calloc(1, sizeof(dgx_ssd1309_t));
    if (scr == NULL) {
        ESP_LOGE(TAG, "SSD1309 screen structure memory allocation failed");
        bw_scr->base.destroy((dgx_screen_t **)&bw_scr);
        return NULL;
    }
    scr->base = *bw_scr;
    free(bw_scr);
    scr->base.base.screen_name    = "SSD1309";
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.update_screen  = dgx_ssd1309_update_screen;
    scr->base.base.destroy        = dgx_ssd1309_destroy_screen;
    scr->rst                      = rst;
    scr->bus                      = bus;
    scr->col_offset               = 0;
    if (bus->bus_type == DGX_BUS_I2C) {
        dgx_i2c_bus_t *i2c_bus = (dgx_i2c_bus_t *)bus;
        i2c_bus->data_stream   = SSD1309_CONTROL_BYTE_DATA_STREAM;
        i2c_bus->cmd_stream    = SSD1309_CONTROL_BYTE_CMD_STREAM;
        i2c_bus->cmd_single    = SSD1309_CONTROL_BYTE_CMD_SINGLE;
        if (bus->buffer_len == 0) {
            bus->buffer_len = 1 + width * ((height + 7) / 8);
            bus->buffer     = (uint8_t *)heap_caps_calloc(1, bus->buffer_len, MALLOC_CAP_DMA);
            if (bus->buffer == NULL) {
                ESP_LOGE(TAG, "SSD1309 I2C buffer allocation failed");
                bus->buffer_len = 0;
                dgx_ssd1309_destroy_screen((dgx_screen_t **)&scr);
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
        if (cmd == SSD1309_CMD_SET_MUX_RATIO) {
            *adj_data = height - 1;
            data      = adj_data;
        } else if (cmd == SSD1309_CMD_SET_COM_PIN_MAP && height == 32) {
            *adj_data = 0x02;
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
