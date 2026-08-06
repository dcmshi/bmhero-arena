/* The client half of the lobby: register or join, learn every peer, punch a
 * hole to each of them, take the server's route verdicts, and hand the caller
 * a LobbyResult the net adapter can be driven from.
 *
 * Three properties make this safe to embed in a game loop:
 *   - No allocation and no blocking. The struct is caller-owned and one
 *     lobby_poll does at most one socket drain plus a few sends.
 *   - Every send goes out of the SAME socket the adapter will later use. That
 *     is the entire point of the exercise: a hole punched from one socket is
 *     worthless to another.
 *   - Every wait has a deadline. Each stage retries its outstanding request on
 *     a fixed cadence, and the whole bootstrap fails after LOBBY_TIMEOUT_MS
 *     naming the stage it died in — a bootstrap that hangs forever is the one
 *     failure mode a player cannot diagnose. */
#include <string.h>
#include "lobby_client.h"
#include "net_adapter.h"

/* ---------- small helpers ---------- */
static uint32_t next_rng(LobbyClient* lc) {
    uint32_t x = lc->rng ? lc->rng : 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    lc->rng = x;
    return x;
}

static const char* stage_name(uint8_t st) {
    switch (st) {
    case LOBBY_C_REGISTERING:    return "REGISTERING";
    case LOBBY_C_JOINING:        return "JOINING";
    case LOBBY_C_WAITING_PEERS:  return "WAITING_PEERS";
    case LOBBY_C_PUNCHING:       return "PUNCHING";
    case LOBBY_C_AWAITING_START: return "AWAITING_START";
    case LOBBY_C_READY:          return "READY";
    case LOBBY_C_FAILED:         return "FAILED";
    default:                     return "IDLE";
    }
}

static void set_fail_text(LobbyClient* lc, const char* a, const char* b) {
    size_t i = 0;
    for (const char* p = a; p && *p && i + 1 < sizeof lc->fail_stage; p++) lc->fail_stage[i++] = *p;
    for (const char* p = b; p && *p && i + 1 < sizeof lc->fail_stage; p++) lc->fail_stage[i++] = *p;
    lc->fail_stage[i] = '\0';
}

static void fail_here(LobbyClient* lc) {
    set_fail_text(lc, stage_name(lc->stage), NULL);
    lc->stage = LOBBY_C_FAILED;
}

static void send_msg(LobbyClient* lc, LobbyEndpoint to, const LobbyMsg* m) {
    uint8_t b[LOBBY_MAX_PACKET];
    size_t n = lobby_pack(m, b);
    if (n) udp_send(lc->sock, to, b, n);
}

static void send_host_req(LobbyClient* lc) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_HOST_REQ;
    m.u.host_req.net_version = lc->net_ver;
    m.u.host_req.num_players = lc->num_players;
    m.u.host_req.private_ep  = lc->private_ep;
    send_msg(lc, lc->server, &m);
}

static void send_join_req(LobbyClient* lc) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_JOIN_REQ;
    m.u.join_req.session_id  = lc->session_id;
    m.u.join_req.net_version = lc->net_ver;
    m.u.join_req.private_ep  = lc->private_ep;
    send_msg(lc, lc->server, &m);
}

static void send_punch_to(LobbyClient* lc, uint8_t slot, LobbyEndpoint to) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_PUNCH;
    m.u.punch.session_id = lc->session_id;
    m.u.punch.from_slot  = lc->local_slot;
    m.u.punch.nonce      = lc->peer[slot].nonce;
    send_msg(lc, to, &m);
}

static void send_punch_report(LobbyClient* lc, uint8_t other, uint8_t ok) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_PUNCH_REPORT;
    m.u.punch_report.session_id = lc->session_id;
    m.u.punch_report.slot_a = (lc->local_slot < other) ? lc->local_slot : other;
    m.u.punch_report.slot_b = (lc->local_slot < other) ? other : lc->local_slot;
    m.u.punch_report.ok     = ok;
    send_msg(lc, lc->server, &m);
}

static void send_start_ack(LobbyClient* lc) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_START_ACK;
    m.u.start_ack.session_id = lc->session_id;
    m.u.start_ack.slot       = lc->local_slot;
    send_msg(lc, lc->server, &m);
}

static void send_start(LobbyClient* lc) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_START;
    m.u.start.seed        = lc->seed;
    m.u.start.arena_id    = lc->arena_id;
    m.u.start.num_players = lc->num_players;
    m.u.start.input_delay = lc->input_delay;
    send_msg(lc, lc->server, &m);
}

static void send_keepalive(LobbyClient* lc) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_KEEPALIVE;
    m.u.start_ack.session_id = lc->session_id;
    m.u.start_ack.slot       = lc->local_slot;
    send_msg(lc, lc->server, &m);
}

/* ---------- readiness predicates ---------- */
static int all_peers_present(const LobbyClient* lc) {
    if (!lc->num_players) return 0;
    for (int i = 0; i < lc->num_players; i++) if (!lc->peer[i].present) return 0;
    return 1;
}

static int local_routes_decided(const LobbyClient* lc) {
    if (!lc->num_players) return 0;
    for (int i = 0; i < lc->num_players; i++) {
        if (i == lc->local_slot) continue;
        if (lc->peer[i].route < 0) return 0;
    }
    return 1;
}

static int all_pairs_decided(const LobbyClient* lc) {
    if (lc->num_players < 2) return 0;
    for (int a = 0; a < lc->num_players; a++)
        for (int b = a + 1; b < lc->num_players; b++)
            if (!lc->route_known[a][b]) return 0;
    return 1;
}

static void fill_result(LobbyClient* lc) {
    LobbyResult* r = &lc->result;
    memset(r, 0, sizeof *r);
    r->local_slot  = lc->local_slot;
    r->num_players = lc->num_players;
    r->arena_id    = lc->arena_id;
    r->input_delay = lc->input_delay;
    r->session_id  = lc->session_id;
    r->seed        = lc->seed;
    r->server      = lc->server;
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++) {
        if (i == lc->local_slot || i >= lc->num_players) continue;
        if (lc->peer[i].route == (int8_t)LOBBY_ROUTE_DIRECT) {
            r->route[i].kind = NET_ROUTE_DIRECT;
            r->route[i].ep   = lc->peer[i].chosen;
        } else {
            r->route[i].kind = NET_ROUTE_RELAY;
        }
    }
}

/* Start the punch clocks HERE, not at PEER_INTRO. Peers arrive minutes apart in
 * a real lobby; a budget started when slot 1 was introduced would already have
 * expired by the time slot 3 showed up, and every pair would fall back to relay
 * without a single probe having been sent. */
static void enter_punching(LobbyClient* lc, uint32_t now) {
    lc->stage = LOBBY_C_PUNCHING;
    for (int i = 0; i < lc->num_players; i++) {
        if (i == lc->local_slot) continue;
        lc->peer[i].punch_begin_ms = now;
        lc->peer[i].last_punch_ms  = now - LOBBY_PUNCH_PERIOD_MS;   /* probe at once */
        lc->peer[i].last_report_ms = now;
    }
}

/* ---------- receive dispatch ---------- */
static const char* reject_name(uint8_t reason) {
    switch (reason) {
    case LOBBY_REJ_BAD_SESSION:      return "BAD_SESSION";
    case LOBBY_REJ_FULL:             return "FULL";
    case LOBBY_REJ_VERSION_MISMATCH: return "VERSION";
    case LOBBY_REJ_EXPIRED:          return "EXPIRED";
    default:                         return "UNKNOWN";
    }
}

static void handle_msg(LobbyClient* lc, uint32_t now, LobbyEndpoint from,
                       const LobbyMsg* m) {
    switch (m->type) {

    case LOBBY_HOST_RESP:
        if (lc->stage != LOBBY_C_REGISTERING || !m->u.host_resp.session_id) break;
        lc->session_id = m->u.host_resp.session_id;
        lobby_code_encode(lc->server, lc->session_id, lc->code);
        lc->local_slot = 0;
        lc->peer[0].present = 1;               /* self: never punched */
        lc->retries = 0;
        lc->stage = LOBBY_C_WAITING_PEERS;
        break;

    case LOBBY_JOIN_RESP:
        if (lc->stage != LOBBY_C_JOINING) break;
        if (m->u.join_resp.slot >= ARENA_MAX_PLAYERS) break;
        if (m->u.join_resp.num_players < 2 ||
            m->u.join_resp.num_players > ARENA_MAX_PLAYERS) break;
        lc->local_slot  = m->u.join_resp.slot;
        lc->num_players = m->u.join_resp.num_players;
        lc->peer[lc->local_slot].present = 1;
        lc->retries = 0;
        lc->stage = LOBBY_C_WAITING_PEERS;
        break;

    case LOBBY_REJECT:
        if (lc->stage != LOBBY_C_JOINING && lc->stage != LOBBY_C_REGISTERING) break;
        set_fail_text(lc, "REJECTED:", reject_name(m->u.reject.reason));
        lc->stage = LOBBY_C_FAILED;
        break;

    case LOBBY_PEER_INTRO: {
        if (lc->stage < LOBBY_C_REGISTERING || lc->stage > LOBBY_C_AWAITING_START) break;
        uint8_t s = m->u.peer_intro.slot;
        if (s >= ARENA_MAX_PLAYERS || s == lc->local_slot) break;
        /* endpoints are always refreshed: a re-intro after a retry may carry a
         * mapping the server observed more recently than the one we hold */
        lc->peer[s].pub  = m->u.peer_intro.public_ep;
        lc->peer[s].priv = m->u.peer_intro.private_ep;
        if (!lc->peer[s].present) {
            lc->peer[s].present        = 1;
            lc->peer[s].route          = -1;
            lc->peer[s].nonce          = next_rng(lc);
            lc->peer[s].punch_begin_ms = now;
            lc->peer[s].last_punch_ms  = now - LOBBY_PUNCH_PERIOD_MS;
        }
        break;
    }

    case LOBBY_PUNCH: {
        /* Reply to the SOURCE of the punch, not to any endpoint we were told
         * about: that reply is the packet that opens our side of the hole, and
         * it has to leave for the address their packet actually came from. */
        if (m->u.punch.session_id != lc->session_id || !lc->session_id) break;
        LobbyMsg r; memset(&r, 0, sizeof r);
        r.type = LOBBY_PUNCH_ACK;
        r.u.punch.session_id = lc->session_id;
        r.u.punch.from_slot  = lc->local_slot;
        r.u.punch.nonce      = m->u.punch.nonce;
        send_msg(lc, from, &r);
        break;
    }

    case LOBBY_PUNCH_ACK: {
        uint8_t s = m->u.punch.from_slot;
        if (m->u.punch.session_id != lc->session_id || !lc->session_id) break;
        if (s >= ARENA_MAX_PLAYERS || s == lc->local_slot) break;
        if (!lc->peer[s].present || !lc->peer[s].nonce) break;
        if (m->u.punch.nonce != lc->peer[s].nonce) break;
        if (lc->peer[s].acked) break;
        lc->peer[s].acked  = 1;
        lc->peer[s].chosen = from;              /* the address that answered */
        send_punch_report(lc, s, 1);
        lc->peer[s].reported    = 1;
        lc->peer[s].last_report_ms = now;
        break;
    }

    case LOBBY_PAIR_ROUTE: {
        uint8_t a = m->u.pair_route.slot_a, b = m->u.pair_route.slot_b;
        if (a >= ARENA_MAX_PLAYERS || b >= ARENA_MAX_PLAYERS || a == b) break;
        if (m->u.pair_route.route > LOBBY_ROUTE_RELAY) break;
        lc->route_known[a][b] = lc->route_known[b][a] = 1;
        if (a == lc->local_slot) lc->peer[b].route = (int8_t)m->u.pair_route.route;
        if (b == lc->local_slot) lc->peer[a].route = (int8_t)m->u.pair_route.route;
        break;
    }

    case LOBBY_START:
        if (lc->stage < LOBBY_C_WAITING_PEERS || lc->stage > LOBBY_C_AWAITING_START) break;
        if (lc->is_host) {
            /* the server echoes our own START back to us; a mismatch means the
             * two sides of the session disagree about the match parameters,
             * which would desync on tick 0 — refuse to start instead */
            if (m->u.start.seed != lc->seed || m->u.start.arena_id != lc->arena_id ||
                m->u.start.num_players != lc->num_players ||
                m->u.start.input_delay != lc->input_delay) {
                set_fail_text(lc, "START_ECHO", NULL);
                lc->stage = LOBBY_C_FAILED;
                break;
            }
        } else {
            if (m->u.start.num_players < 2 ||
                m->u.start.num_players > ARENA_MAX_PLAYERS) break;
            lc->seed        = m->u.start.seed;
            lc->arena_id    = m->u.start.arena_id;
            lc->num_players = m->u.start.num_players;
            lc->input_delay = m->u.start.input_delay;
        }
        lc->start_seen = 1;
        send_start_ack(lc);                     /* every START, refans included */
        break;

    default: break;   /* server-bound types, and GAME/RELAY that raced ahead of
                       * our own READY — gekko retransmits its sync handshake */
    }
}

/* ---------- entry points ---------- */
static void client_init(LobbyClient* lc, UdpSocket* sock, uint32_t now_ms) {
    memset(lc, 0, sizeof *lc);
    lc->sock     = sock;
    lc->begin_ms = now_ms;
    lc->rng      = (now_ms * 2654435761u) ^ ((uint32_t)udp_bound_port(sock) << 13) ^ 0x9E3779B9u;
    if (!lc->rng) lc->rng = 1u;
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++) lc->peer[i].route = -1;
}

int lobby_host_begin(LobbyClient* lc, UdpSocket* sock, LobbyEndpoint server,
                     uint8_t num_players, uint32_t seed, uint8_t arena_id,
                     uint8_t input_delay, uint32_t now_ms) {
    if (!lc || !sock) return -1;
    if (num_players < 2 || num_players > ARENA_MAX_PLAYERS) return -1;
    client_init(lc, sock, now_ms);
    lc->is_host     = 1;
    lc->server      = server;
    lc->num_players = num_players;
    lc->seed        = seed;
    lc->arena_id    = arena_id;
    lc->input_delay = input_delay;
    lc->net_ver     = arena_net_version();
    udp_local_endpoint_for(server, udp_bound_port(sock), &lc->private_ep);
    lc->stage        = LOBBY_C_REGISTERING;
    lc->last_send_ms = now_ms;
    send_host_req(lc);
    return 0;
}

int lobby_join_begin(LobbyClient* lc, UdpSocket* sock, const char* code,
                     uint32_t now_ms) {
    if (!lc || !sock || !code) return -1;
    LobbyEndpoint server; uint16_t sid;
    if (lobby_code_decode(code, &server, &sid) != 0) return -1;
    client_init(lc, sock, now_ms);
    lc->server     = server;
    lc->session_id = sid;
    lc->net_ver    = arena_net_version();
    udp_local_endpoint_for(server, udp_bound_port(sock), &lc->private_ep);
    lc->stage        = LOBBY_C_JOINING;
    lc->last_send_ms = now_ms;
    send_join_req(lc);
    return 0;
}

void lobby_set_forced_relay(LobbyClient* lc) { if (lc) lc->forced_relay = 1; }

const LobbyResult* lobby_result(const LobbyClient* lc) {
    return (lc && lc->stage == LOBBY_C_READY) ? &lc->result : NULL;
}
const char* lobby_code(const LobbyClient* lc) { return lc ? lc->code : ""; }
const char* lobby_fail_stage(const LobbyClient* lc) { return lc ? lc->fail_stage : ""; }

LobbyClientStage lobby_poll(LobbyClient* lc, uint32_t now_ms) {
    if (!lc || !lc->sock) return LOBBY_C_FAILED;
    if (lc->stage == LOBBY_C_READY || lc->stage == LOBBY_C_FAILED ||
        lc->stage == LOBBY_C_IDLE)
        return (LobbyClientStage)lc->stage;

    /* 1. drain the socket (bounded: a poll must never become a whole frame) */
    uint8_t buf[LOBBY_MAX_PACKET];
    LobbyEndpoint from;
    for (int i = 0; i < 128; i++) {
        int n = udp_recv(lc->sock, &from, buf, sizeof buf);
        if (n <= 0) break;
        LobbyMsg m;
        if (lobby_unpack(buf, (size_t)n, &m) != 0) continue;
        handle_msg(lc, now_ms, from, &m);
        if (lc->stage == LOBBY_C_FAILED) return LOBBY_C_FAILED;
    }

    /* 2. the one deadline that covers every stage */
    if (now_ms - lc->begin_ms > LOBBY_TIMEOUT_MS) { fail_here(lc); return LOBBY_C_FAILED; }

    /* 3. per-stage duty */
    switch (lc->stage) {
    case LOBBY_C_REGISTERING:
    case LOBBY_C_JOINING:
        if (now_ms - lc->last_send_ms >= LOBBY_RETRY_MS) {
            lc->last_send_ms = now_ms;
            if (++lc->retries > LOBBY_RETRY_MAX) { fail_here(lc); return LOBBY_C_FAILED; }
            if (lc->stage == LOBBY_C_REGISTERING) send_host_req(lc);
            else                                  send_join_req(lc);
        }
        break;

    case LOBBY_C_WAITING_PEERS:
        if (all_peers_present(lc)) { enter_punching(lc, now_ms); break; }
        /* Keep asking. PEER_INTRO is a one-shot server push and nothing else
         * ever resends it, so a single lost intro would strand us here waiting
         * for a peer that will never be introduced again — but the server's
         * idempotent-retry path replays the intros of every other member to
         * whoever repeats their request. So the request IS the retransmit.
         *
         * Deliberately not counted against LOBBY_RETRY_MAX: this stage is a wait
         * for other humans to press a button, which legitimately outlasts ten
         * retries. The 30s bootstrap deadline is what bounds it. */
        if (now_ms - lc->last_send_ms >= LOBBY_RETRY_MS) {
            lc->last_send_ms = now_ms;
            if (lc->is_host) send_host_req(lc);
            else             send_join_req(lc);
        }
        break;

    case LOBBY_C_PUNCHING:
        for (int i = 0; i < lc->num_players; i++) {
            if (i == lc->local_slot) continue;
            if (lc->peer[i].route >= 0) continue;      /* already arbitrated */

            if (lc->peer[i].reported) {
                /* the report may have been the packet that was lost; the server
                 * ignores a repeat once it has decided, so this is free */
                if (now_ms - lc->peer[i].last_report_ms >= LOBBY_RETRY_MS) {
                    lc->peer[i].last_report_ms = now_ms;
                    send_punch_report(lc, (uint8_t)i, lc->peer[i].acked ? 1u : 0u);
                }
                continue;
            }
            if (lc->forced_relay) {                    /* test hook: never probe */
                send_punch_report(lc, (uint8_t)i, 0);
                lc->peer[i].reported = 1;
                lc->peer[i].last_report_ms = now_ms;
                continue;
            }
            if (now_ms - lc->peer[i].punch_begin_ms > LOBBY_PUNCH_BUDGET_MS) {
                send_punch_report(lc, (uint8_t)i, 0);
                lc->peer[i].reported = 1;
                lc->peer[i].last_report_ms = now_ms;
                continue;
            }
            if (now_ms - lc->peer[i].last_punch_ms >= LOBBY_PUNCH_PERIOD_MS) {
                LobbyEndpoint dest = lc->peer[i].punch_alt ? lc->peer[i].pub
                                                           : lc->peer[i].priv;
                if (!dest.port) dest = lc->peer[i].punch_alt ? lc->peer[i].priv
                                                             : lc->peer[i].pub;
                lc->peer[i].punch_alt ^= 1u;
                if (dest.port) {
                    lc->peer[i].last_punch_ms = now_ms;
                    send_punch_to(lc, (uint8_t)i, dest);
                }
            }
        }
        break;

    default: break;
    }

    /* 4. host duty: START once every pair in the mesh has a route. Resent on
     *    the retry cadence until the server fans it back to us — that echo is
     *    the only proof the server ever saw it. */
    if (lc->is_host && lc->session_id && !lc->start_seen &&
        lc->stage >= LOBBY_C_WAITING_PEERS && all_pairs_decided(lc)) {
        if (!lc->host_start_sent || now_ms - lc->last_start_ms >= LOBBY_RETRY_MS) {
            lc->host_start_sent = 1;
            lc->last_start_ms   = now_ms;
            send_start(lc);
        }
    }

    /* 5. local routes all in: wait for START, then we are done */
    if (lc->stage == LOBBY_C_PUNCHING || lc->stage == LOBBY_C_AWAITING_START) {
        if (local_routes_decided(lc)) {
            if (lc->start_seen) {
                fill_result(lc);
                lc->stage = LOBBY_C_READY;
            } else {
                lc->stage = LOBBY_C_AWAITING_START;
            }
        }
    }

    /* 6. keepalive, so a slow lobby is not reaped under us */
    if (lc->session_id && lc->stage >= LOBBY_C_WAITING_PEERS &&
        lc->stage <= LOBBY_C_AWAITING_START) {
        if (!lc->last_keepalive_ms) lc->last_keepalive_ms = now_ms;
        else if (now_ms - lc->last_keepalive_ms >= LOBBY_KEEPALIVE_MS) {
            lc->last_keepalive_ms = now_ms;
            send_keepalive(lc);
        }
    }

    return (LobbyClientStage)lc->stage;
}

/* Post-READY duty. The socket now belongs to the adapter, so the only way to
 * see a control packet is through the adapter's lobby ring. The one that
 * matters is a START refan: the server keeps resending START until every peer
 * has acked, and if our ack was lost we must ack again or the session sits in
 * RV_STARTING and is reaped 30s later mid-match. */
void lobby_post_poll(LobbyClient* lc, uint32_t now_ms) {
    (void)now_ms;
    if (!lc || lc->stage != LOBBY_C_READY) return;
    uint8_t buf[LOBBY_MAX_PACKET];
    LobbyEndpoint from;
    int n;
    while ((n = net_adapter_take_lobby_packet(&from, buf, sizeof buf)) > 0) {
        LobbyMsg m;
        if (lobby_unpack(buf, (size_t)n, &m) != 0) continue;
        if (m.type == LOBBY_START) {
            send_start_ack(lc);
        } else if (m.type == LOBBY_PUNCH && m.u.punch.session_id == lc->session_id) {
            /* a straggler still probing us: answering costs one datagram and
             * keeps their hole open */
            LobbyMsg r; memset(&r, 0, sizeof r);
            r.type = LOBBY_PUNCH_ACK;
            r.u.punch.session_id = lc->session_id;
            r.u.punch.from_slot  = lc->local_slot;
            r.u.punch.nonce      = m.u.punch.nonce;
            send_msg(lc, from, &r);
        }
    }
}
