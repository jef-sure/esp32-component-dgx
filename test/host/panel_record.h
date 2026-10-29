#pragma once
/*
 * Forced into every source of test_panel_streams: the levels a driver sets on
 * its pins and the waits it makes are recorded with what it sends. The stub
 * of driver/gpio.h comes first and keeps its functions under their names;
 * what the drivers call after it goes to the record.
 */
#include <stdint.h>

#include "driver/gpio.h"

esp_err_t panel_record_level(gpio_num_t pin, uint32_t level);
esp_err_t panel_record_mode(gpio_num_t pin, int mode);
void      panel_record_wait(uint32_t ms);

#define dgx_gpio_set_level(pin, level)    panel_record_level((pin), (level))
#define dgx_gpio_set_direction(pin, mode) panel_record_mode((pin), (int)(mode))
#define dgx_delay(ms)                     panel_record_wait(ms)
