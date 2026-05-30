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
 * @brief Allocate a new virtual screen with the same geometry and a copy of the pixels.
 * @param _scr_src Source virtual screen.
 * @return Cloned screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_vscreen_clone(dgx_screen_t *_scr_src);

/**
 * @brief Expand an 8-bit indexed virtual screen into a 16-bit destination through a LUT.
 * @param _scr_dst         Destination 16-bit screen.
 * @param x_dst,y_dst      Top-left placement on the destination.
 * @param _scr_src         Source 8-bit indexed virtual screen.
 * @param lut              256-entry RGB565 lookup table.
 * @param has_transparency If true, index 0 is treated as transparent.
 */
void dgx_vscreen8_to_screen16(dgx_screen_t *_scr_dst, int x_dst, int y_dst, dgx_screen_t *_scr_src, uint16_t *lut, bool has_transparency);

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

