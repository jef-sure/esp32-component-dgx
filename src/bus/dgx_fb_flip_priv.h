#pragma once
/*
 * dgx_fb_flip_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Two frame buffers that change places: when the display is told to read
 *  the one drawing went into, and when drawing moves to the other one.
 *
 *  The display takes the frame buffer of a frame where the frame before
 *  ends: the one it was last told. A batch of drawing that has ended is a
 *  finished picture, and the display is told to read it. That is a word
 *  that there is something to show, not the showing: until the frame ends
 *  the word can be taken back. So a batch that comes before the display
 *  has taken the picture may go into the same picture: the display is told
 *  to stay with the frame buffer it reads, the batch is drawn, the display
 *  is told again. What several tasks draw in the time of a frame is shown
 *  together, by one change of the frame buffers.
 *
 *  A batch joins a finished picture when both of this hold:
 *
 *   - the frame does not end soon: there is more time left than the
 *     longest of the last batches took to draw, and some to spare. A batch
 *     that is being drawn where a frame ends keeps the whole picture back a
 *     frame;
 *   - the picture has not been kept back over an end of a frame already,
 *     and it is known that the display has not taken it.
 *
 *  A batch that does not join waits until the display has taken the
 *  picture, and goes into the other frame buffer.
 *
 *  Who draws is not asked. A program that draws more often than the
 *  display shows frames gets all its batches into the pictures and sees the
 *  last of them: holding itself back is its own business.
 *
 *  Nothing here depends on the platform: a bus tells how its display is
 *  told and how its frames are counted. The calls of several tasks are to
 *  come one at a time, as all drawing on a screen does.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/** @brief What a bus tells of its display. */
typedef struct _dgx_fb_flip_display_t {
    /** Tell the display which of the two frame buffers to read from the next frame on. */
    void (*show)(void *context, uint8_t frame);
    /**
     * How many frames have ended, how long ago the last one did and how
     * long a frame takes, in microseconds. The count is always given;
     * false when the times are not known. When the count has not changed
     * between two calls, the time of the second one is of the same frame.
     */
    bool (*frames)(void *context, uint32_t *ended, uint32_t *since_us, uint32_t *period_us);
    /** Wait until a frame ends; false when none did in a time many frames long. */
    bool (*wait)(void *context);
    /** The time in microseconds, its low 32 bits. */
    uint32_t (*now_us)(void *context);
} dgx_fb_flip_display_t;

/* a batch that took longer than this counts as this long */
#define DGX_FB_FLIP_BATCH_US_MOST 100000u

/*
 * Where frames are counted where the display takes the frame buffer, the
 * interrupt that counts a frame looks which frame buffer to take a moment
 * before it counts it: a driver sets the next frame going in between. A
 * word given in that moment from another core is too late for the frame,
 * though the count has not changed yet. So a word given this close to where
 * a frame is to end is not relied on there either: the moment is some
 * microseconds, the rest is room for an interrupt that comes late.
 */
#define DGX_FB_FLIP_SWITCH_US 300u

typedef struct _dgx_fb_flip_t {
    const dgx_fb_flip_display_t *display;
    void                        *context;
    uint8_t  drawn;     /* which of the two frame buffers drawing goes into */
    bool     told;      /* the display was told to read `drawn`, and drawing has not moved to the other one */
    bool     kept;      /* a finished picture has not been taken by the display yet, as far as is known */
    bool     closed;    /* and nothing joins it any more */
    bool     at_switch; /* frames are counted where the display takes the frame buffer of the next one, not later */
    bool     timed;     /* `batch_from` is set: a batch has been started here */
    uint32_t guard_us;  /* how close to the end of a frame a word to the display may come too late */
    uint32_t flip_at;   /* the count of frames from which the display surely reads `drawn` */
    uint32_t ready_at;  /* the count when the picture was first finished */
    uint32_t told_at;   /* and when the display was last told of it */
    uint32_t batch_from; /* when the batch being drawn started */
    uint32_t batch_us;  /* how long a batch may take to draw: the longest of the last ones, forgotten by an eighth a batch */
    /* counted since the start; they go round after 2^32 */
    uint32_t flips;     /* times the frame buffers changed places */
    uint32_t joined;    /* batches that went into a picture already finished */
    uint32_t kept_back; /* ends of frames a finished picture was kept back over */
    uint32_t waits;     /* times a batch waited for the display to take a picture */
    uint32_t wait_us;   /* and how long in all */
} dgx_fb_flip_t;

/**
 * @brief Start: the display reads the frame buffer that is not `drawn`, and has been told nothing.
 * @param at_switch Frames are counted where the display takes the frame buffer for the next one, a moment
 *                  later at most, see DGX_FB_FLIP_SWITCH_US. false when they are counted later, up to `guard_us` later.
 * @param guard_us  How close to the end of a frame a word to the display is not relied on.
 */
void dgx_fb_flip_init(dgx_fb_flip_t *flip, const dgx_fb_flip_display_t *display, void *context, uint8_t drawn, bool at_switch, uint32_t guard_us);

/**
 * @brief Before something is drawn while `told` is set: join the finished picture, or wait and move on.
 * @return true when the frame buffers have changed places: `drawn` is the other one, and it lacks what was drawn.
 */
bool dgx_fb_flip_draw(dgx_fb_flip_t *flip);

/**
 * @brief A batch has been drawn: how long it took is kept.
 *
 * To be called before the frame buffer is given what it lacks of the other
 * one. That is the work of the first batch of a picture only, and a batch
 * that joins a picture has none of it, so it is not the time of a batch
 * that is asked for when one is about to join.
 */
void dgx_fb_flip_drawn(dgx_fb_flip_t *flip);

/**
 * @brief The batch being drawn has waited for something a batch that joins a picture never waits for.
 *
 * That time is not counted as the time of the batch.
 */
static inline void dgx_fb_flip_idled(dgx_fb_flip_t *flip, uint32_t us)
{
    flip->batch_from += us;
}

/** @brief A batch has ended and `drawn` is a whole picture: tell the display. */
void dgx_fb_flip_finished(dgx_fb_flip_t *flip);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
