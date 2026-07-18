#ifndef ARENA_SIM_H
#define ARENA_SIM_H

#include "arena_state.h"

/* Initialize a match. Zeroes all padding (hash/snapshot stability). */
void arena_init(ArenaState* s, uint8_t arena_id, uint8_t num_players, uint32_t seed);

/* Advance exactly one 60Hz tick. inputs[i] is player i's input word for this
 * tick. Deterministic: same state + same inputs => bit-identical result. */
void arena_tick(ArenaState* s, const ArenaInput inputs[ARENA_MAX_PLAYERS]);

/* Snapshot helpers (trivial, but keep call sites uniform). */
static inline void arena_save(ArenaState* dst, const ArenaState* src) { *dst = *src; }
static inline void arena_load(ArenaState* dst, const ArenaState* src) { *dst = *src; }

#endif
