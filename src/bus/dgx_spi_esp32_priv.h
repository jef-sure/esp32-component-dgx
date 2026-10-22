#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bus/dgx_spi_esp32.h"

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
    bool                          owns_host; /* the host was initialized by DGX, shared by its displays on it */
    int                           clock_speed_hz;
    uint8_t                       data_queued; /* transfers of pixels in trans_data on their way: 0, 1, or 2 inside write_data_async() */
    uint8_t                       data_slot;   /* the one of trans_data the next transfer of pixels takes */
    spi_device_handle_t           spi;
    spi_device_interface_config_t devcfg;
    spi_transaction_t trans_data[2]; /* pixels, queued in turn: one may be under way when a call returns */
    spi_transaction_t trans_sync; /* commands and parameters, waited for */
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
