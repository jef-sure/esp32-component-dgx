#pragma once
/*
 * dgx_i2c_esp32.h
 *
 *  Created on: 28.10.2022
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
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

/**
 * @brief Initialize an I2C master bus backend for a display panel.
 *
 * @param i2c_num        I2C port number.
 * @param i2c_address    7-bit slave address of the panel.
 * @param sda            SDA data GPIO.
 * @param sclk           SCL clock GPIO.
 * @param clock_speed_hz I2C clock in Hz (e.g. 400000).
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
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

