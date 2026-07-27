/* Round lifecycle + turn-snap gates.
 *
 * Both of these exist because of the 2026-07-27 feel test, and both are the
 * kind of bug that no existing suite could have caught:
 *
 *  - PHASE_ROUND_END used to be TERMINAL. It counted down and then did nothing,
 *    while `gameplay` gates input off, so finishing a round - or dying to your
 *    own bomb, since there is no respawn - froze the player permanently. The
 *    determinism suite never noticed because a frozen sim is still perfectly
 *    deterministic. "Nothing changes" is exactly what it was checking for.
 *
 *  - The turn was a single bounded sweep in all cases. The real walker snaps
 *    instantly in several action states, which is what a stop-then-reverse hits.
 *
 * Sim-only, no floats in the assertions beyond printing.
 */
#include <stdio.h>
#include <string.h>
#include "../src/arena/arena_sim.h"
#include "../src/arena/arena_tuning.h"
#include "../src/arena/arena_geom.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define NEUTRAL arena_input_pack(0, 0, 0, 0, 0)

static void run(ArenaState* s, ArenaInput in0, int n) {
    ArenaInput in[ARENA_MAX_PLAYERS] = { in0, NEUTRAL, NEUTRAL, NEUTRAL };
    for (int t = 0; t < n; t++) arena_tick(s, in);
}

/* Kill everyone except player 0 by hand. Reaching for the bombs would make this
 * a bomb test; the round logic only cares that `alive` drops to 1.
 *
 * The long timer holds them DEAD for the whole test. With TUNE_RESPAWN_TICKS > 0
 * (the current TESTING ACCOMMODATION) a dead player comes back when its timer
 * hits 0, which would keep `alive` at 4 and make the round-lifecycle assertions
 * below untestable. These tests guard the PERMANENT last-man-standing behaviour
 * - the one that returns the moment TUNE_RESPAWN_TICKS goes back to 0 - so they
 * pin the players dead rather than being deleted for being inconvenient. */
static void kill_all_but_zero(ArenaState* s) {
    for (int i = 1; i < s->num_players; i++) {
        s->players[i].hp = 0;
        s->players[i].state = PSTATE_DEAD;
        s->players[i].timer = 30000;      /* long enough to outlast the test */
    }
}

/* ---- the freeze ------------------------------------------------------- */

static void test_round_end_is_not_terminal(void) {
    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);
    CHECK(s.phase == PHASE_PLAY, "reaches PHASE_PLAY after the countdown");

    kill_all_but_zero(&s);
    run(&s, NEUTRAL, 1);
    CHECK(s.phase == PHASE_ROUND_END,
          "last player standing ends the round (phase=%d)", s.phase);
    CHECK(s.players[0].stocks_won == 1,
          "the survivor is credited the round (stocks_won=%d)", s.players[0].stocks_won);

    /* THE REGRESSION: the phase used to stay here for ever. */
    run(&s, NEUTRAL, TUNE_ROUND_END_TICKS + 2);
    CHECK(s.phase != PHASE_ROUND_END,
          "PHASE_ROUND_END must not be terminal - the round has to restart "
          "(still phase=%d after %d ticks)", s.phase, TUNE_ROUND_END_TICKS + 2);
    CHECK(s.phase == PHASE_COUNTDOWN,
          "restart goes through the countdown (phase=%d)", s.phase);
}

static void test_restart_respawns_everyone(void) {
    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);

    /* Move player 0 off its spawn so the reset has something to undo. */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 40);
    Vec3q moved = s.players[0].pos;
    const ArenaGeom* g = arena_geoms[0];
    CHECK(moved.x != g->spawns[0].x, "player 0 actually left its spawn first");

    kill_all_but_zero(&s);
    run(&s, NEUTRAL, 1 + TUNE_ROUND_END_TICKS + 2);

    for (int i = 0; i < 4; i++) {
        CHECK(s.players[i].state != PSTATE_DEAD,
              "player %d is alive again after the restart (state=%d)", i, s.players[i].state);
        CHECK(s.players[i].hp == TUNE_START_HP,
              "player %d hp restored (%d)", i, s.players[i].hp);
        CHECK(s.players[i].pos.x == g->spawns[i].x && s.players[i].pos.z == g->spawns[i].z,
              "player %d is back on its spawn", i);
    }
    CHECK(s.players[0].stocks_won == 1,
          "stocks_won is MATCH state and survives the round reset (%d)",
          s.players[0].stocks_won);
    for (int b = 0; b < ARENA_MAX_BOMBS; b++)
        CHECK(s.bombs[b].state == BSTATE_FREE, "bomb %d cleared by the reset", b);
}

static void test_input_works_again_after_restart(void) {
    /* The point of the whole fix: you can move again. */
    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);
    kill_all_but_zero(&s);
    run(&s, NEUTRAL, 1 + TUNE_ROUND_END_TICKS + 2 + TUNE_COUNTDOWN_TICKS + 1);
    CHECK(s.phase == PHASE_PLAY, "back in play (phase=%d)", s.phase);

    Vec3q before = s.players[0].pos;
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 30);
    CHECK(s.players[0].pos.x != before.x,
          "player 0 responds to input after a round restart (x %d -> %d)",
          before.x, s.players[0].pos.x);
}

static void test_match_end_stops_restarting(void) {
    /* Rounds restart until someone takes the match; then the sim holds, and
     * ending the MATCH is the session's call. */
    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);
    s.players[0].stocks_won = TUNE_ROUNDS_TO_WIN - 1;   /* one round from the match */
    kill_all_but_zero(&s);
    run(&s, NEUTRAL, 1 + TUNE_ROUND_END_TICKS + 4);
    CHECK(s.players[0].stocks_won == TUNE_ROUNDS_TO_WIN,
          "match point reached (stocks_won=%d)", s.players[0].stocks_won);
    CHECK(s.phase == PHASE_ROUND_END,
          "the sim holds at ROUND_END once the match is won (phase=%d)", s.phase);
}

/* ---- the turn snap ---------------------------------------------------- */

static void test_turn_snaps_from_standstill(void) {
    /* From rest there is no momentum to redirect, so the facing must snap -
     * sweeping made the player slide one way while facing another. */
    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);

    uint16_t before = s.players[0].yaw;
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);       /* one tick, hard right */
    uint16_t after = s.players[0].yaw;
    CHECK(after != before, "a standing turn actually moves the facing");

    int16_t err = (int16_t)(after - (uint16_t)0x4000);  /* +X is yaw 90deg */
    if (err < 0) err = (int16_t)-err;
    CHECK(err < (int16_t)TUNE_TURN_RATE,
          "facing SNAPS to the stick from a standstill in ONE tick "
          "(off by %d BAM, one sweep step is %d)", err, (int)TUNE_TURN_RATE);
}

static void test_turn_snaps_at_speed_too(void) {
    /* v12 DESIGN REVERSAL, on feel-test evidence: the facing now snaps at ANY
     * speed, not just from rest.
     *
     * v9 asserted the opposite here - that a moving turn stayed BOUNDED to
     * TUNE_TURN_RATE - and that was the right test for the decision at the time
     * (A1.3's decomp-authentic gradual turn). The decision changed: in an arena
     * the bounded sweep made every direction change arc, because velocity is
     * rebuilt along a facing that is still rotating. This test is REPLACED, not
     * deleted, so the new decision is the one under guard. */
    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);

    run(&s, arena_input_pack(31, 0, 0, 0, 0), 60);      /* get up to speed */
    CHECK(qlen2(s.players[0].vel.x, s.players[0].vel.z) > Q(0.05),
          "player is genuinely moving before the reversal");

    run(&s, arena_input_pack(-31, 0, 0, 0, 0), 1);      /* reverse the stick */
    int16_t err = (int16_t)(s.players[0].yaw - (uint16_t)0xC000);   /* -X is 270deg */
    if (err < 0) err = (int16_t)-err;
    CHECK(err < (int16_t)TUNE_TURN_RATE,
          "a reversal AT SPEED snaps in one tick (off by %d BAM)", err);
}

static void test_snap_threshold_covers_top_speed(void) {
    /* The threshold must stay ABOVE top speed or gradual turning silently
     * returns and the arc comes back with it. Lowering it below TUNE_RUN_SPEED
     * is the documented way to restore the v9-v11 bounded sweep on purpose. */
    CHECK(TUNE_TURN_SNAP_SPEED > TUNE_RUN_SPEED,
          "snap threshold (%d) must exceed top speed (%d), or turns start "
          "sweeping again and direction changes arc",
          (int)TUNE_TURN_SNAP_SPEED, (int)TUNE_RUN_SPEED);
}

static void test_tumble_skids_and_ends_on_time(void) {
    /* Two things a hit must do, both reported wrong from the 2026-07-27 feel
     * test as "upon hit it pushes me back and keeps the running momentum".
     *
     * 1. SKID, not glide. Nothing else touches velocity during TUMBLE, so the
     *    knockback used to carry at CONSTANT speed for the whole stun -
     *    measured at 4.8 units of travel on an arena of half-width 7.9.
     * 2. Stun for TUMBLE_TICKS, not TUMBLE + INVULN. The damage code sets the
     *    timer to the SUM and the exit waited for zero, so the player was
     *    frozen for 90 ticks (1.5 s) rather than the intended 30.
     *
     * Set up exactly as the blast code does, or the test measures a path the
     * game never takes. */
    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);

    s.players[0].state  = PSTATE_TUMBLE;
    s.players[0].timer  = (uint16_t)(TUNE_TUMBLE_TICKS + TUNE_INVULN_TICKS);
    s.players[0].vel.x  = TUNE_KNOCKBACK;
    s.players[0].vel.z  = 0;
    s.players[0].pos.y  = 0;
    q32 x0 = s.players[0].pos.x;

    int ticks_stunned = 0;
    for (int t = 0; t < TUNE_TUMBLE_TICKS + TUNE_INVULN_TICKS + 8; t++) {
        run(&s, NEUTRAL, 1);
        if (s.players[0].state == PSTATE_TUMBLE) ticks_stunned++;
        else break;
    }

    CHECK(ticks_stunned <= TUNE_TUMBLE_TICKS + 2,
          "stun lasts ~TUNE_TUMBLE_TICKS (%d), not TUMBLE+INVULN (%d) - stunned "
          "for %d ticks", TUNE_TUMBLE_TICKS,
          TUNE_TUMBLE_TICKS + TUNE_INVULN_TICKS, ticks_stunned);
    CHECK(s.players[0].timer > 0,
          "the REMAINDER of the timer stays as invulnerability (timer=%d)",
          s.players[0].timer);

    q32 slid = s.players[0].pos.x - x0;
    if (slid < 0) slid = -slid;
    CHECK(slid < Q(1.0),
          "knockback SKIDS to a stop rather than gliding: slid %d Q (%.2f u), "
          "budget 1.0u", (int)slid, (double)slid / 4096.0);
    CHECK(qlen2(s.players[0].vel.x, s.players[0].vel.z) == 0,
          "horizontal velocity is gone once the stun ends, so knockback is not "
          "laundered into run speed");
}

static void test_respawn_returns_a_dead_player(void) {
    /* TESTING ACCOMMODATION, guarded so it cannot rot: a dead player comes back
     * at its spawn after TUNE_RESPAWN_TICKS, with health and control restored.
     * Skipped entirely when respawn is off, which is the intended long-term
     * setting once there are real opponents. */
    if (TUNE_RESPAWN_TICKS == 0) return;

    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);

    /* move off spawn, then die */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 40);
    s.players[0].hp = 0;
    s.players[0].state = PSTATE_DEAD;
    s.players[0].timer = TUNE_RESPAWN_TICKS;

    run(&s, NEUTRAL, TUNE_RESPAWN_TICKS - 1);
    CHECK(s.players[0].state == PSTATE_DEAD,
          "still dead just before the respawn is due (state=%d)", s.players[0].state);

    run(&s, NEUTRAL, 3);
    const ArenaGeom* g = arena_geoms[0];
    CHECK(s.players[0].state != PSTATE_DEAD, "respawned (state=%d)", s.players[0].state);
    CHECK(s.players[0].hp == TUNE_START_HP, "health restored (%d)", s.players[0].hp);
    CHECK(s.players[0].pos.x == g->spawns[0].x && s.players[0].pos.z == g->spawns[0].z,
          "respawned ON the spawn point");
    CHECK(s.players[0].timer > 0, "arrives with brief invulnerability (timer=%d)",
          s.players[0].timer);

    Vec3q before = s.players[0].pos;
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 20);
    CHECK(s.players[0].pos.x != before.x, "responds to input after respawning");
}

int main(void) {
    test_respawn_returns_a_dead_player();
    test_round_end_is_not_terminal();
    test_restart_respawns_everyone();
    test_input_works_again_after_restart();
    test_match_end_stops_restarting();
    test_turn_snaps_from_standstill();
    test_turn_snaps_at_speed_too();
    test_snap_threshold_covers_top_speed();
    test_tumble_skids_and_ends_on_time();

    if (failures == 0) { printf("ALL ROUND/TURN TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", failures);
    return 1;
}
