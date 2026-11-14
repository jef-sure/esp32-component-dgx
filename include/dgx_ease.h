#pragma once
/*
 * dgx_ease.h
 *
 *  How far an animation has got by the clock, and the easing of that
 *  progress. Both kinds of morph take such a t from 0 to 1: the dot morphs
 *  of dgx_morph.h and the handwriting morphs of dgx_hw_font.h, which include
 *  this header. The functions are inline: they are there with any build
 *  options, also without CONFIG_DGX_ENABLE_MORPH.
 *
 *      int64_t t0 = esp_timer_get_time();
 *      float   t;
 *      do {
 *          t = dgx_morph_progress(t0, esp_timer_get_time(), 1000000);
 *          draw(dgx_morph_ease(DGX_MORPH_SMOOTHSTEP, t));
 *      } while (t < 1.0f);
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @return (now - start) / duration clamped to [0, 1]; 1 if duration <= 0. */
static inline float dgx_morph_progress(int64_t start_us, int64_t now_us, int64_t duration_us)
{
    if (duration_us <= 0) return 1.0f;
    float t = (float)(now_us - start_us) / (float)duration_us;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

typedef enum {
    DGX_MORPH_LINEAR = 0,
    DGX_MORPH_EASE_IN_OUT,    ///< 2t² / 1-2(1-t)²
    DGX_MORPH_EASE_IN_OUT_3,  ///< cubic variant
    DGX_MORPH_SMOOTHSTEP,     ///< t²(3-2t)
} dgx_morph_easing_t;

/** @return t, a progress from 0 to 1, eased: slow at both ends for every easing but the linear one. */
static inline float dgx_morph_ease(dgx_morph_easing_t easing, float t)
{
    switch (easing) {
    case DGX_MORPH_EASE_IN_OUT:
        return t < 0.5f ? t * t * 2.0f : 1.0f - (1.0f - t) * (1.0f - t) * 2.0f;
    case DGX_MORPH_EASE_IN_OUT_3:
        return t < 0.5f ? t * t * t * 4.0f : 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t) * 4.0f;
    case DGX_MORPH_SMOOTHSTEP:
        return t * t * (3.0f - 2.0f * t);
    case DGX_MORPH_LINEAR:
    default:
        return t;
    }
}

#ifdef __cplusplus
}
#endif
