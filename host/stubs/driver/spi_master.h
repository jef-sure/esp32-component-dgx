#pragma once
/*
 * The SPI master driver of IDF, as far as DGX uses it: types and flags here,
 * the functions in the tests that need them. SPI_TRANS_DMA_USE_PSRAM and
 * SPI_TRANS_DMA_TX_FAIL came with IDF 5.5; a test that stands for an older
 * IDF defines TEST_SPI_IDF54 before it includes anything, and has neither.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"

/* as esp_attr.h, which the headers of the driver bring along */
#define IRAM_ATTR

typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2, SPI_HOST_MAX } spi_host_device_t;
typedef enum { SPI_DMA_DISABLED = 0, SPI_DMA_CH1 = 1, SPI_DMA_CH2 = 2, SPI_DMA_CH_AUTO = 3 } spi_dma_chan_t;

#define SPICOMMON_BUSFLAG_MASTER (1u << 0)
#define SPICOMMON_BUSFLAG_DUAL   (1u << 6)
#define SPICOMMON_BUSFLAG_WPHD   (1u << 7)
#define SPICOMMON_BUSFLAG_QUAD   (SPICOMMON_BUSFLAG_DUAL | SPICOMMON_BUSFLAG_WPHD)

typedef struct {
    union {
        int mosi_io_num;
        int data0_io_num;
    };
    union {
        int miso_io_num;
        int data1_io_num;
    };
    int sclk_io_num;
    union {
        int quadwp_io_num;
        int data2_io_num;
    };
    union {
        int quadhd_io_num;
        int data3_io_num;
    };
    int      data4_io_num;
    int      data5_io_num;
    int      data6_io_num;
    int      data7_io_num;
    int      max_transfer_sz;
    uint32_t flags;
} spi_bus_config_t;

typedef struct spi_transaction_t spi_transaction_t;
typedef void (*transaction_cb_t)(spi_transaction_t *trans);

#define SPI_DEVICE_HALFDUPLEX (1u << 4)
#define SPI_DEVICE_NO_DUMMY   (1u << 6)

typedef struct {
    uint8_t          command_bits;
    uint8_t          address_bits;
    uint8_t          dummy_bits;
    uint8_t          mode;
    uint16_t         duty_cycle_pos;
    uint16_t         cs_ena_pretrans;
    uint8_t          cs_ena_posttrans;
    int              clock_speed_hz;
    int              input_delay_ns;
    int              spics_io_num;
    uint32_t         flags;
    int              queue_size;
    transaction_cb_t pre_cb;
    transaction_cb_t post_cb;
} spi_device_interface_config_t;

#define SPI_TRANS_MODE_QIO   (1u << 1)
#define SPI_TRANS_USE_TXDATA (1u << 3)
#ifndef TEST_SPI_IDF54
#define SPI_TRANS_DMA_USE_PSRAM (1u << 12)
#define SPI_TRANS_DMA_TX_FAIL   (1u << 31)
#endif

struct spi_transaction_t {
    uint32_t flags;
    uint16_t cmd;
    uint64_t addr;
    size_t   length;
    size_t   rxlength;
    void    *user;
    union {
        const void *tx_buffer;
        uint8_t     tx_data[4];
    };
    union {
        void   *rx_buffer;
        uint8_t rx_data[4];
    };
};

typedef struct spi_device_t *spi_device_handle_t;

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config, spi_dma_chan_t dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host_id);
esp_err_t spi_bus_add_device(spi_host_device_t host_id, const spi_device_interface_config_t *dev_config, spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t host_id, size_t *max_bytes);
esp_err_t spi_device_get_actual_freq(spi_device_handle_t handle, int *freq_khz);
esp_err_t spi_device_queue_trans(spi_device_handle_t handle, spi_transaction_t *trans_desc, TickType_t ticks_to_wait);
esp_err_t spi_device_get_trans_result(spi_device_handle_t handle, spi_transaction_t **trans_desc, TickType_t ticks_to_wait);
esp_err_t spi_device_transmit(spi_device_handle_t handle, spi_transaction_t *trans_desc);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *trans_desc);
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, TickType_t wait);
void      spi_device_release_bus(spi_device_handle_t dev);
