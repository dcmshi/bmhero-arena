# A1.2c (slice 1) — bomb rendering + set/kick input — design spec

**Date:** 2026-07-21 · **Status:** approved (design review in-session)
**Purpose:** third render-bridge slice — draw the sim's live bombs in the arena
so a thrown/set bomb appears, arcs, lands, and vanishes on detonation. First
time the arena shows actual bomb play. Slice of A1.2 (A1.2a player ✅ → A1.2b
spawn+puppet 4 ✅ → **A1.2c bombs** → A1.2c-blasts → anim/HUD/camera later).
Blasts/explosions are a **separate follow-up slice** (need an explosion-effect
asset); this slice is **bombs only** + wiring the set/kick input.
**Builds on:** A1.2b (pooled actors spawned via `func_80027464` + `func_8001ABF4`,
positioned from the sim against a frozen origin, on the flat `MAP_NITROS_1`
arena with a boss-suppression sweep). The bomb model (`gFileArray[9]`) and its
anim config (`D_801163DC`) are the *exact* recipe already proven for the
placeholder actors.
**Companion:** integration notes §8 (spawn/anim/positioning/sweep mechanics),
arena design §2 (bomb mechanics), `arena_state.h` (`ArenaBomb`).

## 1. Goal and exit criterion

Player 0's bombs render in the arena, driven entirely by the sim's `bombs[]`
state. Throwing (or setting) a bomb makes one appear at its sim position; it
follows the sim (arc while airborne, sits when settled) and disappears when the
sim frees it (detonation).

**Exit criterion (human boot gate):** in the arena, press the set/kick button or
hold+release throw → a bomb appears and behaves (arc/land/sit); throw several →
multiple bombs; each vanishes on detonation. No crash; the 4 player actors and
boss suppression are unaffected. `bmhero-arena` sim untouched (pinned hash
`4b6687d4`; this is all fork-side).

## 2. Key insight — position-driven rendering covers every bomb state

`arena_sim` updates `ArenaBomb.pos` (Q20.12) for **all** non-FREE states —
HELD bombs track their owner, AIRBORNE bombs arc under the throw physics
(steep pitch → real vertical motion), SETTLED bombs sit, SLIDING bombs move. So
rendering a bomb actor **at `bomb.pos` each frame — X, Y, AND Z** reproduces all
bomb motion, including the throw arc, with **no per-state visual logic**. Bomb
actors are ours (no game physics), so unlike the player actors (Y left to the
game) their **Y must be mapped from the sim** or the arc collapses to a flat
slide. v1 renders position + a fixed pose; state-driven visuals (fuse blink,
held-attach offset) are deferred.

## 3. Pooled bomb actors (spawn once, toggle per frame)

Spawn a fixed pool of **8** bomb actors once (covers the 6-live-bomb cap with
margin), mapped 1:1 to `bombs[0..7]`, using the proven recipe:
`func_80027464(1, &info, x,y,z, 0)` with `info.unk4 = 9` (bomb mesh),
`info.unk2 = OBJ_TOBIRA1_O` (benign objID), then `func_8001ABF4(slot, 0, 0,
D_801163DC_ADDR)` (bind anim). Store each slot in a native bomb-slot table.

Each frame, for bomb index `i` in `0..7`:
- `arena_bomb_active(i)` (`bombs[i].state != BSTATE_FREE`) → set the actor's Pos
  from `arena_bomb_wx/wy/wz(i)` and `actionState = ACTION_IDLE` (visible);
- else → `actionState = ACTION_NONE` (hidden).

The model + anim binding persist across the actionState toggle, so hide/show is
free (no respawn). Spawn-on-demand was rejected: it adds slot churn and mid-play
spawn risk for no gain over toggling a pool.

## 4. Native / bridge API changes

Bombs live in the **same sim space** as the players, so they reuse the frozen
world origin + reference + scale already captured for the puppets
(`arena_puppet_capture`). New native fns in `arena_bridge.cpp` + recomp-ABI
shims + `REGISTER_FUNC`/`syms.ld` (the proven 4-step bridge, integration notes
§1), all int-arg / float-return (no float args):
- `int   arena_bomb_active(int i)` — `1` if `bombs[i].state != BSTATE_FREE`.
- `float arena_bomb_wx(int i)` — `g_origin_x + (qf(bombs[i].pos.x) - g_ref_sx) * g_scale`.
- `float arena_bomb_wy(int i)` — `g_origin_y + (qf(bombs[i].pos.y) - g_ref_sy) * g_scale` (arc height).
- `float arena_bomb_wz(int i)` — `g_origin_z + (qf(bombs[i].pos.z) - g_ref_sz) * g_scale_z`.
- `void  arena_bomb_set_slot(int i, int slot)` / `int arena_bomb_get_slot(int i)` — bomb-slot table (size 8).

(`g_origin_*`, `g_ref_s*`, `g_scale*`, `qf` are the existing puppet-mapping
statics — reused so bombs share the players' frozen frame. **`g_ref_sy` is new**:
`arena_puppet_capture` currently freezes only `g_ref_sx`/`g_ref_sz`; add
`g_ref_sy = qf(players[0].pos.y)` so a floor-level bomb maps to `g_origin_y` and
an arced bomb rises above it. The vertical scale reuses `g_scale`; if the arc
reads too tall/short that's a one-constant feel tweak.)

## 5. Boss-suppression sweep must preserve bomb slots

The A1.2b sweep deactivates every `gObjects[14..77]` that isn't a player-puppet
slot, each frame before the update loop. Bomb actors are **also ours**, so the
sweep must exclude the 8 bomb slots too. Extend the exclusion set to
`{player slots 1-3} ∪ {bomb slots 0-7}` (read via `arena_puppet_get_slot` /
`arena_bomb_get_slot`). Bomb actors' visibility is then owned solely by §3's
per-frame toggle, not the sweep.

## 6. Input — wire set/kick (bit 14)

The sim input has bit 13 = bomb (hold-grab / release-throw; already wired to
`CONT_B`) and **bit 14 = set/kick (edge-triggered)** — currently hard-coded to
`0`. Wire it:
- Extend `arena_bridge_tick_input(sx, sy, jump, bomb, set)` (native) +
  `arena_export_tick_input` shim + patch `DECLARE_FUNC` to take a 5th `set` arg,
  and pack it: `arena_input_pack(sx, sy, jump, bomb, set)`.
- In the patch, `set = (gActiveContButton & CONT_G) ? 1 : 0` — **`CONT_G`
  (0x2000)**, the N64 Z trigger (Q key on the recomp's default keyboard map).
  The sim edge-detects set/kick internally (via `last_input`), so passing the
  held state is correct.

This lets player 0 set bombs (classic placement) as well as throw them, giving a
more natural way to produce bombs for the demo.

## 7. Non-goals (deferred, with owner)

- **Blasts / explosions → A1.2c slice 2** (needs an explosion-effect asset;
  candidates `gFileArray[0xA/0xB/0xC]`, `blasts[]` growth via `radius_t`/`ttl`).
- State-aware bomb visuals (fuse blink, held-attach offset, sliding spin),
  airborne **height** (Y arc) → fast-follow after blasts; v1 is position + pose.
- Real bomber mesh for the player actors (§8.5b) — unrelated follow-up.
- Only player 0 has a controller, so only player 0's bombs appear — expected
  (the render is roster-general; the other 3 are idle).
- No `bmhero-arena` sim changes (pinned hash `4b6687d4`).

## 8. Build & testing

- Fork branch `feature/a1.2b-spawn-bombers` (continue) or a new
  `feature/a1.2c-bombs` — decide at plan time; touches `src/arena_bridge/*`
  (bomb getters + slot table + `set` arg), `src/main/main.cpp` (register
  exports), `patches/arena_render.c` (bomb pool spawn + per-frame toggle, sweep
  exclusion, `set` wiring), `patches/syms.ld` (new import addresses). Any
  `patches/*.c` edit → LLVM-15 MIPS path + **`make -C patches clean`** before
  the cmake build (memory `recomp-build-toolchain`).
- `bmhero-arena` repo: this spec + a CLAUDE.md status update on completion.
- Verification: build-exit gate + **human boot gate** (only a human can throw a
  bomb and watch it) — confirm a bomb appears on set/throw, follows the sim, and
  vanishes on detonation; screenshot + `arena_bridge.log` via the session
  tooling (integration notes §8.8).
