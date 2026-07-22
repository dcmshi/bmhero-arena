# A1.2e — camera-relative stick input — design spec

**Date:** 2026-07-22 · **Status:** approved (design review in-session)
**Purpose:** fix the A1.2a known item — forward/back reads "compressed"
because the raw stick is fed to the sim in world axes regardless of where the
camera sits. After this slice, stick-up moves the bomberman directly away
from the camera, measured from the real `gView` transform every frame.
First half of the feel pass; **A1.3 (decomp feel constants) follows as its
own slice** once direction is correct — A1.2a proved feel can't be judged
through a broken mapping (blind scale-tuning went the wrong way).
**Builds on:** A1.2a input path (`patches/arena_render.c` battle block),
A1.2d lessons (validate game data before trusting it; forensic markers).
**Companion:** integration notes §8.8 (verification loop), §8.10 (draw/update
gating), design doc §7 (Hero's camera never rotates with facing).

## 1. Goal and exit criterion

**Exit criterion (human boot gate):** in the arena, stick-up moves the player
straight away from the camera on screen, stick-down toward it, left/right
lateral; diagonals read at the same apparent speed as cardinals; the
"compressed" forward/back is gone (human feel pass + measurement log).
Buttons (jump/bomb/set) and every other input untouched; bombs/blasts/set/
kick regression-verified; ≥3 clean boots. Sim untouched (pinned hash
`4b6687d4`). New fork branch `feature/a1.2e-camera-input`.

## 2. What we know (plan-time facts, 2026-07-22)

- Current mapping (`arena_render.c` battle block): `sx = stickX·31/80`,
  `sy = stickY·31/80`, per-axis clamp ±31, packed via
  `arena_export_tick_input(sx, sy, buttons)`; **sim stick-up = −Z**; no
  camera involvement anywhere.
- `gView` is a **named** extern (`struct View`, `variables.h:609`) with
  `.eye`/`.at` Vec3f — already read by upstream patch files (`demo.c`,
  `required_patches.c`), so it resolves and is patch-readable.
- Float math **inside** patches is fine (MIPS has native sqrt; only the
  native-export ABI restricts floats).
- The sim derives facing from the stick ⇒ world-correct input makes player
  and puppet facing world-correct for free.
- Hero's camera never rotates with the player's facing (design doc §7), but
  its yaw in the arena is unmeasured — hence phase 1.

## 3. Phase 1 — measure first (one boot)

Instrument the battle block: log via `arena_export_dbg_u32` at a reduced rate
if a game frame counter is handy in `variables.h` (modulo-gate, no new patch
state), else every frame — it's a temporary forensic build either way:
`eye.x/z`, `at.x/z` (as float bit patterns) and the derived ground-plane
forward. Human walks a square with the CURRENT mapping. Deliverables:
- the arena camera's true yaw/pitch (the "before" record, kept in §8.11);
- validation that `gView` holds sane data in the bypassed arena (A1.2d rule:
  **validate before trusting** — if eye==at or values are wild, STOP and
  re-plan; do not build on garbage);
- how far the camera yaw drifts as the player moves (informs whether the
  perp-sign is globally stable).

## 4. Phase 2 — the mapping (patch-only)

In `arena_render_routine`, replace the raw scaling with:

1. `fx = gView.at.x − gView.eye.x; fz = gView.at.z − gView.eye.z;`
2. `len² = fx² + fz²`; **if `len² < EPS` (degenerate frame / camera cut):
   identity mapping this frame** (stateless fallback, no last-frame memory);
3. normalize; `right = perp(fwd)` — perp sign chosen from phase 1's
   measurement so stick-right is screen-right (documented in-code with the
   sim's stick-up = −Z convention);
4. `world = stickUp·fwd + stickX·right` (floats, stick pre-scaled /80);
5. clamp **as a vector** to magnitude 1, scale ×31, truncate to s32 — vector
   clamp so diagonals don't distort (the old per-axis clamp stays only as a
   final belt-and-braces bound);
6. pack via the existing `arena_export_tick_input` — buttons untouched.

No native changes, no syms.ld changes, no sim changes. Neutral input
(`arena_input_pack(0,…)` for idle players) unchanged.

## 5. Phase 3 — verify + close out

- **Human boot gate:** cardinals + diagonals read screen-relative and
  uniform; throw/set/kick while moving aims as intended; ≥3 clean boots.
  Agent verifies via `capture-game.ps1` + `arena_bridge.log` (§8.8 loop).
- Remove the 1 Hz camera log (or leave behind `#if 0`) after the gate.
- Docs: integration notes gain §8.11 (gView semantics + measured arena
  camera geometry — groundwork for a future arena-camera slice); CLAUDE.md
  status updated; **then A1.3 gets spec'd immediately** (agreed sequencing).

## 6. Non-goals (deferred, with owner)

- Camera behavior/position changes (arena-framing camera) → own later slice.
- Movement speed/accel feel → **A1.3** (decomp transcription, next slice).
- Sim changes of any kind (pinned hash `4b6687d4` unchanged).
- Puppet-input rotation: players 1–3 receive neutral input today; the
  rotation applies at the single live-input site, so networked remote inputs
  (A2 path) are unaffected — each client rotates its own local stick before
  the sim sees it (this is the documented invariant: **rotation happens
  pre-pack, outside the sim**).

## 7. Build & testing

- Fork branch `feature/a1.2e-camera-input` off `feature/a1.2d-bomber-mesh`.
  Touches: `patches/arena_render.c` only.
- `make -C patches clean` before every cmake build (both `build-cmake` and
  `build-rwdi`; iterate on rwdi per §8.8 two-build discipline).
- Verification: build gate + phase-1 measurement boot + phase-3 feel gate.
- `bmhero-arena`: this spec + §8.11 + CLAUDE.md on completion.
