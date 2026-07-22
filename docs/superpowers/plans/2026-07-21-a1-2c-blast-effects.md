# A1.2c Blast Effects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detonations show an explosion at each blast site, via the game's own effect spawner (`func_80081468`) fired on per-blast detonation edges — effect ID identified by a one-boot spike of all 18 candidates.

**Architecture:** Native `arena_bridge` gains a one-shot spike latch, per-blast liveness-edge detection (`arena_blast_new`), and blast world-coord getters (frozen-frame mapping, same as bombs). The patch (a) spike: on the first Q press, spawns a row of effects with IDs `0x2BC..0x2CD` so a human identifies the explosion; (b) final: fires `func_80081468(EXPLOSION_ID, ...)` once per new blast. Spec: `docs/superpowers/specs/2026-07-21-a1-2c-blast-effects-design.md`.

**Tech Stack:** VS 2022 clang-cl (native), LLVM-15 MIPS (patches), the A1.0 three-toolchain recipe (memory `recomp-build-toolchain`).

## Global Constraints

- Fork `C:\Users\dshi\GitRepos\BMHeroRecomp`, branch `feature/a1.2b-spawn-bombers` (continue). `bmhero-arena` gets only doc updates; **sim NOT modified** (pinned hash `4b6687d4`).
- **Patches STATELESS** (mutable patch statics abort `0xC0000409`); no `memset`/`memcpy` in the patch; **no float args across the native-export ABI** (int args / float returns only). Calling a *game* function (`func_80081468`) with float args from the patch is fine (MIPS-to-MIPS).
- Verified facts:
  - `ArenaState.blasts[16]` (`ARENA_MAX_BLASTS`): `Vec3q center`, `u16 radius_t`, `u8 owner`, `u8 ttl` (`ttl==0` = slot free; alive 20 ticks). `arena_state.h`.
  - `void func_80081468(s32 id, f32 x, f32 y, f32 z)` — game effect spawner (`functions.h:202`; `GLOBAL_ASM` body, but function symbols resolve in patches). Known IDs `0x2BC..0x2CD` (18 candidates).
  - Frozen-frame mapping statics in `arena_bridge.cpp`: `g_origin_x/y/z`, `g_ref_sx/sy/sz`, `g_scale`/`g_scale_z`, `qf`. Existing exports end at `arena_export_is_actor_slot = 0x8F000190`.
  - Patch reads `gActiveContButton` (`CONT_G` = 0x2000 = Z/Q) and already has the battle block ordering: sweep → `func_80024744()` → tick → puppets → bombs.
- **[build]** (MANDATORY `make -C patches clean` after any `patches/*.c` edit):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; make.exe -C patches clean; cmake --build build-cmake --target BMHeroRecompiled
  ```
- **[launch]** = human runs `play.bat`, arrow→Battle→Enter, Start + A×3 into the arena; agent verifies via scratchpad `capture-game.ps1` + `arena_bridge.log`.

---

### Task 1: Native — spike latch + blast edge/coord exports

**Files (fork):**
- Modify: `src/arena_bridge/arena_bridge.cpp`, `arena_bridge.h`, `arena_bridge_export.cpp`, `src/main/main.cpp`, `patches/syms.ld`

**Interfaces:**
- Produces (EXPORT names): `arena_export_spike_once()->s32` (1 on first call only), `arena_export_blast_new(i)->s32` (1 exactly once per blast birth; MUST be called every frame for every index), `arena_export_blast_wx/wy/wz(i)->f32`.

- [ ] **Step 1: State + native fns in `arena_bridge.cpp`**

In the anonymous namespace (next to `g_bomb_slot`):
```cpp
    bool  g_blast_prev[ARENA_MAX_BLASTS] = {};   /* liveness last frame (edge detect) */
    bool  g_spike_done = false;                  /* one-shot latch for the ID spike */
```
At the end of the file (after `arena_is_actor_slot`):
```cpp
/* A1.2c slice 2: blasts. Edge-detect per index (the patch calls blast_new for
 * ALL 16 indices every frame, so prev-tracking inside the getter is sound). */
extern "C" int arena_spike_once(void) {
    if (g_spike_done) return 0;
    g_spike_done = true;
    return 1;
}
extern "C" int arena_blast_new(int i) {
    if (i < 0 || i >= ARENA_MAX_BLASTS) return 0;
    bool alive = g_state.blasts[i].ttl != 0;
    bool was   = g_blast_prev[i];
    g_blast_prev[i] = alive;
    return (alive && !was) ? 1 : 0;
}
extern "C" float arena_blast_wx(int i) {
    if (i < 0 || i >= ARENA_MAX_BLASTS) return g_origin_x;
    return g_origin_x + (qf(g_state.blasts[i].center.x) - g_ref_sx) * g_scale;
}
extern "C" float arena_blast_wy(int i) {
    if (i < 0 || i >= ARENA_MAX_BLASTS) return g_origin_y;
    return g_origin_y + (qf(g_state.blasts[i].center.y) - g_ref_sy) * g_scale;
}
extern "C" float arena_blast_wz(int i) {
    if (i < 0 || i >= ARENA_MAX_BLASTS) return g_origin_z;
    return g_origin_z + (qf(g_state.blasts[i].center.z) - g_ref_sz) * g_scale_z;
}
```

- [ ] **Step 2: Declarations in `arena_bridge.h`** (inside the `extern "C"` block)

```c
int   arena_spike_once(void);      /* 1 on first call only (ID-spike latch) */
int   arena_blast_new(int i);      /* 1 once per blast birth; call all i every frame */
float arena_blast_wx(int i);
float arena_blast_wy(int i);
float arena_blast_wz(int i);
```

- [ ] **Step 3: Shims in `arena_bridge_export.cpp`** (append)

```cpp
/* A1.2c slice 2: blast exports. */
extern "C" void arena_export_spike_once(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; _return(ctx, arena_spike_once());
}
extern "C" void arena_export_blast_new(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_blast_new(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_blast_wx(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_blast_wx(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_blast_wy(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_blast_wy(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_blast_wz(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_blast_wz(_arg<0, int>(rdram, ctx)));
}
```

- [ ] **Step 4: Register in `main.cpp`** — fwd-decls next to the other `arena_export_*`:
```cpp
extern "C" void arena_export_spike_once(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_blast_new(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_blast_wx(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_blast_wy(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_blast_wz(uint8_t* rdram, recomp_context* ctx);
```
and in the `REGISTER_FUNC` block (after `arena_export_is_actor_slot`):
```cpp
    REGISTER_FUNC(arena_export_spike_once);
    REGISTER_FUNC(arena_export_blast_new);
    REGISTER_FUNC(arena_export_blast_wx);
    REGISTER_FUNC(arena_export_blast_wy);
    REGISTER_FUNC(arena_export_blast_wz);
```

- [ ] **Step 5: `syms.ld`** — after `arena_export_is_actor_slot = 0x8F000190;`:
```
arena_export_spike_once = 0x8F000194;
arena_export_blast_new = 0x8F000198;
arena_export_blast_wx = 0x8F00019C;
arena_export_blast_wy = 0x8F0001A0;
arena_export_blast_wz = 0x8F0001A4;
```

- [ ] **Step 6: Build** — run **[build]**; expect clean link (new syms unreferenced by the patch yet — harmless).

- [ ] **Step 7: Commit**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add src/arena_bridge/ src/main/main.cpp patches/syms.ld
git commit -m "feat(arena): native blast edge/coords + spike latch (A1.2c slice 2)"
```

---

### Task 2: Effect-ID spike — one boot, 18 candidates on screen

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: Task 1 exports; `func_80081468`, `gActiveContButton`/`CONT_G`, `gPlayerObject`.
- Produces: the explosion effect ID (human-identified) + proof effects render (sweep-safe).

- [ ] **Step 1: Add imports + the spike block**

After the bomb `DECLARE_FUNC`s:
```c
/* A1.2c slice 2: blast exports + the game effect spawner */
DECLARE_FUNC(s32,  arena_export_spike_once);
DECLARE_FUNC(s32,  arena_export_blast_new, s32 i);
DECLARE_FUNC(f32,  arena_export_blast_wx, s32 i);
DECLARE_FUNC(f32,  arena_export_blast_wy, s32 i);
DECLARE_FUNC(f32,  arena_export_blast_wz, s32 i);
extern void func_80081468(s32 id, f32 x, f32 y, f32 z);   /* spawn effect by ID at pos */
```
At the END of the battle block (after the bomb toggle loop), add the spike —
triggered by the FIRST Q press so the human controls timing and is watching:
```c
        /* EFFECT-ID SPIKE (temporary): first Q press spawns one effect of each
         * candidate ID 0x2BC..0x2CD in two rows of 9 around the player.
         * Layout key: col = idx%9 (x: -400..+400 step 100), row = idx/9
         * (z: +150 near row = IDs 2BC..2C4, +300 far row = 2C5..2CD). */
        if ((gActiveContButton & CONT_G) && arena_export_spike_once()) {
            s32 idx;
            for (idx = 0; idx < 18; idx++) {
                f32 ex = gPlayerObject->Pos.x + ((idx % 9) * 100.0f - 400.0f);
                f32 ez = gPlayerObject->Pos.z + 150.0f + ((idx / 9) * 150.0f);
                func_80081468(0x2BC + idx, ex, gPlayerObject->Pos.y, ez);
            }
        }
```

- [ ] **Step 2: Build** — run **[build]**; expect `func_80081468` + new exports to resolve and the exe to link.

- [ ] **Step 3: Human boot gate (the spike)**

**[launch]** → in the arena, stand still, press **Q once**, and describe what appears (agent captures immediately). Wanted: which effect(s) look like an explosion/fireball, identified by position — near row = IDs `0x2BC..0x2C4` left→right, far row = `0x2C5..0x2CD` left→right. Also confirms effects render at all (sweep-safe).
**If nothing appears:** rebuild once with the boss sweep commented out and repeat — if effects then appear, the sweep kills them (STOP: re-plan the sweep per spec §3.1 contingency). If they still don't appear, STOP and re-plan against spec §3.3 (pooled fallback).
**If it crashes:** note whether before/after the Q press (native latch means the row fires once; a crash on press = an ID in the range is unsafe to spawn — bisect the range in halves, same trigger).

- [ ] **Step 4: Commit the spike** (kept in history; removed next task)
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "spike(A1.2c): effect-ID row 0x2BC-0x2CD on first Q press"
```

---

### Task 3: Wire detonations → explosion effect

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: Task 1 exports; the `EXPLOSION_ID` pinned by Task 2.

- [ ] **Step 1: Replace the spike with the per-blast edge loop**

Delete the Task 2 spike block. Near the top defines, add (fill the spiked ID):
```c
/* Explosion effect ID, pinned by the 2026-07-21 effect-ID spike (see spec). */
#define ARENA_EXPLOSION_ID 0x2BC   /* <- replace with the spike's winner */
```
Where the spike block was (end of the battle block), add:
```c
        /* Fire the explosion effect once per new blast (edge from native).
         * blast_new must be called for ALL indices every frame (it updates the
         * native prev-liveness tracking), so no early-out. */
        {
            s32 bi;
            for (bi = 0; bi < 16; bi++) {
                if (arena_export_blast_new(bi)) {
                    func_80081468(ARENA_EXPLOSION_ID,
                                  arena_export_blast_wx(bi),
                                  arena_export_blast_wy(bi),
                                  arena_export_blast_wz(bi));
                }
            }
        }
```

- [ ] **Step 2: Build** — run **[build]**.

- [ ] **Step 3: Human boot gate (the payoff)**

**[launch]** → set (Q) and throw (hold+release LShift) bombs; wait for detonations. Report:
1. Does an explosion appear where each bomb detonates (spread → up to 4 explosions)?
2. Players/bombs unaffected; no crash over a few minutes of play?
3. Effect height OK? (If it appears buried/floating, add a `+ N` Y offset constant next to `ARENA_EXPLOSION_ID` — the splash call sites used +60/+120 — and rebuild once.)
Agent captures + reads `arena_bridge.log` for stability.

- [ ] **Step 4: Commit + push**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "feat(A1.2c): detonations fire the game explosion effect (slice 2)"
git push
```

---

### Task 4: Docs

**Files (bmhero-arena):** `CLAUDE.md`

- [ ] **Step 1:** Update the current-status section: A1.2c complete (slice 2 — detonations show the game's explosion effect via `func_80081468`, ID pinned by the spike; per-blast edges from native). Update the milestone line (A1.2c done → next: real bomber mesh / anim / camera-relative input / HUD). Record the effect-spawner mechanism + pinned ID in integration notes §8 (one short subsection).

- [ ] **Step 2:**
```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md docs/bmhero-recomp-integration-notes.md
git commit -m "docs: A1.2c complete — blasts render via game effect spawner"
git push
```

---

## Plan self-review notes

- **Spec coverage:** §3.1 spike → Task 2 (Q-press trigger = human-controlled timing; two-row layout with a position→ID key; no-render and crash contingencies with STOP points); §3.2 wiring → Task 1 (edge detection inside the getter, prev updated per call — sound because Task 3's loop calls all 16 every frame with no early-out, explicitly commented) + Task 3; §3.3 fallback → deliberate STOP-and-re-plan (its mini-spikes are conditional work, not pre-planned); §4 non-goals untouched; §5 build/testing → [build]/[launch] + gates.
- **Placeholder scan:** `ARENA_EXPLOSION_ID 0x2BC` is an explicit fill-in-from-spike marker with instructions — the plan's one intentional unknown. No other TBDs.
- **Type/name consistency:** native `arena_spike_once`/`arena_blast_*`; exports `arena_export_spike_once`/`arena_export_blast_*` consistent across shim/main/syms/DECLARE_FUNC; addresses continue at `0x8F000194+`; `func_80081468` signature matches `functions.h:202`. Blast loop uses `bi` (no clash with the bomb loop's `bi` — separate scopes).
