#include "dgx_spi_area_protocol.h"

#include <stddef.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include "bus/dgx_bus_protocols.h"

/* the short transfers of an SPI bus are its own: without that bus there is nothing to link them with */
#if CONFIG_DGX_ENABLE_SPI
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
#endif /* CONFIG_DGX_ENABLE_SPI */

/* the window as six commands of a controller that takes its bounds as commands */
static void dgx_screen_with_bus_send_window_cmd8(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    dgx_bus_protocols_t *bus = sbus->bus;
    uint8_t crset[6] = {
        sbus->xcmd_set, (uint8_t)left, (uint8_t)right,
        sbus->ycmd_set, (uint8_t)top,  (uint8_t)bottom,
    };
    bus->write_commands(bus, crset, sizeof(crset) * 8u);
}

/* a bound of the window: a command and its two numbers, a byte or two each by the protocol of the controller */
static void dgx_screen_with_bus_send_range(dgx_screen_with_bus_t *sbus, uint8_t cmd, uint16_t from, uint16_t to)
{
    dgx_bus_protocols_t *bus = sbus->bus;
    bus->write_command(bus, cmd);
    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_STD8) {
        uint8_t range[2] = {(uint8_t)from, (uint8_t)to};
        bus->write_data(bus, range, sizeof(range) * 8u);
    } else {
        uint8_t range[4] = {from >> 8, from & 0xff, to >> 8, to & 0xff};
        bus->write_data(bus, range, sizeof(range) * 8u);
    }
}

/*
 * The window by the commands of the bus, and the command that writes memory
 * after it. A bound that has not changed is not sent again, unless `full`.
 */
static void dgx_screen_with_bus_send_window(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom, bool full)
{
    dgx_bus_protocols_t *bus = sbus->bus;
    if (full || sbus->cached_area.left != left || sbus->cached_area.right != right) {
        dgx_screen_with_bus_send_range(sbus, sbus->xcmd_set, left, right);
        sbus->cached_area.left  = left;
        sbus->cached_area.right = right;
    }
    if (full || sbus->cached_area.top != top || sbus->cached_area.bottom != bottom) {
        dgx_screen_with_bus_send_range(sbus, sbus->ycmd_set, top, bottom);
        sbus->cached_area.top    = top;
        sbus->cached_area.bottom = bottom;
    }
    bus->write_command(bus, sbus->wcmd_send);
}

bool dgx_screen_with_bus_window_is_deferred(const dgx_screen_with_bus_t *sbus)
{
    return sbus->bus->bus_type == DGX_BUS_P8 && sbus->area_protocol != DGX_SCREEN_AREA_PROTO_CMD8;
}

/* the window is only remembered: the display is told of it when pixels come */
static void dgx_screen_with_bus_keep_window(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom, bool full)
{
    sbus->window.left    = left;
    sbus->window.right   = right;
    sbus->window.top     = top;
    sbus->window.bottom  = bottom;
    sbus->window.x       = left;
    sbus->window.y       = top;
    sbus->window.full    = full;
    sbus->window.started = false;
}

/* bytes a pixel takes in a buffer when it takes whole bytes, 0 when pixels share bytes */
static uint32_t dgx_screen_with_bus_pixel_bytes(uint8_t color_bits)
{
    switch (color_bits) {
    case 8:
        return 1;
    case 16:
        return 2;
    case 18:
    case 24:
        return 3;
    case 32:
        return 4;
    default:
        return 0;
    }
}

void dgx_screen_with_bus_write_window(dgx_screen_with_bus_t *sbus, const uint8_t *data, uint32_t lenbits)
{
    dgx_bus_protocols_t *bus = sbus->bus;
    const uint32_t       pixel_bytes = dgx_screen_with_bus_pixel_bytes(sbus->scr.color_bits);
    if (!pixel_bytes) {
        /*
         * Pixels that share bytes, 12 bits each, cannot be cut where a row
         * ends. The window is sent once, with the first of them, and what
         * comes later goes as data alone: a bus that has no such transfer
         * does not take it.
         */
        if (!sbus->window.started) {
            dgx_screen_with_bus_send_window(sbus, sbus->window.left, sbus->window.right, sbus->window.top, sbus->window.bottom, sbus->window.full);
            sbus->window.started = true;
        }
        bus->write_data_async(bus, data, lenbits);
        return;
    }
    const uint32_t width = (uint32_t)sbus->window.right - sbus->window.left + 1u;
    /* a transfer of the bus is no longer than its buffer */
    const uint32_t most = bus->buffer_len >= pixel_bytes ? (uint32_t)(bus->buffer_len / pixel_bytes) : UINT32_MAX;
    uint32_t       pixels = lenbits / (8u * pixel_bytes);
    while (pixels) {
        /* what is left of the window: its rows from the next one on, or first the rest of a row begun */
        uint32_t count;
        if (sbus->window.x == sbus->window.left) {
            count = ((uint32_t)sbus->window.bottom - sbus->window.y + 1u) * width;
            dgx_screen_with_bus_send_window(sbus, sbus->window.left, sbus->window.right, sbus->window.y, sbus->window.bottom, sbus->window.full);
        } else {
            count = (uint32_t)sbus->window.right - sbus->window.x + 1u;
            dgx_screen_with_bus_send_window(sbus, sbus->window.x, sbus->window.right, sbus->window.y, sbus->window.y, sbus->window.full);
        }
        if (count > pixels) count = pixels;
        if (count > most) count = most;
        bus->write_data_async(bus, data, count * pixel_bytes * 8u);
        data += (size_t)count * pixel_bytes;
        pixels -= count;
        /* the next pixel; after the last one of the window comes its first */
        const uint32_t at = (uint32_t)(sbus->window.x - sbus->window.left) + count;
        const uint32_t y = sbus->window.y + at / width;
        sbus->window.x = (uint16_t)(sbus->window.left + at % width);
        sbus->window.y = y > sbus->window.bottom ? sbus->window.top : (uint16_t)y;
    }
    sbus->window.started = true;
}

void dgx_screen_with_bus_set_area_window(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_CMD8) {
        dgx_screen_with_bus_send_window_cmd8(sbus, left, right, top, bottom);
        return;
    }

#if CONFIG_DGX_ENABLE_SPI
    if (sbus->bus->bus_type == DGX_BUS_SPI) {
        dgx_screen_with_bus_set_area_window_spi(sbus, left, right, top, bottom, false);
        return;
    }
#endif

    if (dgx_screen_with_bus_window_is_deferred(sbus)) {
        dgx_screen_with_bus_keep_window(sbus, left, right, top, bottom, false);
        return;
    }
    dgx_screen_with_bus_send_window(sbus, left, right, top, bottom, false);
}

void dgx_screen_with_bus_set_area_window_full(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top,
                                              uint16_t bottom)
{
    if (sbus->area_protocol == DGX_SCREEN_AREA_PROTO_CMD8) {
        dgx_screen_with_bus_send_window_cmd8(sbus, left, right, top, bottom);
        sbus->cached_area.left = left;
        sbus->cached_area.right = right;
        sbus->cached_area.top = top;
        sbus->cached_area.bottom = bottom;
        return;
    }

#if CONFIG_DGX_ENABLE_SPI
    if (sbus->bus->bus_type == DGX_BUS_SPI) {
        dgx_screen_with_bus_set_area_window_spi(sbus, left, right, top, bottom, true);
        return;
    }
#endif

    if (dgx_screen_with_bus_window_is_deferred(sbus)) {
        dgx_screen_with_bus_keep_window(sbus, left, right, top, bottom, true);
        return;
    }
    dgx_screen_with_bus_send_window(sbus, left, right, top, bottom, true);
}
