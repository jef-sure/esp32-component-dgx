#pragma once
/*
 * driver/gpio.h of ESP-IDF for its linux target, which has no GPIO driver:
 * as far as DGX uses it, pins that go nowhere. The pad of a pin is chosen by
 * esp_rom_gpio_pad_select_gpio(), which the linux target declares and does
 * not have: host/window/host_screen.c has it.
 */
#include <stdint.h>

#include "esp_err.h"
#include "esp_rom_gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
#define GPIO_IS_VALID_GPIO(pin)        ((pin) >= 0)
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin) >= 0)

typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;

static inline esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    (void)pin, (void)level;
    return ESP_OK;
}

static inline esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode)
{
    (void)pin, (void)mode;
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif
