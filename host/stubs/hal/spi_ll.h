#pragma once
/*
 * The registers of an SPI host, as far as DGX sends by them: what a transfer
 * by the registers asks to be set, and its bytes. The setters are here; the
 * start of a transfer and whether one is under way are the model's, in the
 * tests that build the SPI bus.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "soc/soc_caps.h"

typedef struct {
    uint8_t cmd_lines;
    uint8_t addr_lines;
    uint8_t data_lines;
} spi_line_mode_t;

typedef struct spi_dev_t {
    spi_line_mode_t lines;
    struct {
        bool fwrite_quad; /* the data written on four lines: the field every chip sets for it */
    } user;
    int             command_bits, addr_bits, dummy;
    uint16_t        command; /* the opcode of a command phase, as given */
    uint64_t        address; /* the address of an address phase, as given */
    bool            miso, mosi, dma_tx;
    size_t          bitlen;
    uint32_t        data_buf[SOC_SPI_MAXIMUM_BUFFER_SIZE / 4];
    bool            applied; /* spi_ll_apply_config() since the last start */
    bool            running; /* a transfer is under way */
} spi_dev_t;

extern spi_dev_t model_spi_hw[SOC_SPI_PERIPH_NUM];
#define SPI_LL_GET_HW(ID) (&model_spi_hw[(ID)])

static inline void spi_ll_master_set_line_mode(spi_dev_t *hw, spi_line_mode_t line_mode)
{
    hw->lines            = line_mode;
    hw->user.fwrite_quad = line_mode.data_lines == 4;
}

static inline void spi_ll_set_command_bitlen(spi_dev_t *hw, int bitlen)
{
    hw->command_bits = bitlen;
}

static inline void spi_ll_set_addr_bitlen(spi_dev_t *hw, int bitlen)
{
    hw->addr_bits = bitlen;
}

static inline void spi_ll_set_command(spi_dev_t *hw, uint16_t cmd, int cmdlen, bool lsbfirst)
{
    (void)cmdlen, (void)lsbfirst;
    hw->command = cmd;
}

static inline void spi_ll_set_address(spi_dev_t *hw, uint64_t addr, int addrlen, uint32_t lsbfirst)
{
    (void)addrlen, (void)lsbfirst;
    hw->address = addr;
}

static inline void spi_ll_set_dummy(spi_dev_t *hw, int dummy_n)
{
    hw->dummy = dummy_n;
}

static inline void spi_ll_enable_miso(spi_dev_t *hw, int enable)
{
    hw->miso = enable;
}

static inline void spi_ll_enable_mosi(spi_dev_t *hw, int enable)
{
    hw->mosi = enable;
}

static inline void spi_ll_dma_tx_enable(spi_dev_t *hw, bool enable)
{
    hw->dma_tx = enable;
}

static inline void spi_ll_set_mosi_bitlen(spi_dev_t *hw, size_t bitlen)
{
    hw->bitlen = bitlen;
}

/* as the driver's: whole words, read from the bytes given */
static inline void spi_ll_write_buffer(spi_dev_t *hw, const uint8_t *buffer_to_send, size_t bitlen)
{
    for (size_t x = 0; x < bitlen; x += 32) memcpy(&hw->data_buf[x / 32], &buffer_to_send[x / 8], 4);
}

static inline void spi_ll_apply_config(spi_dev_t *hw)
{
    hw->applied = true;
}

void     spi_ll_user_start(spi_dev_t *hw);
uint32_t spi_ll_get_running_cmd(spi_dev_t *hw);
