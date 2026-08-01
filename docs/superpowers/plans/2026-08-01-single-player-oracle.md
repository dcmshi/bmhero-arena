# Single-Player Oracle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Probe vanilla single-player Bomberman Hero as a reference oracle, extract
checked-in goldens (real drop/throw anim, bomb rest lift, impact-detonation
timing), and gate every arena build against them — so human feel-boots become
sign-offs, not the debugging loop.

**Architecture:** A new env knob `ARENA_ORACLE=1` auto-boots the game WITHOUT
battle mode (vanilla campaign), mashes through the frontend, runs scripted
input phases (drop a bomb, throw a bomb), and logs per-frame probes from the
existing per-frame patch hook (`arena_render_routine`, which already runs in
every level — battle work is gated on `arena_bridge_is_battle()`, so vanilla
passes through untouched). `tools\oracle.ps1` distills the log into
`tools\oracle\goldens.json`; `tools\oracle-gate.ps1` asserts arena soaks match;
`build.ps1 -Soak` runs the gate automatically when goldens exist.

**Tech Stack:** N64Recomp MIPS patches (C, stateless), native bridge (C++),
recomp export ABI (4 × 32-bit args, floats as bit patterns), PowerShell tooling.

**Spec:** `docs/superpowers/specs/2026-08-01-single-player-oracle-design.md`
(canonical repo). All implementation is in the FORK
(`C:\Users\dshi\GitRepos\BMHeroRecomp`, branch **master**). Do not push either repo.

## Global Constraints

- **Patches are stateless.** No `static` storage in `patches/*.c` — patch statics
  crash (0xC0000409). All counters/gating live native-side; the patch calls
  exports unconditionally (the `arena_export_dbg_cam` idiom).
- **Export ABI:** max 4 args, all 32-bit; floats cross as BIT PATTERNS via
  `fbits()` (patch-side helper at `patches/arena_render.c:209`). f32 *returns*
  are fine. Every export needs all four: `arena_bridge.h` decl +
  `arena_bridge_export.cpp` shim + `main.cpp` extern + `REGISTER_FUNC` +
  `patches/syms.ld` address. An export called without its patch-side
  `DECLARE_FUNC` compiles with only a warning and reads the wrong register —
  treat implicit-declaration warnings as errors.
- **Next free syms.ld address: `0x8F000250`** (last used: `0x8F00024C`).
- **No `recomp_printf` in the load window.** Oracle logging goes through native
  exports only.
- **Sim untouched.** No `lib/bmhero-arena` changes; pinned hash stays `cce00b99`,
  `TUNE_VERSION` stays 15.
- **Build:** `.\build.ps1 -Config rwdi` from the fork root (composes PATH; a
  failed build leaves the previous exe — confirm `BUILD OK` before trusting any
  probe). Known post-make-clean lld regen race is auto-retried.
- **Green soak before any human handoff:** `.\build.ps1 -Config rwdi -Soak 5`.
- **Precedence:** if both `ARENA_ORACLE` and `ARENA_AUTO_BATTLE` are set, oracle
  wins (boots vanilla).
- Log-line formats below are LOAD-BEARING — `oracle.ps1`/`oracle-gate.ps1` parse
  them. New fields go LAST on a line (the `[blastvis]` precedent).

---

### Task 1: Native oracle channel + export plumbing

**Files:**
- Modify: `src/arena_bridge/arena_bridge.h` (append before `#ifdef __cplusplus }` close)
- Modify: `src/arena_bridge/arena_bridge.cpp` (append near `arena_dbg_blast`, ~line 585)
- Modify: `src/arena_bridge/arena_bridge_export.cpp` (append at end)
- Modify: `src/main/main.cpp` (extern block ~line 121; `REGISTER_FUNC` block ~line 1068)
- Modify: `patches/syms.ld` (after line 129, `arena_export_cam_yaw_eff_cos = 0x8F00024C;`)

**Interfaces:**
- Consumes: `ensure_init()`, `g_log` (both `arena_bridge.cpp` internals),
  `_arg<N, int>` / `_return` shim helpers (`arena_bridge_export.cpp` existing pattern).
- Produces (native, for main.cpp): `int arena_oracle_mode(void)`,
  `int arena_oracle_seen(void)`, `void arena_oracle_phase(const char* name)`.
- Produces (exports, for the patch): `arena_export_oracle_mode() -> s32`,
  `arena_export_oracle_frame(s32 level, s32 playerValid, s32 floorYbits, s32 playerYbits)`,
  `arena_export_oracle_anim(s32 idx, s32 framebits, s32 state)`,
  `arena_export_oracle_obj(s32 slot_state, s32 xbits, s32 ybits, s32 zbits)`.

- [ ] **Step 1: Bridge header decls** — append to `arena_bridge.h` before the closing `#ifdef __cplusplus`:

```c
/* Single-player oracle (spec 2026-08-01): ARENA_ORACLE=1 boots VANILLA
 * campaign with per-frame probes; goldens are extracted from the log.
 * Native owns all gating/throttling and the shared frame counter n; the
 * patch calls unconditionally and stays stateless. Floats cross as BITS. */
int   arena_oracle_mode(void);    /* 1 iff ARENA_ORACLE=1 (cached) */
int   arena_oracle_seen(void);    /* 1 once the in-level routine has run (mash-stop) */
void  arena_oracle_phase(const char* name);  /* main.cpp phase markers -> log */
void  arena_oracle_frame(int level, int playerValid, int floorYbits, int playerYbits);
void  arena_oracle_anim(int idx, int framebits, int state);
void  arena_oracle_obj(int slot_state, int xbits, int ybits, int zbits);
```

- [ ] **Step 2: Bridge implementation** — append to `arena_bridge.cpp` after `arena_dbg_blast`:

```cpp
/* ---- Single-player oracle (spec 2026-08-01) ---------------------------- */
/* One monotonic counter n stamps every oracle line: it ticks once per game
 * frame (arena_oracle_frame runs FIRST in the patch's oracle block), so all
 * timing goldens live in one clock regardless of which channel logged them. */
static bool     g_oracle_seen = false;
static unsigned g_oracle_n    = 0;

extern "C" int arena_oracle_mode(void) {
    static const bool on = []() {
        const char* v = std::getenv("ARENA_ORACLE");
        return v && v[0] == '1'; }();
    return on ? 1 : 0;
}
extern "C" int arena_oracle_seen(void) { return g_oracle_seen ? 1 : 0; }

extern "C" void arena_oracle_phase(const char* name) {
    if (!arena_oracle_mode()) return;
    ensure_init();
    if (g_log) { std::fprintf(g_log, "[oracle] phase=%s n=%u\n", name, g_oracle_n);
                 std::fflush(g_log); }
}

/* Heartbeat: in-level signal (mash-stop) + floor/player Y from the game's own
 * ground query. Logged 1-in-30 (readable log); n ticks EVERY call. */
extern "C" void arena_oracle_frame(int level, int playerValid, int floorYbits, int playerYbits) {
    if (!arena_oracle_mode()) return;
    ensure_init();
    g_oracle_seen = true;
    g_oracle_n++;
    union { int i; float f; } fy, py; fy.i = floorYbits; py.i = playerYbits;
    if (g_log && (g_oracle_n % 30u) == 1u) {
        std::fprintf(g_log, "[oracle] frame n=%u level=%d player=%d floorY=%.2f playerY=%.2f\n",
                     g_oracle_n, level, playerValid, fy.f, py.f);
        std::fflush(g_log);
    }
}

/* Player anim, every frame while in-level (the parser needs the frame ramp to
 * measure clip length). frame is f32 bits (func_8001B62C returns f32). */
extern "C" void arena_oracle_anim(int idx, int framebits, int state) {
    if (!arena_oracle_mode()) return;
    union { int i; float f; } fr; fr.i = framebits;
    if (g_log) {
        std::fprintf(g_log, "[oracle-anim] n=%u idx=%d frame=%.1f state=%d\n",
                     g_oracle_n, idx, fr.f, state);
        std::fflush(g_log);
    }
}

/* Game object watch. slot_state = (slot << 16) | (actionState & 0xFFFF).
 * Slots 2..5 = the game's bomb pool -> [oracle-bomb]; 6..13 = the explosion
 * pool (func_8007E76C spawns there) -> [oracle-blast]. The patch only reports
 * ACTIVE objects, so lines are sparse. */
extern "C" void arena_oracle_obj(int slot_state, int xbits, int ybits, int zbits) {
    if (!arena_oracle_mode()) return;
    int slot = (slot_state >> 16) & 0xFFFF, st = slot_state & 0xFFFF;
    union { int i; float f; } x, y, z; x.i = xbits; y.i = ybits; z.i = zbits;
    if (g_log) {
        std::fprintf(g_log, "[oracle-%s] n=%u slot=%d state=%d pos=(%.1f,%.1f,%.1f)\n",
                     slot >= 6 ? "blast" : "bomb", g_oracle_n, slot, st, x.f, y.f, z.f);
        std::fflush(g_log);
    }
}
```

- [ ] **Step 3: Export shims** — append to `arena_bridge_export.cpp` (same `_arg`/`_return` pattern as `arena_export_dbg_blast` at line 188):

```cpp
/* Single-player oracle (spec 2026-08-01). Floats cross as BIT PATTERNS. */
extern "C" void arena_export_oracle_mode(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; _return(ctx, arena_oracle_mode());
}
extern "C" void arena_export_oracle_frame(uint8_t* rdram, recomp_context* ctx) {
    arena_oracle_frame(_arg<0, int>(rdram, ctx), _arg<1, int>(rdram, ctx),
                       _arg<2, int>(rdram, ctx), _arg<3, int>(rdram, ctx));
}
extern "C" void arena_export_oracle_anim(uint8_t* rdram, recomp_context* ctx) {
    arena_oracle_anim(_arg<0, int>(rdram, ctx), _arg<1, int>(rdram, ctx),
                      _arg<2, int>(rdram, ctx));
}
extern "C" void arena_export_oracle_obj(uint8_t* rdram, recomp_context* ctx) {
    arena_oracle_obj(_arg<0, int>(rdram, ctx), _arg<1, int>(rdram, ctx),
                     _arg<2, int>(rdram, ctx), _arg<3, int>(rdram, ctx));
}
```

- [ ] **Step 4: main.cpp registration** — add to the extern block (after line 121, `arena_export_dbg_blast`):

```cpp
extern "C" void arena_export_oracle_mode(uint8_t* rdram, recomp_context* ctx);   // single-player oracle
extern "C" void arena_export_oracle_frame(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_oracle_anim(uint8_t* rdram, recomp_context* ctx);
extern "C" void arena_export_oracle_obj(uint8_t* rdram, recomp_context* ctx);
```

and to the `REGISTER_FUNC` block (after line 1068, `REGISTER_FUNC(arena_export_dbg_blast);`):

```cpp
    REGISTER_FUNC(arena_export_oracle_mode);
    REGISTER_FUNC(arena_export_oracle_frame);
    REGISTER_FUNC(arena_export_oracle_anim);
    REGISTER_FUNC(arena_export_oracle_obj);
```

- [ ] **Step 5: syms.ld addresses** — append after `arena_export_cam_yaw_eff_cos = 0x8F00024C;`:

```
arena_export_oracle_mode  = 0x8F000250;
arena_export_oracle_frame = 0x8F000254;
arena_export_oracle_anim  = 0x8F000258;
arena_export_oracle_obj   = 0x8F00025C;
```

(New next-free is `0x8F000260` — update any "next free" comment if one exists in the file.)

- [ ] **Step 6: Build**

Run: `.\build.ps1 -Config rwdi` (fork root)
Expected: `BUILD OK`. Zero implicit-declaration warnings mentioning `oracle`.

- [ ] **Step 7: Commit**

```powershell
git add src/arena_bridge/arena_bridge.h src/arena_bridge/arena_bridge.cpp src/arena_bridge/arena_bridge_export.cpp src/main/main.cpp patches/syms.ld
git commit -m "feat(oracle): native oracle channel + export plumbing (ARENA_ORACLE)"
```

---

### Task 2: Patch-side oracle probe block

**Files:**
- Modify: `patches/arena_render.c` — DECLARE_FUNC block (~line 108, near `arena_export_dbg_anim`); oracle block at the TOP of `arena_render_routine` (line ~290, before the `gDebugInvincibileFlag` write).

**Interfaces:**
- Consumes: the four `arena_export_oracle_*` exports (Task 1);
  `arena_bridge_is_battle()` (existing); `func_80078168(f32,f32,f32)` ground
  query + `GQ_SEL`/`GQ_H` macros (lines 133-140); `func_8001B880(objId,part)`
  live anim idx + `func_8001B62C(objId,part)` live anim frame (lines 105-106);
  `gPlayerObject->Pos`, `gObjects[k].Pos`, `gObjects[k].actionState`,
  `ACTION_NONE`, `gCurrentLevel`, `fbits()` (all already used in this file).
- Produces: `[oracle]` / `[oracle-anim]` / `[oracle-bomb]` / `[oracle-blast]`
  lines in `arena_bridge.log` whenever `ARENA_ORACLE=1` and a level is running.

- [ ] **Step 1: DECLARE_FUNCs** — add after line 108 (`DECLARE_FUNC(void, arena_export_dbg_anim, ...)`):

```c
/* ---- Single-player oracle (ARENA_ORACLE=1; spec 2026-08-01) ------------ */
/* Native owns all gating/throttling and the shared line counter; the patch
 * calls these unconditionally inside the oracle branch and stays stateless. */
DECLARE_FUNC(s32,  arena_export_oracle_mode);
DECLARE_FUNC(void, arena_export_oracle_frame, s32 level, s32 playerValid, s32 floorYbits, s32 playerYbits);
DECLARE_FUNC(void, arena_export_oracle_anim, s32 idx, s32 framebits, s32 state);
DECLARE_FUNC(void, arena_export_oracle_obj, s32 slot_state, s32 xbits, s32 ybits, s32 zbits);
```

- [ ] **Step 2: Oracle block** — insert at the top of `arena_render_routine`, immediately after the `u16 held_buttons;` declaration and BEFORE the `gDebugInvincibileFlag` write:

```c
    /* Single-player ORACLE (ARENA_ORACLE=1): per-frame probes of the VANILLA
     * campaign, the reference the arena is checked against (goldens). READ
     * ONLY - the game is not modified. Battle never sets the knob; with it
     * unset this whole block costs one cached-bool export call per frame.
     * oracle_frame is called FIRST: it ticks the shared line counter n. */
    if (!arena_bridge_is_battle() && arena_export_oracle_mode()) {
        s32 k;
        s32 pvalid = (gPlayerObject != NULL) ? 1 : 0;
        f32 fy = -30000.0f;   /* the game's own "no floor" sentinel (69AA0.c:401) */
        f32 py = 0.0f;
        if (pvalid) {
            py = gPlayerObject->Pos.y;
            /* The game's ground query (69AA0.c:205) - pure position in,
             * globals out; every object's own ground handling calls it the
             * same way, so an extra refresh is safe (raster precedent). */
            func_80078168(gPlayerObject->Pos.x, gPlayerObject->Pos.y, gPlayerObject->Pos.z);
            { s32 sel = GQ_SEL; fy = GQ_H[sel]; }
        }
        arena_export_oracle_frame((s32)gCurrentLevel, pvalid, fbits(fy), fbits(py));
        if (pvalid) {
            /* live player anim: the same reads the [animw] channel uses */
            arena_export_oracle_anim(func_8001B880(0, 0),
                                     fbits(func_8001B62C(0, 0)),
                                     (s32)gPlayerObject->actionState);
        }
        /* game bomb pool [2..5] and explosion pool [6..13]: active only */
        for (k = 2; k < 14; k++) {
            if (gObjects[k].actionState != ACTION_NONE)
                arena_export_oracle_obj((k << 16) | ((s32)gObjects[k].actionState & 0xFFFF),
                                        fbits(gObjects[k].Pos.x),
                                        fbits(gObjects[k].Pos.y),
                                        fbits(gObjects[k].Pos.z));
        }
    }
```

- [ ] **Step 3: Build**

Run: `.\build.ps1 -Config rwdi`
Expected: `BUILD OK`, no implicit-declaration warnings.

- [ ] **Step 4: No-regression soak (knob unset)**

Run: `.\tools\arena-soak.ps1 -N 3`
Expected: 3/3 PASS — battle boots are untouched (the block is behind
`!arena_bridge_is_battle() && oracle_mode()`; both false in the soak).

- [ ] **Step 5: Commit**

```powershell
git add patches/arena_render.c
git commit -m "feat(oracle): per-frame vanilla probes - anim funnel, bomb/blast pools, ground query"
```

---

### Task 3: Oracle boot path — launcher, mash, input phases

**Files:**
- Modify: `src/main/main.cpp` — `soak_launcher_update` (~line 827) and
  `soak_get_n64_input` (~line 649).
- Modify: `tools/play.ps1` — add `"ARENA_ORACLE"` to the `$knobs` array (line 31-34).

**Interfaces:**
- Consumes: `arena_oracle_mode()`, `arena_oracle_seen()`, `arena_oracle_phase()`
  (Task 1, via `arena_bridge.h`, already included by main.cpp);
  `recomp::start_game`, `recompui::hide_all_contexts`, `supported_games` (existing).
- Produces: `[oracle] phase=<name> n=<n>` markers with names
  `in-level`, `walk`, `stand`, `dropB`, `holdB`, `releaseB`, `DONE`
  (exact strings — `oracle.ps1` keys on them).

- [ ] **Step 1: Launcher auto-start** — replace the body of `soak_launcher_update` with:

```cpp
static void soak_launcher_update(recompui::LauncherMenu *menu) {
    banjo::launcher_animation_update(menu);
    static const char* soak   = std::getenv("ARENA_AUTO_BATTLE");
    static const char* oracle = std::getenv("ARENA_ORACLE");
    static int frames = 0;
    static bool fired = false;
    bool soak_on   = soak && (soak[0] == '1' || soak[0] == '2' || soak[0] == '3' ||
                              soak[0] == '4' || soak[0] == '5' || soak[0] == '6' ||
                              soak[0] == '7' || soak[0] == '8' || soak[0] == '9');
    bool oracle_on = oracle && oracle[0] == '1';
    if ((soak_on || oracle_on) && !fired && ++frames >= 60) {
        std::u8string gid = supported_games[0].game_id;
        if (recomp::is_rom_valid(gid)) {
            fired = true;
            /* ORACLE boots VANILLA: no battle mode. Oracle wins if both set. */
            if (!oracle_on) arena_bridge_set_battle_mode(1);
            recomp::start_game(gid, {});
            recompui::hide_all_contexts();
        }
    }
}
```

- [ ] **Step 2: Mash + phases** — in `soak_get_n64_input`, insert a self-contained
oracle branch immediately after `bool ok = ...get_n64_input(...)` and BEFORE the
existing `soak_active` block (early-return so the battle probe code never also runs):

```cpp
    /* Single-player ORACLE (spec 2026-08-01): mash to a level, then scripted
     * bomb verbs. Poll counter ~= frame counter; phase markers land in the
     * log with the shared n stamp, which is the goldens' clock. */
    static const bool oracle_active = []() {
        const char* v = std::getenv("ARENA_ORACLE");
        return v && v[0] == '1'; }();
    if (oracle_active && ok && controller_num == 0) {
        if (!arena_oracle_seen()) {
            /* same synthetic mash as battle: 4 polls on / 4 off, START|A */
            static uint32_t omash = 0;
            omash++;
            if ((omash >> 2) & 1)
                *buttons |= ((omash >> 3) & 1) ? 0x1000    /* START */
                                               : 0x8000;   /* A */
        } else {
            static uint32_t op = 0;
            op++;
            if (op == 1)    arena_oracle_phase("in-level");
            if (op >= 300 && op < 420) {                 /* locomotion baseline */
                *y = -1.0f;
                if (op == 300) arena_oracle_phase("walk");
            }
            if (op == 420)  arena_oracle_phase("stand"); /* 60 polls of idle */
            if (op >= 480 && op < 484) {                 /* tap B: drop/short throw */
                *buttons |= 0x4000;                       /* CONT_B */
                if (op == 480) arena_oracle_phase("dropB");
            }
            /* 484-900: observe - covers the game bomb's own fuse + blast */
            if (op >= 900 && op < 960) {                 /* hold B ... */
                *buttons |= 0x4000;
                if (op == 900) arena_oracle_phase("holdB");
            }
            if (op == 960)  arena_oracle_phase("releaseB"); /* ... throw */
            /* 960-1260: observe flight + impact */
            if (op == 1260) arena_oracle_phase("DONE");
        }
        return ok;
    }
```

- [ ] **Step 3: play.ps1 knob list** — add `"ARENA_ORACLE"` to `$knobs` in
`tools/play.ps1` (a stale `ARENA_ORACLE=1` would boot the user into the vanilla
campaign instead of the arena — exactly the trap play.ps1 exists to kill):

```powershell
$knobs = @("ARENA_SET_ANIM","ARENA_KICK_ANIM","ARENA_POSE_FRAMES","ARENA_POSE_MOVING",
           "ARENA_CAM_DIST","ARENA_CAM_PITCH","ARENA_CAM_YAW","ARENA_CAM_FOLLOW",
           "ARENA_CAM_OFF","ARENA_CAM_ZFAR","ARENA_AUTO_BATTLE","ARENA_ANIM_SWEEP",
           "ARENA_PROBE_AXIS","ARENA_RASTER_N","ARENA_RASTER_STEP","ARENA_ORACLE")
```

- [ ] **Step 4: Build**

Run: `.\build.ps1 -Config rwdi`
Expected: `BUILD OK`.

- [ ] **Step 5: SINGLE boot trial — where does the mash land?**

```powershell
Get-Process BMHeroRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
$pre = (Get-Content arena_bridge.log -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
$env:ARENA_ORACLE = '1'
Start-Process .\build-rwdi\BMHeroRecompiled.exe -WorkingDirectory (Get-Location)
Start-Sleep -Seconds 90
Get-Process BMHeroRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item Env:\ARENA_ORACLE
Get-Content arena_bridge.log | Select-Object -Skip $pre |
    Select-String '\[oracle' | Select-Object -First 40
```

Expected: `[oracle] frame n=... level=<N> player=1 floorY=...` lines and phase
markers through at least `dropB`. **Record which `level=` the mash lands in.**

**DECISION POINT — read this before proceeding:**
- `player=1` and phase markers present, and `[oracle-bomb]` lines appear after
  `dropB` → the mash reached a playable level; continue to Step 6.
- Landed on a non-playable screen (world map: `player=` may be 1 but no
  `[oracle-bomb]` ever appears after `dropB`; or no `[oracle]` lines at all) →
  implement the **warp fallback** (Step 5b) instead of tuning mash timings.

- [ ] **Step 5b (ONLY if the decision point demands it): warp fallback** —
`patches/arena_warp.c` already redirects `gCurrentLevel` in battle mode just
before the loader reads it (line 29: `gCurrentLevel = ARENA_WARP_MAP;`). Add an
oracle branch in the same patch redirecting to a known playable campaign map:

```c
    else if (arena_export_oracle_mode()) {
        gCurrentLevel = 1;   /* first campaign level; adjust from the level=N
                                the boot trial logged if 1 proves wrong */
    }
```

(`arena_export_oracle_mode` needs its `DECLARE_FUNC` copied into `arena_warp.c`
too — DECLARE_FUNCs are per-file.) Rebuild, rerun Step 5, confirm the level is
playable. Commit whichever variant worked as part of Step 7.

- [ ] **Step 6: 5-boot mash reliability trial** — repeat Step 5's launch loop 5
times (or script it inline). Expected: 5/5 reach `phase=in-level` and produce
`[oracle-bomb]` lines. Anything less than 5/5 → the boot is not oracle-grade;
stop and reassess (this is the spec's trial gate).

- [ ] **Step 7: Commit**

```powershell
git add src/main/main.cpp tools/play.ps1            # + patches/arena_warp.c if 5b ran
git commit -m "feat(oracle): vanilla auto-boot, mash-stop on in-level, scripted drop/throw phases"
```

---

### Task 4: `tools\oracle.ps1` — extraction + goldens

**Files:**
- Create: `tools/oracle.ps1`
- Create: `tools/oracle/goldens.json` (generated by the script; committed)

**Interfaces:**
- Consumes: the log line formats from Tasks 1–3 (exact strings above);
  `build-rwdi\BMHeroRecompiled.exe`.
- Produces: `tools/oracle/goldens.json` with fields
  `drop_anim_idx` (int), `drop_anim_frames` (int), `throw_anim_idx` (int),
  `throw_anim_frames` (int), `bomb_rest_lift` (float),
  `throw_impact_detonates` (bool), `throw_flight_frames` (int),
  `throw_arc_peak` (float), `no_oracle` (string array),
  `provenance` (object: `fork_commit`, `date`, `level`).
  `oracle-gate.ps1` (Task 5) reads exactly these names.

- [ ] **Step 1: Write the script** — `tools/oracle.ps1`:

```powershell
# Boot instrumented VANILLA single-player (ARENA_ORACLE=1), wait for the phase
# script to finish, and distill arena_bridge.log into tools\oracle\goldens.json.
# The goldens are the REFERENCE the arena is gated against (oracle-gate.ps1).
# Spec: bmhero-arena docs/superpowers/specs/2026-08-01-single-player-oracle-design.md
param(
    [switch]$Force,          # overwrite goldens that differ (default: show diff, exit 1)
    [int]$TimeoutSec = 150
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root "build-rwdi\BMHeroRecompiled.exe"
$log  = Join-Path $root "arena_bridge.log"
$out  = Join-Path $root "tools\oracle\goldens.json"
if (-not (Test-Path $exe)) { Write-Error "missing $exe (build first)"; exit 1 }

# -- launch with a CLEAN env (play.ps1 discipline: only ARENA_ORACLE applies) --
$knobs = @("ARENA_SET_ANIM","ARENA_KICK_ANIM","ARENA_POSE_FRAMES","ARENA_POSE_MOVING",
           "ARENA_CAM_DIST","ARENA_CAM_PITCH","ARENA_CAM_YAW","ARENA_CAM_FOLLOW",
           "ARENA_CAM_OFF","ARENA_CAM_ZFAR","ARENA_AUTO_BATTLE","ARENA_ANIM_SWEEP",
           "ARENA_PROBE_AXIS","ARENA_RASTER_N","ARENA_RASTER_STEP","ARENA_ORACLE")
$saved = @{}
foreach ($k in $knobs) { $saved[$k] = [Environment]::GetEnvironmentVariable($k)
                         Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
try {
    Get-Process BMHeroRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    $pre = 0
    if (Test-Path $log) { $pre = (Get-Content $log | Measure-Object -Line).Lines }
    $env:ARENA_ORACLE = '1'
    $p = Start-Process $exe -WorkingDirectory $root -PassThru
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $done = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        if ($p.HasExited) { break }
        $tail = Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Skip $pre
        if ($tail | Select-String -Quiet '\[oracle\] phase=DONE') { $done = $true; break }
    }
    Get-Process BMHeroRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if (-not $done) {
        $last = (Get-Content $log | Select-Object -Skip $pre |
                 Select-String '\[oracle' | Select-Object -Last 3) -join "`n"
        Write-Error "oracle run did NOT reach DONE. Last oracle lines:`n$last`n(fallback trigger - see spec 5.3)"
        exit 1
    }
    $lines = Get-Content $log | Select-Object -Skip $pre
} finally {
    foreach ($k in $knobs) {
        if ($null -ne $saved[$k]) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
        else { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
    }
}

# ------------------------------ parse ---------------------------------------
function PhaseN([string]$name) {
    $m = $lines | Select-String "\[oracle\] phase=$name n=(\d+)" | Select-Object -First 1
    if (-not $m) { Write-Error "phase marker '$name' missing from log"; exit 1 }
    [int]$m.Matches[0].Groups[1].Value
}
$nStand = PhaseN 'stand'; $nDrop = PhaseN 'dropB'
$nHold  = PhaseN 'holdB'; $nRel  = PhaseN 'releaseB'

$anim = $lines | Select-String '\[oracle-anim\] n=(\d+) idx=(\d+) frame=([\d.]+)' |
    ForEach-Object { [pscustomobject]@{ n = [int]$_.Matches[0].Groups[1].Value
                                        idx = [int]$_.Matches[0].Groups[2].Value
                                        fr  = [double]$_.Matches[0].Groups[3].Value } }
$bomb = $lines | Select-String '\[oracle-bomb\] n=(\d+) slot=(\d+) state=(\d+) pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)' |
    ForEach-Object { [pscustomobject]@{ n = [int]$_.Matches[0].Groups[1].Value
                                        y = [double]$_.Matches[0].Groups[5].Value } }
$blast = $lines | Select-String '\[oracle-blast\] n=(\d+)' |
    ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
$frameLn = $lines | Select-String '\[oracle\] frame n=(\d+) level=(\d+) player=\d+ floorY=([-\d.]+)' |
    ForEach-Object { [pscustomobject]@{ n = [int]$_.Matches[0].Groups[1].Value
                                        level = [int]$_.Matches[0].Groups[2].Value
                                        floor = [double]$_.Matches[0].Groups[3].Value } }

# idle baseline: the anim idx that dominates the stand window
$idleIdx = ($anim | Where-Object { $_.n -ge $nStand -and $_.n -lt $nDrop } |
            Group-Object idx | Sort-Object Count -Descending | Select-Object -First 1).Name
# clip after a marker: first idx run different from idle within 150 frames
function ClipAfter([int]$n0) {
    $win = @($anim | Where-Object { $_.n -ge $n0 -and $_.n -lt ($n0 + 150) -and $_.idx -ne [int]$idleIdx })
    if (-not $win) { return $null }
    $idx = $win[0].idx
    $run = @(); foreach ($a in $win) { if ($a.idx -eq $idx) { $run += $a } else { break } }
    [pscustomobject]@{ idx = $idx; frames = $run.Count }
}
$drop  = ClipAfter $nDrop
$throw = ClipAfter $nRel      # throw anim may fire at release; fall back to hold
if (-not $throw) { $throw = ClipAfter $nHold }

# bomb rest: dropped bomb's stable Y (>=10 consecutive samples within 0.5)
$dropBomb = @($bomb | Where-Object { $_.n -ge $nDrop -and $_.n -lt $nHold })
$restY = $null
for ($i = 0; $i -le $dropBomb.Count - 10; $i++) {
    $w = $dropBomb[$i..($i+9)].y
    if ((($w | Measure-Object -Maximum).Maximum - ($w | Measure-Object -Minimum).Minimum) -lt 0.5) {
        $restY = ($w | Measure-Object -Average).Average; break
    }
}
$floorY = ($frameLn | Where-Object { $_.n -ge $nDrop } | Select-Object -First 1).floor
$restLift = if ($null -ne $restY) { [math]::Round($restY - $floorY, 1) } else { $null }

# throw: impact detonation + flight envelope
$air     = $bomb | Where-Object { $_.n -gt $nRel }
$nBlastT = $blast | Where-Object { $_ -gt $nRel } | Select-Object -First 1
$impact = $false; $flight = $null; $arc = $null
if ($air -and $nBlastT) {
    $lastAir = ($air | Where-Object { $_.n -lt $nBlastT } | Select-Object -Last 1).n
    $flight  = $nBlastT - $nRel
    $impact  = (($nBlastT - $lastAir) -le 5) -and ($flight -lt 60)
    $y0   = $air[0].y
    $arc  = [math]::Round((($air | Measure-Object -Property y -Maximum).Maximum - $y0), 1)
}

$goldens = [ordered]@{
    drop_anim_idx          = if ($drop)  { $drop.idx }    else { $null }
    drop_anim_frames       = if ($drop)  { $drop.frames } else { $null }
    throw_anim_idx         = if ($throw) { $throw.idx }    else { $null }
    throw_anim_frames      = if ($throw) { $throw.frames } else { $null }
    bomb_rest_lift         = $restLift
    throw_impact_detonates = $impact
    throw_flight_frames    = $flight
    throw_arc_peak         = $arc
    no_oracle              = @("kick pose (Hero has no kick)",
                               "classic set-on-ground (Hero drops/throws only)",
                               "camera framing / explosion look / fun")
    provenance             = [ordered]@{
        fork_commit = (git -C $root rev-parse --short HEAD)
        date        = (Get-Date -Format 'yyyy-MM-dd')
        level       = ($frameLn | Select-Object -First 1).level
    }
}
# refuse silently-degraded goldens: every gated field must have extracted
$core = @('drop_anim_idx','bomb_rest_lift','throw_flight_frames')
$missing = $core | Where-Object { $null -eq $goldens[$_] }
if ($missing) { Write-Error "extraction incomplete - null: $($missing -join ', '). NOT writing goldens."; exit 1 }

$new = $goldens | ConvertTo-Json -Depth 4
if ((Test-Path $out) -and -not $Force) {
    $old = Get-Content $out -Raw
    if ($old.Trim() -ne $new.Trim()) {
        Write-Host "=== goldens DIFFER from checked-in (rerun with -Force to overwrite) ==="
        Write-Host "--- old ---`n$old`n--- new ---`n$new"
        exit 1
    }
    Write-Host "goldens unchanged."; exit 0
}
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Set-Content -Path $out -Value $new
Write-Host "goldens written to $out`n$new"
```

- [ ] **Step 2: Run the extraction**

Run: `.\tools\oracle.ps1` (fork root)
Expected: `goldens written to ...` with non-null core fields.

- [ ] **Step 3: VALIDATE THE INSTRUMENT (spec §5 — do not skip)**

Check the written goldens against known truths:
- `bomb_rest_lift` ≈ **30** (the decomp constant, 69AA0.c:359). Outside 28–32 →
  suspect the instrument (wrong slot? wrong floor sample?), not the game.
- `drop_anim_idx` should be a plausible clip index 0–63; compare with 29 (the
  decomp's drop-handler clip). A DIFFERENT value here is a REAL finding (runtime
  composition beats static decomp reading) — flag it in the task report, don't
  "fix" it.
- If Hero's tap-B turns out to THROW rather than drop (no stable rest Y), the
  extraction exits with `bomb_rest_lift` null — report this; the phase script
  needs a stationary-drop variant (hold shorter / different verb) before goldens
  can be written. Do not hand-edit nulls into the file.

- [ ] **Step 4: Commit**

```powershell
git add tools/oracle.ps1 tools/oracle/goldens.json
git commit -m "feat(oracle): extraction script + first goldens from vanilla single-player"
```

---

### Task 5: `tools\oracle-gate.ps1` + build wiring + falsifiability

**Files:**
- Create: `tools/oracle-gate.ps1`
- Modify: `build.ps1` (inside the `if ($Soak -gt 0 ...)` block, lines 133–139)

**Interfaces:**
- Consumes: `tools/oracle/goldens.json` (Task 4 field names);
  `tools\arena-soak.ps1` `-N/-Mode/-Rising/-Expect` switches (existing);
  arena log lines `[animw] +NN idx=<i> frame=<f> state=<s>`,
  `[setdbg] t<t> bi=.. live=.. simY=.. wy=<wy> originY=<oy> slot=..`,
  `[throw] t<N> ...` / `[blastvis] ... t<M>` (all existing formats).
- Produces: PASS/FAIL table + nonzero exit on any FAIL; `build.ps1 -Soak` runs
  it automatically when goldens exist.

- [ ] **Step 1: Write the gate** — `tools/oracle-gate.ps1`:

```powershell
# Assert the ARENA build matches the single-player goldens (tools\oracle\goldens.json).
# Three checks, three FAIL-able assertions; the expected values come from the
# GAME, not from constants we chose (trap #1 satisfied by construction).
param([string]$GoldensPath = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$log  = Join-Path $root "arena_bridge.log"
if (-not $GoldensPath) { $GoldensPath = Join-Path $root "tools\oracle\goldens.json" }
if (-not (Test-Path $GoldensPath)) { Write-Error "no goldens at $GoldensPath - run tools\oracle.ps1 first"; exit 1 }
$g = Get-Content $GoldensPath -Raw | ConvertFrom-Json
$fails = @()
function Check([string]$name, [bool]$ok, [string]$detail) {
    $mark = if ($ok) { "PASS" } else { $script:fails += $name; "FAIL" }
    Write-Host ("[oracle-gate] {0,-28} {1}  {2}" -f $name, $mark, $detail)
}
function LogDelta([scriptblock]$run) {
    $pre = 0
    if (Test-Path $log) { $pre = (Get-Content $log | Measure-Object -Line).Lines }
    & $run | Out-Host
    ,(Get-Content $log | Select-Object -Skip $pre)
}

# --- check 1+2: set pose identity/length + bomb rest lift (one mode-4 boot) ---
$m4 = LogDelta { & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\arena-soak.ps1") `
                   -N 1 -Mode 4 -Rising "\[animw\] \+\d+ idx=$($g.drop_anim_idx) frame=(\d+)" }
$poseLines = @($m4 | Select-String "\[animw\] \+\d+ idx=$($g.drop_anim_idx) frame=")
Check "set-pose idx==golden" ($poseLines.Count -gt 0) "golden idx=$($g.drop_anim_idx), saw $($poseLines.Count) frames"
$lenOK = [math]::Abs($poseLines.Count - $g.drop_anim_frames) -le 1
Check "set-pose plays once, full clip" $lenOK "golden $($g.drop_anim_frames) frames, saw $($poseLines.Count)"
$setdbg = $m4 | Select-String '\[setdbg\] .* wy=([-\d.]+) originY=([-\d.]+)' | Select-Object -First 1
if ($setdbg) {
    $lift = [double]$setdbg.Matches[0].Groups[1].Value - [double]$setdbg.Matches[0].Groups[2].Value
    Check "bomb rest lift" ([math]::Abs($lift - $g.bomb_rest_lift) -le 2.0) `
          ("golden {0}, arena {1:N1}" -f $g.bomb_rest_lift, $lift)
} else { Check "bomb rest lift" $false "no [setdbg] line in mode-4 log" }

# --- check 3: throw impact-detonation inside the golden flight envelope -------
$m11 = LogDelta { & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\arena-soak.ps1") `
                    -N 1 -Mode 11 -Expect '\[blastvis\]' }
$tThrow = $m11 | Select-String '\[throw\] t(\d+)'    | Select-Object -First 1
$tBlast = $m11 | Select-String '\[blastvis\] .* t(\d+)' | Select-Object -First 1
if ($tThrow -and $tBlast) {
    $dt  = [int]$tBlast.Matches[0].Groups[1].Value - [int]$tThrow.Matches[0].Groups[1].Value
    $cap = [math]::Ceiling($g.throw_flight_frames * 1.25)
    $ok  = $g.throw_impact_detonates -and ($dt -gt 0) -and ($dt -le $cap) -and ($dt -lt 100)
    Check "throw impact-detonates" $ok "golden flight $($g.throw_flight_frames) (cap $cap), arena dt=$dt"
} else { Check "throw impact-detonates" $false "missing [throw]/[blastvis] in mode-11 log" }

if ($fails) { Write-Host "`n[oracle-gate] FAILED: $($fails -join ', ')"; exit 1 }
Write-Host "`n[oracle-gate] ALL GREEN"; exit 0
```

- [ ] **Step 2: Wire into build.ps1** — inside the existing `if ($Soak -gt 0)`
block (after the soak invocation around line 139), add:

```powershell
        $goldens = Join-Path $root "tools\oracle\goldens.json"
        if (Test-Path $goldens) {
            Write-Host "`n=== oracle gate (goldens vs this build) ==="
            & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\oracle-gate.ps1")
            if ($LASTEXITCODE -ne 0) { Fail "oracle-gate FAILED" }
        } else {
            Write-Host "[oracle-gate] SKIPPED - no tools\oracle\goldens.json yet"
        }
```

(Match the surrounding style — `$root`/`Fail` already exist in build.ps1; adjust
names to whatever that block actually uses.)

- [ ] **Step 3: Green run on the current build**

Run: `.\tools\oracle-gate.ps1`
Expected: depends on Task 4's goldens. If goldens match current defaults
(drop=29, lift=30, impact detonation) → `ALL GREEN`. If a golden DIFFERS from a
current arena default, the gate correctly FAILS — flip the arena default to the
golden (e.g. `arena_set_anim_index()` default in `arena_bridge.cpp`, or
`BOMB_MESH_REST_LIFT` in the same file), rebuild, rerun. **Spec rule: goldens
override arena defaults, in the same commit.**

- [ ] **Step 4: FALSIFIABILITY CHECK (spec §5.2 — the gate must be able to fail)**

```powershell
$env:ARENA_SET_ANIM = '3'      # deliberately wrong pose
.\tools\oracle-gate.ps1        # EXPECTED: FAIL on set-pose checks
Remove-Item Env:\ARENA_SET_ANIM
.\tools\oracle-gate.ps1        # EXPECTED: back to ALL GREEN
```

A gate that stays green with `ARENA_SET_ANIM=3` is broken — stop and fix before
committing. (Wrinkle: arena-soak launches inherit this shell's env, which is
exactly why this works as the falsifiability probe.)

- [ ] **Step 5: Full build-with-gate proof**

Run: `.\build.ps1 -Config rwdi -Soak 3`
Expected: `BUILD OK`, 3/3 soak, then `[oracle-gate] ALL GREEN` — the wired path.

- [ ] **Step 6: Commit**

```powershell
git add tools/oracle-gate.ps1 build.ps1
git commit -m "feat(oracle): golden-driven arena gates wired into build.ps1 -Soak"
```

---

### Task 6: Documentation + final green state

**Files:**
- Modify: `docs/bmhero-recomp-integration-notes.md` (fork submodule's canonical
  copy lives in `lib/bmhero-arena/docs/` — edit the CANONICAL repo's
  `docs/bmhero-recomp-integration-notes.md` and bump the submodule as usual)
- Modify: canonical `docs/HANDOFF-2026-07-30.md` (or successor) + `CLAUDE.md` if
  the status lines change

**Interfaces:**
- Consumes: everything above, finished and green.
- Produces: §8.26 "single-player oracle" in the integration notes; handoff
  entries; commits in both repos (still unpushed).

- [ ] **Step 1: Integration notes §8.26** — add a section covering: the oracle
knob and its precedence; the per-frame probe block and why it is free when off;
the ground-query reuse; the goldens file and its provenance; the gate wiring;
the no-oracle list (kick/classic-set/aesthetics); the boot-trial outcome (mash
vs warp fallback — record which one shipped and which level the oracle runs in).

- [ ] **Step 2: Handoff + CLAUDE.md** — update the current handoff's open-items
list: feel-boot round 3 becomes "sign-off only; oracle-gate covers the
objective checks"; add "rerun tools\oracle.ps1 when a new behavior needs a
golden". Update CLAUDE.md's working-loop block to mention oracle-gate runs
inside `build.ps1 -Soak`.

- [ ] **Step 3: Final verification**

Run (fork): `.\build.ps1 -Config rwdi -Soak 5`
Expected: `BUILD OK`, 5/5, oracle-gate ALL GREEN.
Run (canonical): `tools\gate.ps1`
Expected: GATE GREEN, hash `cce00b99` unchanged (proves the sim was untouched).

- [ ] **Step 4: Commits**

```powershell
# canonical repo
git add docs/bmhero-recomp-integration-notes.md docs/HANDOFF-2026-07-30.md CLAUDE.md
git commit -m "docs: single-player oracle (8.26) - feel-boot objective checks are now gated"
# fork: bump the submodule if the canonical docs moved, plus any doc edits
git add lib/bmhero-arena docs
git commit -m "docs+chore: oracle notes; submodule bump"
```

Do NOT push either repo — both stay local pending the user's word.

---

## Amendment A (2026-08-01, mid-execution) — R-set / air-set / walk-in kick

**Trigger:** user discovery while playing: keyboard **R = bomb SET** (works on
the ground AND in the air), **space = jump**; **running into a set bomb KICKS
it**. Hero therefore has native references for the arena's set pose, kick pose,
and an air set — all three move from "no-oracle: human judgment" to golden-able.
Also folds in two empirical Task-3 findings: `arena_bridge.log` is TRUNCATED
per run (`fopen "w"`), so tools read the whole file (never `-Skip $pre`
delta-scans); and run-to-run world state varies, so goldens key on phase
markers + state sequences, never absolute positions or object counts.

### A1 — Task 3 extension (phase script)

Replace `if (op == 1260) arena_oracle_phase("DONE");` with:

```cpp
            if (op >= 1260 && op < 1264) {               /* tap R: SET at feet */
                *buttons |= 0x0010;                       /* CONT_R */
                if (op == 1260) arena_oracle_phase("setR");
            }
            /* 1264-1380: observe - set anim + bomb at rest */
            if (op >= 1380 && op < 1440) {               /* step clear of the set bomb */
                *y = -1.0f;
                if (op == 1380) arena_oracle_phase("walkoff");
            }
            if (op >= 1440 && op < 1560) {               /* run back in -> KICK */
                *y = 1.0f;
                if (op == 1440) arena_oracle_phase("kickrun");
            }
            /* 1560-1740: observe - kick anim + bomb slide */
            if (op >= 1740 && op < 1746) {               /* jump ... */
                *buttons |= 0x8000;                       /* CONT_A */
                if (op == 1740) arena_oracle_phase("jumpA");
            }
            if (op >= 1752 && op < 1756) {               /* ... R mid-air: AIR SET */
                *buttons |= 0x0010;
                if (op == 1752) arena_oracle_phase("airsetR");
            }
            if (op == 2040) arena_oracle_phase("DONE");
```

Verify from the trace that `setR` produces a bomb at the player's feet with no
throw arc. If mask `0x0010` (CONT_R) produces nothing, try `0x2000` (Z) once
and record which mask is the set button. If the set bomb's fuse expires before
`kickrun` contact, shorten the walkoff/kickrun windows (keep the phase-name
strings) and record the change.

**As built (fork `e6860d3`):** the block above's stick signs were INVERTED
(−1.0f drives +Z, toward the set bomb — the planned walkoff kicked it early);
signs swapped and windows pulled to walkoff 1320–1360 / kickrun 1360–1420,
because the set bomb's fuse measured **106 frames** (set n=632, blast n=738).
Set mask confirmed **0x0010 (CONT_R)** first try. Kick anim observed `idx=33
state=20`, and the clip begins slightly BEFORE contact — Task 4's
two-baseline (idle + walk) filter is load-bearing. Kick timing has ~20 frames
of slack on one verified trial: if kick extraction proves flaky, widen
`kickrun`'s UPPER bound (the fuse allows to roughly +16 frames); never move
its start. Air-set verified (bomb spawns at Y≈185 while airborne).

### A2 — Task 4 (extraction) changes

- Read the WHOLE log after the run (truncation finding above).
- New PhaseN markers: `setR`, `walkoff`, `kickrun`, `jumpA`, `airsetR`.
- New extractions (same ClipAfter mechanism):
  `set_anim` = ClipAfter(n(setR));
  `kick_anim` = first clip in [n(kickrun), n(kickrun)+150] that is neither the
  idle baseline NOR the walk baseline (dominant idx during the `walk` phase —
  the player is MOVING at kick contact, so the idle filter alone is wrong);
  `airset_anim` = ClipAfter(n(airsetR)) excluding the jump clip (dominant idx
  in [n(jumpA), n(airsetR)]).
- `bomb_rest_lift`: prefer the `setR` bomb (a true stationary set); keep the
  `dropB` extraction as fallback.
- New goldens fields: `set_anim_idx`/`set_anim_frames`,
  `kick_anim_idx`/`kick_anim_frames`, `airset_anim_idx`/`airset_anim_frames`,
  `set_button_mask` (string, e.g. "0x0010"), `kick_slide` (bool: the kicked
  bomb moves horizontally after contact rather than detonating on it).
- Core null-refusal list becomes: `set_anim_idx`, `kick_anim_idx`,
  `bomb_rest_lift`, `throw_flight_frames`.
- `no_oracle` shrinks to: camera framing / explosion look / fun. Drop, throw,
  and air-set anims are recorded but ungated (the arena has no air-set verb
  yet; drop/throw poses are not wired arena-side).

### A3 — Task 5 (gate) changes

- The set-pose gate keys on **`set_anim_idx`** (the R-set clip), NOT
  `drop_anim_idx`.
- ADD a kick gate: `arena-soak.ps1 -N 1 -Mode 10 -Rising "\[anim\] idx=$($g.kick_anim_idx) frame=(\d+)"`
  (mode 10 is the existing walk-in kick probe; `[anim]` is its burst channel).
- Defaults flip per the spec rule, in the same commit that turns the gate
  green: `arena_set_anim_index()` default (currently 29) → `set_anim_idx`
  golden; `arena_kick_anim_index()` default (currently −1) → `kick_anim_idx`
  golden; `ARENA_POSE_FRAMES` default (10) → `set_anim_frames` golden if it
  differs.
