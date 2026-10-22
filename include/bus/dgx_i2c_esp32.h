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
    bool                    owns_master_bus; /* the master bus is DGX's, shared by its displays on the port */
} dgx_i2c_bus_t;

/**
 * @brief Initialize an I2C bus backend for a display panel, on a master bus DGX owns.
 *
 * DGX creates the master bus of the port with the first display on it; further
 * displays on the same port share it and must name the same SDA and SCL. The
 * master bus is deleted when the last of them is disposed, unless the program
 * has added devices of its own to it. To put a display on a bus the program
 * already uses for other devices, create the master bus in the program and
 * call dgx_i2c_init_on_bus().
 *
 * @param i2c_num        I2C port number.
 * @param i2c_address    7-bit slave address of the panel; it must answer a probe.
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

/**
 * @brief Initialize an I2C bus backend for a display panel, on a master bus the program owns.
 *
 * For a display that shares the bus with other devices of the program: a touch
 * controller, a clock chip, sensors. DGX adds its device to the bus and removes
 * it when the backend is disposed; the master bus stays the program's to delete,
 * after the backend. Transfers of the display and of the other devices are
 * serialized by the ESP-IDF driver.
 *
 * @param i2c_bus        Master bus made by i2c_new_master_bus().
 * @param i2c_address    7-bit slave address of the panel; it must answer a probe.
 * @param clock_speed_hz I2C clock of this device in Hz (e.g. 400000).
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_i2c_init_on_bus(i2c_master_bus_handle_t i2c_bus, uint8_t i2c_address, int clock_speed_hz);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

