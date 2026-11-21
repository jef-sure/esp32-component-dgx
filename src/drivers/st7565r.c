#include <stdlib.h>
#include <string.h>

#include "dgx_bw_screen.h"
#include "drivers/st7565r.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../vscreen/dgx_bw_screen_priv.h"
#include "../vscreen/dgx_soft_turn_priv.h"
#include "dgx_backlight_priv.h"

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
    gpio_num_t           backlight;
} dgx_st7565r_t;

/*
 * The voltage of the glass at the start is ratio 7, volume 12: on the one
 * module seen on glass the picture is clean from 8 to 14 and the background
 * lights up from 16. dgx_st7565r_contrast() sets both for another module.
 */
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
    {ST7565R_CONTRAST,                          1,    10, {12}  }, //
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
    // clipped to the screen and turned to the panel: from here on the columns and rows are those of the panel
    int pw, ph;
    dgx_soft_turn_panel(scr_, &pw, &ph);
    if (!dgx_soft_turn_rect(scr_, pw, ph, &left, &right, &top, &bottom)) return;
    uint8_t    page_s = (uint16_t)top >> 3;
    uint8_t    page_e = (uint16_t)bottom >> 3;
    uint8_t    clo    = left;
    uint32_t   xw     = right - left + 1u;
    const bool turned = dgx_soft_turned(scr_);
    for (uint8_t page = page_s; page <= page_e; ++page) {
        uint8_t crset[3] = {
            ST7565R_SET_COLUMN_UPPER | (clo >> 4),   //
            ST7565R_SET_COLUMN_LOWER | (clo & 0x0F), //
            ST7565R_SET_PAGE | page                  //
        };
        ESP_LOGD(TAG, "Sending commands: 0x%02x 0x%02x 0x%02x", crset[0], crset[1], crset[2]);
        scr->bus->write_commands(scr->bus, crset, 3 * 8u);
        uint8_t  bytes[128];
        uint8_t *pb = scr->base.v_array + page * pw + left;
        if (turned) {
            // the screen turned: the page of the panel gathered bit by bit
            dgx_soft_turn_page(scr_, scr->base.v_array, pw, ph, page, left, right, bytes);
            pb = bytes;
        }
        ESP_LOGD(TAG, "Sending data for page %x: %" PRIu32 " bytes", page, xw);
        ESP_LOG_BUFFER_HEXDUMP(TAG, pb, xw, ESP_LOG_DEBUG);
        scr->bus->write_data(scr->bus, pb, xw * 8u);
    }
}

/*
 * The screen turned in software, as on the other page controllers: the ADC
 * and the scan direction of the controller mirror only, and which way they
 * mirror depends on how a module wires its glass. With the axes swapped the
 * picture is cleared; the whole screen is sent.
 */
static void st7565r_orientation(dgx_screen_t *scr_, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_st7565r_t *scr = (dgx_st7565r_t *)scr_;
    int            pw, ph;
    dgx_soft_turn_panel(scr_, &pw, &ph);
    dgx_soft_turn_set(scr_, scr->base.v_array, pw, ph, dir_x, dir_y, swap_xy);
    dgx_bw_set_area(scr_, 0, (uint16_t)(scr_->width - 1), 0, (uint16_t)(scr_->height - 1));
    dgx_st7565r_update_screen(scr_, 0, scr_->width - 1, 0, scr_->height - 1);
}

static void dgx_st7565r_destroy_screen(dgx_screen_t **pscr)
{
    dgx_bw_destroy_screen(pscr);
}

/* the picture on or off, the memory of the controller kept */
static esp_err_t dgx_st7565r_display(dgx_screen_t *scr_, bool on)
{
    dgx_st7565r_t *scr = (dgx_st7565r_t *)scr_;
    scr->bus->write_command(scr->bus, on ? ST7565R_DISPLAY_ON : ST7565R_DISPLAY_OFF);
    return ESP_OK;
}

static esp_err_t st7565r_backlight(dgx_screen_t *scr, bool on);
static const dgx_screen_control_t dgx_st7565r_control = {
    .orientation = st7565r_orientation,
    .backlight   = st7565r_backlight,
    .display     = dgx_st7565r_display,
};

dgx_screen_t *dgx_st7565r_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight)
{
    dgx_st7565r_t *scr = (dgx_st7565r_t *)calloc(1, sizeof(dgx_st7565r_t));
    if (!scr) return 0;
    scr->bus                  = bus;
    scr->rst                  = rst;
    scr->backlight            = backlight;
    int16_t           width   = 128; // ST7565R width
    int16_t           height  = 64;  // ST7565R height
    dgx_bw_vscreen_t *bw_vscr = (dgx_bw_vscreen_t *)dgx_bw_init_with(width, height, bus->memory_allocator);
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
    scr->base.base.control        = &dgx_st7565r_control;

    // Initialize non-SPI GPIOs

    if (rst != GPIO_NUM_NC) {
        // Reset the display
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(50);
        dgx_gpio_set_level(rst, 1);
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
    dgx_backlight_on_start(backlight);
    return (dgx_screen_t *)scr;
}

static esp_err_t st7565r_backlight(dgx_screen_t *scr, bool on)
{
    return dgx_backlight_set(((dgx_st7565r_t *)scr)->backlight, on);
}

/* the gain of the voltage regulator and the electronic volume: together the voltage of the glass */
esp_err_t dgx_st7565r_contrast(dgx_screen_t *scr_, uint8_t resistor_ratio, uint8_t volume)
{
    if (resistor_ratio > 7 || volume > 63) return ESP_ERR_INVALID_ARG;
    dgx_st7565r_t *scr    = (dgx_st7565r_t *)scr_;
    const uint8_t  set[3] = {ST7565R_SET_VOLTAGE_RESISTOR_RATIO | resistor_ratio, ST7565R_CONTRAST, volume};
    scr->bus->write_commands(scr->bus, set, 3 * 8u);
    return ESP_OK;
}

/* deprecated until DGX 1.0: dgx_screen_backlight() */
esp_err_t dgx_st7565r_backlight(dgx_screen_t *scr, bool on)
{
    return st7565r_backlight(scr, on);
}
