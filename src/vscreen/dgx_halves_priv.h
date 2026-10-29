#pragma once
/*
 * dgx_halves_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Pixels sent into the window of a display behind a bus through the two
 *  halves of the bus buffer: the window set, the halves filled in turn and
 *  sent. Virtual screens put on a display (transfer.c), the frame of the
 *  glow (morph_render.c) and the two panels of dgx_vscreen_2h_init() go this
 *  one way.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dgx_screen_with_bus.h"
#include "dgx_vscreen_priv.h"

/* a display behind a bus with a buffer for a pixel of `bytes` */
static inline bool dgx_transfer_bus_takes(const dgx_screen_t *dst, uint8_t bytes)
{
    if (!bytes || dst->screen_subtype != DgxPhysicalScreenWithBus || dgx_vscreen_pixel_bytes(dst->color_bits) != bytes) return false;
    const dgx_bus_protocols_t *bus = ((const dgx_screen_with_bus_t *)dst)->bus;
    return bus->buffer && bus->buffer_len >= bytes;
}

/*
 * The transfer buffer in two halves: while one goes to the display, the
 * pixels of the next are put into the other. Sending a piece returns once
 * the pieces before it have gone, so a half is never written while it is
 * being sent; a bus that queues the piece before it waits, as SPI does, goes
 * on to the next half with no pause. A half is of whole pixels, and of
 * whole rows where a row fits in it: a row is then not cut, and a display
 * whose window goes with its pixels is not told a window for the rest of a
 * row. What is left over after whole halves goes first, so the wire starts
 * once that much is copied, and the pieces are as many. A rectangle the
 * whole buffer takes goes in one piece: a second one would cost a transfer
 * of its own. A buffer too small for two halves of a pixel each is one half,
 * and every piece of it is waited for.
 */
typedef struct {
    dgx_screen_t *scr;
    uint8_t      *half[2];
    uint8_t      *at;    /* where the next byte goes */
    uint32_t      len;   /* bytes of a half */
    uint32_t      cap;   /* bytes of the half being filled: len, or the leftover that goes first */
    int           which; /* the half being filled */
    bool          alone; /* one half only */
} dgx_vscreen_halves_t;

static inline void dgx_vscreen_halves_begin(dgx_vscreen_halves_t *h, dgx_screen_t *scr, uint8_t *buffer, size_t buffer_len, uint32_t pixel_bytes,
                                            uint32_t row_bytes, uint32_t all_bytes)
{
    uint32_t len = (uint32_t)(buffer_len / 2u) / pixel_bytes * pixel_bytes;
    h->alone     = false;
    if (all_bytes <= buffer_len) {
        len = all_bytes;
    } else {
        if (len == 0) len = (uint32_t)buffer_len / pixel_bytes * pixel_bytes, h->alone = true;
        /* the halves start at buffer and buffer+len and go out through the SPI DMA,
         * which takes an address and a length of a multiple of 4 bytes: one that is
         * not, the driver copies into a buffer of its own (setup_priv_desc of
         * esp_driver_spi). A multiple of the row length and of 4 bytes at once: a
         * row that is itself one needs no more rows, one that is 2 mod 4 needs an
         * even number of them, an odd row needs four. A half too small for that
         * many rows is cut at rows only, as it was. */
        if (len >= row_bytes) {
            uint32_t rows = 1;
            while (row_bytes * rows % 4u) ++rows;
            const uint32_t cut = row_bytes * rows;
            len                = len >= cut ? len / cut * cut : len / row_bytes * row_bytes;
        }
    }
    h->scr     = scr;
    h->half[0] = h->at = buffer;
    h->half[1]         = h->alone ? buffer : buffer + len;
    h->len             = len;
    h->cap             = len;
    h->which           = 0;
    /* what is left over after whole halves goes first: the wire starts once that much is copied, with as many pieces */
    if (all_bytes > buffer_len && !h->alone && all_bytes % len) h->cap = all_bytes % len;
}

/* bytes left in the half being filled */
static inline uint32_t dgx_vscreen_halves_room(const dgx_vscreen_halves_t *h)
{
    return h->cap - (uint32_t)(h->at - h->half[h->which]);
}

/* the half filled goes to the display, the other one is filled next */
static inline void dgx_vscreen_halves_send(dgx_vscreen_halves_t *h)
{
    h->scr->write_area(h->scr, h->half[h->which], 8u * (uint32_t)(h->at - h->half[h->which]));
    if (h->alone) h->scr->wait_buffer(h->scr);
    h->which ^= 1;
    h->at  = h->half[h->which];
    h->cap = h->len;
}

/* what is left goes; it may still be on its way when this returns */
static inline void dgx_vscreen_halves_flush(dgx_vscreen_halves_t *h)
{
    if (h->at != h->half[h->which]) dgx_vscreen_halves_send(h);
}

/* what is left goes, and all of it has gone when this returns */
static inline void dgx_vscreen_halves_end(dgx_vscreen_halves_t *h)
{
    dgx_vscreen_halves_flush(h);
    h->scr->wait_buffer(h->scr);
}

/* the window of a display behind a bus set, and its buffer in halves for a rectangle of `bytes` a pixel */
static inline void dgx_vscreen_halves_window(dgx_vscreen_halves_t *h, dgx_screen_t *dst, int x, int y, int w, int h_rows, uint32_t bytes)
{
    dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)dst)->bus;
    /* the buffer free first: waiting after the window would send its memory write alone, ahead of the pixels */
    dst->wait_buffer(dst);
    dst->set_area(dst, (uint16_t)x, (uint16_t)(x + w - 1), (uint16_t)y, (uint16_t)(y + h_rows - 1));
    const uint32_t row_bytes = (uint32_t)w * bytes;
    dgx_vscreen_halves_begin(h, dst, bus->buffer, bus->buffer_len, bytes, row_bytes, row_bytes * (uint32_t)h_rows);
}

/*
 * A run of whole pixels put into the halves. A virtual screen keeps its
 * pixels in the byte order of the wire, so this is a plain copy.
 */
static inline void dgx_vscreen_send_row(dgx_vscreen_halves_t *h, const uint8_t *src, uint32_t bytes)
{
    while (bytes) {
        const uint32_t room  = dgx_vscreen_halves_room(h);
        const uint32_t chunk = bytes < room ? bytes : room;
        memcpy(h->at, src, chunk);
        h->at += chunk;
        src += chunk;
        bytes -= chunk;
        if (chunk == room) dgx_vscreen_halves_send(h);
    }
}
