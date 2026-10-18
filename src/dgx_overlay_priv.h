#pragma once
/*
 * dgx_overlay_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  The overlay layer of a screen as the transfers to the screen see it, and
 *  the mixing of it into the pixels they put. Nothing here depends on the
 *  platform.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef struct _dgx_overlay_t {
    dgx_bus_overlay_t        bus;     /* what the transfers mix in */
    dgx_screen_t            *display; /* the screen it lies over */
    dgx_screen_t            *layer;
    dgx_scr_update_area_func layer_update; /* the update_screen of the layer before it was taken over */
    /* the changed part of the screen, both ends included; none when `changed` is false */
    bool                     changed;
    int32_t                  changed_left, changed_top, changed_right, changed_bottom;
    uint32_t                 pre[256];
    uint8_t                 *tiles; /* of the layer, see dgx_bus_overlay_t */
    uint32_t                 tiles_down;
} dgx_overlay_t;

/* the layer over the rectangle of the screen, both ends included, as the transfers mix it in; NULL when none is over it */
static inline const dgx_bus_overlay_t *dgx_overlay_over(const dgx_screen_t *display, int left, int top, int right, int bottom)
{
    const dgx_overlay_t *overlay = display->overlay;
    if (!overlay || overlay->display != display || !overlay->layer) return 0;
    const dgx_bus_overlay_t *bus = &overlay->bus;
    if (bus->right < bus->left || left > bus->right || right < bus->left || top > bus->bottom || bottom < bus->top) return 0;
    return bus;
}

/* the color the pixel (x, y) of the screen gets from the color put there, the layer over it */
static inline uint16_t dgx_overlay_pixel(const dgx_bus_overlay_t *overlay, int x, int y, uint16_t color)
{
    if (x < overlay->left || x > overlay->right || y < overlay->top || y > overlay->bottom) return color;
    const uint8_t index = overlay->pixels[(size_t)(y - overlay->y) * overlay->pitch + (size_t)(x - overlay->x)];
    return index ? dgx_bus_overlay_mix(color, overlay->pre[index], overlay->under) : color;
}

/*
 * The first index from j on in the run p[0 .. n - 1] that is not 0, or n
 * when there is none: empty indices are passed over a word at a time,
 * which is most of a layer.
 */
static inline uint32_t dgx_overlay_next(const uint8_t *p, uint32_t j, uint32_t n)
{
    while (j < n && ((uintptr_t)(p + j) & 3u)) {
        if (p[j]) return j;
        ++j;
    }
    while (j + 4u <= n && !*(const uint32_t *)(const void *)(p + j)) j += 4u;
    while (j < n && !p[j]) ++j;
    return j;
}

/*
 * The same along a row of the layer that the tiles of the layer look over
 * first: `index` is the layer's point `lx` of its row `ly`, and a tile with
 * nothing in it is passed over whole.
 */
static inline uint32_t dgx_overlay_next_in(const dgx_bus_overlay_t *overlay, const uint8_t *index, int32_t lx, int32_t ly, uint32_t j, uint32_t n)
{
    if (!overlay->tiles) return dgx_overlay_next(index, j, n);
    const uint8_t *tiles = overlay->tiles + (size_t)(ly >> DGX_BUS_OVERLAY_TILE_SHIFT) * overlay->tiles_across;
    while (j < n) {
        const int32_t  at   = lx + (int32_t)j;
        const uint32_t next = (uint32_t)((((at >> DGX_BUS_OVERLAY_TILE_SHIFT) + 1) << DGX_BUS_OVERLAY_TILE_SHIFT) - lx);
        const uint32_t end  = next < n ? next : n;
        if (tiles[at >> DGX_BUS_OVERLAY_TILE_SHIFT]) {
            const uint32_t k = dgx_overlay_next(index, j, end);
            if (k < end) return k;
        }
        j = end;
    }
    return n;
}

/* `count` pixels of the row y of the screen from x on, kept two bytes each with the high one first, as DGX keeps them, mixed with the layer */
void dgx_overlay_row(const dgx_bus_overlay_t *overlay, uint8_t *row, int x, int y, int count);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
