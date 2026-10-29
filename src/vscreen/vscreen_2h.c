#include <string.h>

#include "dgx_arch.h"
#include "dgx_bits.h"
#include "dgx_halves_priv.h"
#include "dgx_screen_functions.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/vscreen.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct _dgx_vscreen_2h_t {
    /* first: the screen is a virtual screen to every function that takes one, its pixels and drawing are those of dgx_vscreen_init() */
    dgx_vscreen_t vscreen;
    dgx_screen_t *left_screen;
    dgx_screen_t *right_screen;
} dgx_vscreen_2h_t;

static const char TAG[] = "DGX V-Screen 2H";

// Initialize the display

static void dgx_vscreen_2h_update_area(dgx_screen_t *_scr, int left, int right, int top, int bottom) {
    dgx_vscreen_2h_t *scr = (dgx_vscreen_2h_t *)_scr;
    int ll = DGX_MAX(0, left);
    int lr = DGX_MIN(scr->left_screen->width - 1, right);
    int rl = DGX_MAX(scr->left_screen->width, left);
    int rr = DGX_MIN(scr->left_screen->width + scr->right_screen->width - 1, right);
    int lw = lr - ll + 1;
    int rw = rr - rl + 1;
    /*
     * Whole rows of both panels go side by side through their buses, the same
     * rows to each: only when the panels are of one height, or the shorter one
     * would be sent rows its window has no room for, and the window would take
     * them from its top again. Each panel is sent through the halves of its
     * bus buffer, a row to one and then a row to the other.
     */
    if (scr->left_screen->screen_subtype == DgxPhysicalScreenWithBus && scr->right_screen->screen_subtype == DgxPhysicalScreenWithBus &&
        scr->left_screen->color_bits == 16 && lw == rw && lw == scr->left_screen->width && rw == scr->right_screen->width &&
        scr->left_screen->height == scr->right_screen->height) {
        top = DGX_MAX(0, top);
        bottom = DGX_MIN(_scr->height - 1, bottom);
        if (top > bottom) return;
        dgx_vscreen_halves_t lh, rh;
        dgx_vscreen_halves_window(&lh, scr->left_screen, ll, top, lw, bottom - top + 1, 2u);
        dgx_vscreen_halves_window(&rh, scr->right_screen, rl - scr->left_screen->width, top, rw, bottom - top + 1, 2u);
        for (int br = top; br <= bottom; ++br) {
            const uint8_t *lp_src = scr->vscreen.v_array + (size_t)_scr->width * (size_t)br * 2u;
            dgx_vscreen_send_row(&lh, lp_src, (uint32_t)lw * 2u);
            dgx_vscreen_send_row(&rh, lp_src + (size_t)scr->left_screen->width * 2u, (uint32_t)rw * 2u);
        }
        /* the last pieces of both are on their way when this returns, as before: whatever writes a bus buffer next waits for it */
        dgx_vscreen_halves_flush(&lh);
        dgx_vscreen_halves_flush(&rh);
    } else {
        if (lw > 0) {
            dgx_vscreen_region_to_screen(scr->left_screen, ll, top, _scr, ll, top, lw, bottom - top + 1, 0);
        }
        if (rw > 0) {
            dgx_vscreen_region_to_screen(scr->right_screen, rl - scr->left_screen->width, top, _scr, rl, top, rw, bottom - top + 1, 0);
        }
    }
}

dgx_screen_t *dgx_vscreen_2h_init(dgx_screen_t *left_screen, dgx_screen_t *right_screen) {
    dgx_vscreen_t *vscr = (dgx_vscreen_t *)dgx_vscreen_init(1, 1, left_screen->color_bits, left_screen->rgb_order);
    if (!vscr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }
    dgx_vscreen_2h_t *scr = (dgx_vscreen_2h_t *)calloc(1, sizeof(dgx_vscreen_2h_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        dgx_screen_destroy((dgx_screen_t **)&vscr);
        return NULL;
    }
    dgx_screen_t *base = &scr->vscreen.base;
    base->cg_row_shift = 0;
    base->cg_col_shift = 0;
    base->width = left_screen->width + right_screen->width;
    base->height = DGX_MAX(left_screen->height, right_screen->height);
    base->color_bits = left_screen->color_bits;
    base->rgb_order = left_screen->rgb_order;
    base->dir_x = DgxScreenLeftRight;
    base->dir_y = DgxScreenTopBottom;
    base->swap_xy = false;
    base->screen_name = "V-SCREEN 2H";
    base->screen_submodel = 0;
    base->screen_subtype = DgxVirtualBackScreen;
    base->circle = vscr->base.circle;
    base->destroy = vscr->base.destroy;
    base->set_pixel = vscr->base.set_pixel;
    base->fill_rectangle = vscr->base.fill_rectangle;
    base->draw_line = vscr->base.draw_line;
    base->get_pixel = vscr->base.get_pixel;
    base->set_area = vscr->base.set_area;
    base->read_area = vscr->base.read_area;
    base->write_area = vscr->base.write_area;
    base->wait_buffer = vscr->base.wait_buffer;
    base->solid_circle = vscr->base.solid_circle;
    base->update_screen = dgx_vscreen_2h_update_area;
    scr->left_screen = left_screen;
    scr->right_screen = right_screen;
    dgx_screen_destroy((dgx_screen_t **)&vscr);
    uint32_t asize = dgx_color_points_to_bytes(base->color_bits, base->width * base->height);

    scr->vscreen.v_array = calloc(asize, 1);
    if (!scr->vscreen.v_array) {
        ESP_LOGE(TAG, "Impossible to allocation memory");
        free(scr);
        return NULL;
    }
    return base;
}
