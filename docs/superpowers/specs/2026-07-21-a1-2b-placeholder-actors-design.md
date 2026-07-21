# A1.2b (revised) — spawn + puppet 3 placeholder actors, then try real bombers

**Date:** 2026-07-21 · **Status:** approved (design review in-session)
**Supersedes:** `2026-07-20-a1-2b-spawn-puppet-4-bombers-design.md` (BLOCKED) —
that plan spawned into the wrong object pool (`gObjects[2..5]`, the bomb pool),
anchored to the live player (mirror bug), and treated "no resident bomber
model" as the wall. This revision corrects all three from a fresh decomp trace
(recorded in integration notes §8).
**Purpose:** second render-bridge slice — get the *other three* `ArenaState`
players on screen in the Battle Room, moving under our fixed-point sim, without
the crashes that blocked the first attempt. Ship a reliable multi-actor bridge
with **placeholder** meshes first, then attempt a one-argument upgrade to real
bomber meshes. Slice of A1.2 (A1.2a puppet one ✅ → **A1.2b spawn+puppet 3** →
A1.2c bombs/blasts → anim / camera-relative input / HUD later).
**Builds on:** A1.2a (player 0 puppeted from sim via per-frame displacement),
A1.1b-ii (Battle warps into `MAP_BATTLE_ROOM`), A1.1a (sim ticks natively).
**Companion:** arena design §7 (render bridge), integration notes §1 (patch/
export mechanism, stateless-patch gotcha), §3 (objects), §6 (coord mapping),
§8 (the corrected multi-actor findings), memory `recomp-build-toolchain`.

## 1. Goal and exit criterion

In the Battle Room, players 1–3 are drawn as three extra objects that provably
track their sim positions relative to the player, using a **resident** mesh —
proving the multi-actor spawn+puppet bridge end to end. Then, on the same
scaffold, attempt to swap the placeholder mesh for the real bomber mesh.

**Exit criterion (human boot gate):** click Battle → in the Battle Room, three
extra actors are visible at distinct positions forming the sim's layout with the
player; as the stick moves player 0 (A1.2a), the three hold their sim-relative
positions and do not mirror the player; no crash. **Placeholder pass** ships
even if the actors are bombs. **Upgrade pass** is a bonus: if the bomber mesh
draws, the three look like bombers; if not, the bomb placeholder stays and the
bomber attempt is recorded as deferred. Non-battle launches unchanged;
`bmhero-arena` sim untouched (pinned hash `4b6687d4`).

## 2. The corrected RE picture (why this is tractable now)

The BLOCKED notes conflated two separable things. From the fresh trace:

- **Mesh (what draws)** is set by an object's `Unk140[]` model-part handles,
  populated by **`func_8001BD44(slot, cfgA, cfgB, gFileArray[idx].ptr)`** — it
  loads *any* mesh from *any* file pointer into *any* slot. The generic object
  draw loops `func_8001C464`/`func_8001C5B8` (`boot/17930.c:1236,1257`) iterate
  `gObjects[14..77]` and just follow each active object's `Unk140[]`.
- **Behaviour + some render params** come from `objID` via
  `gObjInfo[objID].behaviour()`, called each frame by the update loop
  `func_8002B154` (`boot/26CE0.c:1208`) for `gObjects[14..77]`.

The "cloning the player crashes" result was the **player *behaviour*** (single-
player logic), not an unavailable mesh. The "cloning the door/plate crashes"
result was the **memcpy'd `unk10E` group links** — avoided entirely by a
*fresh* spawn that lets the game wire those links itself.

**The unlock:** the bomber mesh is **`gFileArray[1]`** and the bomb mesh is
**`gFileArray[9]`** — both low-index core/player assets, resident in every
level including the Battle Room (that is why the player can throw bombs there).
The game loads the player mesh with one line: `func_8001BD44(0, 0, 0x13,
gFileArray[1].ptr)` (`overlays/13AC20/13AC20.c:422`). We reuse exactly that.

## 3. Spawn — synthesized `ObjSpawnInfo` into the generic pool

Spawn via the game's own proper-spawn `func_80027464(count, ObjSpawnInfo*, x,
y, z, rotY)` (`boot/26CE0.c:328`): it scans `gObjects[14..77]` for
`ACTION_NONE`, then per object does `func_8001A928` (init) → `func_8001BD44(idx,
info->unk0, info->unk6, gFileArray[info->unk4].ptr)` (load mesh) → set
Pos/Rot/`objID` → **wire `unk10E`/`unkE6` group links correctly**. Calling it
with `count = 1` spawns one standalone object (its group-link loop wires
nothing cross-object — clean).

`struct ObjSpawnInfo` is 11 bytes (`types.h:830`). We **synthesize** it on the
patch stack (patch-local *stack* data is fine; patch-local *static/global*
mutable state is not — the `0xC0000409` gotcha, integration notes §1):

    ObjSpawnInfo info = {0};
    info.unk4 = FILE_INDEX;   /* 9 = bomb mesh (placeholder), 1 = bomber mesh   */
    info.unk2 = BENIGN_OBJID; /* behaviour() that does not crash (see §5)       */
    info.unk0 = CFG_A;        /* func_8001BD44 config args; bomber uses 0,0x13  */
    info.unk6 = CFG_B;        /*   (13AC20.c:422); bomb uses 0,0                */
    slot = func_80027464(1, &info, x, y, z, rotY);

This *is* the "resident-model `ObjSpawnInfo`" the BLOCKED notes went looking for
in the level loader — no level-loader spelunking needed; we point `unk4` at a
file we already know is resident. Spawn happens once, on the first battle frame,
for players 1–3 (3 calls). The spawned slot indices are remembered **in native
`arena_bridge`** (patch is stateless), read back each frame to position.

## 4. Positioning — fixed captured origin (avoids the mirror bug)

A1.2a puppets player 0 by adding the sim's per-frame **displacement** to the
live `gPlayerObject->Pos` (Y left to the game). The BLOCKED attempt positioned
the others relative to *live* `gPlayerObject->Pos`, which makes them **mirror**
the player's own game-physics movement (the player object also moves under
Hero's player code, not just our sim delta — integration notes §8).

Instead, capture a **fixed origin** once at spawn and map sim→world against it:

    /* captured once, on the first battle frame */
    origin = gPlayerObject->Pos;           /* fixed Vec3f; Y = ground plane     */
    sim_ref = sim_pos_0_at_capture;        /* fixed sim reference (player 0)    */

    /* every frame, for i in 1..3 */
    obj[i].Pos.x = origin.x + (sim_pos_i.x - sim_ref.x) * scale;
    obj[i].Pos.z = origin.z + (sim_pos_i.z - sim_ref.z) * scale;
    obj[i].Pos.y = origin.y;               /* flat arena-0 floor                */
    obj[i].Rot.y = sim_yaw_i in degrees;   /* deg = binang * 360/65536          */

Because `origin`/`sim_ref` are frozen, the three actors follow the *sim*, not
the live player object — no mirror. `scale` reuses A1.2a's value (≈120 Hero
units per sim unit; `TODO(feel)`). Player 0's existing delta-anchor path is
unchanged. Layout comes from the sim's spawn corners (`arena_geom.h`).

## 5. objID / behaviour safety and per-frame ordering

Slots `[14..77]` receive `gObjInfo[objID].behaviour()` every frame, so `unk2`
must be a **benign objID** (behaviour that does not crash). Ordering makes this
easy: the A1.2a patch wraps `gDebugRoutine2` and runs *after* the game's object
update each frame, so our per-frame Pos/Rot write is the **last** word — the
behaviour only needs to not crash, not to be a true no-op.

Candidate benign objIDs, pinned empirically in the spike: an id whose
`gObjInfo[].behaviour` is the empty `func_8002B144` (`26CE0.c:1199`), else the
Battle Room's own door/plate id (known-safe — they run in the room every frame).
We re-assert `actionState = ACTION_IDLE` each frame so nothing deactivates the
puppet out from under us.

## 6. The two passes — one scaffold, one-argument difference

Everything in §3–§5 is shared. Only `info.unk4` (+ the mesh cfg args) differ:

1. **Placeholder pass — `info.unk4 = 9` (bomb mesh).** Single-part, no
   animation, lowest crash risk. Delivers the guaranteed 4-actor bridge proof
   (the exit criterion). This is what ships.
2. **Upgrade pass — `info.unk4 = 1`, `unk0/unk6 = 0/0x13` (bomber mesh).**
   Exactly the game's own player-mesh load. If it draws → real bombers, at the
   cost of one changed argument. If it crashes or renders wrong (needs player
   animation state, wrong scale/pose) → revert to the bomb placeholder, record
   the finding, defer the bomber mesh to a follow-up. **No end state ships
   without a working render** because the placeholder pass is built and verified
   first.

## 7. Native / bridge API changes

- **State in native `arena_bridge.cpp`** (patch stays stateless): a
  `g_puppets_spawned` latch, `g_puppet_slot[3]`, a captured `origin` (`Vec3f`)
  + `sim_ref`, and `scale`. Exports (proven 4-step bridge, integration notes
  §1): `arena_puppets_spawned()` / `arena_set_puppet_slot(i, slot)` /
  `arena_capture_origin(x, y, z)` / `arena_puppet_world(i, &wx, &wz, &yaw_deg)`
  (computed from the sim, reusing the existing `arena_get_bomber_off_x/z/yaw`).
- **Patch** (extends the A1.2a `gDebugRoutine2` wrapper, `patches/arena_render.c`):
  after the original update — if `!arena_puppets_spawned()`, capture origin and
  spawn 3 via `func_80027464` with a stack `ObjSpawnInfo`, stash slots, latch;
  then each frame write Pos/Rot/`actionState` for each stashed slot.
- New exports get `syms.ld` addresses after the existing table (`0x8F000128+`),
  a `REGISTER_FUNC` entry + `extern "C"` fwd-decl in `main.cpp`, and a
  `DECLARE_FUNC` import in the patch. `func_80027464` / `gFileArray` /
  `gObjInfo` / `gObjects` are ordinary decomp symbols the patch already sees.

## 8. Sequenced steps (each independently verifiable on screen)

1. **Spawn spike** — on the first battle frame, spawn **one** object at a fixed
   world offset from the player with `info.unk4 = 9` (bomb) and a candidate
   benign `objID`; confirm it draws and does not crash, and pin the objID.
   *(de-risks spawn + behaviour in one shot)*
2. **Puppet one** — drive that object from sim player 1 via the fixed-origin map
   (§4); confirm it tracks its sim position and does **not** mirror the player.
   *(de-risks positioning for a non-slot-0 object)*
3. **Scale to three** — loop spawn+puppet for players 1–3; confirm the layout on
   screen. **Placeholder pass done — this is the shippable exit state.**
4. **Bomber-mesh upgrade** — swap `info.unk4 = 1` (+ cfg `0,0x13`); if it draws,
   keep it; else revert to bomb and record the finding (§6).

## 9. Non-goals (deferred, with owner)

- Real bomber meshes are *attempted* (step 4) but **not required to ship** —
  if the upgrade fails, bombers are a scoped follow-up.
- Per-player color / tint → follow-up once real (or placeholder) meshes are on
  screen; not needed to prove the bridge.
- Bombs and blasts rendering → **A1.2c**.
- Animation selection (`actionState` / anim hint) → after A1.2c; A1.2b writes
  position + facing only.
- Camera-relative input (forward/back "compression") → the feel pass; measured
  from real `gView.rot`, not guessed (integration notes §5).
- HUD, arena-specific camera framing → later.
- Arena shell stays `MAP_BATTLE_ROOM`; the Nitros boss-arena eval is a separate
  side task.
- No changes to `bmhero-arena` sim code (pinned hash `4b6687d4`); all fork-side
  bridge + patch.

## 10. Risks / open items (all plan-discovery, with fallbacks)

1. **Benign objID choice** (§5) — spike pins it; fallback is the door/plate id
   or, worst case, patch the update loop to skip our slots.
2. **Bomber mesh needs animation/player state to draw** (§6) — the whole reason
   the placeholder pass is built first; fallback is the bomb mesh, no schedule
   risk.
3. **`func_8001BD44` cfg args for the bomber mesh** — start from the game's own
   `0, 0x13` (13AC20.c:422); if `Unk140` isn't wired as expected, that's a spike
   observation, not a crash class.
4. **Draw-range assumption** — confirmed: generic draw loops cover `[14..77]`
   (`17930.c:1236`), which is where `func_80027464` spawns; consistent.
5. **Y anchoring** — captured `origin.y` assumes a flat floor (true for arena 0).
   Per-actor height (jump) is the anim/feel pass, not A1.2b.
6. **Free-slot contention** — the Battle Room live dump showed `[2..13]` free
   and only door/plate resident in `[14..77]`; `func_80027464` needs 3 free of
   64 — ample.

## 11. Build & testing

- Fork branch `feature/a1.2b-spawn-bombers` (continues the existing branch;
  commit `89208b1` already added the per-index getters + capture/clone
  scaffolding this revision reuses).
- Touches (fork): `src/arena_bridge/*` (puppet-slot bookkeeping, captured
  origin, `arena_puppet_world`), `src/main/main.cpp` (register new exports),
  `patches/arena_render.c` (spawn-once + per-frame puppet loop), `patches/syms.ld`
  (new import addresses). Any `patches/*.c` edit → LLVM-15 MIPS path + **`make
  clean` in `patches/` before the cmake build** (stale `patches.elf` = mismatch
  crash; memory `recomp-build-toolchain`).
- `bmhero-arena` repo: this spec + a CLAUDE.md status update and an integration
  notes §8 correction on completion.
- Verification: build-exit gate + **human boot gate** (only a human can see the
  actors on screen) — confirm three extra actors at distinct sim-relative
  positions, no mirror, no crash; record whether the bomber-mesh upgrade landed.
