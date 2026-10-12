#pragma once
/*
 * dgx_fb_tiles_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Two frame buffers that change places: the display reads one, drawing
 *  goes into the other, then the other way round. The one drawing goes into
 *  lacks what was drawn into the other one last time. To give it only that,
 *  the panel is cut into tiles and a bit is kept for a tile: one map of the
 *  tiles drawn in now, one of the tiles this frame buffer still lacks. A
 *  tile it lacks is copied when something is first drawn in it or read from
 *  it, or when the picture is finished and may be shown. Of a tile that is
 *  drawn in by a rectangle written whole only what lies outside the
 *  rectangle is copied: nothing, when the rectangle covers it.
 *
 *  A finished picture is not shown at once, and more may be drawn into it
 *  before the frame buffers change places. So the two are told apart: the
 *  picture is finished by dgx_fb_tiles_complete(), any number of times,
 *  and the frame buffers have changed places at dgx_fb_tiles_changed().
 *
 *  A large piece a finished picture lacks need not be copied at once. Its
 *  owner may leave it for later: the display reads a frame buffer row after
 *  row, so a piece is in time when it is there before the display comes to
 *  its rows, and more may be drawn over it meanwhile, which saves the copy.
 *  Such a piece stays lacked in the maps until the frame buffers change
 *  places; from then on it is the owner's.
 *
 *  A window pixels are sent into may be left before it is full. So its
 *  tiles stay lacked when it is opened, until it is known how far the
 *  pixels went. Then what they did not reach is taken from the other frame
 *  buffer, pixel for pixel, and the tiles are lacked no more.
 *
 *  Nothing here depends on the platform: a bus keeps one and tells it which
 *  frame buffer is which.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief A way to copy a rectangle from one frame buffer into the other that may beat the processor.
 *
 * It is offered what a frame buffer still lacks when its picture is
 * finished, tiles side by side and one under another as one rectangle.
 * Seldom is anything drawn there after it, so it may leave the cache of
 * the chip as it likes.
 *
 * @param left,top   The first column and row of the rectangle on the panel.
 * @param width,rows Its size in pixels.
 * @return false when it has not made the copy: the processor makes it then.
 */
typedef bool (*dgx_fb_tiles_copier_t)(void *context, uint8_t *to, const uint8_t *from, uint32_t left, uint32_t top, uint32_t width, uint32_t rows);

/**
 * @brief A way to leave a rectangle a finished picture lacks for later.
 *
 * It is asked of every rectangle dgx_fb_tiles_complete() would copy, tiles
 * side by side and one under another. It tells how many rows from the top
 * are to be copied at once: all of them, or a number of whole tiles, 0
 * among them. What is left the owner copies itself later, and it has to
 * tell the maps of it by dgx_fb_tiles_lack() as soon as
 * dgx_fb_tiles_complete() has returned.
 *
 * @param left,top   The first column and row of the rectangle on the panel.
 * @param width,rows Its size in pixels.
 * @return Rows from the top to copy at once.
 */
typedef uint32_t (*dgx_fb_tiles_later_t)(void *context, uint32_t left, uint32_t top, uint32_t width, uint32_t rows);

typedef struct _dgx_fb_tiles_t {
    uint32_t *owed;        /* a word a row of tiles, a bit a tile: what the frame buffer drawn into lacks */
    uint32_t *fresh;       /* the tiles drawn in since the frame buffers last changed places */
    uint16_t  width;       /* of the panel, in pixels */
    uint16_t  height;
    uint16_t  rows;        /* rows of tiles */
    uint8_t   shift_x;     /* a tile is 1 << shift_x pixels wide, the last one of a row what is left */
    uint8_t   shift_y;     /* and 1 << shift_y high, the last one of a column what is left */
    uint8_t   pixel_bytes;
    dgx_fb_tiles_copier_t copier; /* NULL when there is none; set by the owner after dgx_fb_tiles_init() */
    void     *copier_context;
    dgx_fb_tiles_later_t later;   /* NULL when nothing is left for later; set by the owner after dgx_fb_tiles_init() */
    void     *later_context;
    /* the tile the last pixel was drawn in, its row of tiles and its place in the row: it is not lacked and is known as drawn in */
    uint32_t  last;
    /* bytes of the frame buffer the copies of the processor went into since the range was last taken; none when copied_to <= copied_from */
    uint32_t  copied_from, copied_to;
    /* bytes copied from one frame buffer into the other since the start, by the processor and by the copier; they go round after 2^32 */
    uint32_t  copied_bytes;
} dgx_fb_tiles_t;

/**
 * @brief Make the maps for a panel; nothing is owed, nothing is drawn.
 * @param shift_x The narrowest tile, 1 << shift_x pixels; it grows until a row of the panel is 32 tiles or less.
 * @param shift_y The height of a tile, 1 << shift_y pixels: a word of the maps for each such part of the height.
 * @return false when there is no memory for the maps.
 */
bool dgx_fb_tiles_init(dgx_fb_tiles_t *tiles, uint16_t width, uint16_t height, uint8_t pixel_bytes, uint8_t shift_x, uint8_t shift_y);

/** @brief Give the maps back. */
void dgx_fb_tiles_free(dgx_fb_tiles_t *tiles);

/**
 * @brief Before a rectangle of the panel is written or read: bring the tiles it touches up to date.
 *
 * Unless the rectangle is written whole, the tiles lacked that lie side by
 * side with them in a row of tiles are brought up to date as well.
 *
 * @param to          The frame buffer drawing goes into.
 * @param from        The other one, which has what the first lacks.
 * @param x0,y0,x1,y1 The rectangle on the panel, both ends included, inside the panel.
 * @param whole       Every pixel of the rectangle is about to be written: only what its tiles have outside it is copied.
 * @param drawn       The rectangle is about to be written, not only read.
 */
void dgx_fb_tiles_ready(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, bool whole,
                        bool drawn);

/**
 * @brief Before pixels are sent into a rectangle of the panel that they are to fill, which is found out later.
 *
 * What the tiles of the rectangle have outside it is brought up to date,
 * as for dgx_fb_tiles_ready() with a rectangle written whole. The tiles are
 * left lacked: until dgx_fb_tiles_close() nothing but the pixels of the
 * rectangle may be written, and nothing read.
 */
void dgx_fb_tiles_open(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief A part of a rectangle given to dgx_fb_tiles_open() got no pixels: take it from the other frame buffer.
 *
 * To be called for every such part before dgx_fb_tiles_close().
 *
 * @param x0,y0,x1,y1 The rectangle that was opened.
 * @param px0,py0,px1,py1 The part of it nothing was written into, both ends included.
 */
void dgx_fb_tiles_unwritten(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t px0,
                            uint16_t py0, uint16_t px1, uint16_t py1);

/** @brief The pixels for a rectangle given to dgx_fb_tiles_open() have ended: its tiles are lacked no more. */
void dgx_fb_tiles_close(dgx_fb_tiles_t *tiles, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* no tile: a row of tiles that is not there */
#define DGX_FB_TILES_NO_TILE UINT32_MAX

/** @brief The same as dgx_fb_tiles_ready() before one pixel of the panel is written. */
static inline void dgx_fb_tiles_pixel(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint16_t x, uint16_t y)
{
    const uint32_t column = (uint32_t)x >> tiles->shift_x, row = (uint32_t)y >> tiles->shift_y;
    const uint32_t tile   = row << 5 | column;
    /* pixels come one next to another more often than not */
    if (tile == tiles->last) return;
    tiles->last = tile;
    if (tiles->owed[row] & 1u << column) dgx_fb_tiles_ready(tiles, to, from, x, y, x, y, false, false);
    tiles->fresh[row] |= 1u << column;
}

/** @brief The same before one pixel of the panel is read. */
static inline void dgx_fb_tiles_pixel_read(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint16_t x, uint16_t y)
{
    if (tiles->owed[(uint32_t)y >> tiles->shift_y] & (1u << (x >> tiles->shift_x))) dgx_fb_tiles_ready(tiles, to, from, x, y, x, y, false, false);
}

/**
 * @brief The picture in `to` is finished: give it every tile it still lacks.
 *
 * It lacks nothing after that and may be shown, but for what was left for
 * later. More may be drawn into it before it is shown, and it may be
 * finished again.
 */
void dgx_fb_tiles_complete(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from);

/**
 * @brief A rectangle left for later is still lacked: to be called for each one right after dgx_fb_tiles_complete().
 * @param x0,y0,x1,y1 What was left of a rectangle offered to `later`, both ends included: whole tiles.
 */
void dgx_fb_tiles_lack(dgx_fb_tiles_t *tiles, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief The frame buffers have changed places, after dgx_fb_tiles_complete() and nothing drawn since.
 *
 * What was drawn since they last did is what the frame buffer drawing goes
 * into now lacks, and nothing is drawn in it yet. What the other one was
 * left lacking is the owner's to copy.
 */
void dgx_fb_tiles_changed(dgx_fb_tiles_t *tiles);

/** @brief dgx_fb_tiles_complete() and dgx_fb_tiles_changed() at once: the picture is finished and shown. */
void dgx_fb_tiles_finish(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from);

/**
 * @brief Take the range of bytes the copies of the processor went into since the last call and forget it.
 * @return false when nothing was copied.
 */
bool dgx_fb_tiles_take_copied(dgx_fb_tiles_t *tiles, uint32_t *from, uint32_t *length);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
