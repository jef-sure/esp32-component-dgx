#include "dgx_spi_area_protocol.h"

#include <string.h>

#include "bus/dgx_bus_protocols.h"
#include "bus/dgx_spi_esp32.h"
#include "bus/dgx_spi_esp32_priv.h"

static void dgx_screen_with_bus_set_area_window_spi_async(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)sbus->bus;
    int queued = 0;
    bool update_x = sbus->cached_area.left != left || sbus->cached_area.right != right;
    bool update_y = sbus->cached_area.top != top || sbus->cached_area.bottom != bottom;

    dgx_spi_wait_pending((dgx_bus_protocols_t *)bus);

    if (update_x) {
        memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
        bus->trans_area[queued].length = 8;
        bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
        bus->trans_area[queued].tx_data[0] = sbus->xcmd_set;
        bus->trans_area[queued].user = set_dc_pin(bus->dcio, 0);
        spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
        queued++;

        memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
        bus->trans_area[queued].length = sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8 ? 16 : 32;
        bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
        if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
            bus->trans_area[queued].tx_data[0] = (uint8_t)left;
            bus->trans_area[queued].tx_data[1] = (uint8_t)right;
        } else {
            bus->trans_area[queued].tx_data[0] = left >> 8;
            bus->trans_area[queued].tx_data[1] = left & 0xff;
            bus->trans_area[queued].tx_data[2] = right >> 8;
            bus->trans_area[queued].tx_data[3] = right & 0xff;
        }
        bus->trans_area[queued].user = set_dc_pin(bus->dcio, 1);
        spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
        queued++;

        sbus->cached_area.left = left;
        sbus->cached_area.right = right;
    }

    if (update_y) {
        memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
        bus->trans_area[queued].length = 8;
        bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
        bus->trans_area[queued].tx_data[0] = sbus->ycmd_set;
        bus->trans_area[queued].user = set_dc_pin(bus->dcio, 0);
        spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
        queued++;

        memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
        bus->trans_area[queued].length = sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8 ? 16 : 32;
        bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
        if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
            bus->trans_area[queued].tx_data[0] = (uint8_t)top;
            bus->trans_area[queued].tx_data[1] = (uint8_t)bottom;
        } else {
            bus->trans_area[queued].tx_data[0] = top >> 8;
            bus->trans_area[queued].tx_data[1] = top & 0xff;
            bus->trans_area[queued].tx_data[2] = bottom >> 8;
            bus->trans_area[queued].tx_data[3] = bottom & 0xff;
        }
        bus->trans_area[queued].user = set_dc_pin(bus->dcio, 1);
        spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
        queued++;

        sbus->cached_area.top = top;
        sbus->cached_area.bottom = bottom;
    }

    memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
    bus->trans_area[queued].length = 8;
    bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
    bus->trans_area[queued].tx_data[0] = sbus->wcmd_send;
    bus->trans_area[queued].user = set_dc_pin(bus->dcio, 0);
    spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
    queued++;

    bus->pending_transactions += queued;
}

static void dgx_screen_with_bus_set_area_window_spi_full_async(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right,
                                                               uint16_t top, uint16_t bottom)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)sbus->bus;
    int            queued = 0;

    dgx_spi_wait_pending((dgx_bus_protocols_t *)bus);

    memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
    bus->trans_area[queued].length = 8;
    bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
    bus->trans_area[queued].tx_data[0] = sbus->xcmd_set;
    bus->trans_area[queued].user = set_dc_pin(bus->dcio, 0);
    spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
    queued++;

    memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
    bus->trans_area[queued].length = sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8 ? 16 : 32;
    bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
        bus->trans_area[queued].tx_data[0] = (uint8_t)left;
        bus->trans_area[queued].tx_data[1] = (uint8_t)right;
    } else {
        bus->trans_area[queued].tx_data[0] = left >> 8;
        bus->trans_area[queued].tx_data[1] = left & 0xff;
        bus->trans_area[queued].tx_data[2] = right >> 8;
        bus->trans_area[queued].tx_data[3] = right & 0xff;
    }
    bus->trans_area[queued].user = set_dc_pin(bus->dcio, 1);
    spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
    queued++;

    memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
    bus->trans_area[queued].length = 8;
    bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
    bus->trans_area[queued].tx_data[0] = sbus->ycmd_set;
    bus->trans_area[queued].user = set_dc_pin(bus->dcio, 0);
    spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
    queued++;

    memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
    bus->trans_area[queued].length = sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8 ? 16 : 32;
    bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
        bus->trans_area[queued].tx_data[0] = (uint8_t)top;
        bus->trans_area[queued].tx_data[1] = (uint8_t)bottom;
    } else {
        bus->trans_area[queued].tx_data[0] = top >> 8;
        bus->trans_area[queued].tx_data[1] = top & 0xff;
        bus->trans_area[queued].tx_data[2] = bottom >> 8;
        bus->trans_area[queued].tx_data[3] = bottom & 0xff;
    }
    bus->trans_area[queued].user = set_dc_pin(bus->dcio, 1);
    spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
    queued++;

    memset(&bus->trans_area[queued], 0, sizeof(bus->trans_area[queued]));
    bus->trans_area[queued].length = 8;
    bus->trans_area[queued].flags = SPI_TRANS_USE_TXDATA;
    bus->trans_area[queued].tx_data[0] = sbus->wcmd_send;
    bus->trans_area[queued].user = set_dc_pin(bus->dcio, 0);
    spi_device_queue_trans(bus->spi, &bus->trans_area[queued], portMAX_DELAY);
    queued++;

    sbus->cached_area.left = left;
    sbus->cached_area.right = right;
    sbus->cached_area.top = top;
    sbus->cached_area.bottom = bottom;
    bus->pending_transactions += queued;
}

void dgx_screen_with_bus_set_area_window(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    dgx_bus_protocols_t *bus = sbus->bus;
    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_CMD8) {
        uint8_t crset[6] = {
            sbus->xcmd_set, (uint8_t)left, (uint8_t)right,
            sbus->ycmd_set, (uint8_t)top,  (uint8_t)bottom,
        };
        bus->write_commands(bus, crset, sizeof(crset) * 8u);
        return;
    }

    if (bus->bus_type == DGX_BUS_SPI) {
        dgx_screen_with_bus_set_area_window_spi_async(sbus, left, right, top, bottom);
        return;
    }

    if (sbus->cached_area.left != left || sbus->cached_area.right != right) {
        if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
            uint8_t xdata[2] = {(uint8_t)left, (uint8_t)right};
            bus->write_command(bus, sbus->xcmd_set);
            bus->write_data(bus, xdata, sizeof(xdata) * 8u);
        } else {
            uint8_t xdata[4] = {left >> 8, left & 0xff, right >> 8, right & 0xff};
            bus->write_command(bus, sbus->xcmd_set);
            bus->write_data(bus, xdata, sizeof(xdata) * 8u);
        }
        sbus->cached_area.left  = left;
        sbus->cached_area.right = right;
    }

    if (sbus->cached_area.top != top || sbus->cached_area.bottom != bottom) {
        if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
            uint8_t ydata[2] = {(uint8_t)top, (uint8_t)bottom};
            bus->write_command(bus, sbus->ycmd_set);
            bus->write_data(bus, ydata, sizeof(ydata) * 8u);
        } else {
            uint8_t ydata[4] = {top >> 8, top & 0xff, bottom >> 8, bottom & 0xff};
            bus->write_command(bus, sbus->ycmd_set);
            bus->write_data(bus, ydata, sizeof(ydata) * 8u);
        }
        sbus->cached_area.top    = top;
        sbus->cached_area.bottom = bottom;
    }
    bus->write_command(bus, sbus->wcmd_send);
}

void dgx_screen_with_bus_set_area_window_full(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top,
                                              uint16_t bottom)
{
    dgx_bus_protocols_t *bus = sbus->bus;

    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_CMD8) {
        uint8_t crset[6] = {
            sbus->xcmd_set, (uint8_t)left, (uint8_t)right,
            sbus->ycmd_set, (uint8_t)top,  (uint8_t)bottom,
        };
        bus->write_commands(bus, crset, sizeof(crset) * 8u);
        sbus->cached_area.left = left;
        sbus->cached_area.right = right;
        sbus->cached_area.top = top;
        sbus->cached_area.bottom = bottom;
        return;
    }

    if (bus->bus_type == DGX_BUS_SPI) {
        dgx_screen_with_bus_set_area_window_spi_full_async(sbus, left, right, top, bottom);
        return;
    }

    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
        uint8_t xdata[2] = {(uint8_t)left, (uint8_t)right};
        uint8_t ydata[2] = {(uint8_t)top, (uint8_t)bottom};
        bus->write_command(bus, sbus->xcmd_set);
        bus->write_data(bus, xdata, sizeof(xdata) * 8u);
        bus->write_command(bus, sbus->ycmd_set);
        bus->write_data(bus, ydata, sizeof(ydata) * 8u);
    } else {
        uint8_t xdata[4] = {left >> 8, left & 0xff, right >> 8, right & 0xff};
        uint8_t ydata[4] = {top >> 8, top & 0xff, bottom >> 8, bottom & 0xff};
        bus->write_command(bus, sbus->xcmd_set);
        bus->write_data(bus, xdata, sizeof(xdata) * 8u);
        bus->write_command(bus, sbus->ycmd_set);
        bus->write_data(bus, ydata, sizeof(ydata) * 8u);
    }
    bus->write_command(bus, sbus->wcmd_send);
    sbus->cached_area.left = left;
    sbus->cached_area.right = right;
    sbus->cached_area.top = top;
    sbus->cached_area.bottom = bottom;
}