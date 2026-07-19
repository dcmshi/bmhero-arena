#include <stdlib.h>
#include <string.h>
#include "gekkonet.h"
#include "sync_session.h"
#include "arena/arena_sim.h"

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
    TickHash      ring[256];
};

SyncSession* sync_create(const SyncConfig* cfg) {
    if (!cfg || cfg->num_players < 2 || cfg->num_players > ARENA_MAX_PLAYERS)
        return NULL;
    SyncSession* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->mode = cfg->mode;
    s->num_players = cfg->num_players;
    s->local_mask = cfg->local_mask;
    arena_init(&s->state, cfg->arena_id, cfg->num_players, cfg->seed);

    GekkoSessionType st = (cfg->mode == SYNC_STRESS) ? GekkoStressSession
                                                     : GekkoGameSession;
    if (!gekko_create(&s->gk, st)) { free(s); return NULL; }

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
        gekko_net_adapter_set(s->gk, gekko_default_adapter(cfg->local_port));
        s->online_adapter = true;
    }

    for (int i = 0; i < cfg->num_players; i++) {
        if (cfg->local_mask & (1u << i)) {
            s->handle[i] = gekko_add_actor(s->gk, GekkoLocalPlayer, NULL);
            gekko_set_local_delay(s->gk, s->handle[i], cfg->input_delay);
        } else {
            GekkoNetAddress a;
            a.data = (void*)cfg->peer_addr[i];
            a.size = (unsigned int)strlen(cfg->peer_addr[i]);
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
        case GekkoSessionStarted:     s->connected = true;  break;
        case GekkoPlayerDisconnected: s->connected = false; break;
        case GekkoDesyncDetected:     s->desynced = true;   break;
        default: break;
        }
    }

    int fresh = 0, gec = 0;
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
            arena_tick(&s->state, in);
            s->ring[s->state.tick & 255u].tick = s->state.tick;
            s->ring[s->state.tick & 255u].hash = arena_hash(&s->state);
            if (!e->data.adv.rolling_back) fresh++;
            break;
        }
        default: break;
        }
    }
    return fresh;
}
