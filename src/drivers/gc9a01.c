#include "drivers/gc9a01.h"

#include <stdlib.h>

#include "dgx_arch.h"
#include "dgx_screen_functions.h"
#include "dgx_spi_area_protocol.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "esp_log.h"

#define GC9A01_SWRESET 0x01
#define GC9A01_SLPOUT  0x11
#define GC9A01_INVON   0x21
#define GC9A01_DISPOFF 0x28
#define GC9A01_DISPON  0x29
#define GC9A01_CASET   0x2A
#define GC9A01_PASET   0x2B
#define GC9A01_RAMWR   0x2C
#define GC9A01_RAMRD   0x2E
#define GC9A01_MADCTL  0x36
#define GC9A01_COLMOD  0x3A

#define GC9A01_MADCTL_MY  0x80
#define GC9A01_MADCTL_MX  0x40
#define GC9A01_MADCTL_MV  0x20
#define GC9A01_MADCTL_BGR 0x08

static const dgx_lcd_init_cmd_t gc9a01_init_cmds[] = {
    {0xEF,          0,                     0,    0,   {0}                                                                     },
    {0xEB,          0,                     1,    0,   {0x14}                                                                  },
    {0xFE,          0,                     0,    0,   {0}                                                                     },
    {0xEF,          0,                     0,    0,   {0}                                                                     },
    {0xEB,          0,                     1,    0,   {0x14}                                                                  },
    {0x84,          0,                     1,    0,   {0x40}                                                                  },
    {0x85,          0,                     1,    0,   {0xFF}                                                                  },
    {0x86,          0,                     1,    0,   {0xFF}                                                                  },
    {0x87,          0,                     1,    0,   {0xFF}                                                                  },
    {0x88,          0,                     1,    0,   {0x0A}                                                                  },
    {0x89,          0,                     1,    0,   {0x21}                                                                  },
    {0x8A,          0,                     1,    0,   {0x00}                                                                  },
    {0x8B,          0,                     1,    0,   {0x80}                                                                  },
    {0x8C,          0,                     1,    0,   {0x01}                                                                  },
    {0x8D,          0,                     1,    0,   {0x01}                                                                  },
    {0x8E,          0,                     1,    0,   {0xFF}                                                                  },
    {0x8F,          0,                     1,    0,   {0xFF}                                                                  },
    {0xB6,          0,                     2,    0,   {0x00, 0x20}                                                            },
    {GC9A01_COLMOD, dgx_panel_adj_colmod, 1,    0,   {0x05}                                                                  },
    {GC9A01_MADCTL, dgx_panel_adj_madctl, 1,    0,   {GC9A01_MADCTL_BGR}                                                     },
    {0x90,          0,                     4,    0,   {0x08, 0x08, 0x08, 0x08}                                                },
    {0xBD,          0,                     1,    0,   {0x06}                                                                  },
    {0xBC,          0,                     1,    0,   {0x00}                                                                  },
    {0xFF,          0,                     3,    0,   {0x60, 0x01, 0x04}                                                      },
    {0xC3,          0,                     1,    0,   {0x13}                                                                  },
    {0xC4,          0,                     1,    0,   {0x13}                                                                  },
    {0xC9,          0,                     1,    0,   {0x22}                                                                  },
    {0xBE,          0,                     1,    0,   {0x11}                                                                  },
    {0xE1,          0,                     2,    0,   {0x10, 0x0E}                                                            },
    {0xDF,          0,                     3,    0,   {0x21, 0x0C, 0x02}                                                      },
    {0xF0,          0,                     6,    0,   {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}                                    },
    {0xF1,          0,                     6,    0,   {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}                                    },
    {0xF2,          0,                     6,    0,   {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}                                    },
    {0xF3,          0,                     6,    0,   {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}                                    },
    {0xED,          0,                     2,    0,   {0x1B, 0x0B}                                                            },
    {0xAE,          0,                     1,    0,   {0x77}                                                                  },
    {0xCD,          0,                     1,    0,   {0x63}                                                                  },
    {0x70,          0,                     9,    0,   {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}                  },
    {0xE8,          0,                     1,    0,   {0x34}                                                                  },
    {0x62,          0,                     12,   0,   {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}},
    {0x63,          0,                     12,   0,   {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}},
    {0x64,          0,                     7,    0,   {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}                              },
    {0x66,          0,                     10,   0,   {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}            },
    {0x67,          0,                     10,   0,   {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}            },
    {0x74,          0,                     7,    0,   {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}                              },
    {0x98,          0,                     2,    0,   {0x3E, 0x07}                                                            },
    {0x35,          0,                     0,    0,   {0}                                                                     },
    {GC9A01_INVON,  0,                     0,    0,   {0}                                                                     },
    {GC9A01_SLPOUT, 0,                     0,    120, {0}                                                                     },
    {GC9A01_DISPON, 0,                     0,    20,  {0}                                                                     },
    {0,             0,                     0xFF, 0,   {0}                                                                     },
};

/*
 * Both bounds of the window go every time, also one that has not changed:
 * the GC9A01 does not work with a bound left out, as the window of the other
 * displays leaves one out. Its datasheet says nothing of it, and
 * Adafruit_GC9A01A and TFT_eSPI send both every time too.
 */
const dgx_panel_t dgx_panel_gc9a01 = {
    .name           = "GC9A01",
    .width          = 240,
    .height         = 240,
    .colmod         = {0x03, 0x05, 0x06},
    .ramrd          = GC9A01_RAMRD,
    .reset_low_ms   = 100,
    .reset_after_ms = 100,
    .flags          = DGX_PANEL_BOTH_BOUNDS,
    .init           = gc9a01_init_cmds,
};

dgx_screen_t *dgx_gc9a01_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo)
{
    return dgx_panel_init(bus, &dgx_panel_gc9a01, rst, GPIO_NUM_NC, color_bits, cbo);
}

void dgx_gc9a01_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_panel_orientation(scr, dir_x, dir_y, swap_xy);
}

void dgx_gc9a01_display_off(dgx_screen_t *_scr)
{
    dgx_screen_with_bus_t *scr = (dgx_screen_with_bus_t *)_scr;

    scr->bus->write_command(scr->bus, GC9A01_DISPOFF);
    /* a bus that sends a command together with its data holds it back until it knows there is none */
    if (scr->bus->sync_write) scr->bus->sync_write(scr->bus);
}

void dgx_gc9a01_display_on(dgx_screen_t *_scr)
{
    dgx_screen_with_bus_t *scr = (dgx_screen_with_bus_t *)_scr;

    scr->bus->write_command(scr->bus, GC9A01_DISPON);
    if (scr->bus->sync_write) scr->bus->sync_write(scr->bus);
}