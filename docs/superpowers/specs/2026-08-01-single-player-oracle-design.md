# Single-player oracle — automating the feel-boot — design spec

**Date:** 2026-08-01 · **Status:** approved
**Purpose:** every feel-round bug so far (sunk bombs, wrong/looping pose,
slide-vs-detonate) was an *objective divergence from the real game*. Turn that
class into machine-checkable gates by probing vanilla single-player as the
reference oracle, so human boots become final sign-offs instead of the
debugging loop. Scope decision (user): catch mismatches automatically, human
still signs off — no attempt to automate aesthetics.

## 1. Exit criterion

`tools\oracle.ps1` (fork) boots instrumented vanilla single-player unattended,
extracts goldens into a checked-in file, and `tools\oracle-gate.ps1` asserts
the arena build matches them — wired into `build.ps1 -Soak`, so no build
reaches the human without oracle-gates green. A deliberately wrong arena
(`ARENA_SET_ANIM=42`) must FAIL the gate before any green is trusted.

## 2. Oracle probe mode (fork side, all additive)

Env knob `ARENA_ORACLE=1`:

- **Vanilla boot:** auto-start the game like `ARENA_AUTO_BATTLE` does but
  WITHOUT `arena_bridge_set_battle_mode(1)` — the arena takeover (spawn block,
  camera/HUD hijack, hazard suppression, bomb-pool sweep) keys on battle mode
  and never engages. Verify this holds; add explicit `arena_oracle_mode()`
  guards only where it doesn't.
- **Frontend mash reused** to reach gameplay, with a new stop condition
  (in-level / player object valid — `arena_routine_seen()` never fires in
  vanilla). If a 5-boot trial shows the mash cannot land in a playable level
  (file-select navigation etc.), FALL BACK to direct level-load (RE the stage
  entry the way the Battle option was built) instead of tuning mash timings.
- **Scripted input phases** (poll-counter based, same pattern as modes
  4/10/11): walk → stand → B (drop) → wait for detonation → hold-release B
  (throw at floor) → wait for impact → R (SET at feet) → walk off → run back
  in (KICK) → jump + R (AIR SET) → `[oracle] DONE`. (Amended 2026-08-01: the
  user found R = set, ground and air, and that running into a set bomb kicks
  it — Hero has native set/kick references after all.)

Instrumentation is logging only — zero behavior change, zero sim changes
(pinned hash untouched):

- `[oracle-anim]` — the existing `func_8001C0EC` patch logs player anim calls
  (idx + frame): the real anim sequence around each bomb verb.
- `[oracle-bomb]` — per-frame pos/state of game bomb slots `gObjects[2..6)`:
  rest Y, throw arc, the frame it explodes.
- `[oracle-blast]` — each `func_8007E76C` call, position + poll counter.
- Floor Y via the same geometry query the raster probe uses — rest height is
  stored as **bombY − floorY** (level-independent; measure the geometry, not
  the player).

## 3. Goldens — extract once, check in

`tools\oracle.ps1`: boot with the knob, wait for DONE, parse
`arena_bridge.log`, write `tools\oracle\goldens.json` (fork, checked in, with
build-hash + date provenance). v1 fields:

| field | meaning |
|---|---|
| `drop_anim_idx`, `drop_anim_frames` | clip + observed length on stationary drop |
| `throw_anim_idx`, `throw_anim_frames` | clip + length on throw |
| `bomb_rest_lift` | bombY − floorY at rest |
| `throw_impact_detonates` | explosion ≤ 5 polls after the arc's last airborne sample, AND total flight < 60 polls (a fuse-out would be ≥ 150) |
| `throw_flight_frames`, `throw_arc_peak` | flight envelope |
| `set_anim_idx`, `set_anim_frames` | clip + length on R-set (ground) — the arena set pose's true reference |
| `kick_anim_idx`, `kick_anim_frames` | clip + length on walk-in kick of a set bomb |
| `airset_anim_idx`, `airset_anim_frames` | clip + length on mid-air R-set (recorded; ungated until the arena has the verb) |
| `set_button_mask`, `kick_slide` | which N64 mask sets; whether the kicked bomb slides rather than detonating on contact |

Re-runs diff before overwriting (the `repin.ps1` pattern). The `no_oracle`
list (behaviors with no single-player reference, left to human judgment) is
down to aesthetics only — camera framing, explosion look, fun — after the
2026-08-01 R-set/kick discovery. Tooling note (empirical, Task 3):
`arena_bridge.log` is TRUNCATED per run, so oracle tooling reads the whole
file; and run-to-run world state varies, so extraction keys on phase markers
and state sequences, never absolute positions or object counts.

## 4. Arena gates against goldens

`tools\oracle-gate.ps1` reads goldens and asserts the ARENA's existing logs
match: set pose plays `set_anim_idx` for `set_anim_frames` exactly once
(amended 2026-08-01 — the R-set clip is the arena set's true reference, not
the B-drop clip); kick pose plays `kick_anim_idx` (mode-10 walk-in probe);
bomb render lift == `bomb_rest_lift`; mode-11 throw shows
`[throw]→[blastvis]` inside the flight envelope (±25 % band), never the
150-tick fuse. Trap #1 satisfied by construction: expected values come from
the game, not from constants we chose. All timing goldens live in the
poll-counter clock on both sides (the ~45-tick poll/tick skew never crosses
the comparison).

If a golden disagrees with a current arena default (e.g. the oracle logs a
different drop clip than 29), the ARENA DEFAULT CHANGES to match the golden —
that is the point of the oracle. The gate is updated in the same commit.

## 5. Validation of the instrument (before trusting any golden)

1. First oracle run must reproduce a known truth: `bomb_rest_lift ≈ 30`
   (decomp constant, 69AA0.c:359). Instrument-vs-decomp disagreement ⇒
   suspect the instrument.
2. Falsifiability: run the gate against a deliberately wrong arena
   (`ARENA_SET_ANIM=42`) and confirm it FAILS.
3. Mash timeout ⇒ `oracle.ps1` exits loudly with the last log marker seen —
   that is the fallback trigger, not a retry loop.

## 6. Non-goals / what stays human

- Kick-pose aesthetics, camera framing, whether the explosion *reads* right,
  fun. (Hero has no kick and no classic set — no oracle exists for them.)
- No visual/screenshot diffing between modes (camera and framing differ by
  design).
- No canonical-repo changes: no sim edits, no TUNE_VERSION/hash implications.
  This spec lives here (specs convention); implementation is fork-only.
