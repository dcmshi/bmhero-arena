# A1.1a — arena submodule + proof-of-life in the recomp — design spec

**Date:** 2026-07-19 · **Status:** approved (design review in-session)
**Purpose:** prove the native arena sim compiles into the recompiled binary
and ticks inside its frame loop — the first integration touching the fork.
Thin vertical slice of A1.1 (A1.1a proof → A1.1b Battle menu → A1.1c spawn
suppression), which is itself part of A1 (A1.0 build/boot done → A1.1
scaffold → A1.2 render bridge → A1.3 feel). A3 online hardening follows A1.
**Companion:** `docs/bmhero-multiplayer-architecture.md` §2 (Domain B is
native mod code), `CLAUDE.md` A1.0 status (build recipe + submodule
decision), memory `recomp-build-toolchain`.

## 1. Goal and exit criterion

Prove our native sim runs inside the recomp process's frame loop — nothing
more. No menu, no rendering, no gameplay coupling.

**Exit criterion:** launch the fork's `BMHeroRecompiled.exe`, load any
campaign level from the normal menu, and observe `arena tick N hash H` in
the recomp's console/log ~once per second, N advancing monotonically and the
hash sequence identical across two runs (determinism holds inside the host).

## 2. Non-goals (deferred, with the milestone that owns each)

- Battle menu entry, and auto-warping into a map on boot → **A1.1b** (the
  warp is the menu's payoff; manual level-load proves the hook here).
- Any on-screen drawing / reading game state → **A1.2** render bridge.
- Suppressing the campaign level's own object spawns → **A1.1c**.
- Feeding real inputs / running a real match → later. A1.1a ticks with
  neutral inputs; the sim is a silent passenger.
- No changes to `bmhero-arena` sim code (pinned hash `4b6687d4` unmoved).

## 3. Three-layer structure

Each layer has one responsibility and a clean boundary.

- **Submodule (pure sim):** add `bmhero-arena` as a git submodule at
  `lib/bmhero-arena` in the fork (`.gitmodules` committed). The fork only
  *reads* it — canonical repo stays untouched. The fork's CMake adds a
  static-lib target compiling `lib/bmhero-arena/src/arena/arena_sim.c`
  (C11; builds under the fork's clang-cl — the no-float sim is
  compiler-agnostic). Pinned to a specific bmhero-arena commit.
- **Bridge (fork-native glue):** new `src/arena_bridge/arena_bridge.{h,cpp}`
  in the fork. Exposes `extern "C" void arena_bridge_frame(void)` which owns
  a `static ArenaState`, calls `arena_init(&s, 0, 4, SEED)` once (lazy, on
  first call), then `arena_tick(&s, neutral_inputs)` each call, and logs
  `arena tick <tick> hash <hash>` via the recomp's logging every 60 calls.
  Fork-specific integration code — never in the submodule. Registered so the
  patch can call it (base export, per `recomp_data_api.cpp`
  `register_base_export` pattern).
- **Patch (frame driver):** new `patches/arena_hook.c` with a `RECOMP_PATCH`
  (or hook) on a per-frame in-game update function, calling
  `arena_bridge_frame()` via the export bridge — the same patch↔native
  pattern as `patches/teleporter_obj.c` + `src/game/recomp_api.cpp`. The
  target function is identified at plan/implementation time (see §6).

## 4. Data flow

Game per-frame function → patch → `arena_bridge_frame()` → native
`arena_tick` + console log. One-way; reads no game state, writes no game
state, renders nothing.

## 5. Build & integration

- Clone/update the fork with `--recurse-submodules`; commit `.gitmodules`.
- Same three-toolchain recipe as A1.0 (memory `recomp-build-toolchain`):
  LLVM 15 for `patches/`, VS clang-cl for native, MSYS2 for make + N64Recomp
  DLLs, composed PATH order preserved.
- Submodule → new native static-lib CMake target linked into the game
  binary. Bridge → existing native build. Patch → `patches.elf` via LLVM 15.
- All work on a fork feature branch (`feature/a1.1a-arena-proof`); the
  submodule bump + integration are the fork's first real commits. The
  `bmhero-arena` repo gets only doc updates (CLAUDE.md status).

## 6. Risk / open item

The one unknown: choosing the per-frame function to patch — it must run once
per frame during gameplay. Resolution order at implementation time:
1. The main-loop / object-update dispatch over `gObjects` (the path the arena
   design already cites via `gObjects[207]`), found in `BMHeroSyms/dump.toml`
   + the decomp.
2. Fallback: hook a function we can already see runs per-frame in existing
   patches (e.g. the camera/HUD update touched by `patches/hud.c` /
   `teleporter_obj.c`).
Verification that the choice is right IS the exit criterion (log fires once
per frame during a level). If no clean single per-frame site exists, the
plan surfaces it before proceeding rather than guessing.

## 7. Testing

Integration/boot verification, not unit tests (the sim is already unit-
tested in its own repo; this milestone adds no sim logic):
1. Fork builds clean with the submodule + bridge + patch (build-exit gate).
2. Human boot: load a level, confirm the console log fires ~1/s with
   advancing tick.
3. Determinism-inside-host: two runs produce the same hash sequence for the
   same elapsed ticks (neutral inputs ⇒ fully reproducible).
