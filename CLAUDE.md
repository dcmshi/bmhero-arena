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
`a55aa9b1` (gcc -O0/-O2 verified locally; `.github/workflows/determinism.yml`
extends to clang/MSVC-runner/ARM64 on push).

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

1. **SDL debug viewer** (small, ROM-free): top-down flat-shape renderer +
   gamepad input over the sim, for feel iteration. Keep it out of `src/arena/`
   (viewer may use floats; the sim may not).
2. **A2 netcode: GekkoNet `SyncSession` wrapper** (ROM-free): couch/online/
   stress configs behind one interface; two-process localhost match as the test.
   GekkoNet: https://github.com/HeatXD/GekkoNet (BSD-2). Host = session
   authority, full-mesh inputs, host-relay fallback (arena doc §6).
3. **A1 render bridge + feel** (needs user's ROM + local BMHeroRecomp build,
   fork of https://github.com/RevoSucks/BMHeroRecomp): spawn/puppet `gObjects`
   entries; transcribe every `TODO(feel)` constant in `arena_tuning.h` from the
   decomp (https://github.com/Bomberhackers/bmhero — start at `gPlayerObject`
   usage and the object update dispatch over `gObjects[207]`; Hack64 wiki has
   supplementary RE notes). Constants go only in `arena_tuning.h`.

## Repo plan

This folder becomes `src/arena/` + `tests/` inside a fork of BMHeroRecomp
(decision: fork, not standalone; mod-packaging deferred). Until the fork
exists it builds standalone with the one-line gcc command or CMake. The
recomp is GPL-3.0 — all code here ships GPL-compatible.

## Known intentional simplifications (v1)

No items/powerups (v2, appends ArenaItem[16] to state), no pits in arena 0,
sudden-death = wall shrink only, host migration deferred, tuning values are
placeholders pending A1 feel-matching.
