# A1.1a Arena Submodule + Proof-of-Life Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the native arena sim compiles into the recompiled binary and ticks in its frame loop — a console log of advancing tick+hash while the game runs.

**Architecture:** Add `bmhero-arena` as a submodule of the fork; compile `arena_sim.c` natively; a fork-owned `arena_bridge` ticks a `static ArenaState` each VI and logs. Driven by the recomp's existing native `vi_callback` (fired every frame) — **no MIPS patch, no patches.elf changes** (simpler and more correct than the spec's patch-driver: the arena is native mod code and the VI retrace is the design's tick edge). Spec: `docs/superpowers/specs/2026-07-19-a1-1a-arena-submodule-proof-design.md`.

**Tech Stack:** VS 2022 clang-cl (native), C11 sim, CMake, the A1.0 three-toolchain build recipe (memory `recomp-build-toolchain`).

## Global Constraints

- Work in the **fork** `C:\Users\dshi\GitRepos\BMHeroRecomp` on branch `feature/a1.1a-arena-proof`. The `bmhero-arena` repo gets only a CLAUDE.md doc update (final task).
- **`bmhero-arena` sim code is not modified** (pinned hash `4b6687d4` unmoved). The fork only *reads* the submodule.
- Submodule path in the fork: `lib/bmhero-arena`, pinned to current `bmhero-arena` `main` (commit `c25b3b4` or newer at add time).
- Fork-specific glue lives in the fork under `src/arena_bridge/`, never in the submodule.
- Build recipe (memory `recomp-build-toolchain`): VS dev shell; full-build PATH `C:\Users\dshi\GitRepos\.tools\llvm15\bin` + VS dev-shell PATH + `C:\msys64\ucrt64\bin` + `C:\msys64\usr\bin`.
- **[build]** = run inside a VS dev shell with that PATH. Canonical invocation (PowerShell):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; <command>
  ```
- Verification is build-exit + human boot (this milestone adds no sim logic; the sim is unit-tested in its own repo).

---

### Task 1: Add the submodule + build it into the binary (link gate)

**Files (in the fork):**
- Create: `.gitmodules` (via `git submodule add`)
- Create: `src/arena_bridge/arena_bridge.h`, `src/arena_bridge/arena_bridge.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `extern "C" void arena_bridge_tick(void);` (native, callable from `main.cpp`) — ticks a lazy-init `static ArenaState` and logs every 60 calls. Task 2 calls it from the VI callback.

- [ ] **Step 1: Branch + add submodule**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git checkout -b feature/a1.1a-arena-proof
git submodule add https://github.com/dcmshi/bmhero-arena.git lib/bmhero-arena
git submodule update --init --recursive lib/bmhero-arena
ls lib/bmhero-arena/src/arena/arena_sim.c
```
Expected: submodule cloned; `arena_sim.c` present; `.gitmodules` created.

- [ ] **Step 2: Write the bridge header**

Create `src/arena_bridge/arena_bridge.h`:
```c
#ifndef ARENA_BRIDGE_H
#define ARENA_BRIDGE_H
/* Fork-native glue between the recomp host and the pure arena sim
 * (lib/bmhero-arena). A1.1a: tick a silent passenger ArenaState once per VI
 * and log proof-of-life. No game state read/written; no rendering. */
#ifdef __cplusplus
extern "C" {
#endif
void arena_bridge_tick(void);   /* call once per VI/frame */
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 3: Write the bridge implementation**

Create `src/arena_bridge/arena_bridge.cpp`:
```cpp
#include "arena_bridge.h"

#include <cstdio>

extern "C" {
#include "arena/arena_sim.h"
}

namespace {
    bool       g_inited = false;
    ArenaState g_state;
    uint32_t   g_calls = 0;
}

extern "C" void arena_bridge_tick(void) {
    if (!g_inited) {
        arena_init(&g_state, 0, 4, 0xB0BB1E5u);  /* 4 idle players; round won't end */
        g_inited = true;
        std::printf("[arena] bridge init: state %zu bytes\n", sizeof(ArenaState));
    }
    /* neutral inputs: silent passenger this milestone */
    const ArenaInput neutral[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
    arena_tick(&g_state, neutral);
    if ((++g_calls % 60u) == 0u) {
        std::printf("[arena] tick %u hash %08x\n",
                    g_state.tick, arena_hash(&g_state));
        std::fflush(stdout);
    }
}
```

- [ ] **Step 4: Wire into CMake**

In the fork's `CMakeLists.txt`, add the sim `.c` and the bridge `.cpp` to the `SOURCES` list (the `set (SOURCES …)` block ending at the `rsp/n_aspMain.cpp` line):
```cmake
    ${CMAKE_SOURCE_DIR}/rsp/n_aspMain.cpp

    ${CMAKE_SOURCE_DIR}/lib/bmhero-arena/src/arena/arena_sim.c
    ${CMAKE_SOURCE_DIR}/src/arena_bridge/arena_bridge.cpp
)
```
And add the two include dirs to the `target_include_directories(BMHeroRecompiled PRIVATE …)` block (the one starting at `${CMAKE_SOURCE_DIR}/include`):
```cmake
    ${CMAKE_SOURCE_DIR}/lib/bmhero-arena/src
    ${CMAKE_SOURCE_DIR}/src/arena_bridge
```
(The sim's headers `#include "arena/arena_sim.h"` etc. resolve from `lib/bmhero-arena/src`.)

- [ ] **Step 5: Build (link gate — bridge unused but compiled/linked)**

To force the linker to keep `arena_bridge_tick` before Task 2 references it, temporarily is unnecessary — instead verify compilation now and prove the wire-up in Task 2. Run **[build]**:
```
cmake --build build-cmake --target BMHeroRecompiled 2>&1 | Select-String -Pattern "arena|error|FAILED|Linking" | Select-Object -Last 8
```
Expected: `arena_sim.c` and `arena_bridge.cpp` compile; `Linking CXX executable BMHeroRecompiled.exe`; no errors. (Unused `arena_bridge_tick` is fine — it's `extern "C"`, not stripped from the object; Task 2 calls it.)

- [ ] **Step 6: Commit (fork)**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add .gitmodules lib/bmhero-arena src/arena_bridge CMakeLists.txt
git commit -m "feat(arena): add bmhero-arena submodule + native arena_bridge (compiles into binary)"
```

---

### Task 2: Drive the bridge from the VI callback + prove it (boot gate)

**Files (in the fork):**
- Modify: `src/main/main.cpp` (wrap the existing `vi_callback`)

**Interfaces:**
- Consumes: `arena_bridge_tick()` (Task 1).
- Produces: the running proof — `[arena] tick N hash H` every ~second.

- [ ] **Step 1: Include the bridge header**

In `src/main/main.cpp`, after the existing include block (near the `#include "recomp_data.h"` line), add:
```cpp
#include "arena_bridge/arena_bridge.h"
```

- [ ] **Step 2: Add a VI-callback wrapper**

The current callback is `.vi_callback = recompinput::update_rumble` (a `void()` fn pointer). Add a wrapper just above the `main()` function (after the `#define REGISTER_FUNC(...)` line) that calls both the original and our tick:
```cpp
// A1.1a: tick the arena sim once per VI alongside the existing rumble update.
static void arena_vi_callback() {
    recompinput::update_rumble();
    arena_bridge_tick();
}
```

- [ ] **Step 3: Point the callback at the wrapper**

In the `ultramodern::events::callbacks_t thread_callbacks{ … }` initializer, change:
```cpp
        .vi_callback = recompinput::update_rumble,
```
to:
```cpp
        .vi_callback = arena_vi_callback,
```

- [ ] **Step 4: Build**

Run **[build]**:
```
cmake --build build-cmake --target BMHeroRecompiled 2>&1 | Select-String -Pattern "error|FAILED|Linking" | Select-Object -Last 5
```
Expected: `Linking CXX executable BMHeroRecompiled.exe`, no errors.

- [ ] **Step 5: Headless-ish boot check (agent)**

Run (PowerShell) with the console shown so stdout is captured — launch, let it run ~6s, then read the log. The recomp prints to stdout with `--show-console`; capture via redirection:
```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
$p = Start-Process -FilePath ".\build-cmake\BMHeroRecompiled.exe" -ArgumentList "--show-console" -WorkingDirectory "C:\Users\dshi\GitRepos\BMHeroRecomp" -RedirectStandardOutput "arena_proof.log" -PassThru
Start-Sleep -Seconds 6
if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id -Confirm:$false }
Select-String -Path arena_proof.log -Pattern "\[arena\]" | Select-Object -First 6
```
Expected: `[arena] bridge init …` then several `[arena] tick N hash H` lines with N increasing (60, 120, 180, …). If stdout redirection is empty (RT64/SDL may reopen the console), fall back to the human step and read the on-screen console window.
Then delete the log: `Remove-Item arena_proof.log`.

- [ ] **Step 6: Human boot gate**

Ask the user to launch `BMHeroRecompiled.exe` (double-click, or with `--show-console` for the log window), load any campaign level, and confirm the console shows `[arena] tick N hash H` advancing ~once per second during play. This is the A1.1a exit criterion.

- [ ] **Step 7: Determinism-inside-host check (agent)**

Two runs must log the same hash for the same tick (neutral inputs ⇒ reproducible). Repeat Step 5 twice capturing to `run1.log`/`run2.log`, then:
```powershell
$a = (Select-String -Path run1.log -Pattern "\[arena\] tick 120 " | Select-Object -First 1).Line
$b = (Select-String -Path run2.log -Pattern "\[arena\] tick 120 " | Select-Object -First 1).Line
if ($a -and $a -eq $b) { "DETERMINISTIC: $a" } else { "MISMATCH: '$a' vs '$b'" }
Remove-Item run1.log, run2.log
```
Expected: `DETERMINISTIC: [arena] tick 120 hash ...` (same hash both runs). If the `tick 120` line isn't captured (timing), compare any common tick line present in both.

- [ ] **Step 8: Commit (fork) + push**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add src/main/main.cpp
git commit -m "feat(arena): tick arena sim from VI callback - proof-of-life log (A1.1a)"
git push -u origin feature/a1.1a-arena-proof
```

---

### Task 3: Docs

**Files:** `bmhero-arena` repo — `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md status**

In the `bmhero-arena` repo's `CLAUDE.md`, append to the status section:
```markdown
**A1.1a complete (2026-07-19).** `bmhero-arena` is a submodule of the fork at
`lib/bmhero-arena`; `arena_sim.c` compiles natively into `BMHeroRecompiled`,
and a fork-owned `src/arena_bridge/` ticks a silent-passenger `ArenaState`
from the recomp's VI callback (`main.cpp`), logging `[arena] tick N hash H`
each second — deterministic across runs. No MIPS patch needed (native VI hook
= the design's tick edge). Fork branch `feature/a1.1a-arena-proof`. Next:
A1.1b Battle menu entry (+ warp-into-map), then A1.1c spawn suppression,
then A1.2 render bridge.
```
Update "Next milestones" A1 line: mark A1.1a done within the A1.1 breakdown.

- [ ] **Step 2: Commit + push (bmhero-arena)**

```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md
git commit -m "docs: A1.1a complete - arena submodule ticks inside the recomp (proof-of-life)"
git push
```

---

## Plan self-review notes

- Spec coverage: §1 goal/exit → Task 2 (boot gate + determinism); §3 submodule → Task 1 Step 1, bridge → Task 1 Steps 2-3, "patch frame driver" → **replaced by the native VI callback** (Task 2), a documented improvement (spec §6's risk — picking a per-frame function — is dissolved: `vi_callback` is *the* per-frame native hook, already wired in `main.cpp:772`); §5 build → Global Constraints + [build]; §7 testing → Tasks 2-3.
- Deviation from spec §3/§6 (patch driver → native VI callback): stated with rationale. The spec's intent ("prove our native code ticks in the frame loop") is fully met and the MIPS import-table plumbing is avoided — strictly lower risk. No fork `patches/` changes.
- The VI callback fires from launcher onward (not only in-game); the exit-criterion log therefore appears from boot, which over-satisfies "runs while a level plays." Human step still loads a level to match the spec's wording.
- No placeholders: every code block is complete; the one fallback (stdout redirection may be empty) names the concrete alternative (human console read).
