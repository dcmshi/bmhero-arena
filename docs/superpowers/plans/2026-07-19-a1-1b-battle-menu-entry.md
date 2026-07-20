# A1.1b Battle Menu Entry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "Battle" entry to the recomp launcher that sets a native battle-mode flag then launches the game; the arena bridge logs battle-mode so a human boot gate proves the hook.

**Architecture:** Two small native additions in the fork — a battle-mode flag on `arena_bridge` (read by the per-VI tick from A1.1a), and a "Battle" option in `on_launcher_init` whose callback mirrors the verified Start-Game path (`recomp::is_rom_valid` gate → set flag → `recomp::start_game`). No `patches/`, no rendering, no forced map warp (A1.1b-ii). Spec: `docs/superpowers/specs/2026-07-19-a1-1b-battle-menu-entry-design.md`.

**Tech Stack:** VS 2022 clang-cl (native), the A1.0 three-toolchain build recipe (memory `recomp-build-toolchain`).

## Global Constraints

- Work in the **fork** `C:\Users\dshi\GitRepos\BMHeroRecomp` on branch `feature/a1.1b-battle-menu` off `master` (which has A1.1a). The `bmhero-arena` repo gets only a CLAUDE.md doc update (final task).
- **`bmhero-arena` sim code is not modified** (pinned hash `4b6687d4` unmoved); no submodule bump.
- Only native files change: `src/arena_bridge/arena_bridge.{h,cpp}`, `src/main/main.cpp`. No `patches/` changes.
- Verified API facts (from the fork at A1.1a `master`):
  - `GameOptionsMenu::add_option(const std::string& title, std::function<void()> callback)` — `lib/RecompFrontend/recompui/src/base/ui_launcher.h`.
  - `void recomp::start_game(const std::u8string& game_id, const std::string& game_mode_id)` and `bool recomp::is_rom_valid(std::u8string& game_id)` — `lib/N64ModernRuntime/librecomp/include/librecomp/game.hpp`.
  - Start-Game reference call: `recomp::start_game(this->game_id, {}); recompui::hide_all_contexts();` (`ui_launcher.cpp:463-464`).
  - `on_launcher_init` in `src/main/main.cpp:568`; `add_default_options()` at line 577; `supported_games[0].game_id` (a `std::u8string`) is a file-scope global.
- **[build]** = run inside a VS dev shell with the composed PATH (memory `recomp-build-toolchain`). Canonical invocation (PowerShell):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; <command>
  ```
- Verification: build-exit + **human boot gate** (an agent cannot click a menu button).

---

### Task 1: Battle-mode flag on the bridge

**Files (fork):**
- Modify: `src/arena_bridge/arena_bridge.h`
- Modify: `src/arena_bridge/arena_bridge.cpp`

**Interfaces:**
- Consumes: A1.1a bridge (`arena_bridge_tick`, `g_state`, `proof()`).
- Produces: `extern "C" void arena_bridge_set_battle_mode(int on);` (Task 2 calls it from the menu). The per-VI proof line reads `[arena] BATTLE MODE tick N hash H` when set, `[arena] tick N hash H` otherwise.

- [ ] **Step 1: Branch**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git checkout master && git checkout -b feature/a1.1b-battle-menu
```

- [ ] **Step 2: Declare the setter in the header**

In `src/arena_bridge/arena_bridge.h`, add the declaration inside the `extern "C"` block, after `void arena_bridge_tick(void);`:
```c
void arena_bridge_set_battle_mode(int on);   /* menu sets this; tick reads it */
```

- [ ] **Step 3: Implement the flag + battle-aware log**

In `src/arena_bridge/arena_bridge.cpp`, add a `g_battle_mode` flag to the anonymous namespace (after `std::FILE* g_log = nullptr;`):
```cpp
    bool g_battle_mode = false;
```
Add the setter after the `arena_bridge_tick` function (outside the namespace):
```cpp
extern "C" void arena_bridge_set_battle_mode(int on) {
    g_battle_mode = (on != 0);
    std::printf("[arena] battle mode -> %s\n", g_battle_mode ? "ON" : "OFF");
    std::fflush(stdout);
    if (g_log) { std::fprintf(g_log, "[arena] battle mode -> %s\n",
                              g_battle_mode ? "ON" : "OFF"); std::fflush(g_log); }
}
```
And make the per-tick proof line battle-aware: replace the existing
```cpp
    if ((++g_calls % 60u) == 0u) {
        proof("[arena] tick %u hash %08x\n", g_state.tick, arena_hash(&g_state));
    }
```
with
```cpp
    if ((++g_calls % 60u) == 0u) {
        proof(g_battle_mode ? "[arena] BATTLE MODE tick %u hash %08x\n"
                            : "[arena] tick %u hash %08x\n",
              g_state.tick, arena_hash(&g_state));
    }
```
(Note: the `g_log` may be opened before the setter is ever called; the setter guards on `g_log` non-null, and the setter is also fine if called before first tick — it only sets a bool and logs.)

- [ ] **Step 4: Build (native compile clean; setter unused until Task 2)**

Run **[build]**:
```
cmake --build build-cmake --target BMHeroRecompiled 2>&1 | Select-String -Pattern "arena_bridge|error|FAILED|Linking CXX exe" | Select-Object -Last 5
```
Expected: `arena_bridge.cpp` recompiles, `Linking CXX executable BMHeroRecompiled.exe`, no errors. (Unused `extern "C"` setter is retained, not stripped.)

- [ ] **Step 5: Commit (fork)**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add src/arena_bridge/arena_bridge.h src/arena_bridge/arena_bridge.cpp
git commit -m "feat(arena): battle-mode flag on the bridge (setter + battle-aware proof log)"
```

---

### Task 2: "Battle" launcher option + human boot gate

**Files (fork):**
- Modify: `src/main/main.cpp`

**Interfaces:**
- Consumes: `arena_bridge_set_battle_mode` (Task 1); `recomp::is_rom_valid`, `recomp::start_game`, `recompui::hide_all_contexts`, `supported_games[0].game_id`.
- Produces: the running proof — Battle launches with `BATTLE MODE` in the log; Start Game does not.

- [ ] **Step 1: Ensure game.hpp is included**

In `src/main/main.cpp`, near the other `librecomp`/recompui includes (after `#include "arena_bridge.h"` added in A1.1a), add:
```cpp
#include "librecomp/game.hpp"
```
(Harmless if already transitively included — the header is include-guarded. It declares `recomp::start_game` / `recomp::is_rom_valid`.)

- [ ] **Step 2: Add the Battle option in on_launcher_init**

In `src/main/main.cpp`, immediately after the `game_options_menu->add_default_options();` line (line ~577) and before `game_options_menu->set_width(...)`, insert:
```cpp
    // A1.1b: Battle mode entry — set the native battle flag, then launch like
    // Start Game. (Forced warp to a dedicated arena map is A1.1b-ii.)
    game_options_menu->add_option("Battle", []() {
        std::u8string gid = supported_games[0].game_id;
        if (recomp::is_rom_valid(gid)) {
            arena_bridge_set_battle_mode(1);
            recomp::start_game(gid, {});
            recompui::hide_all_contexts();
        }
        // No ROM loaded yet: no-op. Load the ROM once via Start Game first;
        // the recomp persists the path, so Battle works on later launches.
    });
```
(The lambda is captureless — `supported_games` is a file-scope global. Placing it before the `for (auto option : get_options())` styling loop means Battle inherits the same styling as the default options.)

- [ ] **Step 3: Build**

Run **[build]**:
```
cmake --build build-cmake --target BMHeroRecompiled 2>&1 | Select-String -Pattern "error|FAILED|Linking CXX exe" | Select-Object -Last 5
```
Expected: `Linking CXX executable BMHeroRecompiled.exe`, no errors. If `is_rom_valid`/`start_game` are unresolved, confirm Step 1's include and that `recompui::hide_all_contexts` is declared in an already-included recompui header (it is used across `main.cpp`).

- [ ] **Step 4: Human boot gate (exit criterion)**

Ask the user to:
1. Launch `C:\Users\dshi\GitRepos\BMHeroRecomp\build-cmake\BMHeroRecompiled.exe` with `--show-console` (so the log window shows), from the fork root.
2. If the ROM isn't already remembered, click **Start Game** once to load it (this also confirms the non-battle path: `arena_bridge.log` shows `[arena] tick N …`, no BATTLE MODE), then return to the launcher.
3. Click the new **Battle** option. Confirm `arena_bridge.log` (fork root) now shows `[arena] battle mode -> ON` then `[arena] BATTLE MODE tick N hash H` lines advancing.
This is the A1.1b exit criterion: Battle sets the flag and launches; Start Game does not.

- [ ] **Step 5: Commit (fork) + push**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add src/main/main.cpp
git commit -m "feat(menu): Battle launcher option - sets battle flag then starts the game (A1.1b)"
git push -u origin feature/a1.1b-battle-menu
```

---

### Task 3: Docs

**Files:** `bmhero-arena` repo — `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md status**

In `bmhero-arena`'s `CLAUDE.md`, add to the status section:
```markdown
**A1.1b complete (2026-07-19).** The recomp launcher has a **Battle** option
(`on_launcher_init`, `main.cpp`) that sets a native battle-mode flag
(`arena_bridge_set_battle_mode`) then launches via `recomp::start_game` — the
same path as Start Game, gated on `recomp::is_rom_valid`. The per-VI proof
log distinguishes battle (`[arena] BATTLE MODE tick N …`) from normal
(`[arena] tick N …`), verified by the human boot gate. Fork branch
`feature/a1.1b-battle-menu`. The battle flag is the seam A1.1b-ii
(warp to a dedicated arena map) and A1.2 (render bridge) build on. Next:
A1.1b-ii forced map-shell warp.
```
Update the "Next milestones" A1 line: mark A1.1b done, next A1.1b-ii.

- [ ] **Step 2: Commit + push (bmhero-arena)**

```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md
git commit -m "docs: A1.1b complete - Battle launcher option + battle-mode flag"
git push
```

---

## Plan self-review notes

- Spec coverage: §1 goal/exit → Task 2 (human boot gate distinguishing Battle vs Start Game); §3 bridge flag → Task 1, menu entry → Task 2 (exact `add_option` + `is_rom_valid`-gated `start_game`); §4 data flow → Tasks 1-2; §5 build → Global Constraints + [build]; §6 testing/risk → Tasks 1-2 (build gate + human boot). Non-goals (warp, rendering, real inputs) untouched.
- No placeholders: every code block is complete and anchored to a verified line/API; the one edge case (no ROM loaded) has explicit no-op behavior, not a vague "handle it."
- Type consistency: `arena_bridge_set_battle_mode(int)` declared (Task 1 header) = used (Task 2 menu); `recomp::start_game(std::u8string, std::string)` / `recomp::is_rom_valid(std::u8string&)` match `game.hpp`; `supported_games[0].game_id` is `std::u8string`, matching both signatures.
- Risk note carried from spec: if `start_game`/`is_rom_valid` don't resolve, Step 1's include + matching the `ui_launcher.cpp:463` reference call site is the fix — not guesswork.
