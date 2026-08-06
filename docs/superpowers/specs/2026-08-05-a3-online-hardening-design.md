# A3 — online hardening (ROM-free) — design

**Date:** 2026-08-05
**Status:** approved in-conversation (five sections, approved whole); this
document is the reviewable record.
**Scope:** canonical repo (`bmhero-arena`) only. The fork is untouched this
milestone; the sim is untouched (pinned hash `fbdb0d08`, `TUNE_VERSION` 21
must not move — nothing here needs sim changes).

## Problem

A2 proved rollback works: `SyncSession` wraps GekkoNet, and two processes on
loopback converge (`tests/test_netplay_p2p.c`). But nothing exists for real
online play: no way to find peers (players would hand-edit `ip:port` strings),
no NAT traversal (the default adapter binds its own socket, so hole punching
is structurally impossible), no fallback when punching fails, no 4-player
soak under real network conditions, and a desync sets a flag that nothing
surfaces or captures.

## User decisions (2026-08-05)

- **Rendezvous:** ONE standalone binary in this repo (C11, same toolchain),
  deployable anywhere — a VPS, a friend's port-forwarded box, or the match
  host's own machine. The lobby code encodes where it runs; deployment is a
  choice, not an architecture decision.
- **WAN soak:** seeded impairment shim at the adapter layer so 4 loopback
  processes soak under simulated WAN in CI, deterministically. One manual
  real-WAN session (viewer + second machine) is the human checkpoint.
- **Desync policy:** hard stop + repro bundle. A desync is a determinism bug;
  freeze the match, dump evidence, feed `trace-diff`-style tooling.
- **Deliverable boundary:** canonical repo only. Fork wiring (in-game lobby,
  remote slots driving puppets) is the next slice, after the netcode is
  proven headless.
- **Integration approach:** custom `GekkoNetAdapter` owning one socket per
  client (registration, punching, game traffic, relay all on the same socket
  — the only way the NAT mapping the server observed is the mapping game
  packets use). Rejected: default-adapter-plus-plumbing (worse punch rates,
  TURN-style port-per-pair relay is more server complexity), and libjuice/ICE
  (wrong weight class, still wants STUN/TURN infra).

## Non-goals (v1)

- Host migration, mid-match rejoin, spectators (design docs already defer).
- Mid-match route changes: direct-vs-relay is fixed at lobby time; a dead
  pair mid-match is a GekkoNet disconnect, handled by the existing event path.
- Matchmaking for strangers, DNS names in lobby codes, IPv6, crypto/auth —
  codes are bearer tokens among friends; documented, not engineered around.
- Host-state resync on desync (v2 candidate once desyncs are provably rare).
- Any fork-side code. The slot contract (§G) is *recorded* now, implemented
  in the fork slice.

## A — components

All C11, no new dependencies:

| unit | responsibility |
|---|---|
| `src/lobby/lobby_proto.h/.c` | shared wire format: fixed-size packet structs, pack/unpack, lobby-code encode/decode, `arena_net_version()`. No allocation; unpack is bounds-checked and fuzz-safe. |
| `src/lobby/rendezvous_main.c` → `arena_rendezvous` | the deploy-anywhere binary: single-threaded poll loop, one UDP socket (default port 40064, `--port` flag), fixed session table (64 sessions × 4 peers, static arrays, no heap growth after start), peer introduction, pair-route arbitration, relay forwarding, idle expiry. |
| `src/netplay/net_adapter.c` | the custom `GekkoNetAdapter`: one socket; per-peer route table (direct endpoint or relay-wrapped); relay wrap/unwrap; seeded impairment hook (delay/jitter/loss/reorder queues) used by tests. |
| `src/netplay/lobby_client.c` | non-blocking bootstrap state machine, pumped by the caller (`lobby_poll()`, no threads): host/join → introductions → punch → route decision → start config → yields a `LobbyResult` consumed by `sync_create`. |
| `tools/replay_bundle.c` | replays a desync bundle headless, prints per-tick hashes; given two peers' bundles, names the first diverging tick and field (reuses `arena_trace`'s field-diff logic). |
| viewer `--host` / `--join CODE` | the human-visible client for the manual real-WAN checkpoint (the viewer already drives everything through `SyncSession`). |

## B — wire protocol

UDP, little-endian fixed-size structs, magic + protocol version in every
header. Client→server requests are fire-and-retry (500ms interval, up to 10)
and responses are idempotent.

Messages:

- `HOST_REQ {net_version, pad}` → `HOST_RESP {session_id}` — request padded
  so the response is never larger (no amplification). The **host client**
  composes the lobby code from the server endpoint it dialed plus the
  returned `session_id` — the server never needs to know its own public IP
  (it often can't, reliably).
- `JOIN_REQ {session_id, net_version, private_endpoint, pad}` →
  `JOIN_RESP {slot, peers_so_far}` or `REJECT {reason}` with named reasons:
  `BAD_SESSION`, `FULL`, `VERSION_MISMATCH`, `EXPIRED`. The joiner decodes
  the code locally (server address + session_id); the code itself never
  travels.
- `PEER_INTRO {slot, public_endpoint, private_endpoint}` — fanned to all
  session members on each join. Private endpoints let two peers behind the
  same NAT connect over the LAN.
- `PUNCH {session_id, from_slot, nonce}` / `PUNCH_ACK` (echoes the nonce) —
  direct peer↔peer.
- `PUNCH_REPORT {pair, ok}` → server; `PAIR_ROUTE {pair, DIRECT|RELAY}` —
  the **server** declares each pair's route so both sides always agree.
- `RELAY {session_id, from_slot, to_slot, payload}` — client→server→client;
  the adapter unwraps. Server forwards only between registered members of
  the named session, source-address-checked.
- `START {seed, arena_id, num_players, input_delay}` — host-authored, server
  fans out repeatedly until every peer sends `START_ACK`. (Slots are already
  assigned at join time via `JOIN_RESP`; host = slot 0.)
- `KEEPALIVE` — every 10s from registered clients pre-match. In-match, game
  traffic itself keeps NAT mappings alive; only relay pairs still touch the
  server.

**Version gate:** `arena_net_version()` = fnv1a over (protocol version,
`TUNE_VERSION`, `sizeof(ArenaState)`, the pinned scripted match's final-state
hash — replayed once at client startup, ~0.3s; the tuning "table" is
`#define`s, so behavioral coverage via the scripted match is the only hash
that also catches `-DTUNE_*` override builds). Carried in
`HOST_REQ`/`JOIN_REQ`; mismatch → `REJECT{VERSION_MISMATCH}`. This enforces
"peers must match BOTH the tune version and the layout" at the front door.

**Session lifetime:** a session in LOBBY expires after 10 idle minutes (code
dies with it). After START, the server drops sessions with no relay pairs
after 30s; sessions with relay pairs live while relay traffic flows and
expire after 60s of relay silence.

**Abuse resistance (cheap, principled):** magic+version check drops noise;
per-IP token-bucket rate limit on non-relay traffic (20 packets/s, burst
40 — relay traffic is limited instead by session membership); padded
requests (above); relay forwarding restricted to session members. Nothing
else in v1.

## C — lobby code

Self-contained: base32-Crockford (no ambiguous characters) over
`{server_ipv4 4B, port 2B, session_id 2B, crc8 1B}` = 9 bytes → 15 chars,
displayed grouped `XXXXX-XXXXX-XXXXX`. Paste one string, zero client
config; the crc8 catches typos before any packet is sent. v1 limitation,
documented: the server needs a public IPv4 (no DNS names in the code).

## D — connection flow (NAT strategy + fallback threshold)

After `PEER_INTRO`, each pair sends `PUNCH` probes to the other's private
endpoint then public endpoint, alternating, every 250ms for up to 3s. First
`PUNCH_ACK` wins (private preferred — that's the LAN path). Both sides send
`PUNCH_REPORT`; if either reports failure at 3s, the server declares the
pair `RELAY` via `PAIR_ROUTE`. **The server decides, so the two sides can
never disagree about their route.** Routes are fixed for the match.

The whole bootstrap (register/join → punch → routes → START acked) has one
overall timeout, 30s: past it, `lobby_client` reports failure naming the
stage it died in (`JOINING`, `PUNCHING`, `AWAITING_START`, …) rather than
hanging — that string is the player-facing error.

## E — SyncSession changes

- `SyncConfig` gains `const GekkoNetAdapter* adapter;` — `NULL` keeps
  `gekko_default_adapter`, so every existing test and the p2p harness run
  unchanged. With a custom adapter, remote-actor addresses are one-byte slot
  indices the adapter maps through its route table (the `ip:port` strings
  are a default-adapter convention, not a GekkoNet requirement).
- The session records confirmed inputs per tick per player into a buffer
  preallocated at `sync_create` (~10 min × 60Hz × 4 × `sizeof(ArenaInput)` ≈
  288KB). At capacity, recording stops (a bundle then covers the first
  10 minutes; the hash ring still covers the recent past). The netplay layer
  may allocate at create; the sim still never does.
- Desync detection cadence: GekkoNet's built-in detector (already enabled —
  `desync_detection = true` with full-frame saving) is the trigger; the
  existing 256-entry hash ring is the forensic backtrail. No additional
  hash-exchange channel in v1 — the architecture doc's "exchange fnv1a every
  K ticks" intent is satisfied by GekkoNet's checksum exchange plus the ring.

## F — desync surfacing (hard stop + bundle)

On `GekkoDesyncDetected`: the session latches `desynced` (already does),
the client loop **stops feeding frames** (match over, surfaced to the
caller with tick + hashes), and the session writes
`desync_<session>_p<slot>_t<tick>.bin`:

- header: bundle magic/version, `arena_net_version()`, seed, arena_id,
  num_players, local_slot, detection tick;
- the 256-entry `{tick, hash}` ring;
- the local `ArenaState` snapshot at detection (944B, post-divergence but
  cheap forensic material);
- the full confirmed-input history.

`tools/replay_bundle` re-simulates the inputs headless and prints the hash
timeline; run on two peers' bundles it names the first diverging tick and
moved field. That output is the deliverable of a desync, the way
`trace-diff.ps1` is for tuning changes.

## G — slot contract (recorded now, fork slice later)

Host = slot 0; guests take the lowest free slot in join order. `LobbyResult`
exposes `local_slot` and `num_players`. The fork's bridge currently
hardcodes "local player = sim player 0"; the fork slice must parameterize it:
local = `local_slot`, puppets = the other slots, camera follows local. This
paragraph is that slice's requirements pointer — nothing in A3 implements it.

## H — testing and exit criteria

- `tests/test_lobby_proto.c` — codec roundtrips, code encode/decode + crc
  rejection, version-gate rejection, truncated/fuzzed-packet safety. Joins
  ctest and `gate.ps1`.
- `tests/test_netplay_mesh.c` — `arena_rendezvous` + 4 client processes on
  loopback, scripted inputs, asserts identical hash@tick across all four
  (extends the `run_p2p_test.sh` pattern). Three variants:
  1. all-direct (loopback always punches);
  2. forced-relay — a test flag drops punch probes, so every pair takes the
     relay path;
  3. impaired — the adapter's seeded shim at the `wan100` profile.
- `tools/net-soak.ps1 -Profile <name> [-Minutes N]` — repeated mesh matches
  reporting rollback-depth histogram, stall count, desync count, and relay
  added-RTT. Profiles: `lan0` (0ms/0%), `wan100` (100ms ± 20ms jitter, 1%
  loss, 1% reorder — a delivered packet swaps with its successor),
  `rough200` (200ms ± 50ms, 3% loss — informational only).
- **Numeric exit criteria at `wan100`:** p95 prediction depth ≤ 8 (the
  window), stalls < 1/min (a stall = a pumped frame where the session
  cannot advance because the prediction window is exhausted), desyncs = 0,
  all-direct and forced-relay variants both green.
- **Falsifiability:** `ARENA_DESYNC_INJECT=<tick>` corrupts one peer's state
  on purpose — must fire the detector, write bundles on all peers, and
  `replay_bundle` must localize the injected tick. Run once per CI netplay
  job; a desync path that has never fired is a gate that cannot fail.
- Manual checkpoint: viewer `--host`/`--join` over a real WAN with a second
  machine — the human-boot equivalent for this milestone.

## Rollout

1. Protocol + rendezvous + unit tests (server testable against a scripted
   client before any GekkoNet involvement).
2. Adapter + lobby client + `SyncConfig.adapter`; mesh test variant 1.
3. Relay path + forced-relay variant; impairment shim + impaired variant.
4. Desync bundle + `replay_bundle` + injection falsifiability check.
5. `net-soak.ps1`, CI wiring, viewer flags; manual real-WAN checkpoint last.
