#pragma once
/*
 * The clip of a screen as the drawing code reads it: left and top are the
 * first column and row that may change, right and bottom one past the last.
 * A screen with no clip has 0, 0, width, height; an empty clip has right 0
 * or bottom 0. The pitch of the memory of a screen stays its width.
 */
#include <stdbool.h>

#include "dgx_screen.h"

static inline int dgx_clip_left(const dgx_screen_t *scr)
{
    return scr->clip_left;
}

static inline int dgx_clip_top(const dgx_screen_t *scr)
{
    return scr->clip_top;
}

static inline int dgx_clip_right(const dgx_screen_t *scr)
{
    return scr->width - scr->clip_right_inset;
}

static inline int dgx_clip_bottom(const dgx_screen_t *scr)
{
    return scr->height - scr->clip_bottom_inset;
}

/* the pixel may change */
static inline bool dgx_clip_has(const dgx_screen_t *scr, int x, int y)
{
    return x >= scr->clip_left && y >= scr->clip_top && x < scr->width - scr->clip_right_inset && y < scr->height - scr->clip_bottom_inset;
}

/* nothing of the rectangle, both ends included, may change */
static inline bool dgx_clip_misses(const dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    return right < scr->clip_left || bottom < scr->clip_top || left >= scr->width - scr->clip_right_inset ||
           top >= scr->height - scr->clip_bottom_inset;
}

/* all of the rectangle, both ends included, may change */
static inline bool dgx_clip_holds(const dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    return left >= scr->clip_left && top >= scr->clip_top && right < scr->width - scr->clip_right_inset &&
           bottom < scr->height - scr->clip_bottom_inset;
}

/* the whole screen may change */
static inline bool dgx_clip_is_whole(const dgx_screen_t *scr)
{
    return !(scr->clip_left | scr->clip_top | scr->clip_right_inset | scr->clip_bottom_inset);
}

/*
 * The area marked as changed, as dgx_screen_touch() marks it, for an area
 * that is already inside the clip: the cut is left out. The pixels and the
 * rectangles of the screens come here, every one of them; the area is not
 * empty.
 */
static inline void dgx_screen_touch_inside(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    if (scr->dirty_right <= scr->dirty_left) {
        scr->dirty_left   = (int16_t)left;
        scr->dirty_top    = (int16_t)top;
        scr->dirty_right  = (int16_t)(right + 1);
        scr->dirty_bottom = (int16_t)(bottom + 1);
    } else {
        if (left < scr->dirty_left) scr->dirty_left = (int16_t)left;
        if (top < scr->dirty_top) scr->dirty_top = (int16_t)top;
        if (right + 1 > scr->dirty_right) scr->dirty_right = (int16_t)(right + 1);
        if (bottom + 1 > scr->dirty_bottom) scr->dirty_bottom = (int16_t)(bottom + 1);
    }
    if (!scr->in_progress) dgx_screen_flush(scr);
}
