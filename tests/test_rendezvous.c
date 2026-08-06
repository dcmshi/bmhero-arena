/* A3 task 2: the rendezvous session table, driven entirely through the pure
 * core — fake packets, a fake clock, and an emit callback that captures every
 * byte the server would have sent. No sockets, no sleeping, no real time. */
#include <stdio.h>
#include <string.h>
#include "../src/lobby/rendezvous.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---------- emit capture ---------- */
typedef struct { LobbyEndpoint to; uint8_t buf[LOBBY_MAX_PACKET]; size_t len; } Sent;
static Sent sent[256]; static int nsent;
static void cap(void* ctx, LobbyEndpoint to, const uint8_t* b, size_t l) {
    (void)ctx;
    if (nsent < 256 && l <= LOBBY_MAX_PACKET) {
        sent[nsent].to = to; memcpy(sent[nsent].buf, b, l); sent[nsent].len = l; nsent++;
    }
}
static void reset(void) { nsent = 0; }

static LobbyEndpoint ep(uint32_t ip, uint16_t port) { LobbyEndpoint e = { ip, port }; return e; }
static int ep_eq(LobbyEndpoint a, LobbyEndpoint b) { return a.ip == b.ip && a.port == b.port; }

/* pack msg m, rv_handle it from `from` at time t */
static void send_msg(Rendezvous* rv, uint32_t t, LobbyEndpoint from, const LobbyMsg* m) {
    uint8_t b[LOBBY_MAX_PACKET]; size_t n = lobby_pack(m, b);
    CHECK(n > 0);
    rv_handle(rv, t, from, b, n, cap, NULL);
}

/* find the first captured msg of `type` sent to `to`, at or after index `start` */
static int find_from(int start, uint8_t type, LobbyEndpoint to, LobbyMsg* out) {
    for (int i = start; i < nsent; i++) {
        LobbyMsg q;
        if (!ep_eq(sent[i].to, to)) continue;
        if (lobby_unpack(sent[i].buf, sent[i].len, &q) != 0) continue;
        if (q.type != type) continue;
        if (out) *out = q;
        return i;
    }
    return -1;
}
static int find_sent(uint8_t type, LobbyEndpoint to, LobbyMsg* out) {
    return find_from(0, type, to, out);
}
static int count_sent(uint8_t type, LobbyEndpoint to) {
    int c = 0, i = -1;
    while ((i = find_from(i + 1, type, to, NULL)) >= 0) c++;
    return c;
}

/* ---------- message builders ---------- */
static void host_req(Rendezvous* rv, uint32_t t, LobbyEndpoint from,
                     uint32_t ver, uint8_t np) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_HOST_REQ;
    m.u.host_req.net_version = ver;
    m.u.host_req.num_players = np;
    m.u.host_req.private_ep  = ep(0xC0A80000u | (from.ip & 0xFFu), from.port);
    send_msg(rv, t, from, &m);
}
static void join_req(Rendezvous* rv, uint32_t t, LobbyEndpoint from,
                     uint16_t sid, uint32_t ver) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_JOIN_REQ;
    m.u.join_req.session_id  = sid;
    m.u.join_req.net_version = ver;
    m.u.join_req.private_ep  = ep(0xC0A80000u | (from.ip & 0xFFu), from.port);
    send_msg(rv, t, from, &m);
}
static void punch_report(Rendezvous* rv, uint32_t t, LobbyEndpoint from,
                         uint16_t sid, uint8_t a, uint8_t b, uint8_t ok) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_PUNCH_REPORT;
    m.u.punch_report.session_id = sid;
    m.u.punch_report.slot_a = a; m.u.punch_report.slot_b = b; m.u.punch_report.ok = ok;
    send_msg(rv, t, from, &m);
}
static void start_msg(Rendezvous* rv, uint32_t t, LobbyEndpoint from, uint8_t np) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_START;
    m.u.start.seed = 0xB0BB1E5; m.u.start.arena_id = 0;
    m.u.start.num_players = np; m.u.start.input_delay = 2;
    send_msg(rv, t, from, &m);
}
static void start_ack(Rendezvous* rv, uint32_t t, LobbyEndpoint from,
                      uint16_t sid, uint8_t slot) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_START_ACK;
    m.u.start_ack.session_id = sid; m.u.start_ack.slot = slot;
    send_msg(rv, t, from, &m);
}
static void relay_msg(Rendezvous* rv, uint32_t t, LobbyEndpoint from, uint16_t sid,
                      uint8_t fs, uint8_t ts, const uint8_t* pay, uint16_t pl) {
    LobbyMsg m; memset(&m, 0, sizeof m);
    m.type = LOBBY_RELAY;
    m.u.relay.session_id = sid; m.u.relay.from_slot = fs; m.u.relay.to_slot = ts;
    m.u.relay.payload_len = pl; m.u.relay.payload = pay;
    send_msg(rv, t, from, &m);
}

/* host at endpoint A with num_players np; returns the session id */
static uint16_t open_session(Rendezvous* rv, uint32_t t, LobbyEndpoint a,
                             uint32_t ver, uint8_t np) {
    LobbyMsg r; memset(&r, 0, sizeof r);
    reset();
    host_req(rv, t, a, ver, np);
    if (find_sent(LOBBY_HOST_RESP, a, &r) < 0) { CHECK(0); return 0; }
    return r.u.host_resp.session_id;
}

/* ---------- 1. host + join flow ---------- */
static void host_join_flow(void) {
    Rendezvous rv; rv_init(&rv, 1000, 42);
    LobbyEndpoint A = ep(0x01010101, 1001), B = ep(0x02020202, 1002),
                  C = ep(0x03030303, 1003), D = ep(0x04040404, 1004),
                  E = ep(0x05050505, 1005), F = ep(0x06060606, 1006);
    LobbyMsg r; memset(&r, 0, sizeof r);

    reset();
    host_req(&rv, 1000, A, 0x1111, 4);
    CHECK(nsent == 1);                              /* exactly one HOST_RESP */
    CHECK(find_sent(LOBBY_HOST_RESP, A, &r) >= 0);
    uint16_t sid = r.u.host_resp.session_id;
    CHECK(sid != 0);
    CHECK(rv_active_sessions(&rv) == 1);

    /* B joins -> slot 1, mutual intro with A */
    reset();
    join_req(&rv, 1010, B, sid, 0x1111);
    CHECK(find_sent(LOBBY_JOIN_RESP, B, &r) >= 0);
    CHECK(r.u.join_resp.slot == 1 && r.u.join_resp.num_players == 4);
    CHECK(count_sent(LOBBY_PEER_INTRO, A) == 1);
    CHECK(count_sent(LOBBY_PEER_INTRO, B) == 1);
    CHECK(find_sent(LOBBY_PEER_INTRO, A, &r) >= 0);
    CHECK(r.u.peer_intro.slot == 1 && ep_eq(r.u.peer_intro.public_ep, B));
    CHECK(find_sent(LOBBY_PEER_INTRO, B, &r) >= 0);
    CHECK(r.u.peer_intro.slot == 0 && ep_eq(r.u.peer_intro.public_ep, A));
    /* the private endpoint the peer advertised is carried through untouched */
    CHECK(r.u.peer_intro.private_ep.ip == (0xC0A80000u | (A.ip & 0xFFu)));

    /* C joins -> slot 2; C learns 0 and 1, A and B each learn C */
    reset();
    join_req(&rv, 1020, C, sid, 0x1111);
    CHECK(find_sent(LOBBY_JOIN_RESP, C, &r) >= 0 && r.u.join_resp.slot == 2);
    CHECK(count_sent(LOBBY_PEER_INTRO, C) == 2);
    CHECK(count_sent(LOBBY_PEER_INTRO, A) == 1);
    CHECK(count_sent(LOBBY_PEER_INTRO, B) == 1);

    /* D joins -> slot 3; D receives 3 intros, slots 0,1,2 */
    reset();
    join_req(&rv, 1030, D, sid, 0x1111);
    CHECK(find_sent(LOBBY_JOIN_RESP, D, &r) >= 0 && r.u.join_resp.slot == 3);
    CHECK(count_sent(LOBBY_PEER_INTRO, D) == 3);
    {
        int seen[RV_MAX_PLAYERS] = { 0, 0, 0, 0 }, i = -1;
        while ((i = find_from(i + 1, LOBBY_PEER_INTRO, D, &r)) >= 0)
            if (r.u.peer_intro.slot < RV_MAX_PLAYERS) seen[r.u.peer_intro.slot] = 1;
        CHECK(seen[0] && seen[1] && seen[2] && !seen[3]);
    }
    CHECK(count_sent(LOBBY_PEER_INTRO, A) == 1);
    CHECK(count_sent(LOBBY_PEER_INTRO, B) == 1);
    CHECK(count_sent(LOBBY_PEER_INTRO, C) == 1);

    /* fifth join -> FULL */
    reset();
    join_req(&rv, 1040, E, sid, 0x1111);
    CHECK(find_sent(LOBBY_REJECT, E, &r) >= 0 && r.u.reject.reason == LOBBY_REJ_FULL);
    CHECK(count_sent(LOBBY_JOIN_RESP, E) == 0);
    CHECK(rv_active_sessions(&rv) == 1);            /* a rejected join makes nothing */

    /* wrong version -> VERSION_MISMATCH (checked before FULL) */
    reset();
    join_req(&rv, 1050, F, sid, 0x2222);
    CHECK(find_sent(LOBBY_REJECT, F, &r) >= 0 &&
          r.u.reject.reason == LOBBY_REJ_VERSION_MISMATCH);

    /* unknown session -> BAD_SESSION */
    reset();
    join_req(&rv, 1060, F, (uint16_t)(sid + 1u), 0x1111);
    CHECK(find_sent(LOBBY_REJECT, F, &r) >= 0 &&
          r.u.reject.reason == LOBBY_REJ_BAD_SESSION);

    /* lobby over (phase STARTING) -> EXPIRED */
    start_msg(&rv, 1070, A, 4);
    reset();
    join_req(&rv, 1080, F, sid, 0x1111);
    CHECK(find_sent(LOBBY_REJECT, F, &r) >= 0 && r.u.reject.reason == LOBBY_REJ_EXPIRED);

    /* still exactly one session through all of that */
    CHECK(rv_active_sessions(&rv) == 1);
}

/* The lobby size is the HOST's num_players, not RV_MAX_PLAYERS: a 2-player
 * session must refuse a third joiner even though two slots are still free.
 * (That bound is a separate guard from "no free slot" and needs its own test —
 * mutation-testing showed removing it changed nothing in a 4-player lobby.) */
static void lobby_size_is_the_hosts(void) {
    Rendezvous rv; rv_init(&rv, 1000, 42);
    LobbyEndpoint A = ep(0x21212121, 2101), B = ep(0x22222221, 2102),
                  C = ep(0x23232321, 2103);
    LobbyMsg r; memset(&r, 0, sizeof r);

    uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
    join_req(&rv, 1010, B, sid, 0x1111);
    reset();
    join_req(&rv, 1020, C, sid, 0x1111);
    CHECK(find_sent(LOBBY_REJECT, C, &r) >= 0 && r.u.reject.reason == LOBBY_REJ_FULL);
    CHECK(count_sent(LOBBY_JOIN_RESP, C) == 0);

    /* a nonsense num_players is clamped into [2, RV_MAX_PLAYERS] */
    LobbyEndpoint D = ep(0x24242421, 2104), E = ep(0x25252521, 2105);
    uint16_t big = open_session(&rv, 1030, D, 0x1111, 99);
    reset();
    join_req(&rv, 1040, E, big, 0x1111);
    CHECK(find_sent(LOBBY_JOIN_RESP, E, &r) >= 0 && r.u.join_resp.num_players == RV_MAX_PLAYERS);

    LobbyEndpoint F = ep(0x26262621, 2106), G = ep(0x27272721, 2107);
    uint16_t tiny = open_session(&rv, 1050, F, 0x1111, 0);
    reset();
    join_req(&rv, 1060, G, tiny, 0x1111);
    CHECK(find_sent(LOBBY_JOIN_RESP, G, &r) >= 0 && r.u.join_resp.num_players == 2);
}

/* ---------- 2. retries are idempotent ---------- */
static void host_retry_idempotent(void) {
    Rendezvous rv; rv_init(&rv, 1000, 42);
    LobbyEndpoint A = ep(0x0A0A0A0A, 2001), B = ep(0x0B0B0B0B, 2002);
    LobbyMsg r1, r2; memset(&r1, 0, sizeof r1); memset(&r2, 0, sizeof r2);

    reset();
    host_req(&rv, 1000, A, 0x1111, 4);
    CHECK(find_sent(LOBBY_HOST_RESP, A, &r1) >= 0);
    reset();
    host_req(&rv, 1100, A, 0x1111, 4);              /* the client's retry */
    CHECK(find_sent(LOBBY_HOST_RESP, A, &r2) >= 0);
    CHECK(r1.u.host_resp.session_id == r2.u.host_resp.session_id);
    CHECK(rv_active_sessions(&rv) == 1);            /* no second session */

    uint16_t sid = r1.u.host_resp.session_id;

    reset();
    join_req(&rv, 1200, B, sid, 0x1111);
    CHECK(find_sent(LOBBY_JOIN_RESP, B, &r1) >= 0);
    CHECK(r1.u.join_resp.slot == 1);
    int intros_first = count_sent(LOBBY_PEER_INTRO, B);
    CHECK(intros_first == 1);

    reset();
    join_req(&rv, 1300, B, sid, 0x1111);            /* the client's retry */
    CHECK(find_sent(LOBBY_JOIN_RESP, B, &r2) >= 0);
    CHECK(r2.u.join_resp.slot == 1);                /* same slot re-sent */
    CHECK(rv_active_sessions(&rv) == 1);

    /* a third endpoint still lands on slot 2, proving `joined` never grew */
    reset();
    join_req(&rv, 1400, ep(0x0C0C0C0C, 2003), sid, 0x1111);
    CHECK(find_sent(LOBBY_JOIN_RESP, ep(0x0C0C0C0C, 2003), &r2) >= 0);
    CHECK(r2.u.join_resp.slot == 2);
}

/* ---------- 3. punch reports -> pair routes ---------- */
static void routes(void) {
    LobbyEndpoint A = ep(0x11111111, 3001), B = ep(0x22222222, 3002);
    LobbyMsg r; memset(&r, 0, sizeof r);

    /* both sides succeeded -> DIRECT, to both peers, exactly once */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1010, B, sid, 0x1111);

        reset();
        punch_report(&rv, 1020, A, sid, 0, 1, 1);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, A) == 0);   /* one side only */
        CHECK(count_sent(LOBBY_PAIR_ROUTE, B) == 0);

        punch_report(&rv, 1030, B, sid, 0, 1, 1);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, A) == 1);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, B) == 1);
        CHECK(find_sent(LOBBY_PAIR_ROUTE, A, &r) >= 0);
        CHECK(r.u.pair_route.slot_a == 0 && r.u.pair_route.slot_b == 1);
        CHECK(r.u.pair_route.route == LOBBY_ROUTE_DIRECT);

        /* duplicate reports do not re-emit */
        punch_report(&rv, 1040, A, sid, 0, 1, 1);
        punch_report(&rv, 1050, B, sid, 0, 1, 1);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, A) == 1);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, B) == 1);
    }

    /* one side failed -> RELAY */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1010, B, sid, 0x1111);
        reset();
        punch_report(&rv, 1020, A, sid, 0, 1, 1);
        punch_report(&rv, 1030, B, sid, 0, 1, 0);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, A) == 1);
        CHECK(find_sent(LOBBY_PAIR_ROUTE, A, &r) >= 0);
        CHECK(r.u.pair_route.route == LOBBY_ROUTE_RELAY);
    }

    /* a report from an endpoint that is not in the session is ignored */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1010, B, sid, 0x1111);
        reset();
        punch_report(&rv, 1020, A, sid, 0, 1, 1);
        punch_report(&rv, 1030, ep(0x99999999, 9999), sid, 0, 1, 1);   /* impostor */
        CHECK(count_sent(LOBBY_PAIR_ROUTE, A) == 0);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, B) == 0);
    }

    /* a report about a pair the reporter is not part of is ignored */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 4);
        join_req(&rv, 1010, B, sid, 0x1111);
        join_req(&rv, 1020, ep(0x33333333, 3003), sid, 0x1111);
        reset();
        punch_report(&rv, 1030, A, sid, 1, 2, 1);      /* A is neither 1 nor 2 */
        punch_report(&rv, 1040, B, sid, 1, 2, 1);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, A) == 0);   /* only one real verdict */
    }
}

/* ---------- 4. START fan-out, refan, acks ---------- */
static void start_refan_acks(void) {
    Rendezvous rv; rv_init(&rv, 1000, 42);
    LobbyEndpoint A = ep(0x41414141, 4001), B = ep(0x42424242, 4002),
                  C = ep(0x43434343, 4003);
    LobbyMsg r; memset(&r, 0, sizeof r);

    uint16_t sid = open_session(&rv, 1000, A, 0x1111, 3);
    join_req(&rv, 1010, B, sid, 0x1111);
    join_req(&rv, 1020, C, sid, 0x1111);

    /* START from a non-host endpoint is ignored */
    reset();
    start_msg(&rv, 1030, B, 3);
    CHECK(nsent == 0);

    /* START from the host fans to everyone */
    reset();
    start_msg(&rv, 1040, A, 3);
    CHECK(count_sent(LOBBY_START, A) == 1);
    CHECK(count_sent(LOBBY_START, B) == 1);
    CHECK(count_sent(LOBBY_START, C) == 1);
    CHECK(find_sent(LOBBY_START, C, &r) >= 0);
    CHECK(r.u.start.seed == 0xB0BB1E5 && r.u.start.num_players == 3 &&
          r.u.start.input_delay == 2);

    /* a tick before the refan interval does nothing */
    reset();
    rv_tick(&rv, 1040 + 100, cap, NULL);
    CHECK(nsent == 0);

    /* B acks; the +600ms refan then reaches only A and C */
    start_ack(&rv, 1100, B, sid, 1);
    reset();
    rv_tick(&rv, 1040 + 600, cap, NULL);
    CHECK(count_sent(LOBBY_START, A) == 1);
    CHECK(count_sent(LOBBY_START, B) == 0);          /* acked: not refanned */
    CHECK(count_sent(LOBBY_START, C) == 1);

    /* everyone acks -> RUNNING, and refans stop */
    start_ack(&rv, 1700, A, sid, 0);
    start_ack(&rv, 1700, C, sid, 2);
    reset();
    rv_tick(&rv, 1040 + 1200, cap, NULL);
    CHECK(nsent == 0);
    rv_tick(&rv, 1040 + 1800, cap, NULL);
    CHECK(nsent == 0);
    CHECK(rv_active_sessions(&rv) == 1);             /* still alive, just quiet */
}

/* ---------- 5. relay forwarding ---------- */
static void relay_forwarding(void) {
    Rendezvous rv; rv_init(&rv, 1000, 42);
    LobbyEndpoint A = ep(0x51515151, 5001), B = ep(0x52525252, 5002),
                  C = ep(0x53535353, 5003);
    static const uint8_t pay[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    LobbyMsg r; memset(&r, 0, sizeof r);

    uint16_t sid = open_session(&rv, 1000, A, 0x1111, 3);
    join_req(&rv, 1010, B, sid, 0x1111);
    join_req(&rv, 1020, C, sid, 0x1111);

    /* peer 1 -> peer 2, forwarded byte-identically to peer 2's public ep */
    reset();
    relay_msg(&rv, 1030, B, sid, 1, 2, pay, 8);
    CHECK(nsent == 1);
    if (nsent == 1) {
        uint8_t expect[LOBBY_MAX_PACKET]; LobbyMsg m;
        memset(&m, 0, sizeof m);
        m.type = LOBBY_RELAY;
        m.u.relay.session_id = sid; m.u.relay.from_slot = 1; m.u.relay.to_slot = 2;
        m.u.relay.payload_len = 8; m.u.relay.payload = pay;
        size_t en = lobby_pack(&m, expect);
        CHECK(ep_eq(sent[0].to, C));
        CHECK(sent[0].len == en && memcmp(sent[0].buf, expect, en) == 0);
    }

    /* wrong source endpoint for the claimed from_slot -> dropped */
    reset();
    relay_msg(&rv, 1040, C, sid, 1, 2, pay, 8);      /* C claims to be slot 1 */
    CHECK(nsent == 0);
    reset();
    relay_msg(&rv, 1045, ep(0x99999999, 9999), sid, 1, 2, pay, 8);
    CHECK(nsent == 0);

    /* unknown session -> dropped */
    reset();
    relay_msg(&rv, 1050, B, (uint16_t)(sid + 1u), 1, 2, pay, 8);
    CHECK(nsent == 0);

    /* unoccupied destination slot -> dropped */
    reset();
    relay_msg(&rv, 1060, B, sid, 1, 3, pay, 8);
    CHECK(nsent == 0);
    reset();
    relay_msg(&rv, 1065, B, sid, 1, 9, pay, 8);      /* out of range */
    CHECK(nsent == 0);

    /* a relay keeps the session alive past the 30s no-relay deadline only if a
     * RELAY route exists; here it just proves last_relay_ms is being written,
     * observed through the expiry rule in the expiry() scenario. A plain
     * forward is still counted as activity: */
    reset();
    relay_msg(&rv, 1070, B, sid, 1, 0, pay, 8);
    CHECK(nsent == 1 && ep_eq(sent[0].to, A));

    /* relaying to self is legal (loopback echo) but must still be verified */
    reset();
    relay_msg(&rv, 1080, B, sid, 1, 1, pay, 8);
    CHECK(nsent == 1 && ep_eq(sent[0].to, B));

    /* a zero-length payload relays fine */
    reset();
    relay_msg(&rv, 1090, B, sid, 1, 2, NULL, 0);
    CHECK(nsent == 1 && ep_eq(sent[0].to, C));
    CHECK(find_sent(LOBBY_RELAY, C, &r) >= 0 && r.u.relay.payload_len == 0);
}

/* ---------- 6. expiry ---------- */
static void expiry(void) {
    LobbyEndpoint A = ep(0x61616161, 6001), B = ep(0x62626262, 6002);
    LobbyMsg r; memset(&r, 0, sizeof r);
    static const uint8_t pay[4] = { 1, 2, 3, 4 };

    /* an idle LOBBY session expires; its id then reads as BAD_SESSION */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        reset();
        rv_tick(&rv, 1000 + RV_LOBBY_EXPIRY_MS - 1, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 1);
        rv_tick(&rv, 1000 + RV_LOBBY_EXPIRY_MS + 1, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 0);
        reset();
        join_req(&rv, 1000 + RV_LOBBY_EXPIRY_MS + 2, B, sid, 0x1111);
        CHECK(find_sent(LOBBY_REJECT, B, &r) >= 0 &&
              r.u.reject.reason == LOBBY_REJ_BAD_SESSION);
    }

    /* activity postpones LOBBY expiry */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1000 + RV_LOBBY_EXPIRY_MS - 100, B, sid, 0x1111);
        rv_tick(&rv, 1000 + RV_LOBBY_EXPIRY_MS + 1, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 1);         /* the join reset the clock */
    }

    /* RUNNING with no relay route: freed 30s after started_ms */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1010, B, sid, 0x1111);
        punch_report(&rv, 1020, A, sid, 0, 1, 1);
        punch_report(&rv, 1030, B, sid, 0, 1, 1);    /* DIRECT: no relay route */
        start_msg(&rv, 1040, A, 2);
        start_ack(&rv, 1050, A, sid, 0);
        start_ack(&rv, 1050, B, sid, 1);
        reset();
        rv_tick(&rv, 1040 + RV_STARTED_EXPIRY_MS - 1, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 1);
        rv_tick(&rv, 1040 + RV_STARTED_EXPIRY_MS + 1, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 0);
    }

    /* RUNNING with a RELAY route: outlives 30s, dies on 60s of relay silence */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1010, B, sid, 0x1111);
        punch_report(&rv, 1020, A, sid, 0, 1, 1);
        punch_report(&rv, 1030, B, sid, 0, 1, 0);    /* -> RELAY route */
        start_msg(&rv, 1040, A, 2);
        start_ack(&rv, 1050, A, sid, 0);
        start_ack(&rv, 1050, B, sid, 1);
        rv_tick(&rv, 1040 + RV_STARTED_EXPIRY_MS + 1, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 1);         /* the 30s rule must not apply */

        uint32_t t = 40000;
        relay_msg(&rv, t, B, sid, 1, 0, pay, 4);
        rv_tick(&rv, t + RV_RELAY_EXPIRY_MS - 1000, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 1);         /* +59s: alive */
        rv_tick(&rv, t + RV_RELAY_EXPIRY_MS + 1000, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 0);         /* +61s: gone */
    }

    /* A RELAY route negotiated early must not have its silence clock running
     * while the lobby is still waiting: the host can dawdle for minutes, and
     * the first tick after START would otherwise reap a brand-new match. */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1010, B, sid, 0x1111);
        punch_report(&rv, 1020, A, sid, 0, 1, 1);
        punch_report(&rv, 1030, B, sid, 0, 1, 0);    /* RELAY route exists at 1030 */
        uint32_t late = 1030 + 5u * 60u * 1000u;     /* ...and the host waits 5 min */
        start_msg(&rv, late, A, 2);
        start_ack(&rv, late + 10, A, sid, 0);
        start_ack(&rv, late + 10, B, sid, 1);
        rv_tick(&rv, late + 20, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 1);
        /* and the clock now runs from START, not from route creation */
        rv_tick(&rv, late + RV_RELAY_EXPIRY_MS - 1000, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 1);
        rv_tick(&rv, late + RV_RELAY_EXPIRY_MS + 1000, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 0);
    }

    /* a freed slot is reusable and fully reset (punch verdicts do not leak) */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1010, B, sid, 0x1111);
        punch_report(&rv, 1020, A, sid, 0, 1, 1);
        /* 1020, not 1000: the punch report was activity and reset the clock */
        rv_tick(&rv, 1020 + RV_LOBBY_EXPIRY_MS + 1, cap, NULL);
        CHECK(rv_active_sessions(&rv) == 0);
        uint32_t t = 1020 + RV_LOBBY_EXPIRY_MS + 10;
        uint16_t sid2 = open_session(&rv, t, A, 0x1111, 2);
        CHECK(sid2 != 0);
        join_req(&rv, t + 10, B, sid2, 0x1111);
        reset();
        punch_report(&rv, t + 20, B, sid2, 0, 1, 1); /* only B has reported now */
        CHECK(count_sent(LOBBY_PAIR_ROUTE, A) == 0); /* stale A verdict must be gone */
        punch_report(&rv, t + 30, A, sid2, 0, 1, 1);
        CHECK(count_sent(LOBBY_PAIR_ROUTE, A) == 1);
    }
}

/* ---------- 7. rate limiting ---------- */
static void rate_limit(void) {
    LobbyEndpoint A = ep(0x71717171, 7001), B = ep(0x72727272, 7002);
    static const uint8_t pay[4] = { 1, 2, 3, 4 };

    /* a flood of HOST_REQs from one IP at one instant is capped by the burst */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        reset();
        for (int i = 0; i < 100; i++) host_req(&rv, 1000, A, 0x1111, 4);
        CHECK(nsent <= RV_RATE_BURST);
        CHECK(nsent >= 10);                          /* not over-throttled either */
        CHECK(rv_active_sessions(&rv) == 1);         /* idempotent, so still one */
    }

    /* the bucket refills over time */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        reset();
        for (int i = 0; i < 100; i++) host_req(&rv, 1000, A, 0x1111, 4);
        int burst = nsent;
        reset();
        host_req(&rv, 1000, A, 0x1111, 4);
        CHECK(nsent == 0);                           /* drained */
        reset();
        host_req(&rv, 1000 + 2000, A, 0x1111, 4);    /* 2s later: refilled */
        CHECK(nsent == 1);
        CHECK(burst > 0);
    }

    /* a flood from one IP does not starve a different IP */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        for (int i = 0; i < 100; i++) host_req(&rv, 1000, A, 0x1111, 4);
        reset();
        host_req(&rv, 1000, ep(0x81818181, 8001), 0x1111, 4);
        CHECK(nsent == 1);
    }

    /* RELAY is NOT rate limited: game traffic must never be throttled */
    {
        Rendezvous rv; rv_init(&rv, 1000, 42);
        uint16_t sid = open_session(&rv, 1000, A, 0x1111, 2);
        join_req(&rv, 1000, B, sid, 0x1111);
        reset();
        for (int i = 0; i < 100; i++) relay_msg(&rv, 1000, B, sid, 1, 0, pay, 4);
        CHECK(nsent == 100);
    }
}

/* ---------- garbage in ---------- */
static void garbage(void) {
    Rendezvous rv; rv_init(&rv, 1000, 42);
    LobbyEndpoint A = ep(0x91919191, 9001);
    uint8_t junk[64];
    uint32_t r = 0xDEADBEEF;
    LobbyMsg q; memset(&q, 0, sizeof q);
    memset(junk, 0, sizeof junk);
    reset();
    for (int i = 0; i < 20000; i++) {
        size_t len = (size_t)(r % 64u);
        for (size_t j = 0; j < len; j++) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; junk[j] = (uint8_t)r; }
        rv_handle(&rv, 1000 + (uint32_t)i, ep(r, (uint16_t)i), junk, len, cap, NULL);
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    }
    rv_tick(&rv, 100000, cap, NULL);
    rv_handle(&rv, 200000, A, NULL, 0, cap, NULL);       /* NULL buffer, zero len */
    /* noise must not manufacture sessions, and the table must still work */
    CHECK(rv_active_sessions(&rv) == 0);
    reset();
    host_req(&rv, 200010, A, 0x1111, 4);
    CHECK(find_sent(LOBBY_HOST_RESP, A, &q) >= 0 && q.u.host_resp.session_id != 0);
    CHECK(rv_active_sessions(&rv) == 1);
}

/* ---------- session table saturation ---------- */
static void table_full(void) {
    Rendezvous rv; rv_init(&rv, 1000, 42);
    LobbyMsg r; memset(&r, 0, sizeof r);
    /* spread hosts across distinct IPs so the rate limiter never fires */
    for (int i = 0; i < RV_MAX_SESSIONS; i++)
        host_req(&rv, 1000, ep(0xA0000000u | (uint32_t)i, 5000), 0x1111, 4);
    CHECK(rv_active_sessions(&rv) == RV_MAX_SESSIONS);
    LobbyEndpoint over = ep(0xB0000001u, 5000);
    reset();
    host_req(&rv, 1000, over, 0x1111, 4);
    CHECK(find_sent(LOBBY_REJECT, over, &r) >= 0 && r.u.reject.reason == LOBBY_REJ_FULL);
    CHECK(rv_active_sessions(&rv) == RV_MAX_SESSIONS);
    /* Every allocated id is nonzero and unique — across MANY rng seeds, not
     * just the one. A full table draws 64 ids from 65536, so a single seed has
     * only a ~3% chance of colliding: testing one seed cannot tell a real
     * allocator from `id = rng` (mutation-testing showed exactly that). */
    {
        int bad = 0;
        for (uint32_t seed = 1; seed <= 200u; seed++) {
            Rendezvous r2; rv_init(&r2, 1000, seed);
            for (int i = 0; i < RV_MAX_SESSIONS; i++)
                host_req(&r2, 1000, ep(0xA0000000u | (uint32_t)i, 5000), 0x1111, 4);
            if (rv_active_sessions(&r2) != RV_MAX_SESSIONS) { bad++; continue; }
            for (int i = 0; i < RV_MAX_SESSIONS; i++) {
                if (r2.sessions[i].id == 0) bad++;
                for (int j = i + 1; j < RV_MAX_SESSIONS; j++)
                    if (r2.sessions[i].id == r2.sessions[j].id) bad++;
            }
        }
        CHECK(bad == 0);
    }
}

int main(void) {
    host_join_flow();
    lobby_size_is_the_hosts();
    host_retry_idempotent();
    routes();
    start_refan_acks();
    relay_forwarding();
    expiry();
    rate_limit();
    garbage();
    table_full();
    if (fails) { printf("test_rendezvous: %d FAILED\n", fails); return 1; }
    printf("test_rendezvous: all passed\n");
    return 0;
}
