#pragma once
/*
 * dgx_spi_esp32.h
 *
 *  Created on: 28.10.2022
 *      Author: KYMJ
 */


#include "bus/dgx_bus_protocols.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef struct _dgx_i2c_bus_t
{
    dgx_bus_protocols_t     protocols;
    uint8_t                 i2c_address;
    i2c_port_t              i2c_num;
    gpio_num_t              sda;
    gpio_num_t              sclk;
    int                     clock_speed_hz;
    i2c_master_dev_handle_t dev_handle;
    i2c_master_bus_handle_t bus_handle;
    uint8_t                 cmd_single;
    uint8_t                 cmd_stream;
    uint8_t                 data_stream;
} dgx_i2c_bus_t;

dgx_bus_protocols_t* dgx_i2c_init(
        i2c_port_t i2c_num, //
        uint8_t i2c_address,//
        gpio_num_t sda,//
        gpio_num_t sclk,//
        int clock_speed_hz//
);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

