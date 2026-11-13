#pragma once
/*
 * Pins for the host, and what dgx_arch_esp32.h gives a driver on the chip:
 * a driver compiled here sets levels and waits for nothing, but in a program
 * with the window on the computer (DGX_HOST_WINDOW, host/window), where a
 * wait is a wait.
 */
#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin) >= 0)
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;

static inline esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { (void)pin, (void)level; return ESP_OK; }
static inline esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) { (void)pin, (void)mode; return ESP_OK; }
static inline esp_err_t dgx_gpio_set_level(gpio_num_t pin, uint32_t level) { (void)pin, (void)level; return ESP_OK; }
static inline esp_err_t dgx_gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) { (void)pin, (void)mode; return ESP_OK; }
static inline int gpio_get_level(gpio_num_t pin) { (void)pin; return 0; }
static inline int dgx_gpio_get_level(gpio_num_t pin) { (void)pin; return 0; }
#ifdef DGX_HOST_WINDOW
void dgx_host_delay(uint32_t ms);
static inline void dgx_delay(uint32_t ms) { dgx_host_delay(ms); }
#else
static inline void dgx_delay(uint32_t ms) { (void)ms; }
#endif
static inline void dgx_delay_us(uint32_t us) { (void)us; }
