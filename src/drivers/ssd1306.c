#include <string.h>

#include "bus/dgx_i2c_esp32.h"
#include "dgx_bw_screen.h"
#include "drivers/ssd1306.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Control byte
#define SSD1306_CONTROL_BYTE_CMD_SINGLE  0x80
#define SSD1306_CONTROL_BYTE_CMD_STREAM  0x00
#define SSD1306_CONTROL_BYTE_DATA_STREAM 0x40

// Fundamental commands (pg.28)
#define SSD1306_CMD_SET_CONTRAST     0x81 // follow with 0x7F
#define SSD1306_CMD_DISPLAY_RAM      0xA4
#define SSD1306_CMD_DISPLAY_ALLON    0xA5
#define SSD1306_CMD_DISPLAY_NORMAL   0xA6
#define SSD1306_CMD_DISPLAY_INVERTED 0xA7
#define SSD1306_CMD_DISPLAY_OFF      0xAE
#define SSD1306_CMD_DISPLAY_ON       0xAF

// Addressing Command Table (pg.30)
#define SSD1306_CMD_SET_MEMORY_ADDR_MODE 0x20 // follow with 0x00 = HORZ mode = Behave like a KS108 graphic LCD
#define SSD1306_CMD_SET_COLUMN_RANGE     0x21 // can be used only in HORZ/VERT mode - follow with 0x00 and 0x7F = COL127
#define SSD1306_CMD_SET_PAGE_RANGE       0x22 // can be used only in HORZ/VERT mode - follow with 0x00 and 0x07 = PAGE7

#define SSD1306_CMD_DEACTIVATE_SCROLL 0x2E //

// Hardware Config (pg.31)
#define SSD1306_CMD_SET_DISPLAY_START_LINE 0x40
#define SSD1306_CMD_SET_SEGMENT_REMAP      0xA1
#define SSD1306_CMD_SET_MUX_RATIO          0xA8 // follow with 0x3F = 64 MUX
#define SSD1306_CMD_SET_COM_SCAN_MODE      0xC8
#define SSD1306_CMD_SET_DISPLAY_OFFSET     0xD3 // follow with 0x00
#define SSD1306_CMD_SET_COM_PIN_MAP        0xDA // follow with 0x12
#define SSD1306_CMD_NOP                    0xE3 // NOP

// Timing and Driving Scheme (pg.32)
#define SSD1306_CMD_SET_DISPLAY_CLK_DIV 0xD5 // follow with 0x80
#define SSD1306_CMD_SET_PRECHARGE       0xD9 // follow with 0xF1
#define SSD1306_CMD_SET_VCOMH_DESELCT   0xDB // follow with 0x30

// Charge Pump (pg.62)
#define SSD1306_CMD_SET_CHARGE_PUMP 0x8D // follow with 0x14

typedef struct
{
    uint8_t cmd;
    uint8_t databytes; // No of data in data; 0xFF = end of cmds.
    uint8_t delay;
    uint8_t data[16];
} lcd_init_cmd_t;

typedef struct _dgx_ssd1306_t
{
    dgx_bw_vscreen_t     base;
    dgx_bus_protocols_t *bus;
    gpio_num_t           rst;
} dgx_ssd1306_t;

DRAM_ATTR static const lcd_init_cmd_t st_init_cmds[] = {
    //
    {SSD1306_CMD_DISPLAY_OFF,            0,    0, {0}   }, //
    {SSD1306_CMD_SET_MUX_RATIO,          1,    0, {31}  }, //
    {SSD1306_CMD_SET_DISPLAY_OFFSET,     1,    0, {0}   }, //
    {SSD1306_CMD_SET_DISPLAY_START_LINE, 0,    0, {0}   }, //
    {SSD1306_CMD_DEACTIVATE_SCROLL,      0,    0, {0}   }, //
    {SSD1306_CMD_SET_SEGMENT_REMAP,      0,    0, {0}   }, //
    {SSD1306_CMD_SET_COM_SCAN_MODE,      0,    0, {0}   }, //
    {SSD1306_CMD_SET_COM_PIN_MAP,        1,    0, {0x02}}, //
    {SSD1306_CMD_SET_CONTRAST,           1,    0, {0x8F}}, //
    {SSD1306_CMD_DISPLAY_RAM,            0,    0, {0}   }, //
    {SSD1306_CMD_DISPLAY_NORMAL,         0,    0, {0}   }, //
    {SSD1306_CMD_SET_DISPLAY_CLK_DIV,    1,    0, {0x80}}, //
    {SSD1306_CMD_SET_CHARGE_PUMP,        1,    0, {0x14}}, //
    {SSD1306_CMD_SET_MEMORY_ADDR_MODE,   1,    0, {0}   }, //
    {SSD1306_CMD_SET_PRECHARGE,          1,    0, {0xF1}}, //
    {SSD1306_CMD_SET_VCOMH_DESELCT,      1,    0, {0x40}}, //
    {SSD1306_CMD_DISPLAY_ON,             0,    0, {0}   }, //
    {0,                                  0xff, 0, {0}   }  // Stop
};

static const char TAG[] = "DGX SSD1306";

void dgx_ssd1306_update_screen(dgx_screen_t *scr_, int left, int right, int top, int bottom)
{
    dgx_ssd1306_t *scr = (dgx_ssd1306_t *)scr_;
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
    uint8_t page_s    = (uint16_t)top >> 3;
    uint8_t page_e    = (uint16_t)bottom >> 3;
    uint8_t seg_shift = (128 - scr_->width) >> 1;
    uint8_t clo       = left + seg_shift;
    uint8_t chi       = right + seg_shift;
    uint8_t crset[6]  = {
        SSD1306_CMD_SET_COLUMN_RANGE, //
        clo,                          //
        chi,                          //
        SSD1306_CMD_SET_PAGE_RANGE,   //
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

void dgx_ssd1306_contrast(dgx_screen_t *scr_, uint8_t contrast)
{
    dgx_ssd1306_t *scr     = (dgx_ssd1306_t *)scr_;
    uint8_t        cset[2] = {
        SSD1306_CMD_SET_CONTRAST, //
        contrast                  //
    };
    scr->bus->write_commands(scr->bus, cset, 2 * 8u);
}

static void dgx_ssd1306_destroy_screen(dgx_screen_t **pscr)
{
    dgx_bw_destroy_screen(pscr);
}

dgx_screen_t *dgx_ssd1306_init(dgx_bus_protocols_t *bus, ssd1306_resolution_t resolution, uint8_t is_ext_vcc, gpio_num_t rst)
{
    int16_t width;
    int16_t height;
    switch (resolution) {
    case SSD1306_128X32:
        width  = 128;
        height = 32;
        break;
    case SSD1306_128X64:
        width  = 128;
        height = 64;
        break;
    case SSD1306_96X16:
        width  = 96;
        height = 16;
        break;
    case SSD1306_64X48:
        width  = 64;
        height = 48;
        break;
    case SSD1306_72X40:
        width  = 72;
        height = 40;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported SSD1306 resolution");
        return NULL;
    }
    dgx_bw_vscreen_t *bw_scr = (dgx_bw_vscreen_t *)dgx_bw_init(width, height);
    if (bw_scr == NULL) {
        ESP_LOGE(TAG, "BW screen initialization failed");
        return NULL;
    }
    dgx_ssd1306_t *scr = (dgx_ssd1306_t *)calloc(1, sizeof(dgx_ssd1306_t));
    if (scr == NULL) {
        ESP_LOGE(TAG, "SSD1306 screen structure memory allocation failed");
        bw_scr->base.destroy((dgx_screen_t **)&bw_scr);
        return NULL;
    }
    scr->base = *bw_scr;
    free(bw_scr);
    scr->base.base.screen_name    = "SSD1306";
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.update_screen  = dgx_ssd1306_update_screen;
    scr->base.base.destroy        = dgx_ssd1306_destroy_screen;
    scr->rst                      = rst;
    scr->bus                      = bus;
    if (bus->bus_type == DGX_BUS_I2C) {
        dgx_i2c_bus_t *i2c_bus = (dgx_i2c_bus_t *)bus;
        i2c_bus->data_stream   = SSD1306_CONTROL_BYTE_DATA_STREAM;
        i2c_bus->cmd_stream    = SSD1306_CONTROL_BYTE_CMD_STREAM;
        i2c_bus->cmd_single    = SSD1306_CONTROL_BYTE_CMD_SINGLE;
        if (bus->buffer_len == 0) {
            bus->buffer_len = 1 + width * ((height + 7) / 8);
            bus->buffer     = (uint8_t *)heap_caps_calloc(1, bus->buffer_len, MALLOC_CAP_DMA);
            if (bus->buffer == NULL) {
                ESP_LOGE(TAG, "SSD1306 I2C buffer allocation failed");
                bus->buffer_len = 0;
                dgx_ssd1306_destroy_screen((dgx_screen_t **)&scr);
                return NULL;
            }
        }
    }
    const lcd_init_cmd_t *lcd_init_cmds = st_init_cmds;

    if (rst != GPIO_NUM_NC) {
        // Initialize non-SPI GPIOs
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        // Reset the display
        dgx_gpio_set_level(rst, 1);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 1);
    }
    // Send all the commands
    uint8_t adj_data[1];
    for (uint8_t cidx = 0; lcd_init_cmds[cidx].databytes != 0xff; ++cidx) {
        uint8_t cmd = lcd_init_cmds[cidx].cmd;
        // if (cmd == SSD1306_CMD_SET_DISPLAY_START_LINE) {
        // }
        ESP_LOGD(TAG, "Sending init command: 0x%02x", cmd);
        scr->bus->write_command(scr->bus, cmd);
        uint8_t const *data = lcd_init_cmds[cidx].data;
        if (cmd == SSD1306_CMD_SET_CHARGE_PUMP) {
            data = adj_data;
            if (is_ext_vcc) {
                *adj_data = 0x10;
            } else {
                *adj_data = 0x14;
            }
            // } else if (cmd == SSD1306_CMD_SET_DISPLAY_OFFSET) {
        } else if (cmd == SSD1306_CMD_SET_MUX_RATIO) {
            *adj_data = height - 1;
            data      = adj_data;
        } else if (cmd == SSD1306_CMD_SET_COM_PIN_MAP) {
            if (height == 64 || height == 48 || height == 40) {
                *adj_data = 0x12;
                data      = adj_data;
            }
        } else if (cmd == SSD1306_CMD_SET_CONTRAST) {
            if (height == 64 || height == 48) {
                data = adj_data;
                if (is_ext_vcc) {
                    *adj_data = 0x9f;
                } else {
                    *adj_data = 0xcf;
                }
            } else if (height != 32) {
                data = adj_data;
                if (is_ext_vcc) {
                    *adj_data = 0x10;
                } else {
                    *adj_data = 0xaf;
                }
            }
        } else if (cmd == SSD1306_CMD_SET_PRECHARGE) {
            data = adj_data;
            if (is_ext_vcc) {
                *adj_data = 0x22;
            } else {
                *adj_data = 0xF1;
            }
        }
        if (lcd_init_cmds[cidx].databytes != 0) {
            bus->write_commands(scr->bus, data, 8);
        }
        if (lcd_init_cmds[cidx].delay) {
            int ms = lcd_init_cmds[cidx].delay == 255 ? 500 : lcd_init_cmds[cidx].delay;
            dgx_delay(ms);
        }
    }
    return (dgx_screen_t *)scr;
}
