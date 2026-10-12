/*
 * fb_flip.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <string.h>

#include "dgx_fb_flip_priv.h"

void dgx_fb_flip_init(dgx_fb_flip_t *flip, const dgx_fb_flip_display_t *display, void *context, uint8_t drawn, bool at_switch, uint32_t guard_us)
{
    memset(flip, 0, sizeof(*flip));
    flip->display   = display;
    flip->context   = context;
    flip->drawn     = drawn;
    flip->at_switch = at_switch;
    flip->guard_us  = guard_us;
}

/* tell the display to read the frame buffer drawing went into */
static void dgx_fb_flip_tell(dgx_fb_flip_t *flip)
{
    uint32_t before, ended, since, period;
    flip->display->frames(flip->context, &before, &since, &period);
    flip->display->show(flip->context, flip->drawn);
    const bool timed = flip->display->frames(flip->context, &ended, &since, &period);
    /*
     * The display takes the frame buffer where a frame ends. Where that
     * is counted, the next one counted is it; one counted while the display
     * was being told makes it wait a frame more than needed, which does no
     * harm. Where frames are counted later, the display told just before
     * that may have started the next frame from the old frame buffer
     * already: then it is the frame after the next.
     */
    const bool late = !flip->at_switch && (!timed || since + flip->guard_us > period);
    flip->flip_at   = ended + (late ? 2u : 1u);
    flip->told_at   = ended;
    flip->told      = true;
    if (!flip->kept) {
        flip->kept     = true;
        flip->ready_at = ended;
    }
    /* a frame that ended meanwhile may have taken the picture: nothing may be drawn into it then */
    if (ended != before) flip->closed = true;
}

bool dgx_fb_flip_draw(dgx_fb_flip_t *flip)
{
    const dgx_fb_flip_display_t *display = flip->display;
    void                        *context = flip->context;
    uint32_t                     ended, since, period;
    if (!flip->told) return false;
    const bool timed = display->frames(context, &ended, &since, &period);
    if ((int32_t)(ended - flip->flip_at) < 0) {
        if (timed && !flip->closed && ended == flip->ready_at && since + flip->batch_us + flip->guard_us < period) {
            /* the display has not taken the picture: it is told to stay where it is, in time for it to be sure */
            uint32_t now;
            display->show(context, flip->drawn ^ 1u);
            display->frames(context, &now, &since, &period);
            if (now == ended && since + flip->guard_us < period) {
                flip->told       = false;
                flip->batch_from = display->now_us(context);
                flip->timed      = true;
                ++flip->joined;
                return false;
            }
            /* a frame ended meanwhile or is about to: which frame buffer it took is not known, so the display is to take the picture */
            dgx_fb_flip_tell(flip);
        }
        const uint32_t from = display->now_us(context);
        ++flip->waits;
        for (;;) {
            display->frames(context, &ended, &since, &period);
            if ((int32_t)(ended - flip->flip_at) >= 0) break;
            if (!display->wait(context)) break; /* no frames are sent: nothing to wait for */
        }
        flip->wait_us += display->now_us(context) - from;
    }
    /* the display reads the picture: drawing goes into the other frame buffer */
    flip->kept_back += flip->told_at - flip->ready_at;
    flip->told   = false;
    flip->kept   = false;
    flip->closed = false;
    flip->drawn ^= 1u;
    ++flip->flips;
    flip->batch_from = display->now_us(context);
    flip->timed      = true;
    return true;
}

void dgx_fb_flip_drawn(dgx_fb_flip_t *flip)
{
    if (flip->timed) {
        /* the longest of the last batches, forgotten by an eighth a batch, but never less than this one took */
        uint32_t took = flip->display->now_us(flip->context) - flip->batch_from;
        if (took > DGX_FB_FLIP_BATCH_US_MOST) took = DGX_FB_FLIP_BATCH_US_MOST;
        const uint32_t less = flip->batch_us - (flip->batch_us >> 3);
        flip->batch_us      = took > less ? took : less;
    }
}

void dgx_fb_flip_finished(dgx_fb_flip_t *flip)
{
    dgx_fb_flip_tell(flip);
}
