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

typedef dgx_output_orientation_t dgx_vscreen_output_orientation_t;

static inline dgx_vscreen_output_orientation_t dgx_vscreen_output_orientation_make(
    dgx_orientation_t xdir, dgx_orientation_t ydir, bool swap_xy)
{
    return dgx_output_orientation_make(xdir, ydir, swap_xy);
}

/**
 * @brief Create a color RAM-backed virtual screen.
 * @param width      Width in pixels.
 * @param height     Height in pixels.
 * @param color_bits Pixel depth in bits (8, 12, 16, 18, 24).
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t* dgx_vscreen_init(int width, int height, uint8_t color_bits, dgx_color_order_t cbo);

/**
 * @brief Tell whether a screen keeps its pixels in the linear dgx_vscreen_t layout.
 *
 * True for dgx_vscreen_init() screens and the drivers built on them (ST7920,
 * vscreen_2h). False for hardware panels and for the page-organised
 * black-and-white screens (SSD1306, ST7565R), although those are also
 * DgxVirtualBackScreen: their v_array must not be accessed as a dgx_vscreen_t.
 */
bool dgx_vscreen_is_linear(const dgx_screen_t *scr);

/**
 * @brief Allocate a new virtual screen with the same geometry and a copy of the pixels.
 * @param _scr_src Source virtual screen.
 * @return Cloned screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_vscreen_clone(dgx_screen_t *_scr_src);

/**
 * @brief How dgx_vscreen8_to_screen16() and dgx_vscreen8_region_to_screen16() use their table: added together.
 *
 * `true` and `false` given here are DGX_LUT_TRANSPARENT and nothing, as
 * they were when this was a bool.
 */
#define DGX_LUT_TRANSPARENT 1u /*!< A pixel of index 0 is not put: what the destination has there stays. */
/**
 * The colors of the table and no others. Without it a display that has
 * hardware for it may be given a table of two colors mixed by that hardware,
 * faster and not to the last bit; see dgx_vscreen8_to_screen16(). It changes
 * nothing where there is no such hardware.
 */
#define DGX_LUT_EXACT 2u
/**
 * Every pixel of the source as a square of @p n by @p n pixels of the
 * destination, @p n from 1 to 255; 0 and 1 are the same as no scale. A
 * 320x200 picture of a game is shown on a 1280x800 display with
 * DGX_LUT_SCALE(4). The pixels are repeated, not mixed.
 */
#define DGX_LUT_SCALE(n) (((uint32_t)(n) & 0xffu) << 8)
/**
 * With DGX_LUT_SCALE(2) or DGX_LUT_SCALE(4), the picture is made larger by
 * Scale2x on the indices, once at 2 and twice at 4: a corner of a square
 * where two equal neighbours meet, and the other two differ, takes their
 * index, so the steps of a slanted edge are filled in. It looks past the
 * region into the source for the neighbours of its edge, so a region gives
 * the pixels the whole source gives there; past the edges of the source its
 * last pixel goes on. At any other scale it changes nothing.
 */
#define DGX_LUT_SMOOTH 4u

/**
 * @brief Expand an 8-bit indexed virtual screen into a 16-bit destination through a LUT.
 *
 * A display whose bus has faster ways than its window is served by them, and
 * the pixels are those of the table. One table is an exception: two colors
 * and what lies between them, as a program makes for a screen a pen with a
 * soft edge draws on, from the color of the paper at 0 to the color of the
 * ink at 255. A bus with hardware that mixes two colors, a DSI display of an
 * ESP32-P4 with two frame buffers, mixes them itself by the bytes of the
 * screen then, and the processor carries no pixel. The first color and the
 * last one come out as the table has them; a color between them may differ
 * from the table by a step of red, green or blue: 3.2% of the range of red
 * or blue, 1.6% of green. A table is taken for such when none of its colors
 * is further than a step from the straight way between its first and its
 * last, nothing is transparent and nothing is scaled. DGX_LUT_EXACT in
 * @p flags asks for the colors of the table whatever it is.
 *
 * DGX_LUT_SCALE(n) in @p flags makes the picture n times as large: the
 * source goes to a rectangle n times as wide and n times as high. A DSI
 * display of an ESP32-P4 makes the colors and the squares by the processor
 * straight in the order of its panel and moves them in by the 2D DMA, the
 * same way in every orientation: a turned screen does not go pixel by pixel
 * through the window.
 *
 * @param _scr_dst     Destination 16-bit screen.
 * @param x_dst,y_dst  Top-left placement on the destination.
 * @param _scr_src     Source 8-bit indexed virtual screen.
 * @param lut          256-entry RGB565 lookup table.
 * @param flags        0, or DGX_LUT_TRANSPARENT, DGX_LUT_EXACT, DGX_LUT_SCALE(n) and DGX_LUT_SMOOTH added together.
 */
void dgx_vscreen8_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, uint16_t *lut, uint32_t flags);

/**
 * @brief Expand a rectangular region of an 8-bit indexed virtual screen into a 16-bit destination through a LUT.
 *
 * The same as dgx_vscreen8_to_screen16() for a part of the source, to send
 * only what has changed: the pixels it puts are those the whole transfer puts
 * in the same place. As with dgx_vscreen_region_to_screen(), @p x_dst and
 * @p y_dst are where the corner of the region goes, not of the whole source:
 * a region taken at (x_src, y_src) of a source shown at (x, y) goes to
 * (x + x_src, y + y_src), and with DGX_LUT_SCALE(n) to
 * (x + n * x_src, y + n * y_src). The region is cut by the source and by the
 * destination; nothing is done when nothing is left of it. A destination
 * may cut a square of a scaled pixel in the middle.
 *
 * @param _scr_dst         Destination 16-bit screen.
 * @param x_dst,y_dst      Where the top-left corner of the region goes on the destination.
 * @param _scr_src         Source 8-bit indexed virtual screen.
 * @param x_src,y_src      Top-left corner of the region in the source.
 * @param width,height     Size of the region in the source.
 * @param lut              256-entry RGB565 lookup table.
 * @param flags            0, or DGX_LUT_TRANSPARENT, DGX_LUT_EXACT, DGX_LUT_SCALE(n) and DGX_LUT_SMOOTH added together.
 */
void dgx_vscreen8_region_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width,
                                     int height, uint16_t *lut, uint32_t flags);

/**
 * @brief Copy the whole source virtual screen into a same-sized destination.
 * @return true on success, false if geometry/format are incompatible.
 */
bool dgx_vscreen_copy(dgx_screen_t *_scr_dst, dgx_screen_t *_scr_src);

/**
 * @brief Blit one virtual screen onto another at the given offset.
 * @param has_transparency If true, the source's transparent color is skipped.
 */
void dgx_vscreen_to_vscreen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, bool has_transparency);

/**
 * @brief Blit one virtual screen onto another with an orientation transform.
 */
void dgx_vscreen_to_vscreen_oriented(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src,
                      bool has_transparency, dgx_output_orientation_t orientation);

/**
 * @brief Push a virtual screen to any destination screen (RAM or hardware panel).
 */
void dgx_vscreen_to_screen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src);

/**
 * @brief Push a rectangular region of a virtual screen to a destination screen.
 */
void dgx_vscreen_region_to_screen(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, int x_src, int y_src, int width, int height);

/**
 * @brief Push a virtual screen to a destination with an orientation transform.
 */
void dgx_vscreen_to_screen_oriented(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src,
                     dgx_output_orientation_t orientation);

/**
 * @brief Push a region of a virtual screen to a destination with an orientation transform.
 */
void dgx_vscreen_region_to_screen_oriented(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src,
                         int x_src, int y_src, int width, int height, dgx_output_orientation_t orientation);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

