#include "dgx_spi_area_protocol.h"

#include <string.h>

#include "bus/dgx_bus_protocols.h"
#include "bus/dgx_spi_esp32.h"
#include "bus/dgx_spi_esp32_priv.h"

/*
 * One short command or parameter transfer. These are 1-4 bytes long: polling
 * them takes a fraction of the time a queued, interrupt-driven transaction
 * needs, and that is what a single pixel or a short line mostly consists of.
 */
static void dgx_spi_area_send(dgx_spi_bus_t *bus, bool is_data, const uint8_t *bytes, uint8_t count)
{
    spi_transaction_t *trans = &bus->trans_sync;
    memset(trans, 0, sizeof(*trans));
    trans->length = count * 8u;
    trans->flags  = SPI_TRANS_USE_TXDATA;
    memcpy(trans->tx_data, bytes, count);
    trans->user = set_dc_pin(bus->dcio, is_data);
    spi_device_polling_transmit(bus->spi, trans);
}

static void dgx_spi_area_send_range(dgx_screen_with_bus_t *sbus, uint8_t cmd, uint16_t from, uint16_t to)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)sbus->bus;
    dgx_spi_area_send(bus, false, &cmd, 1);
    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
        const uint8_t range[2] = {(uint8_t)from, (uint8_t)to};
        dgx_spi_area_send(bus, true, range, sizeof(range));
    } else {
        const uint8_t range[4] = {from >> 8, from & 0xff, to >> 8, to & 0xff};
        dgx_spi_area_send(bus, true, range, sizeof(range));
    }
}

static void dgx_screen_with_bus_set_area_window_spi(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom,
                                                    bool full)
{
    dgx_spi_bus_t *bus = (dgx_spi_bus_t *)sbus->bus;

    /* pixel data of the previous operation may still be queued */
    dgx_spi_wait_pending((dgx_bus_protocols_t *)bus);

    if (full || sbus->cached_area.left != left || sbus->cached_area.right != right) {
        dgx_spi_area_send_range(sbus, sbus->xcmd_set, left, right);
        sbus->cached_area.left  = left;
        sbus->cached_area.right = right;
    }
    if (full || sbus->cached_area.top != top || sbus->cached_area.bottom != bottom) {
        dgx_spi_area_send_range(sbus, sbus->ycmd_set, top, bottom);
        sbus->cached_area.top    = top;
        sbus->cached_area.bottom = bottom;
    }
    dgx_spi_area_send(bus, false, &sbus->wcmd_send, 1);
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
        dgx_screen_with_bus_set_area_window_spi(sbus, left, right, top, bottom, false);
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
        dgx_screen_with_bus_set_area_window_spi(sbus, left, right, top, bottom, true);
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