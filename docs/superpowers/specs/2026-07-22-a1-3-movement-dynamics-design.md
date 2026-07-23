# A1.3 — movement dynamics/feel (campaign-player transcription) — design spec

**Date:** 2026-07-22 · **Status:** approved (design review in-session)
**Purpose:** replace the sim's placeholder movement with the real Bomberman
Hero **campaign-player** ground+air physics — top speed, acceleration,
**turn rate**, momentum, jump impulse, gravity, air control — transcribed
from the decomp, so arena movement feels like the game. Closes the feel gap
the A1.2e user reports isolated to dynamics (direction mapping is already
native-correct; §8.11). **Pure sim-side** (`bmhero-arena`); intentional
pinned-hash change.
**Builds on:** A1.2e (the anchors: `Math_CalcAngleRotated`, `2BF00.c:480`
`Vel = sin/cos(moveAngle)*moveSpeed`), A0 (fixed-point sim + determinism
suite).
**Companion:** `src/arena/arena_tuning.h` (`TUNE_*`, `TUNE_VERSION`),
`src/arena/arena_sim.c` (`player_tick`), `src/arena/arena_math.h`
(`qsin/qcos/iatan2`, Q20.12).

## 1. Goal and exit criterion

**Exit criterion:** the sim's ground+air movement reproduces the campaign
player's dynamics — measured top speed, acceleration, and turn rate (in
world-units/sec and deg/sec) match the decomp's transcribed numbers within a
documented tolerance, asserted by a new test in the determinism suite; all
existing determinism tests green; `TUNE_VERSION` bumped and the CI hash
re-pinned as an intentional gameplay change. Movement direction and the
fixed-point/determinism invariants are unchanged.

## 2. Reference-model decision (settled in brainstorm)

- **Model = campaign player** (not the debug free-mover `func_8002D080`,
  which is instant-velocity/analog-magnitude and arcade-y).
- **Scope = ground + airborne** (jump/gravity/air-control included); **bomb/
  blast recalibration deferred** (already Hero-authentic, A1.2c).
- **Verification = equation transcription + a units-consistency test** (no
  in-game A/B tooling this slice).
- **Approach = faithful model port (A), fallback to constants-first (B)** if
  Phase 1 finds the campaign update unreadable/impractical to transcribe.

## 3. Phase 1 — RE the campaign player physics (decomp-only, no build)

Locate and document the campaign player's per-frame movement update. **Start
anchors (plan-time greps):**
- `code/76640.c:442-443`: `D_801651D0 = moveAngle` (current) vs
  `Math_CalcAngleRotated(gActiveContStickX, -gActiveContStickY)` (target) —
  the turn-toward-target site.
- `boot/2BF00.c:480` `func_8002D080`: `Vel.{x,z} = {sin,cos}(moveAngle*
  DEG_TO_RAD) * moveSpeed` — the velocity integration (shared).
- `boot/math_util.c`: `Math_CalcAngleRotated` body (stick → target angle).
- Trace the player object's update dispatch (the routine that owns
  `gPlayerObject->moveAngle`/`moveSpeed` each frame for normal gameplay, not
  the `Debug_HandleObjMovement` free-mover).

**Deliverables (written into a new integration-notes section + this repo's
findings):**
1. **Turn rule:** does `moveAngle` snap to the target instantly, or rotate
   toward it at a bounded rate (deg/frame)? If bounded, the exact rate and
   whether it depends on speed.
2. **Speed rule:** how `moveSpeed` accelerates toward a stick-driven target
   (accel per frame, top speed, deceleration/friction when neutral, and
   whether analog magnitude scales the target speed).
3. **Airborne:** jump impulse (`Vel.y` on A), gravity per frame, terminal
   velocity, and any reduced air steering.
4. **Units keystone:** the game's **logic tick rate** (verify 30 vs 60 Hz)
   and a **world-unit scale anchor** — a physical constant shared by game and
   sim (e.g. tile size, player collision extent, or a bomb-throw distance)
   that pins Q20.12-units-per-game-unit. Everything else converts through it.

**Decision gate:** if the campaign update is unreadable or too scattered to
transcribe faithfully, STOP and fall back to approach B — port only the
structure Phase 1 *did* establish (at minimum the turn rate), transcribe the
constants that map, and document the residual gap. Re-plan with the user
before proceeding past the gate.

## 4. Phase 2 — port the model into `player_tick`

Reshape `arena_sim.c` `player_tick` to mirror the Phase-1 structure. Expected
changes (finalized by findings):
- **Turn rate:** replace the instant `p->yaw = dir` snap (line ~138) with a
  turn-toward-target step — rotate `p->yaw` toward the stick's binary-angle
  by at most `TUNE_TURN_RATE` per tick (short-arc, wrap-safe). This also makes
  puppet/player facing rotate naturally (A1.2e wired facing off `yaw`).
- **Speed/momentum:** align the accel-to-target with the game's rule
  (target = stick-magnitude-scaled top speed; accel/decel constants from
  Phase 1). Keep the existing structure if Phase 1 confirms it matches;
  otherwise restructure minimally.
- **Airborne:** jump impulse, gravity, terminal, air steering per findings.

**Invariants (hard — see README/CLAUDE.md):** no floats (Q20.12 only, int64
intermediates); the decomp's `sinf/cosf/DEG_TO_RAD` become binary-angle
`qsin/qcos` — **degrees→binary-angle conversion is required** and must be
exact; sim reads nothing outside `ArenaState`/inputs/`static const`; fixed
iteration + tick-phase order preserved; padding stays zeroed.

## 5. Phase 3 — transcribe constants + verify

- **Transcribe** every value into `arena_tuning.h` through the documented
  conversion: `q_value = game_value × (Q-units-per-game-unit) × (Hz factor)`.
  **The 30→60 Hz rescale is the top transcription hazard** — a per-frame
  game accel/speed at 30 Hz must be rescaled for the sim's 60 Hz ticks
  (per-tick deltas halve; per-tick² terms quarter — get this explicit and
  commented per constant). New constant `TUNE_TURN_RATE`; update
  `TUNE_RUN_SPEED/ACCEL/FRICTION/AIR_CONTROL/JUMP_IMPULSE/GRAVITY`; drop the
  `TODO(feel)` markers as each is sourced. Record the scale anchor in a
  header comment.
- **Units test (new, `tests/test_movement.c` or into `test_determinism.c`):**
  drive `player_tick` with a full-forward stick for N ticks; assert measured
  steady-state speed = transcribed top speed; assert time-to-top-speed
  matches the accel; drive a 180° stick flip and assert the turn completes in
  the transcribed turn-rate time. Tolerances documented (fixed-point rounding).
- **Re-pin:** bump `TUNE_VERSION` (2 → 3), run the one-line gcc build +
  `test_determinism`, record the new hash in CLAUDE.md as the intentional
  gameplay change (previous `4b6687d4`).

## 6. Non-goals (deferred, with owner)

- Bomb/blast magnitude recalibration → later (already Hero-authentic, A1.2c).
- Camera, render bridge, HUD, arena hazards (A1.2g) → separate slices.
- **No fork changes:** the render bridge already drives the tick and reads
  back displacement + yaw, so new dynamics + gradual turn appear on screen
  for free; a live harness sanity boot is a nicety, not the gate (the units
  test is authoritative).
- Per-player or item-based speed variants → v2.

## 7. Build & testing

- Repo: **`bmhero-arena` only** (standalone). After every sim change:
  `gcc -std=c11 -Wall -Wextra -O2 -o t src/arena/arena_sim.c
  tests/test_determinism.c && ./t` (plus the new movement test in its own
  build/`ctest` entry). The pinned CI hash changing is expected and must be
  an intentional, documented `TUNE_VERSION` bump.
- Findings → integration-notes movement section + CLAUDE.md status;
  this spec committed.
