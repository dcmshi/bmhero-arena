# A2 SyncSession Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One `SyncSession` C interface over GekkoNet giving couch/online/stress configurations of the same loop, the viewer wired through it, and a two-process localhost match with matching hashes as the exit gate.

**Architecture:** `src/netplay/sync_session.{h,c}` (C11) wraps GekkoNet's C API; the session owns the `ArenaState` and is the only `arena_tick` caller (save/load/advance events = memcpy/memcpy/tick). GekkoNet arrives via FetchContent pinned to a release tag and builds as its own static C++ lib. Spec: `docs/superpowers/specs/2026-07-19-a2-syncsession-design.md`.

**Tech Stack:** C11 (ours), GekkoNet `v20260629200724-02c447c` (C++17 internals, C API, built-in ASIO UDP adapter), CMake FetchContent, ctest + bash two-process harness.

## Global Constraints

- **`src/arena/` unchanged** — pinned hash `4b6687d4` must not move.
- Our code stays C11; C++ enters only through GekkoNet's own static lib (top-level `project()` gains `CXX`).
- GekkoNet pinned to tag `v20260629200724-02c447c`; target name `GekkoNet`; consumers need `GEKKONET_STATIC` defined and, on WIN32, `ws2_32`.
- Verified API facts used throughout (from `gekkonet.h` + `Examples/OnlineSession.cpp` at that tag): `gekko_create/gekko_start/gekko_net_adapter_set/gekko_add_actor/gekko_set_local_delay/gekko_add_local_input/gekko_update_session/gekko_session_events/gekko_network_poll/gekko_default_adapter/gekko_default_adapter_destroy/gekko_destroy`; local actors pass `NULL` address; remote address = `GekkoNetAddress{ data = "ip:port" string, size = strlen }`; save events fill `*state_len`, `*checksum`, copy into `state`; advance events carry `inputs` blob of `num_players × input_size`, plus `rolling_back` flag.
- Branch: `feature/a2-syncsession` (created in Task 1).
- **[ucrt64]** = PowerShell `$env:MSYSTEM='UCRT64'; C:\msys64\usr\bin\bash.exe -lc '<command>'` from repo root `C:\Users\dshi\GitRepos\bmhero-arena`.
- Commit after every task with the message given.

---

### Task 1: GekkoNet dependency + netplay skeleton (link gate)

**Files:**
- Modify: `CMakeLists.txt`
- Create: `src/netplay/sync_session.h`, `src/netplay/sync_session.c` (create/destroy only)
- Test: `tests/test_netplay_link.c`

**Interfaces:**
- Produces: the full public header below (later tasks implement it); CMake target `arena_netplay` that later targets link.

- [ ] **Step 1: Create the branch**

```bash
git checkout -b feature/a2-syncsession
```

- [ ] **Step 2: Write the failing link test**

Create `tests/test_netplay_link.c`:
```c
/* Link gate: GekkoNet fetched, built, and callable through the wrapper. */
#include <stdio.h>
#include "../src/netplay/sync_session.h"

int main(void) {
    SyncConfig cfg = {0};
    cfg.mode = SYNC_STRESS;
    cfg.num_players = 2;
    cfg.local_mask = 0x03;
    cfg.seed = 0xBEEF;
    SyncSession* s = sync_create(&cfg);
    if (!s) { printf("FAIL: sync_create\n"); return 1; }
    if (sync_state(s)->num_players != 2) { printf("FAIL: state init\n"); return 1; }
    sync_destroy(s);
    printf("netplay_link: ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Write the public header**

Create `src/netplay/sync_session.h`:
```c
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

typedef struct {
    SyncMode    mode;
    uint8_t     num_players;                  /* 2..4 */
    uint8_t     local_mask;                   /* bit i = player i is local */
    uint16_t    local_port;                   /* ONLINE: our UDP port */
    const char* peer_addr[ARENA_MAX_PLAYERS]; /* ONLINE: "ip:port" per remote */
    uint32_t    seed;
    uint8_t     arena_id;
    uint8_t     input_delay;                  /* frames, 0-2 typical */
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
```

- [ ] **Step 4: Write the create/destroy skeleton**

Create `src/netplay/sync_session.c`:
```c
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
    (void)s; (void)inputs;
    return 0;   /* Task 2 */
}
```

- [ ] **Step 5: CMake — CXX, FetchContent, netplay lib, link test**

In `CMakeLists.txt`: change the project line to
```cmake
project(bmhero_arena C CXX)
```
Append after the `test_bomb_mechanics` block:
```cmake
# --- netplay: GekkoNet (BSD-2, C API, C++ internals) via FetchContent ---
include(FetchContent)
FetchContent_Declare(gekkonet
  GIT_REPOSITORY https://github.com/HeatXD/GekkoNet.git
  GIT_TAG        v20260629200724-02c447c
  SOURCE_SUBDIR  GekkoLib)          # library only; skips SDL3 examples
FetchContent_MakeAvailable(gekkonet)

add_library(arena_netplay STATIC src/netplay/sync_session.c)
target_include_directories(arena_netplay PUBLIC
  src src/netplay ${gekkonet_SOURCE_DIR}/GekkoLib/include)
target_compile_definitions(arena_netplay PUBLIC GEKKONET_STATIC)
target_link_libraries(arena_netplay PUBLIC arena_sim GekkoNet)
if(WIN32)
  target_link_libraries(arena_netplay PUBLIC ws2_32)
endif()

add_executable(test_netplay_link tests/test_netplay_link.c)
target_link_libraries(test_netplay_link arena_netplay)
add_test(NAME netplay_link COMMAND test_netplay_link)
```

- [ ] **Step 6: Build and run (first build fetches + compiles GekkoNet)**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake -S . -B build -G Ninja && cmake --build build && ./build/test_netplay_link.exe
```
Expected: GekkoNet clones + compiles; `netplay_link: ALL TESTS PASSED`.
If GekkoNet's C++ fails under MinGW gcc 16, STOP and report (spec §6 lists the escape hatches) — do not patch GekkoNet silently.

- [ ] **Step 7: Run whole suite, commit**

Run **[ucrt64]**: `ctest --test-dir build --output-on-failure` → `100% tests passed out of 5`.
```bash
git add CMakeLists.txt src/netplay/sync_session.h src/netplay/sync_session.c tests/test_netplay_link.c
git commit -m "feat(netplay): GekkoNet via FetchContent + SyncSession skeleton (link gate)"
```

---

### Task 2: sync_frame + stress gate (TDD)

**Files:**
- Modify: `src/netplay/sync_session.c` (implement `sync_frame`)
- Test: `tests/test_netplay_stress.c`
- Modify: `CMakeLists.txt` (register)

**Interfaces:**
- Consumes: Task 1 skeleton.
- Produces: working `sync_frame` for all three modes (event pump: session events → connected/desynced; game events → save/load/advance with ring recording).

- [ ] **Step 1: Write the failing stress test**

Create `tests/test_netplay_stress.c`:
```c
/* Determinism gate: GekkoNet stress session continuously rolls back and
 * re-simulates the scripted 4P match, cross-checking state checksums
 * (check_distance 8). Any divergence raises a desync -> fail. */
#include <stdio.h>
#include "../src/netplay/sync_session.h"

int main(void) {
    SyncConfig cfg = {0};
    cfg.mode = SYNC_STRESS;
    cfg.num_players = 4;
    cfg.local_mask = 0x0F;
    cfg.seed = 0xB0BB1E5;
    SyncSession* s = sync_create(&cfg);
    if (!s) { printf("FAIL: create\n"); return 1; }

    uint32_t r = 0xC0FFEE01;
    int advanced = 0;
    for (uint32_t t = 0; t < 3600; t++) {          /* one stressed minute */
        ArenaInput in[ARENA_MAX_PLAYERS];
        for (int i = 0; i < 4; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            int sx = (int)(r & 63) - 32; if (sx < -31) sx = -31;
            int sy = (int)((r >> 6) & 63) - 32; if (sy < -31) sy = -31;
            int bomb = (int)((t + (uint32_t)(i * 37)) % (uint32_t)(90 + i * 80))
                       < (30 + i * 40);
            int set = ((t + (uint32_t)(i * 53)) % 137u) == 0;
            in[i] = arena_input_pack(sx, sy, ((r >> 12) & 31) == 0, bomb, set);
        }
        advanced += sync_frame(s, in);
        if (sync_desynced(s)) {
            printf("FAIL: desync detected at frame %u\n", t);
            return 1;
        }
    }
    printf("stress: advanced %d, tick %u, hash %08x\n",
           advanced, sync_present_tick(s), sync_present_hash(s));
    if (advanced < 3000) { printf("FAIL: session barely advanced\n"); return 1; }
    sync_destroy(s);
    printf("netplay_stress: ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Register and run to verify failure**

Append to `CMakeLists.txt`:
```cmake
add_executable(test_netplay_stress tests/test_netplay_stress.c)
target_link_libraries(test_netplay_stress arena_netplay)
add_test(NAME netplay_stress COMMAND test_netplay_stress)
```
Run **[ucrt64]**: `cmake -S . -B build -G Ninja && cmake --build build && ./build/test_netplay_stress.exe`
Expected: FAIL — `FAIL: session barely advanced` (sync_frame is a stub returning 0).

- [ ] **Step 3: Implement sync_frame**

In `src/netplay/sync_session.c`, replace the stub:
```c
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
```

- [ ] **Step 4: Run to verify pass**

Run **[ucrt64]**: `cmake --build build && ./build/test_netplay_stress.exe && ctest --test-dir build --output-on-failure`
Expected: `netplay_stress: ALL TESTS PASSED`; ctest `100% tests passed out of 6`.

- [ ] **Step 5: Commit**

```bash
git add src/netplay/sync_session.c tests/test_netplay_stress.c CMakeLists.txt
git commit -m "feat(netplay): sync_frame event pump + GekkoNet stress determinism gate"
```

---

### Task 3: Two-process localhost match (the A2 exit gate)

**Files:**
- Test: `tests/test_netplay_p2p.c`, `tests/run_p2p_test.sh`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: full `SyncSession` (Task 2).
- Produces: `test_netplay_p2p --port P --peer ip:port --player N [--ticks T]` printing `p2p tick <T> hash <8hex>`; harness asserting both processes print identical lines.

- [ ] **Step 1: Write the p2p binary**

Create `tests/test_netplay_p2p.c`:
```c
/* Two-process localhost rollback match. The harness runs this twice:
 *   test_netplay_p2p --port 7101 --peer 127.0.0.1:7102 --player 0
 *   test_netplay_p2p --port 7102 --peer 127.0.0.1:7101 --player 1
 * Each side scripts only ITS player's inputs; identical final
 * "p2p tick T hash H" lines prove both simulations converged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/netplay/sync_session.h"

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep(ms * 1000); }
#endif

int main(int argc, char** argv) {
    int port = 0, player = 0, ticks = 600;
    const char* peer = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--port"))   port = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--peer"))   peer = argv[i + 1];
        if (!strcmp(argv[i], "--player")) player = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--ticks"))  ticks = atoi(argv[i + 1]);
    }
    if (!port || !peer || player < 0 || player > 1) {
        fprintf(stderr, "usage: --port P --peer ip:port --player 0|1 [--ticks T]\n");
        return 2;
    }

    SyncConfig cfg = {0};
    cfg.mode = SYNC_ONLINE;
    cfg.num_players = 2;
    cfg.local_mask = (uint8_t)(1u << player);
    cfg.local_port = (uint16_t)port;
    cfg.peer_addr[1 - player] = peer;
    cfg.seed = 0xB0BB1E5;
    cfg.input_delay = 1;
    SyncSession* s = sync_create(&cfg);
    if (!s) { printf("FAIL: create\n"); return 1; }

    /* run until the target tick is well behind us, then read its hash */
    const uint32_t target = (uint32_t)ticks;
    for (int frame = 0; frame < ticks * 20; frame++) {
        uint32_t t = sync_present_tick(s);
        if (t >= target + 90) break;               /* target long confirmed */
        ArenaInput in[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
        int sx = (int)((t / 8 + (uint32_t)(player * 16)) % 63) - 31;
        int bomb = (int)((t + (uint32_t)(player * 37)) % 150) < 40;
        in[player] = arena_input_pack(sx, 10, (t % 120) == 0, bomb,
                                      (t % 137) == 0);
        sync_frame(s, in);
        if (sync_desynced(s)) { printf("FAIL: desync\n"); return 1; }
        sleep_ms(2);
    }

    uint32_t h = sync_hash_at(s, target);
    if (h == 0) { printf("FAIL: never reached tick %u\n", target); return 1; }
    printf("p2p tick %u hash %08x\n", target, h);
    sync_destroy(s);
    return 0;
}
```

- [ ] **Step 2: Write the two-process harness**

Create `tests/run_p2p_test.sh`:
```bash
#!/usr/bin/env bash
# Launches two test_netplay_p2p processes against each other on loopback
# and asserts their final confirmed hashes match. Usage: run_p2p_test.sh <binary>
set -u
BIN="$1"
TICKS=600
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

"$BIN" --port 7101 --peer 127.0.0.1:7102 --player 0 --ticks $TICKS > "$DIR/a.txt" 2>&1 &
PA=$!
"$BIN" --port 7102 --peer 127.0.0.1:7101 --player 1 --ticks $TICKS > "$DIR/b.txt" 2>&1
RB=$?
wait $PA
RA=$?

echo "--- player 0:"; cat "$DIR/a.txt"
echo "--- player 1:"; cat "$DIR/b.txt"
[ $RA -eq 0 ] && [ $RB -eq 0 ] || { echo "netplay_p2p: process failure"; exit 1; }

A=$(grep '^p2p ' "$DIR/a.txt")
B=$(grep '^p2p ' "$DIR/b.txt")
if [ -n "$A" ] && [ "$A" = "$B" ]; then
    echo "netplay_p2p: MATCH - $A"
else
    echo "netplay_p2p: HASH MISMATCH"
    exit 1
fi
```

- [ ] **Step 3: Register with ctest (bash-guarded)**

Append to `CMakeLists.txt`:
```cmake
add_executable(test_netplay_p2p tests/test_netplay_p2p.c)
target_link_libraries(test_netplay_p2p arena_netplay)
find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM)
  add_test(NAME netplay_p2p
           COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_p2p_test.sh
                   $<TARGET_FILE:test_netplay_p2p>)
  set_tests_properties(netplay_p2p PROPERTIES TIMEOUT 120)
else()
  message(STATUS "bash not found - netplay_p2p test skipped")
endif()
```

- [ ] **Step 4: Run it**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -R netplay_p2p --output-on-failure
```
Expected: both process outputs shown, ending `netplay_p2p: MATCH - p2p tick 600 hash <8hex>`; `100% tests passed`. (Windows Firewall may prompt on first run — allow it.)
Then the full suite: `ctest --test-dir build --output-on-failure` → `100% tests passed out of 7`.

- [ ] **Step 5: Commit**

```bash
git add tests/test_netplay_p2p.c tests/run_p2p_test.sh CMakeLists.txt
git commit -m "test(netplay): two-process localhost rollback match - A2 exit gate"
```

---

### Task 4: Viewer through the session

**Files:**
- Modify: `tools/viewer/viewer_main.c`
- Modify: `CMakeLists.txt` (viewer links arena_netplay)

**Interfaces:**
- Consumes: full `SyncSession`.
- Produces: viewer modes — default couch session; `--host <port> --players N`; `--join <ip:port> [--player K]`; `--frames` smoke unchanged (sessionless).

- [ ] **Step 1: Link the viewer against netplay**

In `CMakeLists.txt`, inside the `if(SDL3_FOUND)` block, change:
```cmake
  target_link_libraries(arena_viewer arena_sim SDL3::SDL3)
```
to:
```cmake
  target_link_libraries(arena_viewer arena_netplay SDL3::SDL3)
```
(`arena_netplay` already links `arena_sim` PUBLIC.)

- [ ] **Step 2: Wire the session into viewer_main.c**

Apply these changes to `tools/viewer/viewer_main.c`:

(a) Add include after `"viewer_draw.h"`:
```c
#include "sync_session.h"
```
(b) Replace the argument-parsing block in `main` (the `for (int i = 1; ...)` loop) with:
```c
    int frames_limit = -1;
    uint32_t seed = 0xC0FFEE;
    int host_port = 0, join_player = 1, online_players = 2;
    const char* join_addr = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--frames") == 0) frames_limit = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--seed") == 0) seed = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        if (strcmp(argv[i], "--host") == 0) host_port = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--players") == 0) online_players = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--join") == 0) join_addr = argv[i + 1];
        if (strcmp(argv[i], "--player") == 0) join_player = atoi(argv[i + 1]);
    }
    const int smoke = frames_limit >= 0;
    const int online = !smoke && (host_port != 0 || join_addr != NULL);
```
(c) After the `arena_init(&state, 0, 2, seed);` line, add session setup:
```c
    /* session: couch by default; --host/--join = online; --frames = none */
    SyncSession* session = NULL;
    int local_player = 0;               /* our player index (camera, online) */
    if (!smoke) {
        SyncConfig scfg = {0};
        if (online) {
            scfg.mode = SYNC_ONLINE;
            scfg.num_players = (uint8_t)online_players;
            scfg.input_delay = 1;
            scfg.local_port = (uint16_t)(host_port ? host_port : 7102);
            if (host_port) {            /* host = player 0; A2: 2P online */
                local_player = 0;
                scfg.local_mask = 0x01;
                scfg.peer_addr[1] = "0.0.0.0:0"; /* placeholder; host learns
                    the joiner's address from its first packets (GekkoNet
                    default adapter). If the pinned GekkoNet build requires a
                    concrete address, pass --peer on both sides instead. */
            } else {
                local_player = join_player;
                scfg.local_mask = (uint8_t)(1u << join_player);
                scfg.peer_addr[join_player ? 0 : 1] = join_addr;
            }
            scfg.seed = 0xB0BB1E5;      /* fixed online seed until A3 lobby */
        } else {
            scfg.mode = SYNC_COUCH;
            scfg.num_players = 2;
            scfg.local_mask = 0x03;
            scfg.seed = seed;
        }
        session = sync_create(&scfg);
        if (!session) { fprintf(stderr, "sync_create failed\n"); return 1; }
    }
```
NOTE: if the host-side "learn joiner from packets" placeholder does not hold
for the pinned GekkoNet (remote actors need concrete addresses), make BOTH
sides pass explicit peers: host runs `--host 7101 --peer 127.0.0.1:7102`;
add a `--peer` flag mirroring `--join`'s parsing. Decide by testing in
Step 4 — do not leave the placeholder in the final commit unverified.

(d) Replace the couch device-recount + tick block (`if (!smoke) { int devices... }` through the `for (int t = 0; t < n; t++) { ... arena_tick(...); }` loop) with:
```c
        /* couch: players follow devices (floor 2); recreate session on change */
        if (!smoke && !online) {
            int devices = 0;
            for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
                if (pads.pad[i]) devices++;
            if (keyboard_player(&pads) >= 0) devices++;
            int want = devices < 2 ? 2 : devices;
            if (want != sync_state(session)->num_players) {
                sync_destroy(session);
                SyncConfig scfg = {0};
                scfg.mode = SYNC_COUCH;
                scfg.num_players = (uint8_t)want;
                scfg.local_mask = (uint8_t)((1u << want) - 1u);
                scfg.seed = seed;
                session = sync_create(&scfg);
                if (!session) { fprintf(stderr, "sync_create failed\n"); return 1; }
            }
        }

        uint64_t t_now = SDL_GetTicksNS();
        double ms = (double)(t_now - t_prev) / 1e6;
        t_prev = t_now;

        int n = smoke ? 1 : vclock_advance(&clk, ms);
        for (int t = 0; t < n; t++) {
            ArenaInput in[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
            if (smoke) {
                arena_tick(&state, in);
            } else {
                for (int i = 0; i < sync_state(session)->num_players; i++)
                    if (!online || i == local_player)
                        in[i] = read_input(&pads, online ? 0 : i, &cam);
                sync_frame(session, in);
            }
        }
```
(online: the single local player reads device slot 0 — first pad or
keyboard.)
(e) Point rendering and camera at the session state — replace `const ArenaPlayer* tp = &state.players[cam_target];` and the `draw_*(..., &state, ...)` calls:
```c
        const ArenaState* rs = smoke ? &state : sync_state(session);
        const ArenaPlayer* tp = &rs->players[cam_target];
```
and use `rs` in `vcam_update` target selection, `draw_scene(ren, &cam, rs, ...)`, `draw_facing(ren, &cam, rs, ...)`, `draw_hud(ren, rs, ...)`, and `cam_target = (cam_target + 1) % rs->num_players;` for Tab. The final printf uses `rs` too:
```c
    const ArenaState* fs = smoke ? &state : sync_state(session);
    printf("frames %d tick %u hash %08x\n", frame, fs->tick, arena_hash(fs));
    if (session) sync_destroy(session);
```
(f) Gate the feel keys and surgery (online would desync): in the KEY_DOWN
switch, wrap the cases for `SDLK_P`, `SDLK_RIGHTBRACKET`, `SDLK_LEFTBRACKET`,
and `SDLK_R` with `if (!online) { ... }`, and replace the sudden-death
neutralizer block with:
```c
        if (!smoke && !online) {
            ArenaState* ms = sync_state_debug_mut(session);
            if (ms && !allow_sd && ms->phase == PHASE_SUDDEN_DEATH) {
                ms->phase = PHASE_PLAY;
                ms->phase_timer = 0;
                ms->shrink_step = 0;
            }
        }
```
`R` restart (couch only): recreate the session (destroy + create with the
same couch config; Shift+R mutates `seed` first) instead of `arena_init`.
(g) Session status line — after the `draw_hud` call:
```c
        if (!smoke) {
            char net[96];
            snprintf(net, sizeof net, "NET %s %s",
                     online ? "ONLINE" : "COUCH",
                     sync_connected(session) ? "SYNCED"
                     : (sync_desynced(session) ? "DESYNC!" : "CONNECTING"));
            draw_text(ren, 8, 52 + 20.0f * ARENA_MAX_PLAYERS, 2, net);
        }
```
(add `#include <stdio.h>` is already present for snprintf... it is: viewer_main.c includes stdio.h.)

- [ ] **Step 3: Build + smoke regression**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake --build build && ./build/arena_viewer.exe --frames 300
```
Expected: builds clean; smoke prints the SAME line as before this task
(sessionless path untouched): `frames 300 tick 300 hash eeeb76f6`.

- [ ] **Step 4: Two-window localhost check (agent-driven, then human)**

Run **[ucrt64]** (background the host, run the joiner ~5s, then close):
```
./build/arena_viewer.exe --host 7101 --players 2 &
sleep 2 && ./build/arena_viewer.exe --join 127.0.0.1:7101 --player 1 &
sleep 8 && echo "windows up - check them"
```
Expected: two windows, both showing NET ONLINE SYNCED once connected, both
simulating the same match. If the host cannot add the remote actor without a
concrete peer address (see Step 2c NOTE), implement the `--peer` flag on the
host side now and retest. Then ask the user to play both windows (one WASD,
one whatever) and confirm the match is mirrored and responsive.

- [ ] **Step 5: Full ctest + commit**

Run **[ucrt64]**: `ctest --test-dir build --output-on-failure` → all pass.
```bash
git add tools/viewer/viewer_main.c CMakeLists.txt
git commit -m "feat(viewer): drive the match through SyncSession - couch default, --host/--join online"
```

---

### Task 5: CI netcode job, docs, acceptance, PR

**Files:**
- Create: `.github/workflows/netcode.yml`
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: CI workflow**

Create `.github/workflows/netcode.yml`:
```yaml
name: netcode
on: [push, pull_request]

jobs:
  test:
    strategy:
      matrix:
        os: [ubuntu-latest, windows-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - name: configure + build
        shell: bash
        run: |
          cmake -S . -B build
          cmake --build build --config Release --parallel
      - name: ctest
        shell: bash
        run: ctest --test-dir build -C Release --output-on-failure
```

- [ ] **Step 2: README + CLAUDE.md**

`README.md` Layout section, add:
```
    src/netplay/              SyncSession: one GekkoNet wrapper, three configs
                              (couch / online / stress); owns ArenaState + tick
```
README Build section, add after the viewer line:
```
    # online (2P localhost): window 1: --host 7101 --players 2
    #                        window 2: --join 127.0.0.1:7101 --player 1
```
`CLAUDE.md`: status paragraph append:
```markdown
**A2 SyncSession complete (2026-07-19).** `src/netplay/` wraps GekkoNet
(FetchContent, pinned tag) behind one C interface — couch/online/stress —
session owns ArenaState, sole arena_tick caller. Gates: GekkoNet stress
session (continuous rollback re-sim) + two-process localhost match with
matching confirmed hashes, both in ctest and the `netcode` CI job
(ubuntu+windows). Viewer drives matches through the session (couch default;
`--host/--join` online; `--frames` smoke stays sessionless). Next: A1
render bridge (fork BMHeroRecomp; ROM assets at runtime) or A3
(rendezvous/lobby/WAN soak).
```
Update CLAUDE.md "Next milestones": remove A2 item, renumber (A1 render
bridge becomes 1), add `A3 online hardening (rendezvous, lobby, relay
fallback, WAN soak)` as 2.

- [ ] **Step 3: Acceptance run**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && git diff main --stat -- src/arena/ && ctest --test-dir build --output-on-failure && gcc -std=c11 -Wall -Wextra -O2 -o t src/arena/arena_sim.c tests/test_determinism.c && ./t > /dev/null && echo "sim gate: PASS" && rm t.exe
```
Expected: empty arena diff; all ctest pass; `sim gate: PASS`.

- [ ] **Step 4: Commit, push, PR**

```bash
git add .github/workflows/netcode.yml README.md CLAUDE.md
git commit -m "docs+ci: A2 SyncSession milestone - netcode CI job (ubuntu+windows)"
git push -u origin feature/a2-syncsession
gh pr create --base main --head feature/a2-syncsession --title "A2: SyncSession - GekkoNet rollback netcode (couch/online/stress)" --body "One C interface over GekkoNet (FetchContent, pinned v20260629200724-02c447c, BSD-2): session owns ArenaState and is the sole arena_tick caller; save/load/advance events = memcpy/memcpy/tick. Viewer drives matches through it (couch default, --host/--join for online; --frames smoke unchanged, hash eeeb76f6). Gates: GekkoNet stress session (continuous rollback re-sim over the scripted match) + two-process localhost match asserting identical confirmed hashes; new netcode CI job (ubuntu+windows). src/arena/ untouched (pinned hash 4b6687d4). Spec: docs/superpowers/specs/2026-07-19-a2-syncsession-design.md"
```
Then the human checkpoint: user plays two localhost windows; merge on green CI + good feel.

---

## Plan self-review notes

- Spec coverage: §2 dependency → Task 1; §3 interface → Tasks 1–2 (spec's `sync_confirmed_tick/hash` realized as `sync_present_tick/hash` + `sync_hash_at` — the ring converges to confirmed values, which is what the tests actually need; header docs say so); §4 viewer → Task 4; §5 tests/CI → Tasks 2, 3, 5; §1 non-goals respected (no rendezvous/lobby/relay/spectators/custom adapter).
- Known uncertainty made explicit instead of hidden: whether GekkoNet's default adapter lets the host add a remote actor without a concrete peer address (Task 4 Step 2c NOTE + Step 4 decides it; `--peer` fallback specified).
- The p2p hash comparison uses `sync_hash_at(target)` after running well past `target` — both sides report the same tick's post-rollback-settled hash; GekkoNet's own desync detection runs concurrently as a second net.
- Type consistency: `sync_*` names match between header (Task 1), stress test (Task 2), p2p (Task 3), viewer (Task 4).
