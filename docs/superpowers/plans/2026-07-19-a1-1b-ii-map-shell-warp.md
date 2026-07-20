# A1.1b-ii Map-Shell Warp Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On Battle launch, load `MAP_BATTLE_ROOM` as the visual shell instead of the campaign start, by overriding `gCurrentLevel` when the native battle flag is set.

**Architecture:** A native export (`arena_bridge_is_battle`) lets a MIPS patch read the A1.1b battle flag. A `RECOMP_PATCH` sets `gCurrentLevel = ARENA_WARP_MAP` before the level loader reads it. The exact patch site is resolved by runtime instrumentation (the load flow is spread across large functions with no clean static entry — the milestone's core RE). Spec: `docs/superpowers/specs/2026-07-19-a1-1b-ii-map-shell-warp-design.md`.

**Tech Stack:** VS 2022 clang-cl (native), LLVM-15 MIPS toolchain (patches — first `patches/` change since A1.0), the A1.0 three-toolchain build recipe (memory `recomp-build-toolchain`).

## Global Constraints

- Fork `C:\Users\dshi\GitRepos\BMHeroRecomp`, branch `feature/a1.1b-ii-map-warp` off `master` (has A1.1a/b). `bmhero-arena` gets only a CLAUDE.md doc update.
- **`bmhero-arena` sim code not modified** (pinned hash `4b6687d4`).
- **First `patches/` change since A1.0** — the build runs the LLVM-15 MIPS compile of `patches.elf` and `N64Recomp patches.toml`; the full-build PATH ordering (LLVM15 first, then VS, then `C:\msys64\ucrt64\bin`, then `C:\msys64\usr\bin`) is mandatory (memory `recomp-build-toolchain`).
- Verified facts (decomp at fork `master`):
  - `gCurrentLevel` — RAM `0x8016E428` (`BMHeroSyms/data_dump.toml`); a `s32` game global, declared `extern` for patches.
  - Level loader reads `D_80108238[gCurrentLevel]` then `DecompressFile(0x1C, …)` (map geometry), `Skybox_LoadFromID`, and player spawn from `D_80108238[gCurrentLevel]` — all keyed on `gCurrentLevel` (`boot/2BF00.c` ~line 1053+).
  - Map IDs in `lib/bmhero/include/map_ids.h`: `MAP_BATTLE_ROOM 2`, `MAP_HYPER_ROOM 3`, … `MAP_BOMBER_BASE_ENTRANCE 0`.
  - Native→patch call convention: native `extern "C" void f(uint8_t* rdram, recomp_context* ctx)` returns a value via `_return(ctx, val)` (`src/game/recomp_api.cpp`); registered with `REGISTER_FUNC(f)` in `main.cpp`; the patch imports it with `DECLARE_FUNC(type, f, …)` (see `patches/misc_funcs.h` `recomp_puts`) and links via the patch `#pragma` symbol table.
  - Patch logging: `recomp_printf(fmt, …)` is available to patches (`patches/print.c` `RECOMP_EXPORT`).
  - Patches are auto-included via the Makefile wildcard (`patches/*.c`); a new `patches/arena_warp.c` needs no build-file edit.
  - `RECOMP_PATCH` (`patches/patches.h`) replaces a whole decomp function by symbol name; `strict_patch_mode` validates the symbol exists.
- **[build]** = VS dev shell + composed PATH; canonical invocation (PowerShell):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; <command>
  ```
  A `patches/` change requires re-running the patch recompile before the main build:
  ```
  .\N64Recomp.exe patches.toml   # regenerates RecompiledPatches/ from patches.elf
  cmake --build build-cmake --target BMHeroRecompiled
  ```
  (CMake's `PatchesBin` custom target runs the LLVM-15 `make` in `patches/`; if it doesn't auto-trigger on a patch source change, run `make -C patches` in the dev shell first — the composed PATH puts LLVM-15 `clang`/`ld.lld` first.)
- **[launch]** = `play.bat --show-console` from the fork root (never the raw exe — CWD must be the fork root for `assets/`).
- Verification: build-exit + **human boot gate** (only a human can see which map loaded).

---

### Task 1: `arena_bridge_is_battle` native export + patch bridge (proves the call path)

**Files (fork):**
- Modify: `src/arena_bridge/arena_bridge.h`, `src/arena_bridge/arena_bridge.cpp`
- Create: `src/arena_bridge/arena_bridge_export.cpp` (the `(rdram, ctx)` export)
- Modify: `src/main/main.cpp` (register the export), `CMakeLists.txt` (add the export source)
- Create: `patches/arena_warp.c` (imports the export; logs it once to prove the path)

**Interfaces:**
- Consumes: A1.1b `g_battle_mode`.
- Produces: patch-callable `arena_bridge_is_battle()` returning `1`/`0`; the patch file where Task 2 adds the override.

- [ ] **Step 1: Branch**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git checkout master && git checkout -b feature/a1.1b-ii-map-warp
```

- [ ] **Step 2: Plain C++ getter on the bridge**

In `src/arena_bridge/arena_bridge.h`, add inside the `extern "C"` block:
```c
int arena_bridge_battle_active(void);   /* plain accessor for the export shim */
```
In `src/arena_bridge/arena_bridge.cpp`, add after `arena_bridge_set_battle_mode`:
```cpp
extern "C" int arena_bridge_battle_active(void) {
    return g_battle_mode ? 1 : 0;
}
```

- [ ] **Step 3: The recomp-ABI export shim**

Create `src/arena_bridge/arena_bridge_export.cpp`:
```cpp
// Recomp-ABI shim so MIPS patches can call into the native bridge.
#include "librecomp/recomp.h"
#include "arena_bridge.h"

extern "C" void arena_bridge_is_battle(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return(ctx, arena_bridge_battle_active());
}
```
(`recomp.h` provides `recomp_context` and `_return`; the include path already resolves — `main.cpp` uses librecomp headers.)

- [ ] **Step 4: Register the export**

In `src/main/main.cpp`, find the block of `REGISTER_FUNC(...)` calls (the base-export registrations near `register_base_export`). Add a declaration near the top of the file (after the arena include):
```cpp
extern "C" void arena_bridge_is_battle(uint8_t* rdram, recomp_context* ctx);
```
and add, alongside the other `REGISTER_FUNC(...)` lines:
```cpp
    REGISTER_FUNC(arena_bridge_is_battle);
```
(If the `REGISTER_FUNC` calls live in `src/game/recomp_data_api.cpp` instead — grep `REGISTER_FUNC(` to confirm the active list — add it there with the matching `extern "C"` declaration.)

- [ ] **Step 5: CMake — add the export source**

In `CMakeLists.txt`, add to the `SOURCES` list (next to `arena_bridge.cpp`):
```cmake
    ${CMAKE_SOURCE_DIR}/src/arena_bridge/arena_bridge_export.cpp
```

- [ ] **Step 6: The patch — import + one-shot proof log**

Create `patches/arena_warp.c`:
```c
#include "patches.h"
#include "misc_funcs.h"

#include <ultra64.h>

// Native bridge import (registered in main.cpp). Returns 1 in battle mode.
DECLARE_FUNC(s32, arena_bridge_is_battle);

extern s32 gCurrentLevel;

// A1.1b-ii proof: called from the warp patch site (Task 2). For Task 1 we
// only prove the native call resolves — log once.
static s32 arena_warp_logged = 0;
void arena_warp_probe(void) {
    if (!arena_warp_logged) {
        arena_warp_logged = 1;
        recomp_printf("[arena_warp] battle=%d gCurrentLevel=%d\n",
                      arena_bridge_is_battle(), gCurrentLevel);
    }
}
```
NOTE: `arena_warp_probe` is unused until Task 2 wires it into a real patch; Task 1 only compiles/links it to prove the toolchain + import resolve. It is not a `RECOMP_PATCH` yet, so it is dead code in the ELF — acceptable for the link gate. (If `strict_patch_mode` rejects an unreferenced non-patch function, make it `RECOMP_EXPORT void arena_warp_probe(void)` to keep it.)

- [ ] **Step 7: Build (patches + native)**

Run **[build]**:
```
.\N64Recomp.exe patches.toml
cmake --build build-cmake --target BMHeroRecompiled 2>&1 | Select-String -Pattern "arena|error|FAILED|Linking CXX exe" | Select-Object -Last 8
```
Expected: `patches.elf` rebuilds (LLVM-15), `arena_warp.c` compiles into it, `N64Recomp` regenerates patches, native links `arena_bridge_is_battle`, `Linking CXX executable BMHeroRecompiled.exe`. If the patch can't resolve `arena_bridge_is_battle`, confirm Step 4's `REGISTER_FUNC` is in the active export list (grep `REGISTER_FUNC(`).

- [ ] **Step 8: Commit**

```bash
git add src/arena_bridge/arena_bridge.h src/arena_bridge/arena_bridge.cpp src/arena_bridge/arena_bridge_export.cpp src/main/main.cpp CMakeLists.txt patches/arena_warp.c
git commit -m "feat(arena): arena_bridge_is_battle native export + patch import scaffold (A1.1b-ii)"
```

---

### Task 2: Locate the load site by instrumentation, then override gCurrentLevel

**Files (fork):** `patches/arena_warp.c`

**Interfaces:**
- Consumes: `arena_bridge_is_battle()`, `gCurrentLevel` (Task 1).
- Produces: a `RECOMP_PATCH` that, in battle mode, sets `gCurrentLevel = ARENA_WARP_MAP` before the map loads.

- [ ] **Step 1: Instrument the level-load flow to find the real runtime load site**

The load reads `gCurrentLevel` in `boot/2BF00.c` around line 1053 (`D_80108238[gCurrentLevel]` → `DecompressFile(0x1C, …)`), inside a large function. To confirm the actual runtime path (and that it fires on a Battle boot), add a temporary `RECOMP_PATCH` logging `gCurrentLevel` at the load. Identify the function symbol that contains line 1053:
```bash
awk 'NR<=1053 && /^[A-Za-z_].*\(.*\)[ ]*\{[ ]*$/{ln=NR;sig=$0} END{print ln": "sig}' lib/bmhero/src/boot/2BF00.c
```
Read that function fully (`sed -n '<ln>,+130p' lib/bmhero/src/boot/2BF00.c`). If it is a compact, fully-decompiled function (no `#pragma GLOBAL_ASM` inside), it is the patch target: it can be `RECOMP_PATCH`-replaced with `gCurrentLevel` overridden at the very top. If it is large/irreducible, instead find the **small** function called immediately before line 1053 that runs only on the load path (candidates in the pre-load lines 1041–1052), and `RECOMP_PATCH`-replace *that* to set `gCurrentLevel` first, then call the original body.

- [ ] **Step 2: Add the override patch (against the Step-1 target `FN`)**

In `patches/arena_warp.c`, replace `arena_warp_probe` with the real patch. Substitute `FN` and its exact original body from Step 1's read (RECOMP_PATCH replaces the whole function, so the original statements are copied verbatim after the override):
```c
#define ARENA_WARP_MAP 2   /* MAP_BATTLE_ROOM; change to fall back (see Task 3) */

RECOMP_PATCH void FN(void) {
    // @recomp A1.1b-ii: in battle mode, redirect the level load to the arena
    // shell before the loader reads gCurrentLevel. Runs each load; harmless
    // when not in battle.
    if (arena_bridge_is_battle()) {
        gCurrentLevel = ARENA_WARP_MAP;
    }
    recomp_printf("[arena_warp] load gCurrentLevel=%d battle=%d\n",
                  gCurrentLevel, arena_bridge_is_battle());
    /* --- original body of FN copied verbatim from boot/2BF00.c --- */
    /* (filled from Step 1's read) */
}
```
Add any `extern` declarations the copied body needs (globals/functions it references) at the top of `arena_warp.c`, mirroring how `patches/teleporter_obj.c` declares the externs it uses.

- [ ] **Step 3: Build**

Run **[build]**:
```
.\N64Recomp.exe patches.toml
cmake --build build-cmake --target BMHeroRecompiled 2>&1 | Select-String -Pattern "arena|error|FAILED|Linking CXX exe" | Select-Object -Last 8
```
Expected: clean link. `strict_patch_mode` confirms `FN` is a real symbol.

- [ ] **Step 4: Human boot gate — did the battle room load?**

Ask the user to **[launch]** `play.bat --show-console`, click **Battle**, and report:
1. Console shows `[arena_warp] load gCurrentLevel=2 battle=1` (the override fired), and
2. Which map visibly loaded — the Battle Room (success), Bomber Base (override too late / wrong function → return to Step 1 with the log's evidence), or a black-screen/crash (battle room needs setup → Task 3 fallback).
Also confirm **Start Game** still loads the normal campaign (`battle=0`, no override).

- [ ] **Step 5: Commit**

```bash
git add patches/arena_warp.c
git commit -m "feat(arena): warp to MAP_BATTLE_ROOM on battle launch via gCurrentLevel override (A1.1b-ii)"
```

---

### Task 3: Verify-first fallback + push

**Files (fork):** `patches/arena_warp.c` (only if fallback needed)

- [ ] **Step 1: If the Battle Room black-screens or crashes, fall back**

If Task 2 Step 4 showed the override fires but `MAP_BATTLE_ROOM` doesn't load cleanly (needs multiplayer setup we won't fake this milestone), change the one constant to a known-good open campaign level and re-verify:
```c
#define ARENA_WARP_MAP 8   /* MAP_EMERALD_TUBE — an early open campaign level */
```
Rebuild (**[build]**), re-run the boot gate (**[launch]** → Battle), confirm it loads that level. Try `MAP_HYPER_ROOM` (3) / `MAP_HEAVY_ROOM` (4) first if the user prefers a battle-style room; the constant is the only change.

- [ ] **Step 2: Trim the debug log to once-per-load or remove**

Once a map loads, reduce console noise — keep a single confirmation line by gating it on battle mode only:
```c
    if (arena_bridge_is_battle()) {
        gCurrentLevel = ARENA_WARP_MAP;
        recomp_printf("[arena_warp] -> map %d\n", gCurrentLevel);
    }
```

- [ ] **Step 3: Commit + push**

```bash
git add patches/arena_warp.c
git commit -m "chore(arena): final warp target + trimmed load log (A1.1b-ii)"
git push -u origin feature/a1.1b-ii-map-warp
```

---

### Task 4: Docs

**Files:** `bmhero-arena` — `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md status**

Append to the status section (fill `<MAP>` and `<FN>` with the shipped values):
```markdown
**A1.1b-ii complete (2026-07-19).** Battle launch warps into `<MAP>` instead
of the campaign start: `arena_bridge_is_battle` (native export) lets a MIPS
`RECOMP_PATCH` on `<FN>` (`boot/2BF00.c` level-load path) set `gCurrentLevel`
to the arena shell before the loader reads it; non-battle launches unchanged.
First `patches/` change since A1.0 (LLVM-15 MIPS path). Fork branch
`feature/a1.1b-ii-map-warp`. Next: A1.1c suppress the shell map's own object
spawns, then A1.2 render bridge.
```
Update the "Next milestones" A1 line: mark A1.1b-ii done, next A1.1c.

- [ ] **Step 2: Commit + push (bmhero-arena)**

```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md
git commit -m "docs: A1.1b-ii complete - Battle warps into the arena shell map"
git push
```

---

## Plan self-review notes

- Spec coverage: §1 goal/exit → Task 2 Step 4 (boot gate: battle room vs campaign); §2 mechanism (native export + patch override) → Tasks 1–2; the spec's "core RE is finding the patch site" → Task 2 Step 1 (concrete instrumentation procedure with the load-site line pinned to `2BF00.c:1053`); §3 warp constant + fallback → Task 3; §5 non-goals (spawn suppression, rendering) untouched; §6 build (first `patches/` change, LLVM-15) → Global Constraints + [build]; §7 testing/risk → human boot gate + both fallbacks (wrong site → Step 1; unloadable map → Task 3).
- Honest discovery: Task 2 Step 1 is a real runtime-instrumentation step, not a placeholder — it gives the exact line to anchor from (`2BF00.c:1053`), the awk to find the containing function, and the decision rule (compact → patch it; large → patch the small pre-load callee). The exact `FN` is determined by running the game because the static flow does not expose a single clean small entry (spec §2, acknowledged).
- Type consistency: `arena_bridge_battle_active()` (C++ getter) → `arena_bridge_is_battle(rdram, ctx)` (export shim) → `DECLARE_FUNC(s32, arena_bridge_is_battle)` (patch import) → `REGISTER_FUNC(arena_bridge_is_battle)` (registration). `ARENA_WARP_MAP` single definition. `gCurrentLevel` extern `s32` matches the decomp.
- Risk carried from spec: if no clean patch site emerges even with instrumentation, Task 2 Step 1 says to patch the small pre-load callee rather than the giant loader — and if that also fails, the honest move is to surface it to the user (the plan does not authorize whole-replacing a giant irreducible function).
