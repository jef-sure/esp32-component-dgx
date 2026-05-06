#pragma once
#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"
#include "driver/gpio.h"
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

dgx_screen_t* dgx_st7920_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t cs);
void dgx_st7920_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

