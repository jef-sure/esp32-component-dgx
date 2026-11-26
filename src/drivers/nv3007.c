/*
 * nv3007.c
 *
 *  The NV3007 of the bar panels of 142x428 on SPI. The memory of the
 *  controller is 168x428 (its datasheet: 168RGB x 428); the 142 columns of
 *  the glass begin at column 12 of it, as the vendor's window has it, and so
 *  at column 14 with the columns counted the other way.
 *
 *  The commands the panel starts with are the init sequence Arduino_GFX has
 *  for the 2.79" module (Arduino_NV3007.h, nv3007_279_init_operations): the
 *  vendor's registers opened by FF A5 (the source and gate timings, the gamma
 *  of both polarities), FF 00 to close them, the pixel format, sleep out, and
 *  the display on. Arduino_GFX has another table, nv3007_init_operations, of
 *  another glass; a configuration posted on the Home Assistant forum for a
 *  142x428 module has its values and the same window from column 12. A module
 *  of that glass passes it to dgx_panel_init() in a copy of
 *  dgx_panel_nv3007_142x428. The inversion is off, as Arduino_GFX leaves it
 *  for a panel it is not told is IPS; the colors in RGB order.
 */

#include "drivers/nv3007.h"

#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "esp_log.h"

#define NV3007_SLPOUT  0x11 // Sleep out
#define NV3007_INVOFF  0x20 // Display inversion off
#define NV3007_INVON   0x21 // Display inversion on
#define NV3007_DISPON  0x29 // Display on
#define NV3007_TEON    0x35 // Tearing effect line on
#define NV3007_MADCTL  0x36 // Memory data access control
#define NV3007_COLMOD  0x3A // Interface pixel format
#define NV3007_PAGE    0xFF // A5h opens the registers of the vendor, 00h closes them

#define NV3007_MADCTL_MX 0x40
#define NV3007_MADCTL_MV 0x20

static const char TAG[] = "DGX NV3007";

static const dgx_lcd_init_cmd_t nv3007_init_cmds[] = {
    {NV3007_PAGE,   0,                    1,    0,   {0xA5}                  }, // the registers of the vendor
    {0x9A,          0,                    1,    0,   {0x08}                  }, //
    {0x9B,          0,                    1,    0,   {0x08}                  }, //
    {0x9C,          0,                    1,    0,   {0xB0}                  }, //
    {0x9D,          0,                    1,    0,   {0x16}                  }, //
    {0x9E,          0,                    1,    0,   {0xC4}                  }, //
    {0x8F,          0,                    2,    0,   {0x55, 0x04}            }, //
    {0x84,          0,                    1,    0,   {0x90}                  }, //
    {0x83,          0,                    1,    0,   {0x7B}                  }, //
    {0x85,          0,                    1,    0,   {0x33}                  }, //
    {0x60,          0,                    1,    0,   {0x00}                  }, // gamma
    {0x70,          0,                    1,    0,   {0x00}                  }, //
    {0x61,          0,                    1,    0,   {0x02}                  }, //
    {0x71,          0,                    1,    0,   {0x02}                  }, //
    {0x62,          0,                    1,    0,   {0x04}                  }, //
    {0x72,          0,                    1,    0,   {0x04}                  }, //
    {0x6C,          0,                    1,    0,   {0x29}                  }, //
    {0x7C,          0,                    1,    0,   {0x29}                  }, //
    {0x6D,          0,                    1,    0,   {0x31}                  }, //
    {0x7D,          0,                    1,    0,   {0x31}                  }, //
    {0x6E,          0,                    1,    0,   {0x0F}                  }, //
    {0x7E,          0,                    1,    0,   {0x0F}                  }, //
    {0x66,          0,                    1,    0,   {0x21}                  }, //
    {0x76,          0,                    1,    0,   {0x21}                  }, //
    {0x68,          0,                    1,    0,   {0x3A}                  }, //
    {0x78,          0,                    1,    0,   {0x3A}                  }, //
    {0x63,          0,                    1,    0,   {0x07}                  }, //
    {0x73,          0,                    1,    0,   {0x07}                  }, //
    {0x64,          0,                    1,    0,   {0x05}                  }, //
    {0x74,          0,                    1,    0,   {0x05}                  }, //
    {0x65,          0,                    1,    0,   {0x02}                  }, //
    {0x75,          0,                    1,    0,   {0x02}                  }, //
    {0x67,          0,                    1,    0,   {0x23}                  }, //
    {0x77,          0,                    1,    0,   {0x23}                  }, //
    {0x69,          0,                    1,    0,   {0x08}                  }, //
    {0x79,          0,                    1,    0,   {0x08}                  }, //
    {0x6A,          0,                    1,    0,   {0x13}                  }, //
    {0x7A,          0,                    1,    0,   {0x13}                  }, //
    {0x6B,          0,                    1,    0,   {0x13}                  }, //
    {0x7B,          0,                    1,    0,   {0x13}                  }, //
    {0x6F,          0,                    1,    0,   {0x00}                  }, //
    {0x7F,          0,                    1,    0,   {0x00}                  }, //
    {0x50,          0,                    1,    0,   {0x00}                  }, //
    {0x52,          0,                    1,    0,   {0xD6}                  }, //
    {0x53,          0,                    1,    0,   {0x08}                  }, //
    {0x54,          0,                    1,    0,   {0x08}                  }, //
    {0x55,          0,                    1,    0,   {0x1E}                  }, //
    {0x56,          0,                    1,    0,   {0x1C}                  }, //
    {0xA0,          0,                    3,    0,   {0x2B, 0x24, 0x00}      }, //
    {0xA1,          0,                    1,    0,   {0x87}                  }, //
    {0xA2,          0,                    1,    0,   {0x86}                  }, //
    {0xA5,          0,                    1,    0,   {0x00}                  }, //
    {0xA6,          0,                    1,    0,   {0x00}                  }, //
    {0xA7,          0,                    1,    0,   {0x00}                  }, //
    {0xA8,          0,                    1,    0,   {0x36}                  }, //
    {0xA9,          0,                    1,    0,   {0x7E}                  }, //
    {0xAA,          0,                    1,    0,   {0x7E}                  }, //
    {0xB9,          0,                    1,    0,   {0x85}                  }, //
    {0xBA,          0,                    1,    0,   {0x84}                  }, //
    {0xBB,          0,                    1,    0,   {0x83}                  }, //
    {0xBC,          0,                    1,    0,   {0x82}                  }, //
    {0xBD,          0,                    1,    0,   {0x81}                  }, //
    {0xBE,          0,                    1,    0,   {0x80}                  }, //
    {0xBF,          0,                    1,    0,   {0x01}                  }, //
    {0xC0,          0,                    1,    0,   {0x02}                  }, //
    {0xC1,          0,                    1,    0,   {0x00}                  }, //
    {0xC2,          0,                    1,    0,   {0x00}                  }, //
    {0xC3,          0,                    1,    0,   {0x00}                  }, //
    {0xC4,          0,                    1,    0,   {0x33}                  }, //
    {0xC5,          0,                    1,    0,   {0x7E}                  }, //
    {0xC6,          0,                    1,    0,   {0x7E}                  }, //
    {0xC8,          0,                    2,    0,   {0x33, 0x33}            }, //
    {0xC9,          0,                    1,    0,   {0x68}                  }, //
    {0xCA,          0,                    1,    0,   {0x69}                  }, //
    {0xCB,          0,                    1,    0,   {0x6A}                  }, //
    {0xCC,          0,                    1,    0,   {0x6B}                  }, //
    {0xCD,          0,                    2,    0,   {0x33, 0x33}            }, //
    {0xCE,          0,                    1,    0,   {0x6C}                  }, //
    {0xCF,          0,                    1,    0,   {0x6D}                  }, //
    {0xD0,          0,                    1,    0,   {0x6E}                  }, //
    {0xD1,          0,                    1,    0,   {0x6F}                  }, //
    {0xAB,          0,                    2,    0,   {0x03, 0x67}            }, //
    {0xAC,          0,                    2,    0,   {0x03, 0x6B}            }, //
    {0xAD,          0,                    2,    0,   {0x03, 0x68}            }, //
    {0xAE,          0,                    2,    0,   {0x03, 0x6C}            }, //
    {0xB3,          0,                    1,    0,   {0x00}                  }, //
    {0xB4,          0,                    1,    0,   {0x00}                  }, //
    {0xB5,          0,                    1,    0,   {0x00}                  }, //
    {0xB6,          0,                    1,    0,   {0x32}                  }, //
    {0xB7,          0,                    1,    0,   {0x7E}                  }, //
    {0xB8,          0,                    1,    0,   {0x7E}                  }, //
    {0xE0,          0,                    1,    0,   {0x00}                  }, //
    {0xE1,          0,                    2,    0,   {0x03, 0x0F}            }, //
    {0xE2,          0,                    1,    0,   {0x04}                  }, //
    {0xE3,          0,                    1,    0,   {0x01}                  }, //
    {0xE4,          0,                    1,    0,   {0x0E}                  }, //
    {0xE5,          0,                    1,    0,   {0x01}                  }, //
    {0xE6,          0,                    1,    0,   {0x19}                  }, //
    {0xE7,          0,                    1,    0,   {0x10}                  }, //
    {0xE8,          0,                    1,    0,   {0x10}                  }, //
    {0xEA,          0,                    1,    0,   {0x12}                  }, //
    {0xEB,          0,                    1,    0,   {0xD0}                  }, //
    {0xEC,          0,                    1,    0,   {0x04}                  }, //
    {0xED,          0,                    1,    0,   {0x07}                  }, //
    {0xEE,          0,                    1,    0,   {0x07}                  }, //
    {0xEF,          0,                    1,    0,   {0x09}                  }, //
    {0xF0,          0,                    1,    0,   {0xD0}                  }, //
    {0xF1,          0,                    1,    0,   {0x0E}                  }, //
    {0xF9,          0,                    1,    0,   {0x17}                  }, //
    {0xF2,          0,                    4,    0,   {0x2C, 0x1B, 0x0B, 0x20}}, //
    {0xE9,          0,                    1,    0,   {0x29}                  }, //
    {0xEC,          0,                    1,    0,   {0x04}                  }, //
    {NV3007_TEON,   0,                    1,    0,   {0x00}                  }, //
    {0x44,          0,                    2,    0,   {0x00, 0x10}            }, //
    {0x46,          0,                    1,    0,   {0x10}                  }, //
    {NV3007_PAGE,   0,                    1,    0,   {0x00}                  }, // the registers of the vendor closed
    {NV3007_COLMOD, dgx_panel_adj_colmod, 1,    0,   {0x05}                  }, // 05h for 16 bits, 06h for 18
    {NV3007_MADCTL, dgx_panel_adj_madctl, 1,    0,   {0}                     }, // upright, in the color order of the screen
    {NV3007_INVOFF, 0,                    0,    0,   {0}                     }, // INVON for a module whose colors come out inverted
    {NV3007_SLPOUT, 0,                    0,    120, {0}                     }, //
    {0,             0,                    0xff, 0,   {0}                     }  // Stop
};

/* the display is turned on once its memory is cleared, so whatever was left in it is never shown */
static const dgx_lcd_init_cmd_t nv3007_on_cmds[] = {
    {NV3007_DISPON, 0, 0,    150, {0}}, //
    {0,             0, 0xff, 0,   {0}}  // Stop
};

const dgx_panel_t dgx_panel_nv3007_142x428 = {
    .name           = "NV3007",
    .width          = 142,
    .height         = 428,
    .memory_width   = 168,
    .x_offset       = 12,
    .colmod         = {0, 0x05, 0x06},
    .madctl         = 0x00,
    .reset_high_ms  = 100,
    .reset_low_ms   = 120,
    .reset_after_ms = 120,
    .flags          = DGX_PANEL_CLEAR_FIRST,
    .init           = nv3007_init_cmds,
    .on             = nv3007_on_cmds,
};

dgx_screen_t *dgx_nv3007_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits, dgx_color_order_t cbo,
                              dgx_nv3007_resolution_t resolution)
{
    if (resolution != DGX_NV3007_142X428 && resolution != DGX_NV3007_428X142) {
        ESP_LOGE(TAG, "Unsupported NV3007 resolution %d", (int)resolution);
        return NULL;
    }
    dgx_screen_t *scr = dgx_panel_init(bus, &dgx_panel_nv3007_142x428, rst, backlight, color_bits, cbo);
    /* landscape is the portrait picture turned a quarter, MADCTL MX and MV as rotation 1 of Arduino_GFX */
    if (scr && resolution == DGX_NV3007_428X142) dgx_screen_orientation(scr, DgxScreenRightLeft, DgxScreenTopBottom, true);
    return scr;
}
