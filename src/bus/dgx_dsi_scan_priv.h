#pragma once
/*
 * dgx_dsi_scan_priv.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  When a write behind the scan of a DSI display may start. The display
 *  reads its frame buffer row after row, a frame in the time between two
 *  ends of a frame. The 2D DMA and the accelerator write a rectangle of it
 *  row after row from the top. A row is seen whole when the display reads
 *  it after it is written, in the frame the write is for.
 *
 *  A write as fast as the scan, row for row, or faster stays ahead of it
 *  once it starts before the scan comes to its first row. A slower one
 *  loses on every row, and it has to start earlier by what it loses over
 *  all of them: the scan must not come to its last row before it is
 *  written. On a JC1060P470 (7-inch, JD9165) the 2D DMA copies a row from
 *  PSRAM twice as fast as the display reads one. On a JC8012P4A1C
 *  (10.1-inch, JD9365) with the PSRAM at 200 MHz it copies hardly faster,
 *  127 to 139 KB a millisecond against the 124 the display reads, and the
 *  accelerator mixes slower, 91; anything else that uses the PSRAM
 *  meanwhile slows them down further.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/*
 * Where the scan of the display is, by the time since the end of the last
 * frame, and when it comes to the rows asked about. The display starts to
 * read the first rows of a frame `scan_lead` rows before it shows them.
 */
typedef struct {
    uint32_t period_us; /* how long a frame takes */
    uint32_t since_us;  /* the time since the end of the last frame, less than a period */
    uint32_t now_us;    /* when the scan was looked at: `since_us` runs to it, and a wait counts from it */
    uint32_t enter_us;  /* when, counted from that end, the scan comes to the first of the rows */
    uint32_t last_us;   /* when it comes to the last of them */
    uint32_t leave_us;  /* and when it has read the last of them */
} dgx_dsi_scan_t;

/*
 * How long to wait before a write of `rows` rows from the top may start: it
 * takes `start_us` before its first byte and `takes_us` from its first byte
 * to its last. Where it can start in time, it is the time to the first
 * such moment, 0 when that is now. *in_time is false when no moment is in
 * time: a write too slow, or rows too many, for the time the scan is out of
 * them. Such a write is best started when the scan has just left the rows,
 * a whole frame before it comes back, and the time to that is returned; the
 * display then comes upon its last rows before they are written.
 */
static inline uint32_t dgx_dsi_scan_wait_for(const dgx_dsi_scan_t *scan, uint32_t rows, uint32_t start_us, uint32_t takes_us, bool *in_time)
{
    const uint32_t period = scan->period_us, since = scan->since_us, enter = scan->enter_us, leave = scan->leave_us;
    /* the time the scan takes from the first row to the last: a write that takes longer loses the difference */
    const uint32_t across = scan->last_us - enter;
    /*
     * How long before the scan comes to the first row the write has to
     * start: by the time it writes a row, for the first row, and by all it
     * loses, for the last; the larger of the two.
     */
    const uint32_t one   = takes_us / (rows ? rows : 1u), loses = takes_us > across ? takes_us - across : 0u;
    const uint32_t ahead = start_us + (one > loses ? one : loses);
    /* the scan is out of the rows from `leave` to `enter` of the next frame */
    *in_time = ahead <= period - (leave - enter);
    if (since + ahead <= enter) return 0; /* the scan comes late enough */
    /* the scan has left the rows, and comes back late enough or is never late enough: now is as good as any */
    if (since >= leave && (since + ahead <= enter + period || !*in_time)) return 0;
    /* the moment the scan leaves the rows: in this frame, or in the next when it comes back too soon */
    return (since < leave ? leave : leave + period) - since;
}

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
