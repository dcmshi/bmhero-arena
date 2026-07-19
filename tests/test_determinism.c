/* A0 determinism gates:
 *   T1 replay:    same seed+inputs twice  -> bit-identical per-tick hashes
 *   T2 rollback:  continuous save/restore/re-sim (GekkoNet stress-session
 *                 style) -> re-simmed hashes match first-pass hashes
 *   T3 snapshot:  memcpy save/load round-trips exactly
 *   T4 liveness:  scripted 4P match actually produces bombs/blasts/damage
 * Inputs come from a scripted PRNG (separate from the sim's RNG) so runs are
 * reproducible and bomb/jump edges occur at realistic rates. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/arena/arena_sim.h"
#include "../src/arena/arena_tuning.h"

#define TICKS      (60 * 90)    /* 90 seconds — crosses into sudden death */
#define ROLL_DEPTH 8            /* rollback window */

static uint32_t script_rng;
static uint32_t srand32(void) {
    uint32_t x = script_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return script_rng = x;
}

/* Semi-realistic input script: held directions, occasional jump/bomb edges. */
static void script_inputs(ArenaInput out[ARENA_MAX_PLAYERS], uint32_t tick) {
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++) {
        uint32_t r = srand32();
        int sx = (int)(r & 63) - 32; if (sx < -31) sx = -31;
        int sy = (int)((r >> 6) & 63) - 32; if (sy < -31) sy = -31;
        int jump = ((r >> 12) & 31) == 0;               /* ~2/64 ticks */
        /* per-player hold lengths; player 3 holds past TUNE_SPREAD_TICKS so
         * the spread path is exercised (hash-covered) every cycle */
        int bomb = (int)((tick + (uint32_t)(i * 37)) % (uint32_t)(90 + i * 80))
                   < (30 + i * 40);
        int set  = ((tick + i * 53) % 137) == 0;        /* sparse set presses */
        out[i] = arena_input_pack(sx, sy, jump, bomb, set);
    }
}

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static uint32_t   hashes[TICKS];
    static ArenaInput inputs[TICKS][ARENA_MAX_PLAYERS];
    ArenaState s, s2;

    /* Pre-generate the full input script once. */
    script_rng = 0xC0FFEE01;
    for (uint32_t t = 0; t < TICKS; t++) script_inputs(inputs[t], t);

    /* ---- pass 1: record hashes ---- */
    arena_init(&s, 0, 4, 0xB0BB1E5);
    for (uint32_t t = 0; t < TICKS; t++) { arena_tick(&s, inputs[t]); hashes[t] = arena_hash(&s); }

    /* T4 liveness: did anything happen? */
    {
        int total_stocks = 0, total_hp = 0;
        for (int i = 0; i < 4; i++) {
            total_stocks += s.players[i].stocks_won;
            total_hp     += s.players[i].hp;
        }
        CHECK(total_hp < 4 * TUNE_START_HP || total_stocks > 0,
              "liveness: no damage dealt in 90s scripted match (hp=%d stocks=%d)",
              total_hp, total_stocks);
        printf("liveness: end hp=%d stocks=%d phase=%d\n", total_hp, total_stocks, s.phase);
    }

    /* ---- T1 replay ---- */
    arena_init(&s, 0, 4, 0xB0BB1E5);
    for (uint32_t t = 0; t < TICKS; t++) {
        arena_tick(&s, inputs[t]);
        if (arena_hash(&s) != hashes[t]) { CHECK(0, "replay diverged at tick %u", t); break; }
    }

    /* ---- T3 snapshot round-trip ---- */
    arena_save(&s2, &s);
    CHECK(memcmp(&s2, &s, sizeof(ArenaState)) == 0, "snapshot round-trip mismatch");

    /* ---- T2 rollback stress: every 3 ticks, roll back 1..ROLL_DEPTH and re-sim ---- */
    {
        ArenaState ring[ROLL_DEPTH + 1];
        arena_init(&s, 0, 4, 0xB0BB1E5);
        arena_save(&ring[0], &s);
        for (uint32_t t = 0; t < TICKS; t++) {
            arena_tick(&s, inputs[t]);
            arena_save(&ring[(t + 1) % (ROLL_DEPTH + 1)], &s);
            if (t >= ROLL_DEPTH && (t % 3) == 0) {
                uint32_t back = 1 + (t % ROLL_DEPTH);          /* 1..8 */
                uint32_t from = t - back;                       /* state after tick 'from' */
                arena_load(&s2, &ring[(from + 1) % (ROLL_DEPTH + 1)]);
                for (uint32_t rt = from + 1; rt <= t; rt++) arena_tick(&s2, inputs[rt]);
                if (arena_hash(&s2) != hashes[t]) {
                    CHECK(0, "rollback re-sim diverged at tick %u (depth %u)", t, back);
                    break;
                }
            }
        }
    }

    printf("state size: %zu bytes; %d tick(s) simulated x3 passes\n",
           sizeof(ArenaState), TICKS);
    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d FAILURE(S)\n", failures);
    return 1;
}
