/*
 * transfer.c
 *
 *  A virtual screen put on a screen, a display or another virtual screen:
 *  as it is, or made larger, smoothed, turned and with its pixels of value
 *  0 left out; at the same depth, or as 8-bit indices through a table of
 *  colors on a 16-bit screen.
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_arch.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_overlay_priv.h"
#include "dgx_scale_priv.h"
#include "dgx_screen_with_bus.h"
#include "dgx_vscreen_priv.h"
#include "drivers/vscreen.h"

static const char TAG[] = "DGX V-Screen";

/* ------------------------------------------------------------------------- */
/*  What a transfer is asked for, and where it puts what                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool    transparent;
    bool    exact;
    uint8_t scale;  /* 1 to 255 */
    uint8_t smooth; /* 0, DGX_BUS_SMOOTH or DGX_BUS_SMOOTH_SFX; 0 at any scale but 2, 3 and 4 */
    uint8_t turn;   /* a dgx_output_orientation_t: bit 0 the columns of the source from the right, bit 1 its rows from the bottom, bit 2 the axes swapped */
} dgx_transfer_asked_t;

static dgx_transfer_asked_t dgx_transfer_asked(uint32_t flags)
{
    dgx_transfer_asked_t asked;
    asked.transparent = (flags & DGX_VSCREEN_TRANSPARENT) != 0;
    asked.exact       = (flags & DGX_VSCREEN_EXACT) != 0;
    asked.scale       = (flags >> 8) & 0xffu ? (uint8_t)((flags >> 8) & 0xffu) : 1u;
    /* SFX is a way of smoothing: its bit means nothing without DGX_VSCREEN_SMOOTH, which DGX_VSCREEN_SMOOTH_SFX holds */
    asked.smooth      = asked.scale < 2 || asked.scale > 4 || !(flags & DGX_VSCREEN_SMOOTH) ? 0 : flags & 8u ? DGX_BUS_SMOOTH_SFX : DGX_BUS_SMOOTH;
    asked.turn        = (uint8_t)((flags >> 16) & 7u);
    return asked;
}

/*
 * The source made `scale` times as large and then turned is a picture, the
 * turned one. The region asked for is a rectangle of it, and its corner goes
 * to (x, y). What of that rectangle the turned picture has and the
 * destination has room for is put: the rectangle (x, y, w, h) of the
 * destination, its corner showing the point (u, v) of the turned picture.
 */
typedef struct {
    int     x, y, w, h;
    int32_t u, v;
    int32_t wide, high; /* the larger picture before it is turned */
    uint8_t scale, turn;
} dgx_transfer_place_t;

/* the same as dgx_transfer_place_far() below, in an int for coordinates near enough: a small sprite put many times costs less so */
static inline bool dgx_transfer_place_near(const dgx_screen_t *dst, const dgx_screen_t *src, int x, int y, int x_src, int y_src, int width,
                                           int height, const dgx_transfer_asked_t *asked, dgx_transfer_place_t *place)
{
    const int  n = asked->scale, wide = src->width * n, high = src->height * n;
    const bool swapped = asked->turn & 4u;
    const int  c0 = x_src * n, c1 = (x_src + width) * n, r0 = y_src * n, r1 = (y_src + height) * n;
    const int  cl = asked->turn & 1u ? wide - c1 : c0, ch = asked->turn & 1u ? wide - c0 : c1;
    const int  rl = asked->turn & 2u ? high - r1 : r0, rh = asked->turn & 2u ? high - r0 : r1;
    const int  ul = swapped ? rl : cl, uh = swapped ? rh : ch, vl = swapped ? cl : rl, vh = swapped ? ch : rh;
    const int  tw = swapped ? high : wide, th = swapped ? wide : high;
    int        left = x + ((ul > 0 ? ul : 0) - ul), right = x + ((uh < tw ? uh : tw) - ul);
    int        top = y + ((vl > 0 ? vl : 0) - vl), bottom = y + ((vh < th ? vh : th) - vl);
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > dst->width) right = dst->width;
    if (bottom > dst->height) bottom = dst->height;
    if (right <= left || bottom <= top) return false;
    place->x = left, place->y = top, place->w = right - left, place->h = bottom - top;
    place->u = ul + (left - x), place->v = vl + (top - y);
    place->wide = wide, place->high = high;
    place->scale = asked->scale, place->turn = asked->turn;
    return true;
}

/*
 * Counted in 64 bits: a region or a place however far off is cut where it is,
 * and nothing overflows on the way. Out of line, as only a far one comes
 * here: inlined, it made the usual place of a small sprite slower on the
 * ESP32.
 */
static __attribute__((noinline)) bool dgx_transfer_place_far(const dgx_screen_t *dst, const dgx_screen_t *src, int x, int y, int x_src, int y_src,
                                                             int width, int height, const dgx_transfer_asked_t *asked, dgx_transfer_place_t *place)
{
    const int64_t n = asked->scale, wide = (int64_t)src->width * n, high = (int64_t)src->height * n;
    const bool    swapped = asked->turn & 4u;
    /* the region in the larger picture, along its columns and along its rows, counted the way the turn takes them */
    const int64_t c0 = (int64_t)x_src * n, c1 = ((int64_t)x_src + width) * n, r0 = (int64_t)y_src * n, r1 = ((int64_t)y_src + height) * n;
    const int64_t cl = asked->turn & 1u ? wide - c1 : c0, ch = asked->turn & 1u ? wide - c0 : c1;
    const int64_t rl = asked->turn & 2u ? high - r1 : r0, rh = asked->turn & 2u ? high - r0 : r1;
    /* the rectangle in the turned picture, and as much of it as that has */
    const int64_t ul = swapped ? rl : cl, uh = swapped ? rh : ch, vl = swapped ? cl : rl, vh = swapped ? ch : rh;
    const int64_t tw = swapped ? high : wide, th = swapped ? wide : high;
    int64_t       left = x + ((ul > 0 ? ul : 0) - ul), right = x + ((uh < tw ? uh : tw) - ul);
    int64_t       top = y + ((vl > 0 ? vl : 0) - vl), bottom = y + ((vh < th ? vh : th) - vl);
    /* and of the destination */
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > dst->width) right = dst->width;
    if (bottom > dst->height) bottom = dst->height;
    if (right <= left || bottom <= top) return false;
    place->x = (int)left, place->y = (int)top, place->w = (int)(right - left), place->h = (int)(bottom - top);
    place->u = (int32_t)(ul + (left - x)), place->v = (int32_t)(vl + (top - y));
    place->wide = (int32_t)wide, place->high = (int32_t)high;
    place->scale = asked->scale, place->turn = asked->turn;
    return true;
}

/* in the call itself, the far place out of line: a small sprite put many times does not pay for a call */
static inline __attribute__((always_inline)) bool dgx_transfer_place(const dgx_screen_t *dst, const dgx_screen_t *src, int x, int y, int x_src,
                                                                     int y_src, int width, int height, const dgx_transfer_asked_t *asked,
                                                                     dgx_transfer_place_t *place)
{
    if (width <= 0 || height <= 0) return false;
    /*
     * Coordinates within 2^21 either way: the end of a region, its start and
     * its width added, times a scale of up to 255, and what the place adds to
     * it stay within an int. Moved up by 2^21, each such one is below 2^22,
     * and so is what they make together bit by bit: one test, which the
     * usual place passes.
     */
    const uint32_t near = 1u << 21;
    const uint32_t all  = ((uint32_t)x + near) | ((uint32_t)y + near) | ((uint32_t)x_src + near) | ((uint32_t)y_src + near) |
                         ((uint32_t)width + near) | ((uint32_t)height + near);
    if (__builtin_expect(all < 2u * near, 1)) return dgx_transfer_place_near(dst, src, x, y, x_src, y_src, width, height, asked, place);
    return dgx_transfer_place_far(dst, src, x, y, x_src, y_src, width, height, asked, place);
}

static inline bool dgx_transfer_virtual(const dgx_screen_t *scr)
{
    return scr->screen_subtype == DgxVirtualScreen || scr->screen_subtype == DgxVirtualBackScreen;
}

/* a display behind a bus with a buffer for a pixel of `bytes` */
static bool dgx_transfer_bus_takes(const dgx_screen_t *dst, uint8_t bytes)
{
    if (!bytes || dst->screen_subtype != DgxPhysicalScreenWithBus || dgx_vscreen_pixel_bytes(dst->color_bits) != bytes) return false;
    const dgx_bus_protocols_t *bus = ((const dgx_screen_with_bus_t *)dst)->bus;
    return bus->buffer && bus->buffer_len >= bytes;
}

/* ------------------------------------------------------------------------- */
/*  The buffer of a bus in two halves                                        */
/* ------------------------------------------------------------------------- */

/*
 * The transfer buffer in two halves: while one goes to the display, the
 * pixels of the next are put into the other. Sending a piece returns once
 * the pieces before it have gone, so a half is never written while it is
 * being sent; a bus that queues the piece before it waits, as SPI does, goes
 * on to the next half with no pause. A half is of whole pixels, and of
 * whole rows where a row fits in it: a row is then not cut, and a display
 * whose window goes with its pixels is not told a window for the rest of a
 * row. A rectangle the whole buffer takes goes in one piece: a second one
 * would cost a transfer of its own. A buffer too small for two halves of a
 * pixel each is one half, and every piece of it is waited for.
 */
typedef struct {
    dgx_screen_t *scr;
    uint8_t      *half[2];
    uint8_t      *at;    /* where the next byte goes */
    uint32_t      len;   /* bytes of a half */
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
        if (len >= row_bytes) len = len / row_bytes * row_bytes;
    }
    h->scr     = scr;
    h->half[0] = h->at = buffer;
    h->half[1]         = h->alone ? buffer : buffer + len;
    h->len             = len;
    h->which           = 0;
}

/* bytes left in the half being filled */
static inline uint32_t dgx_vscreen_halves_room(const dgx_vscreen_halves_t *h)
{
    return h->len - (uint32_t)(h->at - h->half[h->which]);
}

/* the half filled goes to the display, the other one is filled next */
static inline void dgx_vscreen_halves_send(dgx_vscreen_halves_t *h)
{
    h->scr->write_area(h->scr, h->half[h->which], 8u * (uint32_t)(h->at - h->half[h->which]));
    if (h->alone) h->scr->wait_buffer(h->scr);
    h->which ^= 1;
    h->at = h->half[h->which];
}

/* what is left goes, and all of it has gone when this returns */
static inline void dgx_vscreen_halves_end(dgx_vscreen_halves_t *h)
{
    if (h->at != h->half[h->which]) dgx_vscreen_halves_send(h);
    h->scr->wait_buffer(h->scr);
}

/* the window of a display behind a bus set, and its buffer in halves for a rectangle of `bytes` a pixel */
static void dgx_vscreen_halves_window(dgx_vscreen_halves_t *h, dgx_screen_t *dst, int x, int y, int w, int h_rows, uint32_t bytes)
{
    dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)dst)->bus;
    dst->set_area(dst, (uint16_t)x, (uint16_t)(x + w - 1), (uint16_t)y, (uint16_t)(y + h_rows - 1));
    dst->wait_buffer(dst);
    const uint32_t row_bytes = (uint32_t)w * bytes;
    dgx_vscreen_halves_begin(h, dst, bus->buffer, bus->buffer_len, bytes, row_bytes, row_bytes * (uint32_t)h_rows);
}

/*
 * A run of whole pixels put into the halves. A virtual screen keeps its
 * pixels in the byte order of the wire, so this is a plain copy.
 */
static void dgx_vscreen_send_row(dgx_vscreen_halves_t *h, const uint8_t *src, uint32_t bytes)
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

/*
 * A row of 16-bit pixels into the halves, the overlay layer of the screen
 * mixed in on the way: the pixels from x on of the row y of the screen.
 */
static void dgx_vscreen_send_row_over(dgx_vscreen_halves_t *h, const uint8_t *src, uint32_t bytes, const dgx_bus_overlay_t *overlay, int x, int y)
{
    while (bytes) {
        const uint32_t room  = dgx_vscreen_halves_room(h);
        const uint32_t chunk = bytes < room ? bytes : room;
        memcpy(h->at, src, chunk);
        dgx_overlay_row(overlay, h->at, x, y, (int)(chunk / 2u));
        x += (int)(chunk / 2u);
        h->at += chunk;
        src += chunk;
        bytes -= chunk;
        if (chunk == room) dgx_vscreen_halves_send(h);
    }
}

/* ------------------------------------------------------------------------- */
/*  The same depth, as it is or turned                                       */
/* ------------------------------------------------------------------------- */

/*
 * A display behind a bus that has a faster way than its window takes a
 * rectangle of a virtual screen as it lies in memory: whole bytes a pixel,
 * the same depth on both sides, the rectangle inside both screens. Returns
 * false when the pixels have to go through the window.
 */
static bool dgx_vscreen_rows_to_bus(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_vscreen_t *scr_src, int x_src, int y_src, int width, int height)
{
    if (_scr_dst->screen_subtype != DgxPhysicalScreenWithBus || _scr_dst->color_bits % 8u != 0) return false;
    dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)_scr_dst)->bus;
    if (!bus->write_rows) return false;
    const uint32_t bip = dgx_color_points_to_bytes(scr_src->base.color_bits, 1);
    return bus->write_rows(bus, (uint16_t)x_dst, (uint16_t)y_dst, (uint16_t)width, (uint16_t)height,
                           DGX_VSCR_PTR(scr_src, DGX_VSCR_OFFSET(scr_src, x_src, y_src)), (uint32_t)scr_src->base.width * bip);
}

/*
 * n pixels of `bytes` each copied but for those of value 0, which leave the
 * destination as it was: a loop for every size, as a pixel of one is tested
 * and copied whole.
 */
static void dgx_transfer_copy_holes(uint8_t *restrict to, const uint8_t *restrict from, int n, uint8_t bytes)
{
    switch (bytes) {
    case 1:
        for (int i = 0; i < n; ++i)
            if (from[i]) to[i] = from[i];
        return;
    case 2:
        for (int i = 0; i < n; ++i, from += 2, to += 2)
            if (from[0] | from[1]) to[0] = from[0], to[1] = from[1];
        return;
    case 3:
        for (int i = 0; i < n; ++i, from += 3, to += 3)
            if (from[0] | from[1] | from[2]) to[0] = from[0], to[1] = from[1], to[2] = from[2];
        return;
    default:
        for (int i = 0; i < n; ++i, from += 4, to += 4)
            if (from[0] | from[1] | from[2] | from[3]) to[0] = from[0], to[1] = from[1], to[2] = from[2], to[3] = from[3];
        return;
    }
}

/*
 * n pixels of a row put, the pixels from x on of the row y of the
 * destination: those of value 0 left out when transparent, the overlay
 * layer of a 16-bit screen mixed in when there is one.
 */
static void dgx_transfer_row(uint8_t *restrict to, const uint8_t *restrict from, int n, uint8_t bytes, bool transparent,
                             const dgx_bus_overlay_t *over, int x, int y)
{
    if (!over) {
        if (transparent) dgx_transfer_copy_holes(to, from, n, bytes);
        else memcpy(to, from, (size_t)n * bytes);
        return;
    }
    for (int c = 0; c < n; ++c, from += 2, to += 2) {
        const uint16_t color = (uint16_t)(from[0] << 8 | from[1]);
        if (transparent && !color) continue;
        const uint16_t mixed = dgx_overlay_pixel(over, x + c, y, color);
        to[0] = (uint8_t)(mixed >> 8), to[1] = (uint8_t)mixed;
    }
}

/*
 * The same along one row of a screen put onto itself, where its pixels
 * would be read after they are put: they go through a copy a piece at a
 * time, from the end of the row they move towards.
 */
static void dgx_transfer_row_onto_itself(uint8_t *to, const uint8_t *from, int n, uint8_t bytes, bool transparent, const dgx_bus_overlay_t *over,
                                         int x, int y)
{
    uint8_t    copy[256];
    const int  piece = (int)sizeof(copy) / 4; /* pixels of up to four bytes */
    const bool back  = to > from;
    for (int done = 0; done < n;) {
        const int k  = n - done < piece ? n - done : piece;
        const int at = back ? n - done - k : done;
        memcpy(copy, from + (size_t)at * bytes, (size_t)k * bytes);
        dgx_transfer_row(to + (size_t)at * bytes, copy, k, bytes, transparent, over, x + at, y);
        done += k;
    }
}

/*
 * The source as it is, its pixel (u, v) at the corner of the rectangle. A
 * display whose bus takes rows of memory gets them so; one behind a window
 * gets them through the halves; a virtual screen of whole bytes a pixel is
 * copied a row at a time; anything else pixel by pixel. A transparent one
 * goes so to a virtual screen only: false is returned for a display then.
 */
static bool dgx_transfer_as_it_is(dgx_screen_t *dst, const dgx_transfer_place_t *p, dgx_screen_t *src, const dgx_bus_overlay_t *over, bool transparent)
{
    dgx_vscreen_t *scr_src = (dgx_vscreen_t *)src;
    const int      sx = p->u, sy = p->v;
    const uint8_t  bytes = dgx_vscreen_pixel_bytes(src->color_bits);
    const bool     rows = bytes && dgx_vscreen_is_linear(src);
    /* a transparent picture goes to a display behind a window by its runs, the general way; to a display that is memory, pixel by pixel below */
    if (transparent && dst->screen_subtype == DgxPhysicalScreenWithBus && !dgx_screen_is_readable(dst)) return false;
    if (!transparent && !over && rows && dgx_vscreen_rows_to_bus(dst, p->x, p->y, scr_src, sx, sy, p->w, p->h)) {
        /* the bus has taken the pixels as they lie in memory */
        return true;
    }
    if (rows && !transparent && dgx_transfer_bus_takes(dst, bytes)) {
        dgx_vscreen_halves_t halves;
        dgx_vscreen_halves_window(&halves, dst, p->x, p->y, p->w, p->h, bytes);
        for (int r = 0; r < p->h; ++r) {
            const uint8_t *from = DGX_VSCR_PTR(scr_src, DGX_VSCR_OFFSET(scr_src, sx, sy + r));
            if (over) dgx_vscreen_send_row_over(&halves, from, (uint32_t)p->w * bytes, over, p->x, p->y + r);
            else dgx_vscreen_send_row(&halves, from, (uint32_t)p->w * bytes);
        }
        dgx_vscreen_halves_end(&halves);
        return true;
    }
    if (rows && dgx_vscreen_is_linear(dst)) {
        dgx_vscreen_t *scr_dst = (dgx_vscreen_t *)dst;
        /*
         * A screen put onto itself further down goes from its last row up, so
         * that no row is read after it is put; put onto the same rows, every
         * row moves along itself.
         */
        const bool      up = dst == src && p->y > sy, along = dst == src && p->y == sy;
        const int       first = up ? p->h - 1 : 0, dy = up ? -1 : 1;
        const ptrdiff_t from_step = dy * (ptrdiff_t)src->width * bytes, to_step = dy * (ptrdiff_t)dst->width * bytes;
        const uint8_t  *from_row = DGX_VSCR_PTR(scr_src, DGX_VSCR_OFFSET(scr_src, sx, sy + first));
        uint8_t        *to_row   = DGX_VSCR_PTR(scr_dst, DGX_VSCR_OFFSET(scr_dst, p->x, p->y + first));
        for (int k = 0, y = p->y + first; k < p->h; ++k, y += dy, from_row += from_step, to_row += to_step) {
            if (!transparent && !over) {
                /* memmove() of the ESP32 goes a byte at a time, memcpy() by words */
                if (along) memmove(to_row, from_row, (size_t)p->w * bytes);
                else memcpy(to_row, from_row, (size_t)p->w * bytes);
            } else if (along) {
                dgx_transfer_row_onto_itself(to_row, from_row, p->w, bytes, transparent, over, p->x, y);
            } else {
                dgx_transfer_row(to_row, from_row, p->w, bytes, transparent, over, p->x, y);
            }
        }
        return true;
    }
    /*
     * Pixel by pixel, the source read in place where it is memory. The place
     * is copied out first: after every call through the screen the compiler
     * would read it again.
     */
    const int     x = p->x, y = p->y, w = p->w, h = p->h;
    const uint8_t bits = src->color_bits;
    if (dst == src) {
        /*
         * A screen put onto itself, read through its calls as it is not rows
         * of whole bytes: from the side the rectangle moves to, its rows from
         * the bottom when it moves down and a row from the right when it moves
         * right, so that no pixel is read after it is put.
         */
        const bool up = y > sy, back = y == sy && x > sx;
        for (int k = 0; k < h; ++k) {
            const int r = up ? h - 1 - k : k;
            for (int j = 0; j < w; ++j) {
                const int c     = back ? w - 1 - j : j;
                uint32_t  color = src->get_pixel(src, sx + c, sy + r);
                if (transparent && !color) continue;
                if (over) color = dgx_overlay_pixel(over, x + c, y + r, (uint16_t)color);
                dst->set_pixel(dst, x + c, y + r, color);
            }
        }
        return true;
    }
    for (int r = 0; r < h; ++r) {
        uint8_t *from = rows ? DGX_VSCR_PTR(scr_src, DGX_VSCR_OFFSET(scr_src, sx, sy + r)) : NULL;
        for (int c = 0; c < w; ++c) {
            uint32_t color = from ? dgx_read_buf_value(bits, &from, c) : src->get_pixel(src, sx + c, sy + r);
            if (transparent && !color) continue;
            if (over) color = dgx_overlay_pixel(over, x + c, y + r, (uint16_t)color);
            dst->set_pixel(dst, x + c, y + r, color);
        }
    }
    return true;
}

/*
 * A turned source for a display behind a bus, not scaled and not
 * transparent. A pixel set alone costs such a display a window and a
 * transfer of its own, so the rectangle goes through one window: the rows of
 * the place it takes on the display, each gathered from the source in the
 * order the turn asks for and sent through the buffer of the bus. Along a
 * row of the display the source is walked by a step of its own, a pixel or a
 * row of it, forth or back. Returns false when the source is not rows of
 * whole bytes in memory, or the bus has no buffer for a pixel.
 */
static bool dgx_transfer_turned_to_bus(dgx_screen_t *dst, const dgx_transfer_place_t *p, dgx_screen_t *src, const dgx_bus_overlay_t *over)
{
    if (dst->screen_subtype != DgxPhysicalScreenWithBus || !dgx_vscreen_is_linear(src)) return false;
    const size_t bip = dgx_vscreen_pixel_bytes(src->color_bits);
    if (!bip) return false;
    const dgx_vscreen_t *scr_src = (const dgx_vscreen_t *)src;
    dgx_bus_protocols_t *bus     = ((dgx_screen_with_bus_t *)dst)->bus;
    const size_t         pitch   = (size_t)src->width * bip;
    if (!bus->buffer || bus->buffer_len < bip || !dst->set_area || !dst->write_area || !dst->wait_buffer) return false;
    const bool swapped = p->turn & 4u, x_back = p->turn & 1u, y_back = p->turn & 2u;
    dst->set_area(dst, (uint16_t)p->x, (uint16_t)(p->x + p->w - 1), (uint16_t)p->y, (uint16_t)(p->y + p->h - 1));
    dst->wait_buffer(dst);
    const uint32_t       row_bytes = (uint32_t)p->w * bip;
    dgx_vscreen_halves_t halves;
    dgx_vscreen_halves_begin(&halves, dst, bus->buffer, bus->buffer_len - bus->buffer_len % bip, (uint32_t)bip, row_bytes, row_bytes * (uint32_t)p->h);
    for (int r = 0; r < p->h; ++r) {
        /* the pixel of the source the first pixel of this row of the display shows */
        const int32_t u = p->u, v = p->v + r;
        int32_t       sx, sy;
        ptrdiff_t     step;
        if (!swapped) {
            sx   = x_back ? p->wide - 1 - u : u;
            sy   = y_back ? p->high - 1 - v : v;
            step = x_back ? -(ptrdiff_t)bip : (ptrdiff_t)bip;
        } else {
            /* a row of the display runs down a column of the source */
            sx   = x_back ? p->wide - 1 - v : v;
            sy   = y_back ? p->high - 1 - u : u;
            step = y_back ? -(ptrdiff_t)pitch : (ptrdiff_t)pitch;
        }
        const uint8_t *from = scr_src->v_array + (size_t)sy * pitch + (size_t)sx * bip;
        for (int count = p->w, x = p->x; count;) {
            /* as many pixels as the half has room for, then the half is sent if it is full */
            const int room = (int)(dgx_vscreen_halves_room(&halves) / bip);
            const int n    = count < room ? count : room;
            uint8_t  *to   = halves.at;
            /* each size of a pixel its own loop: a loop over the bytes of every pixel costs as much again */
            switch (bip) {
            case 1:
                for (int i = 0; i < n; ++i, from += step) *to++ = from[0];
                break;
            case 2:
                for (int i = 0; i < n; ++i, from += step, to += 2) to[0] = from[0], to[1] = from[1];
                break;
            case 3:
                for (int i = 0; i < n; ++i, from += step, to += 3) to[0] = from[0], to[1] = from[1], to[2] = from[2];
                break;
            default:
                for (int i = 0; i < n; ++i, from += step, to += 4) to[0] = from[0], to[1] = from[1], to[2] = from[2], to[3] = from[3];
                break;
            }
            if (over) dgx_overlay_row(over, halves.at, x, p->y + r, n);
            halves.at = to;
            count -= n;
            x += n;
            if (n == room) dgx_vscreen_halves_send(&halves);
        }
    }
    dgx_vscreen_halves_end(&halves);
    return true;
}

/* ------------------------------------------------------------------------- */
/*  8 bits through a table, as it is                                         */
/* ------------------------------------------------------------------------- */

/*
 * Is the table two colors and what lies between them? It is when every color of it is not further
 * than a step of red, of green and of blue from the straight way between its first color and its
 * last: a table a program has made for a pen with a soft edge, from the paper to the ink, however it
 * rounded. Such a table a bus with hardware that mixes two colors does not need: the first color, the
 * last one and the bytes of the screen are enough for it.
 */
static bool dgx_vscreen8_two_colors(const uint16_t *lut)
{
    static const uint8_t shift[3] = {11, 5, 0}, mask[3] = {31, 63, 31};
    for (int c = 0; c < 3; ++c) {
        const int from = (lut[0] >> shift[c]) & mask[c], way = ((lut[255] >> shift[c]) & mask[c]) - from;
        for (int a = 1; a < 255; ++a) {
            const int off = 255 * (((lut[a] >> shift[c]) & mask[c]) - from) - way * a;
            if (off > 255 || off < -255) return false;
        }
    }
    return true;
}

/* n indices made into colors of two bytes each, the high one first, as a display takes them */
static inline void dgx_vscreen8_lut_run(uint8_t *restrict to, const uint8_t *restrict ink, int n, const uint16_t *restrict lut)
{
    for (int i = 0; i < n; ++i) {
        const uint16_t color = lut[ink[i]];
        to[2 * i]            = (uint8_t)(color >> 8);
        to[2 * i + 1]        = (uint8_t)color;
    }
}

/*
 * n pixels of a row of indices made `scale` times as wide, from the pixel
 * `at` of the widened row on, into colors as dgx_vscreen8_lut_run() makes
 * them.
 */
static void dgx_vscreen8_lut_scaled(uint8_t *restrict to, const uint8_t *restrict ink, uint32_t at, int n, uint32_t scale, const uint16_t *restrict lut)
{
    if (scale == 1) {
        dgx_vscreen8_lut_run(to, ink + at, n, lut);
        return;
    }
    const uint8_t *index = ink + at / scale;
    uint32_t       run   = scale - at % scale;
    while (n > 0) {
        const int      k     = (int)run < n ? (int)run : n;
        const uint16_t color = lut[*index];
        for (int i = 0; i < k; ++i) {
            to[2 * i]     = (uint8_t)(color >> 8);
            to[2 * i + 1] = (uint8_t)color;
        }
        to += 2 * k;
        n -= k;
        ++index;
        run = scale;
    }
}

/*
 * An upright picture of indices in squares, not smoothed and not
 * transparent, to a display behind a bus: the colors of the next half are
 * made while the one before it goes to the display.
 */
static void dgx_transfer8_upright_to_bus(dgx_screen_t *dst, const dgx_transfer_place_t *p, const dgx_bus_indexed_t *picture)
{
    const uint32_t       scale = p->scale, cut_x = (uint32_t)p->u % scale, cut_y = (uint32_t)p->v % scale;
    const uint8_t       *first = picture->pixels + (size_t)((uint32_t)p->v / scale) * picture->pitch + (uint32_t)p->u / scale;
    const uint16_t      *lut   = picture->lut;
    const uint32_t       row_bytes = 2u * (uint32_t)p->w;
    dgx_vscreen_halves_t halves;
    dgx_vscreen_halves_window(&halves, dst, p->x, p->y, p->w, p->h, 2u);
    const bool whole_rows = halves.len % row_bytes == 0;
    for (int br = 0; br < p->h; ++br) {
        const uint8_t *ink = first + ((uint32_t)br + cut_y) / scale * picture->pitch;
        if (whole_rows) {
            /* a row is never cut: the half is looked at after it */
            dgx_vscreen8_lut_scaled(halves.at, ink, cut_x, p->w, scale, lut);
            if (picture->overlay) dgx_overlay_row(picture->overlay, halves.at, p->x, p->y + br, p->w);
            halves.at += row_bytes;
            if (!dgx_vscreen_halves_room(&halves)) dgx_vscreen_halves_send(&halves);
            continue;
        }
        for (int bc = 0; bc < p->w;) {
            /* as much of the row as the half has room for, then the half is sent if it is full */
            const int room = (int)(dgx_vscreen_halves_room(&halves) / 2u);
            const int n    = p->w - bc < room ? p->w - bc : room;
            dgx_vscreen8_lut_scaled(halves.at, ink, cut_x + (uint32_t)bc, n, scale, lut);
            if (picture->overlay) dgx_overlay_row(picture->overlay, halves.at, p->x + bc, p->y + br, n);
            halves.at += 2 * n;
            bc += n;
            if (n == room) dgx_vscreen_halves_send(&halves);
        }
    }
    dgx_vscreen_halves_end(&halves);
}

/*
 * An upright picture of indices in squares, not smoothed, into a virtual
 * screen of 16 bits a pixel: every pixel looked up where it lies, as a
 * point of a dot font is stamped many times a text, with nothing to set up.
 */
static void dgx_transfer8_upright_to_memory(dgx_screen_t *dst, const dgx_transfer_place_t *p, const dgx_bus_indexed_t *picture, bool transparent)
{
    dgx_vscreen_t  *scr_dst = (dgx_vscreen_t *)dst;
    const uint32_t  scale = p->scale, cut_x = (uint32_t)p->u % scale, cut_y = (uint32_t)p->v % scale;
    const uint8_t  *first = picture->pixels + (size_t)((uint32_t)p->v / scale) * picture->pitch + (uint32_t)p->u / scale;
    const uint16_t *lut   = picture->lut;
    for (int br = 0; br < p->h; ++br) {
        const uint8_t *ink = first + ((uint32_t)br + cut_y) / scale * picture->pitch;
        uint8_t       *to  = DGX_VSCR_PTR(scr_dst, DGX_VSCR_OFFSET(scr_dst, p->x, p->y + br));
        if (picture->overlay) {
            for (int bc = 0; bc < p->w; ++bc, to += 2) {
                const uint8_t index = ink[((uint32_t)bc + cut_x) / scale];
                if (transparent && !index) continue;
                const uint16_t color = dgx_overlay_pixel(picture->overlay, p->x + bc, p->y + br, lut[index]);
                to[0] = (uint8_t)(color >> 8), to[1] = (uint8_t)color;
            }
            continue;
        }
        if (scale == 1) {
            for (int bc = 0; bc < p->w; ++bc, to += 2) {
                const uint8_t index = ink[bc];
                if (transparent && !index) continue;
                const uint16_t color = lut[index];
                to[0] = (uint8_t)(color >> 8), to[1] = (uint8_t)color;
            }
            continue;
        }
        /* the squares by runs of their pixels along the row */
        uint32_t run = scale - cut_x;
        for (int bc = 0; bc < p->w; ++ink, run = scale) {
            const uint8_t  index = *ink;
            const uint16_t color = lut[index];
            for (; run && bc < p->w; --run, ++bc, to += 2) {
                if (transparent && !index) continue;
                to[0] = (uint8_t)(color >> 8), to[1] = (uint8_t)color;
            }
        }
    }
}

/* the same pixel by pixel through the screen, the pixels of index 0 left out: for a display whose pixels are memory */
static void dgx_transfer8_upright_pixels(dgx_screen_t *dst, const dgx_transfer_place_t *p, const dgx_bus_indexed_t *picture)
{
    /* copied out: after every call through the screen the compiler would read them again */
    const int                x = p->x, y = p->y, w = p->w, h = p->h;
    const uint32_t           scale = p->scale, cut_x = (uint32_t)p->u % scale, cut_y = (uint32_t)p->v % scale, pitch = picture->pitch;
    const uint16_t          *lut   = picture->lut;
    const dgx_bus_overlay_t *over  = picture->overlay;
    const uint8_t           *first = picture->pixels + (size_t)((uint32_t)p->v / scale) * pitch + (uint32_t)p->u / scale;
    if (scale == 1 && !over) {
        /* the usual sprite: an index a pixel, nothing to mix in */
        for (int br = 0; br < h; ++br, first += pitch) {
            for (int bc = 0; bc < w; ++bc) {
                if (first[bc]) dst->set_pixel(dst, x + bc, y + br, lut[first[bc]]);
            }
        }
        return;
    }
    for (int br = 0; br < h; ++br) {
        const uint8_t *ink = first + ((uint32_t)br + cut_y) / scale * pitch;
        for (int bc = 0; bc < w; ++bc) {
            const uint8_t index = ink[scale == 1 ? (uint32_t)bc : ((uint32_t)bc + cut_x) / scale];
            if (!index) continue;
            const uint16_t color = lut[index];
            dst->set_pixel(dst, x + bc, y + br, over ? dgx_overlay_pixel(over, x + bc, y + br, color) : color);
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Any of it: a line of the larger picture for every row                    */
/* ------------------------------------------------------------------------- */

/*
 * The general way. A row of the destination is a line of the larger
 * picture, a row of it or a column of it, read forward or back; the next
 * row is the line next to it. The lines are made by the lines of scale.c,
 * of indices put through the table or of values put as they are, and a row
 * goes to the destination as it can take it: through the halves of a bus,
 * into the memory of a virtual screen, or pixel by pixel. A transparent row
 * goes to a display behind a window by its runs of pixels that are not 0, a
 * window each.
 */
typedef struct {
    dgx_scale_lines_t lines;
    uint8_t          *scratch;
    const uint16_t   *lut;    /* lines of indices through it; NULL: lines of values */
    int32_t           at;     /* the line the first row of the rectangle is */
    int               across; /* the line of the next row: at + across */
    int               dir;    /* a row runs along its line forward (1) or back (-1) */
    uint32_t          count;  /* the points of a line: the width of the rectangle */
    uint8_t           bytes;  /* of a pixel as the destination takes it */
} dgx_transfer_rows_t;

static bool dgx_transfer_rows_begin(dgx_transfer_rows_t *t, const dgx_transfer_place_t *p, const dgx_bus_indexed_t *picture,
                                    const dgx_scale_source_t *source, uint8_t smooth, uint8_t bytes)
{
    const bool swapped = p->turn & 4u, x_back = p->turn & 1u, y_back = p->turn & 2u;
    /* along a row of the destination the turned picture runs along u: a row of the larger one, or a column of it when swapped */
    const bool    back  = swapped ? y_back : x_back;
    const int32_t along = swapped ? p->high : p->wide, first = back ? along - 1 - p->u : p->u;
    const int32_t from  = back ? first - (p->w - 1) : first;
    /* across the rows v runs: the rows of the larger picture, or its columns */
    const bool across_back = swapped ? x_back : y_back;
    t->at                  = across_back ? (swapped ? p->wide : p->high) - 1 - p->v : p->v;
    t->across              = across_back ? -1 : 1;
    t->dir                 = back ? -1 : 1;
    t->count               = (uint32_t)p->w;
    t->bytes               = bytes;
    t->lut                 = picture ? picture->lut : NULL;
    t->scratch = malloc(dgx_scale_scratch(p->scale, smooth, t->count, picture ? 1u : 4u));
    if (!t->scratch) {
        ESP_LOGE(TAG, "no memory for the lines of a transfer %d wide", p->w);
        return false;
    }
    if (picture) dgx_scale_lines_begin(&t->lines, picture, swapped, from, t->count, t->scratch);
    else dgx_scale_values_begin(&t->lines, source, p->scale, smooth, swapped, from, t->count, t->scratch);
    return true;
}

/* the line of the row r of the rectangle */
static inline const void *dgx_transfer_line(dgx_transfer_rows_t *t, int r)
{
    const int32_t at = t->at + r * t->across;
    return t->lut ? (const void *)dgx_scale_line(&t->lines, at) : (const void *)dgx_scale_values(&t->lines, at);
}

/* the pixel i of a row: its index or its value, 0 for a pixel a transparent transfer leaves out */
static inline uint32_t dgx_transfer_key(const dgx_transfer_rows_t *t, const void *line, uint32_t i)
{
    const uint32_t point = t->dir > 0 ? i : t->count - 1u - i;
    return t->lut ? ((const uint8_t *)line)[point] : ((const uint32_t *)line)[point];
}

/* the color or the value of the pixel i */
static inline uint32_t dgx_transfer_color(const dgx_transfer_rows_t *t, const void *line, uint32_t i)
{
    const uint32_t key = dgx_transfer_key(t, line, i);
    return t->lut ? t->lut[key] : key;
}

/* n pixels of a row from its pixel i on, the high byte of each first, as a display takes them and a virtual screen keeps them */
static void dgx_transfer_pixels(const dgx_transfer_rows_t *t, const void *line, uint32_t i, uint32_t n, uint8_t *to)
{
    if (t->lut && t->dir > 0) {
        dgx_vscreen8_lut_run(to, (const uint8_t *)line + i, (int)n, t->lut);
        return;
    }
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t color = dgx_transfer_color(t, line, i + k);
        dgx_vscreen_put_value(to, t->bytes, color);
        to += t->bytes;
    }
}

/* the pixels i .. i + n - 1 of the row r through the window of a display, the window being set already */
static void dgx_transfer_send(dgx_transfer_rows_t *t, dgx_vscreen_halves_t *halves, const void *line, uint32_t i, uint32_t n, int x, int y,
                              const dgx_bus_overlay_t *over)
{
    while (n) {
        const uint32_t room  = dgx_vscreen_halves_room(halves) / t->bytes;
        const uint32_t chunk = n < room ? n : room;
        dgx_transfer_pixels(t, line, i, chunk, halves->at);
        if (over) dgx_overlay_row(over, halves->at, x, y, (int)chunk);
        halves->at += chunk * t->bytes;
        i += chunk, x += (int)chunk, n -= chunk;
        if (chunk == room) dgx_vscreen_halves_send(halves);
    }
}

static void dgx_transfer_rows(dgx_screen_t *dst, const dgx_transfer_place_t *p, dgx_transfer_rows_t *t, bool transparent,
                              const dgx_bus_overlay_t *over)
{
    /*
     * A transparent picture goes to a display behind a window by its runs, a
     * window each, which costs less than a window a pixel; to a display whose
     * pixels are memory at hand, a DSI one, a pixel at a time, which costs
     * less than a window.
     */
    if (dgx_transfer_bus_takes(dst, t->bytes) && !(transparent && dgx_screen_is_readable(dst))) {
        if (!transparent) {
            dgx_vscreen_halves_t halves;
            dgx_vscreen_halves_window(&halves, dst, p->x, p->y, p->w, p->h, t->bytes);
            for (int r = 0; r < p->h; ++r) dgx_transfer_send(t, &halves, dgx_transfer_line(t, r), 0, t->count, p->x, p->y + r, over);
            dgx_vscreen_halves_end(&halves);
            return;
        }
        for (int r = 0; r < p->h; ++r) {
            const void *line = dgx_transfer_line(t, r);
            for (uint32_t i = 0; i < t->count;) {
                /* a run of pixels that are put, a window of its own */
                while (i < t->count && !dgx_transfer_key(t, line, i)) ++i;
                uint32_t end = i;
                while (end < t->count && dgx_transfer_key(t, line, end)) ++end;
                if (end == i) break;
                dgx_vscreen_halves_t halves;
                dgx_vscreen_halves_window(&halves, dst, p->x + (int)i, p->y + r, (int)(end - i), 1, t->bytes);
                dgx_transfer_send(t, &halves, line, i, end - i, p->x + (int)i, p->y + r, over);
                dgx_vscreen_halves_end(&halves);
                i = end;
            }
        }
        return;
    }
    if (t->bytes && dgx_vscreen_is_linear(dst) && dgx_vscreen_pixel_bytes(dst->color_bits) == t->bytes) {
        dgx_vscreen_t *scr_dst = (dgx_vscreen_t *)dst;
        for (int r = 0; r < p->h; ++r) {
            const void *line = dgx_transfer_line(t, r);
            uint8_t    *to   = DGX_VSCR_PTR(scr_dst, DGX_VSCR_OFFSET(scr_dst, p->x, p->y + r));
            if (!transparent && !over) {
                dgx_transfer_pixels(t, line, 0, t->count, to);
                continue;
            }
            for (uint32_t i = 0; i < t->count; ++i, to += t->bytes) {
                if (transparent && !dgx_transfer_key(t, line, i)) continue;
                uint32_t color = dgx_transfer_color(t, line, i);
                if (over) color = dgx_overlay_pixel(over, p->x + (int)i, p->y + r, (uint16_t)color);
                dgx_vscreen_put_value(to, t->bytes, color);
            }
        }
        return;
    }
    /* pixel by pixel; what a pixel is read with is copied out, as after every call through the screen the compiler would read it again */
    const int       x = p->x, y = p->y, h = p->h;
    const uint32_t  count   = t->count;
    const bool      forward = t->dir > 0;
    const uint16_t *lut     = t->lut;
    for (int r = 0; r < h; ++r) {
        const void *line = dgx_transfer_line(t, r);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t point = forward ? i : count - 1u - i;
            const uint32_t key   = lut ? ((const uint8_t *)line)[point] : ((const uint32_t *)line)[point];
            if (transparent && !key) continue;
            uint32_t color = lut ? lut[key] : key;
            if (over) color = dgx_overlay_pixel(over, x + (int)i, y + r, (uint16_t)color);
            dst->set_pixel(dst, x + (int)i, y + r, color);
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  The calls                                                                */
/* ------------------------------------------------------------------------- */

/*
 * A screen put onto itself made larger or turned would read rows it has
 * already put. The part of it the place shows, with the pixels around it
 * that smoothing looks at, is put aside into a screen of its own first, and
 * the transfer is made from there. Returns false when the place does not
 * cover any of that, and nothing needs to be put aside.
 */
static bool dgx_transfer_aside(dgx_screen_t *scr, int x_dst, int y_dst, int x_src, int y_src, int width, int height, uint32_t flags,
                               const dgx_transfer_asked_t *asked, const dgx_transfer_place_t *p)
{
    /* the place along the columns and the rows of the larger picture, flipped back as the turn flipped them */
    const bool swapped = p->turn & 4u;
    int32_t    cl = swapped ? p->v : p->u, ch = cl + (swapped ? p->h : p->w);
    int32_t    rl = swapped ? p->u : p->v, rh = rl + (swapped ? p->w : p->h);
    if (p->turn & 1u) {
        const int32_t f = cl;
        cl = p->wide - ch, ch = p->wide - f;
    }
    if (p->turn & 2u) {
        const int32_t f = rl;
        rl = p->high - rh, rh = p->high - f;
    }
    /* in pixels of the source, inside it */
    const int32_t n = p->scale, reach = asked->smooth ? 2 : 0;
    const int     x0 = cl / n - reach > 0 ? (int)(cl / n - reach) : 0, y0 = rl / n - reach > 0 ? (int)(rl / n - reach) : 0;
    const int     x1 = (ch + n - 1) / n + reach < scr->width ? (int)((ch + n - 1) / n + reach) : scr->width;
    const int     y1 = (rh + n - 1) / n + reach < scr->height ? (int)((rh + n - 1) / n + reach) : scr->height;
    if (x1 <= p->x || p->x + p->w <= x0 || y1 <= p->y || p->y + p->h <= y0) return false;
    dgx_screen_t *aside = dgx_vscreen_init(x1 - x0, y1 - y0, scr->color_bits, scr->rgb_order);
    if (!aside) {
        ESP_LOGE(TAG, "no memory to put aside %dx%d pixels of a screen put onto itself", x1 - x0, y1 - y0);
        return true;
    }
    dgx_vscreen_region_to_screen(aside, 0, 0, scr, x0, y0, x1 - x0, y1 - y0, 0u);
    /* the region where it now lies: every pixel of it lands where it would have, and what is not aside the place did not show */
    dgx_vscreen_region_to_screen(scr, x_dst, y_dst, aside, x_src - x0, y_src - y0, width, height, flags);
    dgx_screen_destroy(&aside);
    return true;
}

void dgx_vscreen_region_to_screen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width, int height,
                                  uint32_t flags)
{
    if (_scr_dst->color_bits != _scr_src->color_bits) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: source color bits %d != destination %d", _scr_src->color_bits, _scr_dst->color_bits);
        return;
    }
    if (!dgx_transfer_virtual(_scr_src)) {
        ESP_LOGE(TAG, "dgx_vscreen_to_screen: source is not virtual");
        return;
    }
    const dgx_transfer_asked_t asked = dgx_transfer_asked(flags);
    dgx_transfer_place_t       p;
    if (!dgx_transfer_place(_scr_dst, _scr_src, x_dst, y_dst, x_src, y_src, width, height, &asked, &p)) return;
    if (_scr_dst == _scr_src && (p.scale > 1 || p.turn) && dgx_transfer_aside(_scr_dst, x_dst, y_dst, x_src, y_src, width, height, flags, &asked, &p))
        return;
    /* the overlay layer of a 16-bit screen over the rectangle is mixed in on the way */
    const dgx_bus_overlay_t *over = _scr_dst->color_bits == 16 ? dgx_overlay_over(_scr_dst, p.x, p.y, p.x + p.w - 1, p.y + p.h - 1) : NULL;
    dgx_screen_progress_up(_scr_dst);
    if (p.scale == 1 && !p.turn && dgx_transfer_as_it_is(_scr_dst, &p, _scr_src, over, asked.transparent)) {
        /* as it lies in memory */
    } else if (p.scale == 1 && !asked.transparent && dgx_transfer_turned_to_bus(_scr_dst, &p, _scr_src, over)) {
        /* turned through one window */
    } else {
        /* the values of the pixels, read from memory where the source is rows of whole bytes */
        dgx_scale_source_t source = {.screen = _scr_src};
        const uint8_t      bytes  = dgx_vscreen_pixel_bytes(_scr_src->color_bits);
        if (bytes && dgx_vscreen_is_linear(_scr_src)) {
            source.memory = ((dgx_vscreen_t *)_scr_src)->v_array;
            source.bytes  = bytes;
            source.pitch  = (uint32_t)_scr_src->width * bytes;
        }
        dgx_transfer_rows_t t;
        if (dgx_transfer_rows_begin(&t, &p, NULL, &source, asked.smooth, bytes)) {
            dgx_transfer_rows(_scr_dst, &p, &t, asked.transparent, over);
            free(t.scratch);
        }
    }
    dgx_screen_touch(_scr_dst, p.x, p.x + p.w - 1, p.y, p.y + p.h - 1);
    dgx_screen_progress_down(_scr_dst);
}

void dgx_vscreen_to_screen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, uint32_t flags)
{
    dgx_vscreen_region_to_screen(_scr_dst, x_dst, y_dst, _scr_src, 0, 0, _scr_src->width, _scr_src->height, flags);
}

void dgx_vscreen8_region_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width,
                                     int height, const uint16_t *lut, uint32_t flags)
{
    if (_scr_dst->color_bits != 16 || _scr_src->color_bits != 8) {
        ESP_LOGE(TAG, "vscreen8_to_screen16: source color bits %d != 8 or destination %d != 16", _scr_src->color_bits, _scr_dst->color_bits);
        return;
    }
    if (!dgx_transfer_virtual(_scr_src) || !dgx_vscreen_is_linear(_scr_src)) {
        ESP_LOGE(TAG, "vscreen8_to_screen16: source is not a virtual screen of rows of bytes");
        return;
    }
    const dgx_transfer_asked_t asked = dgx_transfer_asked(flags);
    dgx_transfer_place_t       p;
    if (!dgx_transfer_place(_scr_dst, _scr_src, x_dst, y_dst, x_src, y_src, width, height, &asked, &p)) return;
    dgx_vscreen_t *scr_src = (dgx_vscreen_t *)_scr_src;
    /* the whole source, and where the rectangle starts in it made larger and turned */
    const dgx_bus_indexed_t picture = {
        .pixels      = scr_src->v_array,
        .pitch       = (uint32_t)_scr_src->width,
        .width       = (uint32_t)_scr_src->width,
        .height      = (uint32_t)_scr_src->height,
        .x           = (uint32_t)p.u,
        .y           = (uint32_t)p.v,
        .scale       = p.scale,
        .smooth      = asked.smooth,
        .orientation = p.turn,
        .lut         = lut,
        /* the overlay layer of the screen, when it is over the rectangle */
        .overlay = dgx_overlay_over(_scr_dst, p.x, p.y, p.x + p.w - 1, p.y + p.h - 1),
    };
    const bool upright_squares = !p.turn && !asked.smooth;
    dgx_screen_progress_up(_scr_dst);
    bool done = false;
    if (_scr_dst->screen_subtype == DgxPhysicalScreenWithBus && !asked.transparent) {
        dgx_bus_protocols_t *bus = ((dgx_screen_with_bus_t *)_scr_dst)->bus;
        if (upright_squares && p.scale == 1 && !picture.overlay && !asked.exact && bus->write_ink && dgx_vscreen8_two_colors(lut)) {
            /* the bus mixes the two colors of the table itself */
            const uint8_t *first = picture.pixels + (size_t)p.v * picture.pitch + (size_t)p.u;
            done = bus->write_ink(bus, (uint16_t)p.x, (uint16_t)p.y, (uint16_t)p.w, (uint16_t)p.h, first, picture.pitch, lut[0], lut[255]);
        }
        /* the bus makes the colors itself and puts them its own faster way */
        if (!done && bus->write_indexed) done = bus->write_indexed(bus, (uint16_t)p.x, (uint16_t)p.y, (uint16_t)p.w, (uint16_t)p.h, &picture);
        /* through the halves of the buffer of the bus; a bus without one is given the pixels one at a time below */
        if (!done && upright_squares && dgx_transfer_bus_takes(_scr_dst, 2)) {
            dgx_transfer8_upright_to_bus(_scr_dst, &p, &picture);
            done = true;
        }
    } else if (upright_squares && dgx_vscreen_is_linear(_scr_dst)) {
        dgx_transfer8_upright_to_memory(_scr_dst, &p, &picture, asked.transparent);
        done = true;
    }
    if (!done && upright_squares && asked.transparent && dgx_screen_is_readable(_scr_dst)) {
        /* holes onto a display that is memory: a pixel costs less there than a window for a run */
        dgx_transfer8_upright_pixels(_scr_dst, &p, &picture);
        done = true;
    }
    if (!done) {
        dgx_transfer_rows_t t;
        if (dgx_transfer_rows_begin(&t, &p, &picture, NULL, asked.smooth, 2u)) {
            dgx_transfer_rows(_scr_dst, &p, &t, asked.transparent, picture.overlay);
            free(t.scratch);
        }
    }
    dgx_screen_touch(_scr_dst, p.x, p.x + p.w - 1, p.y, p.y + p.h - 1);
    dgx_screen_progress_down(_scr_dst);
}

void dgx_vscreen8_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, const uint16_t *lut, uint32_t flags)
{
    dgx_vscreen8_region_to_screen16(_scr_dst, x_dst, y_dst, _scr_src, 0, 0, _scr_src->width, _scr_src->height, lut, flags);
}
