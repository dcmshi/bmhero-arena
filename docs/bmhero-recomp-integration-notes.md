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

### 8.5a PRINCIPLE — the sim's collision geometry MUST match the rendered map (2026-07-24)

When the sim is puppeted onto a **real game map** (the Nitros room stand-in), the
sim's arena geometry (`arena_geom.h`: `half_extent`, `pillars`) **and** the render
scale (`g_scale`) together decide where the player is ALLOWED to move — and that
must line up with the actual walkable floor of whatever map `ARENA_WARP_MAP` points
at, or the player hits **invisible walls / stops mid-floor**. Feel-test 2026-07-24
exposed this: the sim's designed arena0 (a 12×12 ring + 4 central pillars) imposed,
on the open Nitros floor, a boundary that pinned the player at a sim corner (±5.65
sim = ±678 Hero units — confirmed in `[simpos]`) plus 4 invisible mid-arena
collision boxes; nothing was visible at any of those stop points. Two ways to keep
them in sync: **(a)** size/shape the sim arena to the rendered map's real collidable
bounds (measure them — drive the player to the map's own walls with the sim clamp
relaxed and read the extent from the live `gPlayerObject->Pos`), or **(b)** render a
map whose geometry matches the sim's designed arena. Until the real battle arena is
built + rendered, the sim geometry is a **stand-in that must track the warp map** —
changing `ARENA_WARP_MAP` means re-matching the sim geom. (Related: §8.5's pits
crash was the same class of sim/render geometry mismatch.)

### 8.5b Real bomber mesh — A1.2d verdict: mesh resident, ANIMS are not (2026-07-22)

**The slice closed at its decision gate.** The bomber MESH loads and the whole
recipe is now understood, but its ANIMATION data is not reachable in the arena
— and drawing the skeletal mesh without an anim instance white-screens (the
A1.2b RSP abort). Puppets remain bomb placeholders. Everything below is
evidence-backed (log tags + symbolized dumps, 2026-07-22).

**The corrected recipe (what a bomber needs):**
- The bomber is **ONE object, ONE part** — NOT a multi-part load. (The old
  "multi-part loop" reading of `4DFF0.c:424` was a misread: that loop loads up
  to 8 *demo actors*; `D_80134794` is the demo-scene descriptor.)
- `func_8001BD44(objId, part, cfg, src)` (`boot/17930.c:1144`): `part` indexes
  `gObjects[objId].Unk140[part]`; idempotent; each load takes one `D_80165290`
  model-pool slot; `modelTag = func_80010408(src, cfg)`. Bomber mesh = whole
  `gFileArray[1]`, cfg `0x13` (the spawner does this itself via
  `ObjSpawnInfo.unk4=1/.unk6=0x13` — `func_80027464` → `26CE0.c:364`).
- Generic pool [14..77] draws only `Unk140[0]` and `[3]`
  (`func_8001C464`/`func_8001C5B8`); pool [78..141] draws 0,1,3.
- The missing piece vs. the bomb recipe is the **model-anim instance**
  (`D_80165290[modelSlot].unk20`, bound via `func_8001C0EC` → `func_8001BE6C`
  → `func_8001191C`), plus the usual `Unk148` texanim (`func_8001ABF4(objId,
  channel, mode, cfg)` — 4 channels, `D_8016C298` pool, advanced by
  `func_8001AD6C`).

**All three anim-data sources fail in the arena (each disproven):**
1. **Menu stream table `D_80115F34`** (the 13AC20.c menu/mirror bomber path):
   points at garbage in-level. `gFileArray[1]` is **byte-identical** in
   MAP_NITROS_1 and MAP_MIRROR_ROOM (base `0x8028b720`; hdr words at
   +0x3BC8 = `0xBF000000`,`0x0006080A`); `func_800122F0` parses section
   counts/alloc sizes straight from that data → a ~395k "count" → **endless
   `malloc_game` walk** (the ~1.5s freeze; symbolized hang stack:
   `malloc_game ← func_800122F0 ← func_8001BE6C ← func_8001C0EC`). The menus
   evidently run against a different file-1 context.
2. **`gObjInfo` registry** (`0x80124D90`; what the debug binder
   `func_8002BE04`, `2BF00.c:227`, keys off `objID`): the bomber-ish entries
   (392 `OBJ_MIR_BOMBER`, 614 `OBJ_EVBOMBER`, 621 `OBJ_EVS_BOM`, 6
   `OBJ_BOMBER7`) have **NULL `unk38`/`animPtr` in every warpable arena** —
   the registry is populated per-level with the level's roster.
3. **Embedded model anims** (demo-style null-source bind
   `func_8001BE6C(slot,0,0,0)`): cfg `0x13`'s modelTag has **no anim
   directory** — `func_8001191C` AVs (symbolized dump).

**Future lead (start here next time):** player 0 **animates in the arena**, so
valid bomber anim data IS resident via the dedicated player path. Trace who
binds `gPlayerObject`'s anim instance
(`D_80165290[gObjects[0].Unk140[0]].unk20`) at level load and reuse that
source — or share/copy the live instance.

**End state in the fork (`feature/a1.2d-bomber-mesh`):**
- Puppets spawn as bombs; a **null-guarded gObjInfo candidate scan** runs at
  spawn (log tags 50–53 spawn-info ptrs, 60–63 animPtrs, 43 pick) and will
  self-activate the registry recipe in any level that carries a bomber entry.
- **`arena_spawn_gate`** (native, `0x8F0001C0`): the one-shot spawn block waits
  ~90 routine invocations — the routine's first calls run **inside level-enter**
  (`func_800824A8` → `func_80000964` → routine), where `malloc_game` is not
  serviceable.
- Deref discipline: in recomp'd patch code a null/non-KSEG0 pointer deref is a
  **host access violation** (`rdram + (addr − 0x80000000)`), not a harmless
  garbage read — null-check game pointers (many `gObjInfo[].unk38` are NULL).

**Map notes:** arena stays `MAP_NITROS_1` (15). `MAP_MIRROR_ROOM` (71) is a
direct-warp land mine: unregistered function pointer in `func_8001D9E4` at
level-enter (`get_function` → exit; symbolized dump). Battle Room (2) had the
pit/collision aborts (§8.5).

**Related stability fix (the slice's big win):** the "~⅓ of boots" stochastic
load crash had **THREE `recomp_printf`-in-load-window sites**, all now
disabled: `required_patches.c` `load_from_rom_to_addr` (§8.9),
`3d_object_hook.c` `func_800608B8`, and our own `arena_warp.c`
`func_80081C50`. Chain (all three identical): `_Printf` indirect call →
`get_function` "Failed to find function" → `exit(1)` → atexit
`thread_cleaner_thread` dtor → `0xC0000409`. **Never `recomp_printf` in the
load window.** Loads verified stable since.

**Facing (still true, visually unverified — bombs are symmetric):**
`arena_puppet_yaw(i)` returns degrees (`yaw * 360/65536`), written to
`gObjects[slot].Rot.y` each frame after `func_80024744()`; same convention as
player 0 (on-screen since A1.2a).

### 8.5c Player action animations — A1.4 RE (set-bomb recovered; ~~kick has no anim~~ — see 8.22; 2026-07-24)

**Goal:** play the game's own player action anims (set-bomb, kick) on `gPlayerObject`
when the sim registers those events, and **auto-verify via the harness** (read back the
live anim index/frame — no eyeballing). Decomp/recomp-only pass (zero boots). Sources:
hand-decomp `lib/bmhero/src` + the recomp's machine-C `RecompiledFuncs/funcs_*.c` (which
DOES contain the un-migrated player walker overlays — the key unlock this slice).

**The trigger primitive (HIGH).** `func_8001C0EC(objID, part, animIdx, fileID, table)`
(`boot/17930.c:1187`): computes `src = &gFileArray[fileID].ptr[table[animIdx]]` and calls
`func_8001BE6C(objID, part, animIdx, src)` (`17930.c:1159`), which **frees + re-mallocs
only the small per-anim instance** (`D_80165290[slot].unk20`, via `func_8001191C`
`10AB0.c:431`) and sets `unk14 = animIdx`. It **re-points, does not reload** the model
(the model slot `Unk140[part]` and `modelTag` are preserved). `gPlayerObject == &gObjects[0]`.

**The arena player runs `code_extra_0`, table `D_80115808` (bank/fileID 1) — NOT
`code_extra_1`.** The per-world player controller is picked by `D_8016523E`
(`code/76640.c:911`); the arena warps to a normal campaign level (`MAP_NITROS_1`), which
uses **world 0 = `code_extra_0`** (overlay `128D20`). **Proof:** the fork's own
`patches/teleporter_obj.c` is a `RECOMP_PATCH` of `func_80284668_code_extra_0` that calls
`func_8001C0EC(0, 0, 7, 1, &D_80115808)` on the live player. `D_80115808` is a **53-entry**
table (anim idx 0-52); `code_extra_1`'s `D_80115CF8` is a disjoint 25-entry table (different,
smaller anim file). **Correction:** an earlier pass keyed on `code_extra_1` (set = idx 11,
table `0x80115CF8`) — that is the WRONG overlay for the arena; its indices do NOT transfer.

**`D_80115808` resolves directly in patches** (it's in `data_dump.toml`, proven by
teleporter_obj.c using `&D_80115808`) — so no literal-address workaround (§8.2) is needed
for the table. The set trigger is the exact proven form, just a different index:
```c
extern s32 D_80115808[];
func_8001C0EC(0, 0, 29, 1, &D_80115808);   /* set/drop-bomb pose, state 0x0E */
```

**Set-bomb pose = anim 29 (MEDIUM; harness-confirm).** `code_extra_0` state `0x0E` =
`PLAYER_ACTION_DROP_BOMB_0` (`code/69AA0.c:6`), handler `func_80282E5C_code_extra_0`
(`RecompiledFuncs/funcs_53.c:4830`) plays `func_8001C0EC(0,0,29,1,&D_80115808)`. MEDIUM
because the state↔index binding is read from machine-C, not visually confirmed; the harness
read-back (below) is the objective gate — and the game's OWN walker sets 0x0E/anim 29 on a
Z-press in-arena, so pressing set and reading `func_8001B880(0,0)` discovers/confirms it.

**~~Kick has NO player animation (definitive).~~ WRONG — kick poses exist at 32/33
(corrected 2026-07-27, see 8.22).** The code reasoning below is still accurate and is
worth keeping, because it shows exactly how the conclusion overreached: the walk-in kick
is indeed **100% bomb-side** — `func_8007AD60` (`69AA0.c:877-917`) does the bomb
`0x24→0x27` slide (reads `gPlayerObject` for facing but NEVER writes it or calls the anim
primitive), and `69AA0.c` contains **zero** `func_8001C0EC` calls. All true. The error was
the leap from *"no code path plays a kick anim"* to *"no kick anim exists"*. Unreferenced
assets are still assets: `D_80115808` is a 53-entry table and nothing requires the shipped
game to reach every entry. Flipping through all 53 rendered poses (8.22) found clear kick
poses at **32/33**, which the search had ruled out on the strength of the call-graph alone.

**The methodology lesson:** a call-graph search answers *"is this used?"*, never *"does
this exist?"*. For an ASSET, enumerate the asset table and look. That is one run of a
contact sheet, and it would have settled this in the original pass.

**Read-back for auto-verify (getters resolve as functions in patches — no `D_80165290`
deref needed).** With `slot = gObjects[0].Unk140[0]` (part 0 = body):
- `func_8001B880(0,0)` → current anim index (`unk14`) — `17930.c:1088`
- `func_8001B62C(0,0)` → frame counter (`unk24`, advances +2.0/frame, rounded even) — `17930.c:1071`
- `func_8001B44C(0,0)` → nonzero when the clip finished (`unk16 & 2`) — `17930.c:1048`
Verify a triggered set: index → 29 AND the frame counter advances. `D_80165290` is the
256-slot model-anim pool (`variables.h:589`, element `0x70`; `types.h:309`); `Unk140` is
`s16[4]` @ obj `0x140` (`obj.h:128`), the slot index into that pool.

**Full `code_extra_0` action map** (objID 0, bank 1, table `D_80115808`; state 0xA4 →
animIdx): the actions occupy states `0x0B`-`0x36`, anim idx up to ~52 — set/drop `0x0E`→29,
throws `0x17/0x19/0x27/0x28/0x2C/0x30/0x31/0x33/0x35`→{34,36,38,39,42,47…}, impact/hurt
`0x1B-0x22`→{43,44,47,48,49}, warp `0x2E`→7, idle 0, locomotion 1-8. (Machine-C derived;
throw/impact/warp HIGH via called helpers, the rest inferred from clip ordering.)

**SHIPPED (A1.4, fork `feature/a1.4-set-kick-anims`, 2026-07-24 — spec
`docs/superpowers/specs/2026-07-24-a1-4-set-kick-animations-design.md`).** Set-bomb pose
wired end-to-end and harness-verified. A bridge set-edge export (bomb `FREE`->`SETTLED`,
`owner==i` — mirrors `arena_blast_new`, no sim change, hash `5f500fcb` held) drives
`func_8001C0EC(0,0,29,1,&D_80115808)` on player 0's sim set edge; per-frame read-back via
`func_8001B880`/`func_8001B62C` -> `arena_dbg_anim`; probe mode `ARENA_AUTO_BATTLE=4`
presses Z AFTER the sim's 180-tick countdown (set only fires in `PHASE_PLAY` — the initial
probe pressed too early and no bomb was placed) and `arena-soak.ps1 -AnimProbe` asserts the
gate. VERIFIED: idx 29 with the frame counter advancing 0->14 (13 samples), bombs live=2/3,
**5/5 boot-soak green**. The set anim plays out — the walker does NOT stomp it same-frame.
Kick keeps locomotion (no game anim). Human feel-boot pending as the final polish confirm.

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

### 8.8 Tooling (verification loop)

- **Screenshots regardless of focus/occlusion:** fork `tools/capture-game.ps1`
  (`PrintWindow(hwnd, hdc, 2)` / `PW_RENDERFULLCONTENT`) captures the
  RT64/Vulkan window to `game.png` even when it's behind other windows —
  unlike `CopyFromScreen`. Read the PNG directly.
- **Symbolized crash dumps:** launch via fork `playrwdi.bat` (the
  `build-rwdi` RelWithDebInfo exe + PDB); analyze the newest
  `%LOCALAPPDATA%\CrashDumps\*.dmp` with `cdb -z <dmp> -y "srv*C:\sym*https://
  msdl.microsoft.com/download/symbols;<fork>\build-rwdi" -c ".ecxr; kc 20; q"`
  — frames name real recomp'd game functions. This is what cracked the load
  crash (§8.9).
- **In-patch marker logging:** `arena_dbg_u32(tag, val)` → `arena_bridge.log`
  (flushed each call, survives a subsequent crash). No markers logging at all ⇒
  crash at/before that point or during load (see §8.2 data-symbol trap).
- **HANGS produce no crash dump — attach to the live process instead**
  (A1.2d's decisive tool): `cdb -p <pid> -y "<symsrv>;<fork>\build-rwdi"
  -c "~*kc 22; qd"` dumps every thread's stack with real function names and
  detaches harmlessly. This named `malloc_game ← func_800122F0 ← …` in one
  shot. Gotchas: a hang can look like a crash to the player (window closed);
  check `Get-Process BMHeroRecompiled` FIRST — `Responding=True` just means
  the window pump lives. Attach error 87 ⇒ a zombie cdb is still attached —
  kill stray `cdb` processes and retry.
- **Forensic-marker pattern for suspected hangs:** log the *inputs* of a
  dangerous call (pointers, table offsets, header words) with `arena_dbg_u32`
  BEFORE making it — the flushed log survives the hang and tells you what the
  callee saw (this is how the `D_80115F34` garbage header was proven:
  tags 45/46/47 = file base / offset / parsed count). Corollary: **validate
  stream headers before trusting them** — `func_800122F0` mallocs sizes read
  straight from data; a sanity guard (`1 <= count <= 16`) turns an infinite
  hang into a logged no-op.
- **A hung/running game locks the exe** — `lld-link: permission denied` on
  the final link means kill `BMHeroRecompiled` first (it's often a forgotten
  hung instance).
- **Two-build discipline during crash hunts:** iterate on `build-rwdi`
  (symbolized) only, and rebuild `build-cmake` (Release, `play.bat`) once at
  the end — both share `patches/`, so `make -C patches clean` stays mandatory
  between patch edits either way.
- **Hands-off keyboard input to the game is UNRELIABLE** — SDL only takes keys
  when its window has focus, and a background script can't hold the foreground
  from inside the agent session. Working loop: agent builds + verifies
  (screenshot + log); a human does the ~15s launcher→room nav.

### 8.9 A1.2c slice 2 findings — effects, blasts, and THE load-crash fix (2026-07-22)

- **THE stochastic load crash is FIXED.** The "black screen selecting battle"
  that hit ~⅓ of arena boots all along (and contaminated the pool-ceiling
  bisect, the spike attempts, and the neuter experiment) was an **upstream
  debug print**: `required_patches.c` `load_from_rom_to_addr`'s `recomp_printf`
  fires per ROM section load and races the loader. Symbolized RWDI dump named
  the chain: `_Printf` indirect call → `get_function` "Failed to find function"
  → `exit(1)` → atexit `thread_cleaner_thread` dtor → `terminate` →
  `0xC0000409`. Disabled (commented out) — loads are stable since.
  **Corollary: the "pool ceiling ~6-8 actors" finding is INVALID** (those
  crashes were this race); larger pools were never re-tested.
- **Game effect spawner (`func_80081468(id,x,y,z)`) is NOT usable in the
  bypassed arena** (candidate for the real explosion visual): effects spawn and
  live but render **invisible** (per-level effect assets missing?), and several
  IDs crash — `0x2C1` (rain) via its lifecycle, `0x2C7` reproducibly in the
  effect-list draw (`func_8001CDF4` ← `func_800818CC` ← the draw routine).
  IDs `0x2BC/0x2BD/0x2C5` spawn+live harmlessly but show nothing. Using it
  would need the effect-asset/init RE — deferred.
- **Blast visual v1 (shipped): pooled "pop" actors.** 4 blast actors (bomb mesh
  + anim bind, proven recipe) driven from `blasts[]`: shown at each live
  blast's center for its 20-tick life, hidden after. Reads as a bomb-pop at the
  detonation site (set-bomb pops are under the player — cosmetically hidden).
- **The generic `[14..77]` draw IGNORES `ObjectStruct.Scale`** — writing
  Scale 1→12× produced no visible growth (the bomb-pool `[2..5]` draw path
  does use Scale). So size-animated visuals need another mechanism.
- **Boss re-activates if the sweep stops** (tested: entry-window-only sweep →
  boss returned) — the every-frame sweep stays. Neutering the level's
  `unk24/unk28` spawn hooks instead white-screens (they also do draw setup).
- **Tooling:** `build-rwdi` (RelWithDebInfo, PDB) + `playrwdi.bat` — crash
  dumps symbolize (this is what cracked the load crash); machinery reference:
  `docs/bmhero-recomp-patch-machinery-reference.md`.

### 8.10 Draw/update gating & the neutral-input bug (still true)

- **Draw vs update are separately gated** (`boot/17930.c`): `gDebugRoutine1()`
  (draw, line 1562) and `gDebugRoutine2()` (update, line 1797, our
  `arena_render_routine`) — draw runs *before* update in a frame and can fire
  without it, so an actor's draw must not assume its update already ran.
- **Neutral-input bug (fixed A1.2b):** raw `ArenaInput` `0` is NOT neutral —
  `arena_input_pack` offsets the stick by 32, so raw 0 = a full `(−32,−32)`
  stick. Use `arena_input_pack(0,0,0,0,0)` for idle players.

### 8.11 Input & camera truth (A1.2e, 2026-07-22)

- **The game itself makes the stick camera-relative.** `func_80024744`
  (`boot/21E10.c:826`) — the routine our render wrapper calls — rotates
  `gActiveContStickX/Y` IN PLACE by `gView.rot.y` (guRotateF y-axis →
  guMtxXFMF) for `gCameraType ∈ {1,2,5,6,7,8}`. The Nitros arena is **type 6**
  (probe: raw (0,80) → (43.5,68.0) = exactly rot.y 34°). The raw pass-through
  read AFTER that call is therefore already camera-relative; adding a
  gView-based rotation on top DOUBLE-rotates (measured: forward speed cut to
  ~⅓ — the "slow up/down" symptom). **If the arena map ever changes, log
  `gCameraType` first** — outside that set the stick arrives unrotated (the
  likely truth behind A1.2a's Battle-Room-era "compressed" note).
- `struct View` (`types.h:257`): **`.at` @0x00 BEFORE `.eye` @0x0C** (then
  rot/up/dist). `rot.y` = the canonical camera yaw in degrees; the arena rail
  camera swings 25–89° across the room and CUTS (eye teleports ~1800u).
- **Facing — SOLVED (A1.3, 2026-07-23): copy the game's own `moveAngle` 1:1.**
  `gPlayerObject->Rot.y = gPlayerObject->moveAngle`. The game's player update
  (`func_80024744`, which our render wrapper calls) computes `moveAngle`
  authentically from the camera-rotated stick every frame; it's live and
  correct in the bypassed arena (probe-verified: `moveAngle =
  Math_Atan2f(Vel.x,Vel.z)`, e.g. 218.3 for Vel(−11.16,−14.12)). We drive
  POSITION from the sim but borrow the game's facing — authentic by
  construction, user-confirmed. History (dead ends, don't retry): deriving
  `Rot.y` from our **sim yaw** (`180−yaw`, then ±90 offsets) read 90° then 45°
  off and *inconsistent* — because sim yaw turns gradually (A1.3 turn rate) so
  a yaw-derived facing LAGS the real movement during turns; deriving from the
  per-frame `dx/dz` via `Math_Atan2f` also fought angle-convention mismatches.
  The game snaps `moveAngle` while our sim eases the turn, so facing may lead
  the body slightly mid-turn — cosmetic, accepted. Puppets (players 1–3) can't
  use this (not `gPlayerObject`, positioned absolutely) — they keep a
  yaw-placeholder, invisible while they're symmetric bomb-mesh placeholders
  (revisit with the real bomber mesh). `Math_CalcAngleRotated(stickX,−stickY)`
  (`2BF00.c:488`) remains the stick→moveAngle formula of record.
- **Ground bombs rendered below the floor**: bomb/blast `wy` is now clamped to
  `origin_y` (native bridge) — set bombs were sim-live (`live=1..3` on the
  user's Q presses) yet invisible; thrown bombs arc above the floor and always
  showed.
- `sqrtf` is usable in patches (lowers to MIPS sqrt.s under -ffast-math) but
  ALWAYS verify no `U sqrtf` remains: `"...VC\Tools\Llvm\x64\bin\llvm-nm.exe"
  patches\arena_render.o | findstr sqrt` must be empty (the portable LLVM15
  lacks llvm-nm; an emitted libcall links silently and jumps to 0).

### 8.12 The func_map race, its fix, and the boot-soak harness (A1.2e/f, 2026-07-22)

- **Crash class root cause:** the runtime's function map mutates during
  overlay load/unload while the game thread makes indirect calls (`jalr` →
  `get_function`); a lookup losing the race exits ("Failed to find function"
  → `exit(1)` → `0xC0000409`). Confirmed callers: three load-window
  `recomp_printf` sites (disabled) and the **draw dispatcher's
  `gDebugRoutine1()`** (8 symbolized dumps).
- **The fix that works:** direct-dispatch in `required_patches.c`'s existing
  `func_8001D9E4` patch — `if (gDebugRoutine1 == &func_800821E0)
  func_800821E0();` (host-linked, no lookup) else stock indirect. **Gating
  around the window CANNOT work:** the level-enter pump (`func_800824A8` →
  `func_80000964`) runs 30–90+ routine frames synchronously, so counter/NULL
  gates reopen in-pump — reset-gating made the crash DETERMINISTIC (0/10
  soak); direct dispatch took the same flow to 10/10.
- **Boot-soak harness (`tools/arena-soak.ps1` + `ARENA_AUTO_BATTLE` env):**
  `1` = auto-battle + synthetic START/A frontend mash (injected at the
  input-callback level in `main.cpp` — no window focus needed), `2` = auto
  only, `3` = probe (post-entry stick/button injection for movement/set
  checks). PASS = `[capture]` in a fresh `arena_bridge.log` (the game rewrites
  it per process). The mash stops on the `arena_routine_seen` latch (first
  `tick_input` call) — stopping any later risks an in-level START/pause
  livelock (seen on screen). Auto-battle must fire from the launcher UPDATE
  callback (~frame 60), not init (dies ~11s, dumpless).
- **Probe collateral (un-neutralized arena hazards, own slice):** the boss
  room has a live **level-exit trigger** (north — walking in transitions
  levels) and **damage tiles** (corners); both exercise reload/death paths
  that the bypassed arena has never been hardened for.
- **Process rule (user-set):** never hand a build to the human without a
  green soak ON THAT EXACT BUILD (`-N 10`, exit 0). Human boots are for feel,
  facing, and animation judgments only — everything else goes through the
  harness first.

### 8.13 Render-drive = ABSOLUTE (not delta); bomb-render un-pile; camera (A1.4, 2026-07-24)

**Render-drive: player 0 is driven by the sim's ABSOLUTE mapped position, NOT a
per-frame delta.** A1.2a drove player 0 by adding the sim's per-frame displacement
to the live Pos (`Pos += dx/dz`). The A1.4 movement feel-test exposed that this
DOUBLE-drives the player: the game's own walker (`func_80024744`, run just before)
ALSO moves the player from the same stick, and our delta layers on top. The two
position integrators drift apart, and when the SIM player jams against its (small)
arena wall (dx/dz -> 0) the GAME player keeps coasting -> intermittent mid-floor
slowdowns / "running into something". Per-frame `[mv]` trace (sim pos vs rendered
dx vs yaw vs speed) nailed it: `simx` pinned at the wall with `spd=0.15` but
`dx=0`. FIX: drive player 0 by the sim's ABSOLUTE mapped position, exactly like the
3 puppets (`arena_puppet_wx/wz(0)` = frozen origin + (sim_pos - sim_ref)*scale) -
the sim solely owns X/Z, no co-drive. Y is still left to the game (grounding);
the camera still follows Pos. **Capture the origin/ref EARLY** (once, after the
~30-frame draw-gate) - the old 90-frame spawn-gate capture froze the origin AFTER
the player had already drifted ~1400 Hero units (origin swung -930 vs +437 between
sessions). Spawn-once latch for the actor pool is decoupled from the origin latch
(`spawn_gate() && puppet_get_slot(1) < 0`). Result: pervasive jamming gone
(user-confirmed); residual = camera drift + foreshortening (below), both
stand-in artifacts.

**Bomb-render un-pile (set bombs were invisible).** Symptom: set bombs placed in
the sim (`[setdbg]` confirmed) but nothing on screen. Root cause (per-frame
`[setdbg]` slot log): every bomb AND blast actor was spawned then immediately set
`ACTION_NONE` to "start hidden" - but `func_80027464` scans for `ACTION_NONE`
slots, so each hidden actor was REUSED by the next spawn -> all ~10 collapsed into
ONE gObjects slot (slot 17); the blast render loop then set that shared slot
`ACTION_NONE` whenever no blast was live, hiding the bomb the bomb-loop had just
shown. FIX: (1) DROP the A1.2c fallback blast actors (they caused the hiding + ate
model-pool budget; explosion visual deferred - revisit by reusing a bomb's OWN
actor as its blast on detonation to stay under the ceiling); (2) spawn the bomb
pool WITHOUT `ACTION_NONE` so each takes a DISTINCT slot (the per-frame loop hides
inactive ones the same frame -> no flicker); (3) `BOMB_POOL` 6 -> 4 so 3 puppets +
4 bombs = 7 actors stay under the **~8-actor model-pool ceiling** (§8: the
suppressed Nitros boss holds slots that `ACTION_NONE` doesn't free; the old
pile-up masked this by using only ~4 distinct slots). Verified: distinct slots
17/18/19, 32s run with bombs set+detonating = 0 crash dumps, bomb draws
(screenshot). A 5th/6th simultaneous live bomb won't render until the boss slots
are actually freed.

**Verification tooling added this slice:** per-frame `[mv]` movement log (sim pos
+ rendered delta + yaw + speed); probe modes `ARENA_AUTO_BATTLE=4` (set-anim: run +
timed Z press) and `=5` (arena-measure: 4-direction sweep for the floor bounds);
`[setdbg]` set-bomb slot log; and a **dump-tracked crash-check** (launch, dwell N
seconds, assert process alive + 0 new WER dumps) - the plain dwell-soak reports
PASS on `[capture]` BEFORE a dwell-crash, so it can mask crashes that happen during
the probe window; the crash-check catches those.

**Camera (both are Nitros RENDER-STAND-IN artifacts, not sim bugs):**
- **Drift** - the Nitros boss room has a rail camera that swings on its own; input
  is camera-relative (the game rotates the stick by `gView.rot.y`, §8.11), so a
  held direction curves as the camera orbits. With absolute drive the POSITION
  can't drift (render == sim), so any drift is visual/camera.
- **Foreshortening** - moving toward/away from the camera (W/S) covers less screen
  distance per world-unit than side-to-side (A/D) under perspective, so W/S reads
  slower (the A1.2a "compression" note). Isotropic in world space; a perspective
  artifact.
- Both resolve with a real FIXED arena camera (a future item, alongside rendering
  the real walled arena so `arena_classic` geom can be used - §8.5a).

### 8.14 A1.5 fixed camera + A1.2g floor diagnosis + the m2c pipeline (2026-07-26)

**THE UNLOCK THIS SESSION: readable, typed C for any undecompiled function, in
one command** — `tools/decomp-func.ps1 <func>` in the fork.

```
splat  (lib/bmhero/splat.yaml, already configured)  -> asm/nonmatchings/**/*.s
lib/bmhero/tools/m2ctx                              -> ctx.c (decomp's own types)
m2c --context                                       -> typed C
```

Everything was already in the repo (`splat.yaml`, vendored `tools/n64splat`,
`tools/m2ctx`, `tools/asm-differ`); it only needed the ROM staged as
`baserom.z64` plus a venv. Set up at `.tools/decomp-venv` and `.tools/m2c`;
`asm/`, `baserom.z64` and `ctx.c` are all gitignored, so the submodule stays
clean. First run ~2 min (splat), then seconds.

**This beats reading `RecompiledFuncs` machine-C** (the A1.3/A1.4 method): m2c
does cross-function type inference against the decomp's headers, so you get
`gPlayerObject->moveAngle`, not `*(f32*)(a0 + 0x54)`. Validated against ground
truth — `func_80281E50` returns `sp1C = 4.0f`, matching the `0x40800000` turn
step previously derived by hand.

Three constructs in `ctx.c` defeat m2c's C parser and are auto-patched by the
script: `OS_CPU_COUNTER (OS_CLOCK_RATE*3/4)`, and two `sizeof(T) * 3` array
sizes. Symptom if you hit it raw: *"Failed to evaluate expression \*3 at compile
time"*.

**Correction to record:** the decomp does NOT have `code_extra_0` decompiled.
`func_80281E50` is present only as a `#pragma GLOBAL_ASM` stub — 182 of them in
that overlay. The hand-RE from machine-C was justified; what is new is that m2c
can now decompile those stubs on demand.

**Bonus finding:** the walker's turn is **state-dependent**, which the flat
"4.0deg/frame" note does not capture. `actionState` ∈ {5,6,0x1D,0x22} takes a
`360.0f` path (effectively instant); a `func_802813EC()==3` branch swaps 2.0 for
4.0. Worth revisiting when turn feel is next examined.

#### The camera — who actually owns `gView`

`func_8001994C` (decomp `src/boot/17930.c:605`) **derives `eye` and `up.y` every
frame** from `at` + `rot` + `dist`, gated on `D_8016E134 == 0`:

```c
view_rot_y = gView.rot.y + 90.0f;          /* NOTE THE +90 OFFSET */
eye.x = dist * cos(view_rot_y) * cos(rot.x) + at.x;
eye.y = dist * sin(rot.x)                   + at.y;
eye.z = dist * sin(view_rot_y) * cos(rot.x) + at.z;
up.y  = (rot.x >= 90 && rot.x < 270) ? -1 : +1;
```

Probe-confirmed exactly, live: with `at=(0,340,0)`, `rot=(20,114.70,0)`,
`dist=800` → predicted eye `(-683.1, 613.6, -314.5)` vs measured
`(-682.96, 613.62, -314.16)`.

- **`rot.x` is PITCH, `rot.y` is YAW, both in DEGREES.** The rail camera ran at
  pitch **20** — `sin(20)=0.34`, so W/S read at a THIRD of A/D. That was the
  real foreshortening cause. Screen-travel ratio is `sin(pitch)`, not `cos`.
- **The `+90` yaw offset** means `rot.y = 0` puts the eye at **+Z**. There is no
  handedness constant to guess.
- The game nudges `rot.x` of exactly 90/270 by 1 degree (gimbal guard).

#### What A1.5 actually had to do (two things the design got wrong)

1. **STAMP TWICE PER FRAME.** Writing the pose only before `func_80024744` is
   not enough — the game's camera update runs INSIDE that call and reverts it.
   Proven: post-write read-back logged `rot=(60,0,0)` while the next frame's
   entry read `rot=(20,2,0)`. The pre-update stamp is still required so the
   stick rotation (`func_80024744` rotates the stick by `rot.y`) sees our stable
   yaw; the post-update stamp is what the draw sees.
2. **WRITE `eye`/`up` OURSELVES.** `D_8016E134` is evidently non-zero here, so
   the derivation never runs. Diagnostic that caught it: with only `at/rot/dist`
   written, the picture was **PIXEL-IDENTICAL across a 2× change of
   `ARENA_CAM_DIST`**, and the logged `eye` never left the rail's last value.

Pose constants live in `patches/arena_cam.h` — pure constants + math, no game
types, so the SAME header compiles in the MIPS patch and in a host test
(`tools/test_arena_cam.c`, run by `tools/run-cam-tests.ps1`, gated in
`build.ps1`). **No runtime trig**: pitch/yaw are compile-time, so their sin/cos
are literals, avoiding the §8.11 silent-libcall land mine. The host test guards
that the literals still match their angles, that yaw stays 0, that
`sin(pitch) >= 0.85`, and that the effective-yaw trig matches the game's `+90`.
All three guards were verified to FIRE, not merely pass.

#### A1.2g — the fall is NOT the death path

Standing note said arena hazards were damage tiles and "the bypassed death path
crashes". Measured otherwise, with `actionState` logged beside position:

```
ppos=(353.38,   240.00,  630.45)   state=4
ppos=(353.38, 30000.00, 1173.62)   state=4    <-- Y jumps, state UNCHANGED
```

`actionState` never changes → no death state, no crash. **30000 is the ground
query's "no floor here" sentinel**: the player walked off the floor POLYGON,
because the real floor is smaller than the sim's collidable bounds. That is a
**§8.5a violation**, not a hazard-object problem, and it changes what A1.2g has
to fix.

**Floor guard (containment, NOT a fix).** The bridge remembers the last position
with a valid ground height; the patch restores it when the sentinel appears
(state native — patches are stateless). Sentinel samples per run 6 → 2, and the
sweep maps to z=1245 instead of stalling at 630. The sim still believes the
player is out of bounds, so sim and render disagree while the guard holds. The
real fix is re-matching the sim geometry to the measured floor — a sim change
and a `TUNE_VERSION` bump.

`[floor]` in the log tracks on-floor min/max every frame (BEFORE the log
throttle — a 30-frame sample cannot locate an edge) and prints extent, centre
and span. That centre is also what the fixed camera should aim at.

#### Probe modes and gates added

- `ARENA_AUTO_BATTLE=6` — camera probe: logs `[cam] at/eye/rot/up/type/dist`,
  `wrote_rot`/`wrote_at` (post-write read-back), `ppos`, `state`, and `[floor]`.
  Sweeps all four directions. **Mode 6 had to be added to BOTH auto-battle gates
  in `main.cpp`** or the frontend mash never fires and the boot hangs.
- `arena-soak.ps1 -Constant '<regex>'` — value must NEVER change (inverse of
  `-Rising`). Completes the trio with `-Expect`. **Caveat learned the hard way:
  a constancy gate can pass for the WRONG reason** — it passed on the rail
  camera's yaw when that happened to sit still. Assert on a value only YOU set
  (pitch 60 worked; yaw 0 did not, initially).

#### Build race (bit 3× in one session)

Straight after a patches `make clean`, the first cmake run can fail while
N64Recomp regenerates `RecompiledPatches/`; an immediate retry always succeeds.
`build.ps1` now retries once. **This matters more than it looks: a FAILED build
leaves the PREVIOUS exe in place and the soak happily tests that stale binary.**
It silently invalidated an A/B isolation run before it was caught.

---

## §8.15 — Measuring the arena floor directly (probe mode 7), and the anchor bug it exposed (2026-07-26)

### The method: ask the geometry, don't walk it

Every earlier "measurement" of the arena floor drove the player around and logged
where it stopped. That measures **how far the player could go**, not **where the
floor is** — and the two differ exactly when the sim's bounds are wrong, which is
the case you are trying to detect. It also stalls: the first edge you find parks
the player (the floor guard restores it), so one run yields one boundary point.

The game already has the answer. `func_80078168(x, y, z)` (decomp
`src/code/69AA0.c:205`) is the ground query. It is **pure position-driven** — a
chain of five collision calls, no caller context, no object index — and it
refreshes a small set of globals:

```c
sel = D_801776E0 & 1;     /* which of the two result slots holds the answer */
h   = D_80177760[sel];    /* ground height there                            */
```

and the game's own test for *"there is no floor here"* is

```c
sel == 0 && h == -30000.0f      /* 69AA0.c:401 — the branch that leaves unkD4 unset */
```

Copy that rule verbatim rather than inventing a height threshold. `D_801776E0`
and `D_80177760` are auto-named data symbols, so reach them by **address literal**
(§8.2), not `extern`.

**Probe mode 7** (`ARENA_AUTO_BATTLE=7`) walks a grid calling this. Native owns
the cursor and the accumulator so the patch stays stateless (§8.2); the patch
loops a literal 384 points/frame. It prints the extent, the ground-height range,
an **ASCII occupancy map** (shape matters as much as extent — a bounding box is
only the right model if the floor is a filled rectangle), and flags
**edge-saturation** if a hit lands on the box border. Grid is env-tunable
(`ARENA_RASTER_N`, `ARENA_RASTER_STEP`), so survey and edge-refinement are the
same probe with no rebuild. It needs **no player movement**, so nothing can
stall it, and it costs ~1 second.

**Result for MAP_NITROS_1 (level 15).** 81×81 @ 50u (6,561 samples) and 201×201
@ 10u (40,401 samples) agree **exactly**:

```
a filled SQUARE, x ∈ [-950, 950], z ∈ [-950, 950], flat at y = 240
no holes, no pillars, no steps
```

At `g_scale` 120 that is half = 950/120 = **7.9167 sim units on both axes**. The
v5 figure of "1900×900" was the walk-derived one: the player had been stopped by
the sim's own z wall, so the measurement confirmed the very bound it was meant to
check, and the arena was modelled a factor of two too narrow in z.

> **Always log which map you measured.** The render routine runs in *every* level,
> so `[capture]` fires wherever the draw gate happens to open — including the
> stage-select map if the warp hasn't landed. `[capture]` and `[raster]` now carry
> `gCurrentLevel`. A floor measurement that doesn't record its map is a trap; this
> was a live worry until `level=15` proved it.

### The bigger bug: the anchor, not the extent

The raster reports **absolute** Hero coordinates, which made the real problem
visible. From the same run:

```
[capture] origin(0, 240, 0)   ref_s(-7.55, -3.52)
```

`arena_puppet_capture` pinned the **player's spawn Pos** to the **sim's spawn**:

```
hero = origin + (sim - ref_s) * 120
```

So the sim's arena **centre** (0,0) landed at Hero **(906, 422)**, not (0,0), and
the sim's x range mapped to Hero **[-42, 1854]** against a floor of [-950, 950].
**Over half the sim arena hung over the edge.** That is the A1.2g "fall" — the
player is driven off the floor polygon, the ground query returns its sentinel, and
`Pos.y` parks at 30000 with `actionState` unchanged (§8.5a, not a death path).
It also explains why the old walk probe only ever saw `x ∈ [0..353]`, and why
raising `ARENA_CAM_DIST` pushed the arena *off-centre* rather than framing more.

**Fix:** anchor on the measured floor, not on a spawn.

```
hero = FLOOR_CENTRE + sim * scale        (ref_s = 0, the sim's own centre)
```

This also removes the capture's per-run drift by construction — `origin.z` was
logged as 0 on one boot and 396 on another, because the *player's spawn* varies.
The floor does not.

The measured constants live in `patches/arena_cam.h` (`ARENA_FLOOR_CX/CZ/Y/HALF`,
`ARENA_RENDER_SCALE`) and are the single source of truth for **both** sides of
§8.5a: the render anchor and the sim's `arena_geom.h` half-extents.

### Encoding §8.5a as a test

`tools/test_arena_cam.c` now `#include`s the **sim's** `arena_geom.h` from the
submodule and asserts `sim half × scale == measured floor half` on both axes,
that the arena is square, and that every spawn is inside the floor. The two sides
live in different repos, so nothing but a test keeps them together — and it
immediately caught a stale submodule pin. The floor guard also **warns loudly**
the first time it fires now: since the anchor fix it must never fire, so silence
would hide a regression in the very invariant this establishes.

Verification that the fix landed: a full four-direction sweep reaches |x| ≤ 926,
|z| ≤ 786, **all at ground height 240**, and the guard never fires (new
`arena-soak.ps1 -Absent` gate). The ~18 units past the sim's own wall (908) is
the game walker's step landing before our write overwrites it — still well inside
the ±950 floor.

### Still open: `at` is not honoured at draw time

An `ARENA_CAM_DIST` sweep (1200 / 1800 / 2800 / 4000, now runtime-tunable via the
env var — framing used to cost a patch rebuild per trial) **proved our gView write
drives the picture**: zoom tracks the value exactly. And the probe confirms we
leave `gView.at` at the measured floor centre (`wrote_at=(0,340,0)`).

But the rendered view is centred on the **player**, not on that point, and the
player's screen position drifts systematically as the distance changes. So
**pitch, yaw and dist are honoured while `at` is not**, somewhere between our
stamp and the draw. Until that is understood, A1.5 behaves as a
fixed-**orientation follow** camera — which still delivers its actual goal (a
stable yaw, so a held stick direction stops curving) but not a static
whole-arena framing. Next step: sample `gView` inside the *draw* routine
(`func_800821E0`) rather than the update routine, to see whether `at` is
overwritten in between or simply unused by the projection.

---

## §8.16 — A1.5 camera: the pose is correct; three framing hypotheses eliminated (2026-07-27)

> **PARTLY SUPERSEDED BY §8.17.** The conclusion "the drawn floor is offset
> from the collision floor" in this section is **WRONG** — it was inferred from
> screenshots that were silently cropped to a quarter of the frame. The camera
> was correct and the drawn floor *is* the collision floor. What remains valid
> here: the proof that the pose is honoured, the three eliminated hypotheses
> and their RE, and the `shot_measure.py` classifier lesson. Read §8.17 for the
> root cause.

**Correcting §8.15's closing note.** It said `gView.at` was not honoured and the
view followed the player. That was wrong, and it was wrong because it was inferred
from eyeballed screenshots rather than measured.

**Proof the pose IS honoured.** At `ARENA_CAM_DIST=400` the picture is a close-up
of the arena *centre*, with the player — 1100 units away at its corner — nowhere
in frame. That can only happen if `gView.at` drives the view. Separately, the
distance sweep proves our write reaches the render: zoom tracks `ARENA_CAM_DIST`
exactly. And the camera-follow code that *does* overwrite `at` (decomp
`src/code/63F90.c`, `gView.at.x = gPlayerObject->Pos.x` in a dozen places,
dispatched by `func_80076374`) is called **from inside `func_80024744`** — so our
post-update stamp already wins, which the probe confirms (`wrote_at` and the next
frame's entry sample both read `(0,340,0)`).

### The measuring tool that changed the answer

`tools/shot_measure.py` measures where the floor lands on screen in a
`capture-game.ps1` PNG: coverage, centroid, bbox, and the player's pixel position.
Pure stdlib (decodes PNG directly — no PIL). The classifier is derived from
**sampled pixels**, not guessed: the Nitros floor is blue-grey (`b > g`, e.g.
41,50,59) and the void is a green-cyan swirl (`g >= b`, e.g. 25,134,101) at every
brightness, so one channel comparison separates them. The red HUD falls out for
free. A first attempt using "dark pixels" silently classified the swirl's dark
bands as floor and returned a full-screen bbox for every shot — worth remembering:
**sample the actual pixels before writing a classifier.**

This tool overturned two conclusions previously drawn by eye.

### What is actually wrong, in numbers

The floor covers only **13% of the viewport at dist 2400** and hugs the corner
nearest the camera. Sweeping the camera target (`ARENA_CAM_AT_DX/_DZ`) further
+X/+Z keeps *increasing* coverage — 12.9% at offset 0, 28.8% at +475, 47.4% at
+950, still rising — while aiming −X/−Z yields **zero** floor pixels.

So the **drawn** floor is offset from the **collision** floor that probe mode 7
measured. The collision raster alone could not have revealed this; it takes a
render-side measurement to see the two disagree.

### Three hypotheses, each tested and eliminated

| hypothesis | how tested | verdict |
|---|---|---|
| `gView.at` overwritten before the draw | dist-400 close-up | **No** — shows the centre, not the player |
| level far clip plane | probe tag 9 logs `D_801779C8.raw` | **No** — MAP_NITROS_1 authors ZFAR **8000**; the floor's far corner is only ~2400 away |
| level-chunk view culling | forced `recomp_get_render_chunk_radius()` 0→3→6→10 | **No** — coverage moved 12.9%→13.7% then saturated |

Worth recording for its own sake: **ZFAR** is `D_801779C8.raw` (the debug overlay
prints `ZFAR`), set per level from `gLevelInfo[level]->unk2C` (`56800.c:372`) and
consumed by `guPerspective` (`71AA0.c:610`, FOV 50°, aspect 4:3, near 100).
**Chunk culling** is `func_800663EC` (patched in `required_patches.c`, commented
"do view culling for geometry level chunks"): a box of chunk cells around
`gView.at`, sized by `D_80104C70[gDebugDispType][0..5]` plus the recomp's own
`extended_level_chunk_rendering` config option.

The chunk-radius override was **reverted** rather than left in on a dead
hypothesis. The ZFAR override is kept as a guard for maps that *do* author a short
plane, with a comment stating plainly that it was not the cause here.

### Next step

**RenderDoc** (installed). `renderdoccmd capture` launches the game hooked;
`qrenderdoc --python <script>` scripts the analysis headlessly. One capture gives
the real view matrix and the floor mesh's true world bounds — settling "where is
the drawn floor" directly, instead of another hypothesis cycle. Note there is no
`renderdoc.pyd` in the install, so analysis goes through `qrenderdoc --python`
rather than a plain Python import.

Bear in mind this is all on the **Nitros render stand-in**, which the roadmap
replaces with a purpose-built arena. The sim matches the *collision* floor exactly
(the floor guard never fires across a full sweep), so gameplay is consistent; the
mismatch is with what is *drawn*.

---

## §8.17 — ROOT CAUSE ANALYSIS: the A1.5 "camera bug" was a cropped screenshot (2026-07-27)

**Outcome: A1.5 is done.** `ARENA_CAM_DIST` 2800 frames the whole 1900×1900 arena,
centred, with margin on every side. **No camera code needed changing.**

Scripting/API detail for reproducing any of this lives in
`docs/renderdoc-capture-reference.md`.

### Summary

| | |
|---|---|
| **Symptom** | The fixed arena camera appeared not to frame the arena: the floor "hugged the bottom-right corner" and shrank as `ARENA_CAM_DIST` grew (measured: 62% → 4.5% of viewport as dist went 400 → 4000). |
| **Believed cause** | Something in the render pipeline was ignoring or overriding our camera pose. |
| **Actual cause** | `tools/capture-game.ps1` was capturing **only the top-left quarter of the frame**. The camera was correct throughout. |
| **Mechanism** | Backbuffer is **1600×900**. `GetClientRect` reports **800×450** to a **non-DPI-aware** process. The script allocated an 800×450 bitmap and `PrintWindow` blitted the top-left 800×450 of the real surface into it, **unscaled**. |
| **Fix** | One line: `SetProcessDPIAware()` before `GetClientRect`. |
| **Detected by** | A RenderDoc capture of the same session, seconds apart from a screenshot. They disagreed completely. |

### Why it survived so long

The tool produced a **believable** lie. The images showed real game content,
correctly lit and animating, and they *changed when the camera changed* — so
every cheap sanity check passed. Screenshots had been used for months as the
project's visual verification (`arena-verification-loop`), and had never once
been compared against an independent source.

The crop also produced a **coherent false narrative**. A centred arena drifts out
of the top-left quarter as the camera pulls back, which reads exactly as "the
floor hugs the corner and shrinks with distance" — a symptom that *invites*
render-pipeline explanations. Worse, it responded plausibly to experiments: the
`at`-offset sweep showed coverage rising as we aimed +X/+Z, which looked like
strong evidence that the drawn floor was offset from the collision floor.

### The wrong paths, and why each was plausible

Each was hypothesised, implemented, and tested against the cropped images. All
three are **real mechanisms, correctly reverse-engineered** — and all three were
irrelevant. They are recorded because the RE is worth keeping.

| # | Hypothesis | Why plausible | How refuted |
|---|---|---|---|
| 1 | `gView.at` overwritten before the draw | `63F90.c` really does set `gView.at.x = gPlayerObject->Pos.x` in a dozen places | A dist-400 close-up shows the arena **centre**, not the player 1100 units away. Also `func_80076374` (the camera dispatcher) runs *inside* `func_80024744`, so our post-update stamp already wins. |
| 2 | Level far clip plane | ZFAR is per-level (`D_801779C8.raw` ← `gLevelInfo[level]->unk2C`, `56800.c:372`), consumed by `guPerspective` (`71AA0.c:610`), and our camera sits far further back than the map's own | Probe tag 9: MAP_NITROS_1 already authors **ZFAR 8000**; the floor's far corner is only ~2400 away. Overriding it changed nothing. |
| 3 | Level-chunk view culling | `func_800663EC` really is "do view culling for geometry level chunks" — a box of chunks around `gView.at` sized by `D_80104C70[gDebugDispType][0..5]` **+ `recomp_get_render_chunk_radius()`** | Forcing that radius 0 → 3 → 6 → 10 moved coverage 12.9% → 13.7%, then saturated. Reverted rather than left in on a dead hypothesis. |

Two conclusions written into these notes from that evidence were **wrong** and
have been retracted: that `at` was ignored and the view followed the player, and
that the drawn floor was offset from the collision floor.

### What actually settled it

A RenderDoc capture, which confirmed the camera **twice over before anyone looked
at the image**:

- **Depth.** Clip-space `w` is view-space depth. Our camera at pitch 60 / dist
  2400 predicts `depth = 2486.6 − 0.5·z` for a floor point at world `z`; for the
  measured collision floor `z ∈ [−950, 950]` that is `w ∈ [2011.6, 2961.6]`. The
  capture reported **`w ∈ [2006.6, 2966.6]`** — a ~5-unit match.
- **Width.** Clip-x extremes of ±1537/1553 correspond to world `x = ±950`.

Then the saved backbuffer showed the whole arena, framed and centred.

### What would have caught it sooner

- **Comparing the instrument against an independent source, once.** A single
  RenderDoc capture at any point in the previous two sessions would have ended it.
- **Noticing the resolution.** The captures were 800×450 while the game renders
  1600×900. The number was printed on every single capture (`SAVED ... (800x450)`)
  and never questioned.
- **Distrusting a model that keeps losing.** The projection maths disagreed with
  the screenshots in *every* configuration tried. Repeated disagreement between a
  simple model and an instrument is evidence about the instrument.

### Corrective actions taken

1. `capture-game.ps1` fixed (`SetProcessDPIAware`) with a banner in the file
   explaining what the bug cost, so the next reader doesn't re-learn it.
2. `tools/rd-capture.ps1` + `rd_trigger.py` / `rd_saveframe.py` /
   `rd_analyse.py` added — independent visual ground truth, no keyboard needed.
3. `tools/shot_measure.py` added — turns screenshots into numbers (coverage,
   centroid, bbox, player pixel position) instead of impressions.
4. Handoff trap list and `CLAUDE.md` updated; the memory
   `validate-instruments-first` records the general lesson.

### The lesson

**A measuring instrument that has never been checked against an independent
source is a hypothesis, not evidence.** When a model and an instrument disagree
repeatedly, suspect the instrument.

This is the same failure as `measure-geometry-not-player` in different clothing:
there, a measurement was confounded by the very bound it was meant to test; here,
a measurement was confounded by the very framing it was meant to test.

### Tooling notes

See `docs/renderdoc-capture-reference.md` for the qrenderdoc scripting quirks
(no `renderdoc.pyd`; embedded Python 3.6.4; `renderdoccmd` has no trigger verb;
F12 needs foreground focus; pass paths via the environment; `__file__` undefined
and `argv[0]` unwritable; GUI app so log to a file; seek to end-of-frame before
saving the backbuffer).

---

## §8.18 — The A1.4 "camera regression" was never the camera; the set pose now holds (2026-07-27)

**Outcome:** the A1.4 anim gate is **green**, with the camera on *and* off. The
set pose holds for 24 frames instead of flickering for one. Nothing about the
A1.5 camera was involved.

### The wrong attribution, and how it happened

The gate went red when the A1.5 camera landed, and an A/B seemed to pin it:
camera off → PASS 2/2, camera on → FAIL 3/3. That A/B ran across **two different
builds**. Rebuilding to flip a variable is exactly how a stale-exe measurement
creeps in (a failed build leaves the previous binary, §8.14) — and more simply,
two builds differ in more than the variable you meant to change.

Fixed by adding **`ARENA_CAM_OFF=1`**, a runtime toggle for the camera stamp, so
the A/B runs on **one binary**. Result: the gate fails **identically** either
way. Not a camera regression.

> **Rule:** A/B a variable at *runtime* on a single binary wherever possible.
> If you must rebuild, you are comparing two builds, not one variable.

### The real mechanism

The 8-frame `[anim]` burst showed the symptom and nothing around it. A new
`[animw]` window logs **every frame for 40 frames after a set edge**, with the
player's `actionState`:

```
+00 idx=29 frame=0 state=4     <- our set pose
+01 idx=3  frame=2 state=4     <- walker replaces it after ONE frame
+02 idx=3  frame=4 state=4
```

`func_80024744` (the walker) runs **before** our anim block every frame and
re-asserts its own animation unconditionally. The tell is `idx=3 frame=2` on the
very next frame: locomotion's counter **continued** rather than restarting, so
the walker never stopped driving it — our pose was simply overwritten.

So a one-shot trigger survives exactly one frame. **With the camera on or off,
standing still or moving.** Two intermediate hypotheses were tested and refuted:

| hypothesis | test | result |
|---|---|---|
| The A1.5 camera | `ARENA_CAM_OFF` toggle, one binary | identical failure both ways |
| The probe set while still decelerating | widened the stand from 4 to 15 frames (`stop_ticks` is 6) | no change |

This also **corrects §8.5c**, which said the pose "holds when standing" and is
fragile only while moving. It was fragile always; standing never helped.

### The fix: hold, not one-shot

Native opens a 24-frame window on the set edge (`arena_set_hold`); the patch
re-asserts the pose whenever the walker has taken it:

```c
if (set_edge || (arena_export_set_hold() && func_8001B880(0, 0) != 29))
    func_8001C0EC(0, 0, 29, 1, (u32*)D_80115808);
```

The pose now holds for the full window and releases cleanly to locomotion.

### The gate was asserting something impossible

`-Rising 'idx=29 frame=(\d+)'` meant *"the anim frame counter advanced"*. But
holding requires re-triggering, and re-triggering **restarts** the anim — so the
counter is pinned at 0 forever. **The counter can never advance with an
overlay-style trigger**, camera or no camera. The gate was unachievable by
construction, which is why it stayed red and why it invited a false explanation.

The gate's *intent* was always "the pose appeared for a meaningful duration
rather than flickering for one frame". `-AnimProbe` now asserts
`[animw] +12 idx=29` — still showing 12 frames after the edge. That measures the
intent directly and is what the implementation can honestly deliver.

**This is a gate correction, not a weakening.** The old proxy was impossible; the
new one is a stronger statement about what the player actually sees.

### Still open

The pose is **static** — held, not animated, because each re-trigger resets the
frame counter. Real animation needs the game's own set/drop **action state**
engaged so the walker plays the anim itself rather than being fought each frame.
That is the "cleaner approach" §8.5c always pointed at, and it is now the only
remaining A1.4 item.

### The lesson (again)

Two gates in two days were measuring the wrong thing: `test_throw_fixed_arc`
measured residual facing instead of the arc (§8.15), and this one measured an
unachievable frame counter instead of duration. **When a gate stays red, check
what it actually asserts before hunting for what broke it** — and be just as
willing to find the gate wrong as the code.

---

## §8.19 — Feel-test bugs: inverted W/S, the permanent freeze, and the turn snap (2026-07-27)

Four reports from the first real feel test. Three were bugs; one was a
misunderstanding of mine that the user was right to push back on.

### 1. W/S inverted (bridge fix)

The two sides disagreed about which way is "up" on the Y axis:

| | forward is |
|---|---|
| recomp | **positive** — W maps to `Y_AXIS_POS`, and `cur_y += Y_AXIS_POS - Y_AXIS_NEG` (`recompinput/src/profiles.cpp:397`) |
| sim | **negative** — *"sy MUST be -31, not +31: `iatan2(Q(0),Q(-31))` resolves to 0x0000"* (`tune_probes.c`) |

So W drove the sim to +Z, which the fixed camera (eye at +Z) renders as moving
**down**. Negated in the **adapter**, not the sim: the sim's convention is
canonical, tested, and folded into the pinned hash.

**Facing was verified afterwards** rather than assumed, because the bridge copies
the game's `moveAngle` into `Rot.y` while the game computes it from the *raw*
stick — so the two could have disagreed. They don't: holding S gives sim yaw 180
(+Z) and `moveAngle` 0, which in the game's convention `(sin θ, cos θ)` *is* +Z.
The negation actually brought the sim into line with the game's own walker.

### 2. "Stuck in place" — a permanent freeze, not the wall

The first diagnosis was the arena wall (the player does pin at
|z| = half_z − player_radius). That was **wrong as the cause of what the user hit**.

`PHASE_ROUND_END` was **terminal**: it counted its timer down and then did
nothing, while `gameplay` gates player input off in that phase. With no respawn
for a dead player either (`player_tick` returns early on `PSTATE_DEAD`), **both**
of these froze the player permanently:

- dying to your own bomb, or
- killing the three idle puppets → `alive <= 1` → the round "ends".

Fixed in the sim (v9): rounds restart until `TUNE_ROUNDS_TO_WIN`, then the sim
holds — ending the *match* is the session's call. `arena_init` and the restart
share one `round_reset()`, so a restarted round can never be seeded differently
from a fresh one.

> **A frozen sim is still perfectly deterministic**, which is why the determinism
> suite could never have caught this: it was checking that nothing changes.
> `tests/test_round.c` covers it now, and was verified to FAIL against the
> pre-fix sim (12 failures, including "player 0 responds to input after a round
> restart").

### 3. Turn drift — the user was right, I was wrong

I first called this "by design" (no-strafe gradual turn). The user pushed back:
stopping and running the other way shouldn't arc. **They were right**, and the
game says so.

With the fixed camera we can read the game's own `moveAngle` per frame (probe
mode 9). On a stop-then-reverse:

```
f164  moveAngle=180.0   simYaw=  0.0
f165  moveAngle=  0.0   simYaw=354.0    <- game SNAPS in one frame; sim starts sweeping
f194  moveAngle=  0.0   simYaw=180.0    <- sim arrives 30 frames later
```

The decomp agrees, and I had quoted the line without reading it: the real
walker's turn is **per-action-state**, and *"states 5/6/29/34 snap instantly (thr
360)"* (`movement-re.md ## Turn`). The probe shows the player in states 29/30/31
across exactly that manoeuvre.

It was worse than a plain arc, because the bridge takes **facing from the game**
(snapped) and **motion from the sim** (sweeping) — for half a second the model
faced one way and slid the other.

Fixed with `TUNE_TURN_SNAP_SPEED` (v9): below it the facing snaps, above it the
bounded sweep is untouched. Speed is the closest handle the sim has to the game's
action states — with no momentum there is nothing for a gradual turn to conserve.
The feel-confirmed moving turn is preserved exactly (`turn180_ticks` 30,
`turn_radius` 1.349 unchanged); what moved is `ramp_distance` 0.937 → 0.822 and
`ticks_to_90pct` 12 → 11, i.e. the player now accelerates straight out of rest.

### 4. "Teleported to the damage square" — not a hazard

A1.2g had recorded the room's damage tiles as being *at the corners*, which is
exactly where our spawns are, so this needed checking rather than dismissing.
**Idled on the corner pad for 45 s: no damage, no death, no crash**; the sim
stayed `alive=4` / IDLE at its spawn, and the game's actionState merely cycled
1/29/30/31/36 (idle fidgets). They are the room's four **spawn** pads — our
spawns coinciding with them is correct.

The visible teleport is ours and is cosmetic: the game spawns the player at the
floor centre, then the absolute drive engages after the ~30-frame draw-gate
warmup and snaps them to the sim spawn, during the 3-second countdown.

### The recurring lesson

**Three tests in two weeks were measuring the wrong thing**, and each cost real
debugging time before anyone questioned the test itself:

| test | measured | should have measured |
|---|---|---|
| `test_throw_fixed_arc` | residual facing | the arc |
| `-AnimProbe` (`-Rising`) | an unachievable frame counter | pose duration |
| `ticks_to_turn` | a turn from a standstill | a turn **at speed** |

Each looked fine until the behaviour around it changed. When a gate goes red, ask
what it actually asserts *before* hunting for what broke — and be as willing to
find the test wrong as the code.

---

## §8.20 — A1.2g: suppressing the room's hazards (2026-07-27)

The Nitros room is a *render stand-in*. Its hazards belong to a boss fight, not to
our battle ruleset — in battle the **sim owns every hit and every hit point**, and
the game object is a puppet. Any damage the room lands on it is wrong by
definition, and it is also a stability risk: the bypassed death path crashes
(§8.9), so a room hazard is a route to a hard crash.

### The damage chain, traced end to end

Worth writing down, because finding the one right lever took the whole chain:

```
func_80086AD0                (decomp 76640.c:714)
    reads the surface under gPlayerObject, sets D_8016E080 from its TYPE
    0xFF -> 0 (none)   0xF8 -> 2   0xF7 -> 1   0xF5/0xD9 -> 3/4/5
        |
        v
case 5/6 block inside func_80024744   (21E10.c:648)
    non-zero code  ->  damage request in D_80177648
        |
        v
the application, gated on exactly one flag:
    if (!gDebugInvincibileFlag)       (21E10.c:670)
```

Our corner tiles are **0xF7 → code 1**, located exactly by the probe-7
surface-type raster (§8.19).

### The fix: one flag, not one patch per hazard

`gDebugInvincibileFlag = arena_bridge_is_battle() ? 1 : 0;` each frame.

It suppresses the whole class — the tiles and anything else the room throws —
rather than us patching `func_80086AD0`, or the case 5/6 block, or each hazard
type separately. It is the game's own debug facility, and its only other uses are
the debug menu's display and toggle, so nothing else changes. Cleared outside
battle so leaving a match for the campaign in the same process cannot leave the
player invincible.

### Verified, not assumed

The interesting part. "I set a flag and nothing bad happened" proves nothing —
the player might simply never have touched a tile. So log **`D_8016E080`**, the
hazard code the game itself derives:

> Four-direction sweep parks the player at (−926, −908), which is **on** a corner
> tile. Result: **`hazard=1` on 22 of 29 samples**, with no damage, no stun, no
> crash and no level change.

The tile is still **detected**; only its damage is suppressed. That is the
evidence — a non-zero hazard code with no consequence.

### Exit trigger — not reproducible, and probably never a trigger

Three independent checks:

1. The full surface-type raster finds only `0xE1`, `0xE2`, `0xF7`, `0xFF` on the
   floor. **None** of the transition surface types `76640.c` keys off (`0xF1`,
   `0xED`, `0xEC`, `0xE8`, `0xD7`) exists anywhere on it.
2. Two full sweeps with new `[level]` logging (gCurrentLevel + the next-level
   request vars, logged **on change** and **before** the sample throttle, since a
   transition is a single-frame event that a one-in-30 sample would miss):
   `gCurrentLevel` stays **15** throughout.
3. Non-actor `gObjects[14..77]` are already deactivated every frame by the
   existing boss-suppression sweep.

The probe run that used to end on the stage select almost certainly did so
because of the **anchor bug** (§8.15), which drove the player to Hero x=1854 —
far outside the ±950 floor and out of the arena entirely. With the anchor fixed
and the sim's walls at 908, the player is confined well inside the floor.

**Recorded as not-reproducible rather than fixed**, because no change was made for
it. If it reappears, the `[level]` logging will name the transition.

### Still open

The **HUD** — an RmlUi overlay per the design doc, *not* a patch of Hero's own
HUD (which currently shows the campaign's health/score/bomb icons, meaningless in
battle). That is a slice in its own right.

---

## §8.21 — A1.2g HUD: reuse Hero's own instead of an overlay (2026-07-27)

The design doc called for an RmlUi overlay. **Reusing the game's in-level HUD is
far cheaper and looks native** — the art, layout and draw already exist and
already match the game's style. All they lacked was our numbers. No new UI code.

### The HUD variables

Cleanly named in the decomp (`76640.c` `func_80088134` sets the campaign's base
stats), so no RE was needed beyond finding them:

| variable | HUD element | driven with |
|---|---|---|
| `gHealthCount` / `gMaxHealth` | red bars, top-left | sim HP of player 0, clamped |
| `gBombCount` | bomb icon, bottom-right | `3` |
| `gFireCount` | fire icon, bottom-right | `3` |
| `gGemCount` | gem count, bottom-left | `0` |
| `gScore` | digits, top-right | `0` |

**The bomb/fire counters count POWERUPS COLLECTED, and the HUD draws
`count + 1`.** So `3` renders as **"4"** — the maximum, not an off-by-one. The
cap is 3 (`21E10.c:366/374`). Battle has no powerups, so everyone is permanently
fully kitted and showing max is honest rather than decorative.

### Why TUNE_START_HP went 2 → 4 (v11)

The game **hard-codes `gMaxHealth = 4`** and the HUD draws that many slots. A sim
HP of 4 therefore maps **1:1** with no scaling and no half-bars. It is also a real
gameplay change (rounds last longer), which is why it took a version bump rather
than a quiet edit.

### Rules observed

- **Driven every frame**, so the HUD tracks the sim exactly — including across a
  round restart, where a write-once approach would go stale.
- **Battle only.** These are the campaign's own counters; writing them outside
  battle would corrupt a real playthrough. The `if (arena_bridge_is_battle())`
  guard is not optional.

### Verified on screen

4 lit health bars, score `00000`, gems `00`, bomb `4`, fire `4`, and the player
standing clear of the corner damage pad (spawns are at ±660, hazard starts at
750).

### Still open

Zeroing the score only **censors** it to `00000`. Genuinely hiding the field, or
repurposing it per player (round wins was the suggestion), means touching the HUD
**draw** rather than its inputs — a separate decision. Exports for `stocks_won`
and the match phase are in place and unused, ready for whichever is chosen.

A per-player HUD (four healths, one per bomber) is a bigger question: Hero's HUD
is single-player by construction, so that is where an overlay would finally earn
its keep.

## §8.22 — Action poses identified by RENDERING the asset table (2026-07-27)

**Result: set-bomb = 41 (was 29), kick = 32.** Both play on `gPlayerObject` when
the sim registers the event, gated on screen (`arena-soak.ps1 -AnimProbe` for
set, `-Mode 10 -Expect '\[kick\] pose idx=32'` for kick).

### What went wrong with the code-only pass

§8.5c derived set = 29 from the state machine (`code_extra_0` state `0x0E` →
`func_80282E5C_code_extra_0` → `func_8001C0EC(0,0,29,...)`) and marked it MEDIUM
pending a look. It was never looked at, because the harness gate only ever
asserted *"index 29 is playing"* — which is true no matter what 29 draws. **A
gate that asserts your own assumption cannot fail.** On screen 29 reads as a
throw, which is why the user kept reporting the wrong pose while the gate stayed
green.

The kick call was worse: see the correction in §8.5c. A call-graph search says
what is *used*, never what *exists*.

### The tool: `tools/anim-contactsheet.ps1`

One run produces 106 labelled PNGs in `tools/anims/` — two per index across all
53 entries of `D_80115808`, named for the index the game itself reported:

- `ARENA_ANIM_SWEEP=<ticks>` cycles every index in the render patch and logs
  `[animsweep] now playing idx=N` as each starts.
- The script polls that log and screenshots ~0.5 s and ~1.4 s in, so the file
  name comes from **the game's own report**, not from counting elapsed time.
  Two shots because a single frame of a motion is often ambiguous.
- `-Pitch 30 -Dist 450` overrides the play camera (pitch 60 / dist 2800 is near
  top-down — the user's "so zoomed out it's very hard to visually confirm").

Identification is then a human flipping through images, which takes a minute and
is not something a screenshot classifier should be trusted to do.

### The mechanism: one pose window for both actions

Both edges are detected **natively in the bridge** as pure reads of sim state —
no gameplay change, sim hash untouched:

| action | edge | actor |
|---|---|---|
| set  | bomb `FREE → SETTLED`   | `bombs[b].owner` |
| kick | bomb `SETTLED → SLIDING` | `bombs[b].bounced - 1` |

`bounced` is the sim's kicker-grace field and doubles as "who kicked it"; the
state edge alone does not say. `tests/test_bomb_mechanics.c` now asserts that
encoding so the fork can't silently animate the wrong bomber.

The patch gets **one** number, `arena_export_pose_anim()` — the index to hold, or
-1. It needs no edge of its own: per §8.18 the walker re-asserts its own anim
every frame, so a one-shot trigger survives exactly one frame and every action
pose has to be *held* regardless. Set and kick therefore share one window
(24 frames) and one code path. `ARENA_SET_ANIM` / `ARENA_KICK_ANIM` override.

### Still open: the poses are STATIC

Both hold correctly but the frame counter stays pinned at 0 (`arena-log.ps1`
reports `STATIC (set but never advanced)`), because holding means re-triggering
whenever the walker steals the anim, and re-triggering restarts the clip. Real
animation needs the game's own action state engaged so the walker plays the clip
itself. Unchanged from §8.18 — the pose is now the *right* pose, still not moving.

### The probe timing trap (`ARENA_AUTO_BATTLE=10`)

The kick probe sets a bomb, walks clear (the setter is immune until they step
clear), then walks back in. Two constraints, both found by measurement after the
first attempt silently did nothing:

- **The input poll counter runs ~45 ahead of the sim tick.** A set at poll 215
  lands at tick ~175 and is dropped by the 180-tick countdown — visible as
  `[earlybtn] t175..178` with no bomb ever appearing. Mode 4 had been getting
  away with the same window by 3 ticks.
- **`TUNE_FUSE_TICKS` is 150**, so the whole round trip must finish before the
  bomb detonates underneath the player.

---

## §8.23 — The poses ANIMATE: gate the walker's writes, trigger once (2026-07-30)

**Result:** set 41 and kick 32 now *play* instead of holding frame 0. Fork
commit `2cac14c`; fork-only, sim untouched, pinned hash `ff22fa4b` holds.
Closes the "STATIC poses" item from §8.18/§8.22.

### The mechanism

`func_8001BE6C` resets the anim frame counter (`unk24 = 0.0f`, `17930.c:1182`)
on **every** call — that one line is why hold-by-retrigger could never animate:
each re-assert restarted the clip, pinning the counter at 0 *by construction*.

The fix stops the fight instead of winning it every frame. `func_8001C0EC` is
the single anim-trigger funnel the player overlay uses — **all 69 anim calls in
the code_extra_0 machine-C (`funcs_50–54.c`) go through it; zero call
`func_8001BE6C` or `func_8001C158` directly** (checked 2026-07-30). So a
`RECOMP_PATCH` of `func_8001C0EC` (a 2-line function, reimplemented exactly)
drops any anim write to the player body (`objId 0, part 0`) that isn't the held
pose while a pose window is open:

```c
RECOMP_PATCH void func_8001C0EC(s32 objId, s32 part, s32 animIdx, s32 fileID, u32* animTable) {
    if (objId == 0 && part == 0) {
        s32 pose = arena_export_pose_anim();
        if (pose >= 0 && animIdx != pose) return;   /* walker steal dropped */
    }
    func_8001BE6C(objId, part, animIdx, (s32)&gFileArray[fileID].ptr[animTable[animIdx]]);
}
```

The per-frame block now triggers the pose **once** (its idx-mismatch condition
fires only at window open; it stays as a fallback against unfunneled writes),
and the game's own anim engine advances the clip. Window closes after 24 frames
→ the walker's next re-assert passes → locomotion resumes on its own.

Why not "engage the game's action state" (§8.18's original pointer): the
correct poses are **unreferenced assets** — kick 32 provably so (§8.5c: the
walk-in kick is 100% bomb-side), so there is no state to engage; and real
action states bring movement side-effects (snap turns, impulses) that would
fight the ABSOLUTE render-drive (§8.13).

### The gate §8.18 retired is achievable again — and green

`-Rising 'frame=(\d+)'` ("the frame counter advanced") was impossible under
re-triggering and had to be replaced by the weaker duration gate. It now
measures exactly the thing this slice claims:

```powershell
.\tools\arena-soak.ps1 -Mode 4  -Rising '\[animw\] \+\d+ idx=41 frame=(\d+)'  # PASS: 0,2,..,46 ×3 events
.\tools\arena-soak.ps1 -Mode 10 -Rising '\[anim\] idx=32 frame=(\d+)'         # PASS: 0,2,..,14
.\tools\arena-soak.ps1 -AnimProbe                                             # duration gate still PASS
.\tools\arena-soak.ps1 -N 5                                                   # 5/5 boot soak
```

`arena-log.ps1` now classifies `idx 41 … advanced (played)` — the line that
used to read `STATIC (set but never advanced)`.

### Boundaries / known interactions

- The gate consults `arena_export_pose_anim()` — a pure native getter (reads
  two host ints, no I/O). It is **not** `recomp_printf`-class and is safe in
  the load window; the `objId==0 && part==0` check short-circuits it for
  everything but the player body anyway. Outside the arena the window is never
  open, so menu/load/demo pass through byte-identically.
- An `ARENA_ANIM_SWEEP` re-assert would also be dropped while a pose window is
  open; sweep runs (`anim-contactsheet.ps1`) don't run battle probes, so the
  two never coexist in practice.
- If the player dies/warps *inside* a pose window (≤24 frames), that anim write
  is dropped too; the window expires and the walker recovers next frame.
  Cosmetic at worst; damage is suppressed in-arena (§8.20).
- **Feel-verify pending** (with everything since v11): pose duration is fixed
  at 24 frames; if a clip reads as cut off in motion, read `func_8001B44C`
  (clip-finished flag) and close the window on finish instead.

---

## §8.24 — Explosion visual: the bomb pool doubles as the blast pool (2026-07-30)

**Result:** detonation draws a growing ball instead of nothing. Fork commit
`b379c94`; fork-only, sim untouched, hash `ff22fa4b` holds. Implements exactly
the approach §8.13 documented when the separate A1.2c blast actors were dropped.

### The mechanism

A bomb's actor frees on the **exact tick** its blast spawns (`bomb_active`
drops, blast `ttl` starts), so the 4-actor bomb pool covers blasts with **zero
new model-pool slots** — the ~8-actor ceiling is never approached. Per frame,
after the bomb block: collect free bomb actors (slot order), assign to active
blasts (blast order), drive each to the blast's center with
`Scale = blast_radius / 15` (mesh ≈ 15u base). Stateless; a blast may hop
actors when the free set churns, but both actors are the same mesh driven to
the same pos/scale, so the hop is invisible. Excess blasts simply don't render.

Two facts settled from source before writing it:
- **The generic draw honors `gObjects[i].Scale`** — `guScaleF` in the object
  matrix build (`boot/17930.c:474/512`). §8.13 had left this unverified.
- **Spawn default Scale is 1.0** (`17930.c:730`), so the bomb block force-writes
  `Scale = 1.0` on live bombs — a blast-scaled actor coming back as a bomb
  sheds the scale, and non-reused actors are untouched at their default.

### Latent bug fixed in passing

The dead A1.2c blast loop called `arena_export_blast_active` / `_wr` with
**implicit declarations** — for an f32-returning export the caller reads `$v0`
instead of `$f0` (garbage). It never fired only because `blastactor_get_slot`
was always -1. Both now have proper `DECLARE_FUNC`s; the general rule is in the
handoff traps (an implicit decl of a DECLARE_FUNC export compiles with only a
warning and mis-reads float returns).

### Evidence

New export `arena_export_dbg_blast` (`0x8F000240`, full plumbing) → `[blastvis]
k=<blast> slot=<actor> wr=<radius>` per drive frame, bounded by
`TUNE_BLAST_TTL` (20) lines per blast:

```powershell
.\tools\arena-soak.ps1 -Mode 4 -Rising '\[blastvis\] k=0 slot=\d+ wr=(\d+)'
# PASS: 16,32,..,192 then 192 held — growth over 12 ticks, hold for 8, ×2 blasts
```

Pixels confirmed with a **log-triggered screenshot** (poll `arena_bridge.log`
for the first `[blastvis]` — every write is flushed, so polling is exact — then
`capture-game.ps1` immediately and +200ms): a ball several tiles wide at the
detonation point; gone after TTL. The technique generalizes: any flushed marker
can trigger a capture at sub-frame-window precision without input timing.

### ~~Known v1 crudeness~~ — superseded same day by v2 (below)

~~It's a giant **bomb** (blue, fused), not a fireball~~ — the feel test said
exactly that ("reads as a giant bomb, not a blast"), so v1 lasted one session.
Kept because the mechanism notes above (Scale honored by the draw, bomb-pool
reuse, the stateless assignment) remain true and reusable.

### v2 (same day): spawn the GAME'S OWN explosion — fork `6c28ec8`

Reading the game's detonation path found the real thing:

- The game bomb's fuse-out calls `func_800795C8` (`69AA0.c:465`) →
  **`func_8007E76C(x, y, z, type)`** (`code/70C40.c:12`) — the explosion
  spawner. Type 0 = the normal bomb blast.
- Explosions live in a **dedicated pool `gObjects[6..13]`** — separate from
  the bomb pool [2..5] and the generic pool [14..77], so the ~8-actor ceiling
  is entirely out of play. The arena bypass never touched this pool, and the
  old "double bombs" bug proves the path works in the Battle Room (the game
  player's thrown bombs exploded through it).
- The spawner binds a **two-part mesh from `gFileArray[0xD]`** (low-index core
  asset, resident in every level), **plays the explosion SFX**
  (`func_800177D8`), scales by `gFireCount`, and the objID handler runs its
  own grow/expire lifecycle — fully self-managed.

The patch now calls it once per blast birth (`arena_export_blast_new(k)`, the
existing native edge detector); the v1 scaled-ball loop is deleted. The new
`func_8001C0EC` walker gate (§8.23) does not interfere — explosion objIDs ≠ 0.
Screenshots: orange fireball dome + expanding smoke ring at the detonation
tile; board fully clean 2s later (self-expired, no pool litter). Gates:
`-Expect '\[blastvis\] k=\d+ slot=-2'`, pose regression, 5/5 soak — all green.

Note the sim's 192u hitbox is not passed — the game explosion draws its own
(campaign-default) size. If the feel pass wants visual = hitbox, set
`gFireCount` or override `Scale` on the spawned slot.

---

## §8.25 — Feel round 2: authentic poses (29 / none), bomb rest height, cam yaw (2026-07-30)

Fork commit `4696b7d`. Three findings from one feel report, each settled
against the game's own code/geometry before changing anything.

### Bomb rest height: the mesh centre belongs at floor + 30

The game bomb's ground handling rests it at `floor + 30.0f` (`69AA0.c:359`);
the mode-7 raster confirmed our `origin_y` (240) IS the floor (`ground
h=[240..240]`), so our bombs rendered half-sunk ("slightly inside the floor").
`arena_bomb_wy`/`arena_blast_wy` now add `BOMB_MESH_REST_LIFT` (30) — settled
bombs at 270 (gated: `[setdbg] wy=270.00`), and the explosion spawns at
bomb-centre height like the game's own detonation.

### Set pose is 29 after all — the stills lied, twice

The drop handler plays 29 (`func_80282E5C_code_extra_0` via m2c:
`func_8001C0EC(0,0,0x1D,1,D_80115808)`), and **front-view motion strips**
(`tools/anims/strips/`) show 29 is a step-and-reach-DOWN place. The §8.22
"reads as a throw" verdict — and therefore the 41 pick — was a far/back-camera
artifact. 41 is a near-static stand (matching the feel report "looked off");
42 is a crouch/curl. Default flipped to **29**.

**And it was looping.** Clip 29 is 10 frames long (counter wraps 18→0); the
fixed 24-frame window played it 2.4× — the feel report's "doesn't move as fast
as I remember". The window is now `arena_pose_frames()` (default 10 = one exact
play-through; `ARENA_POSE_FRAMES` overrides for longer clips). `-AnimProbe`'s
default gate is the honest rising form on idx 29 again.

### Kick pose is NONE — 32/33 aren't kicks

§8.5c's code-truth stands (kick is 100% bomb-side; zero anim calls), and the
strips show 32/33 are crouch/react clips with no extended leg. Default is now
**-1 = keep locomotion** (the bomb shooting away is the feedback, as in the
real game). `ARENA_SET_ANIM`/`ARENA_KICK_ANIM` accept -1, and 32/33/41/42
remain reachable for comparison.

### New instrument: ARENA_CAM_YAW + the facing trap

Runtime camera yaw (native trig like pitch; default 0 = shipped view). The trap
it uncovered: **the stick co-rotates with `gView.rot.y`, so a MOVING player's
camera-relative facing is yaw-invariant** — orbiting a runner shows the same
angle at every yaw. Mode 4's injected run always faces the camera (that's the
front-shot recipe: `ARENA_AUTO_BATTLE=4 + ARENA_ANIM_SWEEP + follow cam`);
an IDLE player + yaw genuinely orbits, but the idle follow-cam misframes at
yaw≠0 (unresolved; mode 4 sidesteps it). Strip scripts poll the flushed log
markers (`[animw]` / `[animsweep]`) to time captures — sub-window precision,
no input timing.
