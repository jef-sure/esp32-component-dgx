#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bus/dgx_fb_flip_priv.h"
#include "check.h"

/*
 * A display that reads one of two frame buffers, frame after frame, and
 * takes the one it was last told where a frame ends; and tasks that draw
 * batches one at a time. Time goes on in every call, by an amount that is
 * not known beforehand, and a frame may end in the middle of any of them.
 *
 * Whatever happens, the display never starts a frame from a frame buffer a
 * batch is being drawn into, and nothing is drawn into the one it reads.
 * And what it shows of a task never goes back to something older, unless
 * the task that draws is put aside for longer than a frame twice in a row,
 * in the very calls that tell the display.
 *
 * And the rates: what several tasks draw in a frame is shown by one change
 * of the frame buffers, and a task that draws more often than frames are
 * shown gets its batches into every one of them.
 */

#define TASKS_MOST 12

static uint32_t seed = 1961;

static dgx_fb_flip_t flip;

static uint32_t rnd(uint32_t from, uint32_t to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (seed >> 8) % (to - from + 1u);
}

static struct {
    /* the display */
    uint32_t now;          /* microseconds */
    uint32_t period;
    uint32_t jitter;       /* a frame ends up to this much later than it should */
    uint32_t count_delay;  /* and is counted this much later than the display takes the frame buffer */
    bool     running;      /* frames are sent */
    uint32_t next_end;     /* when the frame being sent ends, without the jitter */
    uint32_t next_take;    /* and with it */
    uint32_t count_at;     /* when the frame that has ended is counted; 0 when none waits for it */
    uint8_t  told;         /* the frame buffer the display was last told */
    uint8_t  read;         /* the one it reads */
    uint32_t ended;        /* frames counted */
    uint32_t counted_at;   /* when the last one was */
    uint32_t measured;     /* and the time between the last two */
    /* how long a call may take */
    uint32_t call_least;   /* most of them: from this */
    uint32_t call_most;    /* to this */
    uint32_t stall_one_in; /* one in so many takes up to `stall_most`: the task was put aside */
    uint32_t stall_most;
    /* what is drawn */
    int      drawing;      /* the frame buffer a batch is being drawn into, -1 when none */
    uint32_t version[2][TASKS_MOST]; /* the batch of every task a frame buffer holds */
    uint32_t shown[TASKS_MOST];      /* and the display has shown */
    uint32_t shown_frames[TASKS_MOST]; /* frames that showed a task something new */
    uint32_t frames;       /* frames started */
    uint32_t changes;      /* of them from another frame buffer than the one before */
    uint32_t copy_us;      /* what it takes to give a frame buffer what it lacks, when the first batch of a picture ends */
    uint32_t idle_us;      /* what the first batch of a picture waits for something that is not its drawing */
    bool     untimed;      /* the times of the frames are said not to be known */
    uint32_t late_words;   /* times the display was told to stay with the frame buffer it reads when the frame was about to end */
    uint32_t broken;       /* times something half-drawn was shown, or could have been */
    uint32_t back;         /* times the display went back to an older picture */
} sim;

/* a frame ends here: the display takes the frame buffer it was told */
static void sim_take(void)
{
    if (sim.told != sim.read) ++sim.changes;
    sim.read = sim.told;
    ++sim.frames;
    if (sim.drawing == sim.read) ++sim.broken;
    for (int task = 0; task < TASKS_MOST; ++task) {
        const uint32_t version = sim.version[sim.read][task];
        if (version < sim.shown[task]) ++sim.back;
        if (version > sim.shown[task]) ++sim.shown_frames[task];
        sim.shown[task] = version;
    }
}

static void sim_count(uint32_t at)
{
    ++sim.ended;
    sim.measured   = at - sim.counted_at;
    sim.counted_at = at;
}

static void sim_advance(uint32_t by)
{
    const uint32_t until = sim.now + by;
    while (sim.running) {
        /* what comes first: a frame that has ended is counted, or the next one ends */
        if (sim.count_at && sim.count_at <= until && sim.count_at <= sim.next_take) {
            sim.now = sim.count_at;
            sim_count(sim.now);
            sim.count_at = 0;
        } else if (sim.next_take <= until) {
            sim.now = sim.next_take;
            sim_take();
            if (sim.count_delay) {
                sim.count_at = sim.now + sim.count_delay;
            } else {
                sim_count(sim.now);
            }
            sim.next_end += sim.period;
            sim.next_take = sim.next_end + (sim.jitter ? rnd(0, sim.jitter) : 0u);
        } else {
            break;
        }
    }
    sim.now = until;
}

static uint32_t sim_call_time(void)
{
    if (sim.stall_one_in && rnd(1, sim.stall_one_in) == 1) return rnd(0, sim.stall_most);
    return rnd(sim.call_least, sim.call_most);
}

static void sim_show(void *context, uint8_t frame)
{
    CHECK(context == &sim);
    CHECK(frame < 2);
    /* a word taken back is not tried when the frame is about to end: it would be given again at once, and both may come late */
    if (frame != flip.drawn && sim.ended >= 2 && sim.now - sim.counted_at + flip.guard_us >= sim.measured + 100u) ++sim.late_words;
    sim_advance(sim_call_time());
    sim.told = frame;
    sim_advance(sim_call_time());
}

static bool sim_frames(void *context, uint32_t *ended, uint32_t *since_us, uint32_t *period_us)
{
    CHECK(context == &sim);
    *ended     = sim.ended;
    *since_us  = sim.now - sim.counted_at;
    *period_us = sim.measured;
    const bool timed = !sim.untimed && sim.ended >= 2 && *since_us <= 2u * *period_us;
    sim_advance(sim_call_time());
    return timed;
}

static uint32_t waits_failed;

static bool sim_wait(void *context)
{
    CHECK(context == &sim);
    if (!sim.running) {
        ++waits_failed;
        sim.now += 200000u;
        return false;
    }
    /* until a frame is counted, and a little more before the task runs */
    const uint32_t ended = sim.ended;
    while (sim.ended == ended) sim_advance(sim.count_at ? sim.count_at - sim.now : sim.next_take - sim.now);
    sim_advance(rnd(0, 30));
    return true;
}

static uint32_t sim_now(void *context)
{
    CHECK(context == &sim);
    return sim.now;
}

static const dgx_fb_flip_display_t sim_display = {.show = sim_show, .frames = sim_frames, .wait = sim_wait, .now_us = sim_now};

/* batches drawn since the last sim_start() */
static uint32_t batches;

static void sim_start(uint32_t period, uint32_t jitter, uint32_t count_delay, bool at_switch, uint32_t guard)
{
    memset(&sim, 0, sizeof(sim));
    sim.period      = period;
    sim.jitter      = jitter;
    sim.count_delay = count_delay;
    sim.running     = true;
    sim.now         = 1000;
    sim.next_end    = sim.now + rnd(1, period);
    sim.next_take   = sim.next_end;
    sim.read        = 0;
    sim.told        = 0;
    sim.call_most   = 8;
    sim.drawing     = -1;
    waits_failed    = 0;
    batches         = 0;
    /* the display starts with the first frame buffer: drawing goes into the second */
    dgx_fb_flip_init(&flip, &sim_display, &sim, 1, at_switch, guard);
    CHECK(flip.drawn == 1 && !flip.told && flip.flips == 0);
}

/* go on until a frame has ended, and a little more */
static void next_frame(void)
{
    const uint32_t ended = sim.ended;
    while (sim.ended == ended) sim_advance(50);
    sim_advance(200);
}

/* a batch of a task, `takes` microseconds long; who draws it is known here only, to tell what the display shows of whom */
static void batch(int task, uint32_t takes)
{
    static uint32_t version;
    bool            first = false;
    ++batches;
    if (flip.told) {
        const uint8_t before = flip.drawn;
        if (dgx_fb_flip_draw(&flip)) {
            first = true;
            CHECK(flip.drawn == (before ^ 1u));
            /* the frame buffer drawing goes into now gets what it lacks */
            memcpy(sim.version[flip.drawn], sim.version[before], sizeof(sim.version[0]));
        } else {
            CHECK(flip.drawn == before);
        }
        CHECK(!flip.told);
    }
    if (sim.read == flip.drawn) ++sim.broken;
    sim.drawing = flip.drawn;
    /* drawn piece by piece: a frame that ends meanwhile sees a part of it, which is what must not be shown */
    sim_advance(takes / 2u);
    sim.version[flip.drawn][task] = ++version;
    sim_advance(takes - takes / 2u);
    /* the first batch of a picture may have waited for what a batch that joins never waits for: that is told, and is not its time */
    if (first && sim.idle_us) {
        sim_advance(sim.idle_us);
        dgx_fb_flip_idled(&flip, sim.idle_us);
    }
    dgx_fb_flip_drawn(&flip);
    /* the first batch of a picture leaves the frame buffer lacking what the picture before had: that is copied now */
    if (first) sim_advance(sim.copy_us);
    if (sim.read == flip.drawn) ++sim.broken;
    sim.drawing = -1;
    dgx_fb_flip_finished(&flip);
    CHECK(flip.told);
}

typedef struct {
    uint32_t takes_from, takes_to; /* how long a batch takes */
    uint32_t pause_from, pause_to; /* and how long the task does something else after it */
} task_t;

/* the tasks draw one at a time, the one that has waited longest first, for `time` microseconds */
static void run(const task_t *tasks, int count, uint32_t time)
{
    uint32_t       wants[TASKS_MOST] = {0};
    const uint32_t until             = sim.now + time;
    for (int i = 0; i < count; ++i) wants[i] = sim.now + i;
    while ((int32_t)(until - sim.now) > 0) {
        int next = 0;
        for (int i = 1; i < count; ++i) {
            if ((int32_t)(wants[i] - wants[next]) < 0) next = i;
        }
        if ((int32_t)(wants[next] - sim.now) > 0) sim_advance(wants[next] - sim.now);
        batch(next, rnd(tasks[next].takes_from, tasks[next].takes_to));
        wants[next] = sim.now + rnd(tasks[next].pause_from, tasks[next].pause_to);
    }
}

#define PERIOD 16418u
#define GUARD  2000u

/* since the last frame ended */
static uint32_t since(void)
{
    return sim.now - sim.counted_at;
}

/* a task that draws without a pause: several batches go into every picture, every frame shows a new one, and it waits only where a frame is about to end */
static void test_one_task(void)
{
    static const task_t tasks[] = {{3000, 4000, 0, 0}};
    sim_start(PERIOD, 0, 0, true, GUARD);
    run(tasks, 1, 600 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    CHECK(sim.frames >= 599 && sim.frames <= 601);
    CHECK(flip.flips >= sim.frames - 3 && flip.flips <= sim.frames);
    CHECK(sim.changes >= sim.frames - 3);
    CHECK(sim.shown_frames[0] >= sim.frames - 3);
    /* three or four batches a frame, all but the first of a picture joined to it */
    CHECK(batches >= 3 * sim.frames && batches <= 5 * sim.frames);
    CHECK(flip.joined + flip.flips + 1 >= batches && flip.joined + flip.flips <= batches);
    CHECK(flip.kept_back == 0);
    CHECK(flip.batch_us >= 3000 && flip.batch_us <= 4100);
    /* a batch that would not be over before the frame ends waits for its end, what is left of the frame */
    CHECK(flip.waits >= sim.frames - 3 && flip.waits <= sim.frames);
    CHECK(flip.wait_us <= flip.waits * (4100u + GUARD + 200u) && flip.wait_us >= flip.waits * 1000u);
}

/* three tasks that do not pause: batches of each in every frame, shown by one change of the frame buffers */
static void test_three_tasks(void)
{
    static const task_t tasks[] = {{3000, 4000, 0, 0}, {1000, 1500, 0, 0}, {2000, 2500, 0, 0}};
    sim_start(PERIOD, 0, 0, true, GUARD);
    run(tasks, 3, 600 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    CHECK(flip.flips >= sim.frames - 3 && flip.flips <= sim.frames);
    for (int task = 0; task < 3; ++task) CHECK(sim.shown_frames[task] >= sim.frames - 4);
    CHECK(flip.joined >= 2 * (sim.frames - 4));
    CHECK(flip.kept_back == 0);
}

/*
 * The same when the frame buffer has much to be given after the first batch
 * of a picture, twice the time of a batch: that is not counted as the time
 * of a batch, or nothing would dare to join.
 */
static void test_three_tasks_and_a_copy(void)
{
    static const task_t tasks[] = {{2300, 2400, 0, 0}, {2300, 2400, 0, 0}, {2300, 2400, 0, 0}};
    sim_start(PERIOD, 0, 0, true, GUARD);
    sim.copy_us = 4600;
    run(tasks, 3, 600 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    CHECK(flip.flips >= sim.frames - 3 && flip.flips <= sim.frames);
    for (int task = 0; task < 3; ++task) CHECK(sim.shown_frames[task] >= sim.frames - 4);
    CHECK(flip.batch_us >= 2300 && flip.batch_us <= 2400);
    CHECK(flip.joined >= 2 * (sim.frames - 4));
    CHECK(flip.kept_back == 0);
}

/* the same when the first batch of a picture waits 5 ms for what the picture before left behind: that wait is not the time of a batch either */
static void test_three_tasks_and_a_wait(void)
{
    static const task_t tasks[] = {{2300, 2400, 0, 0}, {2300, 2400, 0, 0}, {2300, 2400, 0, 0}};
    sim_start(PERIOD, 0, 0, true, GUARD);
    sim.idle_us = 5000;
    run(tasks, 3, 600 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    CHECK(flip.flips >= sim.frames - 3 && flip.flips <= sim.frames);
    for (int task = 0; task < 3; ++task) CHECK(sim.shown_frames[task] >= sim.frames - 4);
    CHECK(flip.batch_us >= 2300 && flip.batch_us <= 2400);
    CHECK(flip.joined >= sim.frames - 4);
    CHECK(flip.kept_back == 0);
}

/* tasks that come when they like, each many times a frame: nothing of a frame is lost */
static void test_tasks_with_pauses(void)
{
    static const task_t tasks[] = {{500, 900, 1000, 3000}, {2000, 2600, 1000, 2000}, {100, 200, 1000, 1500}, {1000, 1200, 1000, 9000}};
    sim_start(PERIOD, 0, 0, true, GUARD);
    run(tasks, 4, 1000 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    CHECK(flip.flips >= sim.frames - 3 && flip.flips <= sim.frames);
    for (int task = 0; task < 4; ++task) CHECK(sim.shown_frames[task] >= sim.frames * 9 / 10);
    /* a batch is taken to be as long as the longest ones were lately, so a frame hardly ever ends in one */
    CHECK(flip.kept_back <= sim.frames / 50);
}

/* tasks that draw seldom: a batch is shown by the frame that follows, not later */
static void test_slow_tasks(void)
{
    static const task_t tasks[] = {{1000, 2000, 40000, 90000}, {300, 500, 25000, 70000}};
    sim_start(PERIOD, 0, 0, true, GUARD);
    run(tasks, 2, 3000 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    /* every batch was shown: a change of the frame buffers for each, but for those that joined another one */
    CHECK(flip.flips > 50);
    CHECK(sim.changes + 1 >= flip.flips);
    CHECK(sim.shown_frames[0] + sim.shown_frames[1] >= sim.changes);
    /* a batch waits only when it comes just before a frame ends, and then no longer than to its end */
    CHECK(flip.waits <= flip.flips / 4);
    CHECK(flip.wait_us <= flip.waits * (2000u + GUARD + 200u));
    CHECK(flip.kept_back == 0);
}

/* batches longer than a frame: one after another, each shown by the frame after it */
static void test_long_batches(void)
{
    static const task_t tasks[] = {{20000, 40000, 0, 2000}, {18000, 25000, 0, 0}};
    sim_start(PERIOD, 0, 0, true, GUARD);
    run(tasks, 2, 2000 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    /* but for the first one, of which nothing was known */
    CHECK(flip.joined <= 2);
    CHECK(flip.flips > 600);
    CHECK(sim.shown_frames[0] + sim.shown_frames[1] + 2 >= flip.flips);
}

/* a batch much longer than those before it: the picture it joined is kept back a frame, once, and then shown */
static void test_kept_back(void)
{
    sim_start(PERIOD, 0, 0, true, GUARD);
    /* some frames go by, and the batches are short */
    for (int i = 0; i < 40; ++i) batch(i & 1, 300);
    CHECK(flip.kept_back == 0 && sim.broken == 0 && sim.back == 0);
    CHECK(flip.batch_us >= 300 && flip.batch_us < 500);
    /* just after a frame has started a batch is drawn; the next one joins and is drawn over the end of the frame */
    next_frame();
    batch(0, 300);
    const uint32_t flips = flip.flips, frames = sim.frames, joined = flip.joined;
    const uint32_t first = sim.version[flip.drawn][0];
    batch(1, PERIOD);
    CHECK(flip.joined == joined + 1 && flip.flips == flips);
    CHECK(sim.frames == frames + 1);
    /* the frame that ended meanwhile did not show the first batch: the second one was being drawn */
    CHECK(sim.shown[0] < first);
    CHECK(sim.broken == 0);
    /* the long batch is remembered */
    CHECK(flip.batch_us >= PERIOD && flip.batch_us < PERIOD + 200);
    /* the next batch does not join: it waits until the picture is shown, both batches at once */
    const uint32_t waits = flip.waits;
    batch(2, 300);
    CHECK(flip.joined == joined + 1);
    CHECK(flip.flips == flips + 1 && flip.waits == waits + 1);
    CHECK(flip.kept_back == 1);
    CHECK(sim.shown[0] == first && sim.shown[1] > first);
    CHECK(sim.frames == frames + 2);
    /* for a while nothing joins a picture when less than the time of the long batch is left of the frame */
    CHECK(flip.batch_us >= PERIOD * 7 / 8 && flip.batch_us < PERIOD);
    next_frame();
    batch(0, 300);
    CHECK(flip.flips == flips + 2 && flip.waits == waits + 1);
    while (since() < 4000u) sim_advance(100);
    batch(1, 300);
    CHECK(flip.waits == waits + 2 && flip.joined == joined + 1 && flip.flips == flips + 3);
    /* and the long batch is forgotten, an eighth with every batch */
    for (int i = 0; i < 60; ++i) batch(i & 1, 300);
    CHECK(flip.batch_us < 500);
    CHECK(flip.kept_back == 1);
    CHECK(sim.broken == 0 && sim.back == 0);
}

/* a short batch that turns out long: the picture is kept back over one end of a frame and no more, whatever time is left of the next one */
static void test_kept_back_once(void)
{
    sim_start(PERIOD, 0, 0, true, GUARD);
    for (int i = 0; i < 40; ++i) batch(i & 1, 300);
    /* late in a frame, but with time for a batch as long as those before */
    next_frame();
    batch(0, 300);
    while (since() < PERIOD - GUARD - 1000u) sim_advance(50);
    const uint32_t flips = flip.flips, frames = sim.frames, joined = flip.joined, waits = flip.waits;
    batch(1, 3000);
    CHECK(flip.joined == joined + 1 && sim.frames == frames + 1 && flip.flips == flips);
    CHECK(flip.batch_us >= 3000 && flip.batch_us < 3200);
    /* there is time for another batch like that in the frame that has started */
    CHECK(since() + flip.batch_us + GUARD < PERIOD);
    const uint32_t first = sim.version[flip.drawn][0], second = sim.version[flip.drawn][1];
    batch(2, 300);
    /* but the picture has been kept back once: the batch waited until it was shown */
    CHECK(flip.joined == joined + 1 && flip.waits == waits + 1 && flip.flips == flips + 1);
    CHECK(sim.frames == frames + 2);
    CHECK(sim.shown[0] == first && sim.shown[1] == second && sim.shown[2] < sim.version[flip.drawn][2]);
    CHECK(flip.kept_back == 1);
    CHECK(sim.broken == 0 && sim.back == 0);
}

/* a frame ends while the display is being told of a picture: it may have taken it, so nothing joins it; the next picture is joined as usual */
static void test_frame_ends_in_telling(void)
{
    for (int round = 0; round < 20; ++round) {
        sim_start(PERIOD, 0, 0, true, GUARD);
        for (int i = 0; i < 12; ++i) batch(i & 1, 300);
        /* the batch ends a few microseconds before the frame does, and the calls take longer than that */
        while (sim.next_take - sim.now > 400u) sim_advance(sim.next_take - sim.now > 1000u ? 500 : 10);
        const uint8_t before = flip.drawn;
        if (flip.told && dgx_fb_flip_draw(&flip)) memcpy(sim.version[flip.drawn], sim.version[before], sizeof(sim.version[0]));
        sim_advance(sim.next_take - sim.now - 1u - (uint32_t)round % 8u);
        sim.call_least = 10, sim.call_most = 30;
        const uint32_t frames = sim.frames;
        dgx_fb_flip_drawn(&flip);
        dgx_fb_flip_finished(&flip);
        sim.call_least = 0, sim.call_most = 8;
        CHECK(sim.frames == frames + 1);
        CHECK(flip.closed);
        const uint32_t joined = flip.joined, flips = flip.flips;
        batch(4, 300);
        CHECK(flip.joined == joined && flip.flips == flips + 1);
        CHECK(!flip.closed);
        /* the batch that was held until the frame ended counts as a long one: it is forgotten first */
        for (int i = 0; i < 40; ++i) batch(i & 1, 300);
        next_frame();
        batch(5, 300);
        const uint32_t before_it = flip.joined;
        batch(6, 300);
        CHECK(flip.joined == before_it + 1);
        CHECK(sim.broken == 0 && sim.back == 0);
    }
}

/* how long a batch may take is not known before one has been timed, and a very long one is forgotten in time */
static void test_batch_time(void)
{
    sim_start(PERIOD, 0, 0, true, GUARD);
    sim_advance(7000000u);
    next_frame();
    /* nothing was told: nothing to do before drawing */
    CHECK(!dgx_fb_flip_draw(&flip));
    CHECK(flip.drawn == 1 && flip.flips == 0 && flip.waits == 0 && !flip.told);
    batch(0, 500);
    /* the first batch started nobody knows when */
    CHECK(flip.batch_us == 0 && !flip.timed);
    /* the next one joins it: frames have been counted for long, and this one has just started */
    CHECK(since() < 3000u);
    batch(1, 500);
    CHECK(flip.joined == 1 && flip.flips == 0 && flip.waits == 0);
    CHECK(flip.batch_us >= 500 && flip.batch_us < 600 && flip.timed);
    batch(0, 3000000u);
    CHECK(flip.batch_us == DGX_FB_FLIP_BATCH_US_MOST);
    /* nothing joins a picture for a while */
    const uint32_t joined = flip.joined;
    for (int i = 0; i < 6; ++i) {
        next_frame();
        batch(1, 300);
        batch(0, 300);
        CHECK(flip.joined == joined);
    }
    /* forgotten by an eighth a batch: 100 ms come down to the 300 us of the batches after some 45 of them */
    CHECK(flip.batch_us > 10000);
    for (int i = 0; i < 50; ++i) {
        next_frame();
        batch(0, 300);
    }
    CHECK(flip.batch_us == 300);
    next_frame();
    batch(1, 300);
    const uint32_t before_it = flip.joined;
    batch(0, 300);
    CHECK(flip.joined == before_it + 1);
    CHECK(sim.broken == 0 && sim.back == 0);
}

/*
 * Two tasks that never pause and one that draws seldom and long. How long a
 * batch may take is one number for all of them, and the short batches make
 * the long one forgotten: when it comes late in a frame it joins and keeps
 * the picture back a frame. No more than a frame for each of its batches,
 * and nothing half-drawn is shown.
 */
static void test_seldom_and_long(void)
{
    static const task_t tasks[] = {{800, 1000, 0, 0}, {500, 700, 0, 0}, {9000, 10000, 150000, 400000}};
    sim_start(PERIOD, 0, 0, true, GUARD);
    run(tasks, 3, 6000 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    CHECK(sim.shown_frames[2] > 250);
    CHECK(flip.kept_back > 0 && flip.kept_back <= sim.shown_frames[2]);
    CHECK(sim.shown_frames[0] >= sim.frames - 2 * flip.kept_back - 12 && sim.shown_frames[1] >= sim.frames - 2 * flip.kept_back - 12);
}

/* when the times of the frames are not known, nothing joins a picture: a batch waits for the display */
static void test_untimed(void)
{
    sim_start(PERIOD, 0, 0, true, GUARD);
    next_frame();
    next_frame();
    next_frame();
    batch(0, 300);
    sim.untimed = true;
    batch(1, 300);
    CHECK(flip.joined == 0 && flip.flips == 1 && flip.waits == 1);
    batch(2, 300);
    CHECK(flip.joined == 0 && flip.flips == 2 && flip.waits == 2);
    sim.untimed = false;
    batch(1, 300);
    CHECK(flip.joined == 1 && flip.flips == 2 && flip.waits == 2);
    CHECK(sim.broken == 0 && sim.back == 0);
}

/* a dozen tasks: all of them in every frame */
static void test_many_tasks(void)
{
    task_t tasks[TASKS_MOST];
    for (int i = 0; i < TASKS_MOST; ++i) tasks[i] = (task_t){200, 400, 0, 500};
    sim_start(PERIOD, 0, 0, true, GUARD);
    run(tasks, TASKS_MOST, 600 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    CHECK(flip.flips >= sim.frames - 3 && flip.flips <= sim.frames);
    for (int task = 0; task < TASKS_MOST; ++task) CHECK(sim.shown_frames[task] >= sim.frames - 4);
    CHECK(flip.kept_back == 0);
}

/*
 * Calls that take any time, a task put aside in the middle of telling the
 * display, frames that end late: nothing half-drawn is shown. `count_delay`
 * and `at_switch` are the two ways frames are counted.
 *
 * A task put aside for longer than a frame can make the display go back to
 * the picture before for a frame: it took the finished picture, was told to
 * stay with the old one too late, and told to take the finished one again
 * too late as well. A task cannot help that; it is let through here.
 */
static void test_any_timing(uint32_t jitter, uint32_t count_delay, bool at_switch, uint32_t stall_one_in, uint32_t stall_most)
{
    for (int round = 0; round < 40; ++round) {
        task_t    tasks[5];
        const int count = (int)rnd(1, 5);
        for (int i = 0; i < count; ++i) {
            const uint32_t takes = rnd(0, 3) ? rnd(0, 3000) : rnd(0, 30000);
            const uint32_t pause = rnd(0, 2) ? rnd(0, 2000) : rnd(0, 50000);
            tasks[i] = (task_t){takes / 2, takes, pause / 2, pause};
        }
        sim_start(PERIOD, jitter, count_delay, at_switch, GUARD);
        sim.call_most    = rnd(0, 40);
        sim.stall_one_in = stall_one_in;
        sim.stall_most   = stall_most;
        run(tasks, count, 400 * PERIOD);
        CHECK(sim.broken == 0);
        if (stall_most < PERIOD) CHECK(sim.back == 0);
        if (!stall_one_in) CHECK(sim.late_words == 0);
        CHECK(flip.flips > 10);
        CHECK(flip.flips <= sim.frames + 1);
        /* the frame buffers changed places as many times as the display changed the one it reads, give or take the last one */
        if (stall_most < PERIOD) CHECK(sim.changes <= flip.flips + 1 && flip.flips <= sim.changes + 1);
        CHECK(waits_failed == 0);
    }
}

/* no frames are sent, as before a display is started: drawing goes on, a batch into one frame buffer and the next into the other */
static void test_no_frames(void)
{
    sim_start(PERIOD, 0, 0, true, GUARD);
    sim.running = false;
    for (int i = 0; i < 6; ++i) batch(i % 3, 500);
    CHECK(flip.flips == 5 && flip.joined == 0);
    CHECK(waits_failed == 5);
    CHECK(sim.frames == 0);
    sim.broken = 0; /* nothing reads the frame buffers yet */
    /* and when they start, things go as they should */
    static const task_t tasks[] = {{1000, 1500, 0, 0}, {1000, 1500, 0, 0}};
    sim.running   = true;
    sim.next_end  = sim.now + 100;
    sim.next_take = sim.next_end;
    sim.told      = flip.drawn ^ 1u;
    sim.read      = sim.told;
    memset(sim.shown, 0, sizeof(sim.shown));
    memcpy(sim.version[flip.drawn ^ 1u], sim.version[flip.drawn], sizeof(sim.version[0]));
    flip.told = false;
    waits_failed = 0;
    run(tasks, 2, 300 * PERIOD);
    CHECK(sim.broken == 0 && sim.back == 0);
    CHECK(waits_failed == 0);
    CHECK(flip.flips >= sim.frames - 4);
    CHECK(flip.joined >= sim.frames - 8);
}

int main(void)
{
    test_one_task();
    test_three_tasks();
    test_three_tasks_and_a_copy();
    test_three_tasks_and_a_wait();
    test_tasks_with_pauses();
    test_slow_tasks();
    test_long_batches();
    test_kept_back();
    test_kept_back_once();
    test_frame_ends_in_telling();
    test_batch_time();
    test_seldom_and_long();
    test_untimed();
    test_many_tasks();
    /* frames counted where the display takes the frame buffer */
    test_any_timing(0, 0, true, 0, 0);
    test_any_timing(300, 0, true, 0, 0);
    test_any_timing(300, 0, true, 50, 30000);
    test_any_timing(0, 0, true, 5, 3000);
    /* and 12 rows of the display later, as on a chip of revision 3 and later with the older driver */
    test_any_timing(0, 310, false, 0, 0);
    test_any_timing(300, 310, false, 0, 0);
    test_any_timing(300, 310, false, 50, 30000);
    test_no_frames();
    CHECK_DONE();
}
