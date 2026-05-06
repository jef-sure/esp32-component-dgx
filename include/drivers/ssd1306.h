#pragma once
#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"
#include "dgx_arch_esp32.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef enum {
    SSD1306_128X32,
    SSD1306_128X64,
    SSD1306_96X16,
    SSD1306_64X48,
    SSD1306_72X40
} ssd1306_resolution_t;

void dgx_ssd1306_contrast(dgx_screen_t *scr, uint8_t contrast);
dgx_screen_t *dgx_ssd1306_init(dgx_bus_protocols_t *bus, ssd1306_resolution_t resolution, uint8_t is_ext_vcc, gpio_num_t rst);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif

