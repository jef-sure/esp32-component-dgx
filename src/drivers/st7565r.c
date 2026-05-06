#include <string.h>

#include "dgx_bw_screen.h"
#include "drivers/st7565r.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ST7565R Commands
#define ST7565R_RESET                      0xE2
#define ST7565R_DISPLAY_ON                 0xAF
#define ST7565R_DISPLAY_OFF                0xAE
#define ST7565R_SET_PAGE                   0xB0
#define ST7565R_SET_COLUMN_UPPER           0x10
#define ST7565R_SET_COLUMN_LOWER           0x00
#define ST7565R_START_LINE                 0x40
#define ST7565R_CONTRAST                   0x81
#define ST7565R_NORMAL_DISPLAY             0xA6
#define ST7565R_REVERSE_DISPLAY            0xA7
#define ST7565R_COM_OUTPUT_NORMAL          0xC0
#define ST7565R_COM_OUTPUT_REVERSE         0xC8
#define ST7565R_SET_BOOSTER_RATIO          0xF8
#define ST7565R_SET_POWER_CONTROL          0x28
#define ST7565R_SET_VOLTAGE_RESISTOR_RATIO 0x20
#define ST7565R_SET_ALL_PIXEL_NORMAL       0xA4
#define ST7565R_SET_ALL_PIXEL_ON           0xA5
#define ST7565R_SET_DISPLAY_START_LINE     0x40
#define ST7565R_SET_PAGE_ADDRESS           0xB0
#define ST7565R_SET_COLUMN_ADDRESS         0x00
#define ST7565R_SET_BIAS_7                 0xA3
#define ST7565R_SET_BIAS_9                 0xA2
#define ST7565R_SET_ADC_NORMAL             0xA0

typedef struct
{
    uint8_t cmd;
    uint8_t databytes; // No of data in data; 0xFF = end of cmds.
    uint8_t delay;
    uint8_t data[16];
} lcd_init_cmd_t;

typedef struct _dgx_st7565r_t
{
    dgx_bw_vscreen_t     base;
    dgx_bus_protocols_t *bus;
    gpio_num_t           rst;
} dgx_st7565r_t;

static const lcd_init_cmd_t st_init_cmds[] = {
    {ST7565R_RESET,                             0,    10, {0}   }, //
    {ST7565R_DISPLAY_OFF,                       0,    10, {0}   }, //
    {ST7565R_SET_DISPLAY_START_LINE,            0,    10, {0}   }, //
    {ST7565R_SET_ADC_NORMAL,                    0,    10, {0}   }, //
    {ST7565R_COM_OUTPUT_REVERSE,                0,    10, {0}   }, //
    {ST7565R_NORMAL_DISPLAY,                    0,    10, {0}   }, //
    {ST7565R_SET_BIAS_9,                        0,    10, {0}   }, //
    {ST7565R_SET_POWER_CONTROL | 0x07,          0,    50, {0}   }, //
    {ST7565R_SET_BOOSTER_RATIO,                 1,    10, {0}   }, //
    {ST7565R_SET_VOLTAGE_RESISTOR_RATIO | 0x07, 0,    10, {0}   }, //
    {ST7565R_CONTRAST,                          1,    10, {0x18}}, //
    {ST7565R_DISPLAY_ON,                        0,    10, {0}   }, //
    {ST7565R_SET_ALL_PIXEL_NORMAL,              0,    10, {0}   }, //
    {0,                                         0xff, 0,  {0}   }  // Stop
};

static const char TAG[] = "DGX ST7565R";

void dgx_st7565r_update_screen(dgx_screen_t *scr_, int left, int right, int top, int bottom)
{
    dgx_st7565r_t *scr = (dgx_st7565r_t *)scr_;
    ESP_LOGD(TAG, "Updating screen: left=%d, top=%d, right=%d, bottom=%d", left, top, right, bottom);
    if (left > right) {
        int16_t t = left;
        left      = right;
        right     = t;
    }
    if (top > bottom) {
        int16_t t = top;
        top       = bottom;
        bottom    = t;
    }
    if (right < 0 || bottom < 0 || left >= scr->base.base.width || top >= scr->base.base.height) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= scr->base.base.width) right = scr->base.base.width - 1;
    if (bottom >= scr->base.base.height) bottom = scr->base.base.height - 1;
    uint8_t  page_s = (uint16_t)top >> 3;
    uint8_t  page_e = (uint16_t)bottom >> 3;
    uint8_t  clo    = left;
    uint32_t xw     = right - left + 1u;
    for (uint8_t page = page_s; page <= page_e; ++page) {
        uint8_t crset[3] = {
            ST7565R_SET_COLUMN_UPPER | (clo >> 4),   //
            ST7565R_SET_COLUMN_LOWER | (clo & 0x0F), //
            ST7565R_SET_PAGE | page                  //
        };
        ESP_LOGD(TAG, "Sending commands: 0x%02x 0x%02x 0x%02x", crset[0], crset[1], crset[2]);
        scr->bus->write_commands(scr->bus, crset, 3 * 8u);
        uint8_t *pb = scr->base.v_array + page * scr->base.base.width + left;
        ESP_LOGD(TAG, "Sending data for page %x: %" PRIu32 " bytes", page, xw);
        ESP_LOG_BUFFER_HEXDUMP(TAG, pb, xw, ESP_LOG_DEBUG);
        scr->bus->write_data(scr->bus, pb, xw * 8u);
    }
}

static void dgx_st7565r_destroy_screen(dgx_screen_t **pscr)
{
    dgx_bw_destroy_screen(pscr);
}

dgx_screen_t *dgx_st7565r_init(dgx_bus_protocols_t *bus, gpio_num_t rst)
{
    dgx_st7565r_t *scr = (dgx_st7565r_t *)calloc(1, sizeof(dgx_st7565r_t));
    if (!scr) return 0;
    scr->bus                  = bus;
    scr->rst                  = rst;
    int16_t           width   = 128; // ST7565R width
    int16_t           height  = 64;  // ST7565R height
    dgx_bw_vscreen_t *bw_vscr = (dgx_bw_vscreen_t *)dgx_bw_init(width, height);
    if (bw_vscr == NULL) {
        ESP_LOGE(TAG, "BW screen initialization failed");
        free(scr);
        return 0;
    }
    scr->base = *bw_vscr;
    free(bw_vscr);
    scr->base.base.screen_name    = "ST7565R";
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.rgb_order      = DgxScreenRGB;
    scr->base.base.update_screen  = dgx_st7565r_update_screen;
    scr->base.base.destroy        = dgx_st7565r_destroy_screen;

    // Initialize non-SPI GPIOs

    if (rst != GPIO_NUM_NC) {
        // Reset the display
        gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        gpio_set_level(rst, 1);
        dgx_delay(10);
        gpio_set_level(rst, 0);
        dgx_delay(50);
        gpio_set_level(rst, 1);
        dgx_delay(200);
    }
    // Send all the commands
    for (uint8_t cidx = 0; st_init_cmds[cidx].databytes != 0xff; ++cidx) {
        uint8_t cmd = st_init_cmds[cidx].cmd;
        ESP_LOGD(TAG, "Sending init command: 0x%02x", cmd);
        scr->bus->write_command(scr->bus, cmd);
        uint8_t const *data = st_init_cmds[cidx].data;
        if (st_init_cmds[cidx].databytes != 0) {
            for (uint8_t i = 0; i < st_init_cmds[cidx].databytes; ++i) {
                ESP_LOGD(TAG, "Sending init command: arguments 0x%02x", st_init_cmds[cidx].data[i]);
            }
            scr->bus->write_commands(scr->bus, data, st_init_cmds[cidx].databytes * 8u);
        }
        if (st_init_cmds[cidx].delay) {
            int ms = st_init_cmds[cidx].delay == 255 ? 500 : st_init_cmds[cidx].delay;
            dgx_delay(ms);
        }
    }
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    return (dgx_screen_t *)scr;
}
