#pragma once
/*
 * Pins for the host, and what dgx_arch_esp32.h gives a driver on the chip:
 * a driver compiled here sets levels and waits for nothing.
 */
#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;

static inline esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { (void)pin, (void)level; return ESP_OK; }
static inline esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) { (void)pin, (void)mode; return ESP_OK; }
static inline esp_err_t dgx_gpio_set_level(gpio_num_t pin, uint32_t level) { (void)pin, (void)level; return ESP_OK; }
static inline esp_err_t dgx_gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) { (void)pin, (void)mode; return ESP_OK; }
static inline void dgx_delay(uint32_t ms) { (void)ms; }
static inline void dgx_delay_us(uint32_t us) { (void)us; }
