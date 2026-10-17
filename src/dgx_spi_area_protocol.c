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

/* a bus that takes pixels only together with their command */
static bool dgx_screen_with_bus_bus_defers(const dgx_screen_with_bus_t *sbus)
{
    return sbus->bus->bus_type == DGX_BUS_P8 && sbus->area_protocol != DGX_SCREEN_AREA_PROTO_CMD8;
}

bool dgx_screen_with_bus_window_is_deferred(const dgx_screen_with_bus_t *sbus)
{
    return sbus->window.cut || dgx_screen_with_bus_bus_defers(sbus);
}

/* the window told to the display now, the way its bus does it best */
static void dgx_screen_with_bus_tell_window(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom, bool full)
{
#if CONFIG_DGX_ENABLE_SPI
    if (sbus->bus->bus_type == DGX_BUS_SPI) {
        dgx_screen_with_bus_set_area_window_spi(sbus, left, right, top, bottom, full);
        return;
    }
#endif
    dgx_screen_with_bus_send_window(sbus, left, right, top, bottom, full);
}

/*
 * The scroll of the panel. Addresses are those of a window along the way of
 * the scroll. An address of the area scrolled lies in memory `shift` further,
 * turned round within the area; other addresses lie where they are.
 */
static inline uint16_t dgx_screen_with_bus_scroll_map(const dgx_screen_with_bus_t *sbus, uint16_t address)
{
    const uint32_t at = (uint32_t)address - sbus->scroll.first; /* below the area it is a large number */
    if (at >= sbus->scroll.count) return address;
    const uint32_t to = at + sbus->scroll.shift;
    return (uint16_t)(sbus->scroll.first + (to >= sbus->scroll.count ? to - sbus->scroll.count : to));
}

/* the last address from `address` on, not past `last`, up to which rows lie in memory one after another */
static inline uint16_t dgx_screen_with_bus_scroll_run(const dgx_screen_with_bus_t *sbus, uint16_t address, uint16_t last)
{
    const uint32_t first = sbus->scroll.first, end = first + sbus->scroll.count, turn = end - sbus->scroll.shift;
    const uint32_t stop = address < first ? first : address < turn ? turn : address < end ? end : UINT32_MAX;
    return last < stop ? last : (uint16_t)(stop - 1u);
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

/*
 * A window as it was asked for, to be led to where the scroll of the panel
 * has its rows. One that the scroll does not cut gets other bounds and goes
 * on as any window; one that it cuts is kept, and goes in pieces when its
 * pixels come. Returns true when the window is kept.
 */
static bool dgx_screen_with_bus_scroll_window(dgx_screen_with_bus_t *sbus, uint16_t *left, uint16_t *right, uint16_t *top, uint16_t *bottom,
                                              bool full)
{
    sbus->window.cut = false;
    if (!sbus->scroll.shift) return false;
    uint16_t *from = sbus->scroll.columns ? left : top, *to = sbus->scroll.columns ? right : bottom;
    if (dgx_screen_with_bus_scroll_run(sbus, *from, *to) != *to) {
        dgx_screen_with_bus_keep_window(sbus, *left, *right, *top, *bottom, full);
        sbus->window.cut = true;
        return true;
    }
    *from = dgx_screen_with_bus_scroll_map(sbus, *from);
    *to   = dgx_screen_with_bus_scroll_map(sbus, *to);
    return false;
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
            dgx_screen_with_bus_tell_window(sbus, sbus->window.left, sbus->window.right, sbus->window.top, sbus->window.bottom, sbus->window.full);
            sbus->window.started = true;
        }
        bus->write_data_async(bus, data, lenbits);
        return;
    }
    const uint32_t width = (uint32_t)sbus->window.right - sbus->window.left + 1u;
    /* a transfer of the bus is no longer than its buffer */
    const uint32_t most = bus->buffer_len >= pixel_bytes ? (uint32_t)(bus->buffer_len / pixel_bytes) : UINT32_MAX;
    uint32_t       pixels = lenbits / (8u * pixel_bytes);
    /* a window the scroll of the panel cuts: across its rows, or across its columns, and then every row of it is cut */
    const bool rows_cut = sbus->window.cut && !sbus->scroll.columns, columns_cut = sbus->window.cut && sbus->scroll.columns;
    while (pixels) {
        /*
         * What is left of the window: its rows from the next one on, or first
         * the rest of a row begun. Under a scroll, as much of that as lies in
         * memory in one piece, sent to where it lies.
         */
        uint32_t count;
        if (columns_cut) {
            const uint16_t last = dgx_screen_with_bus_scroll_run(sbus, sbus->window.x, sbus->window.right);
            count = (uint32_t)last - sbus->window.x + 1u;
            dgx_screen_with_bus_tell_window(sbus, dgx_screen_with_bus_scroll_map(sbus, sbus->window.x), dgx_screen_with_bus_scroll_map(sbus, last),
                                            sbus->window.y, sbus->window.y, sbus->window.full);
        } else if (sbus->window.x == sbus->window.left) {
            const uint16_t last = rows_cut ? dgx_screen_with_bus_scroll_run(sbus, sbus->window.y, sbus->window.bottom) : sbus->window.bottom;
            const uint16_t top = rows_cut ? dgx_screen_with_bus_scroll_map(sbus, sbus->window.y) : sbus->window.y;
            count = ((uint32_t)last - sbus->window.y + 1u) * width;
            dgx_screen_with_bus_tell_window(sbus, sbus->window.left, sbus->window.right, top, (uint16_t)(top + (last - sbus->window.y)),
                                            sbus->window.full);
        } else {
            const uint16_t row = rows_cut ? dgx_screen_with_bus_scroll_map(sbus, sbus->window.y) : sbus->window.y;
            count = (uint32_t)sbus->window.right - sbus->window.x + 1u;
            dgx_screen_with_bus_tell_window(sbus, sbus->window.x, sbus->window.right, row, row, sbus->window.full);
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
    if (dgx_screen_with_bus_scroll_window(sbus, &left, &right, &top, &bottom, false)) return;

#if CONFIG_DGX_ENABLE_SPI
    if (sbus->bus->bus_type == DGX_BUS_SPI) {
        dgx_screen_with_bus_set_area_window_spi(sbus, left, right, top, bottom, false);
        return;
    }
#endif

    if (dgx_screen_with_bus_bus_defers(sbus)) {
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
    if (dgx_screen_with_bus_scroll_window(sbus, &left, &right, &top, &bottom, true)) return;

#if CONFIG_DGX_ENABLE_SPI
    if (sbus->bus->bus_type == DGX_BUS_SPI) {
        dgx_screen_with_bus_set_area_window_spi(sbus, left, right, top, bottom, true);
        return;
    }
#endif

    if (dgx_screen_with_bus_bus_defers(sbus)) {
        dgx_screen_with_bus_keep_window(sbus, left, right, top, bottom, true);
        return;
    }
    dgx_screen_with_bus_send_window(sbus, left, right, top, bottom, true);
}

void dgx_screen_with_bus_scroll_init(dgx_screen_with_bus_t *sbus, uint16_t lines, uint8_t area_cmd, uint8_t start_cmd)
{
    sbus->scroll.lines     = lines;
    sbus->scroll.area_cmd  = area_cmd;
    sbus->scroll.start_cmd = start_cmd;
    sbus->scroll.first = sbus->scroll.count = sbus->scroll.shift = 0;
}

/* a command and its numbers, two bytes each, the higher byte first */
static void dgx_screen_with_bus_scroll_send(dgx_screen_with_bus_t *sbus, uint8_t cmd, const uint16_t *numbers, uint8_t count)
{
    dgx_bus_protocols_t *bus = sbus->bus;
    uint8_t              bytes[6];
    for (uint8_t i = 0; i < count; ++i) bytes[2 * i] = (uint8_t)(numbers[i] >> 8), bytes[2 * i + 1] = (uint8_t)numbers[i];
    bus->write_command(bus, cmd);
    bus->write_data(bus, bytes, count * 16u);
}

/*
 * The area and its shift told to the controller. It counts in rows of its
 * memory from the top of the panel, addresses may run the other way; and it
 * is told the row of memory the first row of the area shows.
 */
static void dgx_screen_with_bus_scroll_tell(dgx_screen_with_bus_t *sbus, bool area)
{
    const uint16_t lines = sbus->scroll.lines, count = sbus->scroll.count, shift = sbus->scroll.shift;
    const uint16_t above = sbus->scroll.mirror ? (uint16_t)(lines - sbus->scroll.first - count) : sbus->scroll.first;
    if (area) {
        const uint16_t parts[3] = {above, count, (uint16_t)(lines - above - count)};
        dgx_screen_with_bus_scroll_send(sbus, sbus->scroll.area_cmd, parts, 3);
    }
    const uint16_t start = (uint16_t)(above + (sbus->scroll.mirror && shift ? count - shift : shift));
    dgx_screen_with_bus_scroll_send(sbus, sbus->scroll.start_cmd, &start, 1);
}

void dgx_screen_with_bus_scroll_way(dgx_screen_with_bus_t *sbus, bool columns, bool mirror)
{
    if (sbus->scroll.lines && sbus->scroll.count) {
        /* all the rows are one area again, shown from its first */
        sbus->scroll.first = sbus->scroll.shift = 0;
        sbus->scroll.count = sbus->scroll.lines;
        dgx_screen_with_bus_scroll_tell(sbus, true);
        sbus->scroll.count = 0;
    }
    sbus->scroll.columns = columns;
    sbus->scroll.mirror  = mirror;
}

esp_err_t dgx_screen_with_bus_scroll(dgx_screen_with_bus_t *sbus, int x, int y, int w, int h, int dx, int dy, bool fill, uint32_t color)
{
    dgx_screen_t *scr     = &sbus->scr;
    const bool    columns = sbus->scroll.columns;
    /* along the rows of memory: where the rectangle begins, how long it is and how far the picture goes */
    const int  from = columns ? x : y, count = columns ? w : h, move = columns ? dx : dy;
    /* across them it takes the screen from edge to edge, and the picture does not go that way */
    const bool across = columns ? y == 0 && h == scr->height && dy == 0 : x == 0 && w == scr->width && dx == 0;
    const int  first  = from + (columns ? scr->cg_col_shift : scr->cg_row_shift);
    const bool same   = sbus->scroll.count == count && sbus->scroll.first == first;
    if (dx >= w || dx <= -w || dy >= h || dy <= -h) {
        /* nothing of the picture stays. The area the panel scrolls is let go when it is the one filled: one color has no shift to it */
        if (fill) scr->fill_rectangle(scr, x, y, w, h, color);
        if (sbus->scroll.lines && sbus->scroll.count && same && (columns ? y == 0 && h == scr->height : x == 0 && w == scr->width)) {
            sbus->scroll.shift = 0;
            dgx_screen_with_bus_scroll_tell(sbus, false);
            sbus->scroll.count = 0;
        }
        return ESP_OK;
    }
    if (!sbus->scroll.lines || !across || !dgx_screen_with_bus_pixel_bytes(scr->color_bits)) return ESP_ERR_NOT_SUPPORTED;
    if (first + count > sbus->scroll.lines) return ESP_ERR_NOT_SUPPORTED;
    /* the panel has one area, and one that is shifted cannot be given up for another: its rows are not where they are shown */
    if (sbus->scroll.count && sbus->scroll.shift && !same) return ESP_ERR_NOT_SUPPORTED;
    if (!same) {
        sbus->scroll.first = (uint16_t)first;
        sbus->scroll.count = (uint16_t)count;
        sbus->scroll.shift = 0;
        dgx_screen_with_bus_scroll_tell(sbus, true);
    }
    /*
     * The strip that leaves is filled before the move: it is the strip that
     * comes in at the other end, and comes in as the color. The fill costs
     * more than the commands of the panel, so a caller that draws the strip
     * anyway asks to skip it.
     */
    if (fill) {
        const int strip = move < 0 ? -move : move, at = move > 0 ? from + count - strip : from;
        if (columns) scr->fill_rectangle(scr, at, y, strip, h, color);
        else scr->fill_rectangle(scr, x, at, w, strip, color);
        if (scr->wait_buffer) scr->wait_buffer(scr);
    }
    sbus->scroll.shift = (uint16_t)((sbus->scroll.shift + count - move) % count);
    dgx_screen_with_bus_scroll_tell(sbus, false);
    return ESP_OK;
}
