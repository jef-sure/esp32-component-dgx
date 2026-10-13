#pragma once
#include <stdint.h>

#define CONFIG_FREERTOS_HZ 1000
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
#define portTICK_PERIOD_MS 1u
#define portMAX_DELAY 0xffffffffu
#define pdFALSE 0
#define pdTRUE 1
typedef uint32_t TickType_t;
typedef int BaseType_t;
