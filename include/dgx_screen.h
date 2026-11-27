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
#include "dgx_bits.h"
#include "dgx_deprecated.h"
#include "esp_err.h"

struct _dgx_screen_t;
struct dgx_screen_control_t;

typedef void (*dgx_scr_set_area_func)(struct _dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom);
typedef void (*dgx_scr_update_area_func)(struct _dgx_screen_t *scr, int left, int right, int top, int bottom);
typedef void (*dgx_scr_write_area_func)(struct _dgx_screen_t *scr, uint8_t *data, uint32_t lenbits);
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

/*
 * The way a screen lies on a panel, as its driver takes it: the axes
 * swapped first, the columns of the panel then counted from the right and
 * its rows from the bottom. It is the turn of a transfer that would put the
 * screen onto the panel: with DgxOutputRotate90CW the left column of the
 * screen is the top row of the panel. The three functions after it go back
 * from a turn to the way; text turned by a dgx_output_orientation_t is drawn
 * by them.
 */
static inline dgx_output_orientation_t dgx_output_orientation_make(dgx_orientation_t xdir, dgx_orientation_t ydir, bool swap_xy)
{
    if (!swap_xy) {
        if (xdir == DgxScreenLeftRight) {
            return ydir == DgxScreenTopBottom ? DgxOutputNormal : DgxOutputMirrorY;
        }
        return ydir == DgxScreenTopBottom ? DgxOutputMirrorX : DgxOutputRotate180;
    }
    if (xdir == DgxScreenLeftRight) {
        return ydir == DgxScreenTopBottom ? DgxOutputTranspose : DgxOutputRotate90CCW;
    }
    return ydir == DgxScreenTopBottom ? DgxOutputRotate90CW : DgxOutputTransverse;
}

static inline dgx_orientation_t dgx_output_orientation_xdir(dgx_output_orientation_t orientation)
{
    switch (orientation) {
    case DgxOutputNormal:
    case DgxOutputMirrorY:
    case DgxOutputTranspose:
    case DgxOutputRotate90CCW:
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
    case DgxOutputRotate90CW:
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
     * counter returns to 0. Prefer dgx_screen_batch_begin() and
     * dgx_screen_batch_end() over modifying this field directly.
     */
    int32_t              in_progress;
    /* Pending dirty area, right/bottom exclusive; empty when dirty_right <= dirty_left. */
    int16_t              dirty_left;
    int16_t              dirty_top;
    int16_t              dirty_right;
    int16_t              dirty_bottom;
    /*
     * The clip: drawing changes only the pixels inside it. Left and top are a column and a row of the screen;
     * right and bottom are how many columns and rows lie past it, so that a screen made of zeros has the whole
     * of itself as its clip. Set by dgx_screen_clip() and dgx_screen_unclip(), read by dgx_screen_clip_get().
     */
    int16_t              clip_left;
    int16_t              clip_top;
    int16_t              clip_right_inset;
    int16_t              clip_bottom_inset;
    uint16_t             cg_row_shift;
    uint16_t             cg_col_shift;
    uint8_t             *draw_buffer;
    uint32_t             draw_buffer_len;
    /* basic functions */
    dgx_scr_set_area_func           set_area;
    dgx_scr_write_area_func         write_area;
    union {
        dgx_scr_screen_wait_buffer_func wait; /* called by dgx_screen_wait() */
        dgx_scr_screen_wait_buffer_func wait_buffer DGX_DEPRECATED("use dgx_screen_wait(scr)");
    };
    dgx_scr_draw_pixel_func         set_pixel;
    dgx_scr_read_pixel_func         get_pixel;
    /*
     * Sends what has changed, left .. right and top .. bottom inclusive, to where the screen is shown;
     * dgx_screen_flush() calls it. A program may give its own to a virtual screen it shows itself, one of
     * dgx_vscreen_init() or dgx_vscreen_from_array(), whose own does nothing, and keep what it needs in
     * user_data. A screen of a driver sends its picture to the display by it: do not replace it there.
     * dgx_overlay_set() takes over the one of a layer and calls the one before it after its own work:
     * give yours before that call.
     */
    dgx_scr_update_area_func        update_screen;
    dgx_scr_screen_destroy_func     destroy;
    /* optimized functions */
    dgx_scr_fill_rectangle_func fill_rectangle;
    dgx_scr_line_func           draw_line;
    dgx_scr_circle_func         circle;
    dgx_scr_solid_circle_func   solid_circle;
    /* the overlay layer of the screen, or of the screen this one is the layer of; NULL when none: see dgx_overlay.h */
    struct _dgx_overlay_t *overlay;
    /*
     * The program's own: what its functions given to the screen need, such as an update_screen of its
     * own. DGX neither reads nor writes it, and it is NULL when the screen is made.
     */
    void *user_data;
    /*
     * What the display can be told besides its picture, given by its driver: dgx_screen_orientation(),
     * dgx_screen_backlight(), dgx_screen_display() and dgx_screen_brightness() go through it. NULL for a
     * screen that is no display, a virtual one.
     */
    const struct dgx_screen_control_t *control;
} dgx_screen_t;

/**
 * @brief What a driver does for the calls that control a display, one table per driver.
 *
 * A function the display has no use for is NULL, and the call returns
 * ESP_ERR_NOT_SUPPORTED. A program does not call these: it calls
 * dgx_screen_orientation() and the three after it.
 */
typedef struct dgx_screen_control_t
{
    /** How the screen lies on the panel; the width and height of the screen follow. */
    void (*orientation)(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);
    /** The backlight line on or off; ESP_ERR_NOT_SUPPORTED for a screen made without the line. */
    esp_err_t (*backlight)(dgx_screen_t *scr, bool on);
    /** The picture shown or not, the memory of the display kept. */
    esp_err_t (*display)(dgx_screen_t *scr, bool on);
    /** How bright the pixels of a display that lights them itself are, 0 .. 255. */
    esp_err_t (*brightness)(dgx_screen_t *scr, uint8_t level);
} dgx_screen_control_t;

typedef struct dgx_point_2d
{
    int16_t x;
    int16_t y;
} dgx_point_2d_t;

/* the overlay layer the screen has, or is, let go of: see dgx_overlay.h */
void dgx_overlay_forget(dgx_screen_t *scr);

/**
 * @brief Destroy a screen allocated by a driver or virtual-screen constructor.
 * @param _pscr In/out pointer to the screen pointer. Set to NULL on return.
 */
static inline void dgx_screen_destroy(dgx_screen_t **_pscr)
{
    if (*_pscr && (*_pscr)->overlay) dgx_overlay_forget(*_pscr);
    if (*_pscr && (*_pscr)->destroy) {
        (*_pscr)->destroy(_pscr);
    }
}

/**
 * @brief Turn or mirror the picture of a display: the same call for every driver.
 *
 * The three arguments are those of the driver's own call, dgx_ili9341_orientation()
 * and the like, which stay: the direction of x, the direction of y, and whether
 * the axes are swapped. The width and height of the screen follow. What is on
 * the screen is not redrawn for a color display: turn it before drawing.
 *
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED for a screen that is not turned: a
 *         virtual screen, the ST7565R.
 */
static inline esp_err_t dgx_screen_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    if (!_scr->control || !_scr->control->orientation) return ESP_ERR_NOT_SUPPORTED;
    _scr->control->orientation(_scr, dir_x, dir_y, swap_xy);
    return ESP_OK;
}

/**
 * @brief The backlight on or off, by the line given to the constructor of the display.
 *
 * The line is driven high and low, nothing else: a program that dims the
 * backlight gives GPIO_NUM_NC to the constructor and drives the line by its
 * own PWM.
 *
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED for a screen made without the line
 *         and for a display that has no backlight, an OLED or e-paper.
 */
static inline esp_err_t dgx_screen_backlight(dgx_screen_t *_scr, bool on)
{
    if (!_scr->control || !_scr->control->backlight) return ESP_ERR_NOT_SUPPORTED;
    return _scr->control->backlight(_scr, on);
}

/**
 * @brief The picture shown or not; the display keeps its memory and the screen takes drawing meanwhile.
 *
 * An OLED off lights no pixel, which is what saves it from burn-in. A TFT
 * stops showing its memory, and its backlight stays as it is: turn that off
 * by dgx_screen_backlight().
 *
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED for a screen that has no such
 *         command: a virtual screen, e-paper.
 */
static inline esp_err_t dgx_screen_display(dgx_screen_t *_scr, bool on)
{
    if (!_scr->control || !_scr->control->display) return ESP_ERR_NOT_SUPPORTED;
    return _scr->control->display(_scr, on);
}

/**
 * @brief How bright a display that lights its own pixels is, an OLED: 0 the dimmest it has, 255 the brightest.
 *
 * 0 is dim, not off: dgx_screen_display() turns the picture off. The steps
 * are those of the controller, 256 of the contrast of an SSD1306, SSD1309,
 * SH1106 or SSD1322 and 16 of the master current of an SSD1351.
 *
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED for a display lit from behind,
 *         whose brightness is that of its backlight.
 */
static inline esp_err_t dgx_screen_brightness(dgx_screen_t *_scr, uint8_t level)
{
    if (!_scr->control || !_scr->control->brightness) return ESP_ERR_NOT_SUPPORTED;
    return _scr->control->brightness(_scr, level);
}

/**
 * @brief Wait until the display has taken everything that was sent to it.
 *
 * A bus may still be sending the last pixels when a drawing call returns.
 * Call this before the memory they were sent from is freed or changed, and
 * before timing. Nothing to wait for on a virtual screen.
 */
static inline void dgx_screen_wait(dgx_screen_t *_scr)
{
    if (_scr->wait) _scr->wait(_scr);
}

/**
 * @brief Open a batch: increment the nesting counter that defers flushes.
 *
 * Everything drawn until the matching dgx_screen_batch_end() is collected
 * into one dirty area and sent with a single update_screen() call. Batches
 * nest.
 *
 * @param _scr Screen whose batching depth is being increased.
 * @return New nesting depth.
 */
static inline int dgx_screen_batch_begin(dgx_screen_t *_scr)
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
 * to a display: open a batch with dgx_screen_batch_begin(), draw, take the
 * area that has changed and send only it.
 *
 * The batch must be open from before the first drawing call until the area
 * is taken, around all the drawing, including functions that open batches of
 * their own. Any batch that closes at depth 0 passes the area to
 * update_screen(), and that of a virtual screen does nothing: the area is
 * lost, and there is nothing to take. A program that shows the screen all
 * the time opens one batch before it draws anything and leaves it open, as
 * examples/hw_soft_pen_demo does. One that would rather be called at each
 * change gives the screen an update_screen() of its own, see user_data.
 * After the area is taken, the closing dgx_screen_batch_end() has
 * nothing left to send.
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
 * @brief Let drawing change only the pixels inside a rectangle of the screen.
 *
 * Every call that changes pixels of the screen keeps to it: figures, text,
 * handwriting, pictures and transfers onto the screen. The coordinates stay
 * those of the screen. The rectangle is cut by the screen; one outside it, or
 * one of no width or height, lets nothing be drawn. It takes the place of the
 * one before: to draw inside two, give their common part. A turn of the screen
 * and a new size by dgx_vscreen_array_set() give back the whole screen.
 *
 * What a virtual screen sends to its display is not drawing, and the clip does
 * not cut it.
 *
 * @param _scr Screen to clip.
 * @param x,y  Top-left corner of the rectangle.
 * @param w,h  Its size.
 */
static inline void dgx_screen_clip(dgx_screen_t *_scr, int x, int y, int w, int h)
{
    int right = x + w, bottom = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > _scr->width) right = _scr->width;
    if (bottom > _scr->height) bottom = _scr->height;
    if (x >= right || y >= bottom) x = y = right = bottom = 0;
    _scr->clip_left         = (int16_t)x;
    _scr->clip_top          = (int16_t)y;
    _scr->clip_right_inset  = (int16_t)(_scr->width - right);
    _scr->clip_bottom_inset = (int16_t)(_scr->height - bottom);
}

/**
 * @brief Let drawing change the whole screen again, as it does on a new screen.
 * @param _scr Screen to unclip.
 */
static inline void dgx_screen_unclip(dgx_screen_t *_scr)
{
    _scr->clip_left = _scr->clip_top = _scr->clip_right_inset = _scr->clip_bottom_inset = 0;
}

/**
 * @brief The clip of the screen, to keep it and give it back by dgx_screen_clip() later.
 *
 * @param _scr     Screen whose clip is asked.
 * @param x,y      Out: the top-left corner of the rectangle.
 * @param w,h      Out: its size; 0 when nothing may be drawn.
 */
static inline void dgx_screen_clip_get(const dgx_screen_t *_scr, int *x, int *y, int *w, int *h)
{
    *x = _scr->clip_left;
    *y = _scr->clip_top;
    *w = _scr->width - _scr->clip_right_inset - _scr->clip_left;
    *h = _scr->height - _scr->clip_bottom_inset - _scr->clip_top;
}

/**
 * @brief Mark an area as changed (inclusive bounds, cut by the clip of the screen).
 *
 * Outside a batch the area is flushed at once; inside it is merged into the
 * pending dirty area. Only what lies inside the clip is marked, as drawing
 * changes nothing outside it: to send the rest, unclip the screen first.
 *
 * @param _scr Screen that was drawn on.
 */
static inline void dgx_screen_touch(dgx_screen_t *_scr, int left, int right, int top, int bottom)
{
    if (left < _scr->clip_left) left = _scr->clip_left;
    if (top < _scr->clip_top) top = _scr->clip_top;
    if (right >= _scr->width - _scr->clip_right_inset) right = _scr->width - _scr->clip_right_inset - 1;
    if (bottom >= _scr->height - _scr->clip_bottom_inset) bottom = _scr->height - _scr->clip_bottom_inset - 1;
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
 * @brief Close a batch: decrement the nesting counter; at 0 flush the pending dirty area.
 *
 * @param _scr Screen whose batching depth is being decreased.
 * @return New nesting depth.
 */
static inline int dgx_screen_batch_end(dgx_screen_t *_scr)
{
    int depth = --_scr->in_progress;
    if (depth <= 0) {
        _scr->in_progress = 0; // unbalanced down: recover instead of deferring forever
        depth             = 0;
        dgx_screen_flush(_scr);
    }
    return depth;
}

/**
 * @brief A color of this screen from red, green and blue of 8 bits each, packed as the depth of the screen takes it.
 *
 * For code that does not know the depth it will run at. On a screen of 12,
 * 16, 18 or 24 bits it is what dgx_rgb_to_12() .. dgx_rgb_to_24() give, and
 * on one of 32 bits the color opaque, 0xFFRRGGBB. A screen of fewer bits has
 * no colors and gets the brightness of the color: 0 or 1 on a 1-bit screen,
 * 1 from a mid gray on; one of 16 levels in the high nibble on a 4-bit one,
 * 0x00 .. 0xF0, as it keeps a pixel; 0 .. 255 on an 8-bit one, which suits a
 * table that runs from dark to bright. Every value is one the screen draws
 * as it is. The inks of e-paper are not a brightness: dgx_rgb_to_bwr() gives
 * them.
 */
static inline uint32_t dgx_screen_rgb(const dgx_screen_t *_scr, uint8_t r, uint8_t g, uint8_t b)
{
    switch (_scr->color_bits) {
    case 12:
        return DGX_RGB_12(r, g, b);
    case 16:
        return DGX_RGB_16(r, g, b);
    case 18:
        return DGX_RGB_18(r, g, b);
    case 24:
        return DGX_RGB_24(r, g, b);
    case 32:
        return 0xff000000u | DGX_RGB_24(r, g, b);
    default:
        break;
    }
    const uint32_t gray = (r * 77u + g * 150u + b * 29u) >> 8;
    if (_scr->color_bits == 1) return gray >= 128;
    return _scr->color_bits == 4 ? gray & 0xf0u : gray;
}

/**
 * @deprecated The name dgx_screen_batch_begin() had before.
 */
DGX_DEPRECATED("use dgx_screen_batch_begin()")
static inline int dgx_screen_progress_up(dgx_screen_t *_scr)
{
    return dgx_screen_batch_begin(_scr);
}

/**
 * @deprecated The name dgx_screen_batch_end() had before.
 */
DGX_DEPRECATED("use dgx_screen_batch_end()")
static inline int dgx_screen_progress_down(dgx_screen_t *_scr)
{
    return dgx_screen_batch_end(_scr);
}

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on

#endif
