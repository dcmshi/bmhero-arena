/* Consistency tests for the feel-metrics probes. These assert the probe
 * OUTPUT matches the physics implied by TUNE_* in this same build, so they
 * hold at any tuning — they guard the harness, not a particular tune.
 *
 * Cross-TUNE monotonicity (lower friction => longer stop) cannot be checked
 * here: the constants are compile-time, so one binary only ever sees one
 * tuning. That property is asserted at the script level by
 * tools/tune-report.ps1 -SelfTest, which builds two variants and compares. */
#include <stdio.h>
#include "../tools/tune_probes.h"
#include "../src/arena/arena_tuning.h"
#include "../src/arena/arena_math.h"

static int failures = 0;
#define CHECK(c, ...) do { if(!(c)){ failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while(0)

static double q_to_f(q32 q) { return (double)q / (double)Q_ONE; }

int main(void) {
    TuneMetrics m;
    tune_probes_run(&m);

    /* top speed: TUNE_RUN_SPEED is per-tick; report is per-second (x60). */
    double expect_top = q_to_f(TUNE_RUN_SPEED) * 60.0;
    CHECK(m.top_speed > expect_top * 0.97 && m.top_speed < expect_top * 1.03,
          "top_speed %.3f u/s vs expected ~%.3f", m.top_speed, expect_top);
    CHECK(!m.ramp_capped, "ramp probe hit its cap - tune or harness is broken");

    /* linear accel to target: ~RUN_SPEED/RUN_ACCEL ticks, allow slack */
    int expect_t90 = TUNE_RUN_SPEED / TUNE_RUN_ACCEL;
    CHECK(m.ticks_to_90pct > 0 && m.ticks_to_90pct <= expect_t90 + 3,
          "ticks_to_90pct %d vs expected <=~%d", m.ticks_to_90pct, expect_t90);

    /* stop: decel from top at FRICTION per tick => ~RUN_SPEED/FRICTION ticks */
    int expect_stop_t = TUNE_RUN_SPEED / TUNE_RUN_FRICTION;
    CHECK(m.stop_ticks > 0 && m.stop_ticks <= expect_stop_t + 3,
          "stop_ticks %d vs expected <=~%d", m.stop_ticks, expect_stop_t);
    CHECK(m.stop_distance > 0.0, "stop_distance must be positive (got %.3f)", m.stop_distance);
    CHECK(!m.stop_capped, "stop probe hit its cap - player never came to rest");
    CHECK(m.stop_distance < 20.0, "stop_distance %.3f implausibly large", m.stop_distance);

    /* turn: bounded rate => 180deg takes ~0x8000/TUNE_TURN_RATE ticks */
    int expect_180 = 0x8000 / TUNE_TURN_RATE;
    CHECK(m.turn180_ticks >= expect_180 - 3 && m.turn180_ticks <= expect_180 + 3,
          "turn180_ticks %d vs expected ~%d", m.turn180_ticks, expect_180);
    CHECK(m.turn90_ticks > 0 && m.turn90_ticks < m.turn180_ticks,
          "turn90 (%d) must be positive and shorter than turn180 (%d)",
          m.turn90_ticks, m.turn180_ticks);
    CHECK(!m.turn_capped, "turn probe hit its cap - yaw never reached target");
    CHECK(m.turn_radius > 0.0, "turn_radius must be positive (got %.3f)", m.turn_radius);

    /* jump: apex ~ impulse^2/(2g), airtime ~ 2*impulse/g  (same model as
     * tests/test_movement.c test_jump_arc) */
    double imp = q_to_f(TUNE_JUMP_IMPULSE), grav = q_to_f(TUNE_GRAVITY);
    double expect_apex = (imp * imp) / (2.0 * grav);
    CHECK(m.jump_apex > expect_apex * 0.85 && m.jump_apex < expect_apex * 1.15,
          "jump_apex %.3f vs expected ~%.3f (>15%%)", m.jump_apex, expect_apex);
    int expect_air = (int)(2.0 * imp / grav);
    CHECK(m.jump_airtime >= expect_air - 4 && m.jump_airtime <= expect_air + 4,
          "jump_airtime %d vs expected ~%d", m.jump_airtime, expect_air);

    /* a running jump must cover ground; airtime should be close to standing */
    CHECK(m.runjump_distance > 0.0, "runjump_distance must be positive (got %.3f)",
          m.runjump_distance);
    CHECK(m.runjump_airtime >= expect_air - 6 && m.runjump_airtime <= expect_air + 6,
          "runjump_airtime %d vs expected ~%d", m.runjump_airtime, expect_air);

    /* traverse: crossing 2*half_x at top speed, plus the ramp-up */
    CHECK(m.traverse_ticks > 0, "traverse_ticks must be positive");
    CHECK(!m.traverse_capped, "traverse probe hit its cap - player never crossed");

    if (!failures) { printf("ALL TUNE REPORT TESTS PASSED\n"); return 0; }
    printf("%d FAILURE(S)\n", failures); return 1;
}
