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
#include "../src/arena/arena_geom.h"

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

    /* turn: v12 snaps the facing (TUNE_TURN_SNAP_SPEED above top speed), so a
     * reversal completes in ONE tick regardless of angle. Previously this
     * asserted a bounded ~0x8000/TUNE_TURN_RATE sweep; see the arena-fit block
     * below for why that decision was reversed. */
    CHECK(m.turn180_ticks == 1 && m.turn90_ticks == 1,
          "turns must SNAP in one tick (180 took %d, 90 took %d)",
          m.turn180_ticks, m.turn90_ticks);
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

    /* ---- arena-fit guard on the turn (2026-07-26, REVISED 2026-07-27) -----
     * A GAMEPLAY assertion, not a model-consistency one: it ties the turn
     * tuning to the map the sim actually collides with, so a future map or turn
     * change that makes the arena unturnable fails here rather than in a
     * playtest.
     *
     * The v7 form of this block is what drove TUNE_TURN_RATE 4deg -> 6deg: at
     * 4deg a 180 at top speed swept a 2.06u radius against a 3.87u short
     * half-width. It also asserted a RESPONSIVENESS BAND - 180 in 10..40 ticks -
     * whose lower bound deliberately forbade a snap, because A1.3's bounded turn
     * existed to preserve momentum.
     *
     * v12 REVERSES that. Feel testing found the sweep made every direction
     * change arc (velocity is rebuilt along a still-rotating facing), and made a
     * blast's knockback fire the player off along the old facing. Bomberman is
     * an arcade game; you go where you press. So the band is gone and the
     * radius guard is kept - now trivially satisfied, but it is the assertion
     * that would catch a future attempt to reintroduce a wide turn on a small
     * map. */
    const ArenaGeom* g = arena_geoms[0];
    double half_z = q_to_f(g->half_z);
    CHECK(m.turn_radius < half_z * 0.5,
          "turn radius %.3fu must stay under half the arena's short half-width "
          "(%.3fu, half_z=%.3f) or the arena is unturnable mid-field",
          m.turn_radius, half_z * 0.5, half_z);

    if (!failures) { printf("ALL TUNE REPORT TESTS PASSED\n"); return 0; }
    printf("%d FAILURE(S)\n", failures); return 1;
}
