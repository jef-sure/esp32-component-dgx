#pragma once
#include "freertos/FreeRTOS.h"

#ifdef DGX_HOST_WINDOW
/* a wait is a wait in a program with the window on the computer, and the window is shown in it: host/window */
void dgx_host_delay(uint32_t ms);
#define vTaskDelay(ticks) dgx_host_delay((uint32_t)(ticks) * portTICK_PERIOD_MS)
#else
#define vTaskDelay(ticks) ((void)(ticks))
#endif
