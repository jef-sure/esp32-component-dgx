/*
 * dgx_lcd_init.c
 *
 *  Created on: 16.12.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <string.h>

#include "drivers/dgx_lcd_init.h"
#include "dgx_arch.h"
//#include "esp_log.h"
//
//static const char TAG[] = "DGX LCD INIT";

void dgx_lcd_init(dgx_screen_with_bus_t *scr, const dgx_lcd_init_cmd_t *lcd_init_cmds) {
    dgx_bus_protocols_t *bus = scr->bus;
    for (size_t cidx = 0; lcd_init_cmds[cidx].databytes != 0xff; ++cidx) {
        uint8_t cmd = lcd_init_cmds[cidx].cmd;
        bus->write_command(bus, cmd);
        uint8_t const *data = lcd_init_cmds[cidx].data;
        if (lcd_init_cmds[cidx].adjust_func) {
            data = lcd_init_cmds[cidx].adjust_func(scr, &lcd_init_cmds[cidx]);
        }
        if (lcd_init_cmds[cidx].databytes) {
            // The table of commands stays in flash, and a bus may want what it sends in RAM it can
            // transfer from: the data goes through the buffer of the bus, which is such memory.
            if (bus->buffer && bus->buffer_len >= lcd_init_cmds[cidx].databytes) {
                memcpy(bus->buffer, data, lcd_init_cmds[cidx].databytes);
                data = bus->buffer;
            }
            bus->write_data(bus, data, lcd_init_cmds[cidx].databytes * 8);
        } else if (bus->sync_write) {
            // a bus that sends a command together with its data holds it back until it knows there is none
            bus->sync_write(bus);
        }
        if (lcd_init_cmds[cidx].delay) {
            int ms = lcd_init_cmds[cidx].delay == 255 ? 500 : lcd_init_cmds[cidx].delay;
            dgx_delay(ms);
        }
    }
}

