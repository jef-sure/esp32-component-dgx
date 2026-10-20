/*
 * overlay.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdlib.h>
#include <string.h>

#include "dgx_overlay.h"
#include "dgx_overlay_priv.h"
#include "drivers/vscreen.h"

/* the rectangle, both ends included, joins the changed part of the screen */
static void dgx_overlay_changed(dgx_overlay_t *overlay, int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    const dgx_screen_t *display = overlay->display;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= display->width) right = display->width - 1;
    if (bottom >= display->height) bottom = display->height - 1;
    if (left > right || top > bottom) return;
    if (!overlay->changed) {
        overlay->changed      = true;
        overlay->changed_left = left, overlay->changed_top = top, overlay->changed_right = right, overlay->changed_bottom = bottom;
        return;
    }
    if (left < overlay->changed_left) overlay->changed_left = left;
    if (top < overlay->changed_top) overlay->changed_top = top;
    if (right > overlay->changed_right) overlay->changed_right = right;
    if (bottom > overlay->changed_bottom) overlay->changed_bottom = bottom;
}

/* where the layer has something: its part of the screen joins it; both ends included */
static void dgx_overlay_reaches(dgx_overlay_t *overlay, int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    dgx_bus_overlay_t *bus = &overlay->bus;
    /* within the screen and the layer */
    if (left < bus->x) left = bus->x;
    if (top < bus->y) top = bus->y;
    if (right > bus->x + overlay->layer->width - 1) right = bus->x + overlay->layer->width - 1;
    if (bottom > bus->y + overlay->layer->height - 1) bottom = bus->y + overlay->layer->height - 1;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= overlay->display->width) right = overlay->display->width - 1;
    if (bottom >= overlay->display->height) bottom = overlay->display->height - 1;
    if (left > right || top > bottom) return;
    if (bus->right < bus->left) {
        bus->left = left, bus->top = top, bus->right = right, bus->bottom = bottom;
        return;
    }
    if (left < bus->left) bus->left = left;
    if (top < bus->top) bus->top = top;
    if (right > bus->right) bus->right = right;
    if (bottom > bus->bottom) bus->bottom = bottom;
}

/* the tiles of a rectangle of the layer, both ends included, may have something now */
static void dgx_overlay_tiles_drawn(dgx_overlay_t *overlay, int left, int top, int right, int bottom)
{
    if (!overlay->tiles) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= overlay->layer->width) right = overlay->layer->width - 1;
    if (bottom >= overlay->layer->height) bottom = overlay->layer->height - 1;
    for (int ty = top >> DGX_BUS_OVERLAY_TILE_SHIFT; ty <= bottom >> DGX_BUS_OVERLAY_TILE_SHIFT && top <= bottom; ++ty) {
        uint8_t *row = overlay->tiles + (size_t)ty * overlay->bus.tiles_across;
        for (int tx = left >> DGX_BUS_OVERLAY_TILE_SHIFT; tx <= right >> DGX_BUS_OVERLAY_TILE_SHIFT && left <= right; ++tx) row[tx] = 1;
    }
}

/* what is drawn into the layer comes here on its way out */
static void dgx_overlay_layer_drawn(dgx_screen_t *layer, int left, int right, int top, int bottom)
{
    dgx_overlay_t *overlay = layer->overlay;
    const int32_t  x = overlay->bus.x, y = overlay->bus.y;
    dgx_overlay_tiles_drawn(overlay, left, top, right, bottom);
    dgx_overlay_reaches(overlay, x + left, y + top, x + right, y + bottom);
    dgx_overlay_changed(overlay, x + left, y + top, x + right, y + bottom);
    if (overlay->layer_update) overlay->layer_update(layer, left, right, top, bottom);
}

/* the part of the screen the layer has something in, looked for in its pixels */
static void dgx_overlay_look(dgx_overlay_t *overlay)
{
    const dgx_screen_t *layer = overlay->layer;
    dgx_bus_overlay_t  *bus   = &overlay->bus;
    bus->left = bus->top = 0;
    bus->right = bus->bottom = -1;
    if (overlay->tiles) memset(overlay->tiles, 0, (size_t)bus->tiles_across * overlay->tiles_down);
    for (int ly = 0; ly < layer->height; ++ly) {
        const uint8_t *row = bus->pixels + (size_t)ly * bus->pitch;
        const uint32_t width = (uint32_t)layer->width;
        uint32_t       first = dgx_overlay_next(row, 0, width);
        if (first == width) continue;
        uint32_t last = width - 1;
        while (!row[last]) --last;
        dgx_overlay_reaches(overlay, bus->x + (int32_t)first, bus->y + ly, bus->x + (int32_t)last, bus->y + ly);
        if (!overlay->tiles) continue;
        /* the tiles of the row that have something */
        uint8_t *tiles = overlay->tiles + (size_t)(ly >> DGX_BUS_OVERLAY_TILE_SHIFT) * bus->tiles_across;
        for (uint32_t j = first; j < width; j = dgx_overlay_next(row, j, width)) {
            tiles[j >> DGX_BUS_OVERLAY_TILE_SHIFT] = 1;
            j = ((j >> DGX_BUS_OVERLAY_TILE_SHIFT) + 1u) << DGX_BUS_OVERLAY_TILE_SHIFT;
        }
    }
}

/* the layer taken away; the screen keeps what it has changed until that is taken */
static void dgx_overlay_detach(dgx_overlay_t *overlay)
{
    dgx_bus_overlay_t *bus = &overlay->bus;
    if (!overlay->layer) return;
    if (bus->right >= bus->left) dgx_overlay_changed(overlay, bus->left, bus->top, bus->right, bus->bottom);
    overlay->layer->update_screen = overlay->layer_update;
    overlay->layer->overlay       = NULL;
    overlay->layer                = NULL;
    bus->left = bus->top = 0;
    bus->right = bus->bottom = -1;
    free(overlay->tiles);
    overlay->tiles = NULL;
    bus->tiles     = NULL;
}

void dgx_overlay_forget(dgx_screen_t *scr)
{
    dgx_overlay_t *overlay = scr->overlay;
    if (!overlay) return;
    dgx_overlay_detach(overlay);
    if (overlay->display == scr) {
        scr->overlay = NULL;
        free(overlay);
    }
}

bool dgx_overlay_set(dgx_screen_t *display, dgx_screen_t *layer, int x, int y, const uint16_t *lut, uint8_t alpha)
{
    if (display->color_bits != 16 || layer->color_bits != 8 || !dgx_vscreen_is_linear(layer) || layer == display) return false;
    dgx_overlay_t *overlay = display->overlay;
    if (layer->overlay && layer->overlay != overlay) return false;
    if (overlay && overlay->layer != layer) dgx_overlay_detach(overlay);
    if (!overlay) {
        overlay = calloc(1, sizeof(*overlay));
        if (!overlay) return false;
        overlay->display = display;
        display->overlay = overlay;
    }
    dgx_bus_overlay_t *bus = &overlay->bus;
    if (!overlay->layer) {
        overlay->layer        = layer;
        overlay->layer_update = layer->update_screen;
        layer->update_screen  = dgx_overlay_layer_drawn;
        layer->overlay        = overlay;
        /* the map of tiles; without memory for it the layer is looked at everywhere it reaches, which is slower */
        bus->tiles_across    = (uint32_t)(layer->width + (1 << DGX_BUS_OVERLAY_TILE_SHIFT) - 1) >> DGX_BUS_OVERLAY_TILE_SHIFT;
        overlay->tiles_down  = (uint32_t)(layer->height + (1 << DGX_BUS_OVERLAY_TILE_SHIFT) - 1) >> DGX_BUS_OVERLAY_TILE_SHIFT;
        overlay->tiles       = calloc((size_t)bus->tiles_across * overlay->tiles_down, 1);
        bus->tiles           = overlay->tiles;
    } else if (bus->right >= bus->left) {
        /* where it was is changed */
        dgx_overlay_changed(overlay, bus->left, bus->top, bus->right, bus->bottom);
    }
    bus->pixels = ((dgx_vscreen_t *)layer)->v_array;
    bus->pitch  = (uint32_t)layer->width;
    bus->x      = x;
    bus->y      = y;
    /* the weight of the layer in 32 steps, and its colors times it */
    const uint32_t weight = ((uint32_t)alpha * 32u + 127u) / 255u;
    bus->under            = 32u - weight;
    for (int i = 0; i < 256; ++i) overlay->pre[i] = ((lut[i] | (uint32_t)lut[i] << 16) & 0x07E0F81Fu) * weight;
    bus->pre = overlay->pre;
    dgx_overlay_look(overlay);
    if (bus->right >= bus->left) dgx_overlay_changed(overlay, bus->left, bus->top, bus->right, bus->bottom);
    return true;
}

void dgx_overlay_clear(dgx_screen_t *display)
{
    dgx_overlay_t *overlay = display->overlay;
    if (overlay && overlay->display == display) dgx_overlay_detach(overlay);
}

bool dgx_overlay_take_dirty(dgx_screen_t *display, int *x, int *y, int *width, int *height)
{
    dgx_overlay_t *overlay = display->overlay;
    const bool     changed = overlay && overlay->display == display && overlay->changed;
    *x      = changed ? overlay->changed_left : 0;
    *y      = changed ? overlay->changed_top : 0;
    *width  = changed ? overlay->changed_right - overlay->changed_left + 1 : 0;
    *height = changed ? overlay->changed_bottom - overlay->changed_top + 1 : 0;
    if (overlay && overlay->display == display) {
        overlay->changed = false;
        /* with no layer and nothing left to take, the screen has nothing of it */
        if (!overlay->layer) dgx_overlay_forget(display);
    }
    return changed;
}

void dgx_overlay_row(const dgx_bus_overlay_t *overlay, uint8_t *row, int x, int y, int count)
{
    if (y < overlay->top || y > overlay->bottom) return;
    int from = x > overlay->left ? x : overlay->left, to = x + count - 1 < overlay->right ? x + count - 1 : overlay->right;
    if (from > to) return;
    const uint8_t  *index = overlay->pixels + (size_t)(y - overlay->y) * overlay->pitch + (size_t)(from - overlay->x);
    uint8_t        *at    = row + 2 * (from - x);
    const uint32_t  n = (uint32_t)(to - from + 1), under = overlay->under;
    const uint32_t *pre = overlay->pre;
    const int32_t lx = from - overlay->x, ly = y - overlay->y;
    for (uint32_t j = dgx_overlay_next_in(overlay, index, lx, ly, 0, n); j < n; j = dgx_overlay_next_in(overlay, index, lx, ly, j, n)) {
        /* a run of indices that are not empty */
        for (; j < n && index[j]; ++j) {
            uint8_t       *pixel = at + 2 * j;
            const uint16_t mixed = under ? dgx_bus_overlay_mix((uint16_t)(pixel[0] << 8 | pixel[1]), pre[index[j]], under)
                                         : (uint16_t)((pre[index[j]] >> 5) | (pre[index[j]] >> 21));
            pixel[0]             = (uint8_t)(mixed >> 8);
            pixel[1]             = (uint8_t)mixed;
        }
    }
}
