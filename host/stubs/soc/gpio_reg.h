#pragma once
/*
 * The registers that set and clear GPIO outputs, those of an ESP32-S3. A
 * write goes to the test that includes a bus which writes them: it keeps the
 * levels of its pins.
 */
#include <stdint.h>

#define GPIO_OUT_W1TS_REG  0x08u
#define GPIO_OUT_W1TC_REG  0x0cu
#define GPIO_OUT1_W1TS_REG 0x14u
#define GPIO_OUT1_W1TC_REG 0x18u

void dgx_host_reg_write(uint32_t reg, uint32_t value);
#define REG_WRITE(reg, value) dgx_host_reg_write((reg), (value))
