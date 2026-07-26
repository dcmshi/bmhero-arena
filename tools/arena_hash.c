/* Scripted-match hash generator — the cross-platform determinism pin.
 * Lifted verbatim from .github/workflows/determinism.yml (2026-07-26) so the
 * generator has ONE home instead of living only inside a CI heredoc.
 * Prints the 8-hex-digit final-state hash and nothing else.
 * Any edit to this file changes the pinned hash: don't touch it casually. */
#include <stdio.h>
#include "../src/arena/arena_sim.h"

int main(void) {
    ArenaState s; ArenaInput in[4];
    arena_init(&s, 0, 4, 0xB0BB1E5);
    uint32_t r = 0xC0FFEE01;
    for (uint32_t t = 0; t < 5400; t++) {
        for (int i = 0; i < 4; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            int sx = (int)(r & 63) - 32;        if (sx < -31) sx = -31;
            int sy = (int)((r >> 6) & 63) - 32; if (sy < -31) sy = -31;
            int set  = ((t + i * 53) % 137) == 0;
            /* The (uint32_t) cast is EXPLICIT only to silence -Wsign-compare:
             * `t` is uint32_t so the comparison already converted the int side
             * to unsigned. Same values, same hash — verified 18fbf1bb. */
            int bomb = ((t + i * 37) % (90 + i * 80)) < (uint32_t)(30 + i * 40);
            in[i] = arena_input_pack(sx, sy, ((r >> 12) & 31) == 0, bomb, set);
        }
        arena_tick(&s, in);
    }
    printf("%08x\n", arena_hash(&s));
    return 0;
}
