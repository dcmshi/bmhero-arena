/* A1.3 movement model-consistency tests. Expectations derive from TUNE_*
 * (guards the model structure, not game-fidelity — see plan Global Constraints). */
#include <stdio.h>
#include <string.h>
#include "../src/arena/arena_sim.h"
#include "../src/arena/arena_tuning.h"

static int failures = 0;
#define CHECK(c, ...) do { if(!(c)){ failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while(0)

/* Drive one player with a fixed stick until yaw settles; count ticks.
 * num_players=2 (not 1): with only 1 player, arena_sim's "last player
 * standing" liveness check (alive<=1) fires on tick 1 and flips phase to
 * PHASE_ROUND_END, which gates off all player_tick movement logic for
 * every subsequent tick — freezing yaw and making this helper never
 * converge. Player 1 gets neutral input and never interferes (spawns far
 * from player 0, no bombs in play). See task-2-report.md for the trace. */
static int ticks_to_turn(int sx, int sy, uint16_t start_yaw) {
    ArenaState s; arena_init(&s, 0, 2, 1);
    s.phase = PHASE_PLAY;                       /* skip countdown */
    s.players[0].yaw = start_yaw;
    ArenaInput in[ARENA_MAX_PLAYERS];
    for (int i=0;i<ARENA_MAX_PLAYERS;i++) in[i]=arena_input_pack(0,0,0,0,0);
    in[0]=arena_input_pack(sx,sy,0,0,0);
    uint16_t target = iatan2(Q(sx), Q(-sy));
    for (int t=1;t<=600;t++){ arena_tick(&s,in);
        if ((uint16_t)(s.players[0].yaw - target) <= 2 ||
            (uint16_t)(target - s.players[0].yaw) <= 2) return t; }
    return -1;
}

static void test_turn_is_gradual(void) {
    /* 180-degree flip from facing +Z(0x8000) to stick full-up(-Z, yaw 0).
     * NOTE: sy=-31 here (not +31) — verified against the sim's own
     * iatan2(Q(ix),Q(-iy)) formula: iy=+31 resolves to target 0x8000,
     * identical to start_yaw (no turn at all, degenerate test); iy=-31
     * resolves to target 0x0000, the genuine 180deg reversal this test
     * is meant to exercise. See task-2-report.md for the derivation. */
    int t = ticks_to_turn(0, -31, 0x8000);
    int expect = 0x8000 / TUNE_TURN_RATE;
    CHECK(t > 1, "turn must be gradual, not instant (got %d ticks)", t);
    CHECK(t >= expect-2 && t <= expect+2, "180deg turn took %d ticks, expected ~%d", t, expect);
}

int main(void){
    test_turn_is_gradual();
    if(!failures){ printf("ALL MOVEMENT TESTS PASSED\n"); return 0; }
    printf("%d FAILURE(S)\n", failures); return 1;
}
