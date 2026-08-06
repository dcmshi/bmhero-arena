/* A3 task 1: lobby wire codec, lobby codes, arena_net_version.
 * Every one of the 15 message types roundtrips field-for-field, the packed
 * size of each is pinned (a silent layout change is a wire break), malformed
 * input never succeeds, and 10k random buffers never crash the unpacker. */
#include <stdio.h>
#include <string.h>
#include "../src/lobby/lobby_proto.h"
#include "../src/arena/arena_script.h"
#include "../src/arena/arena_tuning.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static LobbyEndpoint ep(uint32_t ip, uint16_t port) { LobbyEndpoint e = { ip, port }; return e; }

int main(void) {
    uint8_t buf[LOBBY_MAX_PACKET];
    LobbyMsg m, o;
    size_t n;

    memset(&o, 0, sizeof o);

    /* ---- 1. HOST_REQ (padded to LOBBY_REQ_PAD_SIZE) ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_HOST_REQ;
    m.u.host_req.net_version = 0xDEADBEEF;
    m.u.host_req.num_players = 4;
    m.u.host_req.private_ep  = ep(0xC0A80102, 7777);
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_REQ_PAD_SIZE);                    /* padded */
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_HOST_REQ);
    CHECK(o.u.host_req.net_version == 0xDEADBEEF);
    CHECK(o.u.host_req.num_players == 4);
    CHECK(o.u.host_req.private_ep.ip == 0xC0A80102 && o.u.host_req.private_ep.port == 7777);
    /* an unpadded HOST_REQ is refused (anti-amplification) */
    CHECK(lobby_unpack(buf, LOBBY_HDR_SIZE + 11, &o) == -1);

    /* ---- 2. HOST_RESP ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_HOST_RESP;
    m.u.host_resp.session_id = 0x1234;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 2);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_HOST_RESP && o.u.host_resp.session_id == 0x1234);

    /* ---- 3. JOIN_REQ (padded) ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_JOIN_REQ;
    m.u.join_req.session_id  = 0xABCD;
    m.u.join_req.net_version = 0x11223344;
    m.u.join_req.private_ep  = ep(0x0A000005, 9999);
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_REQ_PAD_SIZE);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_JOIN_REQ);
    CHECK(o.u.join_req.session_id == 0xABCD);
    CHECK(o.u.join_req.net_version == 0x11223344);
    CHECK(o.u.join_req.private_ep.ip == 0x0A000005 && o.u.join_req.private_ep.port == 9999);
    CHECK(lobby_unpack(buf, LOBBY_HDR_SIZE + 12, &o) == -1);   /* unpadded refused */

    /* ---- 4. JOIN_RESP ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_JOIN_RESP;
    m.u.join_resp.slot = 3;
    m.u.join_resp.num_players = 4;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 2);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_JOIN_RESP && o.u.join_resp.slot == 3 && o.u.join_resp.num_players == 4);

    /* ---- 5. REJECT ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_REJECT;
    m.u.reject.reason = LOBBY_REJ_VERSION_MISMATCH;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 1);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_REJECT && o.u.reject.reason == LOBBY_REJ_VERSION_MISMATCH);

    /* ---- 6. PEER_INTRO ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_PEER_INTRO;
    m.u.peer_intro.slot = 2;
    m.u.peer_intro.public_ep  = ep(0x4A7D0101, 30000);
    m.u.peer_intro.private_ep = ep(0xC0A8010A, 30001);
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 13);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_PEER_INTRO && o.u.peer_intro.slot == 2);
    CHECK(o.u.peer_intro.public_ep.ip == 0x4A7D0101 && o.u.peer_intro.public_ep.port == 30000);
    CHECK(o.u.peer_intro.private_ep.ip == 0xC0A8010A && o.u.peer_intro.private_ep.port == 30001);

    /* ---- 7. PUNCH ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_PUNCH;
    m.u.punch.session_id = 0x5A5A;
    m.u.punch.from_slot  = 1;
    m.u.punch.nonce      = 0xFEEDFACE;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 7);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_PUNCH && o.u.punch.session_id == 0x5A5A);
    CHECK(o.u.punch.from_slot == 1 && o.u.punch.nonce == 0xFEEDFACE);

    /* ---- 8. PUNCH_ACK (same layout, echoes the nonce) ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_PUNCH_ACK;
    m.u.punch.session_id = 0x0F0F;
    m.u.punch.from_slot  = 2;
    m.u.punch.nonce      = 0x01020304;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 7);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_PUNCH_ACK && o.u.punch.session_id == 0x0F0F);
    CHECK(o.u.punch.from_slot == 2 && o.u.punch.nonce == 0x01020304);

    /* ---- 9. PUNCH_REPORT ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_PUNCH_REPORT;
    m.u.punch_report.session_id = 0x7777;
    m.u.punch_report.slot_a = 0;
    m.u.punch_report.slot_b = 3;
    m.u.punch_report.ok     = 1;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 5);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_PUNCH_REPORT && o.u.punch_report.session_id == 0x7777);
    CHECK(o.u.punch_report.slot_a == 0 && o.u.punch_report.slot_b == 3 && o.u.punch_report.ok == 1);

    /* ---- 10. PAIR_ROUTE ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_PAIR_ROUTE;
    m.u.pair_route.slot_a = 1;
    m.u.pair_route.slot_b = 2;
    m.u.pair_route.route  = LOBBY_ROUTE_RELAY;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 3);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_PAIR_ROUTE && o.u.pair_route.slot_a == 1);
    CHECK(o.u.pair_route.slot_b == 2 && o.u.pair_route.route == LOBBY_ROUTE_RELAY);

    /* ---- 11. RELAY (with payload) ---- */
    static const uint8_t pay[5] = { 1, 2, 3, 4, 5 };
    memset(&m, 0, sizeof m);
    m.type = LOBBY_RELAY;
    m.u.relay.session_id = 0xBEEF; m.u.relay.from_slot = 1; m.u.relay.to_slot = 3;
    m.u.relay.payload_len = 5; m.u.relay.payload = pay;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 6 + 5);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_RELAY && o.u.relay.session_id == 0xBEEF);
    CHECK(o.u.relay.from_slot == 1 && o.u.relay.to_slot == 3);
    CHECK(o.u.relay.payload_len == 5 && memcmp(o.u.relay.payload, pay, 5) == 0);
    CHECK(o.u.relay.payload == buf + LOBBY_HDR_SIZE + 6);   /* points into buf */

    /* ---- 12. GAME (same layout as RELAY) ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_GAME;
    m.u.relay.session_id = 0x0102; m.u.relay.from_slot = 2; m.u.relay.to_slot = 0;
    m.u.relay.payload_len = 5; m.u.relay.payload = pay;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 6 + 5);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_GAME && o.u.relay.session_id == 0x0102);
    CHECK(o.u.relay.from_slot == 2 && o.u.relay.to_slot == 0);
    CHECK(o.u.relay.payload_len == 5 && memcmp(o.u.relay.payload, pay, 5) == 0);
    /* zero-length payload is legal */
    m.u.relay.payload_len = 0; m.u.relay.payload = NULL;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 6);
    CHECK(lobby_unpack(buf, n, &o) == 0 && o.u.relay.payload_len == 0);
    /* max payload fits inside LOBBY_MAX_PACKET */
    {
        static uint8_t big[LOBBY_MAX_PAYLOAD];
        for (int i = 0; i < LOBBY_MAX_PAYLOAD; i++) big[i] = (uint8_t)i;
        m.u.relay.payload_len = LOBBY_MAX_PAYLOAD; m.u.relay.payload = big;
        n = lobby_pack(&m, buf);
        CHECK(n == LOBBY_HDR_SIZE + 6 + LOBBY_MAX_PAYLOAD && n <= LOBBY_MAX_PACKET);
        CHECK(lobby_unpack(buf, n, &o) == 0);
        CHECK(o.u.relay.payload_len == LOBBY_MAX_PAYLOAD &&
              memcmp(o.u.relay.payload, big, LOBBY_MAX_PAYLOAD) == 0);
    }

    /* ---- 13. START ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_START;
    m.u.start.seed = 0xB0BB1E5;
    m.u.start.arena_id = 0;
    m.u.start.num_players = 4;
    m.u.start.input_delay = 2;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 7);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_START && o.u.start.seed == 0xB0BB1E5);
    CHECK(o.u.start.arena_id == 0 && o.u.start.num_players == 4 && o.u.start.input_delay == 2);

    /* ---- 14. START_ACK ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_START_ACK;
    m.u.start_ack.session_id = 0xCAFE;
    m.u.start_ack.slot = 2;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 3);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_START_ACK && o.u.start_ack.session_id == 0xCAFE && o.u.start_ack.slot == 2);

    /* ---- 15. KEEPALIVE (same layout as START_ACK) ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_KEEPALIVE;
    m.u.start_ack.session_id = 0x4242;
    m.u.start_ack.slot = 1;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 3);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_KEEPALIVE && o.u.start_ack.session_id == 0x4242 && o.u.start_ack.slot == 1);

    /* pack refuses invalid types and NULL */
    memset(&m, 0, sizeof m); m.type = 0;
    CHECK(lobby_pack(&m, buf) == 0);
    m.type = (uint8_t)LOBBY_MSG_MAX;
    CHECK(lobby_pack(&m, buf) == 0);
    CHECK(lobby_pack(NULL, buf) == 0);

    /* ---- malformed input never succeeds ---- */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_RELAY;
    m.u.relay.session_id = 0xBEEF; m.u.relay.from_slot = 1; m.u.relay.to_slot = 3;
    m.u.relay.payload_len = 5; m.u.relay.payload = pay;
    n = lobby_pack(&m, buf);
    CHECK(lobby_unpack(buf, 3, &o) == -1);             /* short */
    buf[0] ^= 0xFF; CHECK(lobby_unpack(buf, n, &o) == -1); buf[0] ^= 0xFF; /* magic */
    buf[4] = 99;    CHECK(lobby_unpack(buf, n, &o) == -1); buf[4] = LOBBY_PROTO_VER; /* ver */
    buf[5] = 0;     CHECK(lobby_unpack(buf, n, &o) == -1);  /* type 0 */
    buf[5] = (uint8_t)LOBBY_MSG_MAX;
    CHECK(lobby_unpack(buf, n, &o) == -1);             /* type out of range */
    buf[5] = LOBBY_RELAY;
    CHECK(lobby_unpack(buf, n - 3, &o) == -1);         /* truncated payload */
    CHECK(lobby_unpack(NULL, n, &o) == -1);
    CHECK(lobby_unpack(buf, n, NULL) == -1);

    /* every type: one byte short of its own layout is refused */
    for (int t = 1; t < LOBBY_MSG_MAX; t++) {
        LobbyMsg q; memset(&q, 0, sizeof q); q.type = (uint8_t)t;
        if (t == LOBBY_RELAY || t == LOBBY_GAME) q.u.relay.payload = pay;
        size_t pn = lobby_pack(&q, buf);
        CHECK(pn > LOBBY_HDR_SIZE);
        if (pn > LOBBY_HDR_SIZE) CHECK(lobby_unpack(buf, pn - 1, &o) == -1);
    }

    /* payload_len > LOBBY_MAX_PAYLOAD refused by pack */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_RELAY; m.u.relay.payload = pay;
    m.u.relay.payload_len = LOBBY_MAX_PAYLOAD + 1;
    CHECK(lobby_pack(&m, buf) == 0);
    /* ... and by unpack, if a hostile peer claims it on the wire */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_RELAY; m.u.relay.payload_len = 5; m.u.relay.payload = pay;
    n = lobby_pack(&m, buf);
    buf[LOBBY_HDR_SIZE + 4] = 0xFF; buf[LOBBY_HDR_SIZE + 5] = 0xFF;   /* len = 65535 */
    CHECK(lobby_unpack(buf, n, &o) == -1);

    /* ---- anti-amplification: responses smaller than padded requests ---- */
    memset(&m, 0, sizeof m); m.type = LOBBY_HOST_RESP; m.u.host_resp.session_id = 1;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);
    memset(&m, 0, sizeof m); m.type = LOBBY_JOIN_RESP;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);
    memset(&m, 0, sizeof m); m.type = LOBBY_REJECT; m.u.reject.reason = LOBBY_REJ_FULL;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);
    memset(&m, 0, sizeof m); m.type = LOBBY_PEER_INTRO;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);
    memset(&m, 0, sizeof m); m.type = LOBBY_START;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);
    memset(&m, 0, sizeof m); m.type = LOBBY_PAIR_ROUTE;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);
    memset(&m, 0, sizeof m); m.type = LOBBY_PUNCH;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);

    /* ---- lobby code: roundtrip, format, crc, Crockford aliases ---- */
    char code[LOBBY_CODE_LEN + 1];
    lobby_code_encode(ep(0x08080404, 40064), 0xA31F, code);
    CHECK(strlen(code) == LOBBY_CODE_LEN && code[5] == '-' && code[11] == '-');
    LobbyEndpoint srv; uint16_t sid;
    CHECK(lobby_code_decode(code, &srv, &sid) == 0);
    CHECK(srv.ip == 0x08080404 && srv.port == 40064 && sid == 0xA31F);
    code[1] = (code[1] == 'A') ? 'B' : 'A';            /* corrupt one char */
    CHECK(lobby_code_decode(code, &srv, &sid) == -1);  /* crc catches it */
    lobby_code_encode(ep(0x08080404, 40064), 0xA31F, code);
    for (char* p = code; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32); /* lowercase */
    CHECK(lobby_code_decode(code, &srv, &sid) == 0);   /* aliases accepted */

    /* alias chars decode to their canonical values */
    {
        char c2[LOBBY_CODE_LEN + 1];
        lobby_code_encode(ep(0x7F000001, 1), 0, c2);
        LobbyEndpoint s2; uint16_t d2;
        CHECK(lobby_code_decode(c2, &s2, &d2) == 0);
        CHECK(s2.ip == 0x7F000001 && s2.port == 1 && d2 == 0);
        for (char* p = c2; *p; p++) {                  /* O->0, I/L->1 */
            if (*p == '0') *p = 'O';
            else if (*p == '1') *p = 'L';
        }
        CHECK(lobby_code_decode(c2, &s2, &d2) == 0);
        CHECK(s2.ip == 0x7F000001 && s2.port == 1 && d2 == 0);
    }

    /* bad formats */
    CHECK(lobby_code_decode(NULL, &srv, &sid) == -1);
    CHECK(lobby_code_decode("", &srv, &sid) == -1);
    CHECK(lobby_code_decode("ABCDE-ABCDE-ABCD", &srv, &sid) == -1);        /* short */
    CHECK(lobby_code_decode("ABCDE-ABCDE-ABCDEF", &srv, &sid) == -1);      /* long */
    CHECK(lobby_code_decode("ABCDE-ABCDE-ABCD!", &srv, &sid) == -1);       /* bad char */
    CHECK(lobby_code_decode("ABCDE-ABCDE-ABCDU", &srv, &sid) == -1);       /* U excluded */

    /* full-range roundtrip over a sweep of values */
    {
        uint32_t rr = 0xACE1u;
        for (int i = 0; i < 2000; i++) {
            rr ^= rr << 13; rr ^= rr >> 17; rr ^= rr << 5;
            uint32_t ip = rr;
            rr ^= rr << 13; rr ^= rr >> 17; rr ^= rr << 5;
            uint16_t port = (uint16_t)rr;
            uint16_t s    = (uint16_t)(rr >> 16);
            char c3[LOBBY_CODE_LEN + 1];
            lobby_code_encode(ep(ip, port), s, c3);
            LobbyEndpoint g; uint16_t gs;
            CHECK(lobby_code_decode(c3, &g, &gs) == 0);
            CHECK(g.ip == ip && g.port == port && gs == s);
        }
    }

    /* ---- fuzz: unpack never crashes, always 0 or -1 ---- */
    uint32_t r = 0x12345678;
    for (int i = 0; i < 10000; i++) {
        uint8_t fz[64]; int len = (int)(r % 64);
        for (int j = 0; j < len; j++) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; fz[j] = (uint8_t)r; }
        int rc = lobby_unpack(fz, (size_t)len, &o);
        CHECK(rc == 0 || rc == -1);
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    }
    /* fuzz again with a valid header so the per-type body paths get hit */
    for (int i = 0; i < 10000; i++) {
        uint8_t fz[64];
        memset(fz, 0, sizeof fz);
        fz[0] = 0x56; fz[1] = 0x52; fz[2] = 0x33; fz[3] = 0x41;   /* LOBBY_MAGIC LE */
        fz[4] = LOBBY_PROTO_VER;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        fz[5] = (uint8_t)(1 + (r % (uint32_t)(LOBBY_MSG_MAX - 1)));
        size_t len = LOBBY_HDR_SIZE + (size_t)(r % (uint32_t)(64 - LOBBY_HDR_SIZE));
        for (size_t j = LOBBY_HDR_SIZE; j < len; j++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5; fz[j] = (uint8_t)r;
        }
        int rc = lobby_unpack(fz, len, &o);
        CHECK(rc == 0 || rc == -1);
        if (rc == 0 && (o.type == LOBBY_RELAY || o.type == LOBBY_GAME)) {
            /* a successful unpack must have kept the payload inside the buffer */
            CHECK((size_t)(o.u.relay.payload - fz) + o.u.relay.payload_len <= len);
        }
    }

    /* ---- net_version: composition is exactly what the spec says, and cached ---- */
    uint32_t smh = arena_scripted_match_hash();
    uint32_t v[4] = { LOBBY_PROTO_VER, TUNE_VERSION, (uint32_t)sizeof(ArenaState), smh };
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)v;
    for (size_t i = 0; i < sizeof v; i++) { h ^= p[i]; h *= 16777619u; }
    CHECK(arena_net_version() == h);
    CHECK(arena_net_version() == h);                   /* second call = cache */
    CHECK(arena_net_version() != 0);

    if (fails) { printf("test_lobby_proto: %d FAILED\n", fails); return 1; }
    printf("test_lobby_proto: all passed\n");
    return 0;
}
