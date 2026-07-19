# Bomberman Hero Multiplayer — project context for Claude Code

Multiplayer (battle arena + later co-op campaign) for Bomberman Hero, built on
BMHeroRecomp (N64Recomp static recompilation + RT64). Read these before any work:

- `docs/bmhero-multiplayer-architecture.md` — overall design: two sim domains,
  determinism model, netcode topology, milestones (arena-first, A0 done)
- `docs/bmhero-battle-arena-design.md` — the mode being built now: ruleset,
  ArenaState spec, tick pipeline, GekkoNet plan, render bridge, host-session model
- `README.md` — this repo's layout, build, and the five invariants

## Current status (2026-07-18)

**A0 complete.** Headless deterministic arena sim in `src/arena/`, all tests
green (`tests/test_determinism.c`): bit-identical replay, GekkoNet-style
rollback stress, snapshot round-trip, liveness. Scripted-match hash pinned at
`4b6687d4` (re-pinned 2026-07-19 with the bomb-mechanics correction —
`TUNE_VERSION` 2, first intentional gameplay change; previously `a55aa9b1`)
— CI matrix on GitHub: https://github.com/dcmshi/bmhero-arena.

**A2 SyncSession complete (2026-07-19).** `src/netplay/` wraps GekkoNet
(FetchContent, pinned tag `v20260629200724-02c447c`, BSD-2) behind one C
interface — couch/online/stress — the session owns `ArenaState` and is the
sole `arena_tick` caller. Gates: GekkoNet stress session (continuous
rollback re-sim, 3600 ticks clean) + two-process localhost match with
matching confirmed hashes (`p2p tick 600 hash bbf9c071`), both in ctest and
the `netcode` CI job (ubuntu+windows). Viewer drives matches through the
session (couch default; `--host <port> --peer <addr>` / `--join <addr>
--port <p>` for 2P online; `--frames` smoke stays sessionless, hash
`eeeb76f6`). Spec: `docs/superpowers/specs/2026-07-19-a2-syncsession-design.md`.

**Bomb mechanics are Hero-authentic (2026-07-19).** Fixed-arc throw (no
stick/momentum modifier — decomp-verified in `bmhero src/code/69AA0.c`:
speed 35 / pitch 80° / facing only; kick = flat launch at speed 30), 4-bomb
spread on ≥2s hold, set via input bit 14 (works mid-air), **walk-in kick**
(run into any settled bomb; setter immune until stepped clear;
`BSTATE_SLIDING`; detonates on first contact — owner-recalled, verify in
A1). Cap 6 live bombs. TDD'd in `tests/test_bomb_mechanics.c`; design doc
§2 records mechanics, sources, and decomp anchors for A1 calibration.

**SDL debug viewer complete (2026-07-19).** `tools/viewer/` (floats OK there;
spec + post-playtest addendum in `docs/superpowers/specs/`): camera modes on
F1 — FOLLOW default (fixed yaw; **verified: Hero's real camera never rotates
with facing**, see design doc §7 note), CHASE, ORBIT (battle-mode preview),
TOP — pause/step/slow-mo, HUD with live hash, checkerboard ground, translucent
walls, F2 sudden-death toggle (viewer-side; sim untouched), `--frames N`
deterministic smoke flag. Pure modules unit-tested (`tests/test_viewer_*.c`).
Keyboard playtested; **gamepad path written but not yet device-tested** (no
pad on hand). Toolchain: MSYS2 UCRT64 (gcc/CMake/Ninja/SDL3), README §Windows.

## Hard invariants — breaking any of these breaks netplay

1. **No floats in `src/arena/`.** Q20.12 fixed-point only (`arena_math.h`),
   int64 intermediates. This is what makes cross-arch online play safe.
2. **Sim reads nothing outside** `ArenaState`, the tick's inputs, and
   `static const` data. No time, no globals, no allocation.
3. **Fixed iteration orders** (players 0..3, bombs 0..15, pairs 01,02,03,12,13,23)
   and fixed tick-phase order (see header comment in `arena_sim.c`).
4. **`ArenaState` layout changes = netcode version bump** — static_asserts
   enforce sizes; bump `TUNE_VERSION` and note it.
5. **Padding stays zeroed** (memset at init, whole-struct copies only) — the
   FNV hash covers all bytes.

Run `gcc -std=c11 -Wall -Wextra -O2 -o t src/arena/arena_sim.c
tests/test_determinism.c && ./t` after every sim change. If the pinned CI hash
changes, that must be an intentional gameplay change.

## Next milestones (in order; docs §9 of arena design)

1. **A1 render bridge + feel** (needs user's ROM + local BMHeroRecomp build,
   fork of https://github.com/RevoSucks/BMHeroRecomp): spawn/puppet `gObjects`
   entries; transcribe every `TODO(feel)` constant in `arena_tuning.h` from the
   decomp (https://github.com/Bomberhackers/bmhero — start at `gPlayerObject`
   usage and the object update dispatch over `gObjects[207]`; Hack64 wiki has
   supplementary RE notes). Constants go only in `arena_tuning.h`.
2. **A3 online hardening** (ROM-free): rendezvous server + lobby codes,
   host-relay fallback via a custom GekkoNet adapter, 4P mesh WAN soak,
   desync surfacing UI, automatic player-slot assignment (arena doc §6).

## Repo plan

This folder becomes `src/arena/` + `tests/` inside a fork of BMHeroRecomp
(decision: fork, not standalone; mod-packaging deferred). Until the fork
exists it builds standalone with the one-line gcc command or CMake. The
recomp is GPL-3.0 — all code here ships GPL-compatible.

## Known intentional simplifications (v1)

No items/powerups (v2, appends ArenaItem[16] to state), no pits in arena 0,
sudden-death = wall shrink only, host migration deferred, tuning values are
placeholders pending A1 feel-matching.
