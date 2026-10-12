/*
 * fb_tiles.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdlib.h>
#include <string.h>

#include "dgx_fb_tiles_priv.h"

bool dgx_fb_tiles_init(dgx_fb_tiles_t *tiles, uint16_t width, uint16_t height, uint8_t pixel_bytes, uint8_t shift_x, uint8_t shift_y)
{
    memset(tiles, 0, sizeof(*tiles));
    while (((uint32_t)width + (1u << shift_x) - 1u) >> shift_x > 32u) ++shift_x;
    const uint32_t rows = ((uint32_t)height + (1u << shift_y) - 1u) >> shift_y;
    /* both maps in one piece of memory */
    tiles->owed = (uint32_t *)calloc(2u * rows, sizeof(uint32_t));
    if (!tiles->owed) return false;
    tiles->fresh       = tiles->owed + rows;
    tiles->width       = width;
    tiles->height      = height;
    tiles->rows        = (uint16_t)rows;
    tiles->shift_x     = shift_x;
    tiles->shift_y     = shift_y;
    tiles->pixel_bytes = pixel_bytes;
    tiles->last        = DGX_FB_TILES_NO_TILE;
    return true;
}

void dgx_fb_tiles_free(dgx_fb_tiles_t *tiles)
{
    /* the maps change places, the memory starts at the lower one */
    free(tiles->owed < tiles->fresh ? tiles->owed : tiles->fresh);
    memset(tiles, 0, sizeof(*tiles));
}

/* a rectangle of the panel: the first column and row, and the first ones past the last */
typedef struct {
    uint32_t left, top, right, below;
} dgx_fb_tiles_part_t;

/* copy a rectangle of the panel; `rest` tells that it is what is left when the picture is finished */
static void dgx_fb_tiles_rectangle(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint32_t left, uint32_t top, uint32_t right, uint32_t below,
                                   bool rest)
{
    const uint32_t bytes = tiles->pixel_bytes;
    tiles->copied_bytes += (right - left) * (below - top) * bytes;
    if (rest && tiles->copier && tiles->copier(tiles->copier_context, to, from, left, top, right - left, below - top)) return;
    const size_t   pitch = (size_t)tiles->width * bytes;
    const size_t   at = (size_t)top * pitch + (size_t)left * bytes, length = (size_t)(right - left) * bytes;
    for (uint32_t y = top; y < below; ++y) memcpy(to + (size_t)y * pitch + (size_t)left * bytes, from + (size_t)y * pitch + (size_t)left * bytes, length);
    const uint32_t end = (uint32_t)((size_t)(below - 1u) * pitch + (size_t)left * bytes + length);
    if (tiles->copied_to <= tiles->copied_from) {
        tiles->copied_from = (uint32_t)at;
        tiles->copied_to   = end;
    } else {
        if (at < tiles->copied_from) tiles->copied_from = (uint32_t)at;
        if (end > tiles->copied_to) tiles->copied_to = end;
    }
}

/* the tiles side by side that begin with the lowest bit of `mask`: the place of the first one and how many they are */
static inline uint32_t dgx_fb_tiles_run(uint32_t mask, uint32_t *first)
{
    *first              = (uint32_t)__builtin_ctz(mask);
    const uint32_t rest = ~(mask >> *first); /* its lowest bit set is where the run ends */
    return rest ? (uint32_t)__builtin_ctz(rest) : 32u - *first;
}

static inline uint32_t dgx_fb_tiles_run_mask(uint32_t first, uint32_t count)
{
    return count == 32u ? UINT32_MAX : ((1u << count) - 1u) << first;
}

/*
 * Copy the tiles of a row of tiles the bits of `mask` stand for, or what of
 * them lies in `part`; tiles side by side go as one piece.
 */
static void dgx_fb_tiles_copy(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint32_t row, uint32_t mask, const dgx_fb_tiles_part_t *part)
{
    const uint32_t shift = tiles->shift_x;
    uint32_t       top   = row << tiles->shift_y;
    uint32_t       below = top + (1u << tiles->shift_y);
    if (below > tiles->height) below = tiles->height;
    if (part) {
        if (top < part->top) top = part->top;
        if (below > part->below) below = part->below;
        if (top >= below) return;
    }
    while (mask) {
        uint32_t       first;
        const uint32_t count = dgx_fb_tiles_run(mask, &first);
        mask &= ~dgx_fb_tiles_run_mask(first, count);
        uint32_t left  = first << shift;
        uint32_t right = (first + count) << shift;
        if (right > tiles->width) right = tiles->width;
        if (part) {
            if (left < part->left) left = part->left;
            if (right > part->right) right = part->right;
            if (left >= right) continue;
        }
        dgx_fb_tiles_rectangle(tiles, to, from, left, top, right, below, false);
    }
}

/* the tiles a rectangle touches */
typedef struct {
    uint32_t row0, row1;       /* the rows of tiles, both included */
    uint32_t column0, column1; /* the tiles of such a row, both included */
    uint32_t mask;             /* and their bits */
} dgx_fb_tiles_span_t;

static void dgx_fb_tiles_span(const dgx_fb_tiles_t *tiles, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, dgx_fb_tiles_span_t *span)
{
    span->row0    = y0 >> tiles->shift_y;
    span->row1    = y1 >> tiles->shift_y;
    span->column0 = x0 >> tiles->shift_x;
    span->column1 = x1 >> tiles->shift_x;
    span->mask    = ((2u << (span->column1 - span->column0)) - 1u) << span->column0;
}

/*
 * A rectangle is about to be written whole: of the tiles `lacks` of a row
 * of tiles it touches, only what lies outside it has to come from the other
 * frame buffer. That is the rows of the tiles above it and below it, and in
 * its own rows what the first tile has to the left of it and the last one
 * to the right.
 */
static void dgx_fb_tiles_around(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint32_t row, uint32_t lacks, const dgx_fb_tiles_span_t *span,
                                uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    dgx_fb_tiles_part_t part = {0, 0, tiles->width, y0};
    if (row == span->row0) dgx_fb_tiles_copy(tiles, to, from, row, lacks, &part);
    part.top = y1 + 1u, part.below = tiles->height;
    if (row == span->row1) dgx_fb_tiles_copy(tiles, to, from, row, lacks, &part);
    part.top = y0, part.below = y1 + 1u, part.right = x0;
    if (lacks & (1u << span->column0)) dgx_fb_tiles_copy(tiles, to, from, row, 1u << span->column0, &part);
    part.left = x1 + 1u, part.right = tiles->width;
    if (lacks & (1u << span->column1)) dgx_fb_tiles_copy(tiles, to, from, row, 1u << span->column1, &part);
}

typedef enum {
    DgxFbTilesPart,  /* the rectangle is written in part or read: every tile it touches is brought up to date */
    DgxFbTilesWhole, /* it is written whole: only what its tiles have outside it is */
    DgxFbTilesOpen,  /* it is to be written whole, which is not known yet: the same, and its tiles stay lacked */
} dgx_fb_tiles_how_t;

static void dgx_fb_tiles_touch(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                               dgx_fb_tiles_how_t how, bool drawn)
{
    dgx_fb_tiles_span_t span;
    dgx_fb_tiles_span(tiles, x0, y0, x1, y1, &span);
    for (uint32_t row = span.row0; row <= span.row1; ++row) {
        const uint32_t owed  = tiles->owed[row];
        uint32_t       lacks = owed & span.mask;
        if (lacks && how == DgxFbTilesPart) {
            /*
             * The tiles lacked that lie side by side with these are taken
             * along: they are most likely next, as when a picture is drawn
             * pixel by pixel, and one long piece costs half of what the
             * same tiles cost one at a time.
             */
            for (uint32_t grown = (lacks | lacks << 1 | lacks >> 1) & owed; grown != lacks; grown = (lacks | lacks << 1 | lacks >> 1) & owed) lacks = grown;
            tiles->owed[row] = owed & ~lacks;
            dgx_fb_tiles_copy(tiles, to, from, row, lacks, NULL);
        } else if (lacks) {
            if (how != DgxFbTilesOpen) tiles->owed[row] = owed & ~span.mask;
            dgx_fb_tiles_around(tiles, to, from, row, lacks, &span, x0, y0, x1, y1);
        }
        if (drawn) tiles->fresh[row] |= span.mask;
    }
}

void dgx_fb_tiles_ready(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, bool whole,
                        bool drawn)
{
    dgx_fb_tiles_touch(tiles, to, from, x0, y0, x1, y1, whole ? DgxFbTilesWhole : DgxFbTilesPart, drawn);
}

void dgx_fb_tiles_open(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    dgx_fb_tiles_touch(tiles, to, from, x0, y0, x1, y1, DgxFbTilesOpen, true);
}

void dgx_fb_tiles_unwritten(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t px0,
                            uint16_t py0, uint16_t px1, uint16_t py1)
{
    dgx_fb_tiles_span_t       span;
    const dgx_fb_tiles_part_t part = {px0, py0, (uint32_t)px1 + 1u, (uint32_t)py1 + 1u};
    dgx_fb_tiles_span(tiles, x0, y0, x1, y1, &span);
    /* a tile of the rectangle that is not lacked any more held nothing old when the rectangle was opened */
    for (uint32_t row = (uint32_t)py0 >> tiles->shift_y; row <= (uint32_t)py1 >> tiles->shift_y; ++row) {
        const uint32_t lacks = tiles->owed[row] & span.mask;
        if (lacks) dgx_fb_tiles_copy(tiles, to, from, row, lacks, &part);
    }
}

void dgx_fb_tiles_close(dgx_fb_tiles_t *tiles, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    dgx_fb_tiles_span_t span;
    dgx_fb_tiles_span(tiles, x0, y0, x1, y1, &span);
    for (uint32_t row = span.row0; row <= span.row1; ++row) tiles->owed[row] &= ~span.mask;
}

void dgx_fb_tiles_complete(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from)
{
    const uint32_t rows = tiles->rows;
    for (uint32_t row = 0; row < rows; ++row) {
        while (tiles->owed[row]) {
            uint32_t       first;
            const uint32_t count = dgx_fb_tiles_run(tiles->owed[row], &first);
            const uint32_t mask  = dgx_fb_tiles_run_mask(first, count);
            /*
             * The same tiles of the rows of tiles below go with these as
             * one rectangle, where they stand as alone as here: a run that
             * is longer there is better left whole.
             */
            const uint32_t around = (first ? 1u << (first - 1u) : 0u) | (first + count < 32u ? 1u << (first + count) : 0u);
            uint32_t       last   = row;
            while (last + 1u < rows && (tiles->owed[last + 1u] & (mask | around)) == mask) ++last;
            for (uint32_t taken = row; taken <= last; ++taken) tiles->owed[taken] &= ~mask;
            const uint32_t left = first << tiles->shift_x, top = row << tiles->shift_y;
            uint32_t       right = (first + count) << tiles->shift_x, below = (last + 1u) << tiles->shift_y;
            if (right > tiles->width) right = tiles->width;
            if (below > tiles->height) below = tiles->height;
            /* what the owner leaves for later is not copied, and it tells of it by dgx_fb_tiles_lack() */
            if (tiles->later) {
                const uint32_t now = tiles->later(tiles->later_context, left, top, right - left, below - top);
                if (now < below - top) below = top + now;
            }
            if (below > top) dgx_fb_tiles_rectangle(tiles, to, from, left, top, right, below, true);
        }
    }
}

void dgx_fb_tiles_lack(dgx_fb_tiles_t *tiles, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    dgx_fb_tiles_span_t span;
    dgx_fb_tiles_span(tiles, x0, y0, x1, y1, &span);
    for (uint32_t row = span.row0; row <= span.row1; ++row) tiles->owed[row] |= span.mask;
}

void dgx_fb_tiles_changed(dgx_fb_tiles_t *tiles)
{
    /* what was left for later is not lacked by the frame buffer drawing goes into now */
    memset(tiles->owed, 0, tiles->rows * sizeof(uint32_t));
    /* what was drawn is what the other frame buffer lacks; the map that is empty takes what is drawn next */
    uint32_t *drawn = tiles->fresh;
    tiles->fresh    = tiles->owed;
    tiles->owed     = drawn;
    tiles->last     = DGX_FB_TILES_NO_TILE;
}

void dgx_fb_tiles_finish(dgx_fb_tiles_t *tiles, uint8_t *to, const uint8_t *from)
{
    dgx_fb_tiles_complete(tiles, to, from);
    dgx_fb_tiles_changed(tiles);
}

bool dgx_fb_tiles_take_copied(dgx_fb_tiles_t *tiles, uint32_t *from, uint32_t *length)
{
    if (tiles->copied_to <= tiles->copied_from) return false;
    *from              = tiles->copied_from;
    *length            = tiles->copied_to - tiles->copied_from;
    tiles->copied_from = tiles->copied_to = 0;
    return true;
}
