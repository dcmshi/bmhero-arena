/* Rendezvous server logic. PURE — see rendezvous.h.
 *
 * Everything here is a function of (state, now_ms, one packet). No clock, no
 * sockets, no allocation, no printf: the only way out is `emit`. That is what
 * lets tests drive a whole 4-player lobby, a punch-through negotiation, a
 * relay session and three expiry deadlines in microseconds with no network.
 *
 * The trust model: a UDP source address is the ONLY identity a peer has, so
 * every session-scoped message is authorised by matching `from` against the
 * endpoint registered for the slot it claims. A packet that claims a slot it
 * does not own is dropped silently — never rejected, since replying to a
 * forged source address is exactly the amplification we are avoiding. */
#include <string.h>
#include "rendezvous.h"

/* ---------- small helpers ---------- */
static uint32_t xs32(uint32_t x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
static int ep_eq(LobbyEndpoint a, LobbyEndpoint b) { return a.ip == b.ip && a.port == b.port; }

static void emit_msg(RvEmit emit, void* ctx, LobbyEndpoint to, const LobbyMsg* m) {
    uint8_t b[LOBBY_MAX_PACKET];
    size_t n = lobby_pack(m, b);
    if (n) emit(ctx, to, b, n);
}
static void emit_reject(RvEmit emit, void* ctx, LobbyEndpoint to, uint8_t reason) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_REJECT; m.u.reject.reason = reason;
    emit_msg(emit, ctx, to, &m);
}

/* Free/allocate are the same operation: zero it, then restore the one field
 * whose "empty" value is not zero. A stale punch verdict surviving into a
 * recycled session would hand out a route nobody negotiated. */
static void session_clear(RvSession* s) {
    memset(s, 0, sizeof *s);
    for (int a = 0; a < RV_MAX_PLAYERS; a++)
        for (int b = 0; b < RV_MAX_PLAYERS; b++) s->punch[a][b] = -1;
}

static RvSession* find_session(Rendezvous* rv, uint16_t id) {
    if (!id) return NULL;
    for (int i = 0; i < RV_MAX_SESSIONS; i++)
        if (rv->sessions[i].phase != RV_FREE && rv->sessions[i].id == id)
            return &rv->sessions[i];
    return NULL;
}

/* which slot owns this source address, or -1 */
static int peer_of(const RvSession* s, LobbyEndpoint from) {
    for (int i = 0; i < RV_MAX_PLAYERS; i++)
        if (s->peers[i].used && ep_eq(s->peers[i].public_ep, from)) return i;
    return -1;
}

/* ---------- rate limiting ---------- */
/* Token bucket per /32, RV_RATE_PER_SEC sustained with RV_RATE_BURST burst.
 * Keyed by ip only (not port) so a single host cannot multiply its budget by
 * rotating source ports. Time deltas are unsigned so the 49-day wrap of a
 * millisecond clock is harmless. */
static int rate_take(Rendezvous* rv, uint32_t now_ms, uint32_t ip) {
    RvRateBucket* b = &rv->rate[ip % RV_RATE_BUCKETS];
    uint32_t cap = (uint32_t)RV_RATE_BURST * 1000u;
    uint32_t dt  = now_ms - b->last_ms;
    b->last_ms = now_ms;
    if (dt > cap / RV_RATE_PER_SEC) {          /* long idle: refill, no overflow */
        b->tokens_milli = cap;
    } else {
        b->tokens_milli += dt * RV_RATE_PER_SEC;
        if (b->tokens_milli > cap) b->tokens_milli = cap;
    }
    if (b->tokens_milli < 1000u) return 0;
    b->tokens_milli -= 1000u;
    return 1;
}

/* ---------- fan-out ---------- */
/* A join is a mutual introduction: the newcomer to every incumbent, and every
 * incumbent to the newcomer. Both endpoints travel in each intro — the public
 * one the server observed and the private one the peer advertised — because a
 * pair behind the same NAT can only reach each other on the private pair. */
static void fan_peer_intro(RvSession* s, int newcomer, RvEmit emit, void* ctx) {
    LobbyMsg m;
    for (int i = 0; i < RV_MAX_PLAYERS; i++) {
        if (i == newcomer || !s->peers[i].used) continue;
        memset(&m, 0, sizeof m);
        m.type = LOBBY_PEER_INTRO;
        m.u.peer_intro.slot       = (uint8_t)newcomer;
        m.u.peer_intro.public_ep  = s->peers[newcomer].public_ep;
        m.u.peer_intro.private_ep = s->peers[newcomer].private_ep;
        emit_msg(emit, ctx, s->peers[i].public_ep, &m);

        memset(&m, 0, sizeof m);
        m.type = LOBBY_PEER_INTRO;
        m.u.peer_intro.slot       = (uint8_t)i;
        m.u.peer_intro.public_ep  = s->peers[i].public_ep;
        m.u.peer_intro.private_ep = s->peers[i].private_ep;
        emit_msg(emit, ctx, s->peers[newcomer].public_ep, &m);
    }
}

/* Re-deliver, to ONE peer, an introduction for every other member.
 *
 * Every intro is a single unacknowledged push. Lose one and the client waits in
 * WAITING_PEERS for a peer that will never be introduced again, then dies on its
 * 30s bootstrap deadline — a player-facing failure with no diagnosis. A 4-player
 * lobby fires ~12 of these pushes, so at 1% WAN loss it is not a rare case.
 *
 * The recovery is the retry path the client already has: a repeated HOST_REQ or
 * JOIN_REQ from an endpoint that is already registered replays the intros to
 * THAT endpoint and no other. Aiming the replay only at the retryer is what
 * stops a retry storm from becoming a fan-out storm at everyone else's expense.
 *
 * On amplification: this makes the response to a padded 32-byte request as large
 * as ~65 bytes, so a request forged from a member's address reflects roughly 2x
 * at that member. The bound that matters is the rate limiter, which is keyed on
 * the source ip a spoofer must claim — the victim's own bucket, 20/s. */
static void resend_intros_to(RvSession* s, int who, RvEmit emit, void* ctx) {
    LobbyMsg m;
    for (int i = 0; i < RV_MAX_PLAYERS; i++) {
        if (i == who || !s->peers[i].used) continue;
        memset(&m, 0, sizeof m);
        m.type = LOBBY_PEER_INTRO;
        m.u.peer_intro.slot       = (uint8_t)i;
        m.u.peer_intro.public_ep  = s->peers[i].public_ep;
        m.u.peer_intro.private_ep = s->peers[i].private_ep;
        emit_msg(emit, ctx, s->peers[who].public_ep, &m);
    }
}

static void fan_start(RvSession* s, int only_unacked, RvEmit emit, void* ctx) {
    if (!s->have_start) return;
    for (int i = 0; i < RV_MAX_PLAYERS; i++) {
        if (!s->peers[i].used) continue;
        if (only_unacked && s->peers[i].start_acked) continue;
        emit_msg(emit, ctx, s->peers[i].public_ep, &s->start);
    }
}

/* ---------- handlers ---------- */
static void do_host_req(Rendezvous* rv, uint32_t now, LobbyEndpoint from,
                        const LobbyMsg* in, RvEmit emit, void* ctx) {
    LobbyMsg m; memset(&m, 0, sizeof m);

    /* Idempotent retry. The HOST_RESP may well have been the packet that got
     * lost, so a repeat from the same endpoint must return the SAME id rather
     * than burn another table slot. */
    for (int i = 0; i < RV_MAX_SESSIONS; i++) {
        RvSession* c = &rv->sessions[i];
        if (c->phase != RV_LOBBY || !c->peers[0].used) continue;
        if (!ep_eq(c->peers[0].public_ep, from)) continue;
        c->last_activity_ms = now;
        c->peers[0].last_seen_ms = now;
        m.type = LOBBY_HOST_RESP; m.u.host_resp.session_id = c->id;
        emit_msg(emit, ctx, from, &m);
        resend_intros_to(c, 0, emit, ctx);   /* the retry re-delivers lost intros */
        return;
    }

    RvSession* s = NULL;
    for (int i = 0; i < RV_MAX_SESSIONS; i++)
        if (rv->sessions[i].phase == RV_FREE) { s = &rv->sessions[i]; break; }
    if (!s) { emit_reject(emit, ctx, from, LOBBY_REJ_FULL); return; }

    uint16_t id = 0;
    for (int tries = 0; tries < 4096 && !id; tries++) {
        rv->rng = xs32(rv->rng);
        uint16_t cand = (uint16_t)rv->rng;
        if (cand && !find_session(rv, cand)) id = cand;
    }
    if (!id) { emit_reject(emit, ctx, from, LOBBY_REJ_FULL); return; }

    session_clear(s);
    s->id    = id;
    s->phase = RV_LOBBY;
    uint8_t np = in->u.host_req.num_players;
    if (np < 2) np = 2;
    if (np > RV_MAX_PLAYERS) np = RV_MAX_PLAYERS;
    s->num_players = np;
    s->net_version = in->u.host_req.net_version;   /* the joiners are checked against this */
    s->peers[0].used         = 1;
    s->peers[0].public_ep    = from;
    s->peers[0].private_ep   = in->u.host_req.private_ep;
    s->peers[0].last_seen_ms = now;
    s->joined = 1;
    s->last_activity_ms = now;

    m.type = LOBBY_HOST_RESP; m.u.host_resp.session_id = id;
    emit_msg(emit, ctx, from, &m);
}

static void do_join_req(Rendezvous* rv, uint32_t now, LobbyEndpoint from,
                        const LobbyMsg* in, RvEmit emit, void* ctx) {
    RvSession* s = find_session(rv, in->u.join_req.session_id);
    if (!s) { emit_reject(emit, ctx, from, LOBBY_REJ_BAD_SESSION); return; }
    /* a live session that is no longer a lobby: the match already started */
    if (s->phase != RV_LOBBY) { emit_reject(emit, ctx, from, LOBBY_REJ_EXPIRED); return; }

    LobbyMsg m; memset(&m, 0, sizeof m);
    int existing = peer_of(s, from);
    if (existing >= 0) {                            /* idempotent retry */
        s->last_activity_ms = now;
        s->peers[existing].last_seen_ms = now;
        m.type = LOBBY_JOIN_RESP;
        m.u.join_resp.slot        = (uint8_t)existing;
        m.u.join_resp.num_players = s->num_players;
        emit_msg(emit, ctx, from, &m);
        resend_intros_to(s, existing, emit, ctx);   /* same for a joiner's retry */
        return;
    }
    /* version before capacity: a mismatched build should learn WHY it cannot
     * play even when the lobby happens to be full as well */
    if (in->u.join_req.net_version != s->net_version) {
        emit_reject(emit, ctx, from, LOBBY_REJ_VERSION_MISMATCH); return;
    }
    if (s->joined >= s->num_players) { emit_reject(emit, ctx, from, LOBBY_REJ_FULL); return; }

    int slot = -1;
    for (int i = 0; i < RV_MAX_PLAYERS; i++) if (!s->peers[i].used) { slot = i; break; }
    if (slot < 0) { emit_reject(emit, ctx, from, LOBBY_REJ_FULL); return; }

    s->peers[slot].used         = 1;
    s->peers[slot].public_ep    = from;
    s->peers[slot].private_ep   = in->u.join_req.private_ep;
    s->peers[slot].last_seen_ms = now;
    s->joined++;
    s->last_activity_ms = now;

    m.type = LOBBY_JOIN_RESP;
    m.u.join_resp.slot        = (uint8_t)slot;
    m.u.join_resp.num_players = s->num_players;
    emit_msg(emit, ctx, from, &m);
    fan_peer_intro(s, slot, emit, ctx);
}

static void do_punch_report(Rendezvous* rv, uint32_t now, LobbyEndpoint from,
                            const LobbyMsg* in, RvEmit emit, void* ctx) {
    RvSession* s = find_session(rv, in->u.punch_report.session_id);
    if (!s) return;
    int rep = peer_of(s, from);
    if (rep < 0) return;                            /* not a member: silent drop */
    uint8_t sa = in->u.punch_report.slot_a, sb = in->u.punch_report.slot_b;
    if (sa >= RV_MAX_PLAYERS || sb >= RV_MAX_PLAYERS || sa == sb) return;
    if (rep != (int)sa && rep != (int)sb) return;   /* only the two sides may report */
    int oth = (rep == (int)sa) ? (int)sb : (int)sa;
    if (!s->peers[oth].used) return;

    s->last_activity_ms = now;
    s->peers[rep].last_seen_ms = now;
    s->punch[rep][oth] = in->u.punch_report.ok ? 1 : 0;

    int a = (sa < sb) ? (int)sa : (int)sb;          /* canonical pair order */
    int b = (sa < sb) ? (int)sb : (int)sa;
    if (s->punch[a][b] < 0 || s->punch[b][a] < 0) return;   /* still one-sided */

    LobbyMsg m; memset(&m, 0, sizeof m);
    if (s->route_sent[a][b]) {
        /* Already decided, and this side is asking again — which is exactly what
         * a client does when the PAIR_ROUTE it was owed never arrived. Replay the
         * verdict to the REPORTER only: re-fanning to everyone would let one
         * lossy client multiply its retries across the whole lobby. Verdicts are
         * immutable once set, so the answer is always the same one. PAIR_ROUTE is
         * smaller than the PUNCH_REPORT that triggered it, so this is not an
         * amplifier. */
        m.type = LOBBY_PAIR_ROUTE;
        m.u.pair_route.slot_a = (uint8_t)a;
        m.u.pair_route.slot_b = (uint8_t)b;
        m.u.pair_route.route  = s->route[a][b];
        emit_msg(emit, ctx, from, &m);
        return;
    }

    /* Direct only if BOTH directions saw the hole open. One-way success is a
     * trap: it looks connected until the first packet has to go the other way. */
    uint8_t route = (s->punch[a][b] && s->punch[b][a]) ? (uint8_t)LOBBY_ROUTE_DIRECT
                                                       : (uint8_t)LOBBY_ROUTE_RELAY;
    s->route_sent[a][b] = s->route_sent[b][a] = 1;
    s->route[a][b] = s->route[b][a] = route;
    /* the relay-silence deadline starts when the relay route is created, not
     * at the first forwarded packet — otherwise the session would be reaped
     * before the peers had a chance to use the route we just gave them */
    if (route == LOBBY_ROUTE_RELAY) s->last_relay_ms = now;

    m.type = LOBBY_PAIR_ROUTE;
    m.u.pair_route.slot_a = (uint8_t)a;
    m.u.pair_route.slot_b = (uint8_t)b;
    m.u.pair_route.route  = route;
    for (int i = 0; i < RV_MAX_PLAYERS; i++)        /* everyone learns every route */
        if (s->peers[i].used) emit_msg(emit, ctx, s->peers[i].public_ep, &m);
}

static void do_start(Rendezvous* rv, uint32_t now, LobbyEndpoint from,
                     const LobbyMsg* in, RvEmit emit, void* ctx) {
    /* START carries no session id: it is authorised BY being slot 0's address,
     * so the session is the one this endpoint hosts. */
    RvSession* s = NULL;
    for (int i = 0; i < RV_MAX_SESSIONS; i++) {
        RvSession* c = &rv->sessions[i];
        if (c->phase == RV_FREE || !c->peers[0].used) continue;
        if (ep_eq(c->peers[0].public_ep, from)) { s = c; break; }
    }
    if (!s || s->phase == RV_RUNNING) return;

    int first = (s->phase == RV_LOBBY);
    s->last_activity_ms = now;
    s->peers[0].last_seen_ms = now;
    if (first) {
        s->phase = RV_STARTING;
        s->started_ms = now;
        /* Restart the relay-silence clock unconditionally. A pair can negotiate
         * a RELAY route early and then sit in the lobby for minutes; if the
         * clock kept running from route-creation time, the very first tick after
         * START would see minutes of "silence" and reap a match that had only
         * just begun. Nothing is relayed before START, so silence before it
         * cannot mean anything. (Found by mutation-testing rv_tick's deadline.) */
        s->last_relay_ms = now;
    }
    s->start = *in;
    s->have_start = 1;
    s->last_refan_ms = now;
    fan_start(s, first ? 0 : 1, emit, ctx);
}

static void do_start_ack(Rendezvous* rv, uint32_t now, LobbyEndpoint from,
                         const LobbyMsg* in) {
    RvSession* s = find_session(rv, in->u.start_ack.session_id);
    if (!s) return;
    int p = peer_of(s, from);
    if (p < 0 || in->u.start_ack.slot != (uint8_t)p) return;
    s->last_activity_ms = now;
    s->peers[p].last_seen_ms = now;
    if (s->phase != RV_STARTING) return;
    s->peers[p].start_acked = 1;
    for (int i = 0; i < RV_MAX_PLAYERS; i++)
        if (s->peers[i].used && !s->peers[i].start_acked) return;
    s->phase = RV_RUNNING;                          /* everyone has the START */
}

static void do_keepalive(Rendezvous* rv, uint32_t now, LobbyEndpoint from,
                         const LobbyMsg* in) {
    RvSession* s = find_session(rv, in->u.start_ack.session_id);
    if (!s) return;
    int p = peer_of(s, from);
    if (p < 0) return;
    s->last_activity_ms = now;
    s->peers[p].last_seen_ms = now;
}

static void do_relay(Rendezvous* rv, uint32_t now, LobbyEndpoint from,
                     const LobbyMsg* in, const uint8_t* buf, size_t len,
                     RvEmit emit, void* ctx) {
    RvSession* s = find_session(rv, in->u.relay.session_id);
    if (!s) return;
    uint8_t fs = in->u.relay.from_slot, ts = in->u.relay.to_slot;
    if (fs >= RV_MAX_PLAYERS || ts >= RV_MAX_PLAYERS) return;
    /* the sender must actually own the slot it claims, or this is a free
     * reflector pointed at any address in the session */
    if (!s->peers[fs].used || !ep_eq(s->peers[fs].public_ep, from)) return;
    if (!s->peers[ts].used) return;

    s->last_activity_ms = now;
    s->last_relay_ms    = now;
    s->peers[fs].last_seen_ms = now;
    rv->relay_forwards++;
    emit(ctx, s->peers[ts].public_ep, buf, len);    /* verbatim: never re-packed */
}

/* ---------- entry points ---------- */
void rv_init(Rendezvous* rv, uint32_t now_ms, uint32_t rng_seed) {
    if (!rv) return;
    memset(rv, 0, sizeof *rv);
    for (int i = 0; i < RV_MAX_SESSIONS; i++) session_clear(&rv->sessions[i]);
    for (int i = 0; i < RV_RATE_BUCKETS; i++) {
        rv->rate[i].tokens_milli = (uint32_t)RV_RATE_BURST * 1000u;
        rv->rate[i].last_ms      = now_ms;
    }
    rv->rng = rng_seed ? rng_seed : 1u;
}

void rv_handle(Rendezvous* rv, uint32_t now_ms, LobbyEndpoint from,
               const uint8_t* buf, size_t len, RvEmit emit, void* ctx) {
    if (!rv || !emit) return;
    LobbyMsg in;
    if (lobby_unpack(buf, len, &in) != 0) return;

    /* Game traffic is never throttled — a rollback session bursts far past any
     * sane control-plane rate, and RELAY is already gated on owning a slot in
     * a live session, which a spoofer cannot obtain. */
    if (in.type != LOBBY_RELAY && !rate_take(rv, now_ms, from.ip)) return;

    switch (in.type) {
    case LOBBY_HOST_REQ:     do_host_req(rv, now_ms, from, &in, emit, ctx); break;
    case LOBBY_JOIN_REQ:     do_join_req(rv, now_ms, from, &in, emit, ctx); break;
    case LOBBY_PUNCH_REPORT: do_punch_report(rv, now_ms, from, &in, emit, ctx); break;
    case LOBBY_START:        do_start(rv, now_ms, from, &in, emit, ctx); break;
    case LOBBY_START_ACK:    do_start_ack(rv, now_ms, from, &in); break;
    case LOBBY_KEEPALIVE:    do_keepalive(rv, now_ms, from, &in); break;
    case LOBBY_RELAY:        do_relay(rv, now_ms, from, &in, buf, len, emit, ctx); break;
    default: break;   /* server-originated types, and PUNCH/PUNCH_ACK/GAME,
                       * which are peer-to-peer and never addressed here */
    }
}

void rv_tick(Rendezvous* rv, uint32_t now_ms, RvEmit emit, void* ctx) {
    if (!rv || !emit) return;
    for (int i = 0; i < RV_MAX_SESSIONS; i++) {
        RvSession* s = &rv->sessions[i];
        if (s->phase == RV_FREE) continue;

        if (s->phase == RV_LOBBY) {
            if (now_ms - s->last_activity_ms > RV_LOBBY_EXPIRY_MS) session_clear(s);
            continue;
        }

        if (s->phase == RV_STARTING && now_ms - s->last_refan_ms >= RV_START_REFAN_MS) {
            fan_start(s, 1, emit, ctx);
            s->last_refan_ms = now_ms;
        }

        /* A session the server is not relaying for is only needed long enough
         * to hand out the START; one it IS relaying for must live as long as
         * the match does, so its deadline is relay silence instead. */
        int has_relay = 0;
        for (int a = 0; a < RV_MAX_PLAYERS && !has_relay; a++)
            for (int b = 0; b < RV_MAX_PLAYERS; b++)
                if (s->route_sent[a][b] && s->route[a][b] == LOBBY_ROUTE_RELAY) {
                    has_relay = 1; break;
                }
        if (!has_relay) {
            if (now_ms - s->started_ms > RV_STARTED_EXPIRY_MS) session_clear(s);
        } else if (now_ms - s->last_relay_ms > RV_RELAY_EXPIRY_MS) {
            session_clear(s);
        }
    }
}

uint32_t rv_relay_forwards(const Rendezvous* rv) { return rv ? rv->relay_forwards : 0u; }

int rv_active_sessions(const Rendezvous* rv) {
    if (!rv) return 0;
    int n = 0;
    for (int i = 0; i < RV_MAX_SESSIONS; i++)
        if (rv->sessions[i].phase != RV_FREE) n++;
    return n;
}
