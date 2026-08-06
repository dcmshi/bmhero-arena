/* The scripted 4P match behind the pinned hash — extracted from
 * tools/arena_hash.c (2026-08-05) so the netplay version handshake can
 * replay it. MUST stay bit-identical to the pin's generator: the gate's
 * [hash] check (fbdb0d08 @ TUNE_VERSION 21) enforces that on every run.
 * Header-only, no floats, reads nothing outside the sim API. */
#ifndef ARENA_SCRIPT_H
#define ARENA_SCRIPT_H

#include "arena_sim.h"

static inline uint32_t arena_scripted_match_hash(void) {
    ArenaState s; ArenaInput in[4];
    arena_init(&s, 0, 4, 0xB0BB1E5);
    uint32_t r = 0xC0FFEE01;
    for (uint32_t t = 0; t < 5400; t++) {
        for (int i = 0; i < 4; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            int sx = (int)(r & 63) - 32;        if (sx < -31) sx = -31;
            int sy = (int)((r >> 6) & 63) - 32; if (sy < -31) sy = -31;
            int set  = ((t + i * 53) % 137) == 0;
            /* cast comment preserved from arena_hash.c: silences
             * -Wsign-compare; same values, same hash. */
            int bomb = ((t + i * 37) % (90 + i * 80)) < (uint32_t)(30 + i * 40);
            in[i] = arena_input_pack(sx, sy, ((r >> 12) & 31) == 0, bomb, set);
        }
        arena_tick(&s, in);
    }
    return arena_hash(&s);
}

#endif
