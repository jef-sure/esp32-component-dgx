/*
 * ssd1322.c
 *
 * SSD1322, a 16-gray OLED controller: 480x128 pixels of memory, 4 bits each,
 * two pixels to a byte. Its column address counts groups of four pixels
 * (two bytes), so the smallest piece of memory that can be written is four
 * pixels wide. The driver keeps the picture in a 4-bit virtual screen
 * (dgx_vscreen_init(), 8 KB for 256x64) and sends the changed part row by
 * row, each row as a window of whole columns.
 *
 * Every argument of a command goes with D/C# high, as data. A window is
 * therefore three transfers: 15h with its two columns, 75h with its two
 * rows, and 5Ch with the pixels.
 *
 * A module uses part of the memory: its first column, its rows from 0, and
 * a remap that matches how its glass is wired; see ssd1322_modules. The
 * values are those of u8g2, and of the Newhaven example code for the 3.12"
 * 256x64 modules.
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 */

#include "drivers/ssd1322.h"

#include "dgx_arch.h"
#include "drivers/vscreen.h"
#include "esp_log.h"

#include "../vscreen/dgx_soft_turn_priv.h"
#include "../vscreen/dgx_vscreen_priv.h"

#define SSD1322_CMD_ENABLEGRAY     0x00 ///< puts the table of B8h in force
#define SSD1322_CMD_SETCOLUMN      0x15 ///< start and end column, 0..119, 4 pixels each
#define SSD1322_CMD_WRITERAM       0x5C ///< the data that follow go to memory
#define SSD1322_CMD_SETROW         0x75 ///< start and end row, 0..127
#define SSD1322_CMD_SETREMAP       0xA0 ///< see ssd1322_module_t
#define SSD1322_CMD_STARTLINE      0xA1
#define SSD1322_CMD_DISPLAYOFFSET  0xA2
#define SSD1322_CMD_DISPLAYALLOFF  0xA4
#define SSD1322_CMD_DISPLAYALLON   0xA5
#define SSD1322_CMD_NORMALDISPLAY  0xA6
#define SSD1322_CMD_INVERTDISPLAY  0xA7
#define SSD1322_CMD_FUNCTIONSELECT 0xAB
#define SSD1322_CMD_DISPLAYOFF     0xAE
#define SSD1322_CMD_DISPLAYON      0xAF
#define SSD1322_CMD_PRECHARGE      0xB1
#define SSD1322_CMD_CLOCKDIV       0xB3
#define SSD1322_CMD_SETVSL         0xB4
#define SSD1322_CMD_SETGPIO        0xB5
#define SSD1322_CMD_PRECHARGE2     0xB6
#define SSD1322_CMD_SETGRAY        0xB8
#define SSD1322_CMD_DEFAULTGRAY    0xB9
#define SSD1322_CMD_PRECHARGELEVEL 0xBB
#define SSD1322_CMD_VCOMH          0xBE
#define SSD1322_CMD_CONTRAST       0xC1
#define SSD1322_CMD_MASTERCONTRAST 0xC7
#define SSD1322_CMD_MUXRATIO       0xCA
#define SSD1322_CMD_DISPLAYENHANCE 0xD1
#define SSD1322_CMD_COMMANDLOCK    0xFD

typedef struct {
    uint8_t cmd;
    uint8_t databytes; /* 0xFF ends the table */
    uint8_t data[2];
} ssd1322_init_cmd_t;

/*
 * The sequence of the 256x64 and 240x128 modules; the multiplex ratio and
 * the remap are those of the module. The display is turned on after the
 * memory is cleared.
 */
static const ssd1322_init_cmd_t st_init_cmds[] = {
    {SSD1322_CMD_COMMANDLOCK, 1, {0x12}},                      // unlock
    {SSD1322_CMD_DISPLAYOFF, 0, {0}},                          //
    {SSD1322_CMD_CLOCKDIV, 1, {0x91}},                         // about 80 frames a second
    {SSD1322_CMD_MUXRATIO, 1, {0}},                            // rows of the module
    {SSD1322_CMD_DISPLAYOFFSET, 1, {0x00}},                    //
    {SSD1322_CMD_STARTLINE, 1, {0x00}},                        //
    {SSD1322_CMD_SETREMAP, 2, {0}},                            // of the module
    {SSD1322_CMD_SETGPIO, 1, {0x00}},                          // GPIO pins of the chip off
    {SSD1322_CMD_FUNCTIONSELECT, 1, {0x01}},                   // internal VDD regulator
    {SSD1322_CMD_SETVSL, 2, {0xA0, 0xFD}},                     // external VSL, better low grays
    {SSD1322_CMD_CONTRAST, 1, {0x9F}},                         //
    {SSD1322_CMD_MASTERCONTRAST, 1, {0x0F}},                   //
    {SSD1322_CMD_DEFAULTGRAY, 0, {0}},                         // linear gray table
    {SSD1322_CMD_PRECHARGE, 1, {0xE2}},                        // phase 1 and 2 lengths
    {SSD1322_CMD_DISPLAYENHANCE, 2, {0x82, 0x20}},             //
    {SSD1322_CMD_PRECHARGELEVEL, 1, {0x1F}},                   //
    {SSD1322_CMD_PRECHARGE2, 1, {0x08}},                       // second precharge period
    {SSD1322_CMD_VCOMH, 1, {0x07}},                            //
    {SSD1322_CMD_NORMALDISPLAY, 0, {0}},                       //
    {0, 0xff, {0}}                                             // end
};

/* the NHD-2.7-12864WDW3 by its datasheet: other phase lengths and VCOMH, the enhancement B and second precharge left at reset */
static const ssd1322_init_cmd_t st_init_cmds_nhd27[] = {
    {SSD1322_CMD_COMMANDLOCK, 1, {0x12}},    //
    {SSD1322_CMD_DISPLAYOFF, 0, {0}},        //
    {SSD1322_CMD_CLOCKDIV, 1, {0x91}},       //
    {SSD1322_CMD_MUXRATIO, 1, {0}},          //
    {SSD1322_CMD_DISPLAYOFFSET, 1, {0x00}},  //
    {SSD1322_CMD_STARTLINE, 1, {0x00}},      //
    {SSD1322_CMD_FUNCTIONSELECT, 1, {0x01}}, //
    {SSD1322_CMD_SETREMAP, 2, {0}},          //
    {SSD1322_CMD_MASTERCONTRAST, 1, {0x0F}}, //
    {SSD1322_CMD_CONTRAST, 1, {0x9F}},       //
    {SSD1322_CMD_PRECHARGE, 1, {0xF2}},      //
    {SSD1322_CMD_PRECHARGELEVEL, 1, {0x1F}}, //
    {SSD1322_CMD_SETVSL, 2, {0xA0, 0xFD}},   //
    {SSD1322_CMD_VCOMH, 1, {0x04}},          //
    {SSD1322_CMD_DEFAULTGRAY, 0, {0}},       //
    {SSD1322_CMD_NORMALDISPLAY, 0, {0}},     //
    {0, 0xff, {0}}                           // end
};

/*
 * Remap, first byte: A[1] columns counted from segment 479, A[2] nibble
 * remap, A[4] COM scan from the last row up, A[5] COM split odd/even.
 * With A[2] the left pixel of a byte is its high nibble and the bytes go
 * along the glass from its first column, with A[1] or without: A[1] only
 * tells which way the segments of the glass run. Second byte: B[4] dual
 * COM mode; B[0] is always 1.
 */
typedef struct {
    uint16_t                  width, height; /* the glass */
    uint8_t                   col_offset;    /* the column the glass starts at */
    uint8_t                   segments;      /* segments a pixel: 2 on a glass that drives every pixel by two */
    uint8_t                   remap[2];
    const ssd1322_init_cmd_t *init;
} ssd1322_module_t;

static const ssd1322_module_t ssd1322_modules[] = {
    [SSD1322_256X64]         = {256, 64, 28, 1, {0x14, 0x11}, st_init_cmds},       /* upright on a 3.12" clone */
    [SSD1322_256X64_ZJY]     = {256, 64, 24, 1, {0x16, 0x11}, st_init_cmds},       /* u8g2 issue 2386 */
    [SSD1322_240X128]        = {240, 128, 24, 1, {0x36, 0x11}, st_init_cmds},      /* u8g2 issue 2092 */
    [SSD1322_240X128_TOPWIN] = {240, 128, 24, 1, {0x36, 0x01}, st_init_cmds},      /* u8g2 discussion 2308 */
    [SSD1322_128X64]         = {128, 64, 28, 2, {0x16, 0x11}, st_init_cmds_nhd27}, /* NHD-2.7-12864WDW3 */
};

typedef struct _dgx_ssd1322_t {
    dgx_vscreen_t           base;
    dgx_bus_protocols_t    *bus;
    const ssd1322_module_t *module;
} dgx_ssd1322_t;

static const char TAG[] = "DGX SSD1322";

static void dgx_ssd1322_command(dgx_ssd1322_t *scr, uint8_t cmd, const uint8_t *data, uint32_t len)
{
    dgx_bus_protocols_t *bus = scr->bus;
    bus->write_command(bus, cmd);
    if (len)
        bus->write_data(bus, data, len * 8u);
    else
        bus->sync_write(bus); /* a bus that holds a command back until its data sends it now */
}

/* the window of memory the next pixels go to, columns of 4 pixels; the pixels follow with write_data_async() */
static void dgx_ssd1322_window(dgx_ssd1322_t *scr, int col0, int col1, int row0, int row1)
{
    const uint8_t cols[2] = {(uint8_t)col0, (uint8_t)col1};
    const uint8_t rows[2] = {(uint8_t)row0, (uint8_t)row1};
    dgx_ssd1322_command(scr, SSD1322_CMD_SETCOLUMN, cols, 2);
    dgx_ssd1322_command(scr, SSD1322_CMD_SETROW, rows, 2);
    scr->bus->write_command(scr->bus, SSD1322_CMD_WRITERAM); /* goes out with the pixels */
}

/* the gray, 0..15, of the screen pixel shown at (px, py) of the panel; the left pixel of a byte is its high nibble */
static inline uint8_t dgx_ssd1322_gray(const dgx_ssd1322_t *scr, int px, int py)
{
    int sx, sy;
    dgx_soft_turn_pixel(&scr->base.base, scr->module->width, scr->module->height, px, py, &sx, &sy);
    const uint32_t offset = (uint32_t)sx + (uint32_t)sy * (uint32_t)scr->base.base.width;
    const uint8_t  byte   = scr->base.v_array[offset / 2];
    return (offset & 1) ? (byte & 0x0f) : (byte >> 4);
}

/*
 * Panel pixels x0..x1 of panel row py, whole columns of the controller: a
 * column is 4 pixels, or 2 on a glass of two segments a pixel.
 */
static void dgx_ssd1322_send_run(dgx_ssd1322_t *scr, int py, int x0, int x1)
{
    dgx_bus_protocols_t    *bus   = scr->bus;
    const ssd1322_module_t *m     = scr->module;
    const int               per   = 4 / m->segments; /* pixels a column */
    const uint32_t          bytes = (uint32_t)(x1 - x0 + 1) * m->segments / 2u;
    dgx_ssd1322_window(scr, m->col_offset + x0 / per, m->col_offset + x1 / per, py, py);
    if (m->segments == 1 && !dgx_soft_turned(&scr->base.base)) {
        /* the row lies in the virtual screen as the controller takes it */
        bus->write_data_async(bus, scr->base.v_array + ((size_t)py * m->width + (size_t)x0) / 2, bytes * 8u);
        return;
    }
    /* the window above waited for the transfer before it, so the buffer of the bus is free */
    uint8_t *row = bus->buffer;
    if (m->segments == 1) {
        for (int px = x0, at = 0; px <= x1; px += 2, ++at)
            row[at] = (uint8_t)(dgx_ssd1322_gray(scr, px, py) << 4 | dgx_ssd1322_gray(scr, px + 1, py));
    } else {
        for (int px = x0, at = 0; px <= x1; ++px, ++at) row[at] = (uint8_t)(dgx_ssd1322_gray(scr, px, py) * 0x11u);
    }
    bus->write_data_async(bus, row, bytes * 8u);
}

static void dgx_ssd1322_update_screen(dgx_screen_t *_scr, int left, int right, int top, int bottom)
{
    dgx_ssd1322_t *scr = (dgx_ssd1322_t *)_scr;
    ESP_LOGD(TAG, "update: left=%d, right=%d, top=%d, bottom=%d", left, right, top, bottom);
    if (!dgx_soft_turn_rect(_scr, scr->module->width, scr->module->height, &left, &right, &top, &bottom)) return;
    /* out to whole columns of the controller */
    const int per = 4 / scr->module->segments;
    left &= ~(per - 1);
    right |= per - 1;
    for (int py = top; py <= bottom; ++py) dgx_ssd1322_send_run(scr, py, left, right);
    scr->bus->sync_write(scr->bus);
}

void dgx_ssd1322_contrast(dgx_screen_t *_scr, uint8_t contrast)
{
    dgx_ssd1322_command((dgx_ssd1322_t *)_scr, SSD1322_CMD_CONTRAST, &contrast, 1);
}

void dgx_ssd1322_brightness(dgx_screen_t *_scr, uint8_t brightness)
{
    if (brightness > 15) brightness = 15;
    dgx_ssd1322_command((dgx_ssd1322_t *)_scr, SSD1322_CMD_MASTERCONTRAST, &brightness, 1);
}

bool dgx_ssd1322_grayscale(dgx_screen_t *_scr, const uint8_t *levels)
{
    dgx_ssd1322_t *scr = (dgx_ssd1322_t *)_scr;
    if (!levels) {
        dgx_ssd1322_command(scr, SSD1322_CMD_DEFAULTGRAY, NULL, 0);
        return true;
    }
    /* B8h takes GS1..GS15, each above the one before and none above 180 DCLKs; GS0 is not set */
    for (int i = 2; i < 16; ++i) {
        if (levels[i] > 180 || levels[i] <= levels[i - 1]) {
            ESP_LOGE(TAG, "gray %d (%u) is above 180 or not above gray %d", i, (unsigned)levels[i], i - 1);
            return false;
        }
    }
    dgx_ssd1322_command(scr, SSD1322_CMD_SETGRAY, levels + 1, 15);
    dgx_ssd1322_command(scr, SSD1322_CMD_ENABLEGRAY, NULL, 0); /* the datasheet requires it after B8h */
    return true;
}

void dgx_ssd1322_display_mode(dgx_screen_t *_scr, uint8_t mode)
{
    static const uint8_t cmds[4] = {SSD1322_CMD_NORMALDISPLAY, SSD1322_CMD_DISPLAYALLOFF, SSD1322_CMD_DISPLAYALLON,
                                    SSD1322_CMD_INVERTDISPLAY};
    if (mode > 3) return;
    dgx_ssd1322_command((dgx_ssd1322_t *)_scr, cmds[mode], NULL, 0);
}

dgx_screen_t *dgx_ssd1322_init(dgx_bus_protocols_t *bus, ssd1322_resolution_t resolution, gpio_num_t rst)
{
    if ((unsigned)resolution >= sizeof(ssd1322_modules) / sizeof(ssd1322_modules[0])) {
        ESP_LOGE(TAG, "unsupported resolution %d", (int)resolution);
        return NULL;
    }
    const ssd1322_module_t *m     = &ssd1322_modules[resolution];
    const int               width = m->width, height = m->height;
    if (!bus || !bus->buffer || bus->buffer_len < (size_t)width * m->segments / 2u) {
        ESP_LOGE(TAG, "no bus, or its buffer cannot hold a row");
        return NULL;
    }
    /* the rows go to the bus straight from it: memory of the bus */
    dgx_vscreen_t *vscreen = (dgx_vscreen_t *)dgx_vscreen_init_with(width, height, 4, DgxScreenRGB, bus->memory_allocator);
    if (!vscreen) {
        ESP_LOGE(TAG, "virtual screen initialization failed");
        return NULL;
    }
    dgx_ssd1322_t *scr = (dgx_ssd1322_t *)calloc(1, sizeof(dgx_ssd1322_t));
    if (!scr) {
        ESP_LOGE(TAG, "SSD1322 screen structure memory allocation failed");
        free(vscreen->v_array);
        free(vscreen);
        return NULL;
    }
    scr->base = *vscreen;
    free(vscreen); /* its pixels now belong to the screen */
    scr->base.base.screen_name    = "SSD1322";
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.update_screen  = dgx_ssd1322_update_screen; /* the rest, destroy too, is the virtual screen's */
    scr->bus                      = bus;
    scr->module                   = m;
    if (rst != GPIO_NUM_NC) {
        /* a low pulse of 100 us at least */
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(1);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(1);
        dgx_gpio_set_level(rst, 1);
    }
    /* the chip takes commands 300 ms after its power comes up, and the program may start sooner */
    dgx_delay(300);
    const uint8_t mux = (uint8_t)(height - 1);
    for (const ssd1322_init_cmd_t *c = m->init; c->databytes != 0xff; ++c) {
        ESP_LOGD(TAG, "init command 0x%02x", c->cmd);
        const uint8_t *data = c->cmd == SSD1322_CMD_MUXRATIO ? &mux : c->cmd == SSD1322_CMD_SETREMAP ? m->remap : c->data;
        dgx_ssd1322_command(scr, c->cmd, data, c->databytes);
    }
    /* the memory holds noise after power-up: clear the glass before it is lit */
    dgx_ssd1322_update_screen(&scr->base.base, 0, width - 1, 0, height - 1);
    dgx_ssd1322_command(scr, SSD1322_CMD_DISPLAYON, NULL, 0);
    return (dgx_screen_t *)scr;
}

void dgx_ssd1322_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_ssd1322_t *scr = (dgx_ssd1322_t *)_scr;
    dgx_soft_turn_set(_scr, scr->base.v_array, scr->module->width, scr->module->height, dir_x, dir_y, swap_xy);
    dgx_ssd1322_update_screen(_scr, 0, _scr->width - 1, 0, _scr->height - 1);
}
