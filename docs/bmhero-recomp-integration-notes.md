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

### 8.5c Player action animations — A1.4 RE (set-bomb recovered; kick has no anim; 2026-07-24)

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

**Kick has NO player animation (definitive).** Bomberman Hero offense is grab→charge→throw,
not a walk-into kick. The walk-in kick is **100% bomb-side**: `func_8007AD60`
(`69AA0.c:877-917`) does the bomb `0x24→0x27` slide (reads `gPlayerObject` for facing but
NEVER writes it or calls the anim primitive). In the real game the player just keeps its
locomotion anim while a bomb slides. `69AA0.c` contains **zero** `func_8001C0EC` calls.
Options for the arena's kick feedback (decision open): (a) **no special pose** — keep
locomotion (most authentic, recommended); (b) a **throw** anim as a "shove" stand-in
(`code_extra_0` throw poses 34/36/38/39/42) — arm/upper-body lunge, not a leg kick;
(c) the set/drop pose 29 as a stomp-down. There is no faithful kick to match.

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
