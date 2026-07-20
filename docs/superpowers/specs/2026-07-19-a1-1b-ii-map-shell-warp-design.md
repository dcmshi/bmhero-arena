# A1.1b-ii — warp into a battle-room map shell — design spec

**Date:** 2026-07-19 · **Status:** approved (design review in-session)
**Purpose:** on Battle launch, load a dedicated arena map
(`MAP_BATTLE_ROOM`) as the visual shell instead of the campaign start.
Builds on A1.1b (native battle-mode flag). The higher-risk level-load RE
deliberately isolated from A1.1b. Slice of A1.1 (A1.1a proof → A1.1b menu →
**A1.1b-ii warp** → A1.1c spawn suppression → A1.2 render bridge → A1.3
feel). A3 online hardening follows A1.
**Companion:** A1.1a/A1.1b specs + plans; `CLAUDE.md` A1.1a/b status; memory
`recomp-build-toolchain`.

## 1. Goal and exit criterion

When battle-mode is set (A1.1b flag), the game loads a chosen arena map
instead of the campaign start; non-battle launches are unchanged.

**Exit criterion (human boot gate):** click **Battle** → the game loads
into the Battle Room map (visibly not Bomber Base Entrance), with
`arena_bridge.log` still showing `[arena] BATTLE MODE tick N …`; click
**Start Game** → normal campaign start. If the dedicated battle room does
not load cleanly when entered directly, the fallback (one-constant change to
a known-good open campaign level) still lands Battle in a deliberate,
non-campaign-start map.

## 2. Mechanism

The boot sets `gCurrentLevel`, then the level runner loads
`gLevelInfo[gCurrentLevel]`. We override `gCurrentLevel` to a warp-map
constant when battle-mode is set. Two pieces:

- **Native → patch bridge:** add
  `extern "C" int arena_bridge_is_battle(void)` to `arena_bridge`
  (returns the flag), registered as a base export so a MIPS patch can call
  it — the same patch↔native direction `recomp_puts` uses (`DECLARE_FUNC`
  in the patch, native registration in `main.cpp`'s export list).
- **A small `RECOMP_PATCH`** (new `patches/arena_warp.c`) at the level-entry
  point: reads `arena_bridge_is_battle()` and, when set, forces
  `gCurrentLevel = ARENA_WARP_MAP` before the map data loads.

**The milestone's core RE is finding the right small function to patch.**
`func_80083180` (the level runner, `code/71AA0.c:960`) is a huge irreducible
function — NOT a viable whole-function `RECOMP_PATCH` target. The plan's
first task locates a compact level-entry function that reads
`gLevelInfo[gCurrentLevel]` and can be cleanly replaced with a one-line
override prepended. Candidates to evaluate in the plan, smallest-first:
the level-setup function in `code/56800.c` (reads `gLevelInfo[gCurrentLevel]`
around line 362+), or `func_80082E38` (called at `func_80083180` entry when
`arg0 != 0`). If no compact target exists, the plan surfaces it before
guessing — a giant-function patch is explicitly out of bounds.

## 3. Warp target

`#define ARENA_WARP_MAP MAP_BATTLE_ROOM` in one place (the patch file),
trivially changed to any map id (`map_ids.h`) during runtime testing. This
is the verify-first + fallback lever: try `MAP_BATTLE_ROOM`, and if it does
not load cleanly, change the one constant to a known-good open campaign
level.

## 4. Data flow

Menu sets battle flag (A1.1b) → level-entry patch reads it via the native
export → overrides `gCurrentLevel` → level runner loads
`gLevelInfo[ARENA_WARP_MAP]`. One-way, additive to A1.1a/b. The override is
conditional on the flag every load, so non-battle launches are untouched.

## 5. Non-goals (deferred, with owner)

- The shell map's own gameplay/objects (enemies, campaign player behavior,
  HUD) → **A1.1c** spawn suppression.
- Drawing our arena entities into the map → **A1.2** render bridge.
- Returning to the menu after a battle, round flow, arena selection,
  multiple arenas → later.
- No changes to `bmhero-arena` sim code (pinned hash `4b6687d4` unmoved).

## 6. Build & integration

- Fork branch `feature/a1.1b-ii-map-warp` off `master` (has A1.1a/b).
- Touches: `src/arena_bridge/arena_bridge.{h,cpp}` (the `is_battle` export),
  `src/main/main.cpp` (register the export), and a new `patches/arena_warp.c`
  (the MIPS override) + its registration in the patches build. This is the
  **first `patches/` change** — it goes through the LLVM-15 MIPS toolchain
  (memory `recomp-build-toolchain`); the patch-recompile step
  (`N64Recomp patches.toml`) runs as part of the build.
- `bmhero-arena` repo gets only a CLAUDE.md status update.

## 7. Testing / risk

- Build-exit gate (native + patches compile; `patches.elf` links; the
  patch-recompile step succeeds).
- **Human boot gate** is the exit criterion — an agent cannot see the map;
  the user clicks Battle and reports which map loaded.
- Risks, both with concrete fallbacks:
  1. **Patch target** — mitigated by targeting a *small* level-entry
     function (§2), plan-verified before implementing; the giant runner is
     out of bounds.
  2. **Battle-room loadability** — `MAP_BATTLE_ROOM` may need multiplayer
     setup state; mitigated by the one-constant fallback to a campaign level
     (§3). If a battle room black-screens/crashes on direct entry, switch
     the constant and re-verify rather than reverse-engineering multiplayer
     setup this milestone.
