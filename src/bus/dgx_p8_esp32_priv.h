#pragma once

#include <stdbool.h>
#include "bus/dgx_p8_esp32.h"
#include "esp_lcd_panel_io.h"
/* from IDF 5.3 the i80 bus has a header of its own; in IDF 5.2 its handle is in esp_lcd_panel_io.h */
#if __has_include("esp_lcd_io_i80.h")
#include "esp_lcd_io_i80.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct _dgx_p8_bus_t {
    dgx_bus_protocols_t       protocols;
    esp_lcd_i80_bus_handle_t  i80_bus;
    bool                      owns_i80_bus; /* the bus was created by DGX for this display */
    esp_lcd_panel_io_handle_t io;
    SemaphoreHandle_t         transfer_done_sem;
    bool                      transfer_pending;
    /*
     * A transfer of this bus is a command and the data that belongs to it.
     * So a command is held back until its data comes, or until it is known
     * that there is none: another command, or sync_write().
     */
    uint8_t                   command;
    bool                      command_pending;
    bool                      complained; /* data with no command has been refused, and the log told of it once */
} dgx_p8_bus_t;

void dgx_p8_wait_pending(dgx_p8_bus_t *bus);
