#pragma once
/*
 * dgx_screen_functions.h
 *
 *  Created on: 16.12.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */


#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

void dgx_scr_init_slow_bus_optimized_funcs(dgx_screen_t *scr);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

