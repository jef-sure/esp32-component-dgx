#include <stdint.h>
#include <stdio.h>

#include "bus/dgx_dsi_scan_priv.h"
#include "check.h"

/*
 * When a write behind the scan of a DSI display may start, against the scan
 * and the write followed row by row. The display reads the row r of the
 * rectangle at enter + r * row at the earliest, again a period later; how
 * far ahead it reads is not known, so it is inside the rows from `enter`
 * until `leave`, when it shows the last of them. The write puts its first
 * byte `start` after it is started and finishes a row every takes / rows.
 * Started in time, the scan is not inside the rows, and every row is
 * written before the scan comes to it next: no frame shows some of them new
 * and some old. A moment that is in time is not waited past, and a write
 * that has no such moment starts when the scan leaves the rows.
 */

static uint32_t seed = 2026;

static uint32_t rnd(uint32_t from, uint32_t to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (seed >> 8) % (to - from + 1u);
}

/* whether a write started at s is seen whole: the scan is out of the rows then, and comes to each only once it is written */
static bool whole_from(double s, const dgx_dsi_scan_t *scan, double row, uint32_t rows, double start, double takes)
{
    double at = s;
    while (at >= scan->period_us) at -= scan->period_us;
    if (at >= scan->enter_us && at < scan->leave_us) return false;
    /* when the scan comes to the first row next */
    const double next = at < scan->enter_us ? scan->enter_us : scan->enter_us + (double)scan->period_us;
    for (uint32_t r = 0; r < rows; ++r) {
        /* a microsecond for the rounding of the times to whole ones */
        if (at + start + (r + 1u) * takes / rows > next + r * row + 1.0) return false;
    }
    return true;
}

int main(void)
{
    int    rounds = 0, waited = 0, not_in_time = 0, slow = 0;
    double waited_us = 0;
    for (int round = 0; round < 200000; ++round) {
        /* a panel: rows with the blanking, its frame, the rows before the first one shown */
        const uint32_t lines = rnd(300, 1400), height = lines - rnd(10, 60), before = rnd(2, 20), lead = rnd(0, 8);
        const uint32_t period = rnd(8000, 34000);
        const double   row    = (double)period / lines;
        /* the rows written, the time since the end of the frame, and the write */
        const uint32_t first = rnd(lead, height - 1u), last = rnd(first, first + rnd(0, 1) * (height - 1u - first)) % height;
        const uint32_t top = first, bottom = last < first ? first : last, rows = bottom - top + 1u;
        dgx_dsi_scan_t scan;
        scan.period_us = period;
        scan.since_us  = rnd(0, period - 1u);
        scan.now_us    = 0;
        scan.enter_us  = (uint32_t)((before + top - lead) * row);
        scan.last_us   = (uint32_t)((before + bottom - lead) * row);
        scan.leave_us  = (uint32_t)((before + bottom + 1u) * row);
        if (scan.leave_us > period) continue;
        /* the scan in the same whole microseconds as the bus has it */
        const double   srow  = rows > 1u ? (double)(scan.last_us - scan.enter_us) / (rows - 1u) : row;
        const uint32_t start = rnd(0, 600);
        /* as fast as the scan from a quarter to four times, by the row */
        const double   speed = rnd(25, 400) / 100.0;
        const uint32_t takes = (uint32_t)(rows * srow / speed);
        slow += speed < 1.0;
        bool           in_time;
        const uint32_t wait = dgx_dsi_scan_wait_for(&scan, rows, start, takes, &in_time);
        ++rounds;
        CHECK(wait < 2u * period);
        const double s = (double)scan.since_us + wait;
        if (in_time) {
            if (!whole_from(s, &scan, srow, rows, start, takes)) {
                fprintf(stderr, "round %d: rows %u..%u, since %u, enter %u, last %u, leave %u, period %u, start %u, takes %u: waited %u and not whole\n", round,
                        (unsigned)top, (unsigned)bottom, (unsigned)scan.since_us, (unsigned)scan.enter_us, (unsigned)scan.last_us, (unsigned)scan.leave_us,
                        (unsigned)period, (unsigned)start, (unsigned)takes, (unsigned)wait);
                CHECK(0);
            }
            /* now, when the scan is not in the rows and the write is whole from now, is not waited past */
            const bool out = scan.since_us <= scan.enter_us || scan.since_us >= scan.leave_us;
            if (out && whole_from(scan.since_us, &scan, srow, rows, start + 2.0, takes)) CHECK(wait == 0);
        } else {
            ++not_in_time;
            /* no moment is whole: the write starts when the scan leaves the rows, or now when it has left them */
            CHECK(scan.since_us >= scan.leave_us ? wait == 0 : s == (double)scan.leave_us);
            for (uint32_t t = 0; t < period; t += period / 64u) CHECK(!whole_from(t, &scan, srow, rows, start - (start > 2u ? 2.0 : 0.0), takes));
        }
        if (wait) ++waited, waited_us += wait;
    }
    printf("test_dsi_scan.c: ok: %d writes, %d slower than the scan, %d waited (%.0f us on average), %d with no moment in time\n", rounds, slow, waited,
           waited ? waited_us / waited : 0.0, not_in_time);
    return 0;
}
