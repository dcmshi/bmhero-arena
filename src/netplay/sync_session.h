#ifndef SYNC_SESSION_H
#define SYNC_SESSION_H

/* One interface, three configs of the same loop (design doc s6):
 * COUCH (all local, zero delay), ONLINE (full-mesh UDP), STRESS (GekkoNet
 * synctest: continuous rollback + re-sim — the determinism gate).
 * The session OWNS the ArenaState and is the only arena_tick caller;
 * rollback is invisible to callers. C11; GekkoNet C++ hides behind its lib. */

#include <stdbool.h>
#include <stdint.h>
#include "arena/arena_state.h"

typedef struct SyncSession SyncSession;
typedef enum { SYNC_COUCH, SYNC_ONLINE, SYNC_STRESS } SyncMode;

/* Forward declaration rather than including gekkonet.h: this header is included
 * by sim-side and tool-side code that has no business seeing the GekkoNet API.
 * C11 6.7p3 permits a repeated compatible typedef, so a translation unit that
 * includes both this and gekkonet.h is fine. */
typedef struct GekkoNetAdapter GekkoNetAdapter;

typedef struct {
    SyncMode    mode;
    uint8_t     num_players;                  /* 2..4 */
    uint8_t     local_mask;                   /* bit i = player i is local */
    uint16_t    local_port;                   /* ONLINE + default adapter: our UDP port */
    const char* peer_addr[ARENA_MAX_PLAYERS]; /* ONLINE + default adapter: "ip:port" per remote */
    uint32_t    seed;
    uint8_t     arena_id;
    uint8_t     input_delay;                  /* frames, 0-2 typical */
    GekkoNetAdapter* adapter;                 /* ONLINE: non-NULL = custom adapter (A3);
                                                 remote actor addresses become 1-byte
                                                 slot indices; peer_addr/local_port unused */
} SyncConfig;

SyncSession*      sync_create(const SyncConfig* cfg);   /* NULL on failure */
void              sync_destroy(SyncSession* s);
/* Pump once per 60Hz tick: feed local inputs, run GekkoNet events.
 * Returns fresh (non-rollback) ticks advanced. */
int               sync_frame(SyncSession* s,
                             const ArenaInput local_inputs[ARENA_MAX_PLAYERS]);
const ArenaState* sync_state(const SyncSession* s);     /* present, render-only */
ArenaState*       sync_state_debug_mut(SyncSession* s); /* COUCH-only debug */
bool              sync_connected(const SyncSession* s);
bool              sync_desynced(const SyncSession* s);
uint32_t          sync_present_tick(const SyncSession* s);
uint32_t          sync_present_hash(const SyncSession* s);
/* Hash recorded the last time the sim advanced through `tick` (re-recorded
 * after rollbacks, so it converges to the confirmed value once tick is
 * behind the confirmed frontier). 0 if evicted (ring of 256). */
uint32_t          sync_hash_at(const SyncSession* s, uint32_t tick);

#endif
