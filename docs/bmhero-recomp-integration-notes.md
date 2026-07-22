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
- **Patches must be STATELESS** (A1.2b, 2026-07-21): a static patch cannot use
  file-scope mutable `static`/global variables it writes to — doing so aborts
  the game (`0xC0000409` / `FAST_FAIL_FATAL_APP_EXIT`) the moment the patch runs.
  The static-patch path doesn't set up patch-local `.data`/`.bss` (the mod
  loader does, but these aren't mods). Keep ALL mutable state in native
  `arena_bridge.cpp`, exposed via exports; the patch only reads game globals,
  calls exports, and writes game objects. Writing **game** globals/objects is
  fine (fixed RAM addresses). Memory: `recomp-patch-stateless`.
- **Crash forensics:** dumps land in `%LOCALAPPDATA%\CrashDumps\
  BMHeroRecompiled.exe.*.dmp` (WER). Analyze with `cdb`
  (`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`, from
  `winget install Microsoft.WindowsSDK`):
  `cdb -z <dmp> -y "srv*C:\sym*https://msdl.microsoft.com/download/symbols" -c
  ".ecxr; r; kb 20; q"`. No PDB for the exe, so recomp'd frames show as
  `BMHeroRecompiled+0xNNNNN`, but the fault type is decisive: **`0xC0000409`**
  (FATAL_APP_EXIT via abort/terminate) = the recomp/game hit an invalid op and
  bailed; **`0xC0000005`** = a wild pointer deref in recomp'd code. rdram maps
  as `rdram + (gameaddr - 0x80000000)` (`TO_PTR`, `ultra64.h`), so a native
  shim can read/write game RAM directly (raw byte copies between 4-aligned
  regions preserve byte order).

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

## 8. Object system & multi-actor rendering — A1.2b (draw path SOLVED; positioning open)

**Status (2026-07-21): A1.2b DONE with bomb placeholders.** All 4 players are
puppeted on screen — player 0 (the campaign object) + 3 extra actors spawned
into `gObjects[14..77]`, positioned from the sim, on a clean flat arena
(`MAP_NITROS_1`, boss suppressed). Stable, no mirror, no crash. The only piece
NOT done is swapping the bomb placeholder for the real bomber mesh (a skeletal-
model follow-up, §8.5b). Fork branch `feature/a1.2b-spawn-bombers`. Supersedes
the old "BLOCKED / no resident bomber model" analysis (a misdiagnosis — §8.7).

### 8.1 The working recipe (animated actor via general spawn)

1. **Spawn** into `gObjects[14..77]` with the game's own proper spawn
   `func_80027464(1, &info, x, y, z, rotY)` (`26CE0.c:328`): scans for
   `ACTION_NONE`, then `func_8001A928` (init) → `func_8001BD44` (load model into
   `Unk140`) → set Pos/Rot/`actionState=ACTION_IDLE`/`objID`, wire `unk10E`
   group links correctly. Returns the slot. **Synthesize `ObjSpawnInfo`** on the
   patch stack (`types.h:830`): `unk2`=objID (benign — see §8.4), `unk4`=file
   index (`9`=bomb / `1`=bomber), `unk0`/`unk6`=`func_8001BD44` cfg args.
2. **Bind the animation** — the missing piece. `func_80027464` loads model PARTS
   (`Unk140`) but NOT the animation instance an animated model needs; the draw
   then aborts (`0xC0000409`). Add **`func_8001ABF4(slot, 0, 0, cfg)`**
   (`17930.c:943`) after the spawn: it allocates a slot in the `D_8016C298`
   animation pool (256 slots, `func_8001AA60`), stores it in `Unk148[]`, and
   `func_8001AD6C` advances it each frame. For the bomb, `cfg` = `D_801163DC`
   (its anim config table). **This one call is what unblocks the draw.**
3. **Position** each frame from the sim (§8.3) — but see the open item §8.5.

### 8.2 Two hard patch gotchas (both cost hours here)

- **Auto-named DATA symbols don't resolve in patches.** Game symbols resolve at
  patch *load* via relocs (`--emit-relocs`, `--unresolved-symbols=ignore-all`;
  nothing is in `patches.map`). FUNCTIONS (`func_*`) and NAMED globals
  (`gObjects`, `gPlayerObject`) resolve fine, but an auto-named `D_xxxx` data
  symbol (e.g. `D_801163DC`) does **not** — the unresolved reloc silently
  corrupts the whole patch, crashing at LOAD (even the `RECOMP_PATCH`
  `func_800824A8` breaks). **Symptom: zero in-level markers log at all.**
  **Fix:** reference game data by *literal address* —
  `#define D_801163DC_ADDR ((struct T*)0x801163DC)` — no reloc; the recomp
  translates the address on deref inside the callee.
- **The export ABI reads integer/pointer arg slots only.** `_arg<N,T>` reads
  `(&ctx->r4)[N]` (GPRs). Float ARGS need special-case helpers
  (`_arg_float_a1`/`_f14`) and are fragile. **Pass floats as `u32` bit patterns
  through int args:** `union {f32 f; u32 u;}` in the patch → `(s32)u.u` → native
  `union{u32 u; float f;}` reinterprets. Float RETURNS via `_return` work fine.

### 8.3 Positioning — frozen origin (no mirror)

`obj[i].Pos = origin + (sim_pos_i − sim_ref)·scale`, where `origin` =
`gPlayerObject->Pos` **frozen at spawn** and `sim_ref` = sim player-0's pos
frozen at spawn — NOT the live `gPlayerObject` (which also moves under the game's
own player physics, so anchoring to it *mirrors* the player). State lives native
(`arena_puppet_capture` via the u32-bitcast of §8.2; `arena_puppet_wx/wy/wz`
getters). Yaw `deg = binang·360/65536`.

### 8.4 objID drives behaviour AND collision — the map matters

The per-frame update loop `func_8002B154` (`26CE0.c:1205`) runs, for each active
`gObjects[14..77]`: `gObjInfo[objID].behaviour()` + `func_8001CEF4` +
`func_8001CD20` (collision/physics) + `func_8001AD6C` (anim). Our patch runs
*after* it (`func_80024744` first), so behaviour+collision only need to not
CRASH, not be no-ops.
- **Real objID behaviours have side-effects.** `OBJ_RPLATE` (85) **teleports the
  player** when spawned on it; `OBJ_TOBIRA1_O` (77, the room's door) runs
  "close on entry". A truly inert objID (behaviour == empty `func_8002B144`,
  `26CE0.c:1199`) is still TBD — scan `gObjInfo` @ `0x80124D90` for it.
- **Collision (`func_8001CD20`) runs regardless of objID.** An actor positioned
  OFF the platform / over a pit likely aborts here — the leading theory for §8.5.

### 8.5 RESOLVED — positioning crash was the pits; fixed by a flat arena + boss sweep

- **Root cause:** the Battle Room (`MAP_BATTLE_ROOM`=2) is a central platform
  surrounded by **pits**. Positioning actors to the sim corners (scale 120 →
  ±540 units spread) put them off the platform, where the game's per-object
  collision (`func_8001CD20`, run on every active `[14..77]` object regardless
  of objID) aborts — a crash whose point *wandered* run-to-run.
- **Fix 1 — flat arena.** `ARENA_WARP_MAP` in `patches/arena_warp.c` now = `15`
  (`MAP_NITROS_1`, a Nitros boss room — a big flat open floor, no pits). Loads
  clean via the existing `gCurrentLevel` override (the warp is already a
  direct-map bypass; no world/area setup needed). On flat ground the 3 actors
  position + draw stably.
- **Fix 2 — boss suppression.** A Nitros room spawns its boss, whose per-frame
  behaviour is also flaky. In `arena_render_routine`, **before** `func_80024744`
  (the update loop) runs, deactivate every `gObjects[14..77]` that isn't one of
  our 3 puppet slots (`actionState = ACTION_NONE`). The floor is map geometry
  (not in `gObjects`), so it survives. Result: clean 4-actor arena, stable.
- Confirmed on screen: player + 3 bomb actors at the sim corners, holding
  position as the player moves (no mirror), `simpos` advancing.

### 8.5b FOLLOW-UP — real bomber mesh (skeletal model, deferred)

The actors are **bomb placeholders** (`gFileArray[9]` + `func_8001ABF4` bomb
config — single-part, one clean anim bind). Swapping to the real **bomber mesh**
(`gFileArray[1]`, `func_8001BD44` cfg `0x13`) + a bomber anim config
(`D_80101E8C` @ `0x80101E8C`) **spawns fine but the draw white-screens** (RSP
abort on a malformed model). The player-bomber is a **multi-part SKELETAL
model** — `code/4DFF0.c` loads it in a LOOP (`func_8001BD44(i, 0,
D_80134794->unk34[i], gFileArray[1].ptr + D_80134794->unk14[i])`, per-part
offsets from `D_80134794` @ `0x80134794`), and needs per-part skeletal anim
binds, not one texture-anim config. Real bombers = replicate that multi-part
load + skeletal binds + pick a neutral idle pose. Its own focused RE task.

### 8.6 The two object pools (don't cross them)

- `gObjects[2..5]` = the **bomb pool** (`Get_InactiveObject`, `69AA0.c:434`);
  drawn by a player/bomb-specific path. Do NOT spawn generic actors here.
- `gObjects[14..77]` = the **generic pool** — `func_80027464` spawns here; drawn
  by `func_8001C464`/`func_8001C5B8` (`17930.c:1236/1257`, follow `Unk140`);
  updated by `func_8002B154`. This is where actors go.
- **Mesh is separable from objID:** `func_8001BD44(slot, cfgA, cfgB,
  gFileArray[idx].ptr)` loads any mesh into `Unk140`; objID only drives
  behaviour + render params. Bomber mesh = `gFileArray[1]` (cfg `0,0x13`, per
  `overlays/13AC20/13AC20.c:422`); bomb = `gFileArray[9]` (cfg `0,0`). Both are
  low-index core assets **resident in every level** (that's why the player can
  throw bombs anywhere) — confirmed live: `gFileArray[1]`=`0x8028b720`,
  `[9]`=`0x802c7f30`, both non-null in the Battle Room.

### 8.7 Superseded / historical (dead ends, don't retry)

- **"No resident bomber model" was a MISDIAGNOSIS.** The bomber mesh
  (`gFileArray[1]`) is resident; the real blockers were the animation-instance
  binding (§8.1.2) and the data-symbol reloc (§8.2).
- Raw `memcpy`-cloning door/plate → crash (copies `unk10E` group links pointing
  at the source's slot group). Cloning the player object → crash (single-player
  update logic). A *fresh* `func_80027464` spawn avoids both.

### 8.8 Tooling (this session)

- **Screenshots regardless of focus/occlusion:** `PrintWindow(hwnd, hdc, 2)`
  (`PW_RENDERFULLCONTENT`) captures the RT64/Vulkan window even when it's behind
  other windows — unlike `CopyFromScreen`. Read the PNG directly.
- **In-patch marker logging:** `arena_dbg_u32(tag, val)` → `arena_bridge.log`
  (flushed each call, survives a subsequent crash). No markers logging at all ⇒
  crash at/before that point or during load (see §8.2 data-symbol trap).
- **Hands-off keyboard input to the game is UNRELIABLE** — SDL only takes keys
  when its window has focus, and a background script can't hold the foreground
  from inside the agent session. Working loop: agent builds + verifies
  (screenshot + log); a human does the ~15s launcher→room nav.

### 8.9 Draw/update gating & the neutral-input bug (still true)

- **Draw vs update are separately gated** (`boot/17930.c`): `gDebugRoutine1()`
  (draw, line 1562) and `gDebugRoutine2()` (update, line 1797, our
  `arena_render_routine`) — draw runs *before* update in a frame and can fire
  without it, so an actor's draw must not assume its update already ran.
- **Neutral-input bug (fixed A1.2b):** raw `ArenaInput` `0` is NOT neutral —
  `arena_input_pack` offsets the stick by 32, so raw 0 = a full `(−32,−32)`
  stick. Use `arena_input_pack(0,0,0,0,0)` for idle players.
