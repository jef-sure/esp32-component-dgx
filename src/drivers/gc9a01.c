#include "drivers/gc9a01.h"

#include <stdlib.h>

#include "dgx_arch.h"
#include "dgx_screen_functions.h"
#include "dgx_spi_area_protocol.h"
#include "drivers/dgx_lcd_init.h"
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

typedef struct _dgx_gc9a01_t
{
    dgx_screen_with_bus_t base;
    gpio_num_t            rst;
} dgx_gc9a01_t;

static const char TAG[] = "DGX GC9A01";

static uint8_t *dgx_gc9a01_adj_madctl(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];

    adj_data[0] = init_cmd->data[0];
    if (scr->scr.rgb_order == DgxScreenRGB) {
        adj_data[0] &= (uint8_t)~GC9A01_MADCTL_BGR;
    } else {
        adj_data[0] |= GC9A01_MADCTL_BGR;
    }
    return adj_data;
}

static uint8_t *dgx_gc9a01_adj_colmod(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd)
{
    static uint8_t adj_data[1];

    (void)init_cmd;
    if (scr->scr.color_bits == 12) {
        adj_data[0] = 0x03;
    } else if (scr->scr.color_bits == 18) {
        adj_data[0] = 0x06;
    } else {
        adj_data[0] = 0x05;
    }
    return adj_data;
}

static void dgx_gc9a01_set_area(dgx_screen_t *_scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    dgx_screen_with_bus_t *scr = (dgx_screen_with_bus_t *)_scr;

    if (left > right) {
        uint16_t tmp = left;
        left         = right;
        right        = tmp;
    }
    if (top > bottom) {
        uint16_t tmp = top;
        top          = bottom;
        bottom       = tmp;
    }
    if (left >= _scr->width || top >= _scr->height) {
        return;
    }
    if (right > _scr->width - 1) {
        right = _scr->width - 1;
    }
    if (bottom > _scr->height - 1) {
        bottom = _scr->height - 1;
    }

    left += _scr->cg_col_shift;
    right += _scr->cg_col_shift;
    top += _scr->cg_row_shift;
    bottom += _scr->cg_row_shift;

    dgx_screen_with_bus_set_area_window_full(scr, left, right, top, bottom);
}

DRAM_ATTR static const dgx_lcd_init_cmd_t gc9a01_init_cmds[] = {
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
    {GC9A01_COLMOD, dgx_gc9a01_adj_colmod, 1,    0,   {0x05}                                                                  },
    {GC9A01_MADCTL, dgx_gc9a01_adj_madctl, 1,    0,   {GC9A01_MADCTL_BGR}                                                     },
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

dgx_screen_t *dgx_gc9a01_init(dgx_bus_protocols_t *bus, gpio_num_t rst, uint8_t color_bits, dgx_color_order_t cbo)
{

    if (bus == NULL) {
        ESP_LOGE(TAG, "Display bus is not initialized");
        return NULL;
    }

    dgx_gc9a01_t *scr = (dgx_gc9a01_t *)calloc(1, sizeof(dgx_gc9a01_t));

    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }

    scr->rst                      = rst;
    scr->base.bus                 = bus;
    scr->base.scr.cg_row_shift    = 0;
    scr->base.scr.cg_col_shift    = 0;
    scr->base.scr.width           = 240;
    scr->base.scr.height          = 240;
    scr->base.scr.color_bits      = color_bits;
    scr->base.scr.rgb_order       = cbo;
    scr->base.scr.screen_name     = "GC9A01";
    scr->base.scr.screen_submodel = 0;
    scr->base.scr.screen_subtype  = DgxPhysicalScreenWithBus;

    dgx_screen_with_bus_init_area(&scr->base, GC9A01_CASET, GC9A01_PASET, GC9A01_RAMWR, GC9A01_RAMRD, DGX_SCREEN_AREA_PROTO_STD16);

    if (rst >= 0) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(100);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(100);
    }

    dgx_lcd_init(&scr->base, gc9a01_init_cmds);
    dgx_scr_init_slow_bus_optimized_funcs((dgx_screen_t *)scr);
    scr->base.scr.set_area = dgx_gc9a01_set_area;
    return (dgx_screen_t *)scr;
}

void dgx_gc9a01_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_gc9a01_t *scr     = (dgx_gc9a01_t *)_scr;
    uint8_t       cmd     = GC9A01_MADCTL;
    uint8_t       data[1] = {0};

    _scr->dir_x        = dir_x;
    _scr->dir_y        = dir_y;
    _scr->swap_xy      = swap_xy;
    _scr->cg_row_shift = 0;
    _scr->cg_col_shift = 0;
    _scr->width        = 240;
    _scr->height       = 240;

    if (dir_x == DgxScreenRightLeft) {
        data[0] |= GC9A01_MADCTL_MX;
    }
    if (dir_y == DgxScreenBottomTop) {
        data[0] |= GC9A01_MADCTL_MY;
    }
    if (swap_xy) {
        data[0] |= GC9A01_MADCTL_MV;
    }
    if (_scr->rgb_order == DgxScreenBGR) {
        data[0] |= GC9A01_MADCTL_BGR;
    }

    scr->base.bus->write_command(scr->base.bus, cmd);
    scr->base.bus->write_data(scr->base.bus, data, 8);
}

void dgx_gc9a01_display_off(dgx_screen_t *_scr)
{
    dgx_gc9a01_t *scr = (dgx_gc9a01_t *)_scr;

    scr->base.bus->write_command(scr->base.bus, GC9A01_DISPOFF);
}

void dgx_gc9a01_display_on(dgx_screen_t *_scr)
{
    dgx_gc9a01_t *scr = (dgx_gc9a01_t *)_scr;

    scr->base.bus->write_command(scr->base.bus, GC9A01_DISPON);
}