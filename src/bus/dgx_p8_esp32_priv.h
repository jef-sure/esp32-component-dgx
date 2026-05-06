#pragma once

#include <stdbool.h>
#include "bus/dgx_p8_esp32.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct _dgx_p8_bus_t {
    dgx_bus_protocols_t       protocols;
    esp_lcd_i80_bus_handle_t  i80_bus;
    esp_lcd_panel_io_handle_t io;
    SemaphoreHandle_t         transfer_done_sem;
    bool                      transfer_pending;
} dgx_p8_bus_t;

void dgx_p8_wait_pending(dgx_p8_bus_t *bus);
