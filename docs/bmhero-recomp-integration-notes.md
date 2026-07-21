# BMHeroRecomp integration notes (render bridge RE reference)

**Living reference** for the fork-side integration work (A1.1–A1.2+). Accumulated,
decomp-verified facts so each render-bridge slice starts warm. Fork lives at
`C:\Users\dshi\GitRepos\BMHeroRecomp` (`dcmshi/BMHeroRecomp`), consuming
`bmhero-arena` as a submodule at `lib/bmhero-arena`. Decomp source under
`lib/bmhero/` (the `Bomberhackers/bmhero` submodule). Build recipe: memory
`recomp-build-toolchain` + `CLAUDE.md` A1.0 status.

Everything here is verified against the decomp at fork `master` as of 2026-07-20.

## 1. Patch & native-export mechanism (how our code runs in the game)

- **`RECOMP_PATCH void fn(...)`** (`patches/patches.h`) whole-replaces a decomp
  function by symbol name; `strict_patch_mode` validates the symbol exists. Only
  works on *small, fully-decompiled* functions — a large or `GLOBAL_ASM`
  (irreducible) function can't be cleanly replaced.
- **Native → patch call:** a patch calls native C++ via a 4-step bridge (pattern
  proven in A1.1b-ii + A1.2a):
  1. Native impl in `src/arena_bridge/arena_bridge.cpp` (plain C++).
  2. Recomp-ABI shim in `src/arena_bridge/arena_bridge_export.cpp`:
     `extern "C" void NAME(uint8_t* rdram, recomp_context* ctx)`, reading args
     with `_arg<N,T>(rdram,ctx)` and returning with `_return(ctx,val)`
     (`#include "recomp.h"` + `"librecomp/helpers.hpp"`).
  3. Register in `src/main/main.cpp`: `REGISTER_FUNC(NAME);` (in the
     `REGISTER_FUNC` block ~line 735) + a matching `extern "C"` fwd-decl.
  4. Give it a dummy address in `patches/syms.ld` (`NAME = 0x8F0001XX;` — next
     free after the existing table; ours start at `0x8F000124`) and import in
     the patch with `DECLARE_FUNC(rettype, NAME, argtypes...)`.
  - The three name layers **must differ**: internal C++ name, the exported/shim
    name (= syms.ld + REGISTER_FUNC + the patch's DECLARE_FUNC), and no collision.
- **Native has no global RDRAM handle** — only functions taking `(rdram, ctx)`
  can touch game memory. The per-VI `vi_callback` (A1.1a, set in `main.cpp`) takes
  no args, so it *cannot* read/write `gObjects` directly. Anything touching game
  state must be a MIPS patch (which sees game globals as ordinary C symbols).
- **Build gotcha:** after editing any `patches/*.c`, run `make clean` in
  `patches/` (composed PATH, LLVM-15) before the cmake build — ninja does NOT
  reliably re-run the patch make, and a stale `patches.elf` against new native
  code crashes with an ABI/logic mismatch that *looks* like a code bug.

## 2. Per-frame hook points (where to run each frame, in-level)

- Main loop dispatches two per-frame routine pointers (`boot/17930.c:1562,1797`):
  `gDebugRoutine1()` (draw) and `gDebugRoutine2()` (update). Despite the name
  these are the normal in-level per-frame routines, not debug-only.
- **`func_800824A8`** (`code/71AA0.c:649`, 6 lines) is the level-enter setup —
  called from the same 9 level-transition handlers as the warp target
  `func_80081C50`. It sets `gDebugRoutine1 = &func_800821E0` (draw) and
  `gDebugRoutine2 = &func_80024744` (update). **A1.2a's seam:** `RECOMP_PATCH`
  this 6-liner to point `gDebugRoutine2` at our wrapper, which calls the original
  then does our per-frame work. A plain patch function used as a function pointer
  IS dispatched correctly (verified working).
- `func_800821E0` (the draw routine, `71AA0.c:592`) builds the display list via
  `guPerspective`/`guLookAt(gView...)` — this is where the camera transform is
  consumed each frame (relevant to camera-relative input, §5).

## 3. Objects & the player (what to write for rendering)

- `struct ObjectStruct` (`lib/bmhero/include/obj.h`): `Vec3f Pos`@0x00,
  `Vec3f Scale`@0x0C, `Vec3f Rot`@0x18 (**`Rot.y` = facing, degrees**),
  `Vec3f Vel`@0x24, `f32 moveAngle`@0x3C, `f32 moveSpeed`@0x44,
  `s16 actionState`@0xA4 (**animation/state — anim selection**),
  `s16 objID`@0xE4 (**model id**).
- `gObjects[207]` @ RAM `0x80154150`. `gPlayerObject` (`struct ObjectStruct*`,
  RAM `0x8017753C`) points at `gObjects[0]` after level load.
- **Free-slot spawning (A1.2b):** `Get_InactiveObject()` (`code/69AA0.c:434`)
  scans `gObjects[2..5]` for `actionState == ACTION_NONE` and returns a free
  index (−1 if none). **Only 4 spawnable slots (2,3,4,5)** — exactly enough for
  3 extra bombers (players 1–3) beyond the player at slot 0. If A1.2b needs more
  than that (bombs/blasts later), it must either use a wider pool or a
  spawn-suppression widen.
- Object spawn functions: `func_80027464(slot, ObjSpawnInfo*, x, y, z, rotY)`
  (`code/26CE0.c:328`) and `func_80027C00(...)` (`:410`); the per-object init is
  `gObjInfo[id].spawn()`. A1.2b will likely place a bomberman-model object into a
  free slot and puppet it like A1.2a does slot 0.

## 4. Input (driving the sim from the controller, in a patch)

- Globals (`variables.h`): `f32 gActiveContStickX`, `f32 gActiveContStickY`
  (N64 stick, range ≈ ±80), `u16 gActiveContButton` (held-button mask),
  `u16 gActiveContPressed` (edge). Masks: `CONT_A 0x8000` (jump), `CONT_B 0x4000`
  (bomb) (`PR/os_cont.h`).
- A1.2a maps `sx = stickX * 31/80`, clamped ±31 → `arena_input_pack`. Sim's
  "stick up = −Z" convention holds.
- Native alternative (if ever needed outside a patch):
  `recompinput::profiles::get_n64_input(player, &buttons, &x, &y)`.

## 5. Camera (the forward/back "compression" — A1.2 feel fix)

- `struct View gView` (`variables.h:609`), `struct View { Vec3f at; Vec3f eye;
  Vec3f rot; Vec3f up; f32 dist; }`. `gView.rot.y` is the camera yaw; the draw
  routine feeds `gView.eye/at/up` to `guLookAt`.
- **A1.2a finding:** the Battle Room camera is pitched, so world movement into
  the screen (Z) projects shorter than across it (X). Puppeting X/Z 1:1 makes
  forward/back read "compressed." **Blind Z-scale tuning went the WRONG way**
  (a guessed 1.4× boost looked *more* compressed), which means the camera's
  actual orientation must be *measured*, not assumed.
- **The correct fix (deferred to the feel pass):** camera-relative input — read
  `gView.rot.y`, rotate the stick vector by it before `arena_input_pack`, so
  "up on the stick" means "away from the camera" regardless of the camera angle.
  This is exactly what the SDL viewer's `vcam_stick_to_world` does. Measure
  `gView.rot` in the running Battle Room first (log it from the patch), then
  build the rotation from real numbers.

## 6. Coordinate mapping (sim Q20.12 → Hero float)

- **A1.2a approach that works:** don't teleport to absolute coords (floats/blue-
  screens on bad origin). Instead move the player *by the sim's per-frame
  displacement*: native `arena_export_tick_input` records
  `Δpos = pos_after − pos_before` per tick, scaled (`g_scale`, Hero units per sim
  unit ≈ 120); the patch adds `Δx/Δz` to the live `gPlayerObject->Pos` and
  **leaves `Pos.y` to the game** so it stays grounded and the camera follows. No
  spawn capture, no origin constant — only the scale (and eventually the
  camera-relative rotation) to calibrate.
- Yaw: `deg = binang * 360/65536`.
- In battle mode the render patch drives the tick (`arena_bridge_tick_input`);
  the free-running `vi_callback` tick is gated off (`if (g_battle_mode) return;`)
  so the sim advances exactly once per rendered frame, in lockstep.

## 7. Warp / map shell (A1.1b-ii, for reference)

- `gCurrentLevel` (`s32`, RAM `0x8016E428`) selects the map; `func_80081C50`
  (`71AA0.c:487`, 9 lines) seeds the next-level var + spawn from it just before
  the loader. `RECOMP_PATCH` overrides it to `ARENA_WARP_MAP` (currently
  `MAP_BATTLE_ROOM` = 2) when `arena_bridge_is_battle()`. Map IDs in
  `lib/bmhero/include/map_ids.h`; the dedicated battle rooms (2–6) load cleanly on
  direct entry. A bigger boss arena (Nitros) is a candidate side task but revives
  boss/spawn suppression + warp verification.
