# A1.1b — "Battle" menu entry + battle-mode flag — design spec

**Date:** 2026-07-19 · **Status:** approved (design review in-session)
**Purpose:** add a player-facing **Battle** entry to the recomp's launcher
that sets a native battle-mode flag and launches the game; the arena bridge
reads the flag. First menu hook. Builds on A1.1a (arena ticks natively via
the VI callback). Slice of A1.1b — the forced map warp is deferred to
**A1.1b-ii** (higher-risk level-state-machine RE, isolated on purpose).
**Companion:** A1.1a spec + plan; `CLAUDE.md` A1.1a status; memory
`recomp-build-toolchain`.

## 1. Goal and exit criterion

Prove the player-facing entry point and battle-mode plumbing end-to-end,
without any level-load RE or rendering.

**Exit criterion (human boot gate):** in the launcher, clicking a new
**Battle** option launches into the game with `arena_bridge.log` showing
`[arena] BATTLE MODE tick N hash H`; clicking the existing **Start Game**
launches with the flag off (`[arena] tick N hash H`, no BATTLE MODE). Same
advancing/deterministic ticks as A1.1a.

## 2. Non-goals (deferred, with owner)

- Forcing a specific arena map (`gCurrentLevel` + level-load trigger) →
  **A1.1b-ii**. A1.1b lands in whatever the game normally starts with; the
  flag is the proof, not the destination.
- Any rendering / reading game state / spawning objects → **A1.2** render
  bridge (A1.1c spawn suppression before it).
- Real inputs / a real match / a custom Battle sub-menu or arena picker →
  later. One flat "Battle" option is enough.
- No changes to `bmhero-arena` sim code (pinned hash `4b6687d4` unmoved).

## 3. Components

Two small additions, both in the fork.

- **Bridge flag (`src/arena_bridge/arena_bridge.{h,cpp}`):** add
  `extern "C" void arena_bridge_set_battle_mode(int on);` backed by an
  internal `bool g_battle_mode`. `arena_bridge_tick` reads it and includes
  `BATTLE MODE` in the proof line when set. The flag is the durable seam
  A1.1b-ii and A1.2 build on ("are we in a battle?" answered natively). Lives
  in the fork glue, never the pure submodule.
- **Menu entry (`src/main/main.cpp`, `on_launcher_init`):** after
  `game_options_menu->add_default_options();`, add:
  `game_options_menu->add_option("Battle", callback)` — the frontend's
  `GameOptionsMenu::add_option(const std::string&, std::function<void()>)`
  (verified in `lib/RecompFrontend/recompui/src/base/ui_launcher.h`). The
  callback mirrors the verified Start-Game path
  (`ui_launcher.cpp` → `recomp::start_game(game_id, {})`): set the flag, then
  start the game. Gated on ROM validity the same way Start-Game is
  (`start_game_option` uses a `rom_valid` check + `select_rom` fallback);
  if no ROM is loaded, Battle falls back to the load-ROM flow rather than
  launching into nothing.

## 4. Data flow

Menu click → `arena_bridge_set_battle_mode(1)` + `recomp::start_game(...)` →
game boots → VI callback (A1.1a) → `arena_bridge_tick` reads the flag → logs
battle-mode. One-way, additive to A1.1a's proof; nothing reads game state.

## 5. Build & integration

- Fork branch `feature/a1.1b-battle-menu` off `master` (which has A1.1a).
- Same three-toolchain recipe (memory `recomp-build-toolchain`): only native
  files change (`main.cpp`, `arena_bridge.*`) — no `patches/` touched, no
  submodule bump.
- The `bmhero-arena` repo gets only a CLAUDE.md status update.

## 6. Testing / risk

- Build-exit gate (native compile clean).
- **Human boot gate** is the exit criterion — an agent cannot click a menu
  button; the user clicks Battle vs Start Game and reads `arena_bridge.log`.
- Risk is low and menu-contained: `add_option` is a confirmed public API and
  the launch call is the same `recomp::start_game` the adjacent Start-Game
  option uses. Any callback-wiring quirk surfaces at build/click time against
  that known-good reference. If `recomp::start_game`'s signature/mode arg
  differs from the read (`(game_id, mode_id)`), match the Start-Game call
  site exactly rather than guessing.
