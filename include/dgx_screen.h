#pragma once
#ifdef __cplusplus
// @formatter:off
extern "C"
{
// @formatter:on
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"

struct _dgx_screen_t;

typedef void (*dgx_scr_set_area_func)(struct _dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom);
typedef void (*dgx_scr_update_area_func)(struct _dgx_screen_t *scr, int left, int right, int top, int bottom);
typedef void (*dgx_scr_write_area_func)(struct _dgx_screen_t *scr, uint8_t *data, uint32_t lenbits);
typedef uint32_t (*dgx_scr_read_area_func)(struct _dgx_screen_t *scr, uint8_t *data, uint32_t lenbits);
typedef void (*dgx_scr_draw_pixel_func)(struct _dgx_screen_t *scr, int x, int y, uint32_t color);
typedef uint32_t (*dgx_scr_read_pixel_func)(struct _dgx_screen_t *scr, int x, int y);
typedef void (*dgx_scr_screen_wait_buffer_func)(struct _dgx_screen_t *scr);
typedef void (*dgx_scr_screen_destroy_func)(struct _dgx_screen_t **pscr);
// typedef void (*dgx_scr_wait_buffer_func)(struct _dgx_screen_t *scr);

/* optimized functions <!-- */
typedef void (*dgx_scr_fill_rectangle_func)(struct _dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color);
typedef void (*dgx_scr_line_func)(struct _dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color);
typedef void (*dgx_scr_circle_func)(struct _dgx_screen_t *scr, int x, int y, int r, uint32_t color);
typedef void (*dgx_scr_solid_circle_func)(struct _dgx_screen_t *scr, int x, int y, int r, uint32_t color);
/* optimized functions --> */

typedef enum
{                            //
    DgxScreenBottomTop = -1, //
    DgxScreenTopBottom = 1,  //
    DgxScreenLeftRight = 1,  //
    DgxScreenRightLeft = -1  //
} dgx_orientation_t;

typedef enum
{
    DgxOutputNormal = 0,
    DgxOutputMirrorX,
    DgxOutputMirrorY,
    DgxOutputRotate180,
    DgxOutputTranspose,
    DgxOutputRotate90CCW,
    DgxOutputRotate90CW,
    DgxOutputTransverse
} dgx_output_orientation_t;

static inline dgx_output_orientation_t dgx_output_orientation_make(dgx_orientation_t xdir, dgx_orientation_t ydir, bool swap_xy)
{
    if (!swap_xy) {
        if (xdir == DgxScreenLeftRight) {
            return ydir == DgxScreenTopBottom ? DgxOutputNormal : DgxOutputMirrorY;
        }
        return ydir == DgxScreenTopBottom ? DgxOutputMirrorX : DgxOutputRotate180;
    }
    if (xdir == DgxScreenLeftRight) {
        return ydir == DgxScreenTopBottom ? DgxOutputTranspose : DgxOutputRotate90CW;
    }
    return ydir == DgxScreenTopBottom ? DgxOutputRotate90CCW : DgxOutputTransverse;
}

static inline dgx_orientation_t dgx_output_orientation_xdir(dgx_output_orientation_t orientation)
{
    switch (orientation) {
    case DgxOutputNormal:
    case DgxOutputMirrorY:
    case DgxOutputTranspose:
    case DgxOutputRotate90CW:
        return DgxScreenLeftRight;
    default:
        return DgxScreenRightLeft;
    }
}

static inline dgx_orientation_t dgx_output_orientation_ydir(dgx_output_orientation_t orientation)
{
    switch (orientation) {
    case DgxOutputNormal:
    case DgxOutputMirrorX:
    case DgxOutputTranspose:
    case DgxOutputRotate90CCW:
        return DgxScreenTopBottom;
    default:
        return DgxScreenBottomTop;
    }
}

static inline bool dgx_output_orientation_swap_xy(dgx_output_orientation_t orientation)
{
    switch (orientation) {
    case DgxOutputTranspose:
    case DgxOutputRotate90CCW:
    case DgxOutputRotate90CW:
    case DgxOutputTransverse:
        return true;
    default:
        return false;
    }
}

typedef enum
{                 //
    DgxScreenRGB, //
    DgxScreenBGR  //
} dgx_color_order_t;

typedef enum
{                             //
    DgxPhysicalScreenWithBus, //
    DgxVirtualScreen,         //
    DgxVirtualBackScreen      //
} dgx_screen_subtype_t;

typedef struct _dgx_screen_t
{
    const char          *screen_name;
    int                  screen_submodel;
    dgx_screen_subtype_t screen_subtype;
    dgx_orientation_t    dir_x;
    dgx_orientation_t    dir_y;
    bool                 swap_xy;
    int                  width;
    int                  height;
    uint8_t              color_bits;
    dgx_color_order_t    rgb_order;
    /*
     * Nesting counter used to defer intermediate update_screen() calls while
     * a larger draw operation is still in progress. 0 means a top-level draw
     * may flush immediately; positive values mean flush is deferred until the
     * counter returns to 0. Prefer dgx_screen_progress_up/down helpers over
     * modifying this field directly.
     */
    int32_t              in_progress;
    /* Pending dirty area, right/bottom exclusive; empty when dirty_right <= dirty_left. */
    int16_t              dirty_left;
    int16_t              dirty_top;
    int16_t              dirty_right;
    int16_t              dirty_bottom;
    uint16_t             cg_row_shift;
    uint16_t             cg_col_shift;
    uint8_t             *draw_buffer;
    uint32_t             draw_buffer_len;
    /* basic functions */
    dgx_scr_set_area_func           set_area;
    dgx_scr_write_area_func         write_area;
    dgx_scr_read_area_func          read_area;
    dgx_scr_screen_wait_buffer_func wait_buffer;
    dgx_scr_draw_pixel_func         set_pixel;
    dgx_scr_read_pixel_func         get_pixel;
    dgx_scr_update_area_func        update_screen;
    dgx_scr_screen_destroy_func     destroy;
    /* optimized functions */
    dgx_scr_fill_rectangle_func fill_rectangle;
    dgx_scr_line_func           draw_line;
    dgx_scr_circle_func         circle;
    dgx_scr_solid_circle_func   solid_circle;
} dgx_screen_t;

typedef struct dgx_point_2d
{
    int16_t x;
    int16_t y;
} dgx_point_2d_t;

dgx_point_2d_t _dgx_start_area_pixel( //
    int               left,           //
    int               right,          //
    int               top,            //
    int               bottom,         //
    dgx_orientation_t dir_x,          //
    dgx_orientation_t dir_y           //
);

dgx_point_2d_t _dgx_move_to_next_area_pixel( //
    dgx_point_2d_t    current_point,         //
    int               left,                  //
    int               right,                 //
    int               top,                   //
    int               bottom,                //
    dgx_orientation_t dir_x,                 //
    dgx_orientation_t dir_y,                 //
    bool              swap_xy                //
);

/**
 * @brief Destroy a screen allocated by a driver or virtual-screen constructor.
 * @param _pscr In/out pointer to the screen pointer. Set to NULL on return.
 */
static inline void dgx_screen_destroy(dgx_screen_t **_pscr)
{                                      //
    if (*_pscr && (*_pscr)->destroy) { //
        (*_pscr)->destroy(_pscr);      //
    } //
}

/**
 * @brief Increment the nesting counter that defers flushes.
 *
 * Everything drawn until the matching dgx_screen_progress_down() is collected
 * into one dirty area and sent with a single update_screen() call.
 *
 * @param _scr Screen whose batching depth is being increased.
 * @return New nesting depth.
 */
static inline int dgx_screen_progress_up(dgx_screen_t *_scr)
{                               //
    return ++_scr->in_progress; //
}

/**
 * @brief Send the pending dirty area with update_screen() and clear it.
 * @param _scr Screen to flush.
 */
static inline void dgx_screen_flush(dgx_screen_t *_scr)
{
    if (_scr->dirty_right <= _scr->dirty_left) return;
    int left = _scr->dirty_left, right = _scr->dirty_right - 1;
    int top = _scr->dirty_top, bottom = _scr->dirty_bottom - 1;
    _scr->dirty_left = _scr->dirty_right = _scr->dirty_top = _scr->dirty_bottom = 0;
    if (_scr->update_screen) _scr->update_screen(_scr, left, right, top, bottom);
}

/**
 * @brief Take the pending dirty area and clear it, without update_screen().
 *
 * For a screen that is shown by its owner, such as a virtual screen copied
 * to a display: open a batch with dgx_screen_progress_up(), draw, take the
 * area that has changed and send only it. Outside a batch there is nothing
 * to take, since every change is flushed at once. After it the closing
 * dgx_screen_progress_down() has nothing left to send.
 *
 * @param _scr          Screen that was drawn on.
 * @param left,top      Out: the top-left corner of the area.
 * @param width,height  Out: its size.
 * @return false when nothing has changed; the outputs are zeros then.
 */
static inline bool dgx_screen_take_dirty(dgx_screen_t *_scr, int *left, int *top, int *width, int *height)
{
    bool dirty = _scr->dirty_right > _scr->dirty_left;
    *left   = dirty ? _scr->dirty_left : 0;
    *top    = dirty ? _scr->dirty_top : 0;
    *width  = dirty ? _scr->dirty_right - _scr->dirty_left : 0;
    *height = dirty ? _scr->dirty_bottom - _scr->dirty_top : 0;
    _scr->dirty_left = _scr->dirty_right = _scr->dirty_top = _scr->dirty_bottom = 0;
    return dirty;
}

/**
 * @brief Mark an area as changed (inclusive bounds, clipped to the screen).
 *
 * Outside a batch the area is flushed at once; inside it is merged into the
 * pending dirty area.
 *
 * @param _scr Screen that was drawn on.
 */
static inline void dgx_screen_touch(dgx_screen_t *_scr, int left, int right, int top, int bottom)
{
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= _scr->width) right = _scr->width - 1;
    if (bottom >= _scr->height) bottom = _scr->height - 1;
    if (left <= right && top <= bottom) {
        if (_scr->dirty_right <= _scr->dirty_left) {
            _scr->dirty_left   = (int16_t)left;
            _scr->dirty_top    = (int16_t)top;
            _scr->dirty_right  = (int16_t)(right + 1);
            _scr->dirty_bottom = (int16_t)(bottom + 1);
        } else {
            if (left < _scr->dirty_left) _scr->dirty_left = (int16_t)left;
            if (top < _scr->dirty_top) _scr->dirty_top = (int16_t)top;
            if (right + 1 > _scr->dirty_right) _scr->dirty_right = (int16_t)(right + 1);
            if (bottom + 1 > _scr->dirty_bottom) _scr->dirty_bottom = (int16_t)(bottom + 1);
        }
    }
    if (!_scr->in_progress) dgx_screen_flush(_scr);
}

/**
 * @brief Decrement the nesting counter; at 0 flush the pending dirty area.
 *
 * @param _scr Screen whose batching depth is being decreased.
 * @return New nesting depth.
 */
static inline int dgx_screen_progress_down(dgx_screen_t *_scr)
{
    int depth = --_scr->in_progress;
    if (depth <= 0) {
        _scr->in_progress = 0; // unbalanced down: recover instead of deferring forever
        depth             = 0;
        dgx_screen_flush(_scr);
    }
    return depth;
}

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on

#endif
