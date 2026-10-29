/*
 * The ST7920 driver against a model of the controller on its serial line:
 * what the panel shows has to be the screen as it lies on the panel, in all
 * eight orientations. The controller has no command for an orientation, the
 * driver turns the picture where it sends it.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h"

/*
 * The pins the driver drives, as the test sees them: the chip select, when
 * it is wired, is high for every transfer, and a pin that is not wired is
 * never touched.
 */
static int cs_pin = GPIO_NUM_NC, cs_level, unwired_calls;

static esp_err_t pin_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin < 0) ++unwired_calls;
    else if (pin == cs_pin) cs_level = (int)level;
    return ESP_OK;
}

static esp_err_t pin_set_direction(gpio_num_t pin, gpio_mode_t mode)
{
    (void)mode;
    if (pin < 0) ++unwired_calls;
    return ESP_OK;
}

#define dgx_gpio_set_level(pin, level)    pin_set_level(pin, level)
#define dgx_gpio_set_direction(pin, mode) pin_set_direction(pin, mode)

#include "drivers/st7920.c"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "fonts/TerminusTTFMedium12.h"

#define PW 128
#define PH 64

/* ---- the controller: graphic memory of 32 rows of 16 words, the lower half of the panel in the words 8 to 15 ---- */

static uint8_t panel[PH][PW];
static int     gdram_y, gdram_x;
static bool    next_is_x; /* the address comes as two commands, the row first */
static uint8_t word[2];
static int     word_bytes;
static long    data_bytes, transfers;

static void st_command(uint8_t value)
{
    word_bytes = 0;
    if (!(value & 0x80)) {
        next_is_x = false;
        return; /* the commands of the start-up */
    }
    if (!next_is_x) gdram_y = value & 0x3f;
    else gdram_x = value & 0x0f;
    next_is_x = !next_is_x;
}

static void st_data(uint8_t value)
{
    ++data_bytes;
    word[word_bytes++] = value;
    if (word_bytes < 2) return;
    word_bytes = 0;
    CHECK(gdram_y < 32 && gdram_x < 16);
    const int y = gdram_x < 8 ? gdram_y : gdram_y + 32, x0 = (gdram_x & 7) * 16;
    if (gdram_y < 32 && gdram_x < 16) {
        for (int bit = 0; bit < 16; ++bit) panel[y][x0 + bit] = (word[bit >> 3] >> (7 - (bit & 7))) & 1u;
    }
    ++gdram_x;
}

static void line_write_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    (void)bus;
    const uint32_t bytes = len / 8;
    ++transfers;
    CHECK(cs_pin < 0 || cs_level == 1);
    CHECK(len % 8 == 0 && bytes >= 3 && (bytes & 1));
    CHECK(data[0] == 0xf8 || data[0] == 0xfa);
    for (uint32_t i = 1; i + 1 < bytes; i += 2) {
        CHECK(!(data[i] & 0x0f) && !(data[i + 1] & 0x0f));
        const uint8_t value = (uint8_t)(data[i] | data[i + 1] >> 4);
        if (data[0] == 0xf8) st_command(value);
        else st_data(value);
    }
}

/* where a pixel of the screen lies on the panel: as the MX, MY and MV bits of a color controller put it */
static void on_panel(const dgx_screen_t *scr, int x, int y, int *px, int *py)
{
    const int ax = scr->swap_xy ? y : x, ay = scr->swap_xy ? x : y;
    *px = scr->dir_x == DgxScreenRightLeft ? PW - 1 - ax : ax;
    *py = scr->dir_y == DgxScreenBottomTop ? PH - 1 - ay : ay;
}

static int mismatches(dgx_screen_t *scr)
{
    int wrong = 0;
    for (int y = 0; y < scr->height; ++y) {
        for (int x = 0; x < scr->width; ++x) {
            int px, py;
            on_panel(scr, x, y, &px, &py);
            wrong += panel[py][px] != (dgx_get_pixel(scr, x, y) != 0);
        }
    }
    return wrong;
}

static uint32_t seed = 4242;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

int main(void)
{
    dgx_bus_protocols_t bus = {.bus_type = DGX_BUS_SPI, .write_data = line_write_data};
    memset(panel, 1, sizeof(panel)); /* what the memory of the controller holds at power-up is not known */
    cs_pin            = 5;
    dgx_screen_t *scr = dgx_st7920_init(&bus, GPIO_NUM_NC, cs_pin);
    CHECK(scr && scr->width == PW && scr->height == PH);
    if (!scr) CHECK_DONE();

    for (int turn = 0; turn < 8; ++turn) {
        const dgx_orientation_t dir_x = turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight;
        const dgx_orientation_t dir_y = turn & 2 ? DgxScreenBottomTop : DgxScreenTopBottom;
        const bool              swap = (turn & 4) != 0;
        /* something on the screen before the call: it stays when the size does, and is cleared when the size changes */
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0);
        dgx_fill_rectangle(scr, 3, 5, 20, 9, 1);
        dgx_draw_line(scr, 0, 0, scr->width - 1, scr->height - 1, 1);
        const bool resized = (scr->width == PW) == swap;
        dgx_st7920_orientation(scr, dir_x, dir_y, swap);
        CHECK(scr->width == (swap ? PH : PW) && scr->height == (swap ? PW : PH));
        CHECK(mismatches(scr) == 0);
        int lit = 0;
        for (int y = 0; y < scr->height; ++y) {
            for (int x = 0; x < scr->width; ++x) lit += dgx_get_pixel(scr, x, y) != 0;
        }
        CHECK(resized ? lit == 0 : lit > 180);

        /* the picture: a corner mark that tells the orientations apart, then whatever */
        dgx_fill_rectangle(scr, 0, 0, 5, 2, 1);
        int px, py;
        on_panel(scr, 0, 0, &px, &py);
        CHECK(panel[py][px] == 1);
        CHECK(px == ((turn & 1) ? PW - 1 : 0) && py == ((turn & 2) ? PH - 1 : 0));
        /* the mark is 5 pixels long along x of the screen: along the rows of the panel, or down its columns when the axes are swapped */
        on_panel(scr, 4, 0, &px, &py);
        CHECK(panel[py][px] == 1);
        CHECK(swap ? py == ((turn & 2) ? PH - 5 : 4) : px == ((turn & 1) ? PW - 5 : 4));

        for (int round = 0; round < 400; ++round) {
            const int x = rnd(-5, scr->width + 4), y = rnd(-5, scr->height + 4);
            switch (rnd(0, 5)) {
            case 0:
                dgx_fill_rectangle(scr, x, y, rnd(1, 40), rnd(1, 30), (uint32_t)rnd(0, 1));
                break;
            case 1:
                dgx_set_pixel(scr, x, y, (uint32_t)rnd(0, 1));
                break;
            case 2:
                dgx_draw_line(scr, x, y, rnd(-5, scr->width + 4), rnd(-5, scr->height + 4), (uint32_t)rnd(0, 1));
                break;
            case 3:
                dgx_solid_circle(scr, x, y, rnd(0, 20), (uint32_t)rnd(0, 1));
                break;
            case 4:
                dgx_font_string_utf8_screen(scr, (int16_t)x, (int16_t)y, "ST7920", 1, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
                break;
            default:
                /* a batch: one update for all of it */
                dgx_screen_progress_up(scr);
                dgx_draw_circle(scr, x, y, rnd(1, 25), 1);
                dgx_fill_rectangle(scr, x, y, 3, 3, 0);
                dgx_screen_progress_down(scr);
                break;
            }
            const int wrong = mismatches(scr);
            CHECK(wrong == 0);
            if (wrong) break;
        }
        /* a pixel costs the panel the word it lies in, not the screen: 2 commands of 3 bytes and 1 + 4 bytes of data */
        const long before = data_bytes, sent = transfers;
        dgx_set_pixel(scr, scr->width / 2, scr->height / 2, 1);
        CHECK(data_bytes - before == 2 && transfers - sent == 3);
    }
    dgx_screen_destroy(&scr);
    /* the reset pin is not wired: nothing is done with it */
    CHECK(unwired_calls == 0);

    /* nor is the chip select, tied high: the panel takes every transfer as before */
    cs_pin = GPIO_NUM_NC;
    scr    = dgx_st7920_init(&bus, GPIO_NUM_NC, cs_pin);
    CHECK(scr != NULL);
    if (scr) {
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, 0);
        dgx_font_string_utf8_screen(scr, 3, 20, "ST7920", 1, DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
        dgx_st7920_orientation(scr, DgxScreenRightLeft, DgxScreenBottomTop, false);
        CHECK(mismatches(scr) == 0);
        dgx_screen_destroy(&scr);
    }
    CHECK(unwired_calls == 0);
    CHECK_DONE();
}
