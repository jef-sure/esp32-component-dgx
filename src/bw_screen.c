#include <string.h>

#include "dgx_bw_screen.h"
#include "dgx_screen_functions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char TAG[] = "DGX BW-Screen";

void dgx_bw_set_area(dgx_screen_t *_scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    dgx_bw_vscreen_t *scr = (dgx_bw_vscreen_t *)_scr;
    if (left >= scr->base.width || top >= scr->base.height || right >= scr->base.width || bottom >= scr->base.height) {
        return;
    }
    scr->area.left     = left;
    scr->area.right    = right;
    scr->area.top      = top;
    scr->area.bottom   = bottom;
    scr->area.x_offset = left;
    scr->area.y_offset = top;
}

static inline void _dgx_move_to_next_area_pixel_v(dgx_screen_t *_scr)
{
    dgx_bw_vscreen_t *scr = (dgx_bw_vscreen_t *)_scr;
    if (scr->area.x_offset >= scr->area.right) {
        scr->area.x_offset = scr->area.left;
        if (scr->area.y_offset >= scr->area.bottom) {
            scr->area.y_offset = scr->area.top;
        } else {
            scr->area.y_offset++;
        }
    } else {
        scr->area.x_offset++;
    }
}

void dgx_bw_write_data(dgx_screen_t *scr_, uint8_t *data, uint32_t lenbits)
{
    if (!lenbits) return;
    dgx_bw_vscreen_t *scr    = (dgx_bw_vscreen_t *)scr_;
    const uint16_t    left   = scr->area.left;
    const uint16_t    right  = scr->area.right;
    const uint16_t    top    = scr->area.top;
    const uint16_t    bottom = scr->area.bottom;
    const int         width  = scr->base.width;
    uint8_t          *fb     = scr->v_array;
    uint16_t          x      = scr->area.x_offset;
    uint16_t          y      = scr->area.y_offset;
    uint8_t           smask  = 0x80;
    uint8_t           sbyte  = *data;

    while (lenbits) {
        if (x > right) {
            x = left;
            if (y > bottom) y = top;
            else y++;
        }
        const uint8_t rmask = (uint8_t)(1u << (y & 7));
        const uint8_t rclr  = (uint8_t)~rmask;
        uint8_t      *pb    = fb + (uint32_t)(y >> 3) * width + x;
        uint32_t      run   = (uint32_t)(right - x + 1);
        if (run > lenbits) run = lenbits;
        lenbits -= run;
        x = (uint16_t)(x + run);
        /* Byte-aligned fast path: consume whole source bytes 8 pixels at a time. */
        while (smask == 0x80 && run >= 8) {
            uint8_t s = sbyte;
            pb[0] = (s & 0x80) ? (uint8_t)(pb[0] | rmask) : (uint8_t)(pb[0] & rclr);
            pb[1] = (s & 0x40) ? (uint8_t)(pb[1] | rmask) : (uint8_t)(pb[1] & rclr);
            pb[2] = (s & 0x20) ? (uint8_t)(pb[2] | rmask) : (uint8_t)(pb[2] & rclr);
            pb[3] = (s & 0x10) ? (uint8_t)(pb[3] | rmask) : (uint8_t)(pb[3] & rclr);
            pb[4] = (s & 0x08) ? (uint8_t)(pb[4] | rmask) : (uint8_t)(pb[4] & rclr);
            pb[5] = (s & 0x04) ? (uint8_t)(pb[5] | rmask) : (uint8_t)(pb[5] & rclr);
            pb[6] = (s & 0x02) ? (uint8_t)(pb[6] | rmask) : (uint8_t)(pb[6] & rclr);
            pb[7] = (s & 0x01) ? (uint8_t)(pb[7] | rmask) : (uint8_t)(pb[7] & rclr);
            pb += 8;
            run -= 8;
            if (run || lenbits) sbyte = *++data;
        }
        while (run--) {
            if (!smask) {
                sbyte = *++data;
                smask = 0x80;
            }
            if (sbyte & smask) *pb |= rmask;
            else *pb &= rclr;
            ++pb;
            smask >>= 1;
        }
    }
    scr->area.x_offset = x;
    scr->area.y_offset = y;
}

void dgx_bw_set_pixel(dgx_screen_t *scr_, int x, int y, uint32_t color)
{
    dgx_bw_vscreen_t *scr  = (dgx_bw_vscreen_t *)scr_;
    uint8_t           page = (uint16_t)y >> 3;
    uint8_t           bit  = y & 7;
    uint8_t           mask = 1 << bit;
    uint8_t          *pb   = scr->v_array + page * scr->base.width + x;
    if (color) *pb |= mask;
    else *pb &= ~mask;
    if (!scr_->in_progress) scr_->update_screen(scr_, x, x, y, y);
}

uint32_t dgx_bw_get_pixel(dgx_screen_t *scr_, int x, int y)
{
    dgx_bw_vscreen_t *scr  = (dgx_bw_vscreen_t *)scr_;
    uint8_t           page = (uint16_t)y >> 3;
    uint8_t           bit  = y & 7;
    uint8_t           mask = 1 << bit;
    uint8_t          *pb   = scr->v_array + page * scr->base.width + x;
    return !!(*pb & mask);
}

void dgx_bw_wait_data(dgx_screen_t *scr)
{
    return;
}

void dgx_bw_fast_vline(dgx_screen_t *scr_, int x, int y, int h, uint32_t color)
{
    dgx_bw_vscreen_t *scr  = (dgx_bw_vscreen_t *)scr_;
    int               y2   = y + h - 1;
    uint8_t           page = (uint16_t)y >> 3;
    uint8_t          *pb   = scr->v_array + page * scr->base.width + x;
    for (int yl = y; yl <= y2; yl += 8 - (yl & 7), pb += scr->base.width) {
        page         = (uint16_t)yl >> 3;
        uint8_t mask = 0xff << (yl & 7);
        if ((yl & ~7) == (y2 & ~7)) mask &= 0xff >> (7 - (y2 & 7));
        if (color) *pb |= mask;
        else *pb &= ~mask;
    }
    if (!scr_->in_progress) scr_->update_screen(scr_, x, x, y, y + h - 1);
}

void dgx_bw_fill_rectangle(dgx_screen_t *scr_, int x, int y, int w, int h, uint32_t color)
{
    dgx_bw_vscreen_t *scr = (dgx_bw_vscreen_t *)scr_;
    if (y < 0 || w < 0 || x + w < 0 || x >= scr->base.width || h < 0 || y + h < 0 || y > scr->base.height) return;
    if (x < 0) {
        w = x + w;
        x = 0;
    }
    if (x + w > scr_->width) {
        w = scr_->width - x;
    }
    if (y < 0) {
        h = y + h;
        y = 0;
    }
    if (y + h > scr->base.height) {
        h = scr->base.height - y;
    }
    dgx_screen_progress_up(scr_);
    if (x == 0 && w == scr->base.width && y == 0 && h == scr->base.height) {
        // full screen fill
        memset(scr->v_array, color ? 0xFF : 0x00, ((scr->base.height + 7) / 8) * scr->base.width);
    } else {
        for (int i = 0; i < w; ++i) {
            dgx_bw_fast_vline(scr_, x + i, y, h, color);
        }
    }
    if (!dgx_screen_progress_down(scr_)) scr_->update_screen(scr_, x, x + w - 1, y, y + h - 1);
}

void dgx_bw_update_screen(dgx_screen_t *scr_, int left, int right, int top, int bottom)
{
}

void dgx_bw_destroy_screen(dgx_screen_t **scr_)
{
    dgx_bw_vscreen_t *scr = (dgx_bw_vscreen_t *)*scr_;
    free(scr->v_array);
    free(*scr_);
    *scr_ = 0;
}

dgx_screen_t *dgx_bw_init(int width, int height)
{
    dgx_bw_vscreen_t *scr = (dgx_bw_vscreen_t *)heap_caps_calloc(1, sizeof(dgx_bw_vscreen_t), MALLOC_CAP_DMA);
    if (!scr) {
        ESP_LOGE(TAG, "Impossible to allocate memory for BW virtual screen");
        return 0;
    }
    scr->base.in_progress = 0;
    scr->base.width       = width;
    scr->base.height      = height;
    scr->base.color_bits  = 1;
    scr->base.dir_x       = DgxScreenLeftRight;
    scr->base.dir_y       = DgxScreenTopBottom;
    scr->base.swap_xy     = false;
    scr->v_array          = heap_caps_calloc(1, ((scr->base.height + 7) / 8) * scr->base.width, MALLOC_CAP_DMA);
    if (!scr->v_array) {
        free(scr);
        ESP_LOGE(TAG, "Impossible to allocate memory for BW virtual screen buffer");
        return 0;
    }
    dgx_scr_init_slow_bus_optimized_funcs(&scr->base);
    scr->base.set_area       = dgx_bw_set_area;
    scr->base.set_pixel      = dgx_bw_set_pixel;
    scr->base.get_pixel      = dgx_bw_get_pixel;
    scr->base.write_area     = dgx_bw_write_data;
    scr->base.wait_buffer    = dgx_bw_wait_data;
    scr->base.fill_rectangle = dgx_bw_fill_rectangle;
    scr->base.update_screen  = dgx_bw_update_screen;
    scr->base.destroy        = dgx_bw_destroy_screen;
    dgx_bw_set_area((dgx_screen_t *)scr, 0, width - 1, 0, height - 1);
    return (dgx_screen_t *)scr;
}
