/* Unit tests for the viewer's fixed-timestep scheduler. Floats OK (not sim). */
#include <stdio.h>
#include <math.h>
#include "../tools/viewer/viewer_clock.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    ViewerClock c;

    /* accumulation */
    vclock_init(&c);
    CHECK(vclock_advance(&c, 8.0) == 0, "8ms < one 60Hz period: no tick");
    CHECK(vclock_advance(&c, 9.0) == 1, "17ms accumulated: one tick");
    CHECK(vclock_advance(&c, 3 * VCLOCK_TICK_MS) == 3, "3 periods: 3 ticks");

    /* spiral-of-death cap drains the accumulator */
    vclock_init(&c);
    CHECK(vclock_advance(&c, 5000.0) == VCLOCK_MAX_TICKS, "huge elapsed capped");
    CHECK(vclock_advance(&c, VCLOCK_TICK_MS + 0.01) == 1, "accumulator drained after cap");

    /* pause and single-step */
    vclock_init(&c);
    vclock_toggle_pause(&c);
    CHECK(vclock_advance(&c, 1000.0) == 0, "paused: no ticks");
    vclock_queue_step(&c);
    CHECK(vclock_advance(&c, 0.0) == 1, "queued step fires exactly once");
    CHECK(vclock_advance(&c, 1000.0) == 0, "step does not repeat");
    vclock_toggle_pause(&c);
    CHECK(vclock_advance(&c, VCLOCK_TICK_MS * 0.5) == 0, "unpause resets accumulator");

    /* slow-mo rates */
    vclock_init(&c);
    vclock_cycle_rate(&c);
    CHECK(fabs(vclock_tick_period_ms(&c) - 4 * VCLOCK_TICK_MS) < 1e-9, "1/4x period");
    CHECK(vclock_advance(&c, 4 * VCLOCK_TICK_MS) == 1, "1/4x: 4 real periods = 1 tick");
    vclock_cycle_rate(&c);
    CHECK(vclock_advance(&c, 16 * VCLOCK_TICK_MS) == 1, "1/16x: 16 real periods = 1 tick");
    vclock_cycle_rate(&c);
    CHECK(c.rate == 0, "rate cycles back to 1x");

    /* step is pause-only */
    vclock_init(&c);
    vclock_queue_step(&c);
    CHECK(vclock_advance(&c, 0.0) == 0, "queue_step ignored while running");

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("viewer_clock: ALL TESTS PASSED\n");
    return 0;
}
