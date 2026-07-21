# A1.2b — Spawn + Puppet 4 Bombers — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking. **This plan is spike-driven and human-boot-gated:** several tasks
> end in a *human* visual check in the running game (no automated test can see
> the bombers), and two tasks (spawn, tint) are on-screen investigations whose
> exact final code is resolved during execution. Do **not** dispatch blind
> subagents for the boot-gate tasks — a human must watch the screen.

**Goal:** In the Battle Room, draw all 4 `ArenaState` players as bomberman
objects moving under our fixed-point sim, laid out at the sim's four corners and
distinguishable by color (P0 white / P1 red / P2 blue / P3 black), by extending
the A1.2a render bridge to spawn 3 extra objects and puppet all four.

**Architecture:** Fork-side only. A per-frame `RECOMP_PATCH` (`arena_render.c`,
already puppeting `gObjects[0]` = player 0) spawns 3 extra bomberman objects into
free slots `gObjects[2..5]` once on battle level-enter, then each frame positions
them relative to the live `gPlayerObject` using per-index offsets from the native
bridge. The sim (`lib/bmhero-arena`) is untouched.

**Tech Stack:** N64Recomp static recomp + RT64; MIPS patches via LLVM-15;
native bridge in C++ (clang-cl); the pure sim in C11 (Q20.12 fixed point).

## Global Constraints

- **Fork repo:** `C:\Users\dshi\GitRepos\BMHeroRecomp` (branch off `master`).
- **No changes to `lib/bmhero-arena` sim code** — pinned hash `4b6687d4`,
  `TUNE_VERSION` 2 stay fixed. This slice is bridge + patch only.
- **Native ↔ patch bridge is the proven 4-step pattern** (integration notes §1):
  native impl in `arena_bridge.cpp` → ABI shim in `arena_bridge_export.cpp`
  (`extern "C" void NAME(uint8_t* rdram, recomp_context* ctx)`, `_arg<N,T>` /
  `_return`) → `REGISTER_FUNC(NAME)` + `extern "C"` fwd-decl in `src/main/main.cpp`
  → `NAME = 0x8F0001XX;` in `patches/syms.ld` + `DECLARE_FUNC` in the patch. The
  three name layers must not collide.
- **syms.ld next free address: `0x8F00013C`** (last used `0x8F000138`). Allocate
  new exports sequentially from there.
- **Build gotcha (mandatory):** after editing any `patches/*.c`, run `make clean`
  in `patches/` (composed PATH, LLVM-15 first — memory `recomp-build-toolchain`)
  before the cmake build. A stale `patches.elf` against new native code crashes
  with an ABI/logic mismatch that looks like a code bug.
- **Run via `play.bat`** from the fork root (raw exe double-click crashes
  `0xC0000409` — needs `assets/` resolved from repo root).
- **Verification model:** each task ends with (a) a clean build and (b) a
  **human boot gate** — launch, click Battle, observe the stated on-screen result.
  There is no unit-test harness for the render bridge (matches A1.2a).

---

### Task 1: Spawn spike — one extra bomberman object on screen

Prove we can place a drawable bomberman into a free slot and learn whether its
`behaviour` fights us. This resolves two spec open items (§3): which model /
`ObjSpawnInfo`, and whether the behaviour must be neutralized.

**Files:**
- Modify: `patches/arena_render.c` (add a one-shot spawn in the level-enter patch)

**Interfaces:**
- Consumes: existing `RECOMP_PATCH void func_800824A8(void)` (level-enter),
  `arena_bridge_is_battle()`, game globals `gObjects`, `gPlayerObject`.
- Produces: a spawned object slot index recorded for Task 2/3; a documented
  decision (model + behaviour-suppression yes/no).

- [ ] **Step 1: Find a concrete spawn candidate.**
  In `lib/bmhero/src`, find how the player itself is created so we can reuse its
  model. Search for the player's `ObjSpawnInfo` and `func_80027464` player spawn:
  ```
  grep -rn "gPlayerObject" lib/bmhero/src | grep -i spawn
  grep -rn "func_80027464(1, &" lib/bmhero/src
  ```
  Pick the `ObjSpawnInfo` whose `objID` is `OBJ_PLAYER` (0) or a bomber model
  (`OBJ_BOMBER7` 6). Record its symbol (e.g. `D_8011XXXX`) — that is the spawn
  candidate. If no player `ObjSpawnInfo` is exported, fall back to a known-good
  one from the grep list (e.g. `&D_80113400`) purely to prove the spawn call.

- [ ] **Step 2: Add a one-shot spawn to the level-enter patch.**
  In `patches/arena_render.c`, declare the chosen spawn info + spawn func and
  spawn one object at a fixed offset from the player when battle mode is set.
  Add near the top:
  ```c
  extern struct ObjSpawnInfo D_8011XXXX;   /* the candidate from Step 1 */
  extern s32 func_80027464(s32 kind, struct ObjSpawnInfo* info,
                           f32 x, f32 y, f32 z, f32 rotY);
  static s32 g_spike_slot = -1;
  ```
  In `RECOMP_PATCH void func_800824A8(void)`, after the existing body, add:
  ```c
  if (arena_bridge_is_battle() && gPlayerObject != NULL) {
      g_spike_slot = func_80027464(1, &D_8011XXXX,
                                   gPlayerObject->Pos.x + 300.0f,
                                   gPlayerObject->Pos.y,
                                   gPlayerObject->Pos.z, 0.0f);
      recomp_printf("[arena] spike spawn slot %d\n", g_spike_slot);
  }
  ```
  (Use the patch-side print helper already used in the fork; if none, skip the
  print and read the slot on screen.)

- [ ] **Step 3: Build.**
  `make clean` in `patches/`, then the cmake build target `BMHeroRecompiled`
  with the composed LLVM-15 PATH (memory `recomp-build-toolchain`).
  Expected: clean build, no link errors on `func_80027464` / `D_8011XXXX`.

- [ ] **Step 4: Boot gate — observe the spike.**
  Run `play.bat`, click Battle. Look for a second bomberman ~300 units from the
  player. Record: (a) does it appear? (b) does it stand still, drift, T-pose,
  or crash? (c) what slot index was logged.
  - If it crashes or wildly self-moves → its `behaviour` is active; note that
    Task 2 must neutralize it (e.g. set `actionState`/behaviour pointer to a
    benign value after spawn) or switch to a benign model.
  - If it appears and is roughly static → good; proceed.

- [ ] **Step 5: Commit.**
  ```bash
  git add patches/arena_render.c
  git commit -m "spike(A1.2b): spawn one extra bomberman object in Battle Room"
  ```
  Record the model + behaviour decision in the commit body.

---

### Task 2: Native per-index bomber-offset getters

Add the deterministic math the patch needs: for player `i`, the scaled XZ offset
from player 0's sim position, plus yaw. Player 0 keeps its A1.2a displacement
path unchanged.

**Files:**
- Modify: `src/arena_bridge/arena_bridge.cpp` (compute offsets)
- Modify: `src/arena_bridge/arena_bridge.h` (declare getters)
- Modify: `src/arena_bridge/arena_bridge_export.cpp` (ABI shims)
- Modify: `src/main/main.cpp` (`REGISTER_FUNC` + fwd-decls)
- Modify: `patches/syms.ld` (import addresses)

**Interfaces:**
- Consumes: `g_state` (the native `ArenaState`), `g_scale`, `qf()` from Task 0
  baseline (`arena_bridge.cpp` already has these).
- Produces (native, called by the patch):
  - `float arena_get_bomber_off_x(int i)` → `(sim_pos_i.x - sim_pos_0.x) * scale`
  - `float arena_get_bomber_off_z(int i)` → `(sim_pos_i.z - sim_pos_0.z) * scale`
  - `float arena_get_bomber_yaw(int i)`   → player i yaw in degrees
  Exported (syms.ld / REGISTER_FUNC / DECLARE_FUNC) as `arena_export_bomber_off_x`,
  `arena_export_bomber_off_z`, `arena_export_bomber_yaw`.

- [ ] **Step 1: Implement the offset getters in `arena_bridge.cpp`.**
  Add after the existing player getters:
  ```c
  extern "C" float arena_get_bomber_off_x(int i) {
      if (i < 0 || i >= ARENA_MAX_PLAYERS) return 0.0f;
      return qf(g_state.players[i].pos.x - g_state.players[0].pos.x) * g_scale;
  }
  extern "C" float arena_get_bomber_off_z(int i) {
      if (i < 0 || i >= ARENA_MAX_PLAYERS) return 0.0f;
      return qf(g_state.players[i].pos.z - g_state.players[0].pos.z) * g_scale_z;
  }
  extern "C" float arena_get_bomber_yaw(int i) {
      if (i < 0 || i >= ARENA_MAX_PLAYERS) return 0.0f;
      return (float)g_state.players[i].yaw * (360.0f / 65536.0f);
  }
  ```

- [ ] **Step 2: Declare them in `arena_bridge.h`.**
  Inside the `extern "C"` block:
  ```c
  float arena_get_bomber_off_x(int i);
  float arena_get_bomber_off_z(int i);
  float arena_get_bomber_yaw(int i);
  ```

- [ ] **Step 3: Add ABI shims in `arena_bridge_export.cpp`.**
  ```c
  extern "C" void arena_export_bomber_off_x(uint8_t* rdram, recomp_context* ctx) {
      _return(ctx, arena_get_bomber_off_x(_arg<0, int>(rdram, ctx)));
  }
  extern "C" void arena_export_bomber_off_z(uint8_t* rdram, recomp_context* ctx) {
      _return(ctx, arena_get_bomber_off_z(_arg<0, int>(rdram, ctx)));
  }
  extern "C" void arena_export_bomber_yaw(uint8_t* rdram, recomp_context* ctx) {
      _return(ctx, arena_get_bomber_yaw(_arg<0, int>(rdram, ctx)));
  }
  ```

- [ ] **Step 4: Register in `src/main/main.cpp`.**
  Add fwd-decls next to the existing arena `extern "C"` decls:
  ```c
  extern "C" void arena_export_bomber_off_x(uint8_t*, recomp_context*);
  extern "C" void arena_export_bomber_off_z(uint8_t*, recomp_context*);
  extern "C" void arena_export_bomber_yaw(uint8_t*, recomp_context*);
  ```
  And in the `REGISTER_FUNC` block (~line 735):
  ```c
  REGISTER_FUNC(arena_export_bomber_off_x);
  REGISTER_FUNC(arena_export_bomber_off_z);
  REGISTER_FUNC(arena_export_bomber_yaw);
  ```

- [ ] **Step 5: Allocate addresses in `patches/syms.ld`.**
  Append after `arena_export_player_yaw = 0x8F000138;`:
  ```
  arena_export_bomber_off_x = 0x8F00013C;
  arena_export_bomber_off_z = 0x8F000140;
  arena_export_bomber_yaw   = 0x8F000144;
  ```

- [ ] **Step 6: Build (native + patches).**
  `make clean` in `patches/`, then cmake build. Expected: clean build; new
  symbols resolve. (No on-screen change yet — getters are unused until Task 3.)

- [ ] **Step 7: Commit.**
  ```bash
  git add src/arena_bridge/ src/main/main.cpp patches/syms.ld
  git commit -m "feat(A1.2b): native per-index bomber offset/yaw getters + exports"
  ```

---

### Task 3: Puppet the spiked object from sim player 1, then scale to 4

Wire the spawn + per-frame puppet loop for players 1..3, anchored to the live
player. First prove one (player 1), then extend the loop.

**Files:**
- Modify: `patches/arena_render.c` (spawn 3 on level-enter; puppet loop each frame)

**Interfaces:**
- Consumes: `arena_get_bomber_off_x/z`, `arena_get_bomber_yaw` (Task 2, imported
  via `DECLARE_FUNC` under their export names), `func_80027464`, `gObjects`,
  `gPlayerObject`, `arena_bridge_is_battle()`.
- Produces: slots `g_bomber_slot[1..3]` puppeted every frame.

- [ ] **Step 1: Import the new exports in `arena_render.c`.**
  Add with the existing `DECLARE_FUNC` block:
  ```c
  DECLARE_FUNC(f32, arena_export_bomber_off_x, s32 i);
  DECLARE_FUNC(f32, arena_export_bomber_off_z, s32 i);
  DECLARE_FUNC(f32, arena_export_bomber_yaw,   s32 i);
  ```

- [ ] **Step 2: Replace the Task-1 single spike with a 3-object spawn.**
  Replace `g_spike_slot` with an array and spawn players 1..3 on level-enter.
  Near the top:
  ```c
  static s32 g_bomber_slot[ARENA_NPLAYERS];   /* index by player; [0] unused */
  ```
  In `RECOMP_PATCH void func_800824A8(void)`, after the body, replace the spike
  block with:
  ```c
  if (arena_bridge_is_battle() && gPlayerObject != NULL) {
      for (s32 i = 1; i <= 3; i++) {
          g_bomber_slot[i] = func_80027464(1, &D_8011XXXX,
              gPlayerObject->Pos.x + arena_export_bomber_off_x(i),
              gPlayerObject->Pos.y,
              gPlayerObject->Pos.z + arena_export_bomber_off_z(i), 0.0f);
          /* If Task 1 found the behaviour interferes, neutralize it here,
             e.g.: gObjects[g_bomber_slot[i]].behaviourPtrField = 0;  */
      }
  }
  ```
  (`D_8011XXXX` = the model chosen in Task 1. `ARENA_NPLAYERS` = 4; define a
  local `#define` if not already available in the patch.)

- [ ] **Step 3: Add the per-frame puppet loop in `arena_render_routine`.**
  In the existing `arena_render_routine()`, after the player-0 write, add:
  ```c
  for (s32 i = 1; i <= 3; i++) {
      s32 slot = g_bomber_slot[i];
      if (slot < 0) continue;
      gObjects[slot].Pos.x = gPlayerObject->Pos.x + arena_export_bomber_off_x(i);
      gObjects[slot].Pos.z = gPlayerObject->Pos.z + arena_export_bomber_off_z(i);
      gObjects[slot].Pos.y = gPlayerObject->Pos.y;   /* same floor */
      gObjects[slot].Rot.y = arena_export_bomber_yaw(i);
  }
  ```

- [ ] **Step 4: Build.**
  `make clean` in `patches/`, cmake build. Expected: clean build.

- [ ] **Step 5: Boot gate — 4-corner layout.**
  Run `play.bat`, click Battle. Expect **four** bombers: the player (stick-driven)
  plus three at the sim's other corners, forming a square around the arena. Move
  the player and confirm the other three stay pinned to their sim positions
  (they should appear to hold station since idle sim players don't move). If the
  three overlap the player or sit at wrong corners, the offset scale/sign is off
  — adjust `g_scale`/`g_scale_z` sign or magnitude in `arena_bridge.cpp` and
  rebuild. If a spawned bomber self-moves/crashes, apply the Task-1 behaviour
  neutralization in Step 2.

- [ ] **Step 6: Commit.**
  ```bash
  git add patches/arena_render.c src/arena_bridge/arena_bridge.cpp
  git commit -m "feat(A1.2b): spawn + puppet 4 bombers (anchor-to-player, 4-corner layout)"
  ```

---

### Task 4: Palette tint (investigation) with distinct-model fallback

Make the four bombers distinguishable: P0 white / P1 red / P2 blue / P3 black.
Primary = same model tinted via `gDPSetPrimColor`; single fallback = 4 distinct
models. Time-boxed — do not open-endedly RE an irreducible draw function.

**Files:**
- Modify: `patches/arena_render.c` and/or new `patches/arena_tint.c` (if a seam
  is patched), or `patches/arena_render.c` spawn block (if using distinct models)

**Interfaces:**
- Consumes: the object-draw dispatch inside `func_800821E0` (draw routine),
  `gObjects`, the bomber slots from Task 3.
- Produces: 4 visually distinguishable bombers; the outcome (tint vs. models)
  recorded in CLAUDE.md + integration notes.

- [ ] **Step 1: Locate the per-object model-draw call.**
  In `lib/bmhero/src/code/71AA0.c`, read `func_800821E0` (lines ~592–647). It
  calls ~15 `func_800…` routines; identify which iterates `gObjects` and emits
  each object's model display list (grep those callees for `gObjects[` and
  `gSPMatrix`/`gSPDisplayList`). That callee is the tint seam candidate.

- [ ] **Step 2: Check reducibility of the seam.**
  Confirm the candidate is a fully-decompiled C function (not `GLOBAL_ASM` /
  marked irreducible) — only those can be cleanly `RECOMP_PATCH`ed
  (integration notes §1). Grep the decomp `.c` for the function body; if it is
  `GLOBAL_ASM` or a giant goto machine, mark the seam **irreducible** and go to
  Step 5 (fallback).

- [ ] **Step 3 (tint path): Patch the seam to set prim color per bomber.**
  `RECOMP_PATCH` the draw callee (or wrap it). Before emitting a bomber slot's
  model, push its color from a table, e.g.:
  ```c
  static const u8 g_bomber_rgb[4][3] = {
      {255,255,255},  /* P0 white  */
      {230, 40, 40},  /* P1 red    */
      { 40, 80,230},  /* P2 blue   */
      { 20, 20, 20},  /* P3 black  */
  };
  /* when about to draw gObjects[slot] that maps to player i: */
  gDPSetPrimColor(gMasterDisplayList++, 0, 0,
                  g_bomber_rgb[i][0], g_bomber_rgb[i][1], g_bomber_rgb[i][2], 255);
  ```
  Map slot→player via `g_bomber_slot[]`. Ensure the color is reset/neutral for
  non-bomber objects so the rest of the scene is unaffected. (Note: prim color
  tints only if the model's combiner uses PRIM; if the bomberman model ignores
  prim, this won't visibly color it — that is a valid "seam works but model
  doesn't use prim" outcome → go to Step 5.)

- [ ] **Step 4 (tint path): Build + boot gate.**
  `make clean` in `patches/`, cmake build, `play.bat` → Battle. Expect four
  bombers in white/red/blue/black. If colored → done, go to Step 6. If the seam
  patched cleanly but the model ignores prim color → Step 5.

- [ ] **Step 5 (fallback): 4 distinct models.**
  In the Task-3 spawn block, spawn players 1..3 with three visually distinct
  bomber-type `ObjSpawnInfo`s instead of the player model (candidates by objID:
  `OBJ_MIR_BOMBER` 392, `OBJ_GHOSTMAN` 367, `OBJ_EVBOMBER` 614 — find their
  spawn infos with `grep -rn "objID = 392\|objID = 367\|objID = 614"` and the
  `func_80027464` call that spawns each). Player 0 stays the player model.
  Build + boot gate: four distinguishable bombers by silhouette/color.

- [ ] **Step 6: Commit.**
  ```bash
  git add patches/ src/
  git commit -m "feat(A1.2b): distinguish 4 bombers (tint or distinct models)"
  ```
  Commit body records which path shipped and why.

---

### Task 5: Documentation + branch wrap

**Files:**
- Modify (fork): none beyond code already committed.
- Modify (canonical `bmhero-arena`): `CLAUDE.md`,
  `docs/bmhero-recomp-integration-notes.md`

- [ ] **Step 1: Update the integration notes (§3).**
  Record the concrete spawn call that worked (kind, `ObjSpawnInfo` symbol,
  model), whether behaviour neutralization was needed, and the tint outcome
  (seam symbol + reducible?, or which distinct models were used).

- [ ] **Step 2: Update `CLAUDE.md` status.**
  Add an "A1.2b complete" block (mirroring the A1.2a block): what shipped,
  the fork branch `feature/a1.2b-spawn-bombers`, and set Next to A1.2c
  (bombs/blasts). Note the tint-vs-models outcome.

- [ ] **Step 3: Commit docs (canonical repo).**
  ```bash
  git add CLAUDE.md docs/bmhero-recomp-integration-notes.md
  git commit -m "docs: A1.2b complete — 4 bombers spawned + puppeted in Battle Room"
  ```

- [ ] **Step 4: Merge the fork branch.**
  On the fork, merge `feature/a1.2b-spawn-bombers` into `master` (matching the
  A1.2a merge-commit pattern) once the boot gate passes.

---

## Self-Review

**Spec coverage:**
- §1 goal / 4-corner colored layout → Tasks 3 (layout) + 4 (color).
- §2 anchor-to-player positioning → Task 2 (offset math) + Task 3 (patch write).
- §3 spawn, proven incrementally, behaviour risk → Task 1 (spike) + Task 3 (scale).
- §4 palette primary + distinct-model fallback → Task 4 (both paths).
- §5 sequenced steps → Tasks 1→2→3→4 map 1:1 to spec steps 1–4.
- §6 native/bridge API (per-index getters, syms.ld, REGISTER_FUNC) → Task 2.
- §7 non-goals (bombs, anim, camera, HUD) → excluded from all tasks.
- §8 risks (behaviour, tint irreducible, slot contention, Y anchoring) →
  Task 1 Step 4, Task 4 Steps 2/5, Task 3 Step 5.
- §9 build/test (branch, LLVM-15, make clean, boot gate) → Global Constraints +
  every task's build/boot steps; docs → Task 5.

**Placeholder scan:** `D_8011XXXX` is an intentional discovery variable resolved
in Task 1 Step 1 and reused by name thereafter — flagged as such, not a hidden
TODO. All deterministic code (getters, shims, registration, puppet loop, color
table) is complete. The two investigation tasks (1, 4) give exact procedures,
candidate values, and decision criteria rather than pre-guessed final code,
because their result is only knowable on screen.

**Type consistency:** getter names `arena_get_bomber_off_x/off_z/yaw` and export
names `arena_export_bomber_off_x/off_z/yaw` are used identically across
arena_bridge.cpp/.h, the export shims, main.cpp, syms.ld, and the patch's
DECLARE_FUNC. `g_bomber_slot[]` is defined in Task 3 and consumed in Task 4.
`g_scale`/`g_scale_z` are the existing A1.2a constants.
