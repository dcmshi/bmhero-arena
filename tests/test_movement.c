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
 * from player 0, no bombs in play). */
/* wx/wy is the WARM-UP stick: held first so the player is genuinely MOVING
 * along start_yaw before the turn is measured.
 *
 * That warm-up is load-bearing since v9. The bounded sweep only applies while
 * there is momentum to redirect; below TUNE_TURN_SNAP_SPEED the facing snaps,
 * because the real walker snaps too (measured against the game's own moveAngle
 * on a stop-then-reverse - arena_tuning.h TUNE_TURN_SNAP_SPEED). Measuring a
 * "gradual turn" from a standstill therefore measures the snap path and reports
 * 1 tick, which is correct behaviour failing a test that never established its
 * own precondition. */
static int ticks_to_turn(int sx, int sy, uint16_t start_yaw, int wx, int wy) {
    ArenaState s; arena_init(&s, 0, 2, 1);
    s.phase = PHASE_PLAY;                       /* skip countdown */
    ArenaInput in[ARENA_MAX_PLAYERS];
    for (int i=0;i<ARENA_MAX_PLAYERS;i++) in[i]=arena_input_pack(0,0,0,0,0);

    /* warm up to speed along start_yaw */
    in[0]=arena_input_pack(wx,wy,0,0,0);
    for (int t=0;t<60;t++) arena_tick(&s,in);
    s.players[0].yaw = start_yaw;                /* exact start facing */

    in[0]=arena_input_pack(sx,sy,0,0,0);
    uint16_t target = iatan2(Q(sx), Q(-sy));
    for (int t=1;t<=600;t++){ arena_tick(&s,in);
        if ((uint16_t)(s.players[0].yaw - target) <= 2 ||
            (uint16_t)(target - s.players[0].yaw) <= 2) return t; }
    return -1;
}

static void test_turn_is_immediate(void) {
    /* v12 DESIGN REVERSAL. This test used to assert the opposite - that a
     * 180-degree flip took ~0x8000/TUNE_TURN_RATE ticks - and that was correct
     * for A1.3's decomp-authentic gradual turn.
     *
     * Feel testing overruled it. In an arena the bounded sweep makes every
     * direction change ARC, because velocity is rebuilt along a facing that is
     * still rotating; it also meant a blast's knockback got re-projected along
     * the old facing while it swung round. Bomberman is an arcade game - you go
     * where you press. TUNE_TURN_SNAP_SPEED now sits above top speed so the
     * facing always snaps.
     *
     * Replaced rather than deleted, so the CURRENT decision is the one guarded.
     * Lowering TUNE_TURN_SNAP_SPEED below TUNE_RUN_SPEED restores the sweep, and
     * this test is what will fail first if that happens by accident. */
    int t = ticks_to_turn(0, -31, 0x8000, 0, +31);   /* warmed up facing +Z */
    CHECK(t == 1, "a 180 at speed must SNAP in one tick (took %d)", t);
}

/* Full-forward hold: measure steady-state speed and ticks to reach 90% of it.
 * num_players=2 (not 1, as the brief's snippet had it): with only 1 player,
 * the "last player standing" liveness check (alive<=1) fires at the end of
 * tick 1 and flips phase to PHASE_ROUND_END, gating off movement for every
 * subsequent tick — the measured "top speed" would just be tick 1's single
 * accel step. Same fix as ticks_to_turn above: a neutral, non-interfering
 * player 1 keeps the match in PHASE_PLAY for the whole run. */
static void run_forward(q32* out_top, int* out_ticks90) {
    ArenaState s; arena_init(&s, 0, 2, 1); s.phase = PHASE_PLAY;
    ArenaInput in[ARENA_MAX_PLAYERS];
    for(int i=0;i<ARENA_MAX_PLAYERS;i++) in[i]=arena_input_pack(0,0,0,0,0);
    in[0]=arena_input_pack(31,0,0,0,0);           /* full +X: the open direction from
                                                   * the corner spawn (arena0 is now the
                                                   * Nitros-matched rectangle; spawn sits
                                                   * near the -X/-Z corner, so drive +X to
                                                   * have room to reach top speed) */
    q32 top=0; int t90=-1;
    for(int t=1;t<=240;t++){ arena_tick(&s,in);
        q32 spd=qlen2(s.players[0].vel.x, s.players[0].vel.z);
        if(spd>top) top=spd;
    }
    /* second pass for t90 against measured top */
    arena_init(&s,0,2,1); s.phase=PHASE_PLAY;
    for(int t=1;t<=240;t++){ arena_tick(&s,in);
        q32 spd=qlen2(s.players[0].vel.x, s.players[0].vel.z);
        if(t90<0 && spd>=qmul(top,Q(0.9))){ t90=t; } }
    *out_top=top; *out_ticks90=t90;
}
static void test_top_speed_and_accel(void) {
    q32 top; int t90; run_forward(&top,&t90);
    /* top speed = TUNE_RUN_SPEED at full magnitude (mag clamps to 1.0) */
    q32 err = top>TUNE_RUN_SPEED ? top-TUNE_RUN_SPEED : TUNE_RUN_SPEED-top;
    CHECK(err <= Q(0.003), "top speed %d != TUNE_RUN_SPEED %d", top, TUNE_RUN_SPEED);
    /* linear accel-to-target reaches ~top in about RUN_SPEED/RUN_ACCEL ticks */
    int expect = TUNE_RUN_SPEED / TUNE_RUN_ACCEL;
    CHECK(t90 > 0 && t90 <= expect+3, "reached 90%% speed in %d ticks, expected <=~%d", t90, expect);
}

/* One jump from rest: measure airtime (ticks from launch to landing) and apex. */
static void test_jump_arc(void) {
    ArenaState s; arena_init(&s,0,1,1); s.phase=PHASE_PLAY;
    ArenaInput in[ARENA_MAX_PLAYERS];
    ArenaInput neutral=arena_input_pack(0,0,0,0,0);
    for(int i=0;i<ARENA_MAX_PLAYERS;i++) in[i]=neutral;
    q32 apex=0; int air=0; int launched=0;
    for(int t=1;t<=240;t++){
        in[0] = (t==1) ? arena_input_pack(0,0,1,0,0) /* jump edge */ : neutral;
        arena_tick(&s, in);
        if(s.players[0].pos.y>0){ launched=1; air++; if(s.players[0].pos.y>apex) apex=s.players[0].pos.y; }
        else if(launched) break;
    }
    /* apex ~ impulse^2/(2*gravity); airtime ~ 2*impulse/gravity (ticks) */
    q32 expect_apex = qdiv(qmul(TUNE_JUMP_IMPULSE,TUNE_JUMP_IMPULSE), qmul(Q(2),TUNE_GRAVITY));
    q32 aerr = apex>expect_apex?apex-expect_apex:expect_apex-apex;
    CHECK(launched, "jump produced no airborne frames");
    CHECK(aerr <= qmul(expect_apex,Q(0.15)), "apex %d vs expected %d (>15%%)", apex, expect_apex);
    int expect_air = (2*TUNE_JUMP_IMPULSE)/TUNE_GRAVITY;
    CHECK(air>=expect_air-4 && air<=expect_air+4, "airtime %d ticks vs ~%d", air, expect_air);
}

int main(void){
    test_turn_is_immediate();
    test_top_speed_and_accel();
    test_jump_arc();
    if(!failures){ printf("ALL MOVEMENT TESTS PASSED\n"); return 0; }
    printf("%d FAILURE(S)\n", failures); return 1;
}
