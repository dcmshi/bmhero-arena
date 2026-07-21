# A1.2b Placeholder Actors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In the Battle Room, spawn 3 extra objects and puppet them from the sim (players 1–3) so they track their sim positions with a resident **placeholder** mesh, then attempt a one-argument swap to the real bomber mesh.

**Architecture:** The A1.2a `gDebugRoutine2` wrapper (`patches/arena_render.c`) is extended: on the first battle frame it freezes a world anchor + sim reference in the native bridge and proper-spawns 3 objects via the game's own `func_80027464` with a **synthesized `ObjSpawnInfo`** (`unk4` = a resident file index; benign `objID`); every frame after, it writes each spawned object's `Pos`/`Rot` from native world-coord exports. All mutable state lives in native `arena_bridge` (patches must be stateless — the `0xC0000409` gotcha). Spec: `docs/superpowers/specs/2026-07-21-a1-2b-placeholder-actors-design.md`.

**Tech Stack:** VS 2022 clang-cl (native), LLVM-15 MIPS (patches), the A1.0 three-toolchain recipe (memory `recomp-build-toolchain`).

## Global Constraints

- Fork `C:\Users\dshi\GitRepos\BMHeroRecomp`, branch `feature/a1.2b-spawn-bombers` (already checked out; continues commit `89208b1`). `bmhero-arena` gets only doc updates.
- **`bmhero-arena` sim code not modified** (pinned hash `4b6687d4`); no submodule bump. This is all fork-side bridge + patch.
- Shell map stays `MAP_BATTLE_ROOM` (A1.1b-ii).
- **Patches must be STATELESS:** a `patches/*.c` file may NOT use file-scope mutable `static`/global variables it writes to (aborts with `0xC0000409`). All persistent state lives in native `arena_bridge.cpp`; the patch only reads game globals, calls exports, writes game objects, and uses local stack variables. (Native `static` in `arena_bridge*.cpp` is fine — that's not a patch.)
- **No `memset`/`memcpy` in the patch:** the LLVM-15 `-O0` MIPS build emits an unresolved libc call. Initialize structs field-by-field; bit-cast floats with a `union`, not `memcpy`.
- **No float ARGS across the export ABI:** `_arg<N,T>` reads integer/pointer GPR slots only (floats need special-case helpers). Pass floats in as `u32` bit patterns (`union { f32 f; u32 u; }`) and reinterpret in native. Float RETURNS (`_return(ctx, floatval)`) and int args are both proven and fine.
- Verified facts (decomp at fork `master`, 2026-07-21):
  - **Two object pools.** The generic per-frame UPDATE loop `func_8002B154` (`boot/26CE0.c:1205`) iterates `gObjects[14..77]` and calls `gObjInfo[objID].behaviour()` for each active object. The generic DRAW loops `func_8001C464`/`func_8001C5B8` (`boot/17930.c:1236,1257`) also iterate `gObjects[14..77]` and follow each object's `Unk140[]` model handles. Bombs use a *different* pool (`gObjects[2..5]`, `Get_InactiveObject`); do NOT spawn there.
  - **Proper spawn:** `s32 func_80027464(s32 count, struct ObjSpawnInfo* info, f32 x, f32 y, f32 z, f32 rotY)` (`boot/26CE0.c:328`) scans `gObjects[14..77]` for `ACTION_NONE`; per object does `func_8001A928` (init) → `func_8001BD44(idx, info->unk0, info->unk6, gFileArray[info->unk4].ptr)` (load mesh) → sets `Pos`/`Rot`/`actionState=ACTION_IDLE`/`objID=info->unk2` → wires `unk10E`/`unkE6` group links correctly. `count=1` spawns one standalone object and returns its slot (−1 if none free).
  - **Mesh is separable from objID.** `func_8001BD44` loads any mesh from any `gFileArray[idx].ptr` into any slot; `objID` only drives `behaviour()` + some render params. The player/bomber mesh is `gFileArray[1]` (`overlays/13AC20/13AC20.c:422` loads it with `func_8001BD44(0, 0, 0x13, gFileArray[1].ptr)`); the bomb mesh is `gFileArray[9]` (`code/69AA0.c:619`, `func_8001BD44(arg0, 0, 0, gFileArray[9].ptr)`). Both are low-index core assets resident in every level, incl. the Battle Room.
  - **`struct ObjSpawnInfo`** (`types.h:830`, 11 bytes): `s8 unk0`, `s16 unk2` (=objID), `s16 unk4` (=file index), `s8 unk6`, `s8 unk7`, `s8 unk8`, `s8 unk9`, `char unkA`.
  - **`struct ObjectStruct`** (`obj.h:53`): `Vec3f Pos`@0x00, `Vec3f Rot`@0x18 (`Rot.y`=facing, degrees), `s16 actionState`@0xA4, `s16 objID`@0xE4. `gObjects[207]`, `gPlayerObject`, `gFileArray[700]` all `extern` in `variables.h`; `OBJ_RPLATE=85`, `OBJ_TOBIRA1_O=77`, `ACTION_NONE=0`, `ACTION_IDLE=1` in `obj.h` (pulled in via `variables.h`).
  - **Benign objID:** the Battle Room's door (`OBJ_TOBIRA1_O` 77) and plate (`OBJ_RPLATE` 85) run their `behaviour()` every frame at slots 14/15 without crashing — so a spawned object with one of those objIDs has a known-safe behaviour. Our per-frame write runs AFTER the update (we call `func_80024744()` first), so `behaviour()` only needs to not crash, not be a true no-op.
  - **Ordering:** `arena_render_routine` is `gDebugRoutine2`; it calls the original `func_80024744()` (which runs the object update loop) first, then our code — so our `Pos` write is the last word each frame.
  - Native export ABI: `extern "C" void f(uint8_t* rdram, recomp_context* ctx)`; read args `_arg<N,T>(rdram,ctx)`, return `_return(ctx,val)`; register `REGISTER_FUNC(f)` + `extern "C"` fwd-decl in `main.cpp`; patch imports `DECLARE_FUNC` + a `syms.ld` `0x8F...` address. Existing exports end at `arena_export_spawn_placeholders_once = 0x8F000150`.
- **[build]** (VS dev shell + composed PATH; run after ANY change; the trailing `make clean` in `patches/` is MANDATORY after editing `patches/*.c` — ninja won't reliably re-run the patch make, and a stale `patches.elf` behaves as the OLD code):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; make.exe -C patches clean; cmake --build build-cmake --target BMHeroRecompiled
  ```
- **[launch]** = `play.bat --show-console` from the fork root (raw exe double-click crashes `0xC0000409`; it must run from the repo root so `assets/` resolves).
- Verification: build-exit gate + **human boot gate** (only a human can see the actors on screen). Do not claim success from a clean build alone.

---

### Task 1: Native bridge — puppet state + world-coord exports

**Files (fork):**
- Modify: `src/arena_bridge/arena_bridge.cpp` (puppet state + 8 native fns)
- Modify: `src/arena_bridge/arena_bridge.h` (declare the 8 native fns)
- Modify: `src/arena_bridge/arena_bridge_export.cpp` (8 recomp-ABI shims)
- Modify: `src/main/main.cpp` (REGISTER_FUNC + fwd-decls)
- Modify: `patches/syms.ld` (8 import addresses)

**Interfaces:**
- Produces (patch-callable EXPORT names): `arena_export_puppet_capture(u32 bx, u32 by, u32 bz)` (freeze anchor+ref, once), `arena_export_puppet_ready() -> s32`, `arena_export_puppet_set_slot(s32 i, s32 slot)`, `arena_export_puppet_get_slot(s32 i) -> s32`, `arena_export_puppet_wx(s32 i) -> f32`, `arena_export_puppet_wy(s32 i) -> f32`, `arena_export_puppet_wz(s32 i) -> f32`, `arena_export_puppet_yaw(s32 i) -> f32`.

- [ ] **Step 1: Add puppet state + native functions in `arena_bridge.cpp`**

In `src/arena_bridge/arena_bridge.cpp`, inside the anonymous `namespace { ... }` (after the `g_render_yaw` block, before `float qf(...)`), add:
```cpp
    /* A1.2b puppet actors (players 1-3). State lives here because patches must
     * be stateless. The world anchor is FROZEN at spawn (not the live player
     * object) so actors follow the sim, not the player's own movement (which
     * would mirror). g_ref_s* is player 0's sim pos at capture. */
    bool  g_puppets_ready = false;
    float g_origin_x = 0.0f, g_origin_y = 0.0f, g_origin_z = 0.0f;  /* frozen world anchor */
    float g_ref_sx = 0.0f, g_ref_sz = 0.0f;                          /* frozen sim ref (p0) */
    int   g_puppet_slot[ARENA_MAX_PLAYERS] = { -1, -1, -1, -1 };
```
Then, at the END of the file (after `arena_bridge_set_battle_mode`), add:
```cpp
/* A1.2b: freeze the world anchor (the player's spawn-frame Pos, passed as u32
 * bit patterns to dodge float-arg ABI) and the sim reference (player 0's sim
 * pos now). Idempotent — only the first call takes effect. */
extern "C" void arena_puppet_capture(uint32_t bx, uint32_t by, uint32_t bz) {
    if (g_puppets_ready) return;
    union { uint32_t u; float f; } ux, uy, uz;
    ux.u = bx; uy.u = by; uz.u = bz;
    g_origin_x = ux.f; g_origin_y = uy.f; g_origin_z = uz.f;
    g_ref_sx = qf(g_state.players[0].pos.x);
    g_ref_sz = qf(g_state.players[0].pos.z);
    g_puppets_ready = true;
}
extern "C" int  arena_puppet_ready(void)              { return g_puppets_ready ? 1 : 0; }
extern "C" void arena_puppet_set_slot(int i, int slot){ if (i >= 0 && i < ARENA_MAX_PLAYERS) g_puppet_slot[i] = slot; }
extern "C" int  arena_puppet_get_slot(int i)          { return (i >= 0 && i < ARENA_MAX_PLAYERS) ? g_puppet_slot[i] : -1; }

/* World placement of puppet i, anchored to the FROZEN origin. */
extern "C" float arena_puppet_wx(int i) {
    if (i < 0 || i >= ARENA_MAX_PLAYERS) return g_origin_x;
    return g_origin_x + (qf(g_state.players[i].pos.x) - g_ref_sx) * g_scale;
}
extern "C" float arena_puppet_wy(int i) { (void)i; return g_origin_y; }
extern "C" float arena_puppet_wz(int i) {
    if (i < 0 || i >= ARENA_MAX_PLAYERS) return g_origin_z;
    return g_origin_z + (qf(g_state.players[i].pos.z) - g_ref_sz) * g_scale_z;
}
extern "C" float arena_puppet_yaw(int i) {
    if (i < 0 || i >= ARENA_MAX_PLAYERS) return 0.0f;
    return (float)g_state.players[i].yaw * (360.0f / 65536.0f);
}
```
(`g_state`, `g_scale`, `g_scale_z`, `qf` are file-local in this translation unit; `<cstdint>` for `uint32_t` — add `#include <cstdint>` at the top if not already present.)

- [ ] **Step 2: Declare the native functions in `arena_bridge.h`**

In `src/arena_bridge/arena_bridge.h`, inside the `extern "C"` block, add:
```c
void  arena_puppet_capture(uint32_t bx, uint32_t by, uint32_t bz);
int   arena_puppet_ready(void);
void  arena_puppet_set_slot(int i, int slot);
int   arena_puppet_get_slot(int i);
float arena_puppet_wx(int i);
float arena_puppet_wy(int i);
float arena_puppet_wz(int i);
float arena_puppet_yaw(int i);
```
(Ensure `#include <cstdint>` — or `<stdint.h>` — is present for `uint32_t`.)

- [ ] **Step 3: Add the recomp-ABI shims in `arena_bridge_export.cpp`**

In `src/arena_bridge/arena_bridge_export.cpp`, append:
```cpp
/* A1.2b puppet exports. Floats cross the ABI as u32 bit patterns (args) or via
 * _return (returns); no float _arg is used (unsupported for arbitrary slots). */
extern "C" void arena_export_puppet_capture(uint8_t* rdram, recomp_context* ctx) {
    uint32_t bx = _arg<0, uint32_t>(rdram, ctx);
    uint32_t by = _arg<1, uint32_t>(rdram, ctx);
    uint32_t bz = _arg<2, uint32_t>(rdram, ctx);
    arena_puppet_capture(bx, by, bz);
}
extern "C" void arena_export_puppet_ready(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; _return(ctx, arena_puppet_ready());
}
extern "C" void arena_export_puppet_set_slot(uint8_t* rdram, recomp_context* ctx) {
    arena_puppet_set_slot(_arg<0, int>(rdram, ctx), _arg<1, int>(rdram, ctx));
}
extern "C" void arena_export_puppet_get_slot(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_puppet_get_slot(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_puppet_wx(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_puppet_wx(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_puppet_wy(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_puppet_wy(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_puppet_wz(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_puppet_wz(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_puppet_yaw(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_puppet_yaw(_arg<0, int>(rdram, ctx)));
}
```

- [ ] **Step 4: Register the exports in `main.cpp`**

In `src/main/main.cpp`, next to the existing `arena_export_*` `extern "C"` fwd-decls, add:
```cpp
extern "C" void arena_export_puppet_capture(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_puppet_ready(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_puppet_set_slot(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_puppet_get_slot(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_puppet_wx(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_puppet_wy(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_puppet_wz(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_puppet_yaw(uint8_t* rdram, recomp_context* ctx);
```
and in the `REGISTER_FUNC` block (alongside `REGISTER_FUNC(arena_export_spawn_placeholders_once);`):
```cpp
    REGISTER_FUNC(arena_export_puppet_capture);
    REGISTER_FUNC(arena_export_puppet_ready);
    REGISTER_FUNC(arena_export_puppet_set_slot);
    REGISTER_FUNC(arena_export_puppet_get_slot);
    REGISTER_FUNC(arena_export_puppet_wx);
    REGISTER_FUNC(arena_export_puppet_wy);
    REGISTER_FUNC(arena_export_puppet_wz);
    REGISTER_FUNC(arena_export_puppet_yaw);
```

- [ ] **Step 5: Add `syms.ld` addresses**

In `patches/syms.ld`, after `arena_export_spawn_placeholders_once = 0x8F000150;`:
```
arena_export_puppet_capture = 0x8F000154;
arena_export_puppet_ready = 0x8F000158;
arena_export_puppet_set_slot = 0x8F00015C;
arena_export_puppet_get_slot = 0x8F000160;
arena_export_puppet_wx = 0x8F000164;
arena_export_puppet_wy = 0x8F000168;
arena_export_puppet_wz = 0x8F00016C;
arena_export_puppet_yaw = 0x8F000170;
```

- [ ] **Step 6: Build**

Run **[build]**. Expected: native compiles (new exports), `patches.elf`/N64Recomp still succeed (the new `syms.ld` addresses are unreferenced so far — harmless), `Linking CXX executable BMHeroRecompiled.exe`. If a `REGISTER_FUNC` name and its `syms.ld`/shim name disagree, fix so all three match exactly.

- [ ] **Step 7: Commit**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add src/arena_bridge/ src/main/main.cpp patches/syms.ld
git commit -m "feat(arena): native puppet state + frozen-origin world-coord exports (A1.2b)"
```

---

### Task 2: Spawn spike — one placeholder object, verify it draws & survives

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: `func_80027464`, `gPlayerObject`, `gObjects`, `OBJ_RPLATE`, `ACTION_IDLE`, `struct ObjSpawnInfo` (all decomp symbols); Task 1 exports (`arena_export_puppet_*`).
- Produces: a proven single-object spawn (returns a slot; object draws with the bomb mesh; `behaviour(OBJ_RPLATE)` does not crash). Confirms the benign objID for Tasks 3–5.

This task spawns ONE static object at a fixed offset from the player (no sim puppeting yet) to isolate the spawn/behaviour risk from positioning.

- [ ] **Step 1: Add the spawn declarations + spike to `arena_render.c`**

In `patches/arena_render.c`, after the existing `DECLARE_FUNC(f32, arena_export_player_yaw, s32 i);` line, add the Task-1 imports and the game spawn function:
```c
/* A1.2b puppet exports (Task 1) */
DECLARE_FUNC(void, arena_export_puppet_capture, s32 bx, s32 by, s32 bz);
DECLARE_FUNC(s32,  arena_export_puppet_ready);
DECLARE_FUNC(void, arena_export_puppet_set_slot, s32 i, s32 slot);
DECLARE_FUNC(s32,  arena_export_puppet_get_slot, s32 i);
DECLARE_FUNC(f32,  arena_export_puppet_wx, s32 i);
DECLARE_FUNC(f32,  arena_export_puppet_wy, s32 i);
DECLARE_FUNC(f32,  arena_export_puppet_wz, s32 i);
DECLARE_FUNC(f32,  arena_export_puppet_yaw, s32 i);

/* Game proper-spawn: scans gObjects[14..77], loads mesh from gFileArray[info->unk4]. */
extern s32 func_80027464(s32 count, struct ObjSpawnInfo* info, f32 x, f32 y, f32 z, f32 rotY);
```
Then replace the body of `arena_render_routine` so that, after the existing player-0 puppet block, it spawns one placeholder once:
```c
void arena_render_routine(void) {
    func_80024744();
    if (arena_bridge_is_battle() && gPlayerObject != NULL) {
        /* N64 stick (~+/-80) -> sim stick (+/-31); sim stick up = -Z */
        s32 sx = (s32)(gActiveContStickX * (31.0f / 80.0f));
        s32 sy = (s32)(gActiveContStickY * (31.0f / 80.0f));
        if (sx >  31) sx =  31;
        if (sx < -31) sx = -31;
        if (sy >  31) sy =  31;
        if (sy < -31) sy = -31;
        s32 jump = (gActiveContButton & CONT_A) ? 1 : 0;
        s32 bomb = (gActiveContButton & CONT_B) ? 1 : 0;
        arena_export_tick_input(sx, sy, jump, bomb);
        gPlayerObject->Pos.x += arena_export_player_x(0);
        gPlayerObject->Pos.z += arena_export_player_z(0);
        gPlayerObject->Rot.y  = arena_export_player_yaw(0);

        /* A1.2b SPIKE: spawn ONE placeholder object once, at a fixed offset from
         * the player. Bomb mesh (gFileArray[9]) + benign plate objID. No sim
         * puppeting yet — this isolates "does a fresh spawn draw & not crash?". */
        if (!arena_export_puppet_ready()) {
            union { f32 f; u32 u; } cx, cy, cz;
            cx.f = gPlayerObject->Pos.x;
            cy.f = gPlayerObject->Pos.y;
            cz.f = gPlayerObject->Pos.z;
            arena_export_puppet_capture((s32)cx.u, (s32)cy.u, (s32)cz.u);

            struct ObjSpawnInfo info;
            info.unk0 = 0;              /* func_8001BD44 cfg A */
            info.unk2 = OBJ_RPLATE;     /* benign behaviour (inert plate) */
            info.unk4 = 9;             /* resident bomb-mesh file index */
            info.unk6 = 0;             /* func_8001BD44 cfg B */
            info.unk7 = 0; info.unk8 = 0; info.unk9 = 0; info.unkA = 0;
            {
                f32 px = gPlayerObject->Pos.x + 150.0f;   /* visible fixed offset */
                f32 py = gPlayerObject->Pos.y;
                f32 pz = gPlayerObject->Pos.z;
                s32 slot = func_80027464(1, &info, px, py, pz, 0.0f);
                arena_export_puppet_set_slot(1, slot);
            }
        }
    }
}
```
(Leave the `RECOMP_PATCH void func_800824A8(void)` block below unchanged.)

- [ ] **Step 2: Build**

Run **[build]** (the mandatory `make -C patches clean` runs first). Expected: `patches.elf` links, N64Recomp resolves all `arena_export_puppet_*` imports (present in `syms.ld` from Task 1) and `func_80027464`, native links, exe builds.

- [ ] **Step 3: Human boot gate**

Ask the user to **[launch]** `play.bat --show-console`, click Battle, enter the Battle Room, and report:
1. Does a single extra object (a bomb) appear a short distance from the player and stay put?
2. Does the game keep running (no crash, no graphics-worker access violation) for ~10+ seconds?

**If it crashes** (`0xC0000409`/`0xC0000005`): the plate `behaviour()` likely mis-touches the spawned object. First fallback: change `info.unk2 = OBJ_RPLATE` to `info.unk2 = OBJ_TOBIRA1_O` (door), rebuild, retest. If both crash, surface it — the next lever is finding a null-`behaviour()` objID by dumping `gObjInfo[objID].behaviour` (compare against `func_8002B144`), NOT thrashing. Record the objID that works; it is used in Tasks 3–5.

- [ ] **Step 4: Commit**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "feat(arena): A1.2b spawn spike - one placeholder object via func_80027464"
```

---

### Task 3: Puppet the one actor from the sim (frozen-origin), verify tracking + no mirror

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: Task 1 world-coord exports; the objID pinned in Task 2.
- Produces: puppet slot 1 tracking sim player 1 at a fixed world anchor (no mirror).

- [ ] **Step 1: Replace the static write with per-frame sim positioning**

In `patches/arena_render.c`, keep the spawn-once block from Task 2 but change its spawn position to the player's position (so the anchor is the player's spot, not +150), and add a per-frame positioning write for slot 1. The `arena_render_routine` body becomes:
```c
void arena_render_routine(void) {
    func_80024744();
    if (arena_bridge_is_battle() && gPlayerObject != NULL) {
        s32 sx = (s32)(gActiveContStickX * (31.0f / 80.0f));
        s32 sy = (s32)(gActiveContStickY * (31.0f / 80.0f));
        if (sx >  31) sx =  31;
        if (sx < -31) sx = -31;
        if (sy >  31) sy =  31;
        if (sy < -31) sy = -31;
        s32 jump = (gActiveContButton & CONT_A) ? 1 : 0;
        s32 bomb = (gActiveContButton & CONT_B) ? 1 : 0;
        arena_export_tick_input(sx, sy, jump, bomb);
        gPlayerObject->Pos.x += arena_export_player_x(0);
        gPlayerObject->Pos.z += arena_export_player_z(0);
        gPlayerObject->Rot.y  = arena_export_player_yaw(0);

        /* Spawn one placeholder once, anchored at the player's spawn Pos. */
        if (!arena_export_puppet_ready()) {
            union { f32 f; u32 u; } cx, cy, cz;
            cx.f = gPlayerObject->Pos.x;
            cy.f = gPlayerObject->Pos.y;
            cz.f = gPlayerObject->Pos.z;
            arena_export_puppet_capture((s32)cx.u, (s32)cy.u, (s32)cz.u);

            struct ObjSpawnInfo info;
            info.unk0 = 0; info.unk2 = OBJ_RPLATE; info.unk4 = 9;
            info.unk6 = 0; info.unk7 = 0; info.unk8 = 0; info.unk9 = 0; info.unkA = 0;
            {
                s32 slot = func_80027464(1, &info,
                                         gPlayerObject->Pos.x,
                                         gPlayerObject->Pos.y,
                                         gPlayerObject->Pos.z, 0.0f);
                arena_export_puppet_set_slot(1, slot);
            }
        }

        /* Position puppet 1 from the sim each frame (frozen anchor -> no mirror). */
        {
            s32 slot = arena_export_puppet_get_slot(1);
            if (slot >= 0) {
                gObjects[slot].Pos.x     = arena_export_puppet_wx(1);
                gObjects[slot].Pos.y     = arena_export_puppet_wy(1);
                gObjects[slot].Pos.z     = arena_export_puppet_wz(1);
                gObjects[slot].Rot.y     = arena_export_puppet_yaw(1);
                gObjects[slot].actionState = ACTION_IDLE;
            }
        }
    }
}
```
(If Task 2 pinned `OBJ_TOBIRA1_O` instead of `OBJ_RPLATE`, use that value for `info.unk2` here and in Task 4.)

- [ ] **Step 2: Build**

Run **[build]**. Expected: links + builds clean.

- [ ] **Step 3: Human boot gate**

**[launch]**, Battle → Battle Room. Report:
1. Does the placeholder sit at player 1's sim corner (offset from the player), and hold that world position as you move the stick (i.e., it does NOT slide opposite to the player — no mirror)?
2. Does its facing (`Rot.y`) look sane?
If it mirrors the player: the anchor isn't frozen — verify `arena_puppet_ready()` gates capture so `arena_puppet_capture` runs exactly once (check the console isn't re-capturing). If the scale is off (too near/far), that's the shared `g_scale` (feel pass) — note it, don't block.

- [ ] **Step 4: Commit**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "feat(arena): A1.2b puppet one actor from sim via frozen-origin world coords"
```

---

### Task 4: Scale to three placeholder actors (players 1–3) — shippable pass

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: 3 placeholder actors at the sim's player-1..3 positions. **This is the shippable exit state.**

- [ ] **Step 1: Loop spawn + positioning over players 1..3**

In `patches/arena_render.c`, replace the single-index spawn block and the single-index positioning block (from Task 3) with loops. The `arena_render_routine` body's puppet section becomes (leave the player-0 block above unchanged):
```c
        /* Spawn 3 placeholders once, all anchored at the player's spawn Pos. */
        if (!arena_export_puppet_ready()) {
            union { f32 f; u32 u; } cx, cy, cz;
            cx.f = gPlayerObject->Pos.x;
            cy.f = gPlayerObject->Pos.y;
            cz.f = gPlayerObject->Pos.z;
            arena_export_puppet_capture((s32)cx.u, (s32)cy.u, (s32)cz.u);

            s32 i;
            for (i = 1; i < 4; i++) {
                struct ObjSpawnInfo info;
                info.unk0 = 0; info.unk2 = OBJ_RPLATE; info.unk4 = 9;
                info.unk6 = 0; info.unk7 = 0; info.unk8 = 0; info.unk9 = 0; info.unkA = 0;
                {
                    s32 slot = func_80027464(1, &info,
                                             gPlayerObject->Pos.x,
                                             gPlayerObject->Pos.y,
                                             gPlayerObject->Pos.z, 0.0f);
                    arena_export_puppet_set_slot(i, slot);
                }
            }
        }

        /* Position all 3 from the sim each frame. */
        {
            s32 i;
            for (i = 1; i < 4; i++) {
                s32 slot = arena_export_puppet_get_slot(i);
                if (slot >= 0) {
                    gObjects[slot].Pos.x       = arena_export_puppet_wx(i);
                    gObjects[slot].Pos.y       = arena_export_puppet_wy(i);
                    gObjects[slot].Pos.z       = arena_export_puppet_wz(i);
                    gObjects[slot].Rot.y       = arena_export_puppet_yaw(i);
                    gObjects[slot].actionState = ACTION_IDLE;
                }
            }
        }
```

- [ ] **Step 2: Build**

Run **[build]**. Expected: links + builds clean.

- [ ] **Step 3: Human boot gate**

**[launch]**, Battle → Battle Room. Report:
1. Are there THREE extra placeholder actors, arranged around the player in the sim's layout (the four form a rough square)?
2. As you move the stick, do the three hold their own sim positions (no mirror, no jitter), and does the game stay stable?
If only 1–2 appear: some spawns returned −1 (no free slot) — unlikely given 64 slots, but check the console; else a `func_80027464` `count`/scan detail. Surface it.

- [ ] **Step 4: Commit + push**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "feat(arena): A1.2b spawn+puppet 3 placeholder actors from the sim (shippable)"
git push -u origin feature/a1.2b-spawn-bombers
```

---

### Task 5: Bomber-mesh upgrade attempt (bonus; placeholder is the fallback)

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: Task 4 (the shippable placeholder pass).
- Produces: EITHER real bomber-mesh actors (kept) OR a documented revert to the bomb placeholder.

- [ ] **Step 1: Swap the mesh file (+ its cfg) to the bomber mesh**

In `patches/arena_render.c`, in the spawn loop (Task 4), change the `ObjSpawnInfo` mesh fields from the bomb mesh to the bomber mesh (exactly the game's own player-mesh load args):
```c
                info.unk0 = 0; info.unk2 = OBJ_RPLATE; info.unk4 = 1;     /* gFileArray[1] = bomber mesh */
                info.unk6 = 0x13;                                          /* func_8001BD44 cfg B for the player mesh */
                info.unk7 = 0; info.unk8 = 0; info.unk9 = 0; info.unkA = 0;
```
(Only `unk4` 9→1 and `unk6` 0→0x13 change; `unk2` stays the pinned benign objID.)

- [ ] **Step 2: Build**

Run **[build]**. Expected: links + builds clean.

- [ ] **Step 3: Human boot gate**

**[launch]**, Battle → Battle Room. Report:
1. Do the three actors now render as bomber-shaped models (vs. bombs)?
2. Any crash, garbled mesh, or wrong scale/pose?

- [ ] **Step 4: Keep or revert (decision)**

- **If bombers render acceptably:** keep the change.
  ```bash
  cd /c/Users/dshi/GitRepos/BMHeroRecomp
  git add patches/arena_render.c
  git commit -m "feat(arena): A1.2b upgrade placeholders to the real bomber mesh (gFileArray[1])"
  git push
  ```
- **If it crashes or renders wrong:** revert the mesh fields to the bomb placeholder (`unk4 = 9`, `unk6 = 0`), rebuild to confirm the placeholder still works, and record the exact failure (crash code / visual) — that is the finding for the docs task. Do NOT keep a broken render.
  ```bash
  cd /c/Users/dshi/GitRepos/BMHeroRecomp
  git add patches/arena_render.c
  git commit -m "chore(arena): A1.2b bomber-mesh upgrade deferred - keep bomb placeholder (see notes)"
  git push
  ```

---

### Task 6: Docs — status + integration-notes correction (canonical repo)

**Files (bmhero-arena):** `CLAUDE.md`, `docs/bmhero-recomp-integration-notes.md`

- [ ] **Step 1: Update `CLAUDE.md` current status**

Add a new dated status entry (2026-07-21) summarizing: 3 placeholder actors spawned via `func_80027464` + synthesized `ObjSpawnInfo` into `gObjects[14..77]`, puppeted from the sim against a frozen origin (no mirror); the mesh/behaviour separation that unblocked it; and the bomber-mesh upgrade outcome (kept vs deferred, per Task 5). Mark A1.2b done; next A1.2c (bombs/blasts). Keep the existing older entries.

- [ ] **Step 2: Correct integration notes §8**

In `docs/bmhero-recomp-integration-notes.md` §8, add a resolution note: the "wall" was a conflation — mesh (`func_8001BD44` from resident `gFileArray[1]`/`[9]`) is separable from objID `behaviour()`; the working path is a fresh `func_80027464` spawn into `[14..77]` with a synthesized `ObjSpawnInfo` (resident `unk4`) + benign objID + frozen-origin puppeting. Note the bomber-mesh (`gFileArray[1]`) outcome from Task 5.

- [ ] **Step 3: Commit + push (bmhero-arena)**

```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md docs/bmhero-recomp-integration-notes.md
git commit -m "docs: A1.2b placeholder actors on screen; correct the multi-actor RE wall"
git push
```

- [ ] **Step 4: Update memory (if warranted)**

If Task 5 produced a durable new fact (e.g., the bomber mesh needs animation state, or the pinned benign objID), add/adjust a `reference`/`project` memory and a one-line `MEMORY.md` pointer. Skip if nothing non-obvious beyond what the docs already capture.

---

## Plan self-review notes

- **Spec coverage:** §1 exit criterion → Tasks 4 (placeholder) + 5 (bomber attempt); §2 mesh/behaviour separation → encoded in Task 1 exports + Task 2/5 `ObjSpawnInfo.unk4`; §3 synthesized `ObjSpawnInfo` + `func_80027464` → Task 2/3/4 spawn blocks; §4 frozen-origin positioning (no mirror) → Task 1 `arena_puppet_capture`/`_wx/_wz` + Task 3 boot gate; §5 benign objID + post-update ordering → Global Constraints facts + Task 2 objID pin + `func_80024744()`-first ordering; §6 two passes one-arg diff → Task 4 vs Task 5 (`unk4` 9↔1); §7 native-state/patch-stateless bridge → Task 1; §8 sequenced steps → Tasks 2→3→4→5; §9 non-goals → not implemented (color/anim/A1.2c untouched); §10 risks → Task 2/3/5 boot-gate fallbacks; §11 build/testing → `[build]`/`[launch]` macros + human boot gates.
- **Placeholder scan:** the only unresolved value is the benign objID, deliberately pinned on-screen in Task 2 with a concrete first candidate (`OBJ_RPLATE`) and fallback ladder (door → null-`behaviour()` hunt) — a spike deliverable, not a plan gap. `g_scale` is the existing A1.2a feel constant (not this slice's scope).
- **Type/name consistency:** three name layers kept distinct — native C++ (`arena_puppet_*`), exported shims (`arena_export_puppet_*` = `syms.ld` = `REGISTER_FUNC` = patch `DECLARE_FUNC`), no collisions. Float ABI honored: args as `u32` bit patterns (`arena_export_puppet_capture(s32,s32,s32)` fed `(s32)union.u`), returns via `_return`. `ObjSpawnInfo` fields (`unk0/2/4/6/7/8/9/A`) and `ObjectStruct` fields (`Pos`,`Rot`,`actionState`) match the decomp. `func_80027464` signature matches `boot/26CE0.c:328`.
- **Dead code note:** the prior session's raw-clone exports (`arena_export_spawn_clone_once`, `arena_export_spawn_placeholders_once`, `arena_export_bomber_off_x/z/yaw`) are superseded by this approach and left in place (unused, harmless); a later cleanup pass may remove them across `syms.ld`/`main.cpp`/`arena_bridge*` together.
