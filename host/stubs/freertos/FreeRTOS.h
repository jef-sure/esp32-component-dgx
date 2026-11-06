#pragma once
#include <stdint.h>

/* the ticks of the board: -DCONFIG_FREERTOS_HZ=100 for a program whose board has the 100 of ESP-IDF */
#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 1000
#endif
#define pdMS_TO_TICKS(ms) ((uint32_t)((uint64_t)(ms) * CONFIG_FREERTOS_HZ / 1000u))
#define portTICK_PERIOD_MS ((uint32_t)(1000u / CONFIG_FREERTOS_HZ))
#define portMAX_DELAY 0xffffffffu
#define pdFALSE 0
#define pdTRUE 1
typedef uint32_t TickType_t;
typedef int BaseType_t;
