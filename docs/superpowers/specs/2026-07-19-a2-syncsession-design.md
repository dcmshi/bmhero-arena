# A2 SyncSession (GekkoNet netcode wrapper) — design spec

**Date:** 2026-07-19 · **Status:** approved (design review in-session)
**Purpose:** one `SyncSession` interface over
[GekkoNet](https://github.com/HeatXD/GekkoNet) providing couch, online, and
stress configurations of the same loop, with the viewer wired through it —
playable rollback on localhost. Milestone A2/A3 groundwork per
`docs/bmhero-battle-arena-design.md` §6 and architecture doc §5–6.
**Verified during design:** GekkoNet exposes a C API (`gekkonet.h`:
`gekko_create/gekko_start/gekko_add_actor/gekko_add_local_input/
gekko_update_session`), builds with CMake ≥3.15 (MSVC/GCC/Clang), BSD-2
license (GPL-compatible), session types: game, stress/synctest, spectator;
UDP via built-in ASIO adapter or bring-your-own.

---

## 1. Goals and non-goals

**Goals**

- `src/netplay/sync_session.{h,c}` (C11): one interface, three modes —
  `SYNC_COUCH` (all players local, zero delay), `SYNC_ONLINE` (full-mesh
  UDP peers), `SYNC_STRESS` (GekkoNet synctest: continuous rollback +
  re-sim, the determinism gate).
- The session owns the `ArenaState` and is the only `arena_tick` caller;
  rollback (save/load/advance events) is invisible to callers.
- Viewer drives its match through the session: couch by default (feel
  identical to today), `--host <port> --players N` / `--join <ip:port>`
  for online; two viewer processes on localhost = playable rollback.
- Headless two-process localhost match test asserting identical confirmed
  hashes — the A2 exit criterion. Stress session in ctest + CI.

**Non-goals (this milestone)**

- No rendezvous server, NAT punching, host-relay fallback, lobby UI, or
  spectators (A3). No host migration (deferred per design doc). No custom
  UDP adapter (built-in ASIO now; adapter slot is the A3 seam). No
  matchmaking. Sim (`src/arena/`) unchanged — pinned hash `4b6687d4`
  must not move.

## 2. Dependency

- GekkoNet via CMake `FetchContent`, **pinned to a release tag** (chosen at
  implementation time from the latest release; recorded in CMakeLists).
- Built as its own static lib by its own CMake; our targets link it. The
  top-level `project()` gains `CXX` so the C++ runtime links; all OUR code
  stays C11.
- MinGW note: ASIO needs winsock (`ws2_32`) — link it on WIN32.

## 3. Interface (`src/netplay/sync_session.h`)

```c
typedef struct SyncSession SyncSession;
typedef enum { SYNC_COUCH, SYNC_ONLINE, SYNC_STRESS } SyncMode;

typedef struct {
    SyncMode    mode;
    uint8_t     num_players;               /* 2..4 */
    uint8_t     local_mask;                /* bit i set = player i is local */
    uint16_t    local_port;                /* ONLINE: our UDP port */
    const char* peer_addr[ARENA_MAX_PLAYERS]; /* ONLINE: "ip:port" per remote
                                                 player, NULL for locals */
    uint32_t    seed;
    uint8_t     arena_id;
    uint8_t     input_delay;               /* frames, typically 0-2 */
} SyncConfig;

SyncSession*      sync_create(const SyncConfig* cfg);   /* NULL on failure */
void              sync_destroy(SyncSession* s);
/* Pump once per 60Hz tick: feed local inputs, process GekkoNet events
 * (save/load/advance). Returns net new ticks advanced (>=0). */
int               sync_frame(SyncSession* s,
                             const ArenaInput local_inputs[ARENA_MAX_PLAYERS]);
const ArenaState* sync_state(const SyncSession* s);     /* present, render-only */
bool              sync_connected(const SyncSession* s); /* all peers synced */
uint32_t          sync_confirmed_hash(const SyncSession* s);
uint32_t          sync_confirmed_tick(const SyncSession* s);
```

Internals: `ArenaState` + snapshot ring (GekkoNet dictates save slots via
events; full-state save every frame — 944B makes limited-saving pointless,
and keeping it enables GekkoNet desync detection). Event handling:
`Save` → whole-struct copy + `arena_hash` into the event's checksum slot;
`Load` → whole-struct restore; `Advance` → unpack the event's input block
(4 × `ArenaInput`) and `arena_tick`. Confirmed tick/hash tracked from save
events at GekkoNet's confirmed frame. Exact event/struct field names are
taken from the fetched `gekkonet.h` at implementation time.

## 4. Viewer integration

- Viewer creates a session at startup: default `SYNC_COUCH` with all
  connected devices as local players (device mapping unchanged);
  `--host <port> --players N` = ONLINE with local player 0; `--join
  <ip:port> [--player K]` = ONLINE joining a host as player K (default 1).
  Player-index assignment is manual/config-driven in A2 (both sides must
  agree, mismatches fail the sync handshake); automatic slot assignment
  arrives with the A3 lobby. `--frames` smoke mode keeps driving
  the sim directly (no session) — its pinned behavior is untouched.
- Main loop: `vclock` cadence unchanged; each due tick calls
  `sync_frame(session, inputs)` instead of `arena_tick`; rendering reads
  `sync_state(session)`.
- ONLINE disables: pause/step/slow-mo, `R` restarts, Tab player-count
  changes via hotplug (device changes don't re-init the match), and the F2
  sudden-death phase surgery (any of these would desync peers). COUCH
  keeps today's behavior including F2.
- HUD session line: mode, connection state, confirmed tick + hash.

## 5. Testing

1. **Stress gate** (`tests/test_netplay_stress.c`): `SYNC_STRESS` session
   runs the scripted 4P match (same generator as `test_determinism.c`);
   GekkoNet continuously rolls back and re-simulates, flagging any state
   divergence; asserts completion with matching confirmed hash. Registered
   in ctest.
2. **Two-process localhost match** (`tests/test_netplay_p2p.c` +
   `tests/run_p2p_test.sh`): binary takes `--port --peer --player --ticks`;
   scripted deterministic inputs for the local player; runs to N confirmed
   ticks; prints `tick N hash XXXXXXXX`. The script launches two instances
   against each other on 127.0.0.1, waits, asserts identical output lines.
   Registered in ctest (requires bash — present on dev box and CI).
3. **CI**: new `netcode` workflow job (ubuntu + windows runners): cmake +
   ninja build, ctest (includes stress + p2p). Existing raw-gcc determinism
   matrix untouched.
4. Human checkpoint: two viewer windows on localhost, playable match,
   rollback imperceptible at 0 delay loopback.

## 6. Risks / notes

- GekkoNet 4P mesh is less proven than 2P — accepted; A3 adds WAN soak.
  The wrapper interface is the swap seam if a hand-rolled core is ever
  needed (design doc §5).
- ASIO-on-MinGW quirks (winsock link, `_WIN32_WINNT` define) — handled in
  CMake; if the built-in adapter fights MinGW hard, fallback is building
  GekkoNet with MSVC-compatible flags or vendoring a minimal UDP adapter
  (small, documented escape hatch).
- Session pacing: GekkoNet also has internal frame pacing/run-ahead;
  A2 drives it strictly from our 60Hz vclock and disables run-ahead
  features initially — simplest correct integration first.
- Windows Firewall may prompt on first UDP bind — expected, user allows.
