#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bus/dgx_spi_esp32.h"

enum
{
    DGX_SPI_TRN_XCMD_IDX  = 0, //
    DGX_SPI_TRN_XDATA_IDX = 1, //
    DGX_SPI_TRN_YCMD_IDX  = 2, //
    DGX_SPI_TRN_YDATA_IDX = 3, //
    DGX_SPI_TRN_DCMD_IDX  = 4  //
};

typedef struct _dgx_spi_bus_t
{
    dgx_bus_protocols_t           protocols;
    spi_host_device_t             host_id;
    spi_dma_chan_t                dma_channel;
    gpio_num_t                    mosi;
    gpio_num_t                    miso;
    gpio_num_t                    sclk;
    gpio_num_t                    csio;
    gpio_num_t                    dcio;
    int                           max_transfer_sz;
    int                           clock_speed_hz;
    int                           pending_transactions;
    bool                          is_trans_data_free;
    spi_device_handle_t           spi;
    spi_device_interface_config_t devcfg;
    union
    {
        struct
        {
            spi_transaction_t trans_area_cmd;
            uint8_t           area_data[6];
        };
        spi_transaction_t trans_area[DGX_SPI_TRN_DCMD_IDX + 1];
    };
    spi_transaction_t trans_data;
    spi_transaction_t trans_sync;
    struct
    {
        uint16_t left;
        uint16_t right;
        uint16_t top;
        uint16_t bottom;
    } area;
} dgx_spi_bus_t;

static inline void *set_dc_pin(gpio_num_t dc, uint8_t val)
{
    return (void *)(dc >= 0 ? ((val & 1u) | ((uint32_t)dc << 1)) : 0);
}

static inline gpio_num_t get_dc_pin(void *user_ptr)
{
    uint32_t dc = (uint32_t)user_ptr;
    return dc;
}

void dgx_spi_wait_pending(struct _dgx_bus_protocols_t *_bus);
