/* Deterministic behavior tests for Hero-authentic bomb mechanics.
 * Drives arena_tick with scripted inputs; sim-only, no floats. */
#include <stdio.h>
#include <string.h>
#include "../src/arena/arena_sim.h"
#include "../src/arena/arena_tuning.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define NEUTRAL arena_input_pack(0, 0, 0, 0, 0)

/* run n ticks: player 0 gets in0, everyone else neutral */
static void run(ArenaState* s, ArenaInput in0, int n) {
    ArenaInput in[ARENA_MAX_PLAYERS] = { in0, NEUTRAL, NEUTRAL, NEUTRAL };
    for (int t = 0; t < n; t++) arena_tick(s, in);
}

/* fresh match with countdown skipped (play phase active) */
static void start2(ArenaState* s) {
    arena_init(s, 0, 2, 0xBEEF);
    run(s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);
}
static void start4(ArenaState* s) {
    arena_init(s, 0, 4, 0xBEEF);
    run(s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);
}

static q32 bomb_xz_dist(const ArenaBomb* b, Vec3q from) {
    return qlen2(b->pos.x - from.x, b->pos.z - from.z);
}

static void test_throw_fixed_arc(void) {
    /* Decomp-verified (bmhero 69AA0.c): the throw is a fixed launch from
     * facing + constants — stick tilt at release must NOT change where the
     * bomb lands. Both throws aim +X from spawn (-4.5,-4.5): ~10 clear
     * units along z=-4.5. Player is stopped before release in both cases;
     * the only difference is the stick state on the release tick. */
    ArenaState s;
    /* release with neutral stick */
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 5);      /* grab */
    run(&s, arena_input_pack(31, 0, 0, 1, 0), 3);     /* face +X while holding */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 30);     /* stop (friction) */
    Vec3q origin_a = s.players[0].pos;
    run(&s, NEUTRAL, 120);                            /* release, fly, land */
    CHECK(s.bombs[0].state == BSTATE_SETTLED, "throw A settled");
    q32 dist_a = bomb_xz_dist(&s.bombs[0], origin_a);

    /* release with the stick held full forward */
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 5);
    run(&s, arena_input_pack(31, 0, 0, 1, 0), 3);
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 30);
    Vec3q origin_b = s.players[0].pos;
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);     /* release, stick full */
    run(&s, NEUTRAL, 120);
    CHECK(s.bombs[0].state == BSTATE_SETTLED, "throw B settled");
    q32 dist_b = bomb_xz_dist(&s.bombs[0], origin_b);

    /* identical launch: stick input cannot alter the arc. Allow a few Q of
     * slack for the 1 walking tick before B's bomb leaves the hand. */
    CHECK(qabs(dist_a - dist_b) < Q(0.15),
          "fixed arc: stick at release does not change distance (a=%d b=%d)",
          dist_a, dist_b);
}

static void test_spread(void) {
    ArenaState s;
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 1, 0), TUNE_SPREAD_TICKS + 5); /* arm */
    run(&s, NEUTRAL, 1);                                             /* release */
    int airborne = 0;
    q32 seen_vx[4]; int n_vx = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_AIRBORNE) {
            if (n_vx < 4) seen_vx[n_vx++] = s.bombs[i].vel.x;
            airborne++;
        }
    CHECK(airborne == 4, "spread throws 4 bombs (got %d)", airborne);
    CHECK(s.players[0].live_bombs == 4, "live_bombs == 4 (got %d)",
          s.players[0].live_bombs);
    int distinct = 1;
    for (int a = 0; a < n_vx && distinct; a++)
        for (int b = a + 1; b < n_vx; b++)
            if (seen_vx[a] == seen_vx[b]) distinct = 0;
    CHECK(distinct, "fan headings are distinct (vel.x all differ)");
}

static void test_spread_cap(void) {
    ArenaState s;
    start2(&s);
    /* first spread: 4 live bombs on the floor */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), TUNE_SPREAD_TICKS + 5);
    run(&s, NEUTRAL, 1);
    /* second spread armed while 4 still live: cap 6 clamps it to 2 */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), TUNE_SPREAD_TICKS + 5);
    run(&s, NEUTRAL, 1);
    CHECK(s.players[0].live_bombs == TUNE_MAX_LIVE_BOMBS,
          "cap reached exactly (live=%d)", s.players[0].live_bombs);
    int airborne = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_AIRBORNE) airborne++;
    CHECK(airborne == 2, "second spread clamped to 2 bombs (got %d)", airborne);
}

static void test_set_and_walkin_kick_wall(void) {
    ArenaState s;
    start2(&s);
    /* set: bit-14 edge places a bomb at the feet */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);
    run(&s, NEUTRAL, 1);
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SETTLED) { bi = i; break; }
    CHECK(bi >= 0, "set places a settled bomb");
    CHECK(s.players[0].live_bombs == 1, "set counts toward the cap");
    if (bi < 0) return;
    CHECK(bomb_xz_dist(&s.bombs[bi], s.players[0].pos) < Q(0.2),
          "bomb set at the feet");

    /* setter standing on it must NOT kick it (grace until stepped clear) */
    run(&s, NEUTRAL, 30);
    CHECK(s.bombs[bi].state == BSTATE_SETTLED, "no insta-kick while standing on it");

    /* step away (+Z), then run back (-Z) into it: walk-in kick toward wall.
     * A1.3 (no-strafe scalar-speed-along-facing, ~7x lower TUNE_RUN_ACCEL
     * than the old placeholder) needs much longer than the old instant-snap
     * model to (a) physically clear the setter's grace radius (touch+0.1)
     * while accelerating from rest, and (b) turn the full 180deg (gradual,
     * TUNE_TURN_RATE-limited) and carve back into contact — the player now
     * arcs through the turn (velocity is locked to facing) rather than
     * reversing in place. 60/60 ticks (was 20/30) measured empirically
     * (tools/scratch trace) to clear grace and land the kick; still asserts
     * the same kick mechanics (contact + TUNE_KICK_MIN_VEL), just gives the
     * slower player time to get there. */
    run(&s, arena_input_pack(0, 31, 0, 0, 0), 60);    /* walk +Z, clears grace */
    run(&s, arena_input_pack(0, -31, 0, 0, 0), 60);   /* run -Z back into bomb */
    CHECK(s.bombs[bi].state == BSTATE_SLIDING || s.bombs[bi].state == BSTATE_FREE,
          "walk-in kicks the bomb (state=%d)", s.bombs[bi].state);
    /* slides into the -Z boundary wall and detonates on contact */
    run(&s, NEUTRAL, 40);
    CHECK(s.bombs[bi].state == BSTATE_FREE, "kicked bomb detonated at wall");
    CHECK(s.players[0].live_bombs == 0, "live count back to 0");
}

static void test_kick_hits_player(void) {
    ArenaState s;
    start4(&s);                       /* P3 spawns at (+4.5, -4.5) */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* set at the feet */
    run(&s, NEUTRAL, 1);
    /* A1.3's slower accel + gradual 180deg turn need more travel/turn time
     * than the old instant-snap model to clear grace and land the kick
     * (see test_set_and_walkin_kick_wall for the full derivation); 40/60
     * (was 20/30) measured empirically to still be genuinely BSTATE_SLIDING
     * (not yet exploded) right after the run-back, leaving room to travel
     * on and hit P3 during the wait below. */
    run(&s, arena_input_pack(-31, 0, 0, 0, 0), 40);   /* step away -X */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 60);    /* run +X into the bomb */
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SLIDING) { bi = i; break; }
    CHECK(bi >= 0, "bomb sliding toward P3");
    if (bi < 0) return;
    run(&s, NEUTRAL, 90);             /* ~9 units at TUNE_KICK_SPEED */
    CHECK(s.bombs[bi].state == BSTATE_FREE, "detonated on player contact");
    CHECK(s.players[3].hp < TUNE_START_HP, "P3 took blast damage");
}

static void test_fuse_pops_mid_slide(void) {
    ArenaState s;
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* set */
    /* A1.3: the step-away + run-back round trip now needs ~100 ticks of
     * fuse budget (slower accel to clear grace + gradual 180deg turn —
     * see test_set_and_walkin_kick_wall), which doesn't fit under the old
     * 89-tick pre-burn against a 150-tick fuse. Burn less up front (30, was
     * 89) so there's still genuine fuse left (measured: kicked around fuse
     * ~51) when the walk-in kick lands; it then keeps burning while
     * SLIDING and pops before the bomb could reach a wall (kick travel in
     * the remaining budget stays well under the ~10.5-unit clear run) —
     * still exercises "only the fuse can have popped it". */
    run(&s, NEUTRAL, 30);                             /* burn some of the fuse */
    run(&s, arena_input_pack(-31, 0, 0, 0, 0), 40);   /* step away -X */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 60);    /* run +X into the bomb */
    run(&s, NEUTRAL, 30);
    /* +X has ~10 clear units; only the fuse can have popped it */
    int sliding = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SLIDING) sliding++;
    CHECK(sliding == 0, "fuse detonates a sliding bomb");
}

static void test_set_works_midair(void) {
    ArenaState s;
    start2(&s);
    /* jump, then set at the top of the arc */
    run(&s, arena_input_pack(0, 0, 1, 0, 0), 2);      /* jump press */
    run(&s, arena_input_pack(0, 0, 0, 0, 0), 8);      /* rising */
    CHECK(s.players[0].pos.y > 0, "player is airborne");
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* set mid-air */
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SETTLED) { bi = i; break; }
    CHECK(bi >= 0, "mid-air set places a bomb");
    if (bi >= 0)
        CHECK(s.bombs[bi].pos.y == 0, "bomb lands on the ground below");
}

static void test_set_ignored_while_holding(void) {
    ArenaState s;
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 3);      /* grab + hold */
    run(&s, arena_input_pack(0, 0, 0, 1, 1), 1);      /* set press while holding */
    int settled = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SETTLED) settled++;
    CHECK(settled == 0 && s.players[0].live_bombs == 1,
          "set is ignored while holding a bomb");
}

int main(void) {
    test_throw_fixed_arc();
    test_spread();
    test_spread_cap();
    test_set_and_walkin_kick_wall();
    test_kick_hits_player();
    test_fuse_pops_mid_slide();
    test_set_works_midair();
    test_set_ignored_while_holding();
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("bomb_mechanics: ALL TESTS PASSED\n");
    return 0;
}
