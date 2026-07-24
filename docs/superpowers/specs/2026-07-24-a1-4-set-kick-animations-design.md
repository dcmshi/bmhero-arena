# A1.4 — set/kick player animations — design spec

**Date:** 2026-07-24 · **Status:** approved (design review in-session)
**Purpose:** player 0 plays the game's own **set-bomb** and **walk-in-kick**
animations when the sim registers those events. The puppet currently performs
none of the game's player action-anims (A1.2c/A1.2e/A1.3 punch-list item).
**Key requirement (user):** verify the animations **automatically via the
boot-soak harness** — no manual eyeballing — by exposing the player's live
anim-instance state as a readable signal, the same way the A1.2e facing work
read `moveAngle`.
**Builds on:** A1.2d anim findings (§8.5b: player 0 animates in-arena via the
player path; `D_80165290` model-anim instances; `func_8001CAAC`/`func_8001AD6C`
advance frames), A1.3 (movement + facing via game `moveAngle`), A1.2f harness
(`tools/arena-soak.ps1`, `ARENA_AUTO_BATTLE` probe mode).
**Fork branch base:** `feature/a1.3-feel` (submodule at A1.3 `ba7c346`,
`TUNE_VERSION` 3, facing = game `moveAngle` copy). Fork-side only.

## 1. Goal and exit criterion

**Exit (objective, harness-verified):** on a set-bomb edge and on a
walk-in-kick edge, player 0's model-anim instance switches to the game's
set/kick anim index AND its frame counter advances — asserted by the harness
in probe mode (inject set, then kick; read the anim-state exports). Green
soak on the build. Player 0 only. Human feel-boot is a final polish confirm,
NOT the verifier. Sim gameplay unchanged (pinned hash `5f500fcb` holds; any
new sim export is read-only and must not alter the hash).

## 2. Approach (settled)

Direct anim-trigger (approach A): on a sim set/kick **edge**, call the game's
own animation-set path (`func_8001C0EC` → `func_8001BE6C`, the model-anim
binder used for the walk/idle anim) on `gPlayerObject` with the set/kick anim
index. Decoupled from position (we still drive Pos from the sim); does not
re-engage the game's player state machine (rejected: it also moves the player,
fighting our sim-driven Pos, and lives in the undecompiled walker).

## 3. Phase 1 — RE the player action anims (decomp-only, gated)

Find, in the fork decomp (`C:\Users\dshi\GitRepos\BMHeroRecomp\lib\bmhero`):
1. The player's **set-bomb** and **kick** animation indices (in the player's
   anim set — start at `code/69AA0.c` bomb/set/kick code and the player's
   `gObjInfo`/`D_80165290` anim table; cross-ref how the walk anim index is
   selected, since player 0 already animates walking).
2. The **trigger call** the game uses to start a player action anim (likely
   `func_8001C0EC(objId, part, animIdx, fileID, animTable)` or
   `func_8001BE6C` with a new index; identify `part`, `fileID`, `animTable`
   for the player).
3. The anim-instance **state fields** to read back for verification:
   `D_80165290[gObjects[0].Unk140[part]]` — the current index (`unk14`/`unk15`)
   and frame time (`unk24`).

Write findings to a new integration-notes subsection (§8.5c). **Decision
gate:** if the set/kick indices or the trigger aren't recoverable, fall back
to what IS recoverable (set-only, or a single generic "action" anim for both)
and document the gap — re-plan with the user before proceeding past the gate.

## 4. Phase 2 — anim-state exports + auto-verify probe

- **Native (`arena_bridge`):** read-only exports of player 0's live anim
  index and frame counter (from `D_80165290` via the slot), e.g.
  `arena_export_player_anim_idx()` / `arena_export_player_anim_frame()`.
- **Probe mode (`main.cpp` `ARENA_AUTO_BATTLE=3` extension):** after in-level
  latch, inject a set press, wait, then walk-in-kick; the render routine logs
  the anim index+frame each frame (gated, temporary) so `arena-soak.ps1`
  (or a decode step) asserts: index → set-anim then → kick-anim, frame counter
  advancing during each. This is the objective gate.

## 5. Phase 3 — trigger on sim edges

- **Sim edges:** expose set/kick edge signals (read-only, like
  `arena_blast_new`) — e.g. `arena_export_set_edge(i)` / `_kick_edge(i)` — 1
  exactly once per event. (Sim-side edge tracking is native/bridge state, not
  a gameplay change; hash unaffected.)
- **Patch (`arena_render.c`):** each frame, if a set/kick edge fired for
  player 0, call the Phase-1 anim trigger. Guard so the anim plays once per
  event and doesn't stomp the walk anim every frame (let it play out, then
  return to locomotion).

## 6. Non-goals (deferred)

- Puppet (players 1–3) animations — they're symmetric bomb-mesh placeholders;
  revisit with the real bomber mesh (A1.2d §8.5b).
- Throw / hold / hurt / death animations — future anim slice.
- Real explosion visual, arena hardening (A1.2g), HUD — separate slices.
- No `bmhero-arena` sim gameplay change (hash `5f500fcb` unchanged).

## 7. Build & testing

- Fork branch `feature/a1.4-set-kick-anims` off `feature/a1.3-feel`. Touches
  `patches/arena_render.c`, `src/arena_bridge/*`, `src/main/main.cpp`; decomp
  reads in `lib/bmhero` (read-only). Any `patches/*.c` edit →
  `make -C patches clean` before the cmake build. Two-build discipline
  (iterate `build-rwdi`, final `build-cmake`); `[nmcheck]` if float libcalls
  are touched.
- **Verification:** Phase-1 zero boots; Phase-2/3 the harness probe is the
  objective gate (anim index switch + frame advance); **every build handed to
  the human is soaked green first** (rule). Human feel-boot only confirms it
  looks right.
- Docs: §8.5c findings on completion; CLAUDE.md status.
