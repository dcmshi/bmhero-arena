# A1.2a — puppet the player object from ArenaState — design spec

**Date:** 2026-07-20 · **Status:** approved (design review in-session)
**Purpose:** first render-bridge slice — each game frame, drive the campaign
player object (`gObjects[0]`) in the Battle Room from our fixed-point sim so
Hero's renderer draws a bomberman moving per `ArenaState`. First time our
simulated match appears on screen. Slice of A1.2 (A1.2a puppet one → A1.2b
spawn+puppet 4 → A1.2c bombs/blasts → anim/HUD/camera later).
**Builds on:** A1.1a (sim ticks natively via VI callback), A1.1b (battle
flag), A1.1b-ii (Battle warps into `MAP_BATTLE_ROOM`).
**Companion:** arena design §7 (render bridge), architecture §2 (Domain B),
memory `recomp-build-toolchain`.

## 1. Goal and exit criterion

In the Battle Room, the on-screen Bomberman moves according to our Q20.12
sim, rendered by Hero — no object spawning (reuse `gObjects[0]`).

**Exit criterion (human boot gate):** click Battle → in the Battle Room,
moving the stick moves the bomberman per `ArenaState` player 0 (our
fixed-point run/turn physics), drawn by Hero. Non-battle launches unchanged.

## 2. Mechanism (patch-driven, in-frame)

A `RECOMP_PATCH` on a per-frame in-level function performs, in order, each
frame while battle mode is set:
1. **Read input:** pack the game's current controller state (stick X/Y,
   jump + bomb buttons) into an `ArenaInput` via `arena_input_pack`.
2. **Tick the sim:** call native export `arena_bridge_tick_input(u16 packed)`
   — exactly one deterministic `arena_tick` with that input on player 0
   (others neutral). In battle mode this drives the tick at game-frame
   cadence; A1.1a's free-running VI-callback tick is gated off in battle
   mode (kept for non-battle proof-of-life) so the sim ticks once per frame,
   in sync with rendering.
3. **Write the puppet:** call native export
   `arena_get_player_render(int i, float* x, float* y, float* z, float* yaw_deg)`
   (maps `ArenaState` player i → Hero coords) and write `gObjects[0].Pos.{x,y,z}`
   and `gObjects[0].Rot.y`. Placed to run after the game's own player update
   so our write wins.

Native exports go through the `syms.ld` `0x8F...` import table + `REGISTER_FUNC`
pattern established in A1.1b-ii; the patch imports them with `DECLARE_FUNC`.

## 3. Coordinate mapping

`ArenaState` positions are Q20.12 (arena ≈ ±6 units); Hero uses floats in the
Battle Room's own scale. The native mapper computes
`world_axis = (q20_12 / 4096) * SCALE + ORIGIN_axis` per axis, and
`yaw_deg = binang * (360/65536)`. `SCALE` and `ORIGIN` are calibration
constants (`TODO(feel)`) in the bridge; the plan seeds a first guess and they
are tuned on-screen. Y (height) maps the sim's jump height similarly; ground
is `ORIGIN_y`.

## 4. Data flow

Controller → patch packs `ArenaInput` → `arena_bridge_tick_input` →
`ArenaState` advances (deterministic) → `arena_get_player_render` maps →
patch writes `gObjects[0]`. One-way; the sim never reads game state.

## 5. Non-goals (deferred, with owner)

- Spawning the other 3 bombers → **A1.2b**.
- Bombs and blasts rendering → **A1.2c**.
- Animation selection (run/jump/tumble → `actionState`, `anim_hint`) →
  fast-follow; A1.2a writes position + facing only (whatever idle/anim the
  object has is acceptable).
- Player model/color per index → A1.2b (one puppet here uses the existing
  player model).
- Hiding Hero's HUD, camera framing for the arena → later.
- Fully suppressing Hero's own player physics → only if the overwrite
  visibly fights (see §6); default is overwrite-after-update.
- No changes to `bmhero-arena` sim code (pinned hash `4b6687d4`).

## 6. Risks / open items

Both are plan-discovery with concrete fallbacks:
1. **Per-frame in-level patch point** that runs after the player update —
   found by tracing the in-level update path (same method A1.1b-ii used to
   find `func_80081C50`). If the chosen site runs before the player update
   (our write gets clobbered), move to a later-running site; the on-screen
   result (does the puppet track the stick or jitter/lag) is the signal.
2. **Coord scale/origin** — pure calibration, iterated on-screen; a wrong
   scale shows as movement too large/small, a wrong origin as an offset.
3. **Overwrite fights Hero physics** (jitter/rubber-banding because Hero
   also moves `gObjects[0]` from the same stick) — fallback: gate Hero's
   player-update for `gObjects[0]` in battle mode (a second small patch)
   so only our sim drives it.

## 7. Build & testing

- Fork branch `feature/a1.2a-puppet-player` off `master`.
- Touches: `src/arena_bridge/*` (new exports + coord mapper),
  `src/main/main.cpp` (register exports + gate the VI tick in battle),
  `patches/arena_render.c` (new; the per-frame puppet patch),
  `patches/syms.ld` (new import addresses). `patches/` change → LLVM-15 MIPS
  path (memory `recomp-build-toolchain`).
- `bmhero-arena` repo: CLAUDE.md status update only.
- Verification: build-exit gate + **human boot gate** (only a human can see
  the bomberman move) — move the stick, confirm the on-screen bomberman
  follows our sim.
