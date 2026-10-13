/*
 * The 8-bit parallel bus, src/bus/p8_esp32.c, with a screen on it, against
 * a model of what the display gets on its pins.
 *
 * The driver of IDF under the bus is one of two. On an ESP32 and an
 * ESP32-S2 the bus is made of the I2S peripheral: a transfer is a command
 * byte and then the data, the command is sent whatever its number, -1 goes
 * out as 0xFF, and on an ESP32 pixels go in pairs of bytes, so an odd number
 * of them gets a byte more. On an ESP32-S3 and an ESP32-P4 it is the LCD_CAM
 * peripheral, and a command of -1 sends no command. The model here does what
 * those drivers do; which of the two is chosen by TEST_LCDCAM, see
 * test_p8_lcdcam.c.
 *
 * The display is a controller of the MIPI display command set: 0x2A and 0x2B
 * set the window, 0x2C writes memory from the start of it, any other command
 * ends the writing, and data that follows a command the display does not
 * know is lost.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "bus/p8_esp32.c"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

#define PW 120
#define PH 90

#ifdef TEST_LCDCAM
static const bool lcdcam = true;
#else
static const bool lcdcam = false;
#endif

/* ---- the display ---- */

static uint32_t panel[PW * PH];
static int      panel_pixel_bytes = 2;
static int      panel_command = -1;                       /* the command the data that comes belongs to */
static int      win_l, win_r = PW - 1, win_t, win_b = PH - 1;
static int      at_x, at_y;                               /* the next pixel written */
static uint8_t  part[4];                                  /* bytes of a pixel or of a bound that is not whole yet */
static int      part_bytes;
static long     stray_bytes;                              /* data the display took for parameters of a command it does not know */
static long     unknown_commands, written_pixels;
static long     data_alone;                               /* transfers of data with no command, as the bus asked for them */
static long     longest_transfer;

static void panel_take_command(uint8_t command)
{
    panel_command = command;
    part_bytes = 0; /* a pixel that is not whole is dropped */
    switch (command) {
    case 0x2A:
    case 0x2B:
        break;
    case 0x2C:
        at_x = win_l, at_y = win_t;
        break;
    case 0x36:
    case 0x3A:
        break; /* commands of a driver the model lets through */
    default:
        ++unknown_commands;
    }
}

static void panel_take_data(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        switch (panel_command) {
        case 0x2A:
        case 0x2B:
            if (part_bytes < 4) part[part_bytes++] = data[i];
            if (part_bytes == 4) {
                int from = part[0] << 8 | part[1], to = part[2] << 8 | part[3];
                CHECK(from <= to && to < (panel_command == 0x2A ? PW : PH));
                if (panel_command == 0x2A) win_l = from, win_r = to;
                else win_t = from, win_b = to;
                ++part_bytes; /* more than four is a mistake of the sender, and is not taken */
            }
            break;
        case 0x2C:
            part[part_bytes++] = data[i];
            if (part_bytes == panel_pixel_bytes) {
                uint32_t color = 0;
                for (int k = 0; k < panel_pixel_bytes; ++k) color = color << 8 | part[k];
                panel[at_x + at_y * PW] = color;
                ++written_pixels;
                part_bytes = 0;
                if (++at_x > win_r) {
                    at_x = win_l;
                    if (++at_y > win_b) at_y = win_t;
                }
            }
            break;
        case 0x36:
        case 0x3A:
            break;
        default:
            ++stray_bytes;
        }
    }
}

/* ---- the driver of IDF ---- */

struct esp_lcd_i80_bus_t {
    size_t max_transfer_bytes;
};
struct esp_lcd_panel_io_t {
    esp_lcd_panel_io_color_trans_done_cb_t done;
    void                                  *user;
    size_t                                 max_transfer_bytes;
};

esp_err_t esp_lcd_new_i80_bus(const esp_lcd_i80_bus_config_t *bus_config, esp_lcd_i80_bus_handle_t *ret_bus)
{
    *ret_bus = calloc(1, sizeof(**ret_bus));
    (*ret_bus)->max_transfer_bytes = bus_config->max_transfer_bytes;
    return ESP_OK;
}

esp_err_t esp_lcd_del_i80_bus(esp_lcd_i80_bus_handle_t bus)
{
    free(bus);
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_io_i80(esp_lcd_i80_bus_handle_t bus, const esp_lcd_panel_io_i80_config_t *io_config, esp_lcd_panel_io_handle_t *ret_io)
{
    *ret_io = calloc(1, sizeof(**ret_io));
    (*ret_io)->done = io_config->on_color_trans_done;
    (*ret_io)->user = io_config->user_ctx;
    (*ret_io)->max_transfer_bytes = bus->max_transfer_bytes;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io)
{
    free(io);
    return ESP_OK;
}

static void driver_command(int lcd_cmd)
{
    if (lcd_cmd == -1) {
        ++data_alone;
        if (lcdcam) return; /* no command phase */
    }
    panel_take_command((uint8_t)lcd_cmd);
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    (void)io;
    CHECK(param_size <= 32); /* LCD_I80_IO_FORMAT_BUF_SIZE: the driver asserts it */
    driver_command(lcd_cmd);
    if (param && param_size) panel_take_data(param, param_size);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *color, size_t color_size)
{
    CHECK(color && color_size && color_size <= io->max_transfer_bytes);
    if ((long)color_size > longest_transfer) longest_transfer = (long)color_size;
    driver_command(lcd_cmd);
    panel_take_data(color, color_size);
    if (!lcdcam && (color_size & 1)) {
        /* an ESP32 sends pixels two bytes at a time: the second of the last pair is whatever lay next */
        const uint8_t next = 0xEE;
        panel_take_data(&next, 1);
    }
    io->done(io, NULL, io->user);
    return ESP_OK;
}

/* ---- the test ---- */

static uint32_t seed = 12345;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static uint32_t rnd_color(int bits)
{
    return bits == 16 ? (uint32_t)rnd(0, 0xffff) : (uint32_t)rnd(0, 0xffff) << 8 | (uint32_t)rnd(0, 0xff);
}

static void run(int bits, int rounds)
{
    panel_pixel_bytes = bits == 16 ? 2 : 3;
    memset(panel, 0, sizeof(panel));
    panel_command = -1, part_bytes = 0;
    win_l = win_t = 0, win_r = PW - 1, win_b = PH - 1;

    dgx_bus_protocols_t *bus = dgx_p8_init(1, 2, 3, 4, 5, 6, 7, 8, 9, GPIO_NUM_NC, 10, 11, 10 * 1000 * 1000);
    CHECK(bus && bus->bus_type == DGX_BUS_P8);
    dgx_screen_with_bus_t sbus;
    memset(&sbus, 0, sizeof(sbus));
    dgx_screen_t *lcd = &sbus.scr;
    sbus.bus = bus;
    lcd->width = PW, lcd->height = PH, lcd->color_bits = (uint8_t)bits, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    dgx_screen_with_bus_init_area(&sbus, 0x2A, 0x2B, 0x2C, 0, DGX_SCREEN_AREA_PROTO_STD16);
    /* as a driver sets its controller up: a command with its data, and one with none */
    const uint8_t colmod = 0x55;
    bus->write_command(bus, 0x3A);
    bus->write_data(bus, &colmod, 8);
    bus->write_command(bus, 0x36);
    bus->sync_write(bus);
    dgx_scr_init_slow_bus_optimized_funcs(lcd);

    dgx_screen_t *model = dgx_vscreen_init(PW, PH, (uint8_t)bits, DgxScreenRGB);
    dgx_screen_t *picture = dgx_vscreen_init(37, 23, (uint8_t)bits, DgxScreenRGB);
    CHECK(model && picture);
    for (int y = 0; y < picture->height; ++y) {
        for (int x = 0; x < picture->width; ++x) dgx_set_pixel(picture, x, y, rnd_color(bits));
    }
    dgx_screen_t *both[2] = {lcd, model};

    for (int round = 0; round < rounds; ++round) {
        const int      what = rnd(0, 9), x = rnd(-10, PW + 5), y = rnd(-10, PH + 5);
        const int      w = rnd(1, PW + 10), h = rnd(1, PH + 10), r = rnd(0, 30);
        const uint32_t color = rnd_color(bits);
        const dgx_output_orientation_t turn = (dgx_output_orientation_t)rnd(0, 7);
        const int      part_x = rnd(-3, 20), part_y = rnd(-3, 12), part_w = rnd(1, 40), part_h = rnd(1, 25);
        const dgx_point_2d_t quad[4] = {{(int16_t)x, (int16_t)y}, {(int16_t)(x + w / 2), (int16_t)(y + rnd(-5, 5))},
                                        {(int16_t)(x + w / 2 + rnd(-5, 5)), (int16_t)(y + h / 2)}, {(int16_t)(x + rnd(-5, 5)), (int16_t)(y + h / 2)}};
        for (int k = 0; k < 2; ++k) {
            dgx_screen_t *scr = both[k];
            switch (what) {
            case 0:
                dgx_fill_rectangle(scr, x, y, w, h, color); /* often more than the buffer of the bus holds */
                break;
            case 1:
                dgx_fill_rectangle(scr, 0, 0, PW, PH, color);
                break;
            case 2:
                dgx_set_pixel(scr, x, y, color);
                break;
            case 3:
                dgx_draw_line(scr, x, y, x + w - 20, y + h - 20, color);
                break;
            case 4:
                dgx_solid_circle(scr, x, y, r, color);
                break;
            case 5:
                dgx_font_string_utf8_screen(scr, (int16_t)x, (int16_t)y, "P8 bus", color, DgxOutputNormal, 1 + (round & 1), TerminusTTFMedium12(), NULL, NULL);
                break;
            case 6:
                dgx_vscreen_to_screen(scr, x, y, picture);
                break;
            case 7:
                dgx_vscreen_region_to_screen(scr, x, y, picture, part_x, part_y, part_w, part_h);
                break;
            case 8:
                dgx_draw_texture_quad(scr, quad, picture, 0, 0, picture->width, picture->height);
                break;
            default:
                dgx_vscreen_to_screen_oriented(scr, x, y, picture, turn);
                break;
            }
        }
        if (round % 16 == 15 || round == rounds - 1) {
            lcd->wait_buffer(lcd);
            int wrong = 0;
            for (int py = 0; py < PH; ++py) {
                for (int px = 0; px < PW; ++px) wrong += panel[px + py * PW] != dgx_get_pixel(model, px, py);
            }
            CHECK(wrong == 0);
            if (wrong) break;
        }
    }
    /* nothing went to the display as data of a command it does not know, and nothing was asked to go with no command */
    CHECK(stray_bytes == 0);
    CHECK(unknown_commands == 0);
    CHECK(data_alone == 0);
    CHECK(written_pixels > 0);
    /* a fill of the whole screen is more than a transfer takes: it went in several, each with its window */
    CHECK(longest_transfer <= 4000 && longest_transfer > 3000);

    dgx_screen_destroy(&model);
    dgx_screen_destroy(&picture);
    bus->dispose(bus);
}

/* a screen whose window lies off it takes no pixels, and does not put them into the window before */
static void test_window_off_screen(void)
{
    panel_pixel_bytes = 2;
    dgx_bus_protocols_t  *bus = dgx_p8_init(1, 2, 3, 4, 5, 6, 7, 8, 9, GPIO_NUM_NC, 10, 11, 10 * 1000 * 1000);
    dgx_screen_with_bus_t sbus;
    memset(&sbus, 0, sizeof(sbus));
    dgx_screen_t *lcd = &sbus.scr;
    sbus.bus = bus;
    lcd->width = PW, lcd->height = PH, lcd->color_bits = 16, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    dgx_screen_with_bus_init_area(&sbus, 0x2A, 0x2B, 0x2C, 0, DGX_SCREEN_AREA_PROTO_STD16);
    dgx_scr_init_slow_bus_optimized_funcs(lcd);
    dgx_fill_rectangle(lcd, 0, 0, PW, PH, 0x1111);
    lcd->wait_buffer(lcd);
    uint8_t pixels[8] = {0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd};
    lcd->set_area(lcd, 3, 6, 2, 2);
    lcd->set_area(lcd, PW, PW + 3, 2, 2);
    lcd->write_area(lcd, pixels, 64);
    lcd->wait_buffer(lcd);
    int changed = 0;
    for (int i = 0; i < PW * PH; ++i) changed += panel[i] != 0x1111;
    CHECK(changed == 0);
    lcd->set_area(lcd, 3, 6, 2, 2);
    lcd->write_area(lcd, pixels, 64);
    lcd->wait_buffer(lcd);
    for (int x = 3; x <= 6; ++x) CHECK(panel[x + 2 * PW] == 0xabcd);
    bus->dispose(bus);
}

int main(void)
{
    run(16, 3000);
    run(24, 3000);
    test_window_off_screen();
    CHECK(stray_bytes == 0 && data_alone == 0);
    CHECK_DONE();
}
