#pragma once
#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"
#include "driver/gpio.h"
#include "dgx_arch.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

dgx_screen_t *dgx_st7565r_init(dgx_bus_protocols_t *bus, gpio_num_t rst);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif

