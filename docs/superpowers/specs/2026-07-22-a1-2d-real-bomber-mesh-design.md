# A1.2d — real bomber mesh (skeletal) + facing — design spec

**Date:** 2026-07-22 · **Status:** approved (design review in-session)
**Purpose:** players 1–3's puppet actors stop being bomb placeholders and
render as real bombers (`gFileArray[1]` multi-part skeletal model) in a
neutral idle pose, rotated to face their sim heading. This is the §8.5b
follow-up deferred from A1.2b. Rendering only — sim untouched.
**Builds on:** A1.2b (generic-pool spawn + anim-bind recipe, boss-suppression
sweep, patch/export bridge), A1.2c (bombs/blasts render; symbolized-dump
tooling `build-rwdi`/`playrwdi.bat`; load crash fixed → old pool-ceiling
finding invalid).
**Companion:** integration notes §8.5b/§8.6, patch-machinery reference,
`lib/bmhero/src/code/4DFF0.c` (decomp, checked out in the fork).

## 1. Goal and exit criterion

**Exit criterion (human boot gate):** screenshot shows 3 idle bombers at the
sim spawn corners (player 0 = the game's own player object, unchanged); their
facing visibly tracks movement direction; a full match loop (move / throw /
set / kick / detonate) runs with no white screen or crash across ≥5 clean
boots; bombs + blast pops still render (regression pass). Sim untouched
(pinned hash `4b6687d4`). New fork branch `feature/a1.2d-bomber-mesh`.

## 2. What we know / the wall being broken

- **Known failure (§8.5b):** naive swap bomb→bomber (`gFileArray[1]`, single
  `func_8001BD44` cfg `0x13` + one anim bind) spawns fine but the **draw
  white-screens** (RSP abort on a malformed model). The bomber is multi-part
  skeletal; a one-shot load builds a broken model.
- **The game's own recipe (`4DFF0.c` ~line 424, demo path):**
  `for i in 0..7: if (D_80134794->unk14[i]!=-1 && unk34[i]!=-1) {
  func_8001BD44(i, 0, unk34[i], gFileArray[1].ptr + unk14[i]);
  func_8001BE6C(i,0,0,0); func_8001B754(0,0); gObjects[i].actionState = 1; }`
  — note it writes `gObjects[i]` for i=0..7: parts may be **separate
  low-index objects**, not one object with a multi-part `Unk140`. Caveat:
  this is demo/cutscene context (`D_80134794` is demo-driven); the in-level
  player may assemble differently.
- **New leverage since §8.5b was deferred:** white screens are now debuggable
  (`playrwdi.bat` + `cdb` symbolized dumps — this cracked the load crash),
  and the "pool ceiling ~6–8" finding is invalid (was the load-crash race),
  so pool headroom is unknown, not known-bad.

## 3. Phase 1 — RE the player assembly (decomp-only, zero boots)

Answer from `lib/bmhero/src/code/4DFF0.c` + callers; write findings into
integration notes §8.5b **before any fork code**:

1. **Object topology:** is the in-level bomber N objects (one per part, per
   the loop above) or one object? What do `func_8001BE6C(i,0,0,0)` and
   `func_8001B754(0,0)` do (attach / skeleton root / pose)?
2. **Descriptor:** is `D_80134794` (fields `unk10`, `unk14[8]`, `unk34[8]`,
   `unk54`) demo-specific or the general player descriptor? If demo-specific,
   find where the **in-level** player loads its parts.
3. **Approach-B check (folded in):** does a single separable
   "assemble bomber" function wrap this loop? If yes and it's free of
   `gPlayerObject`/single-player state, call it instead of hand-rolling.
4. **`func_8001ABF4` arg semantics:** demo code calls
   `(0, partIdx?, 0, &D_80101E8C[n])` — first two args don't match our A1.2b
   `(slot, 0, 0, cfg)` usage. Pin down what they mean.
5. **Idle pose:** how the `D_80101E8C` anim-config family selects a pose;
   pick a neutral idle.
6. **Facing:** `ObjectStruct` Y-rotation field offset + units (s16 binary
   angle?), from how the game writes player rotation.

## 4. Phase 2 — one-puppet spike (player 1 only)

Replicate the discovered recipe on player 1's actor: per-part loads + binds +
idle pose, an `arena_dbg_u32` marker after **every** load/bind step, build,
screenshot-verify. White screens → symbolized-dump loop. Also **measure pool
pressure**: what one skeletal bomber consumes (slots / model / anim pools) —
this replaces the invalidated ceiling finding.

**Decision gate at phase end:** if the skeletal draw can't be stabilized in
the spike, stop and choose fallback explicitly (single-part stand-in mesh, or
keep bombs) — no grinding past the gate.

## 5. Phase 3 — all three + facing

- Roll the recipe to players 2–3.
- **Facing:** per-frame Y-rotation write from sim heading (Q20.12 → game
  angle units, converted bridge/patch-side; the sim is not touched). Neutral
  input keeps last facing.
- If parts are separate objects: per-frame position write moves the root,
  verify parts follow via the game's attach mechanism; the boss-suppression
  sweep's `arena_is_actor_slot` exemption list must cover **every part slot**
  so the sweep doesn't eat limbs.

## 6. Constraints (standing invariants the plan inherits)

- Patches stay **stateless** — new state lives native in `arena_bridge`.
- Auto-named `D_*` data symbols: literal-address pattern unless they resolve
  via `data_dump.toml` (per the patch-machinery reference).
- Native exports: 4-arg/32-bit ABI, floats as u32 bit patterns.
- Any `patches/*.c` edit → `make -C patches clean` before the cmake build.

## 7. Non-goals (deferred, with owner)

- Walk/run animation → next A1.2 item (anim pass), after this slice.
- Per-player colors/tint (white/black/red/blue) → rejected from this slice;
  revisit with the anim/HUD pass.
- Camera-relative input, HUD → later A1.2 items.
- Real explosion visual (effect-asset RE) → still deferred (§8.9).
- Re-testing larger *bomb* pools → separate small task enabled by the pool
  re-measurement here.
- No `bmhero-arena` sim changes (pinned hash `4b6687d4`).

## 8. Build & testing

- Fork branch **`feature/a1.2d-bomber-mesh`** (new, from
  `feature/a1.2b-spawn-bombers`). Touches: `patches/arena_render.c`,
  `patches/syms.ld` (if new exports), `src/arena_bridge/*`; decomp reads in
  `lib/bmhero` (read-only).
- Verification: build-exit gate + **human boot gates** (phase 2: one bomber
  idle on screen; phase 3: three bombers + facing + full-match regression).
  Agent verifies via `capture-game.ps1` + `arena_bridge.log` markers; human
  does the ~15s launcher→Battle nav. Phase 1 needs zero boots.
- Docs close-out: integration notes §8.5b rewritten from findings; CLAUDE.md
  status updated; this spec committed in `bmhero-arena`.
