/*
 * A check of the NV3007 driver on a 142x428 SPI bar module.
 *
 * The demo runs four scenes in a loop and prints to the log what each one
 * should look like:
 *   1. colors: named color bars and color ramps;
 *   2. the eight orientations of the screen, four seconds each;
 *   3. speed: the whole screen filled 10 times in a row (the screen flashes),
 *      then the time of one fill;
 *   4. lines one pixel wide and lines of single points.
 *
 * Change the pins and the SPI clock below to match your wiring.
 */
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_spi_esp32.h"
#include "dgx_bits.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_screen.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "drivers/nv3007.h"
#include "scenes.h"

static const char *TAG = "nv3007_demo";

#define DEMO_LCD_HOST SPI2_HOST

/*
 * GPIOs of an ESP32-C3: the SPI pins of its IO MUX (6, 7, 10) and three free
 * ones, away from the flash (11-17), USB (18, 19), the console (20, 21) and
 * the strapping pins (2, 8, 9).
 */
enum {
    DEMO_PIN_MOSI      = 7,  /* SDA, SDI or DIN of the module */
    DEMO_PIN_SCLK      = 6,  /* SCL or CLK */
    DEMO_PIN_CS        = 10,
    DEMO_PIN_DC        = 3,  /* DC or RS */
    DEMO_PIN_RST       = 4,  /* RES or RST; GPIO_NUM_NC if it is tied to 3.3V or EN */
    DEMO_PIN_BACKLIGHT = 5,  /* BLK or BL; GPIO_NUM_NC if it is tied to 3.3V */
    DEMO_SPI_MHZ       = 20, /* 40 may work as well: try it once the pictures are right */
};

/* the color order the driver is told; DgxScreenBGR if red and blue come out swapped */
#define DEMO_COLOR_ORDER DgxScreenRGB

/* 1 if the colors come out inverted, like a photo negative: the panel is then told INVON */
#define DEMO_INVERT 0

/* 1 to start the panel with the other table of Arduino_GFX, nv3007_init_operations, instead of the driver's one for a 2.79" module */
#define DEMO_TABLE_MAIN 0

#if DEMO_TABLE_MAIN
/* nv3007_init_operations of Arduino_GFX, with the pixel format and MADCTL of the screen */
static const dgx_lcd_init_cmd_t table_main[] = {
    {0xFF,   0,                    1,    0,   {0xA5}                  }, //
    {0x11, 0,                    0,    120, {0}                     }, //
    {0xFF,   0,                    1,    0,   {0xA5}                  }, // the registers of the vendor
    {0x9A,          0,                    1,    0,   {0x08}                  }, //
    {0x9B,          0,                    1,    0,   {0x08}                  }, //
    {0x9C,          0,                    1,    0,   {0xB0}                  }, //
    {0x9D,          0,                    1,    0,   {0x17}                  }, //
    {0x9E,          0,                    1,    0,   {0xC2}                  }, //
    {0x8F,          0,                    2,    0,   {0x22, 0x04}            }, //
    {0x84,          0,                    1,    0,   {0x90}                  }, //
    {0x83,          0,                    1,    0,   {0x7B}                  }, //
    {0x85,          0,                    1,    0,   {0x4F}                  }, //
    {0x6E,          0,                    1,    0,   {0x0F}                  }, // gamma: V0
    {0x7E,          0,                    1,    0,   {0x0F}                  }, //
    {0x60,          0,                    1,    0,   {0x00}                  }, // V63
    {0x70,          0,                    1,    0,   {0x00}                  }, //
    {0x6D,          0,                    1,    0,   {0x39}                  }, // V1
    {0x7D,          0,                    1,    0,   {0x31}                  }, //
    {0x61,          0,                    1,    0,   {0x0A}                  }, // V62
    {0x71,          0,                    1,    0,   {0x0A}                  }, //
    {0x6C,          0,                    1,    0,   {0x35}                  }, // V2
    {0x7C,          0,                    1,    0,   {0x29}                  }, //
    {0x62,          0,                    1,    0,   {0x0F}                  }, // V61
    {0x72,          0,                    1,    0,   {0x0F}                  }, //
    {0x68,          0,                    1,    0,   {0x4F}                  }, // V20
    {0x78,          0,                    1,    0,   {0x45}                  }, //
    {0x66,          0,                    1,    0,   {0x33}                  }, // V43
    {0x76,          0,                    1,    0,   {0x33}                  }, //
    {0x6B,          0,                    1,    0,   {0x14}                  }, // V4
    {0x7B,          0,                    1,    0,   {0x14}                  }, //
    {0x63,          0,                    1,    0,   {0x09}                  }, // V59
    {0x73,          0,                    1,    0,   {0x09}                  }, //
    {0x6A,          0,                    1,    0,   {0x13}                  }, // V6
    {0x7A,          0,                    1,    0,   {0x16}                  }, //
    {0x64,          0,                    1,    0,   {0x08}                  }, // V57
    {0x74,          0,                    1,    0,   {0x08}                  }, //
    {0x69,          0,                    1,    0,   {0x07}                  }, //
    {0x79,          0,                    1,    0,   {0x0D}                  }, //
    {0x65,          0,                    1,    0,   {0x05}                  }, //
    {0x75,          0,                    1,    0,   {0x05}                  }, //
    {0x67,          0,                    1,    0,   {0x33}                  }, //
    {0x77,          0,                    1,    0,   {0x33}                  }, //
    {0x6F,          0,                    1,    0,   {0x00}                  }, //
    {0x7F,          0,                    1,    0,   {0x00}                  }, //
    {0x50,          0,                    1,    0,   {0x00}                  }, //
    {0x52,          0,                    1,    0,   {0xD6}                  }, //
    {0x53,          0,                    1,    0,   {0x04}                  }, //
    {0x54,          0,                    1,    0,   {0x04}                  }, //
    {0x55,          0,                    1,    0,   {0x1B}                  }, //
    {0x56,          0,                    1,    0,   {0x1B}                  }, //
    {0xA0,          0,                    3,    0,   {0x2A, 0x24, 0x00}      }, //
    {0xA1,          0,                    1,    0,   {0x84}                  }, //
    {0xA2,          0,                    1,    0,   {0x85}                  }, //
    {0xA8,          0,                    1,    0,   {0x34}                  }, //
    {0xA9,          0,                    1,    0,   {0x80}                  }, //
    {0xAA,          0,                    1,    0,   {0x73}                  }, //
    {0xAB,          0,                    2,    0,   {0x03, 0x61}            }, //
    {0xAC,          0,                    2,    0,   {0x03, 0x65}            }, //
    {0xAD,          0,                    2,    0,   {0x03, 0x60}            }, //
    {0xAE,          0,                    2,    0,   {0x03, 0x64}            }, //
    {0xB9,          0,                    1,    0,   {0x82}                  }, //
    {0xBA,          0,                    1,    0,   {0x83}                  }, //
    {0xBB,          0,                    1,    0,   {0x80}                  }, //
    {0xBC,          0,                    1,    0,   {0x81}                  }, //
    {0xBD,          0,                    1,    0,   {0x02}                  }, //
    {0xBE,          0,                    1,    0,   {0x01}                  }, //
    {0xBF,          0,                    1,    0,   {0x04}                  }, //
    {0xC0,          0,                    1,    0,   {0x03}                  }, //
    {0xC4,          0,                    1,    0,   {0x33}                  }, //
    {0xC5,          0,                    1,    0,   {0x80}                  }, //
    {0xC6,          0,                    1,    0,   {0x73}                  }, //
    {0xC7,          0,                    1,    0,   {0x00}                  }, //
    {0xC8,          0,                    2,    0,   {0x33, 0x33}            }, //
    {0xC9,          0,                    1,    0,   {0x5B}                  }, //
    {0xCA,          0,                    1,    0,   {0x5A}                  }, //
    {0xCB,          0,                    1,    0,   {0x5D}                  }, //
    {0xCC,          0,                    1,    0,   {0x5C}                  }, //
    {0xCD,          0,                    2,    0,   {0x33, 0x33}            }, //
    {0xCE,          0,                    1,    0,   {0x5F}                  }, //
    {0xCF,          0,                    1,    0,   {0x5E}                  }, //
    {0xD0,          0,                    1,    0,   {0x61}                  }, //
    {0xD1,          0,                    1,    0,   {0x60}                  }, //
    {0xB0,          0,                    4,    0,   {0x3A, 0x3A, 0x00, 0x00}}, //
    {0xB6,          0,                    1,    0,   {0x32}                  }, //
    {0xB7,          0,                    1,    0,   {0x80}                  }, //
    {0xB8,          0,                    1,    0,   {0x73}                  }, //
    {0xE0,          0,                    1,    0,   {0x00}                  }, //
    {0xE1,          0,                    2,    0,   {0x03, 0x0F}            }, //
    {0xE2,          0,                    1,    0,   {0x04}                  }, //
    {0xE3,          0,                    1,    0,   {0x01}                  }, //
    {0xE4,          0,                    1,    0,   {0x0E}                  }, //
    {0xE5,          0,                    1,    0,   {0x01}                  }, //
    {0xE6,          0,                    1,    0,   {0x19}                  }, //
    {0xE7,          0,                    1,    0,   {0x10}                  }, //
    {0xE8,          0,                    1,    0,   {0x10}                  }, //
    {0xE9,          0,                    1,    0,   {0x21}                  }, //
    {0xEA,          0,                    1,    0,   {0x12}                  }, //
    {0xEB,          0,                    1,    0,   {0xD0}                  }, //
    {0xEC,          0,                    1,    0,   {0x04}                  }, //
    {0xED,          0,                    1,    0,   {0x07}                  }, //
    {0xEE,          0,                    1,    0,   {0x07}                  }, //
    {0xEF,          0,                    1,    0,   {0x09}                  }, //
    {0xF0,          0,                    1,    0,   {0xD0}                  }, //
    {0xF1,          0,                    1,    0,   {0x0E}                  }, //
    {0xF9,          0,                    1,    0,   {0x56}                  }, //
    {0xF2,          0,                    4,    0,   {0x26, 0x1B, 0x0B, 0x20}}, //
    {0xEC,          0,                    1,    0,   {0x04}                  }, //
    {0x35,   0,                    1,    0,   {0x00}                  }, //
    {0x44,          0,                    2,    0,   {0x00, 0x10}            }, //
    {0x46,          0,                    1,    0,   {0x10}                  }, //
    {0xFF,   0,                    1,    0,   {0x00}                  }, // the registers of the vendor closed
    {0x3A, dgx_panel_adj_colmod, 1,    0,   {0x05}                  }, // 05h for 16 bits, 06h for 18
    {0x36, dgx_panel_adj_madctl, 1,    0,   {0}                     }, // upright, in the color order of the screen
    {0x20, 0,                    0,    0,   {0}                     }, // INVON for a module whose colors come out inverted
    {0x11, 0,                    0,    200, {0}                     }, //
    {0,             0,                    0xff, 0,   {0}                     }  // Stop
};
#endif

static dgx_screen_t *make_screen(dgx_bus_protocols_t *bus)
{
#if DEMO_TABLE_MAIN
    static dgx_panel_t panel;
    panel      = dgx_panel_nv3007_142x428;
    panel.init = table_main;
    dgx_screen_t *scr = dgx_panel_init(bus, &panel, (gpio_num_t)DEMO_PIN_RST, (gpio_num_t)DEMO_PIN_BACKLIGHT, 16, DEMO_COLOR_ORDER);
#else
    dgx_screen_t *scr =
        dgx_nv3007_init(bus, (gpio_num_t)DEMO_PIN_RST, (gpio_num_t)DEMO_PIN_BACKLIGHT, 16, DEMO_COLOR_ORDER, DGX_NV3007_142X428);
#endif
    if (scr && DEMO_INVERT) {
        bus->write_command(bus, 0x21); /* INVON */
        bus->sync_write(bus);
    }
    return scr;
}

void app_main(void)
{
    ESP_LOGI(TAG, "NV3007 demo, test %d: MOSI %d, SCLK %d, CS %d, DC %d, RST %d, backlight %d, SPI %d MHz, invert %d, table %s", DEMO_TEST,
             DEMO_PIN_MOSI, DEMO_PIN_SCLK, DEMO_PIN_CS, DEMO_PIN_DC, DEMO_PIN_RST, DEMO_PIN_BACKLIGHT, DEMO_SPI_MHZ, DEMO_INVERT,
             DEMO_TABLE_MAIN ? "main" : "2.79");
    dgx_bus_protocols_t *bus   = dgx_spi_init(DEMO_LCD_HOST, SPI_DMA_CH_AUTO, (gpio_num_t)DEMO_PIN_MOSI, GPIO_NUM_NC, (gpio_num_t)DEMO_PIN_SCLK,
                                              (gpio_num_t)DEMO_PIN_CS, (gpio_num_t)DEMO_PIN_DC, DEMO_SPI_MHZ * 1000000, 0);
    dgx_screen_t        *aside = dgx_bw_init(96, 10);
    dgx_screen_t        *scr   = bus ? make_screen(bus) : NULL;
    if (!bus || !aside || !scr) {
        ESP_LOGE(TAG, "the bus, the screen or the memory for the labels failed");
        return;
    }
    for (;;) {
        ESP_LOGI(TAG, "test %d, colors: bars RED, GREEN, BLUE, WHITE, BLACK in these colors; below them ramps from black to red, green, "
                      "blue and white",
                 DEMO_TEST);
        demo_colors(scr, dgx_rgb_to_16);
        vTaskDelay(pdMS_TO_TICKS(8000));
        for (int way = 0; way < DEMO_WAYS; ++way) {
            const demo_way_t *d = &demo_ways[way];
            dgx_screen_orientation(scr, d->dir_x, d->dir_y, d->swap_xy);
            ESP_LOGI(TAG, "test %d, orientation %d of 8, %s, %dx%d: all text reads normally, the large F looks like the small one", DEMO_TEST,
                     way + 1, d->name, scr->width, scr->height);
            demo_orientation(scr, aside, dgx_rgb_to_16, way);
            vTaskDelay(pdMS_TO_TICKS(4000));
        }
        dgx_screen_orientation(scr, DgxScreenLeftRight, DgxScreenTopBottom, false);
        ESP_LOGI(TAG, "test %d, speed: the screen flashes 10 times", DEMO_TEST);
        static const uint8_t fills[5][3] = {
            {255, 0,   0  },
            {0,   255, 0  },
            {0,   0,   255},
            {255, 255, 255},
            {0,   0,   0  },
        };
        const int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < 10; ++i) dgx_fill_rectangle(scr, 0, 0, scr->width, scr->height, dgx_rgb_to_16(fills[i % 5][0], fills[i % 5][1], fills[i % 5][2]));
        dgx_screen_wait(scr);
        const int us = (int)((esp_timer_get_time() - t0) / 10);
        ESP_LOGI(TAG, "test %d, speed: one fill of %dx%d takes %d us at SPI %d MHz", DEMO_TEST, scr->width, scr->height, us, DEMO_SPI_MHZ);
        demo_speed(scr, dgx_rgb_to_16, us, DEMO_SPI_MHZ);
        vTaskDelay(pdMS_TO_TICKS(6000));
        ESP_LOGI(TAG, "test %d, lines: row 1 lines 1 px wide, every other at an odd x; rows 2 and 3 the same 2 px line; rows 4 and 5 "
                      "dotted lines",
                 DEMO_TEST);
        demo_pixels(scr, dgx_rgb_to_16);
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}
