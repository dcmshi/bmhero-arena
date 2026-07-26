/* Per-tick state trace of the scripted match — turns an opaque hash change into
 * "the runs diverge at tick N, and here is the field that moved".
 *
 * The scripted match is IDENTICAL to tools/arena_hash.c (same seed, same input
 * derivation) so a trace explains that exact pinned hash. Keep the two in sync:
 * if one changes, the other must change the same way.
 *
 * Emits CSV to stdout. Pair with tools/trace-diff.ps1, which builds two variants
 * and reports the first diverging tick.
 *
 *   arena_trace                # 5400 ticks, all players
 *   arena_trace --ticks 600    # shorter run
 *   arena_trace --player 0     # only player 0's columns (narrower diff)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/arena/arena_sim.h"

#define DEFAULT_TICKS 5400

int main(int argc, char** argv) {
    uint32_t ticks = DEFAULT_TICKS;
    int only_player = -1;   /* -1 = all */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) {
            ticks = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--player") && i + 1 < argc) {
            only_player = atoi(argv[++i]);
            if (only_player < 0 || only_player >= 4) {
                fprintf(stderr, "--player must be 0..3\n"); return 2;
            }
        } else {
            fprintf(stderr, "usage: arena_trace [--ticks N] [--player 0..3]\n");
            return 2;
        }
    }

    int lo = (only_player < 0) ? 0 : only_player;
    int hi = (only_player < 0) ? 3 : only_player;

    /* header */
    printf("tick,hash");
    for (int p = lo; p <= hi; p++)
        printf(",p%d_x,p%d_y,p%d_z,p%d_vx,p%d_vz,p%d_yaw,p%d_state,p%d_hp",
               p, p, p, p, p, p, p, p);
    printf(",live_bombs,phase\n");

    ArenaState s; ArenaInput in[4];
    arena_init(&s, 0, 4, 0xB0BB1E5);
    uint32_t r = 0xC0FFEE01;

    for (uint32_t t = 0; t < ticks; t++) {
        for (int i = 0; i < 4; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            int sx = (int)(r & 63) - 32;        if (sx < -31) sx = -31;
            int sy = (int)((r >> 6) & 63) - 32; if (sy < -31) sy = -31;
            int set  = ((t + i * 53) % 137) == 0;
            int bomb = ((t + i * 37) % (90 + i * 80)) < (uint32_t)(30 + i * 40);
            in[i] = arena_input_pack(sx, sy, ((r >> 12) & 31) == 0, bomb, set);
        }
        arena_tick(&s, in);

        int live = 0;
        for (int b = 0; b < ARENA_MAX_BOMBS; b++)
            if (s.bombs[b].state != BSTATE_FREE) live++;

        printf("%u,%08x", (unsigned)s.tick, arena_hash(&s));
        for (int p = lo; p <= hi; p++) {
            const ArenaPlayer* pl = &s.players[p];
            printf(",%d,%d,%d,%d,%d,%u,%u,%u",
                   pl->pos.x, pl->pos.y, pl->pos.z, pl->vel.x, pl->vel.z,
                   (unsigned)pl->yaw, (unsigned)pl->state, (unsigned)pl->hp);
        }
        printf(",%d,%u\n", live, (unsigned)s.phase);
    }
    return 0;
}
