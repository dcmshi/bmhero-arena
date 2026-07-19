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

static void test_throw_tilt(void) {
    /* Both throws aim +X from spawn (-4.5,-4.5): ~10 clear units along
     * z=-4.5 (walls at +/-6, pillars only within |z|<=2.5), so neither
     * trajectory clips geometry and distances compare cleanly. */
    ArenaState s;
    /* neutral-stick release = short lob */
    start2(&s);
    Vec3q origin = s.players[0].pos;
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 5);      /* grab */
    run(&s, arena_input_pack(31, 0, 0, 1, 0), 3);     /* face +X while holding */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 2);      /* stick back to neutral
                                                         (deadzone keeps yaw) */
    run(&s, NEUTRAL, 120);                            /* release neutral: lob */
    CHECK(s.bombs[0].state == BSTATE_SETTLED, "lob settled");
    q32 lob = bomb_xz_dist(&s.bombs[0], origin);

    /* full-tilt release = farther throw */
    start2(&s);
    origin = s.players[0].pos;
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 5);      /* grab */
    run(&s, arena_input_pack(31, 0, 0, 1, 0), 3);     /* face +X, full tilt */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);     /* release at full tilt */
    run(&s, NEUTRAL, 120);
    CHECK(s.bombs[0].state == BSTATE_SETTLED, "throw settled");
    q32 thrown = bomb_xz_dist(&s.bombs[0], origin);
    CHECK(thrown > lob + Q(0.5),
          "full tilt lands farther than neutral lob (thrown=%d lob=%d)", thrown, lob);
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

static void test_set_and_kick_wall(void) {
    ArenaState s;
    start2(&s);
    /* set: bit-14 edge with nothing nearby places a bomb at the feet */
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

    /* face -Z (wall 1.5 units away at spawn), then kick */
    run(&s, arena_input_pack(0, -31, 0, 0, 0), 1);    /* turn */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* kick press */
    CHECK(s.bombs[bi].state == BSTATE_SLIDING, "second set-press kicks");
    /* slides into the -Z boundary wall and detonates on contact */
    run(&s, NEUTRAL, 40);
    CHECK(s.bombs[bi].state == BSTATE_FREE, "kicked bomb detonated at wall");
    CHECK(s.players[0].live_bombs == 0, "live count back to 0");
}

static void test_kick_hits_player(void) {
    ArenaState s;
    start4(&s);                       /* P3 spawns at (+4.5, -4.5) */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* set */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);     /* face +X toward P3 */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* kick */
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SLIDING) { bi = i; break; }
    CHECK(bi >= 0, "bomb sliding toward P3");
    if (bi < 0) return;
    run(&s, NEUTRAL, 80);             /* 9 units at TUNE_KICK_SPEED ~= 65 ticks */
    CHECK(s.bombs[bi].state == BSTATE_FREE, "detonated on player contact");
    CHECK(s.players[3].hp < TUNE_START_HP, "P3 took blast damage");
}

static void test_fuse_pops_mid_slide(void) {
    ArenaState s;
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* set */
    run(&s, NEUTRAL, TUNE_FUSE_TICKS - 10);           /* burn fuse down */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);     /* face +X (10 units clear) */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* kick */
    run(&s, NEUTRAL, 20);
    /* far from any obstacle after ~2.8 units: only the fuse can have popped */
    int sliding = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SLIDING) sliding++;
    CHECK(sliding == 0, "fuse detonates a sliding bomb");
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
    test_throw_tilt();
    test_spread();
    test_spread_cap();
    test_set_and_kick_wall();
    test_kick_hits_player();
    test_fuse_pops_mid_slide();
    test_set_ignored_while_holding();
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("bomb_mechanics: ALL TESTS PASSED\n");
    return 0;
}
