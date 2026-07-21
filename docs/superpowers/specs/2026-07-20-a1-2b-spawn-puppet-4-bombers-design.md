# A1.2b — spawn + puppet 4 bombers — design spec

**Date:** 2026-07-20 · **Status:** approved (design review in-session)
**Purpose:** second render-bridge slice — spawn 3 additional bomberman objects
in the Battle Room and puppet all 4 (players 0–3) from our fixed-point sim, so
Hero draws a full 4-way match laid out correctly and the bombers are
distinguishable by color. First time the whole simulated roster appears on
screen. Slice of A1.2 (A1.2a puppet one ✅ → **A1.2b spawn+puppet 4** → A1.2c
bombs/blasts → anim/HUD/camera later).
**Builds on:** A1.2a (player 0 puppeted from sim via per-frame displacement),
A1.1b-ii (Battle warps into `MAP_BATTLE_ROOM`), A1.1a (sim ticks natively).
**Companion:** arena design §7 (render bridge), integration notes §3 (objects &
spawning), §6 (coord mapping), memory `recomp-build-toolchain`.

## 1. Goal and exit criterion

In the Battle Room, all 4 `ArenaState` players are drawn as bomberman objects
moving under our Q20.12 physics, laid out at the sim's four corners, and
distinguishable by color (P0 white / P1 red / P2 blue / P3 black).

**Exit criterion (human boot gate):** click Battle → in the Battle Room, four
bombers are visible in a square; the stick drives player 0 (as A1.2a) and the
other three sit at their sim positions relative to it. The four are visually
tell-apart-able. Non-battle launches unchanged; `bmhero-arena` sim untouched
(hash `4b6687d4`).

## 2. Positioning — anchor to the live player

A1.2a puppets player 0 = the campaign `gPlayerObject` by adding the sim's
per-frame **displacement** (dx/dz) to its live `Pos`, leaving Y to the game so
it stays grounded and the camera follows. A1.2b keeps that for player 0 and
positions the other three **relative to that same live anchor**:

    obj[i].Pos.x = gPlayerObject->Pos.x + (sim_pos_i.x - sim_pos_0.x) * scale
    obj[i].Pos.z = gPlayerObject->Pos.z + (sim_pos_i.z - sim_pos_0.z) * scale
    obj[i].Pos.y = gPlayerObject->Pos.y            /* same floor as player 0 */
    obj[i].Rot.y = sim_yaw_i in degrees

Because the offset is recomputed from absolute sim positions every frame, it is
**self-correcting** (no accumulated drift) and needs **no absolute-origin
constant** — which is exactly what blue-screened in A1.2a when it tried to
teleport to absolute world coords. The layout comes straight from the sim's
spawn corners (`arena_geom.h`: ±4.5 units). `scale` reuses A1.2a's value
(≈120 Hero units per sim unit; `TODO(feel)`).

The delta-anchor for player 0 is unchanged, so player 0 keeps its grounded,
camera-followed behaviour and the other three are pinned to it.

## 3. Spawn — the real unknown, proven incrementally

3 extra objects go into free slots `gObjects[2..5]` (integration notes §3:
`Get_InactiveObject` scans exactly slots 2–5 → enough for 3 extras) via the
game's own spawn routine `func_80027464(slot, ObjSpawnInfo*, x, y, z, rotY)`
(or `func_80027C00`), called once from the patch on battle level-enter.

**Risk surfaced in research:** spawning `OBJ_PLAYER` (id 0) copies would create
4 player *controllers*, whose `behaviour` may fight our per-frame puppet write
or assume a single player and crash. So the spike (step 1 below) spawns **one**
extra object and observes whether its `behaviour` interferes. We overwrite the
object's transform every frame regardless; if the behaviour still fights
(jitter / self-movement / crash), we either (a) pick a bomber model with benign
behaviour, or (b) neutralize the spawned object's behaviour pointer / action
state after spawn. Which model/ObjSpawnInfo to pass and whether to null the
behaviour are **plan-discovery items**, resolved on-screen in step 1.

## 4. Palette tint — bounded investigation + fallback

Target look: P0 white / P1 red / P2 blue / **P3 black**. Mechanism:
inject `gDPSetPrimColor` per object in the object-draw path so each bomber
renders in its player color. **The tint seam is not yet identified** — object
drawing is dispatched through ~15 `func_800…` calls inside `func_800821E0`
(the draw routine), and the per-model draw may be an irreducible / `GLOBAL_ASM`
function with no clean `RECOMP_PATCH` seam.

Therefore palette-tint is a **time-boxed investigation, not a guarantee**. If a
reducible per-object seam is found, apply the four colors. If not, stop and
fall back in order, flagging which occurred:
1. **Different existing models** — puppet 4 visually distinct bomber-type
   models already in the ROM (candidates: `OBJ_PLAYER`, `OBJ_MIR_BOMBER` 392,
   `OBJ_GHOSTMAN` 367, `OBJ_EVBOMBER` 614) so they're distinguishable without
   palette RE. (Also the natural home if the spawn spike already forces
   non-`OBJ_PLAYER` models for behaviour reasons — §3.)
2. **Same model, color deferred** — all four as the player model; palette
   becomes its own follow-up once spawn+puppet is solid.

This keeps the risky draw-path RE from stalling the slice: steps 1–3 deliver
"4 bombers moving per the sim" independent of the tint outcome.

## 5. Sequenced steps (each independently verifiable on screen)

1. **Spawn spike** — spawn 1 static extra object in the Battle Room at a fixed
   offset from the player; confirm it draws and note behaviour interference.
   *(de-risks spawn; resolves model + behaviour-suppression choice)*
2. **Puppet 1** — drive that object from sim player 1 via anchor-to-player
   (§2); confirm it tracks its sim position as the player moves.
   *(de-risks positioning for a non-slot-0 object)*
3. **Scale to 4** — loop spawn+puppet for players 1–3; confirm the four-corner
   layout on screen.
4. **Palette** — investigate the tint seam (§4); apply 4 colors or fall back
   per §4, and record which in CLAUDE.md.

## 6. Native / bridge API changes

- **Per-index getters** replace A1.2a's single-player getters: instead of
  returning player 0's *displacement*, the render getter for `i≥1` returns the
  **absolute offset** `(sim_pos_i - sim_pos_0) * scale` and yaw for player `i`;
  player 0 keeps the displacement path. One entry point, e.g.
  `arena_get_bomber_render(int i, float* dx_or_off, float* off_z, float* yaw)`.
- **Spawn/init entry** the patch calls once on battle level-enter, if any
  native bookkeeping is needed (e.g. remembering spawned slot indices). Spawn
  itself is done in the patch (only patches see `gObjects` / can call
  `func_80027464`); the native side supplies target offsets/colors.
- New exports get `syms.ld` addresses after `0x8F000128`, a `REGISTER_FUNC`
  entry + `extern "C"` fwd-decl in `main.cpp`, and `DECLARE_FUNC` in the patch
  — the proven 4-step bridge (integration notes §1).
- The render loop in `arena_render.c` iterates players 1..3 (spawn once,
  puppet each frame) in addition to the existing player-0 write.

## 7. Non-goals (deferred, with owner)

- Bombs and blasts rendering → **A1.2c**.
- Animation selection (run/jump/tumble → `actionState` / `anim_hint`) →
  fast-follow after A1.2c; A1.2b writes position + facing only.
- Camera-relative input (the forward/back "compression" feel item) → the feel
  pass; measured from real `gView.rot`, not guessed (integration notes §5).
- HUD, arena-specific camera framing → later.
- Arena shell stays `MAP_BATTLE_ROOM`; the Nitros boss-arena eval remains a
  separate side task.
- No changes to `bmhero-arena` sim code (pinned hash `4b6687d4`); this is all
  fork-side bridge + patch.

## 8. Risks / open items

All are plan-discovery with concrete fallbacks:
1. **Spawn behaviour interference** (§3) — spike reveals it; fallback is a
   benign model or neutralized behaviour pointer.
2. **Palette seam may be irreducible** (§4) — time-boxed; fallback to distinct
   models, then to deferred color.
3. **Free-slot contention** — assumes the Battle Room leaves slots 2–5 free
   (integration notes: room is "already a clean versus arena"). If the room
   pre-populates some, the spike surfaces it; fallback is a wider pool or a
   small spawn-suppression patch.
4. **Y anchoring** — copying `gPlayerObject.Pos.y` assumes a flat floor
   (true for arena 0). If a bomber needs its own height (jump), that's the
   anim/feel pass, not A1.2b.

## 9. Build & testing

- Fork branch `feature/a1.2b-spawn-bombers` off `master`.
- Touches (fork): `src/arena_bridge/*` (per-index getters, spawn bookkeeping,
  color table), `src/main/main.cpp` (register new exports), `patches/arena_render.c`
  (spawn on level-enter + per-frame puppet loop; possibly a second patch for
  the tint seam), `patches/syms.ld` (new import addresses). Any `patches/*.c`
  edit → LLVM-15 MIPS path + `make clean` in `patches/` before the cmake build
  (memory `recomp-build-toolchain`).
- `bmhero-arena` repo: this spec + CLAUDE.md status update on completion.
- Verification: build-exit gate + **human boot gate** (only a human can see the
  four bombers) — confirm four bombers in a square, player 0 tracks the stick,
  and record the palette outcome.
