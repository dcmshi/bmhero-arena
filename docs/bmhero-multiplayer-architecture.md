# Bomberman Hero Multiplayer — Networking & Rollback Architecture

**Foundation:** mod/patch layer on [BMHeroRecomp](https://github.com/RevoSucks/BMHeroRecomp) (N64Recomp static recompilation + RT64)
**Modes:** co-op campaign, battle arena · **Play:** local (same machine) and online
**Status:** design v0.1 — 2026-07-18

---

## 1. Context and constraints

BMHeroRecomp is a *static recompilation*, not a source port. Game logic executes as machine-translated MIPS functions operating on an emulated 8MB RDRAM buffer (one contiguous `VirtualAlloc`/`mmap` block — `librecomp/src/recomp.cpp`), with the [bomberhackers/bmhero](https://github.com/Bomberhackers/bmhero) decomp providing symbols and headers used by the patch system. Consequences:

- **We cannot freely refactor game logic.** Changes go through the recomp's patch mechanism: instruction patches (`bmhero.toml`), C function replacements compiled to MIPS (`patches/`), and native mod hooks/events (`librecomp` mod system). This is a real, supported workflow — the repo already ships patched functions in C using decomp headers.
- **Game state is unusually legible for netcode.** Everything the sim touches lives in RDRAM: the fixed object pool `gObjects[207]` (decomp `variables.h`), a single `gPlayerObject` pointer, and a seeded PRNG (`gRandSeed` + `Math_Random`). Bounded, contiguous, inspectable.
- **The hard part is execution state, not data state.** ultramodern maps N64 threads (game, scheduler, audio) onto real host threads. RDRAM holds N64 thread stacks and register contexts, but host-thread call stacks are *not* in RDRAM. A mid-frame snapshot cannot be restored naively. This single fact drives the whole rollback strategy (§4).
- **Rendering is already decoupled.** RT64's high-framerate interpolation is render-only ("changing framerate has no effect on gameplay" — README). The sim ticks at N64 VI cadence (60Hz, game logic effectively 30Hz for much of Hero). Netcode operates on sim ticks; rendering stays untouched.
- **Bomberman Hero has no multiplayer mode.** Both modes are new content. That freedom is exploited below.

## 2. Core architecture: two sim domains

The key decision: **don't use one netcode model for both modes.** Their requirements differ, and the recomp's constraints price them very differently.

| | Co-op campaign | Battle arena |
|---|---|---|
| Sim | Existing recompiled level logic | **New, written natively** as mod code |
| State | 8MB RDRAM (opaque-ish) | Small native struct (~KB), designed for copying |
| Netcode | Delay-based lockstep | GGPO-style rollback |
| Rollback cost | Would need full-sim savestates (hard) | memcpy (trivial) |
| Latency tolerance | High — co-op platforming forgives 3–5 frames of input delay | Low — bomb dodging and knockback demand rollback |

### Domain A — co-op campaign: lockstep on the recompiled sim

Both players run the full game instance. Each sim tick consumes a `(P1 inputs, P2 inputs)` pair; ticks advance only when both are known. Determinism (§3) guarantees identical worlds — no state is ever transmitted, only inputs.

Online, inputs are scheduled `d` frames ahead (input delay, adaptive 2–5 frames ≈ 33–83ms one-way budget). Under jitter the sim briefly stalls rather than mispredicts. This is Milestone 2's honest tradeoff: co-op gets simplicity and zero desync-by-construction, at the cost of feeling like a slightly heavier controller on bad connections.

Second-player injection (the patch work): allocate a second bomberman from the `gObjects` pool at level load; route controller port 2 into its input handler; generalize the handful of systems that assume `gPlayerObject` is singular — camera (follow midpoint, clamp separation, or leash P2 to P1 with off-screen warp, Kirby-style), HUD (second health readout), hit/interaction checks that compare against `gPlayerObject`, respawn/checkpoint rules, and level-exit triggers (require both players / either player, per design choice). This is grind, not risk: the decomp's symbols make each site findable, and the recomp's `strict_patch_mode` validates every patched symbol.

### Domain B — battle arena: native deterministic sim with rollback

The arena mode's rules don't exist in the ROM, so its simulation is written fresh as **native mod code** — and therefore designed for rollback from day one:

- All match state in one POD struct: `ArenaState { players[4], bombs[N], blasts[N], tiles[W*H], items[N], rng, tick }` — fixed-point math, no pointers (indices only), no heap. Target < 64KB. Snapshot = `memcpy`; restore = `memcpy`.
- Deterministic by construction: integer/fixed-point positions and velocities, explicit PRNG in-state, iteration in fixed index order.
- The recompiled game becomes the **presentation layer**: each rendered frame, a bridge writes ArenaState into `gObjects` entries (positions, animation states, model IDs) so Hero's own renderer, models, and effects draw the match. Movement *feel* is achieved by transcribing constants (walk speed, accel, bomb timers, blast radii) from the decomp'd player/bomb code into the native sim — reference, not execution.
- Rollback loop is textbook GGPO: predict remote inputs (repeat-last), sim ahead; on late input mismatch, restore snapshot at divergence tick, re-sim to present (8-frame window, so ≤8 memcpys + re-sims of a tiny sim — microseconds). The bridge only ever renders confirmed-or-predicted *present* state, so the recompiled side never participates in rollback.

This sidesteps the host-thread snapshot problem entirely for the mode that needs rollback, and postpones full-sim savestates to an optional future milestone (§4).

## 3. Determinism (what lockstep and rollback both require)

Same inputs at same ticks ⇒ identical state on every peer. Audit list for the recompiled sim:

- **RNG** — `gRandSeed` is in RDRAM and advances only via `Math_Random`: deterministic if peers start from an agreed seed (host sends seed in match-start handshake) and no code path seeds from wall-clock/frame-count-since-boot. Verify at boot-of-level via patch.
- **Floating point** — recompiled MIPS FP ops map to host IEEE-754 single/double ops. Identical binaries on identical ISAs are bit-identical. **Cross-arch (x86_64 ↔ ARM64) needs empirical validation** — flush-to-zero/NaN-payload edge cases differ. Mitigation if it fails: gate cross-arch matches, or restrict them to the arena mode (fixed-point, always deterministic).
- **Input path** — single choke point: `ultramodern::input::set_callbacks`. The netcode layer replaces the device-poll callback with "return inputs for tick T from the sync queue." Local devices feed the queue for the local player; the network feeds remote slots. Local-only play is the degenerate case (all slots local, zero delay) — **local and online share one code path**, which is how local play stays bug-for-bug identical with online.
- **Frame boundary** — VI retrace (ultramodern `events.cpp`) is the tick edge; input polls are tagged per-frame (`recomp_set_current_frame_poll_id` exists for exactly this). One inputs-set per sim tick, never mid-tick refresh.
- **Time sources** — patch/pin any `osGetTime`/`osGetCount` reads that leak into game logic (they mostly feed profiling; audit confirms).
- **Desync detection** — every K ticks, each peer sends `fnv1a(gObjects) ^ fnv1a(gRandSeed..)` (cheap, targeted ranges rather than 8MB). Mismatch ⇒ surface immediately with tick number; in co-op, offer host-state resync (host streams RDRAM delta) rather than hard abort.

## 4. Snapshots and the full-rollback question

Rollback for Domain B needs only `memcpy(ArenaState)`. Full-sim savestates (which would enable rollback co-op, replays, and spectator join) are harder and deliberately deferred:

- **Data:** RDRAM (8MB + mod RAM) — cheap. Use OS page write-protection for dirty-page tracking so per-tick incremental snapshots copy only touched pages (typically a few hundred KB), not 8MB. libretro-style emulator netplay proves this class of approach.
- **Execution:** host threads must be checkpointed at a *quiescent point* — end of VI tick, when the game/audio/scheduler threads are all parked in `mesgqueue` waits. At quiescence, each host thread's "resume point" is one of a small enumerable set of wait-sites; a savestate records (RDRAM, per-thread N64 register context, which wait-site each thread resumes at, librecomp device state: VI regs, AI buffer cursors, SI/PI pending ops, timer queue). Restoring re-parks threads at their recorded wait-sites. Feasible, non-trivial, and *not on the critical path* for either shipped mode — scheduled as Milestone 5 (stretch).

## 5. Transport, sessions, and topology

- **Transport:** UDP. Inputs are sent unreliably + redundantly (each packet carries the last ~8 ticks of inputs — GGPO's trick; loss only matters if 8 consecutive packets drop). Control/lobby traffic over a lightweight reliable layer (ENet, or yojimbo) on a second channel.
- **Sync core:** adopt **[GekkoNet](https://github.com/HeatXD/GekkoNet)** (C/C++ rollback SDK, GGPO/GGRS-inspired, BSD-2, actively maintained — releases through 2026). It covers both domains out of the box: local/couch sessions with per-player input delay (Domain A + all local play), online rollback with prediction and run-ahead (Domain B), spectator sessions, desync detection, and a "stress session" mode that continuously rolls back locally — exactly the M4 sync-test harness. Wrap it behind a thin `SyncSession` interface so a hand-rolled core stays a fallback. GGPO itself is proven but dormant.
- **Topology:** P2P for 2 players (both modes are 2P-first; arena scales to 4 later — full-mesh P2P input exchange, GGPO-style, still fine at 4). **NAT traversal:** UDP hole-punching via a tiny rendezvous server (session codes, à la "room ID"); fall back to a relay (self-hosted TURN-alike) when punching fails (~10–15% of pairs). Rendezvous/relay is stateless enough to run on one small VPS.
- **Sessions:** join-code lobby in the recomp's RmlUi menu system (it already hosts config menus; mod menus are precedented). Handshake carries: game/patch version hash (hard-gate mismatches — determinism depends on identical binaries), mode, RNG seed, input delay, player slots. Mid-match disconnect: co-op pauses with rejoin window (host keeps authoritative state for resync); arena awards the round after a timeout.
- **Save data:** co-op progress writes to the *host's* save only (guests get a "co-op session" scratch save) — avoids cross-contaminating single-player saves.

## 6. Integration map (where code actually goes)

| Piece | Mechanism | Repo location |
|---|---|---|
| Input rerouting | `ultramodern::input::set_callbacks` wrapper | `src/main/` native |
| Tick gating (stall until inputs known) | VI event / scheduling hook | native, ultramodern hook |
| P2 spawn, camera, HUD, triggers | C function patches w/ decomp headers | `patches/*.c` |
| RNG seed handshake, time-source pinning | patch + native API (`recomp_api.cpp` pattern) | both |
| Arena sim + rollback core | pure native library + unit tests | new `src/netplay/`, `src/arena/` |
| Arena→render bridge | native writes into `gObjects` via decomp structs | native w/ decomp headers |
| Lobby UI | RmlUi menus | `src/main/` |
| Desync checksums | native, reads RDRAM ranges | `src/netplay/` |

## 7. Milestones — **arena-first** (decision 2026-07-18; detailed in `bmhero-battle-arena-design.md`)

1. **A0 — Headless arena sim.** ✅ *Done 2026-07-18 — `bmhero-arena/`: 944B ArenaState, full tick pipeline, replay + rollback-stress + snapshot tests green, CI hash-pin `a55aa9b1`.*
2. **A1 — Arena on screen.** Battle menu entry, map-shell load, render bridge, 1P feel-matched against Hero's campaign movement.
3. **A2 — 4P couch battle.** Pad assignment, shared camera, HUD, full ruleset, 3 arenas.
4. **A3 — Online arena (rollback).** GekkoNet online 4P mesh + rendezvous server + lobby + desync surfacing. Builds the transport/session/lobby stack the campaign will reuse.
5. **B0 — Local co-op campaign.** P2 injection patches: spawn, port-2 input, camera, HUD, triggers.
6. **B1 — Campaign determinism harness.** Dual-instance replay runs, RDRAM checksum comparison; fix RNG/time violations. Gate for B2.
7. **B2 — Online co-op (lockstep).** Delay-based backend on the A3 session stack + host-state resync.
8. **S1 (stretch) — Full-sim savestates.** Quiescent-point checkpointing (§4) → rollback co-op, replays, spectators.

Arena-first front-loads all reusable netcode (transport, sessions, lobby, desync tooling) onto the mode with the smallest integration surface; the campaign track (B0–B2) then only adds patch-layer work. A-track and B0 remain parallelizable (different codebases entirely).

## 8. Risks and open questions

- **Singular-player assumptions run deep** (camera, cutscenes, level scripting all touch `gPlayerObject`). M0 exists to size this early; scope valve: co-op "leash" camera instead of free dual camera, skip co-op in boss/vehicle levels initially (Hero's sled/copter levels may stay 1P-at-a-time or race-style).
- **Cross-arch float determinism** unvalidated — M1 measures it; worst case co-op is same-arch-gated, arena unaffected.
- **Frame pacing vs. lockstep stalls:** RT64 interpolation expects steady sim ticks; stalls will visibly hitch. Adaptive input delay + a 1–2 tick elasticity buffer mitigates.
- **Audio during arena rollback:** none — recompiled side never rolls back; arena SFX triggered edge-wise from confirmed state only.
- **GPL-3.0** (recomp) — netcode/mod layer must ship GPL-compatible; fine for a community project, noted for contributors.
- **Open:** arena ruleset (classic Bomberman grid vs. Hero's free movement?), 4P co-op later?, dedicated-server relay vs. pure P2P at 4 players.

## 9. Precedents leaned on

- **GGPO / GekkoNet** — rollback core design, input redundancy scheme.
- **sm64coopdx** (SM64 decomp co-op) — proof that N64 platformer co-op's real cost is singular-player assumptions (camera/HUD/scripting), not netcode.
- **libretro netplay / RetroArch runahead** — savestate-based rollback on opaque sims; dirty-page snapshotting.
- **Zelda64Recomp mod ecosystem** — patches + native mod hooks on N64Recomp at scale, the exact mechanism this design builds on.

## 10. Other BM Hero code resources (survey 2026-07-18)

No other source port or reimplementation exists; the recomp + decomp are the code base. Useful satellites: [Hack64 BM Hero hacking docs](https://hack64.net/wiki/doku.php?id=bomberman_hero) (data-format RE notes), [BMHeroSyms](https://github.com/RevoSucks/BMHeroSyms) (symbol DB, already a submodule), a community [Archipelago randomizer integration](https://github.com/Happyhappyism/Archipelago/releases?q=%22Bomberman+Hero%22&expanded=true) (known memory addresses for game state), [Models](https://models.spriters-resource.com/nintendo_64/bombhero/)/[Textures Resource](https://textures.spriters-resource.com/nintendo_64/bombhero/) rips (design reference only; ship assets ROM-loaded), and [BM64Recomp](https://github.com/RevoSucks/BM64Recomp) (Bomberman 64, which *has* a battle mode, same recomp framework — reference implementation candidate).
