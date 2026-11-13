/*
 * The SSD1680 driver against a model of the controller. As the datasheet
 * has it: a byte with D/C# low is a command and its arguments come with
 * D/C# high; while BUSY is high the chip takes nothing; in deep sleep it
 * takes nothing until a hardware reset; 24h and 26h write the planes at the
 * address counter, X first and Y after it within the window (11h = 03h),
 * a byte for 8 sources with the first one in its high bit. 20h after 22h
 * F7h shows the planes on the glass, red where the red bit is set, white
 * where the black-and-white bit is; with sources from S8 (21h) the glass
 * starts at bit 7 of byte 0. What the glass shows after a refresh has to
 * be the screen as it lies on the panel, on both modules, in all eight
 * orientations.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h"

#define PIN_RST  16
#define PIN_BUSY 4

static void pin_level(int pin, uint32_t level);
static int  busy_level(int pin);

static esp_err_t pin_set_level(gpio_num_t pin, uint32_t level)
{
    pin_level(pin, level);
    return ESP_OK;
}

static esp_err_t pin_set_direction(gpio_num_t pin, gpio_mode_t mode)
{
    (void)mode;
    CHECK(pin >= 0);
    return ESP_OK;
}

#define dgx_gpio_set_level(pin, level)    pin_set_level(pin, level)
#define dgx_gpio_set_direction(pin, mode) pin_set_direction(pin, mode)
#define dgx_gpio_get_level(pin)           busy_level(pin)

#include "drivers/ssd1680.c"

#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

#define XBYTES 22  /* 176 sources */
#define GATES  296

/* the module under test: its glass */
static int pw, ph;

/* ---- the controller ---- */

static uint8_t ram_bw[GATES][XBYTES], ram_red[GATES][XBYTES];
static uint8_t glass[GATES][176]; /* the ink of each pixel after the last refresh */
static int     xs, xe, ys, ye, xc, yc;
static int     entry, border, sensor, ctrl2, refreshes;
static uint8_t output[3], ctrl1[2];
static bool    asleep, reset_low;
static int     busy_left;   /* reads of BUSY that still find it high */
static long    violations;  /* commands or data the chip would not have taken */
static long    sleeps, resets, sw_resets;
static uint8_t pending_cmd;
static uint8_t args[8];
static int     args_got, args_need;
static int     ram_target; /* 0x24 or 0x26 while their data come, 0 else */

static void por(void)
{
    xs = 0, xe = XBYTES - 1, ys = 0, ye = GATES - 1, xc = 0, yc = 0;
    entry = 0x03, border = 0xC0, sensor = 0x48, ctrl2 = 0xFF;
    output[0] = 0x27, output[1] = 0x01, output[2] = 0x00;
    ctrl1[0] = 0x00, ctrl1[1] = 0x00;
}

static uint32_t noise = 1;

/* a reset: out of deep sleep, the registers as at power-up, the memory left as it is (deep sleep 1) or noise */
static void pin_level(int pin, uint32_t level)
{
    CHECK(pin == PIN_RST);
    if (pin != PIN_RST) return;
    if (!level) {
        reset_low = true;
        return;
    }
    if (!reset_low) return;
    reset_low = false;
    ++resets;
    /* a refresh is not to be cut short by a reset */
    if (busy_left) ++violations;
    asleep = false;
    por();
    /* the driver sends both planes whole: what deep sleep kept must not matter */
    for (int g = 0; g < GATES; ++g)
        for (int x = 0; x < XBYTES; ++x) {
            noise   = noise * 1664525u + 1013904223u;
            ram_bw[g][x]  = (uint8_t)(noise >> 24);
            ram_red[g][x] = (uint8_t)(noise >> 16);
        }
    busy_left = 2;
}

static int busy_level(int pin)
{
    CHECK(pin == PIN_BUSY);
    if (asleep) return 1; /* BUSY stays high in deep sleep */
    if (busy_left) {
        --busy_left;
        return 1;
    }
    return 0;
}

static int arity(uint8_t cmd)
{
    switch (cmd) {
    case 0x01:
        return 3;
    case 0x10:
    case 0x11:
    case 0x18:
    case 0x22:
    case 0x3C:
    case 0x4E:
        return 1;
    case 0x21:
    case 0x44:
    case 0x4F:
        return 2;
    case 0x45:
        return 4;
    case 0x12:
    case 0x20:
    case 0x24:
    case 0x26:
        return 0;
    default:
        CHECK(!"a command the driver has no use for");
        return 0;
    }
}

/* the ink a pixel of the glass shows by the planes */
static uint8_t ink_at(int g, int s)
{
    if (ram_red[g][s >> 3] & (0x80 >> (s & 7))) return DGX_BWR_RED;
    return (ram_bw[g][s >> 3] & (0x80 >> (s & 7))) ? DGX_BWR_WHITE : DGX_BWR_BLACK;
}

static void complete(void)
{
    switch (pending_cmd) {
    case 0x01:
        memcpy(output, args, 3);
        break;
    case 0x10:
        if (args[0] & 1) {
            asleep = true;
            ++sleeps;
        }
        break;
    case 0x11:
        entry = args[0];
        break;
    case 0x12:
        por();
        ++sw_resets;
        busy_left = 3;
        break;
    case 0x18:
        sensor = args[0];
        break;
    case 0x21:
        memcpy(ctrl1, args, 2);
        break;
    case 0x22:
        ctrl2 = args[0];
        break;
    case 0x3C:
        border = args[0];
        break;
    case 0x44:
        xs = args[0], xe = args[1];
        CHECK(xs <= xe && xe < XBYTES);
        break;
    case 0x45:
        ys = args[0] | args[1] << 8, ye = args[2] | args[3] << 8;
        CHECK(ys <= ye && ye < GATES);
        break;
    case 0x4E:
        xc = args[0];
        break;
    case 0x4F:
        yc = args[0] | args[1] << 8;
        break;
    case 0x24:
    case 0x26:
        ram_target = pending_cmd;
        break;
    case 0x20: {
        /* the setup of the driver, as the module needs it */
        CHECK(ctrl2 == 0xF7);
        CHECK(output[0] == ((ph - 1) & 0xff) && output[1] == (ph - 1) >> 8 && output[2] == 0x00);
        CHECK(entry == 0x03);
        CHECK(sensor == 0x80);
        CHECK(border == 0x05);
        CHECK(ctrl1[0] == 0x00 && ctrl1[1] == 0x80); /* from S8: the glass starts at bit 7 of byte 0 */
        for (int g = 0; g < ph; ++g)
            for (int s = 0; s < pw; ++s) glass[g][s] = ink_at(g, s);
        ++refreshes;
        busy_left = 7;
        break;
    }
    default:
        break;
    }
}

static void command(uint8_t cmd)
{
    if (busy_left || asleep) ++violations;
    /* the command before did not get all its arguments */
    CHECK(args_got == args_need);
    ram_target  = 0;
    pending_cmd = cmd;
    args_got    = 0;
    args_need   = arity(cmd);
    if (!args_need) complete();
}

static void data(uint8_t value)
{
    if (busy_left || asleep) ++violations;
    if (args_got < args_need) {
        args[args_got++] = value;
        if (args_got == args_need) complete();
        return;
    }
    CHECK(ram_target != 0);
    if (!ram_target) return;
    CHECK(entry == 0x03);
    (ram_target == 0x24 ? ram_bw : ram_red)[yc][xc] = value;
    if (++xc > xe) {
        xc = xs;
        if (++yc > ye) yc = ys;
    }
}

/* ---- the bus ---- */

static void line_write_command(struct _dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    command(cmd);
}

static void line_write_commands(struct _dgx_bus_protocols_t *bus, const uint8_t *cmds, uint32_t len)
{
    (void)bus;
    for (uint32_t i = 0; i < len / 8; ++i) command(cmds[i]);
}

static void line_write_data(struct _dgx_bus_protocols_t *bus, const uint8_t *d, uint32_t len)
{
    (void)bus;
    for (uint32_t i = 0; i < len / 8; ++i) data(d[i]);
}

static void line_sync_write(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

static long allocated;

static uint8_t *line_malloc(uint32_t len)
{
    ++allocated;
    return (uint8_t *)malloc(len);
}

/* where a pixel of the screen lies on the glass: as the orientation of the driver puts it */
static void on_panel(const dgx_screen_t *scr, int x, int y, int *px, int *py)
{
    const int ax = scr->swap_xy ? y : x, ay = scr->swap_xy ? x : y;
    *px = scr->dir_x == DgxScreenRightLeft ? pw - 1 - ax : ax;
    *py = scr->dir_y == DgxScreenBottomTop ? ph - 1 - ay : ay;
}

/* a refresh, and the wait for it as a program waits */
static void show(dgx_screen_t *scr)
{
    const long before = refreshes;
    dgx_ssd1680_refresh(scr);
    CHECK(refreshes == before + 1);
    int polls = 0;
    while (dgx_ssd1680_busy(scr) && polls < 1000) ++polls;
    CHECK(polls > 0 && polls < 1000); /* the model keeps BUSY high for a few reads, for ever in deep sleep */
    CHECK(!dgx_ssd1680_busy(scr));
}

static int mismatches(dgx_screen_t *scr)
{
    int wrong = 0;
    for (int y = 0; y < scr->height; ++y) {
        for (int x = 0; x < scr->width; ++x) {
            int px, py;
            on_panel(scr, x, y, &px, &py);
            const uint32_t want = dgx_get_pixel(scr, x, y);
            if (glass[py][px] != want) {
                if (!wrong) fprintf(stderr, "first at (%d,%d) on the glass (%d,%d): want %u got %u\n", x, y, px, py, want, glass[py][px]);
                ++wrong;
            }
        }
    }
    return wrong;
}

static uint32_t seed = 8128;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static uint32_t any_ink(void)
{
    return (uint32_t)rnd(0, 2);
}

static const struct {
    ssd1680_resolution_t resolution;
    int                  width, height;
} modules[] = {
    {SSD1680_128X296_BWR, 128, 296},
    {SSD1680_122X250_BWR, 122, 250},
};

static void run(dgx_bus_protocols_t *bus, int m)
{
    pw = modules[m].width, ph = modules[m].height;
    asleep = false, busy_left = 0;
    const long resets0 = resets, sw0 = sw_resets;
    const long    allocated0 = allocated;
    dgx_screen_t *scr        = dgx_ssd1680_init(bus, modules[m].resolution, PIN_RST, PIN_BUSY);
    CHECK(scr && scr->width == pw && scr->height == ph && scr->color_bits == 8);
    CHECK(allocated == allocated0 + 2); /* both planes from the bus */
    if (!scr) return;
    CHECK(resets == resets0 + 1 && sw_resets == sw0 + 1);

    /* a new screen is white, and so is the glass after it is shown; the controller goes to sleep */
    show(scr);
    CHECK(asleep);
    CHECK(mismatches(scr) == 0);
    CHECK(glass[0][0] == DGX_BWR_WHITE && glass[ph - 1][pw - 1] == DGX_BWR_WHITE);

    /* each ink as it is: the planes as the datasheet has them, and the value read back */
    for (uint32_t ink = DGX_BWR_BLACK; ink <= DGX_BWR_RED; ++ink) {
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, DGX_BWR_WHITE);
        dgx_fill_rectangle(scr, 8, 20, 30, 40, ink);
        dgx_set_pixel(scr, 50, 70, ink);
        CHECK(dgx_get_pixel(scr, 8, 20) == ink && dgx_get_pixel(scr, 37, 59) == ink && dgx_get_pixel(scr, 50, 70) == ink);
        CHECK(dgx_get_pixel(scr, 7, 20) == DGX_BWR_WHITE && dgx_get_pixel(scr, 38, 59) == DGX_BWR_WHITE);
        show(scr);
        CHECK(glass[20][8] == ink && glass[59][37] == ink && glass[70][50] == ink);
        CHECK(glass[20][7] == DGX_BWR_WHITE && glass[60][37] == DGX_BWR_WHITE && glass[70][51] == DGX_BWR_WHITE);
        const int at = 20 * 16 + 1; /* the byte of pixels 8..15 of gate line 20 */
        const dgx_ssd1680_t *d = (const dgx_ssd1680_t *)scr;
        CHECK(d->bw[at] == (ink == DGX_BWR_BLACK ? 0x00 : 0xff) && d->red[at] == (ink == DGX_BWR_RED ? 0xff : 0x00));
    }

    /* the window of write_area(): pixels past its last one go on from its first */
    {
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, DGX_BWR_WHITE);
        uint8_t px6[6] = {DGX_BWR_BLACK, DGX_BWR_RED, DGX_BWR_BLACK, DGX_BWR_RED, DGX_BWR_RED, DGX_BWR_BLACK};
        scr->set_area(scr, 10, 11, 30, 31);
        scr->write_area(scr, px6, 6 * 8u);
        CHECK(dgx_get_pixel(scr, 10, 30) == DGX_BWR_RED && dgx_get_pixel(scr, 11, 30) == DGX_BWR_BLACK);
        CHECK(dgx_get_pixel(scr, 10, 31) == DGX_BWR_BLACK && dgx_get_pixel(scr, 11, 31) == DGX_BWR_RED);
        CHECK(dgx_get_pixel(scr, 12, 30) == DGX_BWR_WHITE && dgx_get_pixel(scr, 10, 32) == DGX_BWR_WHITE);
        /* a window off the screen takes nothing */
        scr->set_area(scr, (uint16_t)scr->width, (uint16_t)(scr->width + 3), 0, 0);
        scr->write_area(scr, px6, 6 * 8u);
        CHECK(dgx_get_pixel(scr, 0, 0) == DGX_BWR_WHITE);
    }

    for (int turn = 0; turn < 8; ++turn) {
        const dgx_orientation_t dir_x = turn & 1 ? DgxScreenRightLeft : DgxScreenLeftRight;
        const dgx_orientation_t dir_y = turn & 2 ? DgxScreenBottomTop : DgxScreenTopBottom;
        const bool              swap  = (turn & 4) != 0;
        /* what was drawn before the call stays where it is on the glass */
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, DGX_BWR_WHITE);
        dgx_fill_rectangle(scr, 3, 5, 40, 9, DGX_BWR_RED);
        int mx, my;
        on_panel(scr, 3, 5, &mx, &my);
        dgx_ssd1680_orientation(scr, dir_x, dir_y, swap);
        CHECK(scr->width == (swap ? ph : pw) && scr->height == (swap ? pw : ph));
        const long resets1 = resets;
        show(scr);
        CHECK(resets == resets1 + 1); /* woken from sleep by a reset */
        CHECK(glass[my][mx] == DGX_BWR_RED);
        CHECK(mismatches(scr) == 0);

        /* a corner mark that tells the orientations apart */
        dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, DGX_BWR_WHITE);
        dgx_fill_rectangle(scr, 0, 0, 5, 2, DGX_BWR_BLACK);
        show(scr);
        int px, py;
        on_panel(scr, 0, 0, &px, &py);
        CHECK(px == ((turn & 1) ? pw - 1 : 0) && py == ((turn & 2) ? ph - 1 : 0));
        CHECK(glass[py][px] == DGX_BWR_BLACK);
        on_panel(scr, 4, 0, &px, &py);
        CHECK(glass[py][px] == DGX_BWR_BLACK);
        CHECK(swap ? py == ((turn & 2) ? ph - 5 : 4) : px == ((turn & 1) ? pw - 5 : 4));

        for (int round = 0; round < 12; ++round) {
            for (int k = 0; k < 10; ++k) {
                const int x = rnd(-5, scr->width + 4), y = rnd(-5, scr->height + 4);
                switch (rnd(0, 6)) {
                case 0:
                    dgx_fill_rectangle(scr, x, y, rnd(-30, 60), rnd(-20, 30), any_ink());
                    break;
                case 1:
                    dgx_set_pixel(scr, x, y, any_ink());
                    break;
                case 2:
                    dgx_draw_line(scr, x, y, rnd(-5, scr->width + 4), rnd(-5, scr->height + 4), any_ink());
                    break;
                case 3:
                    dgx_solid_circle(scr, x, y, rnd(0, 20), any_ink());
                    break;
                case 4:
                    dgx_font_string_utf8_screen(scr, (int16_t)x, (int16_t)y, "SSD1680", any_ink(), (dgx_output_orientation_t)rnd(0, 7), 1,
                                                TerminusTTFMedium12(), NULL, NULL);
                    break;
                case 5: {
                    /* a picture of the same depth through the window of the screen */
                    dgx_screen_t *pic = dgx_vscreen_init(rnd(1, 30), rnd(1, 20), 8, DgxScreenRGB);
                    CHECK(pic != NULL);
                    if (!pic) break;
                    for (int py2 = 0; py2 < pic->height; ++py2)
                        for (int px2 = 0; px2 < pic->width; ++px2) dgx_set_pixel(pic, px2, py2, any_ink());
                    dgx_vscreen_to_screen(scr, x, y, pic, 0);
                    dgx_screen_destroy(&pic);
                    break;
                }
                default:
                    dgx_draw_circle(scr, x, y, rnd(1, 25), any_ink());
                    break;
                }
            }
            show(scr);
            const int wrong = mismatches(scr);
            CHECK(wrong == 0);
            if (wrong) {
                fprintf(stderr, "module %d turn %d round %d\n", m, turn, round);
                break;
            }
        }
    }
    CHECK(violations == 0);

    /* destroyed while the glass changes: the refresh is waited for, and the controller sent to sleep */
    dgx_ssd1680_refresh(scr);
    CHECK(busy_left > 0);
    const long sleeps0 = sleeps;
    dgx_screen_destroy(&scr);
    CHECK(scr == NULL);
    CHECK(busy_left == 0 && asleep && sleeps == sleeps0 + 1);
    CHECK(violations == 0);
}

int main(void)
{
    dgx_bus_protocols_t bus = {
        .bus_type       = DGX_BUS_SPI,
        .write_command  = line_write_command,
        .write_commands = line_write_commands,
        .write_data     = line_write_data,
        .sync_write     = line_sync_write,
    };
    bus.memory_allocator = line_malloc;
    por();

    /* the colors of dgx_colors.h go to the nearest of the three */
    CHECK(DGX_BLACK(dgx_rgb_to_bwr) == DGX_BWR_BLACK);
    CHECK(DGX_WHITE(dgx_rgb_to_bwr) == DGX_BWR_WHITE);
    CHECK(DGX_RED(dgx_rgb_to_bwr) == DGX_BWR_RED);
    CHECK(DGX_MAROON(dgx_rgb_to_bwr) == DGX_BWR_RED);
    CHECK(DGX_ORANGE(dgx_rgb_to_bwr) == DGX_BWR_RED);
    CHECK(DGX_NAVY(dgx_rgb_to_bwr) == DGX_BWR_BLACK);
    CHECK(DGX_GREEN(dgx_rgb_to_bwr) == DGX_BWR_BLACK);
    CHECK(DGX_CYAN(dgx_rgb_to_bwr) == DGX_BWR_WHITE);
    CHECK(DGX_LIGHTGREY(dgx_rgb_to_bwr) == DGX_BWR_WHITE);
    CHECK(DGX_PINK(dgx_rgb_to_bwr) == DGX_BWR_WHITE);
    /* and the rule is the nearest of the three, for every color */
    for (int r = 0; r < 256; r += 5)
        for (int g = 0; g < 256; g += 5)
            for (int b = 0; b < 256; b += 5) {
                const long d[3] = {
                    (long)r * r + g * g + b * b,                                         /* black */
                    (long)(255 - r) * (255 - r) + (255 - g) * (255 - g) + (255 - b) * (255 - b), /* white */
                    (long)(255 - r) * (255 - r) + g * g + b * b,                         /* red */
                };
                const uint32_t ink = dgx_rgb_to_bwr((uint8_t)r, (uint8_t)g, (uint8_t)b);
                CHECK(d[ink] <= d[0] && d[ink] <= d[1] && d[ink] <= d[2]);
            }

    /* refused: no such module, no BUSY line, no bus */
    CHECK(dgx_ssd1680_init(&bus, (ssd1680_resolution_t)(sizeof(modules) / sizeof(modules[0])), PIN_RST, PIN_BUSY) == NULL);
    CHECK(dgx_ssd1680_init(&bus, SSD1680_128X296_BWR, PIN_RST, GPIO_NUM_NC) == NULL);
    CHECK(dgx_ssd1680_init(NULL, SSD1680_128X296_BWR, PIN_RST, PIN_BUSY) == NULL);

    for (int m = 0; m < (int)(sizeof(modules) / sizeof(modules[0])); ++m) run(&bus, m);

    /* no reset line: no deep sleep, for nothing would wake the controller from it */
    pw = 128, ph = 296;
    asleep = false, busy_left = 0;
    const long resets0 = resets, sleeps0 = sleeps;
    dgx_screen_t *scr = dgx_ssd1680_init(&bus, SSD1680_128X296_BWR, GPIO_NUM_NC, PIN_BUSY);
    CHECK(scr != NULL);
    if (scr) {
        dgx_fill_rectangle(scr, 10, 10, 50, 20, DGX_BWR_BLACK);
        show(scr);
        CHECK(!asleep);
        dgx_fill_rectangle(scr, 20, 40, 50, 20, DGX_BWR_RED);
        dgx_set_pixel(scr, 0, 0, 7); /* a value above red is red */
        CHECK(dgx_get_pixel(scr, 0, 0) == DGX_BWR_RED);
        show(scr);
        CHECK(mismatches(scr) == 0);
        dgx_screen_destroy(&scr);
    }
    CHECK(resets == resets0 && sleeps == sleeps0);
    CHECK(violations == 0);

    CHECK_DONE();
}
