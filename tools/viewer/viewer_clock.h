#ifndef VIEWER_CLOCK_H
#define VIEWER_CLOCK_H

/* Fixed-timestep tick scheduler with pause / single-step / slow-motion.
 * Pure logic, no SDL — unit-tested headlessly (tests/test_viewer_clock.c). */

#define VCLOCK_TICK_MS   (1000.0 / 60.0)
#define VCLOCK_MAX_TICKS 8   /* per advance; excess wall time is dropped */
#define VCLOCK_NUM_RATES 3   /* 1x, 1/4x, 1/16x */

typedef struct {
    double acc_ms;
    int    paused;
    int    step_queued;
    int    rate;             /* 0 = 1x, 1 = 1/4x, 2 = 1/16x */
} ViewerClock;

void   vclock_init(ViewerClock* c);
void   vclock_toggle_pause(ViewerClock* c);
void   vclock_queue_step(ViewerClock* c);   /* one tick next advance; paused only */
void   vclock_cycle_rate(ViewerClock* c);
double vclock_tick_period_ms(const ViewerClock* c);
/* Feed elapsed wall ms; returns sim ticks to run now (0..VCLOCK_MAX_TICKS). */
int    vclock_advance(ViewerClock* c, double elapsed_ms);

#endif
