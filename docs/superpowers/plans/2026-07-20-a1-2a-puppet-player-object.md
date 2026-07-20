# A1.2a Puppet Player Object Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In the Battle Room, drive the campaign player object (`gPlayerObject`) each frame from our fixed-point sim, so Hero renders a bomberman moving per `ArenaState`.

**Architecture:** A tiny `RECOMP_PATCH` on the level-enter setup `func_800824A8` redirects the per-frame routine pointer `gDebugRoutine2` through our wrapper; each frame the wrapper reads the controller, ticks our sim via a native export, then writes `gPlayerObject->Pos/Rot` from native coord-mapper exports. Uses the A1.1b-ii native-export/`syms.ld` pattern + LLVM-15 patch path. Spec: `docs/superpowers/specs/2026-07-20-a1-2a-puppet-player-object-design.md`.

**Tech Stack:** VS 2022 clang-cl (native), LLVM-15 MIPS (patches), the A1.0 three-toolchain recipe (memory `recomp-build-toolchain`).

## Global Constraints

- Fork `C:\Users\dshi\GitRepos\BMHeroRecomp`, branch `feature/a1.2a-puppet-player` off `master`. `bmhero-arena` gets only a CLAUDE.md doc update.
- **`bmhero-arena` sim code not modified** (pinned hash `4b6687d4`); no submodule bump.
- Shell map stays `MAP_BATTLE_ROOM` (A1.1b-ii); Nitros eval is a separate side task.
- Verified facts (decomp at fork `master`):
  - `struct ObjectStruct`: `Vec3f Pos` @0x00, `Vec3f Rot` @0x18 (`Rot.y` = facing, degrees); `s16 objID` @0xE4; `s16 actionState` @0xA4 (`lib/bmhero/include/obj.h`).
  - `gPlayerObject` — `struct ObjectStruct*` (RAM `0x8017753C`), set to `gObjects[0]` at level load; declared `extern` in `variables.h`.
  - Per-frame in-level routines: the main loop (`boot/17930.c:1562,1797`) calls `gDebugRoutine1()` and `gDebugRoutine2()` each frame. `func_800824A8` (`code/71AA0.c:649`, 6 lines) is the level-enter setup — called from the same 9 level-transition handlers as `func_80081C50` — and sets `gDebugRoutine1 = &func_800821E0`, `gDebugRoutine2 = &func_80024744`.
  - Controller globals (`variables.h`): `f32 gActiveContStickX`, `f32 gActiveContStickY` (N64 stick, ≈±80), `u16 gActiveContButton` (held buttons). Masks: `CONT_A 0x8000` (jump), `CONT_B 0x4000` (bomb) (`PR/os_cont.h`).
  - Native export ABI: `extern "C" void f(uint8_t* rdram, recomp_context* ctx)`; read args `_arg<N,T>(rdram,ctx)`, return `_return(ctx,val)` (`recomp.h` + `librecomp/helpers.hpp`); register `REGISTER_FUNC(f)` in `main.cpp`; patch imports `DECLARE_FUNC` + a `syms.ld` `0x8F...` address (A1.1b-ii pattern).
- **[build]** (VS dev shell + composed PATH):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; cmake --build build-cmake --target BMHeroRecompiled
  ```
- **[launch]** = `play.bat --show-console` from the fork root.
- Verification: build-exit + **human boot gate** (only a human sees the bomberman move).

---

### Task 1: Native exports — tick-with-input + coord-mapped getters

**Files (fork):**
- Modify: `src/arena_bridge/arena_bridge.{h,cpp}` (coord constants + getters)
- Modify: `src/arena_bridge/arena_bridge_export.cpp` (recomp-ABI shims)
- Modify: `src/main/main.cpp` (register exports; gate the VI tick in battle)
- Modify: `patches/syms.ld` (import addresses)

**Interfaces:**
- Produces (patch-callable): `arena_bridge_tick_input(int sx, int sy, int jump, int bomb)` (packs + ticks player 0); `float arena_get_player_x(int i)`, `_y`, `_z`, `_yaw_deg(int i)` (ArenaState[i] → Hero coords).

- [ ] **Step 1: Branch**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git checkout master && git checkout -b feature/a1.2a-puppet-player
```

- [ ] **Step 2: Coord constants + getters on the bridge**

In `src/arena_bridge/arena_bridge.h`, add inside the `extern "C"` block:
```c
void  arena_bridge_tick_input(int sx, int sy, int jump, int bomb);  /* tick player 0 */
float arena_get_player_x(int i);
float arena_get_player_y(int i);
float arena_get_player_z(int i);
float arena_get_player_yaw_deg(int i);
```
In `src/arena_bridge/arena_bridge.cpp`, add near the top (after includes):
```cpp
/* Coord mapping: ArenaState Q20.12 -> Hero Battle-Room float coords.
 * TODO(feel): calibrate on-screen (Task 3). Seed: arena is +/-6 units;
 * guess ~40 Hero units per arena unit, room centered near origin. */
static float g_scale    = 40.0f;
static float g_origin_x = 0.0f;
static float g_origin_y = 0.0f;   /* ground height in Hero coords */
static float g_origin_z = 0.0f;

static float qf(int32_t q) { return (float)q / 4096.0f; }  /* Q20.12 -> float */
```
And after `arena_bridge_tick` / the accessors, add:
```cpp
extern "C" void arena_bridge_tick_input(int sx, int sy, int jump, int bomb) {
    /* ensure init (mirror arena_bridge_tick's lazy init) */
    arena_bridge_tick_ensure_init();
    ArenaInput in[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
    in[0] = arena_input_pack(sx, sy, jump, bomb, 0);
    arena_tick(arena_bridge_state(), in);
}

extern "C" float arena_get_player_x(int i) {
    return qf(arena_bridge_state()->players[i].pos.x) * g_scale + g_origin_x;
}
extern "C" float arena_get_player_y(int i) {
    return qf(arena_bridge_state()->players[i].pos.y) * g_scale + g_origin_y;
}
extern "C" float arena_get_player_z(int i) {
    return qf(arena_bridge_state()->players[i].pos.z) * g_scale + g_origin_z;
}
extern "C" float arena_get_player_yaw_deg(int i) {
    /* binary angle (65536 = full turn) -> degrees */
    return (float)arena_bridge_state()->players[i].yaw * (360.0f / 65536.0f);
}
```
This needs two small refactors in `arena_bridge.cpp` so the new functions can reach the state and init: extract the lazy-init and expose the state pointer. Replace the top-of-file `namespace { ... g_state ...; }` and `arena_bridge_tick`'s init block so that:
```cpp
// (in the anonymous namespace, keep g_inited/g_state/g_calls/g_log/g_battle_mode)
static void ensure_init() {
    if (!g_inited) {
        arena_init(&g_state, 0, 4, 0xB0BB1E5u);
        g_inited = true;
        g_log = std::fopen("arena_bridge.log", "w");
    }
}
```
and add (outside the namespace):
```cpp
void arena_bridge_tick_ensure_init(void) { ensure_init(); }
ArenaState* arena_bridge_state(void) { return &g_state; }
```
(declare both `extern "C"` in the anonymous-namespace-adjacent scope or file-local `extern "C"` prototypes; they are internal helpers, no syms.ld entry). Update `arena_bridge_tick` to call `ensure_init()` instead of its inline init.

- [ ] **Step 3: Recomp-ABI shims for the exports**

In `src/arena_bridge/arena_bridge_export.cpp`, add:
```cpp
extern "C" void arena_export_tick_input(uint8_t* rdram, recomp_context* ctx) {
    int sx   = _arg<0, int>(rdram, ctx);
    int sy   = _arg<1, int>(rdram, ctx);
    int jump = _arg<2, int>(rdram, ctx);
    int bomb = _arg<3, int>(rdram, ctx);
    arena_bridge_tick_input(sx, sy, jump, bomb);
}
extern "C" void arena_export_player_x(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_get_player_x(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_player_y(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_get_player_y(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_player_z(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_get_player_z(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_player_yaw(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_get_player_yaw_deg(_arg<0, int>(rdram, ctx)));
}
```
(Verify `_arg<N,int>` / `_arg` float handling against `recomp_api.cpp` usages; int args and float returns are both used there.)

- [ ] **Step 4: Register exports + syms.ld addresses**

In `src/main/main.cpp`, add extern decls near the A1.1b-ii one:
```cpp
extern "C" void arena_export_tick_input(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_player_x(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_player_y(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_player_z(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_player_yaw(uint8_t* rdram, recomp_context* ctx);
```
and alongside `REGISTER_FUNC(arena_bridge_is_battle);`:
```cpp
    REGISTER_FUNC(arena_export_tick_input);
    REGISTER_FUNC(arena_export_player_x);
    REGISTER_FUNC(arena_export_player_y);
    REGISTER_FUNC(arena_export_player_z);
    REGISTER_FUNC(arena_export_player_yaw);
```
In `patches/syms.ld`, after `arena_bridge_is_battle = 0x8F000124;`:
```
arena_export_tick_input = 0x8F000128;
arena_export_player_x = 0x8F00012C;
arena_export_player_y = 0x8F000130;
arena_export_player_z = 0x8F000134;
arena_export_player_yaw = 0x8F000138;
```

- [ ] **Step 5: Gate the VI-callback tick in battle mode**

The A1.2a tick happens in the patch (Task 2). Stop the free-running VI tick from double-ticking in battle. In `src/arena_bridge/arena_bridge.cpp` `arena_bridge_tick`, wrap the tick+log body so it only runs when NOT in battle:
```cpp
extern "C" void arena_bridge_tick(void) {
    ensure_init();
    if (g_battle_mode) return;   /* battle: the render patch drives the tick */
    const ArenaInput neutral[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
    arena_tick(&g_state, neutral);
    if ((++g_calls % 60u) == 0u)
        proof("[arena] tick %u hash %08x\n", g_state.tick, arena_hash(&g_state));
}
```

- [ ] **Step 6: Build (native only so far; patch calls come in Task 2)**

Run **[build]**. Expected: native compiles (new exports), `patches.elf`/N64Recomp still succeed (no new patch symbols referenced yet — syms.ld additions are just address definitions, harmless if unreferenced), `Linking CXX executable BMHeroRecompiled.exe`.

- [ ] **Step 7: Commit**

```bash
git add src/arena_bridge/ src/main/main.cpp patches/syms.ld
git commit -m "feat(arena): native tick-with-input + coord-mapped player getters; gate VI tick in battle (A1.2a)"
```

---

### Task 2: Per-frame render patch — puppet gPlayerObject

**Files (fork):** Create `patches/arena_render.c`

**Interfaces:**
- Consumes: Task 1 exports; `gPlayerObject`, `gActiveContStickX/Y`, `gActiveContButton`, `func_80024744`, `func_800821E0`, `gDebugRoutine1/2`, `func_8001ECB8`, `func_80081D78`, `func_80000964`, `arena_bridge_is_battle`.

- [ ] **Step 1: Write the render patch**

Create `patches/arena_render.c`:
```c
#include "patches.h"
#include "misc_funcs.h"

#include <ultra64.h>
#include "types.h"
#include "variables.h"

/* Import the EXPORT (syms.ld / REGISTER_FUNC) names, not the internal C++ names. */
DECLARE_FUNC(s32,  arena_bridge_is_battle);
DECLARE_FUNC(void, arena_export_tick_input, s32 sx, s32 sy, s32 jump, s32 bomb);
DECLARE_FUNC(f32,  arena_export_player_x, s32 i);
DECLARE_FUNC(f32,  arena_export_player_y, s32 i);
DECLARE_FUNC(f32,  arena_export_player_z, s32 i);
DECLARE_FUNC(f32,  arena_export_player_yaw, s32 i);

extern void func_80024744(void);            /* original per-frame routine 2 */
extern void func_800821E0(void);            /* per-frame routine 1 (draw) */
extern void func_8001ECB8(void);
extern void func_80081D78(void);
extern void func_80000964(void);
extern void (*gDebugRoutine1)(void);
extern void (*gDebugRoutine2)(void);

/* Per-frame in-level: run the original routine-2 work, then in battle mode
 * drive our sim from the controller and puppet gPlayerObject from it. */
void arena_render_routine(void) {
    func_80024744();
    if (arena_bridge_is_battle() && gPlayerObject != NULL) {
        /* N64 stick (~+/-80) -> sim stick (+/-31); sim stick up = -Z */
        s32 sx = (s32)(gActiveContStickX * (31.0f / 80.0f));
        s32 sy = (s32)(gActiveContStickY * (31.0f / 80.0f));
        if (sx >  31) sx =  31; if (sx < -31) sx = -31;
        if (sy >  31) sy =  31; if (sy < -31) sy = -31;
        s32 jump = (gActiveContButton & CONT_A) ? 1 : 0;
        s32 bomb = (gActiveContButton & CONT_B) ? 1 : 0;
        arena_export_tick_input(sx, sy, jump, bomb);
        gPlayerObject->Pos.x = arena_export_player_x(0);
        gPlayerObject->Pos.y = arena_export_player_y(0);
        gPlayerObject->Pos.z = arena_export_player_z(0);
        gPlayerObject->Rot.y = arena_export_player_yaw(0);
    }
}

/* Level-enter setup: original body, but route per-frame routine 2 through
 * our wrapper so the puppet write runs every frame in-level. */
RECOMP_PATCH void func_800824A8(void) {
    func_8001ECB8();
    gDebugRoutine1 = &func_800821E0;
    gDebugRoutine2 = &arena_render_routine;   /* was &func_80024744 */
    func_80081D78();
    func_80000964();
}
```
(`arena_render_routine` is a plain patch function used as a function pointer — the recomp dispatches indirect calls through its function table, which includes patch functions. If that assumption is wrong, see Step 3's fallback.)

- [ ] **Step 2: Build**

Run **[build]** (patch recompile + native). Expected: `patches.elf` links, `N64Recomp patches.toml` resolves all `arena_*` imports (present in syms.ld from Task 1), native links, exe builds. If N64Recomp reports an undefined `arena_*` symbol, a syms.ld entry is missing/misnamed vs the `REGISTER_FUNC` name.

- [ ] **Step 3: Human boot gate**

Ask the user to **[launch]** `play.bat --show-console`, click Battle, enter the Battle Room, and report:
1. Does the bomberman respond to the stick, moving per our sim (may be wrong scale/offset — that's Task 3)?
2. Console: any `[arena]` errors; does it stay running (no crash from the function-pointer redirect)?
**Fallback if the puppet never moves / the routine isn't called** (function-pointer-to-patch not dispatched): instead of redirecting `gDebugRoutine2`, whole-replace the *small* per-frame path differently — patch `func_800824A8` to keep the original routine2 and instead add the write by patching `func_80081C50`'s sibling... no: the concrete fallback is to make `arena_render_routine` reachable by giving it `RECOMP_EXPORT` and confirming the recomp's indirect-call table includes exports; if still not called, move the tick+write into the A1.1b-ii `func_80081C50` path is not per-frame — instead patch the per-frame `func_80024744` by whole-replacement is 100 lines (avoid). Report the symptom and we choose between (a) `RECOMP_EXPORT` on the routine, (b) a native VI-driven RDRAM write if a global rdram handle can be captured at init. Do not thrash silently — surface it.

- [ ] **Step 4: Commit**

```bash
git add patches/arena_render.c
git commit -m "feat(arena): per-frame render patch - puppet gPlayerObject from sim (A1.2a)"
```

---

### Task 3: Calibrate the coordinate mapping (human, on-screen)

**Files (fork):** `src/arena_bridge/arena_bridge.cpp` (`g_scale`, `g_origin_*`)

- [ ] **Step 1: Iterate scale/origin from what's on screen**

With Task 2 running, the bomberman moves but likely at the wrong scale/position. Ask the user to describe: movement too fast/large (lower `g_scale`), too small (raise it), or offset from where the stick should put it (adjust `g_origin_x/z`); if it floats/sinks, adjust `g_origin_y` to ground. Change the constants in `arena_bridge.cpp`, **[build]**, **[launch]**, repeat until the bomberman moves a sensible distance across the room and sits on the ground. Record the values.
(Guidance: the arena is ±6 sim units wide; pick `g_scale` so full-stick travel crosses roughly the room, and `g_origin` so the sim's center maps to the room's center/floor.)

- [ ] **Step 2: Commit the calibrated constants**

```bash
git add src/arena_bridge/arena_bridge.cpp
git commit -m "chore(arena): calibrate A1.2a coord mapping (scale/origin) to the Battle Room"
git push -u origin feature/a1.2a-puppet-player
```

---

### Task 4: Docs

**Files:** `bmhero-arena` — `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md status**

Append to the status section (fill calibrated scale/origin):
```markdown
**A1.2a complete (2026-07-20).** First render bridge: in the Battle Room the
campaign player object (`gPlayerObject`) is puppeted each frame from our
fixed-point sim — a `RECOMP_PATCH` on `func_800824A8` routes the per-frame
`gDebugRoutine2` through `arena_render_routine`, which reads the controller,
ticks the sim via `arena_bridge_tick_input`, and writes `gPlayerObject->Pos/Rot`
from coord-mapped native getters (scale <S>, origin <O>). The VI-callback
tick is gated off in battle (patch drives the tick for render lockstep).
Moving the stick moves the on-screen bomberman per our physics. Fork branch
`feature/a1.2a-puppet-player`. Next: A1.2b spawn+puppet all 4 bombers.
```
Update the "Next milestones" A1.2 line: mark A1.2a done, next A1.2b.

- [ ] **Step 2: Commit + push (bmhero-arena)**

```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md
git commit -m "docs: A1.2a complete - player object puppeted from the sim"
git push
```

---

## Plan self-review notes

- Spec coverage: §2 mechanism → Tasks 1-2 (native tick+getters, patch routes gDebugRoutine2 through the wrapper; tick in patch, VI gated); §3 coord mapping → Task 1 constants + Task 3 calibration; §4 data flow → Task 2 routine order (tick → get → write); §6 risks: patch point resolved statically to `func_800824A8`/`gDebugRoutine2` (not left to instrumentation — better than the spec's "discovery"), with the one residual unknown (function-pointer-to-patch dispatch) called out with a surface-don't-thrash fallback in Task 2 Step 3; coord scale = Task 3; overwrite-vs-physics is moot here because we replace routine2's contribution, and gPlayerObject's own physics update... (note: if Hero's separate player-physics still moves gPlayerObject and fights our write, the fallback is the same as spec §6.3 — gate it; surfaced at the Task 2 boot gate).
- Type consistency (verified, fixed inline): three name layers, kept distinct on purpose — internal C++ (`arena_bridge_tick_input`, `arena_get_player_x/y/z/yaw_deg`, `arena_bridge.cpp`), recomp-ABI shims = the exported names (`arena_export_tick_input`, `arena_export_player_x/y/z/yaw`, in `arena_bridge_export.cpp` + `REGISTER_FUNC` + `syms.ld`), and the patch `DECLARE_FUNC`s import the **export** names. Task 2's patch was corrected to call `arena_export_*` (not the C++ names) — the shim name can't equal the C++ name anyway (same-symbol collision), so the two layers must differ.
