/* Rendezvous server logic — PURE: no sockets, no clock, no allocation.
 * The caller owns time (now_ms) and I/O (RvEmit). rendezvous_main.c wraps
 * this in a socket loop; tests drive it with fake packets and a fake clock. */
#ifndef RENDEZVOUS_H
#define RENDEZVOUS_H

#include "lobby_proto.h"

#define RV_MAX_SESSIONS      64
#define RV_MAX_PLAYERS       4
#define RV_RATE_BUCKETS      256
#define RV_RATE_PER_SEC      20
#define RV_RATE_BURST        40
#define RV_LOBBY_EXPIRY_MS   (10u * 60u * 1000u)
#define RV_STARTED_EXPIRY_MS (30u * 1000u)
#define RV_RELAY_EXPIRY_MS   (60u * 1000u)
#define RV_START_REFAN_MS    500u

enum { RV_FREE = 0, RV_LOBBY, RV_STARTING, RV_RUNNING };

typedef void (*RvEmit)(void* ctx, LobbyEndpoint to, const uint8_t* buf, size_t len);

typedef struct {
    uint8_t       used, start_acked;
    LobbyEndpoint public_ep, private_ep;
    uint32_t      last_seen_ms;
} RvPeer;

typedef struct {
    uint16_t id;
    uint8_t  phase;                        /* RV_* */
    uint8_t  num_players, joined;
    /* The HOST's net_version, captured from HOST_REQ. The server never
     * computes a version of its own (it has no sim): a joiner is compared
     * against this, so a rendezvous binary can outlive many sim versions. */
    uint32_t net_version;
    int8_t   punch[RV_MAX_PLAYERS][RV_MAX_PLAYERS];   /* [a][b]: a's verdict; -1 unseen */
    uint8_t  route_sent[RV_MAX_PLAYERS][RV_MAX_PLAYERS];
    uint8_t  route[RV_MAX_PLAYERS][RV_MAX_PLAYERS];   /* LobbyRoute, valid if route_sent */
    RvPeer   peers[RV_MAX_PLAYERS];
    LobbyMsg start;                        /* valid if have_start */
    uint8_t  have_start;
    uint32_t last_activity_ms, started_ms, last_refan_ms, last_relay_ms;
} RvSession;

typedef struct { uint32_t tokens_milli, last_ms; } RvRateBucket;

typedef struct {
    RvSession    sessions[RV_MAX_SESSIONS];
    RvRateBucket rate[RV_RATE_BUCKETS];
    uint32_t     rng;
    /* Every packet this server has forwarded on a RELAY route, since boot. The
     * core is printf-free, so this counter is the ONLY way an observer outside
     * it can tell relayed traffic from direct traffic — which is what makes
     * "the punch worked" and "the punch silently fell back to relay"
     * distinguishable instead of both looking like a green test. */
    uint32_t     relay_forwards;
} Rendezvous;

void rv_init(Rendezvous* rv, uint32_t now_ms, uint32_t rng_seed);
void rv_handle(Rendezvous* rv, uint32_t now_ms, LobbyEndpoint from,
               const uint8_t* buf, size_t len, RvEmit emit, void* ctx);
void rv_tick(Rendezvous* rv, uint32_t now_ms, RvEmit emit, void* ctx);
int  rv_active_sessions(const Rendezvous* rv);
uint32_t rv_relay_forwards(const Rendezvous* rv);

#endif
