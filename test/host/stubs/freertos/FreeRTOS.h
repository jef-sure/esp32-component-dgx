#pragma once
#include <stdint.h>

#define CONFIG_FREERTOS_HZ 1000
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
typedef uint32_t TickType_t;
