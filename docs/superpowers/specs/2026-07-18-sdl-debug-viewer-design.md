# SDL Debug Viewer — design spec

**Date:** 2026-07-18 · **Status:** approved (design review in-session)
**Purpose:** ROM-free interactive viewer over the A0 headless arena sim
(`src/arena/`) for gameplay-feel iteration ahead of A1.
**Companion docs:** `docs/bmhero-battle-arena-design.md` §4–5,
`CLAUDE.md` (milestone list).

---

## 1. Goals and non-goals

**Goals**

- Drive the deterministic arena sim live at 60Hz with gamepad or keyboard and
  *see* movement, jump arcs, bomb throws, blasts, and knockback.
- Mirror the real game's presentation: Hero-style chase camera (behind the
  player, pitched down ~45°) with camera-relative stick controls.
- Feel-iteration tooling: pause, single-tick step, slow-motion, deterministic
  restart, live state readout (tick, hash, per-player physics).
- Zero changes to `src/arena/` — pinned CI hash `a55aa9b1` must not move.

**Non-goals (v1)**

- No tick interpolation (render = latest ticked state), no replay
  recording/playback (A2's GekkoNet stress session covers that), no netcode,
  no audio, no tuning hot-reload (tuning constants are compile-time by
  design; iterate via rebuild), no shipping this tool in the eventual
  BMHeroRecomp fork's mod package — it is a dev tool.

## 2. Architecture

New top-level directory `tools/viewer/` (floats permitted; the no-float rule
applies only to `src/arena/`):

    tools/viewer/
      viewer_main.c    entry point; SDL init, gamepad/keyboard input, input
                       packing (camera-yaw rotation), 60Hz fixed-timestep
                       accumulator loop, pause/step/slow-mo time control
      viewer_cam.c/.h  chase camera + top-down camera; view/projection
      viewer_draw.c/.h software transform, painter-sorted flat-shaded
                       rendering via SDL_RenderGeometry; primitives; HUD
      viewer_font8.h   embedded 8x8 bitmap font (public-domain glyph data)

The viewer is a strict client of the sim's public surface: `arena_init`,
`arena_tick`, `arena_hash`, read-only `ArenaState`, and `arena_geoms`.
Inputs are the only data flowing *into* the sim — the same one-way contract
the A1 render bridge will use.

**Main loop:** SDL wall-clock accumulator; every 1/60 s (scaled by the
slow-mo factor) pack inputs for all players and call `arena_tick` once.
Render every display frame (vsync) from the latest ticked state. Paused:
no ticks; `]` advances exactly one tick.

## 3. Camera and input mapping

- **Chase cam (default):** tracks player 0 (Tab cycles target). Camera yaw
  trails the tracked player's yaw with exponential smoothing (viewer-only
  float constant, initial ~0.08/tick). Position = look-target − forward(yaw)
  · DIST + (0, HEIGHT, 0), look-target = player pos + (0, ~1.0, 0); initial
  DIST = 4.0, HEIGHT = 5.0 (≈45° pitch: vertical drop ≈ DIST). All
  constants live at the top of
  `viewer_cam.c` and are expected to be hand-tuned.
- **Input transform:** the sim interprets stick in world space (stick up =
  −Z, `arena_sim.c` player update). The viewer rotates the physical stick
  vector by the *camera's* current yaw before `arena_input_pack`, giving
  camera-relative controls exactly like the campaign. Quantization to the
  sim's 6-bit encoding happens only at pack time.
- **Top-down toggle (F1):** orthographic XZ view, stick maps identity
  (world-space). For spacing/layout checks; chase cam is the feel view.
- **Devices:** SDL3 `SDL_Gamepad`, hotplug-aware; pads map to players 0..3
  in connect order. Keyboard always drives the lowest player without a pad:
  WASD move, Space jump, Left-Shift bomb (hold to grab/charge, release to
  throw — matching the sim's input semantics).

## 4. Rendering

- Arena collision prisms (`arena_geoms` AABBs): filled flat-shaded boxes,
  faces painter-sorted by view-space centroid depth (few dozen faces —
  trivial cost). Simple directional shade: base · (0.6 + 0.4·max(0, n·l)).
- Ground plane: 1-unit grid lines (speed perception) + arena bounds.
- Players: colored billboard circles (4 fixed colors) + **ground shadow
  blob** at (x, 0, z) — the primary height cue; radius from
  `TUNE_PLAYER_RADIUS`. State tint: flash while invulnerable, grey when
  dead, tumble marker.
- Bombs: smaller circles; held = follows owner; settled = fuse flash rate
  increases as `fuse` → 0. Blasts: translucent expanding circle billboards
  driven by `radius_t`/`ttl`. Shadow blobs for airborne bombs too.
- HUD (8×8 font, 2× scaled): top line `tick`, `arena_hash` (live), phase +
  phase_timer, sim-rate indicator; one line per active player: index/color,
  state name, hp, pos, vel, held/live bombs, relevant timer. Bottom line:
  key help.

## 5. Controls (summary)

| Key | Action |
|---|---|
| P | pause / resume |
| ] | step one tick (while paused) |
| [ | cycle slow-mo 1× → ¼× → 1/16× |
| R / Shift+R | restart same seed / new seed |
| Tab | cycle chase-cam target player |
| F1 | chase cam ↔ top-down |
| G | toggle grid |
| Esc | quit |

Gamepad: left stick move, A (south) jump, X (west) bomb grab/throw.

## 6. Build and toolchain

- **Toolchain (decided):** MSYS2 UCRT64 — `winget install MSYS2.MSYS2`,
  then `pacman -S --needed mingw-w64-ucrt-x86_64-gcc …-cmake …-ninja` and
  the prebuilt SDL3 package (verify exact name with `pacman -Ss sdl3` at
  install time).
- **CMake:** root `CMakeLists.txt` gains `find_package(SDL3 CONFIG QUIET)`;
  if found, target `arena_viewer` (sources above + `src/arena/arena_sim.c`,
  link `SDL3::SDL3`, C11, `-Wall -Wextra`). SDL3 absent ⇒ viewer target
  skipped with a status message; sim + tests build as before. The
  documented one-line gcc test build stays valid and toolchain-free.
- CI is unchanged — the viewer is not built in CI (SDL dependency stays out
  of the determinism gate).

## 7. Testing and acceptance

1. `src/arena/` diff is empty; determinism suite re-run passes with hash
   `a55aa9b1` (gcc container or MSYS2 gcc).
2. Viewer builds via CMake+Ninja under MSYS2 with SDL3 found.
3. Boots to a playable arena: keyboard-only session works; pad session
   works; 2P (pad + keyboard) works.
4. Chase cam frames the player from behind at ~45°; stick-up moves the
   player away from camera; camera follows turns smoothly.
5. Pause/step/slow-mo behave (step advances HUD tick by exactly 1; hash
   changes only when ticks advance; restart with same seed reproduces the
   same hash sequence).
6. Feel sign-off is the user's, by playing — this tool exists to make that
   loop fast; viewer constants (camera) are expected to be tweaked.

## 8. Risks / notes

- Chase-cam smoothing may feel swimmy initially — constants are isolated
  and viewer-only; tune freely.
- SDL3 package naming/availability in MSYS2 is verified at install time;
  fallback is SDL2 (API deltas are small at this feature level) or
  FetchContent-built SDL3 — either keeps the design intact.
- The embedded font header must carry a public-domain/CC0 glyph set so the
  repo stays GPL-compatible for the eventual fork.

## 9. Post-playtest addendum (2026-07-19)

Feel-testing corrected the camera design. Research (period reviews,
GameFAQs guides, StrategyWiki) confirmed the original game's camera is
**fixed-angle position-follow — it never rotates with the player's
facing** (slight C-button tilt only, while standing). The rotate-behind
chase cam specced in §3 caused motion sickness and did not match the
original. Changes relative to the spec above:

- Camera modes, `F1` cycles: **FOLLOW** (fixed-yaw position-follow —
  new default, Hero-authentic) → CHASE (spec §3 behavior, kept for
  comparison; gained an opposition deadband + turn-rate cap) → **ORBIT**
  (fixed seat framing the whole ring — previews the battle-mode camera,
  arena design doc §7) → TOP.
- `F2` toggles sudden-death wall shrink (default **off**): the viewer
  resets the phase after each tick so long test sessions aren't cut
  short. Viewer-side only; sim code untouched; never active in
  `--frames` smoke runs.
- Rendering hardening found in testing: floor/walls subdivided (whole-
  face near-plane culling made large quads blink out), checkerboard
  floor replaces grid lines, stable painter sort (insertion-order
  tie-break), walls render translucent (the camera legitimately looks
  through the near wall at the player; entities stay readable behind
  the glass).
