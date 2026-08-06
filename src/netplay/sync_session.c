#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gekkonet.h"
#include "sync_session.h"
#include "desync_bundle.h"
#include "arena/arena_sim.h"
#include "lobby/lobby_proto.h"      /* arena_net_version() for the bundle header */

typedef struct { uint32_t tick, hash; } TickHash;

struct SyncSession {
    SyncMode      mode;
    uint8_t       num_players;
    uint8_t       local_mask;
    GekkoSession* gk;
    bool          online_adapter;   /* default adapter owned by this session */
    ArenaState    state;
    bool          connected;
    bool          desynced;
    int           handle[ARENA_MAX_PLAYERS];
    /* Custom-adapter actor addresses. GekkoNet compares addresses as opaque
     * byte blobs (size + bytes), so one byte per slot is a legal address — and
     * it must be storage the SESSION owns, since gekko keeps the pointer. */
    uint8_t       slot_addr[ARENA_MAX_PLAYERS];
    TickHash      ring[256];
    SyncStats     stats;
    bool          started;          /* GekkoSessionStarted seen (ONLINE) */
    /* --- desync bundle material --- */
    uint32_t      seed;             /* as passed to arena_init; state.rng moves on */
    uint8_t       local_slot;       /* lowest bit of local_mask */
    ArenaInput*   rec;              /* BUNDLE_MAX_TICKS * 4 inputs, tick-major */
    uint32_t      rec_count;        /* ticks recorded (highest tick + 1) */
    SyncDesyncInfo dinfo;
    bool          have_dinfo;
    bool          corrupt_pending;  /* sync_debug_corrupt armed */
};

static void bwr16(FILE* f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}
static void bwr32(FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

SyncSession* sync_create(const SyncConfig* cfg) {
    if (!cfg || cfg->num_players < 2 || cfg->num_players > ARENA_MAX_PLAYERS)
        return NULL;
    SyncSession* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->mode = cfg->mode;
    s->num_players = cfg->num_players;
    s->local_mask = cfg->local_mask;
    s->seed = cfg->seed;
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        if (cfg->local_mask & (1u << i)) { s->local_slot = (uint8_t)i; break; }
    /* The ONE allocation this layer makes, and it happens here, at create:
     * the confirmed-input history a desync bundle replays (~288KB). */
    s->rec = calloc((size_t)BUNDLE_MAX_TICKS * ARENA_MAX_PLAYERS, sizeof(ArenaInput));
    if (!s->rec) { free(s); return NULL; }
    arena_init(&s->state, cfg->arena_id, cfg->num_players, cfg->seed);

    GekkoSessionType st = (cfg->mode == SYNC_STRESS) ? GekkoStressSession
                                                     : GekkoGameSession;
    if (!gekko_create(&s->gk, st)) { free(s->rec); free(s); return NULL; }

    GekkoConfig gc;
    memset(&gc, 0, sizeof gc);
    gc.num_players = cfg->num_players;
    gc.max_spectators = 0;
    gc.input_size = (unsigned int)sizeof(ArenaInput);
    gc.state_size = (unsigned int)sizeof(ArenaState);
    gc.limited_saving = false;              /* 944B: save every frame, keeps
                                               desync detection available */
    gc.desync_detection = true;
    gc.input_prediction_window = 8;
    gc.check_distance = (cfg->mode == SYNC_STRESS) ? 8 : 0;
    gekko_start(s->gk, &gc);

    if (cfg->mode == SYNC_ONLINE) {
        if (cfg->adapter) {
            gekko_net_adapter_set(s->gk, cfg->adapter);   /* caller owns lifetime */
        } else {
            gekko_net_adapter_set(s->gk, gekko_default_adapter(cfg->local_port));
            s->online_adapter = true;
        }
    }

    for (int i = 0; i < cfg->num_players; i++) {
        if (cfg->local_mask & (1u << i)) {
            s->handle[i] = gekko_add_actor(s->gk, GekkoLocalPlayer, NULL);
            gekko_set_local_delay(s->gk, s->handle[i], cfg->input_delay);
        } else {
            GekkoNetAddress a;
            if (cfg->adapter) {
                s->slot_addr[i] = (uint8_t)i;
                a.data = &s->slot_addr[i];
                a.size = 1;
            } else {
                a.data = (void*)cfg->peer_addr[i];
                a.size = (unsigned int)strlen(cfg->peer_addr[i]);
            }
            s->handle[i] = gekko_add_actor(s->gk, GekkoRemotePlayer, &a);
        }
    }
    /* local-only sessions have no sync handshake */
    s->connected = (cfg->mode != SYNC_ONLINE);
    return s;
}

void sync_destroy(SyncSession* s) {
    if (!s) return;
    gekko_destroy(&s->gk);
    if (s->online_adapter) gekko_default_adapter_destroy();
    free(s->rec);
    free(s);
}

const ArenaState* sync_state(const SyncSession* s) { return &s->state; }
ArenaState* sync_state_debug_mut(SyncSession* s) {
    return (s->mode == SYNC_COUCH) ? &s->state : NULL;
}
bool sync_connected(const SyncSession* s) { return s->connected; }
bool sync_desynced(const SyncSession* s) { return s->desynced; }
uint32_t sync_present_tick(const SyncSession* s) { return s->state.tick; }
uint32_t sync_present_hash(const SyncSession* s) { return arena_hash(&s->state); }
uint32_t sync_hash_at(const SyncSession* s, uint32_t tick) {
    const TickHash* th = &s->ring[tick & 255u];
    return (th->tick == tick) ? th->hash : 0;
}
void sync_stats(const SyncSession* s, SyncStats* out) {
    if (!s || !out) return;
    *out = s->stats;
}

bool sync_desync_info(const SyncSession* s, SyncDesyncInfo* out) {
    if (!s || !out || !s->have_dinfo) return false;
    *out = s->dinfo;
    return true;
}

void sync_debug_corrupt(SyncSession* s) { if (s) s->corrupt_pending = true; }

int sync_dump_bundle(const SyncSession* s, const char* path) {
    if (!s || !path) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    bwr32(f, BUNDLE_MAGIC);
    bwr32(f, BUNDLE_VERSION);
    bwr32(f, arena_net_version());
    bwr32(f, s->seed);
    uint8_t hb[4] = { s->state.arena_id, s->num_players, s->local_slot, 0 };
    fwrite(hb, 1, 4, f);
    bwr32(f, s->have_dinfo ? s->dinfo.tick : 0u);
    bwr32(f, s->have_dinfo ? s->dinfo.local_hash : 0u);
    bwr32(f, s->have_dinfo ? s->dinfo.remote_hash : 0u);
    bwr32(f, (uint32_t)(s->have_dinfo ? s->dinfo.remote_slot : -1));

    for (uint32_t i = 0; i < BUNDLE_RING; i++) {
        bwr32(f, s->ring[i].tick);
        bwr32(f, s->ring[i].hash);
    }
    /* the one raw-bytes field; see desync_bundle.h on why, and its LE caveat */
    fwrite(&s->state, 1, sizeof(ArenaState), f);

    bwr32(f, s->rec_count);
    for (uint32_t t = 0; t < s->rec_count; t++)
        for (int p = 0; p < ARENA_MAX_PLAYERS; p++)
            bwr16(f, s->rec[t * ARENA_MAX_PLAYERS + p]);

    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    return bad ? -1 : 0;
}

int sync_frame(SyncSession* s, const ArenaInput inputs[ARENA_MAX_PLAYERS]) {
    if (s->mode == SYNC_ONLINE) gekko_network_poll(s->gk);

    for (int i = 0; i < s->num_players; i++) {
        if (s->local_mask & (1u << i)) {
            ArenaInput in = inputs[i];
            gekko_add_local_input(s->gk, s->handle[i], &in);
        }
    }

    int sec = 0;
    GekkoSessionEvent** sev = gekko_session_events(s->gk, &sec);
    for (int i = 0; i < sec; i++) {
        switch (sev[i]->type) {
        case GekkoSessionStarted:     s->connected = true; s->started = true; break;
        case GekkoPlayerDisconnected: s->connected = false; break;
        case GekkoDesyncDetected:
            /* keep the FIRST report: later ones are downstream of it */
            if (!s->have_dinfo) {
                s->dinfo.tick        = (uint32_t)sev[i]->data.desynced.frame;
                s->dinfo.local_hash  = sev[i]->data.desynced.local_checksum;
                s->dinfo.remote_hash = sev[i]->data.desynced.remote_checksum;
                s->dinfo.remote_slot = -1;
                for (int k = 0; k < s->num_players; k++)
                    if (s->handle[k] == sev[i]->data.desynced.remote_handle) {
                        s->dinfo.remote_slot = k; break;
                    }
                s->have_dinfo = true;
            }
            s->desynced = true;
            break;
        default: break;
        }
    }

    int fresh = 0, rb = 0, gec = 0;
    GekkoGameEvent** gev = gekko_update_session(s->gk, &gec);
    for (int i = 0; i < gec; i++) {
        GekkoGameEvent* e = gev[i];
        switch (e->type) {
        case GekkoSaveEvent:
            *e->data.save.state_len = (unsigned int)sizeof(ArenaState);
            *e->data.save.checksum = arena_hash(&s->state);
            memcpy(e->data.save.state, &s->state, sizeof(ArenaState));
            break;
        case GekkoLoadEvent:
            memcpy(&s->state, e->data.load.state, sizeof(ArenaState));
            break;
        case GekkoAdvanceEvent: {
            ArenaInput in[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
            memcpy(in, e->data.adv.inputs,
                   (size_t)s->num_players * sizeof(ArenaInput));
            /* Record the inputs this tick actually ran with. Rollbacks overwrite,
             * so the history converges to the confirmed inputs once a tick is
             * behind the confirmed frontier — same argument as the hash ring. */
            uint32_t T = s->state.tick;
            if (T < BUNDLE_MAX_TICKS) {
                memcpy(&s->rec[T * ARENA_MAX_PLAYERS], in,
                       ARENA_MAX_PLAYERS * sizeof(ArenaInput));
                if (T + 1 > s->rec_count) s->rec_count = T + 1;
            }
            arena_tick(&s->state, in);
            /* Test-only divergence, injected BEFORE the ring write so the ring
             * honestly records what the live sim held after this tick — that is
             * what makes replay_bundle name this tick and not the next one. */
            if (s->corrupt_pending && !e->data.adv.rolling_back) {
                s->state.rng ^= 1u;
                s->corrupt_pending = false;
            }
            s->ring[s->state.tick & 255u].tick = s->state.tick;
            s->ring[s->state.tick & 255u].hash = arena_hash(&s->state);
            if (e->data.adv.rolling_back) rb++; else fresh++;
            break;
        }
        default: break;
        }
    }

    if (s->mode == SYNC_ONLINE && s->started) {
        s->stats.pumps++;
        s->stats.rollback_ticks += (uint32_t)rb;
        if ((uint32_t)rb > s->stats.max_rollback_depth)
            s->stats.max_rollback_depth = (uint32_t)rb;
        s->stats.rbhist[rb > 8 ? 8 : rb]++;
        if (fresh == 0 && s->connected) s->stats.stall_frames++;
    }
    return fresh;
}
