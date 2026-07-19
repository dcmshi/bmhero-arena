#include "viewer_clock.h"

void vclock_init(ViewerClock* c) {
    c->acc_ms = 0; c->paused = 0; c->step_queued = 0; c->rate = 0;
}

void vclock_toggle_pause(ViewerClock* c) {
    c->paused = !c->paused;
    c->acc_ms = 0;           /* no burst of ticks on unpause */
    c->step_queued = 0;
}

void vclock_queue_step(ViewerClock* c) { if (c->paused) c->step_queued = 1; }

void vclock_cycle_rate(ViewerClock* c) { c->rate = (c->rate + 1) % VCLOCK_NUM_RATES; }

double vclock_tick_period_ms(const ViewerClock* c) {
    double p = VCLOCK_TICK_MS;
    for (int i = 0; i < c->rate; i++) p *= 4.0;
    return p;
}

int vclock_advance(ViewerClock* c, double elapsed_ms) {
    if (c->paused) {
        int n = c->step_queued;
        c->step_queued = 0;
        return n;
    }
    c->acc_ms += elapsed_ms;
    double period = vclock_tick_period_ms(c);
    int n = 0;
    while (c->acc_ms >= period && n < VCLOCK_MAX_TICKS) { c->acc_ms -= period; n++; }
    if (c->acc_ms >= period) c->acc_ms = 0;   /* fell behind: drop, don't spiral */
    return n;
}
