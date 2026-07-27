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
 * a bomb test; the round logic only cares that `alive` drops to 1. */
static void kill_all_but_zero(ArenaState* s) {
    for (int i = 1; i < s->num_players; i++) {
        s->players[i].hp = 0;
        s->players[i].state = PSTATE_DEAD;
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

static void test_turn_still_sweeps_at_speed(void) {
    /* At speed the bounded sweep must survive: it is what makes turning
     * redirect momentum instead of teleporting the facing, and the 6deg/frame
     * rate was feel-confirmed on 2026-07-27. */
    ArenaState s;
    arena_init(&s, 0, 4, 0xBEEF);
    run(&s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);

    run(&s, arena_input_pack(31, 0, 0, 0, 0), 60);      /* get up to speed */
    CHECK(qlen2(s.players[0].vel.x, s.players[0].vel.z) > TUNE_TURN_SNAP_SPEED,
          "player is genuinely moving before the reversal");

    uint16_t before = s.players[0].yaw;
    run(&s, arena_input_pack(-31, 0, 0, 0, 0), 1);      /* reverse the stick */
    int16_t step = (int16_t)(s.players[0].yaw - before);
    if (step < 0) step = (int16_t)-step;
    CHECK(step <= (int16_t)TUNE_TURN_RATE,
          "a moving turn is still BOUNDED to TUNE_TURN_RATE (stepped %d, max %d)",
          step, (int)TUNE_TURN_RATE);
    CHECK(step > 0, "a moving turn still turns");
}

static void test_snap_threshold_is_below_top_speed(void) {
    /* If the snap threshold ever crept up to normal running speed, every turn
     * would snap and the gradual-turn model would silently vanish. */
    CHECK(TUNE_TURN_SNAP_SPEED < TUNE_RUN_SPEED / 4,
          "snap threshold (%d) must stay well under top speed (%d) or the "
          "bounded turn stops existing", (int)TUNE_TURN_SNAP_SPEED, (int)TUNE_RUN_SPEED);
}

int main(void) {
    test_round_end_is_not_terminal();
    test_restart_respawns_everyone();
    test_input_works_again_after_restart();
    test_match_end_stops_restarting();
    test_turn_snaps_from_standstill();
    test_turn_still_sweeps_at_speed();
    test_snap_threshold_is_below_top_speed();

    if (failures == 0) { printf("ALL ROUND/TURN TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", failures);
    return 1;
}
