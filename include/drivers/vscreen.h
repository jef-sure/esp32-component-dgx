#pragma once
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "dgx_screen.h"

typedef struct _dgx_vscreen_t {
    dgx_screen_t base;
    uint8_t *v_array;
    struct {
        uint16_t left;
        uint16_t right;
        uint16_t top;
        uint16_t bottom;
        uint16_t x_offset;
        uint16_t y_offset;
    } area;
} dgx_vscreen_t;

/**
 * @brief Create a color RAM-backed virtual screen.
 * @param width      Width in pixels.
 * @param height     Height in pixels.
 * @param color_bits Pixel depth in bits: 1, 4, 8, 12, 16, 18, 24 or 32.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @return Newly allocated screen, or NULL on failure: no memory, a depth of other bits than 1, 4, 8, 12, 16, 18, 24 or 32,
 *         or a size with no pixels. Its bytes are to fit in 32 bits. Free with dgx_screen_destroy().
 */
dgx_screen_t* dgx_vscreen_init(int width, int height, uint8_t color_bits, dgx_color_order_t cbo);

/**
 * @brief Create a virtual screen over memory the program owns.
 *
 * The same screen as from dgx_vscreen_init(), with its pixels in @p v_array:
 * a camera frame, a decoded picture, a buffer of another library. Nothing is
 * copied, and dgx_screen_destroy() leaves the memory alone. It must outlive
 * the screen.
 *
 * The memory has to be laid out the way a virtual screen keeps its pixels:
 * rows one right after another, with no padding even when a pixel is smaller
 * than a byte, and the high byte of a pixel first. For RGB565 that is the
 * order of the wire, LVGL's LV_COLOR_FORMAT_RGB565_SWAPPED. It has to be RAM
 * aligned as its pixels are, because a display may read it by DMA, and hold
 * dgx_color_points_to_bytes(color_bits, width * height) bytes.
 *
 * When the rows of the memory are longer than the picture, give the length of
 * a row in pixels as @p width and take the picture with
 * dgx_vscreen_region_to_screen().
 *
 * @param v_array    The pixels.
 * @param width      Width in pixels, also the step from one row to the next.
 * @param height     Height in pixels.
 * @param color_bits Pixel depth in bits, as for dgx_vscreen_init().
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @return Newly allocated screen, or NULL on failure, also when @p v_array is NULL, or the depth or the size is one
 *         dgx_vscreen_init() refuses. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_vscreen_from_array(void *v_array, int width, int height, uint8_t color_bits, dgx_color_order_t cbo);

/**
 * @brief Point a screen of dgx_vscreen_from_array() at other memory.
 *
 * The next drawing or transfer uses @p pixels, under the rules of
 * dgx_vscreen_from_array(). The depth and the color order stay. A transfer
 * keeps no hold on its source, so this may follow it at once, whatever the bus,
 * and the old memory is free to be used again. The window of write_area() goes
 * back to the pixel (0, 0) and nothing is left dirty, as on a new screen.
 *
 * Costs a few assignments, so a screen can be made once and pointed at every
 * band a renderer gives.
 *
 * An error is logged and nothing changes for a screen of any other kind, and
 * for one laid over a display by dgx_overlay_set(), or with a layer over it:
 * dgx_overlay_clear() it first, and the place the layer changed is forgotten
 * here with the rest. The same for NULL or a size with no pixels.
 *
 * @param scr     A screen of dgx_vscreen_from_array().
 * @param pixels  The pixels.
 * @param width   Width in pixels, also the step from one row to the next.
 * @param height  Height in pixels.
 */
void dgx_vscreen_array_set(dgx_screen_t *scr, void *pixels, int width, int height);

/**
 * @brief Tell whether a screen keeps its pixels in the linear dgx_vscreen_t layout.
 *
 * True for dgx_vscreen_init() and dgx_vscreen_from_array() screens and the
 * drivers built on them (ST7920, vscreen_2h). False for hardware panels and
 * for the page-organised black-and-white screens (SSD1306, ST7565R), although
 * those are also DgxVirtualBackScreen: their v_array must not be accessed as
 * a dgx_vscreen_t.
 */
bool dgx_vscreen_is_linear(const dgx_screen_t *scr);

/**
 * @brief Allocate a new virtual screen with the same geometry and a copy of the pixels.
 * @param _scr_src Source virtual screen.
 * @return Cloned screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_vscreen_clone(dgx_screen_t *_scr_src);

/**
 * @brief Read a rectangle of a screen as 32-bit colors 0xAARRGGBB, alpha 0xff: for a window on a PC.
 *
 * The rows go to @p dst one under another, @p dst_stride pixels apart:
 * the layout of SDL_PIXELFORMAT_ARGB8888. Each channel is widened to 8
 * bits with its high bits repeated below, so white is 0xffffffff at any
 * depth. A pixel of 1 bit is white or black. A pixel of 8 bits is an index
 * into @p lut, the RGB565 table dgx_vscreen8_to_screen16() takes, or a
 * level of grey when @p lut is NULL. The overlay layer of the screen is not
 * mixed in.
 *
 * A virtual screen is read straight from its memory; any other screen that
 * gives its pixels back (dgx_screen_is_readable()) pixel by pixel.
 *
 * @param scr           The screen.
 * @param x,y           Top-left corner of the rectangle.
 * @param width,height  Size of the rectangle; it must lie within the screen, and x + width and y + height fit in an int.
 * @param lut           256-entry RGB565 table for an 8-bit screen, or NULL.
 * @param dst           Where the colors go.
 * @param dst_stride    Pixels from the start of a row of @p dst to the start of the next, at least @p width.
 * @return false, and nothing written, when the rectangle is not within the screen or the screen does not give its
 *         pixels back.
 */
bool dgx_vscreen_to_argb8888(dgx_screen_t *scr, int x, int y, int width, int height, const uint16_t *lut, uint32_t *dst,
                             int dst_stride);

/*
 * How a virtual screen is put on a screen: the last argument of
 * dgx_vscreen_to_screen(), dgx_vscreen_region_to_screen(),
 * dgx_vscreen8_to_screen16() and dgx_vscreen8_region_to_screen16(), 0 or
 * these added together. Every transfer takes every flag but
 * DGX_VSCREEN_MASK, which is for a table of colors; DGX_VSCREEN_EXACT
 * changes a transfer of 16 bits only together with DGX_VSCREEN_ALPHA.
 */

/**
 * A pixel of value 0, an index 0 through a table, is not put: what the
 * destination has there stays. `true` is this flag. Smoothed, it is the
 * pixels of the larger picture that are left out where they are 0: 0 is
 * smoothed as any other value, so the outline of a sprite against its
 * holes is smoothed too, and a corner next to a hole may become one.
 */
#define DGX_VSCREEN_TRANSPARENT 1u
/**
 * Through a table: the colors of the table and no others. Without it a
 * display that has hardware for it may be given a table of two colors mixed
 * by that hardware, faster and not to the last bit; see
 * dgx_vscreen8_to_screen16(). With DGX_VSCREEN_ALPHA a DSI display mixes
 * the picture, through a table or of 16 bits, with its frame buffer by the
 * processor, to the last bit, instead of by the accelerator of the
 * ESP32-P4, which may give red, green or blue ±1 off, rarely seen by the
 * eye. It changes nothing where there is no such hardware.
 */
#define DGX_VSCREEN_EXACT 2u
/**
 * With DGX_VSCREEN_SCALE(2), (3) or (4), the picture is made larger by
 * Scale2x, by Scale3x, or by Scale2x twice: a corner of a square where two
 * equal neighbours meet, and the other two differ, takes their pixel, so the
 * steps of a slanted edge are filled in. Pixels are equal when their values
 * are: indices, not their colors, through a table. The smoothing looks past
 * the region into the source for the neighbours of its edge, so a region
 * gives the pixels the whole source gives there; past the edges of the
 * source its last pixel goes on. At any other scale it changes nothing.
 */
#define DGX_VSCREEN_SMOOTH 4u
/**
 * The same by Scale2xSFX and Scale3xSFX (Sp00kyFox, 2015), DGX_VSCREEN_SMOOTH
 * and a bit of its own: a corner is not
 * filled where its diagonal through the pixel goes on as a thin line of
 * one pixel, which plain Scale2x thickens with lumps, and Scale3xSFX fills
 * the corners of edges at 45 degrees more evenly. It looks two pixels
 * around and costs more work a pixel.
 */
#define DGX_VSCREEN_SMOOTH_SFX 0x0cu
/**
 * Every pixel of the source as a square of @p n by @p n pixels of the
 * destination, @p n from 1 to 255; 0 and 1 are the same as no scale, and
 * more than 255 is 255. A 320x200 picture of a game is shown on a 1280x800
 * display with DGX_VSCREEN_SCALE(4). The pixels are repeated, not mixed.
 * The macro evaluates @p n twice: pass a value, not an expression with side
 * effects such as `s++`.
 */
#define DGX_VSCREEN_SCALE(n) ((uint32_t)(n) > 255u ? 0xff00u : ((uint32_t)(n) & 0xffu) << 8)
/**
 * The picture turned or mirrored on the destination, @p o a
 * dgx_output_orientation_t: with DgxOutputRotate90CW the left column of the
 * source is the top row of what it puts. Scaling and smoothing are done
 * before the turn and look the same turned.
 */
#define DGX_VSCREEN_ORIENTED(o) (((uint32_t)(o) & 7u) << 16)
/**
 * The picture over what lies under it by @p a, 0 to 255: 255 the picture
 * alone, as without the flag, 128 half and half, 0 nothing of it. Each
 * channel of a pixel is mixed in 32 steps, (picture × w + under × (32 − w))
 * / 32 with w = a × 32 / 255 rounded, as the overlay layer mixes. Under it
 * is what the destination holds: a virtual screen and a display that can be
 * read, a DSI one, give their pixels, so a picture put by steps of @p a over
 * another fades into it. A display behind a bus that cannot be read has
 * black under it: the picture fades to black and from it. A screen of one
 * bit takes the picture from 128 on. Through a table, the colors of the
 * table are mixed, not the indices. A DSI display of 16 bits mixes a picture
 * through a table, or of 16 bits, with its frame buffer by bands: by the
 * accelerator, which may give red, green or blue ±1 off the formula, or, with
 * DGX_VSCREEN_EXACT or under an overlay layer, by the processor to the last
 * bit. A transparent picture, a smoothed one of 16 bits, and any picture on
 * a DSI display of 24 bits are mixed pixel by pixel, which is slow. A pixel
 * under an overlay layer is read with the layer in it, and the layer is mixed
 * over the result again: put the layer away for a crossfade under it. @p a is
 * evaluated once.
 */
#define DGX_VSCREEN_ALPHA(a) (((255u - ((uint32_t)(a) & 0xffu)) & 0xffu) << 24)
/**
 * Through a table only: the 8-bit picture is a mask. A pixel of it is how
 * much of lut[255] goes over what lies under it, 0 none, 255 all, in 32
 * steps as DGX_VSCREEN_ALPHA mixes, and with DGX_VSCREEN_ALPHA(a) the two
 * are multiplied: a soft shape drawn once into an 8-bit screen, by the soft
 * pen, the soft circle or soft text, is put on any picture. It takes a
 * virtual screen and a display that can be read, a DSI one; on a display
 * behind a bus that cannot be read the flag changes nothing, and the picture
 * goes through the table as it does without it, so give it a table from the
 * paper to the ink there, dgx_lut_gradient(). The indices are made larger,
 * smoothed and turned as the other flags ask, and the overlay layer is mixed
 * over the result.
 */
#define DGX_VSCREEN_MASK 0x10u

/* The names of these flags in release 0.6, when only the transfers through a table had them. */
#define DGX_LUT_TRANSPARENT DGX_VSCREEN_TRANSPARENT
#define DGX_LUT_EXACT       DGX_VSCREEN_EXACT
#define DGX_LUT_SCALE(n)    DGX_VSCREEN_SCALE(n)
#define DGX_LUT_SMOOTH      DGX_VSCREEN_SMOOTH

/**
 * @brief Put a virtual screen on a screen of the same depth: a display, or another virtual screen.
 *
 * The source goes with its corner at (@p x_dst, @p y_dst), cut by the
 * destination. With DGX_VSCREEN_SCALE(), DGX_VSCREEN_SMOOTH and
 * DGX_VSCREEN_ORIENTED() it is made larger and turned, and it takes the
 * rectangle it then is; with DGX_VSCREEN_TRANSPARENT its pixels of value 0
 * are not put.
 *
 * A display behind a bus gets the pixels through one window when nothing is
 * transparent and a pixel takes whole bytes; a transparent picture goes by
 * the runs of pixels it puts, a window each, and 12-bit pixels go one by
 * one. A display whose bus takes rows of memory, a DSI display, gets
 * an upright picture as it is so. The overlay layer of a 16-bit
 * destination, dgx_overlay.h, is mixed in on the way.
 *
 * @param _scr_dst     Destination screen, of the depth of the source.
 * @param x_dst,y_dst  Where the corner of the source goes.
 * @param _scr_src     Source virtual screen.
 * @param flags        0, or DGX_VSCREEN_TRANSPARENT, DGX_VSCREEN_SCALE(n), DGX_VSCREEN_SMOOTH or DGX_VSCREEN_SMOOTH_SFX and
 *                     DGX_VSCREEN_ORIENTED(o) added together.
 */
void dgx_vscreen_to_screen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, uint32_t flags);

/**
 * @brief Put a rectangular region of a virtual screen on a screen of the same depth.
 *
 * The same as dgx_vscreen_to_screen() for a part of the source, to send
 * only what has changed: the pixels it puts are those the whole transfer
 * puts in the same place. (@p x_dst, @p y_dst) is where the corner of the
 * region goes, made larger and turned as the flags ask, not of the whole
 * source: a region at (x_src, y_src) of a source shown upright at (x, y) goes
 * to (x + n * x_src, y + n * y_src) with DGX_VSCREEN_SCALE(n). A region cut
 * by the source puts what is left of it where the whole region would put
 * it. Nothing is done when nothing is left of it.
 *
 * @param _scr_dst         Destination screen, of the depth of the source.
 * @param x_dst,y_dst      Where the corner of the region goes.
 * @param _scr_src         Source virtual screen.
 * @param x_src,y_src      Top-left corner of the region in the source.
 * @param width,height     Size of the region in the source.
 * @param flags            As for dgx_vscreen_to_screen().
 */
void dgx_vscreen_region_to_screen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width, int height,
                                  uint32_t flags);

/**
 * @brief Expand an 8-bit indexed virtual screen into a 16-bit destination through a LUT.
 *
 * The same as dgx_vscreen_to_screen(), every index put as its color from
 * the table; DGX_VSCREEN_TRANSPARENT leaves out index 0.
 *
 * A display whose bus has faster ways than its window is served by them,
 * and the pixels are those of the table. A DSI display of an ESP32-P4 makes
 * the colors, the squares and the smoothing by the processor straight in the
 * order of its panel and moves them in by the 2D DMA, the same way however
 * the screen lies on the panel and however the picture is turned: such a
 * transfer does not go pixel by pixel through the window.
 *
 * One table is an exception: two colors and what lies between them, as a
 * program makes for a screen a pen with a soft edge draws on, from the color
 * of the paper at 0 to the color of the ink at 255. A bus with hardware that
 * mixes two colors, a DSI display of an ESP32-P4 with two frame buffers,
 * mixes them itself by the bytes of the screen then, and the processor
 * carries no pixel. The first color and the last one come out as the table
 * has them; a color between them may differ from the table by ±1 in red,
 * green or blue: 3.2% of the range of red or blue, 1.6% of green, rarely
 * seen by the eye. A table is taken for such when none of its colors is
 * further than 1 in red, green or blue from the straight way between its
 * first and its last, nothing is transparent, scaled or turned.
 * DGX_VSCREEN_EXACT asks for the colors of the table whatever it is.
 *
 * @param _scr_dst     Destination 16-bit screen.
 * @param x_dst,y_dst  Where the corner of the source goes.
 * @param _scr_src     Source 8-bit indexed virtual screen.
 * @param lut          256-entry RGB565 lookup table.
 * @param flags        0, or DGX_VSCREEN_TRANSPARENT, DGX_VSCREEN_EXACT, DGX_VSCREEN_SCALE(n), DGX_VSCREEN_SMOOTH or
 *                     DGX_VSCREEN_SMOOTH_SFX and DGX_VSCREEN_ORIENTED(o) added together.
 */
void dgx_vscreen8_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, const uint16_t *lut, uint32_t flags);

/**
 * @brief Expand a rectangular region of an 8-bit indexed virtual screen into a 16-bit destination through a LUT.
 *
 * The same as dgx_vscreen8_to_screen16() for a part of the source, placed
 * as by dgx_vscreen_region_to_screen(). A destination may cut a square of a
 * scaled pixel in the middle.
 *
 * @param _scr_dst         Destination 16-bit screen.
 * @param x_dst,y_dst      Where the corner of the region goes.
 * @param _scr_src         Source 8-bit indexed virtual screen.
 * @param x_src,y_src      Top-left corner of the region in the source.
 * @param width,height     Size of the region in the source.
 * @param lut              256-entry RGB565 lookup table.
 * @param flags            As for dgx_vscreen8_to_screen16().
 */
void dgx_vscreen8_region_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width,
                                     int height, const uint16_t *lut, uint32_t flags);

/**
 * @brief Copy the whole source virtual screen into a same-sized destination.
 * @return true on success, false if geometry/format are incompatible.
 */
bool dgx_vscreen_copy(dgx_screen_t *_scr_dst, dgx_screen_t *_scr_src);

/*
 * The calls of release 0.6 and before, kept for the programs written for
 * them: dgx_vscreen_to_screen() and dgx_vscreen_region_to_screen() without
 * flags get 0, and dgx_vscreen_to_vscreen() is dgx_vscreen_to_screen() with
 * DGX_VSCREEN_TRANSPARENT or without it. The call is chosen by the number of
 * its arguments; any other number does not build.
 */
#define DGX_VSCREEN_PICK5_(_1, _2, _3, _4, _5, _6, _7, _8, _9, name, ...) name
#define DGX_VSCREEN_TO_SCREEN_4_(dst, x, y, src)                          dgx_vscreen_to_screen(dst, x, y, src, 0u)
#define dgx_vscreen_to_screen(...)                                                                                                           \
    DGX_VSCREEN_PICK5_(__VA_ARGS__, dgx_vscreen_to_screen_takes_4_or_5_arguments, dgx_vscreen_to_screen_takes_4_or_5_arguments,              \
                       dgx_vscreen_to_screen_takes_4_or_5_arguments, dgx_vscreen_to_screen_takes_4_or_5_arguments, dgx_vscreen_to_screen,    \
                       DGX_VSCREEN_TO_SCREEN_4_, dgx_vscreen_to_screen_takes_4_or_5_arguments, dgx_vscreen_to_screen_takes_4_or_5_arguments, \
                       dgx_vscreen_to_screen_takes_4_or_5_arguments, dgx_vscreen_to_screen_takes_4_or_5_arguments)(__VA_ARGS__)
#define DGX_VSCREEN_PICK9_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, name, ...) name
#define DGX_VSCREEN_REGION_TO_SCREEN_8_(dst, x, y, src, x_src, y_src, width, height) \
    dgx_vscreen_region_to_screen(dst, x, y, src, x_src, y_src, width, height, 0u)
#define DGX_VSCREEN_WRONG9_ dgx_vscreen_region_to_screen_takes_8_or_9_arguments
#define dgx_vscreen_region_to_screen(...)                                                                                                            \
    DGX_VSCREEN_PICK9_(__VA_ARGS__, DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_,                              \
                       dgx_vscreen_region_to_screen, DGX_VSCREEN_REGION_TO_SCREEN_8_, DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_, \
                       DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_, DGX_VSCREEN_WRONG9_)(__VA_ARGS__)
#define dgx_vscreen_to_vscreen(dst, x, y, src, has_transparency) \
    dgx_vscreen_to_screen(dst, x, y, src, (has_transparency) ? DGX_VSCREEN_TRANSPARENT : 0u)

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

