#pragma once
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "dgx_screen.h"

/**
 * @brief Compose two child screens into one logical screen, side by side.
 *
 * Drawing on the returned screen dispatches to the left or right child based on
 * the X coordinate, presenting both panels as a single wider surface.
 *
 * @param left_screen  Screen mapped to the left half.
 * @param right_screen Screen mapped to the right half.
 * @return Composite screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_vscreen_2h_init(dgx_screen_t *left_screen, dgx_screen_t *right_screen);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

