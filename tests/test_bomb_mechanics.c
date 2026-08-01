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
     * bomb lands.
     *
     * The setup has to let the TURN CONVERGE first, or it doesn't isolate what
     * it claims to. Spawn facing is "look at the arena centre" (arena_init), and
     * the throw follows facing BY DESIGN, so while the player is still turning,
     * a stick held on the release tick legitimately moves the bomb — by changing
     * facing, not the arc. The old setup allowed only 3 turn ticks and so was
     * really measuring residual turn; it went red the moment arena 0 became
     * square in v8 and the spawn moved from ~64 degrees off +X to exactly 45.
     *
     * So: hold the facing input long enough for the yaw to settle, stop the
     * player, then ASSERT both preconditions (same facing, and the release tick
     * adds no turn) before comparing distances. */
    ArenaState s;
    const ArenaInput HOLD_STOP = arena_input_pack(0, 0, 0, 1, 0);
    const ArenaInput HOLD_FWD  = arena_input_pack(31, 0, 0, 1, 0);

    /* release with neutral stick */
    start2(&s);
    run(&s, HOLD_STOP, 5);       /* grab */
    run(&s, HOLD_FWD, 30);       /* face +X while holding, until the turn settles */
    run(&s, HOLD_STOP, 40);      /* stop (friction) */
    uint16_t yaw_a = s.players[0].yaw;
    Vec3q origin_a = s.players[0].pos;
    run(&s, NEUTRAL, 120);                            /* release, fly, land */
    /* v15: thrown bombs impact-detonate; detonate() preserves pos, so the
     * stale bomb record still marks the impact point. */
    CHECK(s.bombs[0].state == BSTATE_FREE, "throw A impact-detonated");
    q32 dist_a = bomb_xz_dist(&s.bombs[0], origin_a);

    /* release with the stick held full forward */
    start2(&s);
    run(&s, HOLD_STOP, 5);
    run(&s, HOLD_FWD, 30);
    run(&s, HOLD_STOP, 40);
    uint16_t yaw_b = s.players[0].yaw;
    Vec3q origin_b = s.players[0].pos;
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);     /* release, stick full */
    CHECK(s.players[0].yaw == yaw_b,
          "precondition: turn has converged, so the release tick adds no turn "
          "(yaw %u -> %u)", yaw_b, s.players[0].yaw);
    run(&s, NEUTRAL, 120);
    CHECK(s.bombs[0].state == BSTATE_FREE, "throw B impact-detonated");
    q32 dist_b = bomb_xz_dist(&s.bombs[0], origin_b);

    CHECK(yaw_a == yaw_b, "precondition: both throws launch from the same facing "
                          "(a=%u b=%u)", yaw_a, yaw_b);
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
    /* v15 REDESIGN: the old setup floored 4 bombs with a first spread, but
     * thrown bombs now impact-detonate, so nothing stays live long enough to
     * crowd the cap. SET bombs (unchanged rules) fill it instead: 4 settled
     * bombs + a spread of 4 must clamp to cap (6) - 4 = 2 launches. */
    ArenaState s;
    start2(&s);
    /* pre-place 4 settled bombs directly (fresh fuses; spaced so nothing
     * chains or gets kicked) - direct setup like the other contact tests,
     * immune to fuse-vs-choreography timing */
    for (int k = 0; k < 4; k++) {
        s.bombs[k].state = BSTATE_SETTLED;
        s.bombs[k].owner = 0;
        s.bombs[k].fuse  = TUNE_FUSE_TICKS;
        s.bombs[k].bounced = 0;
        s.bombs[k].pos.x = Q(2.0) + k * Q(1.5);
        s.bombs[k].pos.y = 0;
        s.bombs[k].pos.z = Q(3.0);
    }
    s.players[0].live_bombs = 4;
    run(&s, arena_input_pack(0, 0, 0, 1, 0), TUNE_SPREAD_TICKS + 5); /* arm */
    run(&s, NEUTRAL, 1);                                             /* release */
    CHECK(s.players[0].live_bombs == TUNE_MAX_LIVE_BOMBS,
          "cap reached exactly (live=%d)", s.players[0].live_bombs);
    int airborne = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_AIRBORNE) airborne++;
    CHECK(airborne == 2, "spread clamped to 2 bombs (got %d)", airborne);
}

static void test_throw_impact_detonates(void) {
    /* v15: a thrown bomb explodes on contact with anything and never settles.
     * (a) floor: fly the whole arc, assert the blast is born the tick the
     *     flight ends, at ground level. */
    ArenaState s;
    start2(&s);
    /* aim at OPEN FLOOR: the spawn-facing throw reaches a wall before the
     * floor (first run of this test: wall hit at z=-7.62, y=0.39). Face +X
     * and let the turn converge, exactly like test_throw_fixed_arc. */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 5);      /* grab */
    run(&s, arena_input_pack(31, 0, 0, 1, 0), 30);    /* face +X while holding */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 40);     /* stop (friction) */
    run(&s, NEUTRAL, 1);                              /* release */
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_AIRBORNE) { bi = i; break; }
    CHECK(bi >= 0, "throw launches an airborne bomb");
    if (bi < 0) return;
    int landed = 0;
    for (int t = 0; t < 200 && !landed; t++) {
        run(&s, NEUTRAL, 1);
        uint8_t st = s.bombs[bi].state;
        CHECK(st != BSTATE_SETTLED, "thrown bomb never settles (tick %d)", t);
        if (st != BSTATE_AIRBORNE) {
            landed = 1;
            CHECK(st == BSTATE_FREE, "impact -> detonated (state=%d)", st);
            CHECK(s.bombs[bi].pos.y == 0,
                  "floor impact at ground level (y=%d tick=%d x=%d z=%d)",
                  s.bombs[bi].pos.y, t, s.bombs[bi].pos.x, s.bombs[bi].pos.z);
            int fresh = 0;
            for (int k = 0; k < ARENA_MAX_BLASTS; k++)
                if (s.blasts[k].ttl >= TUNE_BLAST_TTL - 1 &&
                    s.blasts[k].center.x == s.bombs[bi].pos.x &&
                    s.blasts[k].center.z == s.bombs[bi].pos.z) fresh = 1;
            CHECK(fresh, "a blast is born at the impact point");
        }
    }
    CHECK(landed, "the flight ends within 200 ticks");

    /* (b) wall: an airborne bomb crossing into a wall detonates IN THE AIR
     *    (blast center above the floor), not after falling to the ground.
     *    Direct setup, like the other contact tests: high enough that gravity
     *    can't floor it before the wall does. */
    start2(&s);
    s.bombs[0].state  = BSTATE_AIRBORNE;
    s.bombs[0].owner  = 0;
    s.bombs[0].bounced = 0;
    /* wall (~7.6u) must arrive before the floor: at Q(0.6)/tick the wall is
     * ~13 ticks out; gravity floors it at ~18 (y0=2, vy=+0.05, g=0.0175) */
    s.bombs[0].pos.x = 0; s.bombs[0].pos.y = Q(2.0); s.bombs[0].pos.z = 0;
    s.bombs[0].vel.x = Q(0.60); s.bombs[0].vel.y = Q(0.05); s.bombs[0].vel.z = 0;
    s.players[0].live_bombs = 1;
    int hit = 0;
    for (int t = 0; t < 60 && !hit; t++) {
        run(&s, NEUTRAL, 1);
        if (s.bombs[0].state != BSTATE_AIRBORNE) {
            hit = 1;
            CHECK(s.bombs[0].state == BSTATE_FREE, "wall impact detonates");
            CHECK(s.bombs[0].pos.y > 0,
                  "detonated in the air on wall contact (y=%d)", s.bombs[0].pos.y);
        }
    }
    CHECK(hit, "the bomb reached the wall within 60 ticks");

    /* (c) OPEN-FLOOR impact must detonate ON contact, not glide. Regression
     * found by eye in feel round 4 (v15): collide_static's floor clamp zeroes
     * vel.y before the floor check reads it, and the wall compare ignored y -
     * a bomb landing on open floor became a floor-glider at constant
     * horizontal speed until it found a wall. Sub-test (a) and the mode-11
     * gate both stayed green because the arena is small enough that the wall
     * always arrived inside their bounds - the discriminator is WHERE (and
     * how soon) the blast is born. Ballistic here: fall from y=1.4 at
     * g=0.0175 is ~13 ticks, range ~13 x 0.18 = 2.3u; a glide only ends at
     * the wall (~7.3u, ~40 ticks). */
    start2(&s);
    s.bombs[0].state   = BSTATE_AIRBORNE;
    s.bombs[0].owner   = 0;
    s.bombs[0].bounced = 0;
    s.bombs[0].pos.x = 0;         s.bombs[0].pos.y = Q(1.4); s.bombs[0].pos.z = 0;
    s.bombs[0].vel.x = Q(0.18);   s.bombs[0].vel.y = 0;      s.bombs[0].vel.z = 0;
    s.players[0].live_bombs = 1;
    int floored = 0;
    for (int t = 0; t < 60 && !floored; t++) {
        run(&s, NEUTRAL, 1);
        if (s.bombs[0].state != BSTATE_AIRBORNE) {
            floored = 1;
            CHECK(s.bombs[0].state == BSTATE_FREE, "open-floor impact detonates");
            CHECK(t <= 20, "detonates ON impact, no glide (tick %d)", t);
            CHECK(s.bombs[0].pos.x < Q(4.0),
                  "detonates at ballistic range, not at a wall (x=%d)",
                  s.bombs[0].pos.x);
            CHECK(s.bombs[0].pos.y == 0, "at ground level (y=%d)", s.bombs[0].pos.y);
        }
    }
    CHECK(floored, "open-floor impact detonates within 60 ticks");
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

    /* Walk-in kick set up DIRECTLY (explicit position + velocity) instead of the old
     * step-away/run-back arena choreography, which was calibrated to the pre-A1.3
     * movement + the old 12x12 square arena and broke when both changed (TUNE_VERSION
     * 5, rectangular Nitros-matched geom). The mechanic under test is unchanged
     * (arena_sim.c BSTATE_SETTLED): a moving, grounded, grace-cleared player touching
     * a settled bomb kicks it into a slide. Bomb at center (clear of walls); P0 just
     * -Z of it, moving +Z into it. */
    s.bombs[bi].pos.x = 0; s.bombs[bi].pos.z = 0;
    s.bombs[bi].bounced = 0;                              /* clear grace (verified above) */
    s.players[0].pos.x = 0; s.players[0].pos.z = -Q(0.6); s.players[0].pos.y = 0;
    s.players[0].yaw = 0x8000;                            /* face +Z (toward the bomb) */
    s.players[0].vel.x = 0; s.players[0].vel.z = Q(0.10); /* moving +Z > TUNE_KICK_MIN_VEL */
    s.players[0].state = PSTATE_RUN;
    run(&s, NEUTRAL, 1);                                 /* touch while moving -> kick */
    CHECK(s.bombs[bi].state == BSTATE_SLIDING,
          "walk-in kicks the bomb (state=%d)", s.bombs[bi].state);
    /* The kicker's identity is published in `bounced` as idx+1. This is grace
     * bookkeeping for the sim, but the render bridge reads it to know WHOSE kick
     * animation to play - the SETTLED->SLIDING edge alone doesn't say who did it.
     * Asserted here so a change to the grace encoding can't silently make the
     * fork play a kick pose on the wrong bomber. */
    CHECK(s.bombs[bi].bounced == 1,
          "kicker published as bounced=idx+1 (got %d)", (int)s.bombs[bi].bounced);
    /* kicked +Z: slides into the +Z boundary wall and detonates on contact */
    run(&s, NEUTRAL, 60);
    CHECK(s.bombs[bi].state == BSTATE_FREE, "kicked bomb detonated at wall");
    CHECK(s.players[0].live_bombs == 0, "live count back to 0");
}

static void test_kick_hits_player(void) {
    ArenaState s;
    start4(&s);
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* set at the P0 feet */
    run(&s, NEUTRAL, 1);
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SETTLED) { bi = i; break; }
    CHECK(bi >= 0, "set places a settled bomb");
    if (bi < 0) return;
    /* A bomb sliding into a player must detonate on contact and blast-damage them.
     * Set the slide up directly (robust vs geometry/movement tuning); the kick
     * TRANSITION itself is covered by test_set_and_walkin_kick_wall. */
    s.players[3].pos.x = 0; s.players[3].pos.z = Q(2.0); s.players[3].pos.y = 0;
    s.bombs[bi].pos.x = 0; s.bombs[bi].pos.z = 0; s.bombs[bi].bounced = 0;
    s.bombs[bi].state = BSTATE_SLIDING;
    s.bombs[bi].vel.x = 0; s.bombs[bi].vel.z = TUNE_KICK_SPEED;   /* slide +Z into P3 */
    CHECK(s.bombs[bi].state == BSTATE_SLIDING, "bomb sliding toward P3");
    run(&s, NEUTRAL, 40);
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
    /* v17: the bomb DROPS from the hands and settles on landing (feel round
     * 5: "it shouldn't immediately appear on the floor, it should drop") -
     * the vanilla air-set births the bomb airborne (oracle airsetR, Y~185). */
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_FALLING) { bi = i; break; }
    CHECK(bi >= 0, "mid-air set DROPS a bomb (FALLING, not floor-teleport)");
    if (bi < 0) return;
    CHECK(s.bombs[bi].pos.y > 0,
          "the dropped bomb starts at the player, not the floor (y=%d)",
          s.bombs[bi].pos.y);
    int landed = 0;
    for (int t = 0; t < 120 && !landed; t++) {
        run(&s, NEUTRAL, 1);
        uint8_t st = s.bombs[bi].state;
        if (st == BSTATE_SETTLED) {
            landed = 1;
            CHECK(s.bombs[bi].pos.y == 0, "settles ON the floor");
        } else {
            CHECK(st == BSTATE_FALLING,
                  "a falling set never impact-detonates (state=%d t=%d)", st, t);
            if (st != BSTATE_FALLING) return;
        }
    }
    CHECK(landed, "the dropped bomb settles within 120 ticks");
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

static void test_no_jump_while_charging(void) {
    /* v18: once the hold crosses TUNE_SPREAD_TICKS the walker is winding up
     * the 4-bomb spread (windmilling) - vanilla does not let you jump out of
     * that (feel round 8). A jump DURING a short hold stays legal (vanilla
     * jump-throws). */
    ArenaState s;
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 3);              /* grab */
    run(&s, arena_input_pack(0, 0, 1, 1, 0), 1);              /* jump while holding */
    CHECK(s.players[0].state == PSTATE_JUMP,
          "jump WHILE HOLDING (short hold) is legal - vanilla jump-throws");
    /* land, then charge past the spread threshold and try to jump */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 60);             /* land, keep holding */
    CHECK(s.players[0].pos.y == 0, "back on the ground");
    run(&s, arena_input_pack(0, 0, 0, 1, 0), TUNE_SPREAD_TICKS);  /* charge */
    run(&s, arena_input_pack(0, 0, 1, 1, 0), 1);              /* jump press mid-charge */
    CHECK(s.players[0].state != PSTATE_JUMP && s.players[0].pos.y == 0,
          "no jump while CHARGING the spread (state=%d y=%d)",
          s.players[0].state, s.players[0].pos.y);
}

int main(void) {
    test_throw_fixed_arc();
    test_throw_impact_detonates();
    test_spread();
    test_spread_cap();
    test_set_and_walkin_kick_wall();
    test_kick_hits_player();
    test_fuse_pops_mid_slide();
    test_set_works_midair();
    test_set_ignored_while_holding();
    test_no_jump_while_charging();
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("bomb_mechanics: ALL TESTS PASSED\n");
    return 0;
}
