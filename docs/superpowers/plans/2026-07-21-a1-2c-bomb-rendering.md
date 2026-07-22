# A1.2c Bomb Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the sim's live bombs in the arena (pool of 16 bomb actors toggled from `bombs[].state`, positioned from the sim incl. the throw arc) and wire the set/kick input, so throwing/setting a bomb makes one appear, arc, land, and vanish on detonation.

**Architecture:** Reuse the A1.2b render-bridge machinery verbatim — the bomb model (`gFileArray[9]`) + anim bind (`func_8001ABF4`) + frozen-origin positioning + boss-suppression sweep. Native `arena_bridge` gains bomb-state getters (reading `state->bombs[i]`) + a bomb-slot table + a `set` input arg + a `g_ref_sy` capture; the patch spawns a 16-actor bomb pool once and drives it each frame. Spec: `docs/superpowers/specs/2026-07-21-a1-2c-bomb-rendering-design.md`.

**Tech Stack:** VS 2022 clang-cl (native), LLVM-15 MIPS (patches), the A1.0 three-toolchain recipe (memory `recomp-build-toolchain`).

## Global Constraints

- Fork `C:\Users\dshi\GitRepos\BMHeroRecomp`, branch `feature/a1.2b-spawn-bombers` (continue — A1.2b's bridge is the base). `bmhero-arena` gets only doc updates.
- **`bmhero-arena` sim code NOT modified** (pinned hash `4b6687d4`); no submodule bump. The bomb *cap* (6→16) is a sim change and is explicitly out of scope (A1.3).
- Arena is `MAP_NITROS_1` (`ARENA_WARP_MAP`=15, boss-suppressed) — unchanged.
- **Patches must be STATELESS** — all mutable state in native `arena_bridge.cpp` (patch-local mutable statics abort `0xC0000409`). Patch reads game globals, calls exports, writes game objects, uses locals only.
- **No `memset`/`memcpy` in the patch** (unresolved libc at `-O0`) — init structs field-by-field; bitcast floats with a `union`.
- **No float ARGS across the export ABI** — pass floats as `u32` bit patterns through int args; float RETURNS via `_return` are fine.
- Verified facts (this repo / decomp, as used in A1.2b):
  - Sim state (`src/arena/arena_state.h`): `ArenaState.bombs[16]` (`ARENA_MAX_BOMBS`), each `ArenaBomb { Vec3q pos; Vec3q vel; u8 owner; u8 state; u16 fuse; u8 bounced; u8 pad[3]; }`. `state==BSTATE_FREE` (=0) means empty; non-zero = live. `find_free_bomb` fills the lowest free index. Bombs live in the same sim space as players (Q20.12).
  - Input (`arena_state.h`): `arena_input_pack(sx,sy,jump,bomb,set)` — bit 13 `bomb`, bit 14 `set` (edge-triggered, sim edge-detects via `last_input`).
  - Bomb render recipe (A1.2b, integration notes §8): `func_80027464(1,&info,x,y,z,0)` with `info.unk4=9`, `info.unk2=OBJ_TOBIRA1_O`, then `func_8001ABF4(slot,0,0,(struct T*)0x801163DC)` (bomb anim; `D_801163DC` is a data symbol → literal address). `struct ObjSpawnInfo` fields `unk0/2/4/6/7/8/9/A`. `struct ObjectStruct`: `Vec3f Pos`@0, `Vec3f Rot`@0x18, `s16 actionState`@0xA4. `gObjects[207]`, `gPlayerObject`, `gActiveContStickX/Y`, `gActiveContButton` in `variables.h`; `ACTION_NONE`=0/`ACTION_IDLE`=1, `OBJ_TOBIRA1_O`=77 in `obj.h`. `CONT_G`=0x2000 (`PR/os_cont.h`; Z trigger / Q key).
  - Native puppet mapping (from A1.2b, `arena_bridge.cpp`): statics `g_origin_x/y/z`, `g_ref_sx/sz`, `g_scale`(120)/`g_scale_z`, `qf(q)=q/4096.0f`, `g_puppet_slot[4]`, `g_state`. `arena_puppet_capture(bx,by,bz)` freezes origin+ref; `arena_puppet_get_slot(i)`. The boss sweep in `arena_render.c` deactivates every `gObjects[14..77]` not in `{puppet slots 1-3}` before `func_80024744`.
  - Export ABI: `extern "C" void f(uint8_t* rdram, recomp_context* ctx)`; `_arg<N,T>` (int/ptr slots), `_return(ctx,val)`; `REGISTER_FUNC(f)`+fwd-decl in `main.cpp`; patch `DECLARE_FUNC`+`syms.ld 0x8F...`. Existing exports end at `arena_export_dbg_u32 = 0x8F000174`.
- **[build]** (VS dev shell + composed PATH; the `make -C patches clean` is MANDATORY after any `patches/*.c` edit — stale `patches.elf` behaves as old code):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; make.exe -C patches clean; cmake --build build-cmake --target BMHeroRecompiled
  ```
- **[launch]** = human runs `play.bat` from the fork root, navigates arrow→Battle→Enter, then Start + A×3 into the arena. Agent verifies via the scratchpad `capture-game.ps1` (PrintWindow screenshot) + `arena_bridge.log`. Hands-off input injection is unreliable (SDL focus) — the human does the nav.
- Verification: build-exit gate + **human boot gate** (only a human can throw a bomb and watch it).

---

### Task 1: Native bomb state — `set` input, `g_ref_sy`, bomb getters + slot table + exports

**Files (fork):**
- Modify: `src/arena_bridge/arena_bridge.cpp` (bomb statics + `g_ref_sy` + `set` arg + bomb fns)
- Modify: `src/arena_bridge/arena_bridge.h` (declare new fns; update `arena_bridge_tick_input` sig)
- Modify: `src/arena_bridge/arena_bridge_export.cpp` (shims)
- Modify: `src/main/main.cpp` (register exports; fwd-decls)
- Modify: `patches/syms.ld` (import addresses)

**Interfaces:**
- Produces (patch-callable EXPORT names): `arena_export_tick_input(sx,sy,jump,bomb,set)` (now 5 args); `arena_export_bomb_active(i)->s32`; `arena_export_bomb_wx/wy/wz(i)->f32`; `arena_export_bomb_set_slot(i,slot)`; `arena_export_bomb_get_slot(i)->s32`; `arena_export_is_actor_slot(slot)->s32`.

- [ ] **Step 1: Bomb statics + `g_ref_sy` in `arena_bridge.cpp`**

In the anonymous namespace, next to `g_ref_sx, g_ref_sz`, add `g_ref_sy` and a bomb-slot table:
```cpp
    float g_ref_sy = 0.0f;                              /* frozen sim ref Y (p0) */
    int   g_bomb_slot[ARENA_MAX_BOMBS] = {              /* A1.2c: 16 bomb actors */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
```

- [ ] **Step 2: Capture `g_ref_sy`; add `set` to the tick**

In `arena_puppet_capture`, alongside `g_ref_sx`/`g_ref_sz`:
```cpp
    g_ref_sy = qf(g_state.players[0].pos.y);
```
Change `arena_bridge_tick_input` to take `set` and pack it. Replace its signature + the `in[0]` line:
```cpp
extern "C" void arena_bridge_tick_input(int sx, int sy, int jump, int bomb, int set) {
    ensure_init();
    Vec3q before = g_state.players[0].pos;
    ArenaInput neutral = arena_input_pack(0, 0, 0, 0, 0);
    ArenaInput in[ARENA_MAX_PLAYERS] = { neutral, neutral, neutral, neutral };
    in[0] = arena_input_pack(sx, sy, jump, bomb, set);
    arena_tick(&g_state, in);
```
(Leave the rest of the function — the `g_render_*` delta calc + `[simpos]` log — unchanged.)

- [ ] **Step 3: Bomb getters + slot table + actor-slot check (end of `arena_bridge.cpp`)**

After the puppet functions, add:
```cpp
/* A1.2c bombs: same frozen frame as the puppets, reading state->bombs[i]. */
extern "C" int arena_bomb_active(int i) {
    if (i < 0 || i >= ARENA_MAX_BOMBS) return 0;
    return g_state.bombs[i].state != BSTATE_FREE ? 1 : 0;
}
extern "C" float arena_bomb_wx(int i) {
    if (i < 0 || i >= ARENA_MAX_BOMBS) return g_origin_x;
    return g_origin_x + (qf(g_state.bombs[i].pos.x) - g_ref_sx) * g_scale;
}
extern "C" float arena_bomb_wy(int i) {
    if (i < 0 || i >= ARENA_MAX_BOMBS) return g_origin_y;
    return g_origin_y + (qf(g_state.bombs[i].pos.y) - g_ref_sy) * g_scale;   /* arc height */
}
extern "C" float arena_bomb_wz(int i) {
    if (i < 0 || i >= ARENA_MAX_BOMBS) return g_origin_z;
    return g_origin_z + (qf(g_state.bombs[i].pos.z) - g_ref_sz) * g_scale_z;
}
extern "C" void arena_bomb_set_slot(int i, int slot) { if (i >= 0 && i < ARENA_MAX_BOMBS) g_bomb_slot[i] = slot; }
extern "C" int  arena_bomb_get_slot(int i)           { return (i >= 0 && i < ARENA_MAX_BOMBS) ? g_bomb_slot[i] : -1; }

/* 1 if `slot` is one of our actors (player puppet or bomb) — for the boss sweep. */
extern "C" int arena_is_actor_slot(int slot) {
    for (int i = 1; i < ARENA_MAX_PLAYERS; i++) if (g_puppet_slot[i] == slot) return 1;
    for (int i = 0; i < ARENA_MAX_BOMBS;   i++) if (g_bomb_slot[i]   == slot) return 1;
    return 0;
}
```
(`BSTATE_FREE`, `ARENA_MAX_BOMBS`, `ARENA_MAX_PLAYERS` come from `arena/arena_state.h`, already included via `arena/arena_sim.h`.)

- [ ] **Step 4: Declarations in `arena_bridge.h`**

Update the `arena_bridge_tick_input` decl and add the bomb fns inside the `extern "C"` block:
```c
void  arena_bridge_tick_input(int sx, int sy, int jump, int bomb, int set);  /* tick player 0 */
int   arena_bomb_active(int i);
float arena_bomb_wx(int i);
float arena_bomb_wy(int i);
float arena_bomb_wz(int i);
void  arena_bomb_set_slot(int i, int slot);
int   arena_bomb_get_slot(int i);
int   arena_is_actor_slot(int slot);
```

- [ ] **Step 5: Shims in `arena_bridge_export.cpp`**

Replace `arena_export_tick_input` (now 5 args) and append the bomb shims:
```cpp
extern "C" void arena_export_tick_input(uint8_t* rdram, recomp_context* ctx) {
    int sx   = _arg<0, int>(rdram, ctx);
    int sy   = _arg<1, int>(rdram, ctx);
    int jump = _arg<2, int>(rdram, ctx);
    int bomb = _arg<3, int>(rdram, ctx);
    int set  = _arg<4, int>(rdram, ctx);
    arena_bridge_tick_input(sx, sy, jump, bomb, set);
}
extern "C" void arena_export_bomb_active(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_bomb_active(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_bomb_wx(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_bomb_wx(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_bomb_wy(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_bomb_wy(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_bomb_wz(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_bomb_wz(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_bomb_set_slot(uint8_t* rdram, recomp_context* ctx) {
    arena_bomb_set_slot(_arg<0, int>(rdram, ctx), _arg<1, int>(rdram, ctx));
}
extern "C" void arena_export_bomb_get_slot(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_bomb_get_slot(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_is_actor_slot(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_is_actor_slot(_arg<0, int>(rdram, ctx)));
}
```

- [ ] **Step 6: Register in `main.cpp`**

Add the fwd-decls next to the other `arena_export_*`:
```cpp
extern "C" void arena_export_bomb_active(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_bomb_wx(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_bomb_wy(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_bomb_wz(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_bomb_set_slot(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_bomb_get_slot(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_is_actor_slot(uint8_t* rdram, recomp_context* ctx);
```
and in the `REGISTER_FUNC` block (after `arena_export_dbg_u32`):
```cpp
    REGISTER_FUNC(arena_export_bomb_active);
    REGISTER_FUNC(arena_export_bomb_wx);
    REGISTER_FUNC(arena_export_bomb_wy);
    REGISTER_FUNC(arena_export_bomb_wz);
    REGISTER_FUNC(arena_export_bomb_set_slot);
    REGISTER_FUNC(arena_export_bomb_get_slot);
    REGISTER_FUNC(arena_export_is_actor_slot);
```
(`arena_export_tick_input` is already registered — only its shim body changed.)

- [ ] **Step 7: `syms.ld` addresses**

In `patches/syms.ld`, after `arena_export_dbg_u32 = 0x8F000174;`:
```
arena_export_bomb_active = 0x8F000178;
arena_export_bomb_wx = 0x8F00017C;
arena_export_bomb_wy = 0x8F000180;
arena_export_bomb_wz = 0x8F000184;
arena_export_bomb_set_slot = 0x8F000188;
arena_export_bomb_get_slot = 0x8F00018C;
arena_export_is_actor_slot = 0x8F000190;
```

- [ ] **Step 8: Build**

Run **[build]**. Expected: native compiles (new bomb fns + 5-arg tick), patch relinks (the existing patch still calls `arena_export_tick_input` with 4 args — see Task 2 for the 5-arg update; **this step will build but the 4-arg call is now ABI-mismatched, so do NOT boot between Task 1 and Task 2**), exe links. If the native side fails to compile, check `BSTATE_FREE`/`ARENA_MAX_BOMBS` are visible (they are, via `arena/arena_sim.h`).

- [ ] **Step 9: Commit**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add src/arena_bridge/ src/main/main.cpp patches/syms.ld
git commit -m "feat(arena): native bomb-state exports + set input + g_ref_sy (A1.2c)"
```

---

### Task 2: Patch — bomb pool spawn + per-frame toggle + sweep exclusion + set wiring

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: Task 1 exports; `func_80027464`, `func_8001ABF4`, `D_801163DC_ADDR`, `gObjects`, `gPlayerObject`, `gActiveContButton`, `CONT_G`, `OBJ_TOBIRA1_O`, `ACTION_IDLE/NONE`.

- [ ] **Step 1: Update imports (5-arg tick + bomb exports)**

In `patches/arena_render.c`, change the `arena_export_tick_input` `DECLARE_FUNC` to 5 args and add the bomb imports after the puppet ones:
```c
DECLARE_FUNC(void, arena_export_tick_input, s32 sx, s32 sy, s32 jump, s32 bomb, s32 set);
```
```c
/* A1.2c bomb exports */
DECLARE_FUNC(s32,  arena_export_bomb_active, s32 i);
DECLARE_FUNC(f32,  arena_export_bomb_wx, s32 i);
DECLARE_FUNC(f32,  arena_export_bomb_wy, s32 i);
DECLARE_FUNC(f32,  arena_export_bomb_wz, s32 i);
DECLARE_FUNC(void, arena_export_bomb_set_slot, s32 i, s32 slot);
DECLARE_FUNC(s32,  arena_export_bomb_get_slot, s32 i);
DECLARE_FUNC(s32,  arena_export_is_actor_slot, s32 slot);
```
(Remove the now-unused `arena_export_dbg_u32` `DECLARE_FUNC` if present — it is not called in the shipped patch.)

- [ ] **Step 2: Sweep excludes all actor slots (via the native check)**

Replace the boss-suppression sweep body (the `s1/s2/s3` + `for k` block at the top of `arena_render_routine`) with the actor-slot check:
```c
    if (arena_bridge_is_battle() && gPlayerObject != NULL) {
        s32 k;
        for (k = 14; k < 78; k++) {
            if (!arena_export_is_actor_slot(k))
                gObjects[k].actionState = ACTION_NONE;
        }
    }
```

- [ ] **Step 3: Wire set/kick into the tick**

In the battle block, replace the `arena_export_tick_input(sx, sy, jump, bomb);` call with a `set` read + 5-arg call:
```c
        s32 jump = (gActiveContButton & CONT_A) ? 1 : 0;
        s32 bomb = (gActiveContButton & CONT_B) ? 1 : 0;
        s32 set  = (gActiveContButton & CONT_G) ? 1 : 0;   /* Z trigger / Q key */
        arena_export_tick_input(sx, sy, jump, bomb, set);
```

- [ ] **Step 4: Spawn the 16 bomb actors (in the spawn-once block, after the 3 players)**

Inside `if (!arena_export_puppet_ready()) { ... }`, after the player `for (i=1;i<4;i++)` spawn loop, add the bomb pool spawn:
```c
            {
                s32 bi;
                for (bi = 0; bi < 16; bi++) {
                    struct ObjSpawnInfo binfo;
                    binfo.unk0 = 0; binfo.unk2 = OBJ_TOBIRA1_O; binfo.unk4 = 9;
                    binfo.unk6 = 0; binfo.unk7 = 0; binfo.unk8 = 0; binfo.unk9 = 0; binfo.unkA = 0;
                    {
                        s32 slot = func_80027464(1, &binfo,
                                                 gPlayerObject->Pos.x,
                                                 gPlayerObject->Pos.y,
                                                 gPlayerObject->Pos.z, 0.0f);
                        if (slot >= 0) {
                            func_8001ABF4(slot, 0, 0, D_801163DC_ADDR);
                            gObjects[slot].actionState = ACTION_NONE;   /* start hidden */
                        }
                        arena_export_bomb_set_slot(bi, slot);
                    }
                }
            }
```

- [ ] **Step 5: Toggle + position bombs each frame (after the player-positioning loop)**

At the end of the battle block (after the player `for (i=1;i<4;i++)` positioning loop), add:
```c
        {
            s32 bi;
            for (bi = 0; bi < 16; bi++) {
                s32 slot = arena_export_bomb_get_slot(bi);
                if (slot >= 0) {
                    if (arena_export_bomb_active(bi)) {
                        gObjects[slot].Pos.x       = arena_export_bomb_wx(bi);
                        gObjects[slot].Pos.y       = arena_export_bomb_wy(bi);
                        gObjects[slot].Pos.z       = arena_export_bomb_wz(bi);
                        gObjects[slot].actionState = ACTION_IDLE;   /* visible */
                    } else {
                        gObjects[slot].actionState = ACTION_NONE;   /* hidden */
                    }
                }
            }
        }
```

- [ ] **Step 6: Build**

Run **[build]** (the `make -C patches clean` runs first). Expected: `patches.elf` links, N64Recomp resolves all `arena_export_*` imports, exe links.

- [ ] **Step 7: Human boot gate**

Ask the user to **[launch]** and enter the arena, then: **hold+release B (throw)** and **tap Q (set)**, and report — I (agent) then capture a screenshot + read `arena_bridge.log`:
1. Does a bomb appear on set/throw, sit/arc at a sensible spot, and **vanish on detonation**?
2. Do the 4 player actors + boss suppression still work (no regression)?
3. Any crash, or missing/garbled bombs?
**Watch items (surface, don't thrash):** if the game crashes at spawn, the 16-bomb pool may exhaust the model/anim pool → reduce the pool (e.g. 8) and note it. If bombs float/sink or the arc reads wrong, that's `g_scale` vertical feel — note the direction, it's a one-constant tweak, not a blocker. If bombs never appear, check the log shows the sim making bombs (throw wired) and that `arena_export_bomb_active` returns 1.

- [ ] **Step 8: Commit + push**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "feat(arena): A1.2c bomb rendering - 16-actor pool + set/kick input"
git push
```

---

### Task 3: Docs

**Files (bmhero-arena):** `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md status**

Add an A1.2c entry to the current-status section: bombs render (16-actor pool toggled from `bombs[].state`, positioned incl. arc via `g_ref_sy`), set/kick wired to `CONT_G`; the sweep now spares all actor slots via `arena_is_actor_slot`. Note blasts are the next slice and the sim bomb-cap (6→16) is deferred to A1.3. Update the A1.2 milestone line: A1.2c bombs done → A1.2c blasts next.

- [ ] **Step 2: Commit + push (bmhero-arena)**

```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md
git commit -m "docs: A1.2c bomb rendering done (bombs on screen, set/kick wired)"
git push
```

---

## Plan self-review notes

- **Spec coverage:** §2 position-driven incl. Y → Task 1 Step 3 `arena_bomb_wy` (with `g_ref_sy` from Step 2) + Task 2 Step 5; §3 pooled 16 toggle → Task 2 Steps 4-5; §4 native API → Task 1 (all int-arg/float-return, no float args); §5 sweep exclusion → Task 1 `arena_is_actor_slot` + Task 2 Step 2; §6 set/kick `CONT_G` → Task 1 (`set` arg) + Task 2 Step 3; §7 consistency → informational (bomb graphics = real asset, throw physics already matched); §8 non-goals → not implemented (blasts, state visuals, sim cap untouched); §9 build/testing → `[build]`/`[launch]` + human boot gate.
- **Placeholder scan:** no TBDs. Pool 16, `CONT_G`=0x2000, addresses `0x8F000178+` all concrete. The vertical-scale feel note is a labelled boot-gate observation, not a gap.
- **Type/name consistency:** three name layers distinct — native (`arena_bomb_*`, `arena_is_actor_slot`, `arena_bridge_tick_input`), exports (`arena_export_bomb_*`, `arena_export_is_actor_slot`, `arena_export_tick_input` = syms.ld = REGISTER_FUNC = patch DECLARE_FUNC), no collisions. `arena_bridge_tick_input` gains its 5th arg consistently across .h/.cpp/shim/patch. Bomb-slot table sized `ARENA_MAX_BOMBS` (16) everywhere. **Task 1↔2 coupling flagged:** the tick shim goes 5-arg in Task 1 but the patch call updates in Task 2 — Step 8 of Task 1 says don't boot between tasks (the interim build is ABI-mismatched by design).
