#pragma once

#include <stdint.h>

#include "dgx_screen_with_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

void dgx_screen_with_bus_set_area_window(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom);
void dgx_screen_with_bus_set_area_window_full(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top,
											  uint16_t bottom);

#ifdef __cplusplus
}
#endif