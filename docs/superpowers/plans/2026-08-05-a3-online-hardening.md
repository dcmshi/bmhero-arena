# A3 Online Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Real online 4P arena play: a deploy-anywhere rendezvous/relay binary, lobby codes, NAT hole-punching with server-arbitrated relay fallback, a custom GekkoNet adapter, CI-grade simulated-WAN soak, and hard-stop desync capture with offline localization.

**Architecture:** One UDP socket per client does everything (rendezvous registration, punching, game traffic, relay) via a custom `GekkoNetAdapter`; the rendezvous server is a pure, socketless session-table library (`rendezvous.c`) wrapped by a thin socket loop (`rendezvous_main.c`), so almost all logic is unit-testable in-process. Every datagram on the wire is a `lobby_proto` packet (game traffic rides in `LOBBY_GAME`/`LOBBY_RELAY` wrappers), so parsing is uniform and stray traffic is droppable.

**Tech Stack:** C11, GekkoNet (already vendored via FetchContent), BSD sockets/winsock, CMake + ctest, PowerShell tools, bash test orchestration (pattern: `tests/run_p2p_test.sh`).

**Spec:** `docs/superpowers/specs/2026-08-05-a3-online-hardening-design.md`

## Global Constraints

- **The sim is untouched.** `src/arena/arena_sim.c`, `arena_state.h`, `arena_tuning.h`, `arena_math.h`, `arena_geom.h`, `arena_sintab.h` must not change. Pinned hash `fbdb0d08` @ `TUNE_VERSION 21` must survive `tools\gate.ps1` after every task. The one new file allowed under `src/arena/` is `arena_script.h` (a header-only extraction whose correctness the gate's `[hash]` check proves).
- No floats anywhere under `src/arena/` (including `arena_script.h`). Netplay/lobby layers may use floats only for logging/stats, never for anything a peer must agree on.
- All new C compiles clean under `-std=c11 -Wall -Wextra -Werror` (the gate's exact flags).
- No new external dependencies. Sockets via a small in-repo wrapper (`udp_socket.h`).
- Heap allocation in netplay/lobby: at create/init time only — except the GekkoNet receive path, whose contract REQUIRES per-packet mallocs (GekkoNet calls `free_data(res->addr.data)`, `free_data(res->data)`, `free_data(res)` — three separate heap blocks per result; `backend.cpp:158-160`).
- `LobbyEndpoint` fields are HOST byte order in memory; conversion happens only in `udp_socket.c` and in the lobby-code bytes (big-endian there).
- Wire format: little-endian, magic `0x41335256` ("A3RV" as LE u32), `LOBBY_PROTO_VER 1`, max packet 1200B.
- Fixed protocol constants (from the spec — do not improvise): default server port **40064**; request retry **500ms × 10**; punch probes **every 250ms for up to 3s**, alternating private/public; bootstrap overall timeout **30s**; rate limit **20 pkt/s, burst 40**, non-relay only; session expiry: lobby idle **10 min**, started-with-no-relay-pairs **30s**, relay-silent **60s**; START refan every **500ms**.
- Every new pure-C (socketless) test suite joins BOTH ctest and `tools/gate.ps1`; socket/process tests join ctest only.
- Windows + POSIX. Link `ws2_32` on WIN32 for anything using sockets (existing pattern: `CMakeLists.txt:87-89`).
- Commit per task, message prefix `net:` (protocol/server) or `netplay:` (client/session), `tools:` for tools, `test:` allowed for test-only commits.
- Local build/test commands: `cmake --build build --parallel` then `ctest --test-dir build -R <name> --output-on-failure`; the gate is `tools\gate.ps1` (PowerShell, from repo root).

---

### Task 1: Wire codec — `lobby_proto` + lobby codes + `arena_net_version()`

**Files:**
- Create: `src/arena/arena_script.h`
- Modify: `tools/arena_hash.c` (call the extracted function; behavior identical)
- Create: `src/lobby/lobby_proto.h`, `src/lobby/lobby_proto.c`
- Create: `tests/test_lobby_proto.c`
- Modify: `CMakeLists.txt` (new `arena_lobby` lib + test target), `tools/gate.ps1` (new suite)

**Interfaces:**
- Consumes: `arena_state.h` (`ArenaState`, `arena_hash`, `arena_input_pack`), `arena_sim.h` (`arena_init`, `arena_tick`), `arena_tuning.h` (`TUNE_VERSION`).
- Produces (everything later tasks call):
  - `uint32_t arena_scripted_match_hash(void)` (arena_script.h)
  - `size_t lobby_pack(const LobbyMsg* m, uint8_t out[LOBBY_MAX_PACKET])` — bytes written, 0 = invalid
  - `int lobby_unpack(const uint8_t* buf, size_t len, LobbyMsg* out)` — 0 ok, −1 malformed; `relay.payload` points into `buf`
  - `void lobby_code_encode(LobbyEndpoint server, uint16_t session_id, char out[LOBBY_CODE_LEN + 1])`
  - `int lobby_code_decode(const char* str, LobbyEndpoint* server, uint16_t* session_id)` — 0 ok, −1 bad format/crc
  - `uint32_t arena_net_version(void)` — cached after first call
  - types `LobbyEndpoint`, `LobbyMsg`, `LobbyMsgType`, `LobbyRejectReason`, `LobbyRoute`, `NetRouteKind`, `NetRoute`

- [ ] **Step 1: Extract the scripted match into `src/arena/arena_script.h`**

The loop comes verbatim from `tools/arena_hash.c` (which was itself lifted verbatim from CI). The gate's `[hash]` check is the guard: if this extraction changes behavior, the pin `fbdb0d08` breaks and the gate goes red.

```c
/* The scripted 4P match behind the pinned hash — extracted from
 * tools/arena_hash.c (2026-08-05) so the netplay version handshake can
 * replay it. MUST stay bit-identical to the pin's generator: the gate's
 * [hash] check (fbdb0d08 @ TUNE_VERSION 21) enforces that on every run.
 * Header-only, no floats, reads nothing outside the sim API. */
#ifndef ARENA_SCRIPT_H
#define ARENA_SCRIPT_H

#include "arena_sim.h"

static inline uint32_t arena_scripted_match_hash(void) {
    ArenaState s; ArenaInput in[4];
    arena_init(&s, 0, 4, 0xB0BB1E5);
    uint32_t r = 0xC0FFEE01;
    for (uint32_t t = 0; t < 5400; t++) {
        for (int i = 0; i < 4; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            int sx = (int)(r & 63) - 32;        if (sx < -31) sx = -31;
            int sy = (int)((r >> 6) & 63) - 32; if (sy < -31) sy = -31;
            int set  = ((t + i * 53) % 137) == 0;
            /* cast comment preserved from arena_hash.c: silences
             * -Wsign-compare; same values, same hash. */
            int bomb = ((t + i * 37) % (90 + i * 80)) < (uint32_t)(30 + i * 40);
            in[i] = arena_input_pack(sx, sy, ((r >> 12) & 31) == 0, bomb, set);
        }
        arena_tick(&s, in);
    }
    return arena_hash(&s);
}

#endif
```

`tools/arena_hash.c` becomes:

```c
/* Scripted-match hash generator — the cross-platform determinism pin.
 * The match itself lives in src/arena/arena_script.h (shared with the
 * netplay version handshake). Any behavior change breaks the pinned hash:
 * the gate proves this file and the header still agree with the pin. */
#include <stdio.h>
#include "../src/arena/arena_script.h"

int main(void) {
    printf("%08x\n", arena_scripted_match_hash());
    return 0;
}
```

- [ ] **Step 2: Run the gate — the pin must NOT move**

Run: `tools\gate.ps1`
Expected: `[hash] fbdb0d08 matches pin (TUNE_VERSION 21)` and GATE GREEN. If the hash moved, the extraction is wrong — fix it; do NOT repin.

- [ ] **Step 3: Write `src/lobby/lobby_proto.h`**

```c
/* A3 lobby/rendezvous wire format — shared by server, client, and adapter.
 * Pure codec: no sockets, no allocation; unpack is bounds-checked and safe
 * on hostile input. All multi-byte wire fields little-endian. LobbyEndpoint
 * is HOST byte order in memory (converted at the socket boundary and in the
 * lobby-code bytes, which are big-endian). */
#ifndef LOBBY_PROTO_H
#define LOBBY_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define LOBBY_MAGIC        0x41335256u  /* "VR3A" in LE bytes = "A3RV" tag */
#define LOBBY_PROTO_VER    1
#define LOBBY_HDR_SIZE     6            /* u32 magic + u8 ver + u8 type */
#define LOBBY_MAX_PACKET   1200
#define LOBBY_MAX_PAYLOAD  1024         /* RELAY / GAME payload cap */
#define LOBBY_DEFAULT_PORT 40064
#define LOBBY_REQ_PAD_SIZE 32           /* HOST_REQ/JOIN_REQ padded size:
                                           every response must be smaller
                                           (anti-amplification) */

typedef struct { uint32_t ip; uint16_t port; } LobbyEndpoint;

typedef enum {
    LOBBY_HOST_REQ = 1, LOBBY_HOST_RESP, LOBBY_JOIN_REQ, LOBBY_JOIN_RESP,
    LOBBY_REJECT, LOBBY_PEER_INTRO, LOBBY_PUNCH, LOBBY_PUNCH_ACK,
    LOBBY_PUNCH_REPORT, LOBBY_PAIR_ROUTE, LOBBY_RELAY, LOBBY_GAME,
    LOBBY_START, LOBBY_START_ACK, LOBBY_KEEPALIVE,
    LOBBY_MSG_MAX
} LobbyMsgType;

typedef enum { LOBBY_REJ_BAD_SESSION = 1, LOBBY_REJ_FULL,
               LOBBY_REJ_VERSION_MISMATCH, LOBBY_REJ_EXPIRED } LobbyRejectReason;

typedef enum { LOBBY_ROUTE_DIRECT = 0, LOBBY_ROUTE_RELAY = 1 } LobbyRoute;

/* Client-side route to one peer (filled by the lobby client, consumed by the
 * net adapter). Lives here so lobby_client.h need not include gekkonet.h. */
typedef enum { NET_ROUTE_NONE = 0, NET_ROUTE_DIRECT, NET_ROUTE_RELAY } NetRouteKind;
typedef struct { uint8_t kind; LobbyEndpoint ep; } NetRoute; /* ep unused for RELAY */

typedef struct {
    uint8_t type;                       /* LobbyMsgType */
    union {
        struct { uint32_t net_version; uint8_t num_players;
                 LobbyEndpoint private_ep; } host_req;
        struct { uint16_t session_id; } host_resp;
        struct { uint16_t session_id; uint32_t net_version;
                 LobbyEndpoint private_ep; } join_req;
        struct { uint8_t slot; uint8_t num_players; } join_resp;
        struct { uint8_t reason; } reject;
        struct { uint8_t slot; LobbyEndpoint public_ep, private_ep; } peer_intro;
        struct { uint16_t session_id; uint8_t from_slot; uint32_t nonce; } punch;
                                        /* PUNCH and PUNCH_ACK (ack echoes nonce) */
        struct { uint16_t session_id; uint8_t slot_a, slot_b, ok; } punch_report;
        struct { uint8_t slot_a, slot_b, route; } pair_route;  /* LobbyRoute */
        struct { uint16_t session_id; uint8_t from_slot, to_slot;
                 uint16_t payload_len; const uint8_t* payload; } relay;
                                        /* RELAY (via server) and GAME (direct) */
        struct { uint32_t seed; uint8_t arena_id, num_players,
                 input_delay; } start;
        struct { uint16_t session_id; uint8_t slot; } start_ack;
                                        /* START_ACK and KEEPALIVE */
    } u;
} LobbyMsg;

size_t lobby_pack(const LobbyMsg* m, uint8_t out[LOBBY_MAX_PACKET]);
int    lobby_unpack(const uint8_t* buf, size_t len, LobbyMsg* out);

#define LOBBY_CODE_LEN 17               /* "XXXXX-XXXXX-XXXXX" */
void lobby_code_encode(LobbyEndpoint server, uint16_t session_id,
                       char out[LOBBY_CODE_LEN + 1]);
int  lobby_code_decode(const char* str, LobbyEndpoint* server,
                       uint16_t* session_id);

/* fnv1a over {LOBBY_PROTO_VER, TUNE_VERSION, sizeof(ArenaState),
 * arena_scripted_match_hash()}. First call replays the 5400-tick scripted
 * match (~0.3s); result is cached. */
uint32_t arena_net_version(void);

#endif
```

- [ ] **Step 4: Write the failing test `tests/test_lobby_proto.c`**

Follow the house test style (plain `main`, counters, prints, nonzero exit on failure — see `tests/test_movement.c` for the register). Cover, with real asserts:

```c
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

    /* roundtrip every type (spot-check every field of each) */
    memset(&m, 0, sizeof m);
    m.type = LOBBY_HOST_REQ;
    m.u.host_req.net_version = 0xDEADBEEF;
    m.u.host_req.num_players = 4;
    m.u.host_req.private_ep  = ep(0xC0A80102, 7777);
    size_t n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_REQ_PAD_SIZE);                    /* padded */
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.type == LOBBY_HOST_REQ);
    CHECK(o.u.host_req.net_version == 0xDEADBEEF);
    CHECK(o.u.host_req.num_players == 4);
    CHECK(o.u.host_req.private_ep.ip == 0xC0A80102 && o.u.host_req.private_ep.port == 7777);
    /* ... same pattern for the other 14 types; RELAY/GAME with a payload:  */
    static const uint8_t pay[5] = { 1, 2, 3, 4, 5 };
    memset(&m, 0, sizeof m);
    m.type = LOBBY_RELAY;
    m.u.relay.session_id = 0xBEEF; m.u.relay.from_slot = 1; m.u.relay.to_slot = 3;
    m.u.relay.payload_len = 5; m.u.relay.payload = pay;
    n = lobby_pack(&m, buf);
    CHECK(n == LOBBY_HDR_SIZE + 6 + 5);
    CHECK(lobby_unpack(buf, n, &o) == 0);
    CHECK(o.u.relay.payload_len == 5 && memcmp(o.u.relay.payload, pay, 5) == 0);

    /* malformed input never succeeds */
    CHECK(lobby_unpack(buf, 3, &o) == -1);             /* short */
    buf[0] ^= 0xFF; CHECK(lobby_unpack(buf, n, &o) == -1); buf[0] ^= 0xFF; /* magic */
    buf[4] = 99;    CHECK(lobby_unpack(buf, n, &o) == -1); buf[4] = LOBBY_PROTO_VER; /* ver */
    buf[5] = 0;     CHECK(lobby_unpack(buf, n, &o) == -1);  /* type 0 */
    buf[5] = LOBBY_RELAY;
    CHECK(lobby_unpack(buf, n - 3, &o) == -1);         /* truncated payload */

    /* payload_len > LOBBY_MAX_PAYLOAD refused by pack */
    m.u.relay.payload_len = LOBBY_MAX_PAYLOAD + 1;
    CHECK(lobby_pack(&m, buf) == 0);

    /* anti-amplification: responses smaller than padded requests */
    memset(&m, 0, sizeof m); m.type = LOBBY_HOST_RESP; m.u.host_resp.session_id = 1;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);
    memset(&m, 0, sizeof m); m.type = LOBBY_JOIN_RESP;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);
    memset(&m, 0, sizeof m); m.type = LOBBY_REJECT; m.u.reject.reason = LOBBY_REJ_FULL;
    CHECK(lobby_pack(&m, buf) < LOBBY_REQ_PAD_SIZE);

    /* lobby code: roundtrip, format, crc, Crockford aliases */
    char code[LOBBY_CODE_LEN + 1];
    lobby_code_encode(ep(0x08080404, 40064), 0xA31F, code);
    CHECK(strlen(code) == LOBBY_CODE_LEN && code[5] == '-' && code[11] == '-');
    LobbyEndpoint srv; uint16_t sid;
    CHECK(lobby_code_decode(code, &srv, &sid) == 0);
    CHECK(srv.ip == 0x08080404 && srv.port == 40064 && sid == 0xA31F);
    code[1] = (code[1] == 'A') ? 'B' : 'A';            /* corrupt one char */
    CHECK(lobby_code_decode(code, &srv, &sid) == -1);  /* crc catches it */
    lobby_code_encode(ep(0x08080404, 40064), 0xA31F, code);
    for (char* p = code; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; /* lowercase */
    CHECK(lobby_code_decode(code, &srv, &sid) == 0);   /* aliases accepted */

    /* fuzz: unpack never crashes, always 0 or -1 */
    uint32_t r = 0x12345678;
    for (int i = 0; i < 10000; i++) {
        uint8_t fz[64]; int len = (int)(r % 64);
        for (int j = 0; j < len; j++) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; fz[j] = (uint8_t)r; }
        int rc = lobby_unpack(fz, (size_t)len, &o);
        CHECK(rc == 0 || rc == -1);
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    }

    /* net_version: composition is exactly what the spec says, and cached */
    uint32_t smh = arena_scripted_match_hash();
    uint32_t v[4] = { LOBBY_PROTO_VER, TUNE_VERSION, (uint32_t)sizeof(ArenaState), smh };
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)v;
    for (size_t i = 0; i < sizeof v; i++) { h ^= p[i]; h *= 16777619u; }
    CHECK(arena_net_version() == h);
    CHECK(arena_net_version() == h);                   /* second call = cache */

    if (fails) { printf("test_lobby_proto: %d FAILED\n", fails); return 1; }
    printf("test_lobby_proto: all passed\n");
    return 0;
}
```

- [ ] **Step 5: Add build wiring, run the test, verify it FAILS to build (no lobby_proto.c yet)**

`CMakeLists.txt`, after the `arena_trace` block, before the GekkoNet section:

```cmake
# --- A3 lobby/rendezvous protocol (pure C, no sockets in this lib) ---
add_library(arena_lobby STATIC src/lobby/lobby_proto.c)
target_include_directories(arena_lobby PUBLIC src src/lobby)
target_link_libraries(arena_lobby PUBLIC arena_sim)   # net_version replays the scripted match

add_executable(test_lobby_proto tests/test_lobby_proto.c)
target_link_libraries(test_lobby_proto arena_lobby)
add_test(NAME lobby_proto COMMAND test_lobby_proto)
```

Run: `cmake --build build --parallel 2>&1 | tail -5`
Expected: FAILS — `lobby_proto.c` missing.

- [ ] **Step 6: Implement `src/lobby/lobby_proto.c`**

Structure (complete the mechanical parts following exactly this pattern):

```c
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
    if (!m || m->type <= 0 || m->type >= LOBBY_MSG_MAX) return 0;
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
    case LOBBY_JOIN_REQ:
        wr16(b, m->u.join_req.session_id); wr32(b + 2, m->u.join_req.net_version);
        wr_ep(b + 6, m->u.join_req.private_ep);
        memset(out + LOBBY_HDR_SIZE + 12, 0, LOBBY_REQ_PAD_SIZE - (LOBBY_HDR_SIZE + 12));
        n = LOBBY_REQ_PAD_SIZE; break;
    case LOBBY_RELAY: case LOBBY_GAME:
        if (m->u.relay.payload_len > LOBBY_MAX_PAYLOAD) return 0;
        wr16(b, m->u.relay.session_id); b[2] = m->u.relay.from_slot;
        b[3] = m->u.relay.to_slot; wr16(b + 4, m->u.relay.payload_len);
        memcpy(b + 6, m->u.relay.payload, m->u.relay.payload_len);
        n += 6 + m->u.relay.payload_len; break;
    /* ... HOST_RESP (session u16), JOIN_RESP (slot u8, num u8),
       REJECT (reason u8), PEER_INTRO (slot u8, pub ep 6B, priv ep 6B),
       PUNCH/PUNCH_ACK (session u16, from u8, nonce u32),
       PUNCH_REPORT (session u16, a u8, b u8, ok u8),
       PAIR_ROUTE (a u8, b u8, route u8),
       START (seed u32, arena u8, num u8, delay u8),
       START_ACK/KEEPALIVE (session u16, slot u8) — same LE style ... */
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
        if (len < LOBBY_REQ_PAD_SIZE) return -1;
        out->u.host_req.net_version = rd32(b);
        out->u.host_req.num_players = b[4];
        out->u.host_req.private_ep  = rd_ep(b + 5);
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
    /* ... every other type: check exact minimum body size first, then read.
       A type with body shorter than its layout returns -1, never reads
       past len. ... */
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
/* decode: strip dashes, map chars case-insensitively with Crockford aliases
 * (O/o->0, I/i/L/l->1), reject anything else or wrong length, rebuild the 9
 * bytes, verify crc8. Return -1 on any failure. */
```

`arena_net_version()`:

```c
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
```

- [ ] **Step 7: Build, run test, verify PASS**

Run: `cmake --build build --parallel && ctest --test-dir build -R lobby_proto --output-on-failure`
Expected: PASS.

- [ ] **Step 8: Add the gate suite and run the full gate**

In `tools/gate.ps1` after the `tune_probes` suite line:

```powershell
Run-Suite "lobby_proto"     @($SIM, (Join-Path $root "src\lobby\lobby_proto.c"),
                               (Join-Path $root "tests\test_lobby_proto.c"))
```

Run: `tools\gate.ps1`
Expected: new `[lobby_proto]` line green, `[hash]` still `fbdb0d08`, GATE GREEN.

- [ ] **Step 9: Commit**

```bash
git add src/arena/arena_script.h tools/arena_hash.c src/lobby/ tests/test_lobby_proto.c CMakeLists.txt tools/gate.ps1
git commit -m "net: lobby wire codec, lobby codes, arena_net_version (A3 task 1)"
```

---

### Task 2: Rendezvous core — pure session-table logic

**Files:**
- Create: `src/lobby/rendezvous.h`, `src/lobby/rendezvous.c`
- Create: `tests/test_rendezvous.c`
- Modify: `CMakeLists.txt` (add `rendezvous.c` to `arena_lobby`, new test), `tools/gate.ps1` (new suite)

**Interfaces:**
- Consumes: everything from Task 1.
- Produces:
  - `void rv_init(Rendezvous* rv, uint32_t now_ms, uint32_t rng_seed)`
  - `void rv_handle(Rendezvous* rv, uint32_t now_ms, LobbyEndpoint from, const uint8_t* buf, size_t len, RvEmit emit, void* ctx)`
  - `void rv_tick(Rendezvous* rv, uint32_t now_ms, RvEmit emit, void* ctx)`
  - `int rv_active_sessions(const Rendezvous* rv)`
  - `typedef void (*RvEmit)(void* ctx, LobbyEndpoint to, const uint8_t* buf, size_t len)`

- [ ] **Step 1: Write `src/lobby/rendezvous.h`**

```c
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
} Rendezvous;

void rv_init(Rendezvous* rv, uint32_t now_ms, uint32_t rng_seed);
void rv_handle(Rendezvous* rv, uint32_t now_ms, LobbyEndpoint from,
               const uint8_t* buf, size_t len, RvEmit emit, void* ctx);
void rv_tick(Rendezvous* rv, uint32_t now_ms, RvEmit emit, void* ctx);
int  rv_active_sessions(const Rendezvous* rv);

#endif
```

- [ ] **Step 2: Write the failing test `tests/test_rendezvous.c`**

Same CHECK style as Task 1. Emit-capture harness:

```c
#include <stdio.h>
#include <string.h>
#include "../src/lobby/rendezvous.h"

typedef struct { LobbyEndpoint to; uint8_t buf[LOBBY_MAX_PACKET]; size_t len; } Sent;
static Sent sent[256]; static int nsent;
static void cap(void* ctx, LobbyEndpoint to, const uint8_t* b, size_t l) {
    (void)ctx; if (nsent < 256) { sent[nsent].to = to; memcpy(sent[nsent].buf, b, l); sent[nsent].len = l; nsent++; }
}
static void reset(void) { nsent = 0; }
/* helper: pack msg m, rv_handle it from `from` at time t */
static void send_msg(Rendezvous* rv, uint32_t t, LobbyEndpoint from, const LobbyMsg* m) {
    uint8_t b[LOBBY_MAX_PACKET]; size_t n = lobby_pack(m, b);
    rv_handle(rv, t, from, b, n, cap, NULL);
}
/* helper: find first captured msg of a type sent to `to`; unpack into out; -1 if none */
static int find_sent(uint8_t type, LobbyEndpoint to, LobbyMsg* out);
```

Scenarios (each a function called from `main`, each starting from a fresh `rv_init(&rv, 1000, 42)`):

1. **host_join_flow:** HOST_REQ (the server compares a joiner's version to the HOST's stored version, it never computes its own — use literal `0x1111`) from ep A → exactly one HOST_RESP to A with nonzero session_id; JOIN_REQ (same version) from B, C, D → JOIN_RESPs with slots 1, 2, 3 and `num_players == 4`; each join fans PEER_INTRO of the newcomer to the older members AND intros of all older members to the newcomer (after D joins: `find_sent` shows D received 3 intros with slots 0,1,2). Fifth JOIN → REJECT `LOBBY_REJ_FULL`. JOIN with version `0x2222` → REJECT `LOBBY_REJ_VERSION_MISMATCH`. JOIN with unknown session_id → REJECT `LOBBY_REJ_BAD_SESSION`. JOIN to a session whose phase is already RV_STARTING/RV_RUNNING → REJECT `LOBBY_REJ_EXPIRED` (the lobby is over; a freed/never-existed id is `BAD_SESSION`).
2. **host_retry_idempotent:** two HOST_REQs from the same endpoint → both HOST_RESPs carry the SAME session_id; `rv_active_sessions == 1`. Same for a repeated JOIN_REQ from the same endpoint → same slot re-sent, `joined` unchanged.
3. **routes:** after 2 peers joined (num_players 2): PUNCH_REPORT(0,1,ok=1) from A, then PUNCH_REPORT(0,1,ok=1) from B → PAIR_ROUTE(0,1,DIRECT) emitted to BOTH peers exactly once. Fresh session, reports ok=1 and ok=0 → PAIR_ROUTE RELAY. Only one side reported → no PAIR_ROUTE yet.
4. **start_refan_acks:** host sends START → phase RV_STARTING, START fanned to all peers; `rv_tick` at +600ms refans to non-acked only; START_ACKs from everyone → phase RV_RUNNING, no more refans. START from a non-slot-0 endpoint is ignored.
5. **relay_forwarding:** RELAY{sid, from=1, to=2, payload} from peer 1's registered endpoint → forwarded verbatim (byte-identical buffer) to peer 2's public endpoint; RELAY from a WRONG source endpoint → dropped (nsent unchanged); RELAY with unknown session → dropped; RELAY updates `last_relay_ms`.
6. **expiry:** LOBBY session idle: `rv_tick` at +RV_LOBBY_EXPIRY_MS+1 frees it (subsequent JOIN → BAD_SESSION); RUNNING with no relay routes: freed 30s after `started_ms`; RUNNING with a RELAY route: alive at +59s of relay silence, freed at +61s.
7. **rate_limit:** 100 HOST_REQs from one IP at the same now_ms → at most RV_RATE_BURST responses; RELAY packets are NOT rate limited (send 100 valid relays → 100 forwards).

- [ ] **Step 3: Build wiring; verify the test fails to link**

`CMakeLists.txt`: change `arena_lobby` sources to `src/lobby/lobby_proto.c src/lobby/rendezvous.c`; add:

```cmake
add_executable(test_rendezvous tests/test_rendezvous.c)
target_link_libraries(test_rendezvous arena_lobby)
add_test(NAME rendezvous COMMAND test_rendezvous)
```

Run: `cmake --build build --parallel 2>&1 | tail -5` — expect link failure (no rendezvous.c).

- [ ] **Step 4: Implement `src/lobby/rendezvous.c`**

Implementation notes (the tests above are the contract; key logic):

- `rv_handle`: (1) `lobby_unpack`; drop on −1. (2) If type is not `LOBBY_RELAY`: token-bucket by `from.ip % RV_RATE_BUCKETS` — `tokens_milli += (now - last) * RV_RATE_PER_SEC` capped at `RV_RATE_BURST * 1000`; a packet costs 1000; empty → drop. (3) Dispatch by type.
- Session allocation: xorshift32 the `rng` until a nonzero id not currently in use; table full → emit REJECT{FULL}.
- Idempotency: HOST_REQ whose `from` equals an existing LOBBY session's peer-0 public_ep → re-emit that session's HOST_RESP. JOIN_REQ whose `from` matches an existing peer → re-emit its JOIN_RESP.
- Every valid packet for a session updates `last_activity_ms` and the peer's `last_seen_ms`.
- PUNCH_REPORT: store `punch[a][b] = ok` (only from the endpoint registered for the reporting slot — the reporter is identified by matching `from` to a peer, and must be slot a or b). When `punch[a][b] >= 0 && punch[b][a] >= 0` and `!route_sent[a][b]`: route = both 1 ? DIRECT : RELAY; emit PAIR_ROUTE to ALL joined peers; mark `route_sent` symmetric.
- START: only from peer 0's endpoint; store, `have_start = 1`, phase RV_STARTING, `started_ms = now`, fan immediately.
- `rv_tick`: per session — RV_LOBBY: expire on `now - last_activity_ms > RV_LOBBY_EXPIRY_MS`. RV_STARTING: refan START to non-acked peers every RV_START_REFAN_MS. RV_STARTING/RV_RUNNING: `has_relay` = any `route_sent[a][b] && route[a][b] == LOBBY_ROUTE_RELAY`; if `!has_relay && now - started_ms > RV_STARTED_EXPIRY_MS` → free; if `has_relay && now - last_relay_ms > RV_RELAY_EXPIRY_MS` → free.
- Freeing = `memset(session, 0, sizeof *session)` (phase RV_FREE).
- No allocation anywhere; no I/O other than `emit`.

- [ ] **Step 5: Build, run, verify PASS**

Run: `cmake --build build --parallel && ctest --test-dir build -R rendezvous --output-on-failure`

- [ ] **Step 6: Add gate suite, run gate**

`tools/gate.ps1`:

```powershell
Run-Suite "rendezvous"      @($SIM, (Join-Path $root "src\lobby\lobby_proto.c"),
                               (Join-Path $root "src\lobby\rendezvous.c"),
                               (Join-Path $root "tests\test_rendezvous.c"))
```

Run: `tools\gate.ps1` — expect GATE GREEN with both new suites.

- [ ] **Step 7: Commit**

```bash
git add src/lobby/rendezvous.h src/lobby/rendezvous.c tests/test_rendezvous.c CMakeLists.txt tools/gate.ps1
git commit -m "net: rendezvous session-table core, fully unit-tested (A3 task 2)"
```

---

### Task 3: `udp_socket` wrapper + `arena_rendezvous` binary

**Files:**
- Create: `src/netplay/udp_socket.h`, `src/netplay/udp_socket.c`
- Create: `src/lobby/rendezvous_main.c`
- Create: `tests/test_udp_socket.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 types, Task 2 `rv_*`.
- Produces:
  - `int udp_global_init(void)` — WSAStartup on WIN32, idempotent; 0 ok
  - `int udp_open(UdpSocket* s, uint16_t bind_port)` — 0 ok; port 0 = ephemeral; socket is NONBLOCKING
  - `uint16_t udp_bound_port(const UdpSocket* s)`
  - `int udp_send(UdpSocket* s, LobbyEndpoint to, const void* buf, size_t len)` — 0 ok
  - `int udp_recv(UdpSocket* s, LobbyEndpoint* from, void* buf, size_t cap)` — >0 length, 0 nothing pending, −1 error
  - `int udp_local_endpoint_for(LobbyEndpoint remote, uint16_t bound_port, LobbyEndpoint* out)` — the private-endpoint trick: UDP-connect a throwaway socket to `remote`, `getsockname` the local IP, pair it with `bound_port`
  - `uint32_t udp_now_ms(void)` — monotonic ms (GetTickCount64 / CLOCK_MONOTONIC)
  - `void udp_close(UdpSocket* s)`
  - `typedef struct { intptr_t fd; } UdpSocket;`
  - Binary: `arena_rendezvous [--port N]` (default `LOBBY_DEFAULT_PORT`), logs one line per event to stdout

- [ ] **Step 1: Write the failing test `tests/test_udp_socket.c`**

```c
#include <stdio.h>
#include <string.h>
#include "../src/netplay/udp_socket.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    CHECK(udp_global_init() == 0);
    CHECK(udp_global_init() == 0);                    /* idempotent */
    UdpSocket a, b;
    CHECK(udp_open(&a, 0) == 0);
    CHECK(udp_open(&b, 0) == 0);
    CHECK(udp_bound_port(&a) != 0 && udp_bound_port(&b) != 0);

    LobbyEndpoint to = { 0x7F000001u, udp_bound_port(&b) };   /* 127.0.0.1 */
    CHECK(udp_send(&a, to, "ping", 4) == 0);

    uint8_t buf[64]; LobbyEndpoint from = { 0, 0 };
    int n = 0;
    for (int i = 0; i < 200 && n <= 0; i++) n = udp_recv(&b, &from, buf, sizeof buf);
    CHECK(n == 4 && memcmp(buf, "ping", 4) == 0);
    CHECK(from.port == udp_bound_port(&a));

    CHECK(udp_recv(&b, &from, buf, sizeof buf) == 0); /* nonblocking empty */

    LobbyEndpoint priv;
    CHECK(udp_local_endpoint_for(to, udp_bound_port(&a), &priv) == 0);
    CHECK(priv.port == udp_bound_port(&a) && priv.ip != 0);

    uint32_t t0 = udp_now_ms(); (void)t0;             /* compiles, monotonic-ish */

    udp_close(&a); udp_close(&b);
    if (fails) { printf("test_udp_socket: %d FAILED\n", fails); return 1; }
    printf("test_udp_socket: all passed\n");
    return 0;
}
```

- [ ] **Step 2: CMake wiring; verify build fails**

```cmake
# --- A3 sockets + rendezvous binary ---
add_library(arena_udp STATIC src/netplay/udp_socket.c)
target_include_directories(arena_udp PUBLIC src src/netplay src/lobby)
if(WIN32)
  target_link_libraries(arena_udp PUBLIC ws2_32)
endif()

add_executable(test_udp_socket tests/test_udp_socket.c)
target_link_libraries(test_udp_socket arena_udp)
add_test(NAME udp_socket COMMAND test_udp_socket)

add_executable(arena_rendezvous src/lobby/rendezvous_main.c)
target_link_libraries(arena_rendezvous arena_lobby arena_udp)
```

- [ ] **Step 3: Implement `udp_socket.c`**

Standard portable UDP: `#ifdef _WIN32` → winsock2.h + ws2tcpip.h, `WSAStartup` guarded by a static flag, `ioctlsocket(FIONBIO)`; else sys/socket.h + fcntl `O_NONBLOCK`. `udp_send` converts `LobbyEndpoint` (host order) → `sockaddr_in` with `htonl`/`htons`; `udp_recv` converts back with `ntohl`/`ntohs`; `recvfrom` returning WSAEWOULDBLOCK/EAGAIN → 0. `udp_local_endpoint_for`: temp socket, `connect()` to remote, `getsockname`, read `sin_addr`, close temp, `out->port = bound_port`. `udp_now_ms`: `GetTickCount64() & 0xFFFFFFFF` / `clock_gettime(CLOCK_MONOTONIC)` ms.

- [ ] **Step 4: Run test, verify PASS**

Run: `cmake --build build --parallel && ctest --test-dir build -R udp_socket --output-on-failure`

- [ ] **Step 5: Implement `rendezvous_main.c`**

```c
/* The deploy-anywhere rendezvous+relay binary. All logic is rendezvous.c;
 * this is the socket loop. Single-threaded, one socket, no allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rendezvous.h"
#include "../netplay/udp_socket.h"

static UdpSocket g_sock;
static void emit(void* ctx, LobbyEndpoint to, const uint8_t* buf, size_t len) {
    (void)ctx; udp_send(&g_sock, to, buf, len);
}

int main(int argc, char** argv) {
    uint16_t port = LOBBY_DEFAULT_PORT;
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--port")) port = (uint16_t)atoi(argv[i + 1]);
    if (udp_global_init() != 0 || udp_open(&g_sock, port) != 0) {
        fprintf(stderr, "arena_rendezvous: cannot bind UDP %u\n", port);
        return 1;
    }
    static Rendezvous rv;
    rv_init(&rv, udp_now_ms(), (uint32_t)udp_now_ms() ^ 0xA3A3A3A3u);
    printf("arena_rendezvous: listening on UDP %u\n", port);
    fflush(stdout);
    uint8_t buf[LOBBY_MAX_PACKET];
    LobbyEndpoint from;
    uint32_t last_tick = 0;
    for (;;) {
        int n = udp_recv(&g_sock, &from, buf, sizeof buf);
        uint32_t now = udp_now_ms();
        if (n > 0) rv_handle(&rv, now, from, buf, (size_t)n, emit, NULL);
        if (now - last_tick >= 100u) { rv_tick(&rv, now, emit, NULL); last_tick = now; }
        if (n <= 0) {
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
#endif
        }
    }
}
```

Add one `printf` per state change inside... no — the core is pure. Instead log from main: after `rv_handle`, compare `rv_active_sessions` to a remembered count and print `sessions=%d` on change. Keep the core silent.

- [ ] **Step 6: Build all, smoke the binary**

Run: `cmake --build build --parallel && ./build/arena_rendezvous --port 47555 &` then kill it after seeing the listening line (bash: `sleep 1; kill %1`).
Expected: `arena_rendezvous: listening on UDP 47555`.

- [ ] **Step 7: Commit**

```bash
git add src/netplay/udp_socket.h src/netplay/udp_socket.c src/lobby/rendezvous_main.c tests/test_udp_socket.c CMakeLists.txt
git commit -m "net: udp_socket wrapper + arena_rendezvous binary (A3 task 3)"
```

---

### Task 4: Custom GekkoNet adapter with routes, relay wrapping, impairment

**Files:**
- Create: `src/netplay/net_adapter.h`, `src/netplay/net_adapter.c`
- Create: `tests/test_net_adapter.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 codec + `NetRoute`, Task 3 `udp_*`, `gekkonet.h` (`GekkoNetAdapter`, `GekkoNetAddress`, `GekkoNetResult`).
- Produces:
  - `GekkoNetAdapter* net_adapter_init(UdpSocket* sock, uint16_t session_id, uint8_t local_slot)` — singleton; NULL if already initialized
  - `void net_adapter_set_route(uint8_t slot, NetRoute route)`
  - `void net_adapter_set_relay(LobbyEndpoint relay)`
  - `void net_adapter_impair(const NetImpairment* imp)` — NULL disables
  - `int net_adapter_take_lobby_packet(LobbyEndpoint* from, uint8_t* buf, size_t cap)` — >0 len, 0 none
  - `void net_adapter_shutdown(void)`
  - `typedef struct { uint32_t seed; uint16_t delay_ms, jitter_ms; uint8_t loss_pct, reorder_pct; } NetImpairment;`

- [ ] **Step 1: Write `net_adapter.h`**

```c
/* Custom GekkoNetAdapter: ONE socket for punching, game traffic, and relay,
 * so the NAT mapping the rendezvous observed is the mapping game packets
 * use. Every outgoing game packet is wrapped LOBBY_GAME (direct) or
 * LOBBY_RELAY (via server); every datagram received is a lobby_proto packet.
 *
 * SINGLETON: GekkoNetAdapter carries no user context (same limitation as
 * gekko_default_adapter) — one adapter per process. Memory contract: each
 * received result is THREE heap blocks (result, addr bytes, payload) because
 * GekkoNet frees each via free_data (backend.cpp:158-160). free_data = free. */
#ifndef NET_ADAPTER_H
#define NET_ADAPTER_H

#include "gekkonet.h"
#include "../lobby/lobby_proto.h"
#include "udp_socket.h"

typedef struct { uint32_t seed; uint16_t delay_ms, jitter_ms;
                 uint8_t loss_pct, reorder_pct; } NetImpairment;

GekkoNetAdapter* net_adapter_init(UdpSocket* sock, uint16_t session_id, uint8_t local_slot);
void net_adapter_set_route(uint8_t slot, NetRoute route);
void net_adapter_set_relay(LobbyEndpoint relay);
void net_adapter_impair(const NetImpairment* imp);
int  net_adapter_take_lobby_packet(LobbyEndpoint* from, uint8_t* buf, size_t cap);
void net_adapter_shutdown(void);

#endif
```

- [ ] **Step 2: Write the failing test `tests/test_net_adapter.c`**

Single process, adapter on socket A, a raw `udp_socket` peer on socket B playing "the network":

```c
#include <stdio.h>
#include <string.h>
#include "../src/netplay/net_adapter.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* drain B until a packet arrives (bounded), unpack it */
static int recv_msg(UdpSocket* b, LobbyMsg* m, uint8_t* raw, LobbyEndpoint* from) {
    for (int i = 0; i < 500; i++) {
        int n = udp_recv(b, from, raw, LOBBY_MAX_PACKET);
        if (n > 0) return lobby_unpack(raw, (size_t)n, m) == 0 ? n : -1;
    }
    return 0;
}

int main(void) {
    CHECK(udp_global_init() == 0);
    UdpSocket a, b;
    CHECK(udp_open(&a, 0) == 0 && udp_open(&b, 0) == 0);
    LobbyEndpoint ep_b = { 0x7F000001u, udp_bound_port(&b) };
    LobbyEndpoint ep_a = { 0x7F000001u, udp_bound_port(&a) };

    GekkoNetAdapter* ga = net_adapter_init(&a, 0x1234, /*local_slot*/ 0);
    CHECK(ga != NULL);
    CHECK(net_adapter_init(&a, 0x1234, 0) == NULL);    /* singleton */

    /* DIRECT send: gekko hands a 1-byte slot address; wire shows LOBBY_GAME */
    NetRoute r = { NET_ROUTE_DIRECT, ep_b };
    net_adapter_set_route(1, r);
    uint8_t slot1 = 1;
    GekkoNetAddress ad = { &slot1, 1 };
    ga->send_data(&ad, "hi", 2);
    LobbyMsg m; uint8_t raw[LOBBY_MAX_PACKET]; LobbyEndpoint from;
    CHECK(recv_msg(&b, &m, raw, &from) > 0);
    CHECK(m.type == LOBBY_GAME && m.u.relay.session_id == 0x1234);
    CHECK(m.u.relay.from_slot == 0 && m.u.relay.to_slot == 1);
    CHECK(m.u.relay.payload_len == 2 && memcmp(m.u.relay.payload, "hi", 2) == 0);

    /* RELAY send: route kind RELAY -> wrapped LOBBY_RELAY to the relay ep */
    NetRoute rr = { NET_ROUTE_RELAY, { 0, 0 } };
    net_adapter_set_route(2, rr);
    net_adapter_set_relay(ep_b);                       /* B doubles as "server" */
    uint8_t slot2 = 2; GekkoNetAddress ad2 = { &slot2, 1 };
    ga->send_data(&ad2, "yo", 2);
    CHECK(recv_msg(&b, &m, raw, &from) > 0);
    CHECK(m.type == LOBBY_RELAY && m.u.relay.to_slot == 2);

    /* receive: B sends LOBBY_GAME from slot 1 -> one result, addr = {1} */
    LobbyMsg gm; memset(&gm, 0, sizeof gm);
    gm.type = LOBBY_GAME; gm.u.relay.session_id = 0x1234;
    gm.u.relay.from_slot = 1; gm.u.relay.to_slot = 0;
    gm.u.relay.payload_len = 3; gm.u.relay.payload = (const uint8_t*)"abc";
    uint8_t pk[LOBBY_MAX_PACKET]; size_t pn = lobby_pack(&gm, pk);
    udp_send(&b, ep_a, pk, pn);
    GekkoNetResult** res = NULL; int nres = 0;
    for (int i = 0; i < 500 && nres == 0; i++) res = ga->receive_data(&nres);
    CHECK(nres == 1);
    CHECK(res[0]->addr.size == 1 && *(uint8_t*)res[0]->addr.data == 1);
    CHECK(res[0]->data_len == 3 && memcmp(res[0]->data, "abc", 3) == 0);
    ga->free_data(res[0]->addr.data); ga->free_data(res[0]->data); ga->free_data(res[0]);

    /* wrong session id -> dropped */
    gm.u.relay.session_id = 0x9999; pn = lobby_pack(&gm, pk);
    udp_send(&b, ep_a, pk, pn);
    /* non-game lobby packet -> take_lobby_packet, not a gekko result */
    LobbyMsg hm; memset(&hm, 0, sizeof hm);
    hm.type = LOBBY_START_ACK; hm.u.start_ack.session_id = 0x1234; hm.u.start_ack.slot = 2;
    pn = lobby_pack(&hm, pk);
    udp_send(&b, ep_a, pk, pn);
    nres = -1;
    for (int i = 0; i < 500; i++) { res = ga->receive_data(&nres); if (nres > 0) break; }
    CHECK(nres == 0);                                  /* neither became a game result */
    uint8_t lb[LOBBY_MAX_PACKET]; LobbyEndpoint lfrom;
    int ln = net_adapter_take_lobby_packet(&lfrom, lb, sizeof lb);
    CHECK(ln > 0);
    CHECK(lobby_unpack(lb, (size_t)ln, &m) == 0 && m.type == LOBBY_START_ACK);

    /* impairment: 100% loss drops everything */
    NetImpairment imp = { 7u, 0, 0, 100, 0 };
    net_adapter_impair(&imp);
    ga->send_data(&ad, "xx", 2);
    int got = 0; uint8_t tmp[LOBBY_MAX_PACKET]; LobbyEndpoint f2;
    for (int i = 0; i < 100; i++) if (udp_recv(&b, &f2, tmp, sizeof tmp) > 0) got = 1;
    CHECK(!got);
    /* delay 50ms: not immediate, arrives after pumping past the deadline */
    NetImpairment imp2 = { 7u, 50, 0, 0, 0 };
    net_adapter_impair(&imp2);
    ga->send_data(&ad, "zz", 2);
    got = 0;
    for (int i = 0; i < 5; i++) if (udp_recv(&b, &f2, tmp, sizeof tmp) > 0) got = 1;
    CHECK(!got);                                       /* not yet */
    uint32_t t0 = udp_now_ms();
    while (udp_now_ms() - t0 < 80) { int k = 0; ga->receive_data(&k); }  /* pump flushes due sends */
    for (int i = 0; i < 500 && !got; i++) if (udp_recv(&b, &f2, tmp, sizeof tmp) > 0) got = 1;
    CHECK(got);

    net_adapter_shutdown();
    udp_close(&a); udp_close(&b);
    if (fails) { printf("test_net_adapter: %d FAILED\n", fails); return 1; }
    printf("test_net_adapter: all passed\n");
    return 0;
}
```

- [ ] **Step 3: CMake wiring; verify fail**

Add `src/netplay/net_adapter.c` to `arena_netplay` sources and link `arena_lobby arena_udp` into `arena_netplay`; test target:

```cmake
add_library(arena_netplay STATIC src/netplay/sync_session.c src/netplay/net_adapter.c)
target_link_libraries(arena_netplay PUBLIC arena_sim arena_lobby arena_udp GekkoNet)

add_executable(test_net_adapter tests/test_net_adapter.c)
target_link_libraries(test_net_adapter arena_netplay)
add_test(NAME net_adapter COMMAND test_net_adapter)
```

(Keep the existing `GEKKONET_STATIC` define and include dirs on `arena_netplay`. `lobby_client.c` joins these sources in Task 5, not here.)

- [ ] **Step 4: Implement `net_adapter.c`**

Module state (singleton):

```c
typedef struct { uint32_t due_ms; LobbyEndpoint to; uint16_t len; uint8_t buf[LOBBY_MAX_PACKET]; } DelayedPkt;
static struct {
    int          active;
    UdpSocket*   sock;
    uint16_t     session_id;
    uint8_t      local_slot;
    NetRoute     route[ARENA_MAX_PLAYERS];
    LobbyEndpoint relay;
    NetImpairment imp; int imp_on; uint32_t imp_rng;
    DelayedPkt   dq[512]; int dq_n;                     /* overflow: drop + count */
    uint8_t      lobbyq[16][LOBBY_MAX_PACKET]; uint16_t lobbyq_len[16];
    LobbyEndpoint lobbyq_from[16]; int lobbyq_head, lobbyq_count;
    GekkoNetResult* results[64];
    uint32_t     drop_count, overflow_count;
    GekkoNetAdapter iface;
} g;
```

- `send_data(addr, data, len)`: slot = `*(uint8_t*)addr->data`; look up route; build `LobbyMsg` type `LOBBY_GAME` (DIRECT → dest = route.ep) or `LOBBY_RELAY` (RELAY → dest = g.relay), `from_slot = g.local_slot`, `to_slot = slot`, payload = data; `lobby_pack`; then `impaired_send(dest, buf, n)`.
- `impaired_send`: if off → `udp_send`. Else xorshift `imp_rng`: loss roll (`% 100 < loss_pct` → drop, count); delay = `delay_ms ± (rng % (jitter+1))`; reorder roll adds +1ms to the PREVIOUS queued packet's due time (cheap reorder); delay == 0 → send now; else queue (full → send immediately + overflow_count++).
- `flush_due()` (called at the top of `receive_data`): send queued packets whose `due_ms <= udp_now_ms()`, compact the queue.
- `receive_data(len)`: `flush_due()`; drain socket (bounded 64 packets); each: `lobby_unpack` — fail → drop_count++. Type `LOBBY_GAME` or `LOBBY_RELAY` with `session_id == g.session_id` and `to_slot == g.local_slot`: malloc 3 blocks → result{addr = 1 byte `from_slot`, data = payload copy}; append to `g.results`. Any other lobby type: push into the lobby ring (full → overwrite oldest). Return `g.results`, `*len = count`.
- `free_data = free`. `net_adapter_shutdown`: `memset(&g, 0, sizeof g)` (frees nothing — gekko already freed delivered results; undelivered ones only exist between receive_data and gekko's loop, which is within one pump).

- [ ] **Step 5: Run test, verify PASS; run whole ctest for no regressions**

Run: `cmake --build build --parallel && ctest --test-dir build -R net_adapter --output-on-failure && ctest --test-dir build --output-on-failure`

- [ ] **Step 6: Commit**

```bash
git add src/netplay/net_adapter.h src/netplay/net_adapter.c tests/test_net_adapter.c CMakeLists.txt
git commit -m "netplay: custom GekkoNet adapter - one socket, routes, relay wrap, impairment (A3 task 4)"
```

---

### Task 5: Lobby client + `SyncConfig.adapter` + 4P mesh test (direct variant)

**Files:**
- Create: `src/netplay/lobby_client.h`, `src/netplay/lobby_client.c`
- Modify: `src/netplay/sync_session.h` (add `adapter` field), `src/netplay/sync_session.c` (use it; slot-byte actor addresses)
- Create: `tests/test_netplay_mesh.c`, `tests/run_mesh_test.sh`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–4 everything; `sync_session.h` as it exists (`sync_create`, `sync_frame`, `sync_hash_at`, `sync_desynced`, `sync_present_tick`).
- Produces:
  - `int lobby_host_begin(LobbyClient* lc, UdpSocket* sock, LobbyEndpoint server, uint8_t num_players, uint32_t seed, uint8_t arena_id, uint8_t input_delay, uint32_t now_ms)` — 0 ok
  - `int lobby_join_begin(LobbyClient* lc, UdpSocket* sock, const char* code, uint32_t now_ms)` — 0 ok, −1 bad code
  - `LobbyClientStage lobby_poll(LobbyClient* lc, uint32_t now_ms)` — pump; call every few ms until READY/FAILED
  - `const LobbyResult* lobby_result(const LobbyClient* lc)` — valid at READY
  - `const char* lobby_code(const LobbyClient* lc)` — host only, valid once session id known
  - `const char* lobby_fail_stage(const LobbyClient* lc)` — e.g. `"PUNCHING"`
  - `void lobby_post_poll(LobbyClient* lc, uint32_t now_ms)` — post-READY duty: drain `net_adapter_take_lobby_packet`, re-ack START refans
  - `void lobby_set_forced_relay(LobbyClient* lc)` — test hook: never punch, report `ok = 0` for all pairs
  - stages: `LOBBY_C_REGISTERING, LOBBY_C_JOINING, LOBBY_C_WAITING_PEERS, LOBBY_C_PUNCHING, LOBBY_C_AWAITING_START, LOBBY_C_READY, LOBBY_C_FAILED`
  - `LobbyResult { uint8_t local_slot, num_players, arena_id, input_delay; uint16_t session_id; uint32_t seed; NetRoute route[ARENA_MAX_PLAYERS]; LobbyEndpoint server; }`
  - `SyncConfig` gains `GekkoNetAdapter* adapter;` — non-NULL replaces the default adapter and switches remote actor addresses to 1-byte slot indices (GekkoNet compares actor addresses as opaque byte blobs — `backend.cpp:229/470/489` use size+bytes `Equals`)

- [ ] **Step 1: `sync_session.h` — add the adapter field and doc line**

```c
typedef struct {
    SyncMode    mode;
    uint8_t     num_players;
    uint8_t     local_mask;
    uint16_t    local_port;                   /* ONLINE + default adapter: our UDP port */
    const char* peer_addr[ARENA_MAX_PLAYERS]; /* ONLINE + default adapter: "ip:port" per remote */
    uint32_t    seed;
    uint8_t     arena_id;
    uint8_t     input_delay;                  /* frames, 0-2 typical */
    GekkoNetAdapter* adapter;                 /* ONLINE: non-NULL = custom adapter (A3);
                                                 remote actor addresses become 1-byte
                                                 slot indices; peer_addr/local_port unused */
} SyncConfig;
```

`sync_session.h` must now include `gekkonet.h`? No — forward-declare: add `typedef struct GekkoNetAdapter GekkoNetAdapter;` before the struct (gekkonet.h's own typedef is compatible; guard with `#ifndef GEKKONET_H` is NOT possible — just use the forward declaration, C allows repeated compatible typedefs in C11 §6.7).

- [ ] **Step 2: `sync_session.c` — honor `cfg->adapter`**

In `sync_create`, replace the ONLINE adapter block:

```c
    if (cfg->mode == SYNC_ONLINE) {
        if (cfg->adapter) {
            gekko_net_adapter_set(s->gk, cfg->adapter);   /* caller owns lifetime */
        } else {
            gekko_net_adapter_set(s->gk, gekko_default_adapter(cfg->local_port));
            s->online_adapter = true;
        }
    }
```

And in the actor loop, remote branch:

```c
        } else {
            GekkoNetAddress a;
            if (cfg->adapter) {
                s->slot_addr[i] = (uint8_t)i;         /* new field: uint8_t slot_addr[ARENA_MAX_PLAYERS]; */
                a.data = &s->slot_addr[i];
                a.size = 1;
            } else {
                a.data = (void*)cfg->peer_addr[i];
                a.size = (unsigned int)strlen(cfg->peer_addr[i]);
            }
            s->handle[i] = gekko_add_actor(s->gk, GekkoRemotePlayer, &a);
        }
```

Run existing netplay tests to prove no regression: `ctest --test-dir build -R "netplay_link|netplay_stress|netplay_p2p" --output-on-failure` — all PASS (NULL adapter path untouched).

- [ ] **Step 3: Write `lobby_client.h`**

```c
/* Lobby bootstrap state machine. Non-blocking: caller pumps lobby_poll every
 * few ms. During bootstrap the client reads the socket directly; after READY
 * the net adapter owns the socket and lobby_post_poll drains stray lobby
 * packets (START refans) from the adapter's queue. */
#ifndef LOBBY_CLIENT_H
#define LOBBY_CLIENT_H

#include "../lobby/lobby_proto.h"
#include "udp_socket.h"

#define LOBBY_RETRY_MS        500u
#define LOBBY_RETRY_MAX       10
#define LOBBY_PUNCH_PERIOD_MS 250u
#define LOBBY_PUNCH_BUDGET_MS 3000u
#define LOBBY_TIMEOUT_MS      30000u

typedef enum { LOBBY_C_IDLE = 0, LOBBY_C_REGISTERING, LOBBY_C_JOINING,
               LOBBY_C_WAITING_PEERS, LOBBY_C_PUNCHING, LOBBY_C_AWAITING_START,
               LOBBY_C_READY, LOBBY_C_FAILED } LobbyClientStage;

typedef struct {
    uint8_t       local_slot, num_players, arena_id, input_delay;
    uint16_t      session_id;
    uint32_t      seed;
    NetRoute      route[ARENA_MAX_PLAYERS];   /* [local_slot] = NET_ROUTE_NONE */
    LobbyEndpoint server;
} LobbyResult;

typedef struct {
    uint8_t  stage, is_host, forced_relay;
    UdpSocket* sock;
    LobbyEndpoint server, private_ep;
    uint16_t session_id;
    uint8_t  local_slot, num_players;
    uint32_t net_ver, seed; uint8_t arena_id, input_delay;
    uint32_t begin_ms, last_send_ms, rng;
    uint8_t  start_seen, host_start_sent;
    struct {
        uint8_t  present, acked, reported;
        LobbyEndpoint pub, priv, chosen;
        uint32_t nonce, punch_begin_ms, last_punch_ms;
        int8_t   route;                        /* -1 unknown, else LobbyRoute */
        uint8_t  punch_alt;                    /* alternates priv/pub probes */
    } peer[ARENA_MAX_PLAYERS];
    LobbyResult result;
    char code[LOBBY_CODE_LEN + 1];
    char fail_stage[16];
} LobbyClient;

int  lobby_host_begin(LobbyClient* lc, UdpSocket* sock, LobbyEndpoint server,
                      uint8_t num_players, uint32_t seed, uint8_t arena_id,
                      uint8_t input_delay, uint32_t now_ms);
int  lobby_join_begin(LobbyClient* lc, UdpSocket* sock, const char* code,
                      uint32_t now_ms);
LobbyClientStage lobby_poll(LobbyClient* lc, uint32_t now_ms);
void lobby_post_poll(LobbyClient* lc, uint32_t now_ms);
const LobbyResult* lobby_result(const LobbyClient* lc);
const char* lobby_code(const LobbyClient* lc);
const char* lobby_fail_stage(const LobbyClient* lc);
void lobby_set_forced_relay(LobbyClient* lc);

#endif
```

- [ ] **Step 4: Implement `lobby_client.c`**

The state machine, stage by stage (every stage: overall `now - begin_ms > LOBBY_TIMEOUT_MS` → FAILED with the current stage name copied to `fail_stage`):

- `lobby_host_begin`: store config; `udp_local_endpoint_for(server, udp_bound_port(sock), &private_ep)`; `net_ver = arena_net_version()`; local_slot = 0; stage REGISTERING; send HOST_REQ now (and every LOBBY_RETRY_MS from `lobby_poll`).
- `lobby_join_begin`: `lobby_code_decode` (−1 → return −1 without touching stage); same private-ep sniff; stage JOINING; send JOIN_REQ on the retry cadence.
- `lobby_poll`: drain socket (`udp_recv` loop), `lobby_unpack`, dispatch:
  - REGISTERING + HOST_RESP → session_id; `lobby_code_encode(server, session_id, code)`; mark self present (slot 0, pub unknown — never punched by self); stage WAITING_PEERS.
  - JOINING + JOIN_RESP → local_slot, num_players; stage WAITING_PEERS. JOINING + REJECT → FAILED, fail_stage = `"REJECTED:<reason>"` (short names: `VERSION`, `FULL`, `BAD_SESSION`, `EXPIRED`).
  - PEER_INTRO (any stage ≥ WAITING_PEERS): record `peer[slot].pub/priv/present`; assign a nonce (`rng` xorshift, nonzero); `punch_begin_ms = now`.
  - WAITING_PEERS: when all `num_players` slots present (self included) → stage PUNCHING. (`forced_relay`: skip probing; immediately send PUNCH_REPORT ok=0 for every pair containing local_slot.)
  - PUNCHING: for each present remote peer not yet `acked` and not `reported`: every LOBBY_PUNCH_PERIOD_MS send PUNCH alternating to `priv`/`pub` (`punch_alt ^= 1`); past `punch_begin_ms + LOBBY_PUNCH_BUDGET_MS` → send PUNCH_REPORT{min(local,peer), max(local,peer), ok=0} to server, mark `reported`.
  - PUNCH received (from anyone, any stage): reply PUNCH_ACK echoing the nonce TO THE SOURCE ENDPOINT of the punch (that reply is what opens our NAT for them).
  - PUNCH_ACK received: if nonce matches `peer[slot].nonce`: `acked = 1`, `chosen = ` the ack's source endpoint; send PUNCH_REPORT{pair, ok=1}; mark `reported`.
  - PAIR_ROUTE: if the pair contains local_slot, set `peer[other].route`; count decided pairs. Host additionally counts ALL pairs: when all `n*(n-1)/2` pairs decided → build + send START (once; server refans), `host_start_sent = 1`.
  - START received (joiner or host): store seed/arena/num_players/input_delay (host: verify it echoes what it sent); send START_ACK; if all pairs containing local_slot have routes → fill `result` (routes: DIRECT → `chosen`, RELAY → kind only; server = `server`) → stage READY.
  - AWAITING_START is the stage between PUNCHING complete (local pairs decided) and START arrival — enter it when local routes are decided but no START yet.
  - Retry cadence: whatever the current outstanding request is (HOST_REQ/JOIN_REQ), resend every LOBBY_RETRY_MS, up to LOBBY_RETRY_MAX — exceeding it → FAILED (stage name).
  - Send KEEPALIVE {session, slot} every 10s once registered (WAITING_PEERS onward, pre-READY).
- `lobby_post_poll`: `net_adapter_take_lobby_packet` drain; a START refan → re-send START_ACK via the raw socket (the adapter shares it — use `udp_send(lc->sock, lc->server, ...)`).

No allocation; the struct is caller-owned. All sends go through `udp_send(lc->sock, ...)` — same socket the adapter will use, which is the whole point.

- [ ] **Step 5: Write the mesh client `tests/test_netplay_mesh.c`**

```c
/* 4-process rendezvous+mesh convergence test client. The harness
 * (run_mesh_test.sh) launches arena_rendezvous, one --host and three --join
 * of the printed code, and asserts all four "mesh " lines match.
 *   test_netplay_mesh --server 127.0.0.1:PORT --host 4 [--ticks N] [--forced-relay] [--impair PROFILE]
 *   test_netplay_mesh --server 127.0.0.1:PORT --join CODE [...]
 * Exit: 0 converged, 1 failure, 2 usage, 3 desync (used by later variants). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/netplay/lobby_client.h"
#include "../src/netplay/net_adapter.h"
#include "../src/netplay/sync_session.h"

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep(ms * 1000); }
#endif

static int parse_ep(const char* s, LobbyEndpoint* out); /* "a.b.c.d:port" -> host-order */

int main(int argc, char** argv) {
    const char* server_s = NULL; const char* join_code = NULL;
    int host_players = 0, ticks = 600, forced_relay = 0;
    const char* impair = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server") && i + 1 < argc) server_s = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host_players = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join_code = argv[++i];
        else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--forced-relay")) forced_relay = 1;
        else if (!strcmp(argv[i], "--impair") && i + 1 < argc) impair = argv[++i];
    }
    if ((!host_players && !join_code) || (host_players && !server_s)) {
        fprintf(stderr, "usage: --server ip:port --host N | --join CODE [--ticks N] [--forced-relay] [--impair lan0|wan100|rough200]\n");
        return 2;
    }

    if (udp_global_init() != 0) { printf("FAIL udp_init\n"); return 1; }
    UdpSocket sock;
    if (udp_open(&sock, 0) != 0) { printf("FAIL udp_open\n"); return 1; }

    static LobbyClient lc;                       /* zero-init */
    if (host_players) {
        LobbyEndpoint srv;
        if (parse_ep(server_s, &srv) != 0) { printf("FAIL server addr\n"); return 2; }
        lobby_host_begin(&lc, &sock, srv, (uint8_t)host_players,
                         0xB0BB1E5, 0, 1, udp_now_ms());
    } else {
        if (lobby_join_begin(&lc, &sock, join_code, udp_now_ms()) != 0) {
            printf("FAIL bad code\n"); return 2;
        }
    }
    if (forced_relay) lobby_set_forced_relay(&lc);

    LobbyClientStage st;
    int code_printed = 0;
    do {
        st = lobby_poll(&lc, udp_now_ms());
        if (host_players && !code_printed && lobby_code(&lc)[0]) {
            printf("code %s\n", lobby_code(&lc)); fflush(stdout);
            code_printed = 1;
        }
        sleep_ms(2);
    } while (st != LOBBY_C_READY && st != LOBBY_C_FAILED);
    if (st == LOBBY_C_FAILED) { printf("FAIL stage=%s\n", lobby_fail_stage(&lc)); return 1; }

    const LobbyResult* lr = lobby_result(&lc);
    GekkoNetAdapter* ga = net_adapter_init(&sock, lr->session_id, lr->local_slot);
    for (int i = 0; i < lr->num_players; i++)
        if (i != lr->local_slot) net_adapter_set_route((uint8_t)i, lr->route[i]);
    net_adapter_set_relay(lr->server);
    if (impair) {
        NetImpairment imp = { 0xA3A3A3A3u ^ lr->local_slot, 0, 0, 0, 0 };
        if (!strcmp(impair, "wan100"))  { imp.delay_ms = 50;  imp.jitter_ms = 10; imp.loss_pct = 1; imp.reorder_pct = 1; }
        if (!strcmp(impair, "rough200")){ imp.delay_ms = 100; imp.jitter_ms = 25; imp.loss_pct = 3; imp.reorder_pct = 2; }
        net_adapter_impair(&imp);       /* lan0 = all zero = effectively off */
    }

    SyncConfig cfg = {0};
    cfg.mode = SYNC_ONLINE;
    cfg.num_players = lr->num_players;
    cfg.local_mask = (uint8_t)(1u << lr->local_slot);
    cfg.seed = lr->seed;
    cfg.arena_id = lr->arena_id;
    cfg.input_delay = lr->input_delay;
    cfg.adapter = ga;
    SyncSession* s = sync_create(&cfg);
    if (!s) { printf("FAIL sync_create\n"); return 1; }

    const uint32_t target = (uint32_t)ticks;
    const int me = lr->local_slot;
    for (int frame = 0; frame < ticks * 40; frame++) {
        uint32_t t = sync_present_tick(s);
        if (t >= target + 90) break;
        ArenaInput in[ARENA_MAX_PLAYERS] = { 0, 0, 0, 0 };
        /* per-slot deterministic script, same family as test_netplay_p2p */
        int sx = (int)((t / 8 + (uint32_t)(me * 16)) % 63) - 31;
        int bomb = (int)((t + (uint32_t)(me * 37)) % 150) < 40;
        in[me] = arena_input_pack(sx, 10, (t % 120) == (uint32_t)me, bomb,
                                  (t % 137) == 0);
        sync_frame(s, in);
        lobby_post_poll(&lc, udp_now_ms());
        if (sync_desynced(s)) { printf("FAIL: desync\n"); return 3; }
        sleep_ms(2);
    }
    uint32_t h = sync_hash_at(s, target);
    if (h == 0) { printf("FAIL: never confirmed tick %u\n", target); return 1; }
    printf("mesh slot=%d tick=%u hash=%08x\n", me, target, h);
    sync_destroy(s);
    net_adapter_shutdown();
    return 0;
}
```

- [ ] **Step 6: Write `tests/run_mesh_test.sh`**

Follow `run_p2p_test.sh`'s scratch-dir dance verbatim (MSYS2 mktemp trap — copy that block). Then:

```bash
#!/usr/bin/env bash
# run_mesh_test.sh <mesh_bin> <rendezvous_bin> <variant>
# variant: direct | relay | impair
set -u
BIN="$1"; RV="$2"; VARIANT="${3:-direct}"
TICKS=600
PORT=$((47000 + RANDOM % 2000))
# ... scratch dir block copied from run_p2p_test.sh -> $DIR ...

EXTRA=""
case "$VARIANT" in
  relay)  EXTRA="--forced-relay" ;;
  impair) EXTRA="--impair wan100" ;;
esac

"$RV" --port $PORT > "$DIR/rv.txt" 2>&1 &
RVPID=$!
trap 'kill $RVPID 2>/dev/null; rm -rf "$DIR"' EXIT
sleep 1

"$BIN" --server 127.0.0.1:$PORT --host 4 --ticks $TICKS $EXTRA > "$DIR/p0.txt" 2>&1 &
P0=$!
CODE=""
for i in $(seq 1 50); do
    CODE=$(grep -m1 '^code ' "$DIR/p0.txt" 2>/dev/null | cut -d' ' -f2)
    [ -n "$CODE" ] && break
    sleep 0.2
done
[ -n "$CODE" ] || { echo "mesh: no lobby code from host"; cat "$DIR/p0.txt"; exit 1; }

for i in 1 2 3; do
    "$BIN" --server 127.0.0.1:$PORT --join "$CODE" --ticks $TICKS $EXTRA > "$DIR/p$i.txt" 2>&1 &
    eval "P$i=\$!"
done
FAIL=0
for i in 0 1 2 3; do
    eval "wait \$P$i"; RC=$?
    [ $RC -eq 0 ] || { echo "mesh: player $i exit $RC"; FAIL=1; }
done
for i in 0 1 2 3; do echo "--- player $i:"; cat "$DIR/p$i.txt"; done
[ $FAIL -eq 0 ] || exit 1

REF=$(grep '^mesh ' "$DIR/p0.txt" | sed 's/slot=[0-9]//')
for i in 1 2 3; do
    CUR=$(grep '^mesh ' "$DIR/p$i.txt" | sed 's/slot=[0-9]//')
    [ "$CUR" = "$REF" ] || { echo "mesh: HASH MISMATCH p$i"; exit 1; }
done
if [ "$VARIANT" = "relay" ]; then
    grep -q 'relay' "$DIR/rv.txt" || true   # informational; route assertions live in test_rendezvous
fi
echo "mesh($VARIANT): MATCH - $REF"
exit 0
```

- [ ] **Step 7: CMake wiring**

```cmake
add_executable(test_netplay_mesh tests/test_netplay_mesh.c)
target_link_libraries(test_netplay_mesh arena_netplay)
if(BASH_PROGRAM)
  add_test(NAME netplay_mesh_direct
           COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mesh_test.sh
                   $<TARGET_FILE:test_netplay_mesh> $<TARGET_FILE:arena_rendezvous> direct)
  set_tests_properties(netplay_mesh_direct PROPERTIES TIMEOUT 180)
endif()
```

(`lobby_client.c` joins `arena_netplay` sources here.)

- [ ] **Step 8: Build; run the direct mesh test until green**

Run: `cmake --build build --parallel && ctest --test-dir build -R netplay_mesh_direct --output-on-failure`
Expected: `mesh(direct): MATCH - mesh tick=600 hash=XXXXXXXX`. This is the milestone gate of the whole plan — four processes, real rendezvous, real punching (loopback always punches), identical confirmed hashes.

- [ ] **Step 9: Full ctest + gate; commit**

Run: `ctest --test-dir build --output-on-failure && tools\gate.ps1`

```bash
git add src/netplay/lobby_client.h src/netplay/lobby_client.c src/netplay/sync_session.h src/netplay/sync_session.c tests/test_netplay_mesh.c tests/run_mesh_test.sh CMakeLists.txt
git commit -m "netplay: lobby client + SyncConfig.adapter; 4P mesh converges via rendezvous (A3 task 5)"
```

---

### Task 6: Relay path end-to-end (forced-relay variant)

**Files:**
- Modify: `tests/run_mesh_test.sh` (nothing — variant already parameterized), `CMakeLists.txt` (new ctest entry)
- Modify (only if the variant exposes gaps): `src/netplay/lobby_client.c`, `src/lobby/rendezvous.c`, `src/netplay/net_adapter.c`

**Interfaces:** consumes Task 5's `--forced-relay` flag (already in the mesh client) and Task 2's route arbitration.

- [ ] **Step 1: Add the ctest entry**

```cmake
  add_test(NAME netplay_mesh_relay
           COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mesh_test.sh
                   $<TARGET_FILE:test_netplay_mesh> $<TARGET_FILE:arena_rendezvous> relay)
  set_tests_properties(netplay_mesh_relay PROPERTIES TIMEOUT 180)
```

- [ ] **Step 2: Run it; debug the relay path until green**

Run: `cmake --build build --parallel && ctest --test-dir build -R netplay_mesh_relay --output-on-failure`

With `--forced-relay` on all four clients, every pair reports `ok = 0`, the server declares all six pairs RELAY, and ALL game traffic flows client → server → client. Likely first-run gaps, in the order to check them: (a) the server's RELAY forward validates the sender against the peer's registered endpoint — the mesh client's socket IS the registered socket, so this should hold; (b) `net_adapter_set_relay` must be called before the first `sync_frame` (it is, in the mesh client); (c) the rendezvous session must stay alive on relay traffic (`last_relay_ms` updates — Task 2 test 6 covers the logic; this proves it over real sockets).

Add relay evidence the harness can assert on. The rendezvous core is printf-free (pure), so add a getter — `uint32_t rv_relay_forwards(const Rendezvous* rv);` in `rendezvous.h`, a counter incremented on every relay forward in `rendezvous.c` — and in `rendezvous_main.c`'s loop print `relay active\n` (once, latched, fflush) the first time the getter goes nonzero. Then replace Task 5's placeholder `|| true` line in `run_mesh_test.sh` with:

```bash
if [ "$VARIANT" = "relay" ]; then
    grep -q '^relay active' "$DIR/rv.txt" || { echo "mesh: relay carried no packets"; exit 1; }
fi
```

- [ ] **Step 3: Verify direct variant still green (no accidental relay-always)**

Run: `ctest --test-dir build -R "netplay_mesh" --output-on-failure` — both variants green. In the DIRECT run's `rv.txt`, the `relay active` line must be ABSENT — add that negative assert to the harness's direct branch (`! grep -q '^relay active' "$DIR/rv.txt"`); this makes the direct test falsifiable — it now fails if punching silently broke and traffic fell back to relay.

- [ ] **Step 4: Commit**

```bash
git add tests/run_mesh_test.sh src/lobby/rendezvous.c src/lobby/rendezvous_main.c CMakeLists.txt
git commit -m "net: forced-relay mesh variant green; relay-vs-direct asserted from server logs (A3 task 6)"
```

---

### Task 7: SyncStats + impaired variant + `tools/net-soak.ps1`

**Files:**
- Modify: `src/netplay/sync_session.h`, `src/netplay/sync_session.c` (stats counters)
- Modify: `tests/test_netplay_mesh.c` (metrics + rbhist output)
- Create: `tools/net-soak.ps1`
- Modify: `CMakeLists.txt` (impaired ctest entry)

**Interfaces:**
- Produces:
  - `typedef struct { uint32_t rollback_ticks, max_rollback_depth, stall_frames, pumps; uint32_t rbhist[9]; } SyncStats;` — `rbhist[d]` = pumps whose rollback depth was d (8 = "8 or more")
  - `void sync_stats(const SyncSession* s, SyncStats* out)`
  - mesh client prints: `metrics slot=N stalls=U rb_ticks=U rb_max=U pumps=U` and `rbhist 0:N 1:N ... 8:N`

- [ ] **Step 1: Implement stats in `sync_session.c`**

Add to `struct SyncSession`: `SyncStats stats; bool started;`. In `sync_frame`:

```c
    int rb = 0;
    /* inside the game-event loop's AdvanceEvent case: */
            if (e->data.adv.rolling_back) rb++; else fresh++;
    /* after the loop: */
    if (s->mode == SYNC_ONLINE && s->started) {
        s->stats.pumps++;
        s->stats.rollback_ticks += (uint32_t)rb;
        if ((uint32_t)rb > s->stats.max_rollback_depth) s->stats.max_rollback_depth = (uint32_t)rb;
        s->stats.rbhist[rb > 8 ? 8 : rb]++;
        if (fresh == 0 && s->connected) s->stats.stall_frames++;
    }
```

`started` set where `GekkoSessionStarted` is handled. `sync_stats` copies the struct. Keep couch/stress behavior untouched (stats stay zero there — asserted implicitly by existing tests passing).

- [ ] **Step 2: Print metrics from the mesh client (after the "mesh" line)**

```c
    SyncStats st_; sync_stats(s, &st_);
    printf("metrics slot=%d stalls=%u rb_ticks=%u rb_max=%u pumps=%u\n",
           me, st_.stall_frames, st_.rollback_ticks, st_.max_rollback_depth, st_.pumps);
    printf("rbhist");
    for (int i = 0; i <= 8; i++) printf(" %d:%u", i, st_.rbhist[i]);
    printf("\n");
```

- [ ] **Step 3: Impaired ctest entry; run until green**

```cmake
  add_test(NAME netplay_mesh_impaired
           COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mesh_test.sh
                   $<TARGET_FILE:test_netplay_mesh> $<TARGET_FILE:arena_rendezvous> impair)
  set_tests_properties(netplay_mesh_impaired PROPERTIES TIMEOUT 300)
```

Run: `cmake --build build --parallel && ctest --test-dir build -R netplay_mesh_impaired --output-on-failure`
Note: under wan100 the run is SLOWER (rollbacks + stalls); the client's `ticks * 40` frame budget and the 300s timeout absorb it. Hashes must still MATCH — impairment changes timing, never state.

- [ ] **Step 4: Write `tools/net-soak.ps1`**

Structure (house PS style — `$ErrorActionPreference = "Stop"`, repo-root derivation like `gate.ps1`):

```powershell
# Repeated 4P mesh matches under a named impairment profile; aggregates the
# clients' metrics lines and enforces the A3 exit criteria.
#   tools\net-soak.ps1 -Profile wan100 -Matches 5
#   tools\net-soak.ps1 -Profile rough200 -Matches 2   # informational: no thresholds
# Exit criteria (wan100 and lan0 only): desyncs == 0, p95 rollback depth <= 8,
# stalls < 1/min of simulated play.
param([ValidateSet("lan0","wan100","rough200")][string]$Profile = "wan100",
      [int]$Matches = 5, [int]$Ticks = 3600, [switch]$ForcedRelay)
```

Per match: pick `$port = Get-Random -Min 47000 -Max 48999`; `Start-Process` `arena_rendezvous` with stdout to a per-match file; start host client (`--impair $Profile`, plus `--forced-relay` when the switch is set), poll its stdout file for `^code `, start 3 joiners, `Wait-Process` all with a 300s timeout (kill + fail on expiry); parse each client file: any exit ≠ 0 or `FAIL`/`desync` line → desync/failure count; sum `rbhist` buckets and `stalls`. After all matches: p95 from the summed histogram (smallest d where cumulative ≥ 0.95 × pumps); `stallRate = totalStalls / (Matches * Ticks / 3600.0)` per minute — note 3600 ticks = 60s of sim, so minutes = `Matches * Ticks / 3600`; print a summary table; enforce thresholds unless `$Profile -eq "rough200"`; exit nonzero on violation. Binaries located at `build\test_netplay_mesh.exe` / `build\arena_rendezvous.exe` (fall back to no `.exe` suffix for POSIX).

- [ ] **Step 5: Run one short soak locally**

Run: `tools\net-soak.ps1 -Profile wan100 -Matches 2 -Ticks 1800`
Expected: summary table, `SOAK GREEN`, exit 0. Then `tools\net-soak.ps1 -Profile lan0 -Matches 1` — trivially green (baseline sanity: near-zero rollbacks on loopback).

- [ ] **Step 6: Full ctest + gate; commit**

```bash
git add src/netplay/sync_session.h src/netplay/sync_session.c tests/test_netplay_mesh.c tools/net-soak.ps1 CMakeLists.txt
git commit -m "netplay: rollback/stall stats, impaired mesh variant, net-soak.ps1 with exit criteria (A3 task 7)"
```

---

### Task 8: Desync bundles + `replay_bundle` + injection falsifiability

**Files:**
- Create: `src/netplay/desync_bundle.h` (on-disk format, shared)
- Modify: `src/netplay/sync_session.h`, `src/netplay/sync_session.c` (input recording, desync info, bundle dump, corrupt hook)
- Create: `tools/replay_bundle.c`
- Modify: `tests/test_netplay_mesh.c` (`--inject`/bundle flags), `tests/run_mesh_test.sh` (inject variant), `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `desync_bundle.h`:

```c
/* On-disk desync bundle. Little-endian, packed by explicit field writes (no
 * struct dumping — no padding on disk). */
#ifndef DESYNC_BUNDLE_H
#define DESYNC_BUNDLE_H
#include <stdint.h>
#include "../arena/arena_state.h"
#define BUNDLE_MAGIC   0x41334442u   /* "A3DB" */
#define BUNDLE_VERSION 1u
#define BUNDLE_MAX_TICKS 36000u      /* 10 min @ 60Hz */
typedef struct {
    uint32_t magic, version, net_version, seed;
    uint8_t  arena_id, num_players, local_slot, pad;
    uint32_t detect_tick, local_checksum, remote_checksum;
    int32_t  remote_slot;            /* -1 if unknown */
    /* then: 256 x {u32 tick, u32 hash} ring; 944B state snapshot;
       u32 input_count; input_count x 4 x u16 inputs */
} BundleHeader;
#endif
```

  - `sync_session.h` additions:

```c
typedef struct { uint32_t tick, local_hash, remote_hash; int remote_slot; } SyncDesyncInfo;
bool sync_desync_info(const SyncSession* s, SyncDesyncInfo* out);  /* false if no desync */
int  sync_dump_bundle(const SyncSession* s, const char* path);     /* 0 ok */
void sync_debug_corrupt(SyncSession* s);  /* TEST HOOK: next advanced tick XORs
                                             state.rng with 1 after arena_tick -
                                             a deterministic divergence for
                                             falsifying the desync pipeline */
```

  - `replay_bundle <a.bin> [b.bin]` — exit 0 and prints `DIVERGED tick=N ...` lines (see Step 4)
  - mesh client: `--inject T` (corrupt at present tick ≥ T) and `--bundle-dir DIR`; on desync prints `desync tick=U local=%08x remote=%08x`, dumps `DIR/desync_slotN.bin`, exits 3

- [ ] **Step 1: Input recording + desync capture + corrupt hook in `sync_session.c`**

- `sync_create`: `s->rec = malloc(BUNDLE_MAX_TICKS * ARENA_MAX_PLAYERS * sizeof(ArenaInput));` (calloc), `rec_count = 0`. Free in `sync_destroy`.
- AdvanceEvent (before `arena_tick`): `uint32_t T = s->state.tick; if (T < BUNDLE_MAX_TICKS) { memcpy(&s->rec[T * 4], in, 4 * sizeof(ArenaInput)); if (T + 1 > s->rec_count) s->rec_count = T + 1; }` — rollbacks overwrite, converging to confirmed inputs, same argument as the hash ring.
- After `arena_tick` in the same case: `if (s->corrupt_pending && !e->data.adv.rolling_back) { s->state.rng ^= 1u; s->corrupt_pending = false; }` — the corruption must land on a FRESH advance, never inside a rollback re-sim (a re-simmed tick would be overwritten by later rollbacks, making the divergence tick nondeterministic and the inject test flaky).
- SessionEvent `GekkoDesyncDetected`: capture `frame/local_checksum/remote_checksum/remote_handle` into `s->dinfo`; map handle → slot by scanning `s->handle[]`; keep the existing `desynced = true`.
- `sync_dump_bundle`: fopen wb; write header fields individually (LE via the same wr32/wr16 helpers — copy them, or write bytes; do NOT fwrite structs), then the ring (`s->ring[i].tick, .hash` for i 0..255), the 944B state (`fwrite(&s->state, 1, sizeof(ArenaState), f)` — in-memory layout, LE hosts only, documented), then `rec_count` and `rec_count * 4` inputs.

- [ ] **Step 2: Write `tools/replay_bundle.c`**

```c
/* Offline desync localization.
 *   replay_bundle a.bin          -> replay a's inputs; compare to a's hash
 *                                   ring; report where the LIVE session
 *                                   diverged from its own confirmed inputs.
 *   replay_bundle a.bin b.bin    -> also compare the two input histories and
 *                                   both rings; name the culprit peer and the
 *                                   first diverging tick; dump sim state
 *                                   fields at that tick (arena_trace-style).
 * Exit 0 = analysis printed; 1 = unreadable/invalid bundle. */
```

Algorithm (implement exactly):

1. Load bundle(s); validate magic/version; refuse mismatched `net_version` between two bundles (print both, exit 1 — different builds can't be compared).
2. Two bundles: compare input histories tick by tick over `min(rec_count_a, rec_count_b)`; first difference → print `CONFIRMED INPUTS DIFFER tick=N player=P a=%04x b=%04x` (that's a GekkoNet/adapter-level bug — inputs are supposed to be identical once confirmed) and continue to step 3 anyway.
3. Replay: `arena_init(&s, arena_id, num_players, seed)`; per tick apply `rec[T*4..T*4+3]`, `arena_tick`, record `hash[T+1] = arena_hash(&s)`... (note the ring records post-tick hashes keyed by `state.tick` — mirror `sync_session.c:129-130`: after tick T executes, `state.tick` is T+1 and the ring holds `(T+1, hash)`).
4. For each bundle: walk its ring entries with `tick <= rec_count`; first entry where ring hash ≠ replay hash → `DIVERGED bundle=<file> tick=N ring=%08x replay=%08x`. No mismatch → `CONSISTENT bundle=<file>` (that peer's live sim matched its confirmed inputs; the other peer is the culprit).
5. At the earliest divergence tick N: re-run the replay to N−1 and to N, and print the arena_trace-style field lines (`pX_x, pX_y, pX_z, pX_vx, pX_vz, pX_yaw, pX_state, pX_hp` per player + `live_bombs, phase` — same fields as `tools/arena_trace.c:46-48`) for both ticks, labeled `before`/`after`.

CMake: `add_executable(replay_bundle tools/replay_bundle.c)` + `target_link_libraries(replay_bundle arena_sim)` + include dirs `src`.

- [ ] **Step 3: Mesh client `--inject T` + `--bundle-dir DIR`**

In the arg loop: `--inject` (int, default −1), `--bundle-dir` (string, default `.`). In the frame loop before `sync_frame`: `if (inject >= 0 && (int)sync_present_tick(s) >= inject) { sync_debug_corrupt(s); inject = -1; }`. Replace the desync early-return with:

```c
        if (sync_desynced(s)) {
            SyncDesyncInfo di;
            if (sync_desync_info(s, &di))
                printf("desync tick=%u local=%08x remote=%08x\n",
                       di.tick, di.local_hash, di.remote_hash);
            char path[512];
            snprintf(path, sizeof path, "%s/desync_slot%d.bin", bundle_dir, me);
            if (sync_dump_bundle(s, path) == 0) printf("bundle %s\n", path);
            fflush(stdout);
            return 3;
        }
```

- [ ] **Step 4: Inject variant in `run_mesh_test.sh`**

New variant `inject` (4th argv = replay_bundle path):

```bash
  inject) EXTRA="--bundle-dir $DIR" ;;    # --inject 300 goes ONLY to joiner 1, added below
```

Joiner 1 gets `--inject 300`; all clients get `--bundle-dir $DIR`. Assertions replace the hash-match block for this variant:

- the injected client (p1) exited 3 and printed `^desync tick=`;
- at least one OTHER client also exited 3 (GekkoNet's checksum exchange fires on both sides of a pair);
- `$DIR/desync_slot1.bin` exists plus at least one other bundle;
- `"$REPLAY" "$DIR/desync_slot1.bin"` prints `DIVERGED` with `tick=N` where `300 <= N <= 320` (the corrupt lands on the next advanced tick; confirmation lag ≤ prediction window + delay) — extract N and range-check it;
- two-bundle mode: `"$REPLAY" "$DIR/desync_slot1.bin" "$DIR/desync_slot<other>.bin"` names bundle slot1 as DIVERGED and the other as CONSISTENT.

CMake:

```cmake
  add_test(NAME netplay_desync_inject
           COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mesh_test.sh
                   $<TARGET_FILE:test_netplay_mesh> $<TARGET_FILE:arena_rendezvous> inject
                   $<TARGET_FILE:replay_bundle>)
  set_tests_properties(netplay_desync_inject PROPERTIES TIMEOUT 180)
```

- [ ] **Step 5: Run all four mesh variants + full ctest + gate**

Run: `cmake --build build --parallel && ctest --test-dir build -R "netplay_mesh|netplay_desync" --output-on-failure && ctest --test-dir build --output-on-failure && tools\gate.ps1`

The inject test is the falsifiability proof for the whole desync pipeline: detector fires, bundles write, offline replay localizes the corrupted tick and names the culprit peer. Without it, the desync path is a gate that has never failed.

- [ ] **Step 6: Commit**

```bash
git add src/netplay/desync_bundle.h src/netplay/sync_session.h src/netplay/sync_session.c tools/replay_bundle.c tests/test_netplay_mesh.c tests/run_mesh_test.sh CMakeLists.txt
git commit -m "netplay: desync bundles + replay_bundle localization + injection falsifiability (A3 task 8)"
```

---

### Task 9: Viewer `--host`/`--join CODE` + README

**Files:**
- Modify: `tools/viewer/viewer_main.c` (replace the A2 manual addressing block, `viewer_main.c:77-135` region)
- Modify: `README.md` (new binaries + soak tool)
- Modify: `CMakeLists.txt` (viewer links get the new libs — `arena_netplay` already carries them after Task 5)

**Interfaces:** consumes Tasks 1–8; no new interfaces produced.

- [ ] **Step 1: Replace the viewer's online bootstrap**

New flags: `--host <server_ip[:port]>` (port defaults to `LOBBY_DEFAULT_PORT`), `--players N` (2–4, default 2), `--join <CODE>`. Delete the A2 `--peer`/`--port`/`--player` manual plumbing and its `fixed online seed until A3 lobby` comment — the lobby now carries the seed (host uses `0xB0BB1E5 ^ (uint32_t)time(NULL)` so consecutive matches differ; joiners get it from START).

Bootstrap runs BEFORE `SDL_Init`, in the console (a debug tool may block there):

```c
    SyncSession* session = NULL;
    int local_player = 0;
    LobbyClient lc; memset(&lc, 0, sizeof lc);
    UdpSocket sock;
    GekkoNetAdapter* ga = NULL;
    int online = (host_arg != NULL || join_code != NULL);
    if (online) {
        if (udp_global_init() != 0 || udp_open(&sock, 0) != 0) {
            fprintf(stderr, "viewer: udp init failed\n"); return 1;
        }
        if (host_arg) {
            LobbyEndpoint srv;
            if (viewer_parse_server(host_arg, &srv) != 0) { fprintf(stderr, "bad --host\n"); return 2; }
            lobby_host_begin(&lc, &sock, srv, (uint8_t)players_arg,
                             0xB0BB1E5u ^ (uint32_t)time(NULL), 0, 1, udp_now_ms());
        } else if (lobby_join_begin(&lc, &sock, join_code, udp_now_ms()) != 0) {
            fprintf(stderr, "bad code\n"); return 2;
        }
        LobbyClientStage st; int shown = 0;
        do {
            st = lobby_poll(&lc, udp_now_ms());
            if (host_arg && !shown && lobby_code(&lc)[0]) {
                printf("lobby code: %s  (waiting for %d players...)\n",
                       lobby_code(&lc), players_arg); fflush(stdout);
                shown = 1;
            }
            sleep_ms_portable(5);
        } while (st != LOBBY_C_READY && st != LOBBY_C_FAILED);
        if (st == LOBBY_C_FAILED) {
            fprintf(stderr, "lobby failed at stage %s\n", lobby_fail_stage(&lc)); return 1;
        }
        const LobbyResult* lr = lobby_result(&lc);
        ga = net_adapter_init(&sock, lr->session_id, lr->local_slot);
        for (int i = 0; i < lr->num_players; i++)
            if (i != lr->local_slot) net_adapter_set_route((uint8_t)i, lr->route[i]);
        net_adapter_set_relay(lr->server);
        SyncConfig c = {0};
        c.mode = SYNC_ONLINE; c.num_players = lr->num_players;
        c.local_mask = (uint8_t)(1u << lr->local_slot);
        c.seed = lr->seed; c.arena_id = lr->arena_id;
        c.input_delay = lr->input_delay; c.adapter = ga;
        local_player = lr->local_slot;
        session = sync_create(&c);
    }
```

In the frame loop: call `lobby_post_poll(&lc, udp_now_ms())` when online; on first `sync_desynced`:

```c
        if (online && sync_desynced(session) && !desync_latched) {
            desync_latched = 1;
            SyncDesyncInfo di;
            if (sync_desync_info(session, &di))
                fprintf(stderr, "DESYNC tick=%u local=%08x remote=%08x\n",
                        di.tick, di.local_hash, di.remote_hash);
            char p[128]; snprintf(p, sizeof p, "desync_viewer_slot%d.bin", local_player);
            if (sync_dump_bundle(session, p) == 0)
                fprintf(stderr, "bundle written: %s (run replay_bundle on it)\n", p);
            SDL_SetWindowTitle(win, "bmhero arena viewer - DESYNC (match stopped)");
        }
        if (!desync_latched) { /* existing input+sync_frame pumping */ }
        /* rendering continues on the frozen last state */
```

Helpers: `viewer_parse_server` is `parse_ep` from the mesh client with the `:port` part optional (missing → `LOBBY_DEFAULT_PORT`) — a static function in `viewer_main.c`, dotted-quad only (no DNS, per spec). `sleep_ms_portable` is the same `#ifdef _WIN32 Sleep/usleep` pair the mesh client uses (bootstrap runs before `SDL_Init`, so don't reach for `SDL_Delay`).

Keep smoke mode and couch mode byte-identical (they never touch the new code).

- [ ] **Step 2: Build; two-instance loopback smoke**

Run (two shells, or background the first):
`./build/arena_rendezvous --port 47901 &`
`./build/arena_viewer --host 127.0.0.1:47901 --players 2` → prints `lobby code: XXXXX-XXXXX-XXXXX`
`./build/arena_viewer --join <that code>`
Expected: both windows open into the same match; WASD moves one player, visible in both. Close both; kill the server.

- [ ] **Step 3: README**

Add to the tools/layout section: `arena_rendezvous` (deploy-anywhere lobby+relay server, `--port`, default 40064), `test_netplay_mesh` + `run_mesh_test.sh` (4P mesh harness, four variants), `tools/net-soak.ps1` (profiles + exit criteria), `replay_bundle` (desync localization), viewer `--host/--join`. One line each, matching the README's existing register.

- [ ] **Step 4: Full ctest + gate + commit**

Run: `ctest --test-dir build --output-on-failure && tools\gate.ps1`

```bash
git add tools/viewer/viewer_main.c README.md CMakeLists.txt
git commit -m "tools: viewer hosts/joins by lobby code; desync freeze + bundle; README (A3 task 9)"
```

---

## Verification (whole-plan exit)

1. `tools\gate.ps1` — GATE GREEN, `[hash] fbdb0d08` unmoved, `TUNE_VERSION 21` unmoved.
2. `ctest --test-dir build --output-on-failure` — all suites including the four mesh variants.
3. `tools\net-soak.ps1 -Profile wan100 -Matches 5` — SOAK GREEN (p95 rollback ≤ 8, stalls < 1/min, desyncs 0).
4. `tools\net-soak.ps1 -Profile wan100 -Matches 2 -ForcedRelay` — green (relay under impairment).
5. Manual real-WAN checkpoint (human, post-merge): `arena_rendezvous` on a reachable box, viewer `--host`/`--join` across a real network — the A3 human boot. NOT a plan task; it's the user's checkpoint, scheduled like a feel round.

## Deferred / recorded

- The fork slice (local = `local_slot`, puppets = other slots, in-game lobby) — spec §G carries the contract.
- IPv6 / DNS names in codes; crypto/auth; host migration; mid-match rerouting — spec non-goals.
- `rough200` profile is informational only, never a gate.
