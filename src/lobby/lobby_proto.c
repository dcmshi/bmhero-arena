/* A3 lobby/rendezvous wire codec. See lobby_proto.h for the contract.
 *
 * Two rules hold this file together:
 *   1. Every multi-byte field is written and read BYTE BY BYTE, little-endian.
 *      No struct overlays, no unaligned dereferences — the wire layout is the
 *      same on every target regardless of alignment rules or endianness.
 *   2. lobby_unpack() is the only code that ever sees hostile bytes. It checks
 *      the exact minimum body size for the type BEFORE reading any of it, so a
 *      truncated or lying packet returns -1 instead of reading past `len`. */
#include <string.h>
#include "lobby_proto.h"
#include "../arena/arena_script.h"
#include "../arena/arena_tuning.h"

/* --- LE writers/readers (no unaligned deref: byte-by-byte) --- */
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
                                           p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                                              | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr_ep(uint8_t* p, LobbyEndpoint e) { wr32(p, e.ip); wr16(p + 4, e.port); }
static LobbyEndpoint rd_ep(const uint8_t* p) { LobbyEndpoint e = { rd32(p), rd16(p + 4) }; return e; }

size_t lobby_pack(const LobbyMsg* m, uint8_t out[LOBBY_MAX_PACKET]) {
    if (!m || !out || m->type == 0 || m->type >= LOBBY_MSG_MAX) return 0;
    wr32(out, LOBBY_MAGIC);
    out[4] = LOBBY_PROTO_VER;
    out[5] = m->type;
    uint8_t* b = out + LOBBY_HDR_SIZE;
    size_t n = LOBBY_HDR_SIZE;
    switch (m->type) {
    case LOBBY_HOST_REQ:
        wr32(b, m->u.host_req.net_version); b[4] = m->u.host_req.num_players;
        wr_ep(b + 5, m->u.host_req.private_ep);
        memset(out + LOBBY_HDR_SIZE + 11, 0, LOBBY_REQ_PAD_SIZE - (LOBBY_HDR_SIZE + 11));
        n = LOBBY_REQ_PAD_SIZE; break;
    case LOBBY_HOST_RESP:
        wr16(b, m->u.host_resp.session_id);
        n += 2; break;
    case LOBBY_JOIN_REQ:
        wr16(b, m->u.join_req.session_id); wr32(b + 2, m->u.join_req.net_version);
        wr_ep(b + 6, m->u.join_req.private_ep);
        memset(out + LOBBY_HDR_SIZE + 12, 0, LOBBY_REQ_PAD_SIZE - (LOBBY_HDR_SIZE + 12));
        n = LOBBY_REQ_PAD_SIZE; break;
    case LOBBY_JOIN_RESP:
        b[0] = m->u.join_resp.slot; b[1] = m->u.join_resp.num_players;
        n += 2; break;
    case LOBBY_REJECT:
        b[0] = m->u.reject.reason;
        n += 1; break;
    case LOBBY_PEER_INTRO:
        b[0] = m->u.peer_intro.slot;
        wr_ep(b + 1, m->u.peer_intro.public_ep);
        wr_ep(b + 7, m->u.peer_intro.private_ep);
        n += 13; break;
    case LOBBY_PUNCH: case LOBBY_PUNCH_ACK:
        wr16(b, m->u.punch.session_id); b[2] = m->u.punch.from_slot;
        wr32(b + 3, m->u.punch.nonce);
        n += 7; break;
    case LOBBY_PUNCH_REPORT:
        wr16(b, m->u.punch_report.session_id); b[2] = m->u.punch_report.slot_a;
        b[3] = m->u.punch_report.slot_b; b[4] = m->u.punch_report.ok;
        n += 5; break;
    case LOBBY_PAIR_ROUTE:
        b[0] = m->u.pair_route.slot_a; b[1] = m->u.pair_route.slot_b;
        b[2] = m->u.pair_route.route;
        n += 3; break;
    case LOBBY_RELAY: case LOBBY_GAME:
        if (m->u.relay.payload_len > LOBBY_MAX_PAYLOAD) return 0;
        if (m->u.relay.payload_len && !m->u.relay.payload) return 0;
        wr16(b, m->u.relay.session_id); b[2] = m->u.relay.from_slot;
        b[3] = m->u.relay.to_slot; wr16(b + 4, m->u.relay.payload_len);
        if (m->u.relay.payload_len) memcpy(b + 6, m->u.relay.payload, m->u.relay.payload_len);
        n += 6 + m->u.relay.payload_len; break;
    case LOBBY_START:
        wr32(b, m->u.start.seed); b[4] = m->u.start.arena_id;
        b[5] = m->u.start.num_players; b[6] = m->u.start.input_delay;
        n += 7; break;
    case LOBBY_START_ACK: case LOBBY_KEEPALIVE:
        wr16(b, m->u.start_ack.session_id); b[2] = m->u.start_ack.slot;
        n += 3; break;
    default:
        return 0;
    }
    return n;
}

int lobby_unpack(const uint8_t* buf, size_t len, LobbyMsg* out) {
    if (!buf || !out || len < LOBBY_HDR_SIZE) return -1;
    if (rd32(buf) != LOBBY_MAGIC || buf[4] != LOBBY_PROTO_VER) return -1;
    uint8_t t = buf[5];
    if (t == 0 || t >= LOBBY_MSG_MAX) return -1;
    memset(out, 0, sizeof *out);
    out->type = t;
    const uint8_t* b = buf + LOBBY_HDR_SIZE;
    size_t body = len - LOBBY_HDR_SIZE;
    switch (t) {
    case LOBBY_HOST_REQ:
        /* the padding is mandatory on the wire: a response must never be able
         * to be larger than the request that triggered it */
        if (len < LOBBY_REQ_PAD_SIZE) return -1;
        out->u.host_req.net_version = rd32(b);
        out->u.host_req.num_players = b[4];
        out->u.host_req.private_ep  = rd_ep(b + 5);
        return 0;
    case LOBBY_HOST_RESP:
        if (body < 2) return -1;
        out->u.host_resp.session_id = rd16(b);
        return 0;
    case LOBBY_JOIN_REQ:
        if (len < LOBBY_REQ_PAD_SIZE) return -1;
        out->u.join_req.session_id  = rd16(b);
        out->u.join_req.net_version = rd32(b + 2);
        out->u.join_req.private_ep  = rd_ep(b + 6);
        return 0;
    case LOBBY_JOIN_RESP:
        if (body < 2) return -1;
        out->u.join_resp.slot        = b[0];
        out->u.join_resp.num_players = b[1];
        return 0;
    case LOBBY_REJECT:
        if (body < 1) return -1;
        out->u.reject.reason = b[0];
        return 0;
    case LOBBY_PEER_INTRO:
        if (body < 13) return -1;
        out->u.peer_intro.slot       = b[0];
        out->u.peer_intro.public_ep  = rd_ep(b + 1);
        out->u.peer_intro.private_ep = rd_ep(b + 7);
        return 0;
    case LOBBY_PUNCH: case LOBBY_PUNCH_ACK:
        if (body < 7) return -1;
        out->u.punch.session_id = rd16(b);
        out->u.punch.from_slot  = b[2];
        out->u.punch.nonce      = rd32(b + 3);
        return 0;
    case LOBBY_PUNCH_REPORT:
        if (body < 5) return -1;
        out->u.punch_report.session_id = rd16(b);
        out->u.punch_report.slot_a     = b[2];
        out->u.punch_report.slot_b     = b[3];
        out->u.punch_report.ok         = b[4];
        return 0;
    case LOBBY_PAIR_ROUTE:
        if (body < 3) return -1;
        out->u.pair_route.slot_a = b[0];
        out->u.pair_route.slot_b = b[1];
        out->u.pair_route.route  = b[2];
        return 0;
    case LOBBY_RELAY: case LOBBY_GAME: {
        if (body < 6) return -1;
        uint16_t pl = rd16(b + 4);
        if (pl > LOBBY_MAX_PAYLOAD || body < (size_t)(6 + pl)) return -1;
        out->u.relay.session_id = rd16(b); out->u.relay.from_slot = b[2];
        out->u.relay.to_slot = b[3]; out->u.relay.payload_len = pl;
        out->u.relay.payload = b + 6;
        return 0;
    }
    case LOBBY_START:
        if (body < 7) return -1;
        out->u.start.seed        = rd32(b);
        out->u.start.arena_id    = b[4];
        out->u.start.num_players = b[5];
        out->u.start.input_delay = b[6];
        return 0;
    case LOBBY_START_ACK: case LOBBY_KEEPALIVE:
        if (body < 3) return -1;
        out->u.start_ack.session_id = rd16(b);
        out->u.start_ack.slot       = b[2];
        return 0;
    default:
        break;
    }
    return -1;
}

/* --- lobby code: base32-Crockford over {ip4 BE, port BE, session BE, crc8} --- */
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static uint8_t crc8(const uint8_t* p, size_t n) {      /* CRC-8, poly 0x07 */
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1));
    }
    return c;
}
void lobby_code_encode(LobbyEndpoint server, uint16_t session_id,
                       char out[LOBBY_CODE_LEN + 1]) {
    uint8_t d[9];
    d[0] = (uint8_t)(server.ip >> 24); d[1] = (uint8_t)(server.ip >> 16);
    d[2] = (uint8_t)(server.ip >> 8);  d[3] = (uint8_t)server.ip;
    d[4] = (uint8_t)(server.port >> 8); d[5] = (uint8_t)server.port;
    d[6] = (uint8_t)(session_id >> 8);  d[7] = (uint8_t)session_id;
    d[8] = crc8(d, 8);
    /* 72 bits -> 15 symbols, MSB-first, 3 zero pad bits at the end */
    int oi = 0;
    for (int s = 0; s < 15; s++) {
        int bit = s * 5, sym = 0;
        for (int k = 0; k < 5; k++) {
            int idx = bit + k;
            int v = (idx < 72) ? ((d[idx >> 3] >> (7 - (idx & 7))) & 1) : 0;
            sym = (sym << 1) | v;
        }
        if (s == 5 || s == 10) out[oi++] = '-';
        out[oi++] = B32[sym];
    }
    out[oi] = '\0';
}

/* Crockford symbol value, case-insensitive, with the human aliases:
 * O/o -> 0 and I/i/L/l -> 1. U is deliberately not a symbol. -1 = not one. */
static int b32val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    char u = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
    if (u == 'O') return 0;
    if (u == 'I' || u == 'L') return 1;
    for (int i = 10; i < 32; i++) if (B32[i] == u) return i;
    return -1;
}

int lobby_code_decode(const char* str, LobbyEndpoint* server, uint16_t* session_id) {
    if (!str || !server || !session_id) return -1;
    int sym[15], ns = 0;
    for (const char* p = str; *p; p++) {
        if (*p == '-') continue;                 /* grouping is cosmetic */
        if (ns >= 15) return -1;                 /* too many symbols */
        int v = b32val(*p);
        if (v < 0) return -1;
        sym[ns++] = v;
    }
    if (ns != 15) return -1;
    uint8_t d[9];
    memset(d, 0, sizeof d);
    for (int s = 0; s < 15; s++) {
        for (int k = 0; k < 5; k++) {
            int idx = s * 5 + k;
            int bit = (sym[s] >> (4 - k)) & 1;
            if (idx < 72) {
                if (bit) d[idx >> 3] |= (uint8_t)(1u << (7 - (idx & 7)));
            } else if (bit) {
                return -1;   /* the 3 trailing pad bits are zero by construction;
                              * nonzero means this code was not one of ours, and
                              * crc8 cannot see them (it covers d[0..7] only) */
            }
        }
    }
    if (crc8(d, 8) != d[8]) return -1;
    server->ip   = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16)
                 | ((uint32_t)d[2] << 8)  | (uint32_t)d[3];
    server->port = (uint16_t)(((uint16_t)d[4] << 8) | d[5]);
    *session_id  = (uint16_t)(((uint16_t)d[6] << 8) | d[7]);
    return 0;
}

uint32_t arena_net_version(void) {
    static uint32_t cached = 0;
    if (cached) return cached;
    uint32_t v[4] = { LOBBY_PROTO_VER, TUNE_VERSION,
                      (uint32_t)sizeof(ArenaState), arena_scripted_match_hash() };
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)v;
    for (size_t i = 0; i < sizeof v; i++) { h ^= p[i]; h *= 16777619u; }
    cached = h ? h : 1u;
    return cached;
}
