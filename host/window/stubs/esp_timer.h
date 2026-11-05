#pragma once
/* the timer of the board on the computer, host/window/host_screen.c: microseconds since the program began */
#include <stdint.h>

int64_t esp_timer_get_time(void);
