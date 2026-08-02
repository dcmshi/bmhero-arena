# Project log — milestone history (append-only archive)

Chronological record of completed milestones, moved out of `CLAUDE.md` on
2026-07-29 to keep the always-loaded context file short. **This file is history,
not current state.** For current state read `CLAUDE.md`, then the newest
`docs/HANDOFF-*.md`.

Entries are preserved verbatim from the CLAUDE.md they were written in, newest
first, so some contain notes that were later superseded or corrected — those
corrections are recorded in later entries and in the integration notes. When an
entry disagrees with `docs/bmhero-recomp-integration-notes.md`, the notes win.

---

## Feel rounds 9–11 + oracle 2.0 (2026-08-01 → 2026-08-02)

> Superseded-by-nothing as of writing; current state lives in `CLAUDE.md` +
> `docs/HANDOFF-2026-08-02.md`. RE detail: §8.32–§8.35.

Three user feel rounds, ten reported bugs, ten fixes — **zero sim changes**
(v18 `04e8af49` pinned throughout; everything was render/bridge parity). The
big finds: the walker's own CARRY is structurally broken in battle (its pool
bomb dies to the per-frame sweep — the bridge now drives carry/windup/
charge-run/carry-jump/throw/air-toss clips from vanilla goldens); the walker
only re-asserts anims on its own state transitions (three separate bugs);
player 0's Pos.y is sim-owned ALWAYS (the walker's ground scan stood on
door-class bomb actors at floor+210 — and `damageState` blinds the push scan
but not the ground scan); vanilla gives NO standing-on-bombs support
(measured — the user's contrary expectation is a pending design call).

Then **oracle 2.0** (spec + plan + 3-task subagent execution, adversarial
reviews with synthetic fixtures): choreography for both scripted boots moved
into tick-unit verb tables (`verb_script.h`, one ×2 poll conversion site —
the 2:1 clock had cost three separate bugs), and gate check 17 now diffs the
arena's ENTIRE per-verb anim timeline against vanilla's
(`tools\anim-diff.ps1`, `timelines.json`), with a known-divergences register
that goes red when a mute goes stale. Falsifiability proven on the real gate.
The differ caught **three real bugs on its first run** (post-set idle clip
41-vs-0, jump-ascent clip timing, missing post-toss recovery [30,10]+[8,6])
— exactly the "less things to manually check off for qa" the user asked for.
oracle-gate: 4 checks → 17. Both repos pushed at `ebbb041` / `4e8ad23`.

## Current status (2026-07-27)

> **RESUMING? READ `docs/HANDOFF-2026-07-27.md` FIRST.** It lists the open items
> in priority order, the traps that cost time, and the one-time setup already
> done. It supersedes `HANDOFF-2026-07-26.md`, which is kept only for the
> A1.2g/A1.5 detail. RE detail is in integration notes **§8.14** (camera + RE
> tooling), **§8.15** (floor measurement + the anchor bug), **§8.22** (action
> poses).
>
> **State:** canonical `main` and fork `master` both pushed and green.
> `TUNE_VERSION` **14**, hash **`ff22fa4b`**. **A1.2g and A1.5 are complete.**
> The next thing is a feel-boot of everything since v11 — none of it is
> feel-verified.

**ACTION POSES FIXED — set **41** (was 29), kick **32** (2026-07-27, §8.22).**
Q played a throw because 29 was derived from the state machine and never looked
at: the harness gate only asserted *"index 29 is playing"*, which is true no
matter what 29 DRAWS. ***A gate that asserts your own assumption cannot fail*** —
it stayed green through three feel tests reporting the wrong animation. Settled by
**rendering the asset table**: `tools/anim-contactsheet.ps1` (fork) sweeps all 53
entries of `D_80115808` and saves 106 labelled PNGs named from the game's own
`[animsweep]` report; the user identified set=41/42, kick=32/33 by eye in a minute.
**§8.5c's "kick has NO player animation (definitive)" is WRONG** — the reasoning
was sound (the walk-in kick IS 100% bomb-side; `69AA0.c` has zero anim calls) but
it leapt from *no code path plays one* to *none exists*. ***A call-graph search
answers "is this used?", never "does this exist?"*** — for an ASSET, enumerate the
table and look. Both edges are pure reads of sim state (set = bomb `FREE→SETTLED`
w/ `owner`; kick = `SETTLED→SLIDING` w/ `bounced-1`), so **no sim change, hash
`ff22fa4b` holds**; `test_bomb_mechanics` now pins the `bounced == kicker+1`
encoding the bridge depends on. One shared 24-frame pose window
(`arena_export_pose_anim` → index or -1) replaces the set-only path — every action
pose must be HELD anyway (§8.18). Gated on the shipped binary: `-AnimProbe` →
`[animw] +12 idx=41`, new `-Mode 10` → `[kick] pose idx=32`, 10/10 soak, CI green.
*Open:* both poses are **STATIC** — holding means re-triggering, which restarts the
clip (unchanged from §8.18; needs the game's own action state engaged).

**A1.2g KEYSTONE DONE — sim geometry re-matched to the DIRECTLY MEASURED floor
(`TUNE_VERSION` 7 → 8, hash `07fc6ade` → `4eacdd02`, 2026-07-26).** New **probe
mode 7** measures the floor by asking the game's OWN ground query
(`func_80078168`) on a grid, instead of walking a player and logging where it
stopped — a walk measures *how far the player could go*, not *where the floor
is*, and stalls at the first edge. One run, ~1s, no player movement. Result at
level 15: **a filled 1900×1900 SQUARE** centred on Hero (0,0), flat at y=240, no
holes or pillars; 50u and 10u grids agree exactly. So `half_x = half_z =
Q(7.9167)` — **square**, not the v5 rectangle, whose "1900×900" was walk-derived
with the player stopped by *the sim's own z wall* (the measurement confirmed the
bound it was meant to check).
**The ANCHOR was the bigger bug (§8.15):** the render frame was pinned to the
player's spawn, putting the sim's arena centre at Hero **(906,422)** instead of
(0,0) — the sim's x range mapped to **[-42,1854]** against a floor of
[-950,950], so **over half the sim arena hung off the map**. That is the A1.2g
fall. Now `hero = FLOOR_CENTRE + sim*scale` from measured constants, which also
kills the per-run capture drift (`origin.z` was 0 one boot, 396 another).
**§8.5a is now a TEST:** `tools/test_arena_cam.c` includes the *sim's*
`arena_geom.h` and asserts sim-extent × scale == measured floor (it caught a
stale submodule pin immediately). The floor guard now **warns** if it ever fires.
Verified: guard never fires across a full arena sweep (new `-Absent` gate); sweep
reaches |x|≤926, |z|≤786 all at ground height 240; 5/5 soak; gate green.
`test_bomb_mechanics`'s fixed-arc test was **not isolating what it claimed** —
spawn facing is "look at the arena centre", so with only 3 turn ticks the release
tick was still TURNING and the test measured facing, not the arc. It now holds
until the yaw settles and asserts both preconditions; the property does hold.

**A1.2g HUD DONE - by REUSING Hero's own, not an RmlUi overlay (section 8.21).**
The art, layout and draw already exist and look native; they only lacked our
numbers. Driven every frame, battle only: `gHealthCount` <- sim HP,
`gBombCount`/`gFireCount` <- 3 (these count POWERUPS and the HUD draws count+1, so
they render as **4** - the max), `gGemCount` <- 0, `gScore` <- 0.
`TUNE_START_HP` 2 -> **4** (v11, hash `3d2f2f0e` -> `ac74a6a5`) because the game
HARD-CODES `gMaxHealth = 4`, so sim HP maps 1:1 with no scaling or half-bars.
Non-battle is untouched - these are the campaign's own counters.
Verified on screen: 4 lit bars, score 00000, gems 00, bomb 4, fire 4, player clear
of the corner pad. **A1.2g is complete.**
*Open:* zeroing the score only CENSORS it to 00000 - hiding it, or repurposing it
per player, means touching the HUD DRAW (exports for `stocks_won` / phase are in
place, unused). A true per-player HUD is where an overlay would earn its keep,
since Hero's HUD is single-player by construction.

**A1.2g HAZARDS SUPPRESSED (2026-07-27, section 8.20).** The Nitros room's damage
tiles no longer affect the player. Traced the chain end to end - `func_80086AD0`
(76640.c:714) sets `D_8016E080` from the surface type (our corners are 0xF7 -> 1)
-> the case 5/6 block in `func_80024744` turns that into a damage request ->
applied unless **`gDebugInvincibileFlag`** (21E10.c:670). One named flag
suppresses the whole class rather than a patch per hazard; set every frame in
battle and CLEARED outside it. In battle the SIM owns every hit, so room damage is
wrong by definition - and it was a crash route, since the bypassed death path
crashes.
***Verified, not assumed:*** logging `D_8016E080` shows the tile still DETECTED
while its damage is suppressed - a sweep parks the player ON a corner tile and
reports `hazard=1` on 22/29 samples with no damage, stun, crash or level change.
**EXIT TRIGGER - not reproducible:** the full surface-type raster finds no
transition surface type anywhere on the floor, two sweeps keep `gCurrentLevel` at
15, and non-actor objects are already swept. The old stage-select ending was
almost certainly the ANCHOR bug driving the player to Hero x=1854, off the map.
Recorded as not-reproducible, not fixed - no change was made for it.
**A1.2g remaining: the HUD** (RmlUi overlay, not a patch of Hero's HUD).

**FEEL-TEST FIXES - `TUNE_VERSION` 8 -> 9, hash `4eacdd02` -> `7a6226c5`
(2026-07-27).** The first real feel test produced four reports; three were bugs
(section 8.19). **Turn rate 6 deg/frame CONFIRMED good** - that decision is settled.
**(a) W/S were INVERTED** - the recomp maps W to `Y_AXIS_POS` (positive) while the
sim wants forward NEGATIVE (`tune_probes.c`: *"sy MUST be -31"*). Negated in the
ADAPTER, not the sim. Facing verified afterwards, not assumed.
**(b) PERMANENT FREEZE** - `PHASE_ROUND_END` was terminal (counted down, then did
nothing) and `gameplay` gates input off there; with no respawn either, dying to
your own bomb OR killing the idle puppets froze the player for good. Rounds now
restart. ***A frozen sim is still deterministic, which is why the determinism
suite could never catch it*** - `tests/test_round.c` does, and was verified to
FAIL against the pre-fix sim.
**(c) TURN DRIFT - the user was right and I was wrong.** I called it "by design";
measured against the GAME'S OWN `moveAngle` (probe mode 9), the real walker SNAPS
180->0 in ONE frame on a stop-then-reverse where our sim swept for 30. The decomp
had said so all along (*"states 5/6/29/34 snap instantly"*). New
`TUNE_TURN_SNAP_SPEED`; the moving turn is untouched (`turn180_ticks` 30,
`turn_radius` 1.349), `ramp_distance` 0.937 -> 0.822.
**(d) Corner pads are NOT damage tiles** - 45 s idle on one: no damage, no death.
They are the room's spawn pads; the visible teleport is our drive engaging after
the draw-gate warmup (cosmetic, during the countdown).

**A1.5 FIXED ARENA CAMERA — DONE (2026-07-27).** `ARENA_CAM_DIST` **2800** frames
the whole 1900x1900 arena, centred, margin on every side. The camera code never
needed fixing: **`tools/capture-game.ps1` was capturing only the TOP-LEFT QUARTER
of the frame** (backbuffer is 1600x900; `GetClientRect` reports 800x450 to a
non-DPI-aware process, so PrintWindow blitted a quarter unscaled). A centred arena
drifts out of that crop as the camera pulls back — exactly the "floor hugs the
corner and shrinks" symptom. One-line fix (`SetProcessDPIAware()`); §8.17.
It cost **three wrong root causes** (at-overwrite, far clip plane, chunk culling),
all real mechanisms, correctly RE'd, all irrelevant. **RenderDoc settled it in one
capture**: clip-space depth `w in [2006.6, 2966.6]` vs `[2011.6, 2961.6]` predicted
for the measured floor — so the drawn floor IS the collision floor and the camera
WAS where we set it. New `tools/rd-capture.ps1` (+ `rd_*.py`) captures a frame with
no keyboard, via qrenderdoc's embedded Python over target control.
***Lesson: a measuring instrument that has never been checked against an
independent source is a hypothesis, not evidence.*** When a model and an
instrument disagree repeatedly, suspect the instrument.

**A1.4 ANIM GATE — GREEN; it was NEVER the camera (2026-07-27).** The red gate had
been attributed to A1.5 by an A/B run across **two builds**. New `ARENA_CAM_OFF=1`
runtime toggle lets the A/B run on **one binary**: the gate failed **identically**
with the camera on and off. Real mechanism, from a new `[animw]` per-frame window:
`func_80024744` (the walker) runs BEFORE our anim block and re-asserts its own
animation **every frame**, so a one-shot trigger survives exactly one frame —
camera on or off, standing or moving (this **corrects §8.5c**, which claimed the
pose holds while standing). Fixed by **holding**: a 24-frame window re-asserts the
pose whenever the walker takes it. Also **the gate was asserting the impossible** —
`-Rising` demanded an advancing frame counter, but holding requires re-triggering,
which restarts the anim and pins the counter at 0 forever. `-AnimProbe` now asserts
`[animw] +12 idx=29` (still showing 12 frames after the edge), which is the gate's
actual intent. Gate green both ways; 5/5 soak. §8.18.
***Still open:*** the pose is STATIC (held, not animated). Real animation needs the
game's own set action state engaged so the walker plays it itself.

**A1.5 FIXED CAMERA — works, on `feature/a1.5-fixed-camera` (fork), NOT merged
(2026-07-26).** The Nitros rail camera swung yaw 58→178° and sat at pitch **20°**
(`sin(20)=0.34`, so W/S read at a THIRD of A/D). Because the game rotates the
stick by `gView.rot.y`, that corrupted **input** as well as the picture — a held
direction curved, which is what made feel-testing untrustworthy. Now a static
**pitch 60 / yaw 0** pose, pitch+yaw provably constant across 22 samples/run;
5/5 soak; sim untouched. Two things measurement forced: the pose must be
**stamped twice per frame** (the game's camera update runs inside
`func_80024744` and reverts it), and we must write **`eye`/`up` ourselves**
(`func_8001994C`'s `D_8016E134` gate is closed here — caught because the picture
was pixel-identical across a 2× `ARENA_CAM_DIST` change).
**SUPERSEDED (2026-07-27):** the "framing is not finished" note above is obsolete
— framing is done at `DIST` 2800, and `at` now anchors on the *measured* floor
centre. The "pixel-identical across a 2× DIST change" evidence was itself an
artifact of the cropped-screenshot bug (§8.17); writing `eye`/`up` ourselves is
kept because it works and is self-consistent, not because the gate was proven
closed.

**RE TOOLING — `tools/decomp-func.ps1` gives typed C for ANY undecompiled
function in one command** (splat → `m2ctx` → m2c). Beats reading
`RecompiledFuncs` machine-C: real type inference against the decomp's own
headers. Validated against ground truth (`func_80281E50` → `sp1C = 4.0f`,
matching the hand-derived `0x40800000`). It immediately corrected the A1.5
design. **Note:** the decomp does NOT have `code_extra_0` decompiled — 182
`GLOBAL_ASM` stubs — so the earlier hand-RE was justified; m2c just decompiles
them on demand now. §8.14.

**A1.2g — the fall is NOT the death path (diagnosis corrects a standing note;
SUPERSEDED above — root cause was the render ANCHOR, now fixed).**
`actionState` stays **4** while `Pos.y` jumps to 30000: that is the ground
query's "no floor here" sentinel, i.e. the player walked off the floor POLYGON
because the real floor is smaller than the sim's collidable bounds (**§8.5a
violation**, not a hazard-object problem). A **floor guard** (containment, not a
fix) stops the visible fall. The real fix — re-matching sim geometry to the
measured floor — is **the keystone open item**; it also gives the camera its
true centre. Exit trigger / damage tiles / HUD are untouched.

**KNOWN RED — RESOLVED 2026-07-27, and the attribution was WRONG.** The A/B that
blamed the camera (off → PASS 2/2, on → FAIL 3/3) ran across two builds; on a
single binary with `ARENA_CAM_OFF` the gate fails identically either way. Cause
was the walker re-asserting its anim every frame, plus a gate asserting an
unachievable frame counter. See the A1.4 paragraph above and §8.18.

**TURN RATE TUNED — `TUNE_VERSION` 6 → 7, hash `18fbf1bb` → `07fc6ade`
(2026-07-26).** `TUNE_TURN_RATE` `0x02D8` (4.0°/frame, the authentic
`code_extra_0` walker) → **`0x0444` (6.0°/frame)**. First tune chosen from
**measured numbers rather than a feel-boot**: at 4° a 180 at top speed sweeps a
**2.06u radius against an arena whose short half-width is 3.87u — 53% of it**,
so you could not turn around mid-field without eating a wall. 6° cuts that to
1.35u (35%) and a 180 to 30 ticks (0.50s), keeping the turn visibly gradual
(A1.3's whole point). Metrics delta: `turn180_ticks 45→30`, `turn90_ticks
23→15`, `turn_radius 2.063→1.349`; nothing else moved. Deterministic at
`-O0/-O2/-O3`; full gate green; **9/9 ctest**; **all 6 CI legs agree on
`07fc6ade`**.
**Guarded by new arena-fit tests** in `tests/test_tune_report.c` — turn radius
must stay under half the arena's short half-width, and a 180 must land in the
10–40 tick band (gradual, but dodgeable). These **fail at 4°/frame**, so the
decision is encoded in tests rather than a comment.

**SHIPPED TO BOTH DEFAULT BRANCHES (2026-07-27).** Fork `master` @ **`7302005`**
carries A1.5 + the A1.2g geometry/anchor fix, submodule @ `50e5dad`
(TUNE_VERSION 8, hash `4eacdd02`). Verified on the merged tree: full patch
rebuild + 5/5 soak + anim gate + cam tests. `feature/a1.5-fixed-camera` is
retired — **work from `master`**. (Earlier note, 2026-07-26, below.)

**SHIPPED TO BOTH DEFAULT BRANCHES (2026-07-26).** Canonical `main` @ `73402a3`
(both workflows green). **Fork `master` @ `cacaaa4`** — note the fork's default
is `master`, not `main`. Master had been stranded at **A1.2a**; it now carries
**A1.2b → A1.4 + the build/soak tooling (37 commits)** with the submodule at
`73402a3` (v7). Verified on that exact merged tree: full patch rebuild (`make
clean`) + **5/5 boot soak green**. The per-slice feature branches are retired as
the working convention — merge to `master` from here.
***Human feel-boot on the 6°/frame rate is the one thing still pending*** — the
value was chosen from measured numbers, so it wants a subjective confirm.

**CI now runs every unit test.** Gaps closed: `test_movement` was **not in
CMakeLists at all** (so `ctest` silently skipped it), and `test_bomb_mechanics`
ran only under netcode's 2 OSes, never the 6-leg cross-arch matrix. Both fixed.
Also fixed two **pre-existing** breakages this surfaced: `tools/viewer/
viewer_draw.c` never got updated when arena 0 went rectangular in v5 (still
referenced `half_extent` and mis-indexed the geom registry — invisible to CI
because runners have no SDL3, so the viewer target is skipped), and
`tests/run_p2p_test.sh` used a `mktemp -d` that MSYS2 bash can create but not
write into, making local `ctest` permanently red.

**Tuning-loop toolkit shipped — the build/verify loop is now scripted end to end
(2026-07-26).**
Spec `docs/superpowers/specs/2026-07-26-tuning-loop-toolkit-design.md`, plan
`docs/superpowers/plans/2026-07-26-tuning-loop-toolkit.md`. Canonical branch
`feature/tuning-loop-toolkit`; fork branch `feature/build-and-soak-tooling`.

**The new loop** (replaces the manual dance):

```
tools\tune-report.ps1 -Compare base,friction=0.020,friction=0.045   # pick a value from numbers
<edit arena_tuning.h + bump TUNE_VERSION>
tools\gate.ps1                                                      # everything CI runs, one command
tools\repin.ps1                                                     # rewrite both pins, shows the diff
<commit, push, bump submodule in the fork>
.\build.ps1 -Config rwdi -Soak 5                                    # fork: PATH+patches+build+soak
```

- **`tools/tune_probes.c` + `tune_report.c`** — objective feel metrics: top
  speed/ramp, **stop distance+time** (the friction knob), **180/90 turn time +
  radius** (the turn knob), jump + running-jump arcs, arena traverse (the
  60-vs-30 Hz question). Probes recentre player 0 (spawn 0 sits against two
  walls; a 180 at top speed sweeps ~6.8u and would measure the wall) and use
  `num_players=2` (with 1, the liveness check ends the round on tick 1).
- **`tools/tune-report.ps1`** — variant sweep, `knob=value` (`friction`,
  `accel`, `top`, `air`, `gravity`, `jump`, `turn`); `-SelfTest` asserts
  cross-tune monotonicity. Enabled by `#ifndef` guards now wrapping every
  `TUNE_*` define.
- **`tools/tune_metrics.baseline`** — pinned in CI beside the hash. The hash
  proves *something* changed; this shows *what* (`stop_distance 0.308 -> 0.183`).
- **`tools/pinned_hash.txt`** now holds `<TUNE_VERSION> <hash>`, so CI and
  `repin.ps1` distinguish an intentional tune from an **invariant-#4 violation**
  (hash or metrics moved, version didn't) — `repin.ps1` refuses in that case.
  The generator moved out of the workflow heredoc into `tools/arena_hash.c`.
- **`tools/gate.ps1`** — all four suites + hash + metrics with CI's exact flags.
- **Fork `build.ps1`** — composes the LLVM15/VS/MSYS2 PATH, rebuilds stale
  patches (`make clean`, with `CC=clang LD=ld.lld` — a bare `make` picks up
  MSYS2 gcc and rejects every MIPS flag), builds, and `-Soak N` fails the build
  unless the soak is green.
- **`arena-soak.ps1`** — generic `-Expect '<regex>'` / `-Rising '<regex>'` /
  `-Mode <n>`; `-AnimProbe` is now an alias. New probes need no harness edit.

**Debugging utils (same slice):**
- **`tools/arena_trace.c` + `tools/trace-diff.ps1`** — per-tick CSV of the SAME
  scripted match as `arena_hash.c`, and a differ that reports the **first
  diverging tick and which field moved**. Answers "the hash changed — where?",
  which the pin alone can't. Verified: `base` vs `friction=0.020` → *"tick 731,
  p1_x/p1_vx"*. `-OptA -O0 -OptB -O3` on the same tuning is a **determinism
  hunt** — divergence there is an invariant #1/#2 bug, not a tune (currently
  bit-identical over 1200 ticks of full state). `gate.ps1` guards the two files
  from drifting apart (trace's final hash must equal `arena_hash`'s).
- **`tools/conv.ps1`** — the unit translation this project constantly does by
  hand: Q20.12 ↔ sim units ↔ **Hero units (×120, `arena_bridge.cpp:23`)** ↔
  game u/frame (S=1/119) ↔ BAM/degrees ↔ ticks/seconds. `-Table` decodes the
  whole tuning table. Cross-checks: `-Deg 4` → BAM `728` = `TUNE_TURN_RATE`,
  and its "180° = 45 ticks" matches the probe's measured `turn180_ticks`.
- **Fork `tools/arena-log.ps1`** — summarise/filter `arena_bridge.log` instead
  of eyeballing 30k. Default view gives marker counts, the `[capture]` draw-gate
  line (absent = bridge never armed = soak HANG), and per-anim-index **"advanced
  (played)" vs "STATIC (set but never advanced)"** — it independently flags the
  known A1.4 hold-while-moving artifact. `-Marker/-Grep/-Tail/-Follow`.

Gotcha worth remembering: **MSYS2 gcc invoked by absolute path fails silently
(exit 1, no diagnostic) unless its own `bin` is on PATH** — all the PS scripts
prepend it.

## Current status (2026-07-24)

**SESSION WRAP-UP (2026-07-24, late) — A1.4 + the A1.3/movement feel pass shipped;
user feel-tested "better", remaining items are polish or Nitros-stand-in
artifacts.** Full detail in the dated paragraphs below, integration notes
8.5a/8.5b/8.5c/8.13, and docs/bmhero-player-movement-re.md. All pushed: canonical
`main` @ hash `18fbf1bb` / `TUNE_VERSION` 6 (CI green); fork
`feature/a1.4-set-kick-anims` @ `00be604`.

DONE this session:
- **A1.4 set-bomb animation** - player 0 plays the game's own set/drop pose
  (`func_8001C0EC(0,0,29,1,&D_80115808)`, code_extra_0 anim 29) on the sim set
  edge. RE'd zero-boot from `RecompiledFuncs` (the machine-C of the un-migrated
  overlays - the key methodology unlock this session; §8.5c). **Kick has NO game
  anim** (offense is grab/throw; walk-in kick is bomb-side only) -> locomotion
  kept. The set pose is fragile WHILE MOVING (the game walker re-asserts
  locomotion each frame) - holds when standing; hold-while-moving is **DEFERRED**
  (fights the walker; the bomb visibly appears so the set is functional).
- **Bomb rendering fixed** (§8.13) - set bombs were placed but invisible: every
  bomb+blast actor was spawn-then-ACTION_NONE'd, so func_80027464 REUSED the slot
  and all piled into one gObjects slot which the blast loop then hid. Un-piled
  into distinct slots + dropped the fallback blast actors (freed pool budget) +
  BOMB_POOL 6->4 to stay under the ~8-actor model-pool ceiling. Screenshot-
  confirmed a bomb draws.
- **A1.3 walker constants applied** (turn 4deg/frame, top 18, accel 1.5, air 1.0)
  recovered from `RecompiledFuncs`; **friction decoupled** to Q(0.030) (v6) for a
  snappier stop (feel-tested "better", less slide).
- **Rectangular arena** matched to the measured Nitros floor + **map registry**
  (arena_geom.h: `arena_nitros_standin`[0] used, `arena_classic`[1] deferred).
  Principle recorded: the sim's collidable bounds MUST track the rendered map
  (§8.5a).
- **Absolute-drive player 0** (§8.13) - replaced the A1.2a delta drive that
  double-drove the player (game walker + sim delta) and jammed the sim against
  its walls -> mid-floor slowdowns. Sim now solely owns X/Z (like the puppets),
  origin captured early (~30-frame draw-gate, not the drifting 90-frame gate).
- **CI green** - the netcode bomb test was recalibrated to be geometry/movement-
  independent (explicit setups).

KNOWN / stand-in artifacts (Nitros is a RENDER stand-in, not the real arena):
- **Camera drift** - the Nitros rail camera swings; input is camera-relative
  (game rotates the stick by camera yaw, §8.11) so movement curves as it orbits.
- **Foreshortening** - W/S (toward/away from camera) reads slower than A/D
  (perspective). Both go away with a real fixed arena camera.
- Puppets (players 1-3) are red bomb-mesh placeholders (real bomber mesh deferred,
  §8.5b).

NEXT STEPS (rough order):
1. **A1.2g arena hardening** - exit trigger + damage tiles (probe found them live
   in the Nitros room; the bypassed death path crashes) + **HUD**.
2. **Explosion visual** - reuse a bomb's own actor as its blast on detonation
   (stays under the ~8 model-pool ceiling that forced dropping the blast actors).
3. **Set-pose hold-while-moving** - revisit with a cleaner approach (engage the
   game's set state, or frame save/restore) if wanted.
4. **Real battle arena** - build/render a map matching the sim's designed arena;
   then `arena_classic` geom + a fixed camera resolve the drift/foreshortening/
   pillars together.
5. **Feel knobs still open** - turn rate (4deg authentic; could speed up for
   arena), friction (Q(0.030) first cut), the 60-vs-30 Hz question.

**A1.4 set-bomb animation complete — fork-side, sim untouched, 5/5 soak green
(2026-07-24).** Player 0 now plays the game's OWN set/drop-bomb pose when the
sim registers a set: `func_8001C0EC(0, 0, 29, 1, &D_80115808)` on
`gPlayerObject` (code_extra_0 anim **29**, bank 1, table `D_80115808` — the exact
trigger form the fork already proved in `patches/teleporter_obj.c`, anim 7). The
whole player action-anim map was RE'd **zero-boot from `RecompiledFuncs`** (the
recomp's machine-C of the un-migrated `code_extra_0` walker — the key unlock;
corrected an earlier `code_extra_1`/idx-11 misread). **Kick has NO game
animation** — Bomberman Hero offense is grab→throw; the walk-in kick is 100%
bomb-side (`69AA0.c` never animates the player), so the puppet keeps locomotion
during a slide (user chose "no kick pose"; §8.5c). The set **edge** is detected
natively in the bridge (bomb `FREE→SETTLED`, `owner==i`, mirroring
`arena_blast_new`) — pure read of sim state, **no gameplay change, pinned hash
`5f500fcb` holds**. **Auto-verified via the harness** (user requirement, no
eyeballing): read-back getters `func_8001B880`/`func_8001B62C` →
`arena_dbg_anim` burst-log; probe mode `ARENA_AUTO_BATTLE=4` presses Z after the
sim's 180-tick countdown (set only fires in `PHASE_PLAY`); `arena-soak.ps1
-AnimProbe` asserts `[anim] idx=29` with the frame counter advancing. Gate PASS
(idx 29, frame 0→14; bombs live=2/3), **5/5 boot-soak green**; the pose plays out
(walker doesn't stomp it). Fork branch `feature/a1.4-set-kick-anims` (pushed).
Human feel-boot pending as the final polish confirm. Next: A1.2g arena hardening
(exit trigger, damage tiles) + HUD.

**A1.3 GAP recovery — standard walker constants recovered AND APPLIED (2026-07-24;
`TUNE_VERSION` 3 -> 4, CI hash `5f500fcb` -> `582455c8`).** The A1.3 turn rate (the
one "unrecoverable" GAP, guessed ~12deg/frame) and the horizontal speed model were
pulled from `RecompiledFuncs` (same source as A1.4): the real standard walker
(`code_extra_0`) turns at **4.0deg/frame** (bounded), top speed **18** / accel **1.5**
flat-ground (vs the auto-runner-sourced 10 / 0.2 A1.3 shipped), air accel 1.0. Applied
to `arena_tuning.h` under the existing height-anchor scale S=1/119 (self-consistent with
the shipped jump/gravity): turn `0x02D8`, run-speed `Q(0.151)`, accel/friction
`Q(0.0126)`, air `Q(0.0084)`. Stick->target keeps continuous mag-scaling (approximates
the game's discrete 0/6/12/18 tiers). Verified: `test_determinism` + `test_movement`
green under `-Wextra -Werror`; new hash `582455c8` deterministic across `-O0/-O2/-O3`.
Full findings + conversion in `docs/bmhero-player-movement-re.md`. A1.3's prior feel was
user-confirmed on the OLD (auto-runner) values, so a fresh fork feel-test on these
authentic numbers is pending.

**Playtest fixes — arena geometry (2026-07-24 late).** The fork feel-test of the
new movement exposed that the sim arena (arena0: 12×12 ring + 4 pillars) never
matched the open Nitros render stand-in → invisible walls/pillars (the player was
pinned at a sim corner ±678 Hero out + blocked at 4 mid-floor boxes; principle
recorded §8.5a). **FIXED:** arena0 is now a **rectangular** geom matched to the
measured Nitros floor (~1900×900 Hero @ scale 120 → `half_x`≈7.9, `half_z`≈3.87
sim, **no pillars**, corner spawns); `collide_static` is per-axis. Intentional
gameplay change: `TUNE_VERSION` 4 → **5**, CI hash `582455c8` → **`77bae30e`**
(deterministic `-O0/-O2`; `test_det` + `test_movement` green `-Werror`). The
Nitros floor was measured with a mode-5 sweep probe (enlarge sim arena, log live
`gPlayerObject->Pos`). **STILL OPEN from the same playtest (fork-side, next):**
(a) set bombs place at the right spot but don't **render** — all bomb+blast actors
collapse into one `gObjects` slot (spawn-then-`ACTION_NONE` lets the game spawner
reuse it; §8 pool-ceiling caveat on the fix); (b) the set **anim** only twitches
while moving (the game walker re-asserts locomotion each frame). Fork feel-test of
the rectangular arena pending.

## Current status (2026-07-22)

**A1.3 movement dynamics complete — sim-only, no fork changes (2026-07-22
late).** The campaign player's free-walk physics (`docs/bmhero-player-movement-re.md`,
decomp-sourced from the auto-runner `code_extra_3` + shared `func_8002D080`/
vertical routines) are transcribed into `arena_sim.c`'s `player_tick`: **no-strafe
scalar-speed-along-facing** (a single `moveSpeed` accelerates/decelerates toward a
stick-magnitude target at one rate — accel == friction — then velocity is rebuilt
each tick as `speed × (sin(yaw), −cos(yaw))`, replacing the old per-axis
stick-vector accel) plus a **gradual bounded turn** toward the stick target
(replacing the instant `p->yaw = dir` snap; rate is the one empirical GAP — no
decomp source for the standard walker's turn, seeded ≈12°/frame). Vertical
physics are now the **real game constants** (jump impulse, gravity, and a new
terminal-velocity clamp the old sim lacked). This is an **intentional gameplay
change**: `TUNE_VERSION` 2 → **3**, and the CI-pinned scripted-match hash is
re-pinned `4b6687d4` → **`5f500fcb`** (regenerated with the exact CI hash
generator, confirmed deterministic across 6 local runs). New `tests/test_movement.c`
guards the model (turn-toward-target, scalar-speed accel/decel/clamp, terminal
velocity) and is now wired into `.github/workflows/determinism.yml` as its own
step on every matrix leg, right after the existing determinism-test step. No
`src/arena/*.c` logic beyond the transcription changed; no fork/render-bridge
work in this slice. Local gate green: `test_det` → `ALL TESTS PASSED`;
`test_movement` → `ALL MOVEMENT TESTS PASSED` (both also verified under
`-Wextra -Werror`, matching CI's flags exactly).

**A1.3 fork feel-test + facing SOLVED (2026-07-23).** The fork
(`feature/a1.3-feel`, submodule bumped to the merged A1.3 `ba7c346`) was
rebuilt and the new movement **user-confirmed good** — momentum, gradual
turn, no-strafe, and the bigger jump all feel right. **Facing** (the last
gap, carried from A1.2e) is fixed by copying the game's own value 1:1:
`gPlayerObject->Rot.y = gPlayerObject->moveAngle` (integration notes §8.11).
Deriving facing from sim yaw or dx/dz fought turn-lag + angle conventions
(read 90°/45° off, inconsistent); the game already computes the authentic
`moveAngle` from the camera-rotated stick each frame, so we borrow it (we
still drive position from the sim). Machine-verified via a game-truth probe
(logs the game's `moveAngle`/`Vel` vs our sim yaw/dx/dz) + green soaks on
every handoff. **Empirical knobs still open for a later feel pass:** the turn
rate (~12°/frame seed) and the 60-vs-30 Hz question — the user did NOT report
movement 2× fast, so 60 Hz stands for now. Puppet facing is a yaw-placeholder
(invisible on symmetric bomb-mesh placeholders; revisit with the real bomber
mesh). Next: player set/kick **animations** + feedback, **A1.2g** arena
hardening (exit trigger, damage tiles), HUD.

**A1.2e closed + A1.2f harness shipped — input direction is native-correct;
the load-crash class is fixed at its mechanism; boots are machine-verified
(2026-07-22 evening).** The headline discovery (§8.11): **the game already
rotates the stick by camera yaw natively** (`func_80024744`, camtype set
{1,2,5,6,7,8}; the arena is type 6) — our added gView rotation was
double-rotating (forward speed cut ~⅓); the raw pass-through IS the correct
camera-relative mapping. Facing: `Rot.y = 180 − sim_yaw`, derived from the
game's movement equations (§8.11). Set/Q was NEVER broken — user presses
reached the sim (`live=1..3`); ground bombs rendered BELOW the floor
(bomb/blast `wy` now floor-clamped). The stochastic crash class is finally
dead at the mechanism (§8.12): the runtime's func_map races indirect calls
during level transitions — the draw dispatcher now DIRECT-dispatches our
hook (`required_patches.c` `func_8001D9E4` patch); gating around the window
provably cannot work (level-enter pump runs 30–90+ routine frames; 0/10 →
10/10 soak on the fix). **A1.2f boot-soak harness** (`tools/arena-soak.ps1`
+ `ARENA_AUTO_BATTLE` env: auto-battle from the launcher update callback,
synthetic frontend mash, probe mode with in-level stick/button injection) —
~50 machine boots today; **rule: no build reaches the human without a green
soak on that exact build.** Probe collateral: the arena has a live
level-exit trigger (N) and damage tiles (corners) → hardening slice
(A1.2g). **User-confirmed:** movement direction correct. **Open punch list
(next slices):** A1.3 dynamics = speed/accel/turn/momentum from the decomp
(`Math_CalcAngleRotated`, `2BF00.c:480` player physics — THE feel gap);
set/kick player ANIMATIONS + feedback (puppet performs none of the game's
player anims); facing exactness polish; on-screen set-bomb visibility
confirm (machine-verified rendered; user still reports invisible —
re-verify after anims); arena hazards (A1.2g). Fork branch
`feature/a1.2e-camera-input` (pushed).

**A1.2d closed — bomber mesh deferred (anims not resident in the arena);
load-crash class fully dead (2026-07-22).** The real-bomber slice ended at its
decision gate with a complete RE map (§8.5b rewritten): the bomber MESH is
resident everywhere (`gFileArray[1]` cfg `0x13`, spawner-loadable) but its
ANIMATIONS are unreachable — the menu's `D_80115F34` stream table is garbage
in-level (file 1 byte-identical across maps; `func_800122F0` parses a ~395k
section count → endless `malloc_game` walk), the `gObjInfo` registry carries
no bomber entries in any warpable arena (per-level population), and cfg
`0x13`'s modelTag embeds no anims (`func_8001191C` AVs on the demo-style
null-source bind). Unposed skeletal draw white-screens (A1.2b), so puppets
stay bomb placeholders; a null-guarded gObjInfo candidate scan self-activates
the recipe wherever entries appear. **Documented lead for the next attempt:
player 0 animates in-arena ⇒ valid anim data IS resident via the player path —
start by tracing `gPlayerObject`'s anim-instance bind.** Big wins shipped
anyway: (1) the stochastic "black screen selecting battle" crash class is
**fully dead** — it had THREE racing `recomp_printf` load-window sites
(`required_patches.c` §8.9 + `3d_object_hook.c` + our `arena_warp.c`), all
disabled, loads verified stable; (2) new `arena_spawn_gate` native export
(spawn block deferred ~90 frames — the render routine's first invocations run
inside level-enter where the game heap isn't serviceable); (3) hard-won patch
rules recorded: null/non-KSEG0 derefs in patch code are host AVs;
`MAP_MIRROR_ROOM` (71) is a direct-warp land mine (`func_8001D9E4`).
Regression-verified: bombs/blasts/set/kick all still work. Sim untouched
(pinned hash `4b6687d4`). Fork branch `feature/a1.2d-bomber-mesh` (pushed).

**A1.2c complete — blasts render as pops; THE load crash fixed (2026-07-22).**
Slice 2 shipped the fallback blast visual: 4 pooled blast actors (bomb mesh,
proven recipe) appear at each live `blasts[]` center for the blast's 20-tick
life — detonations now give visible feedback at the right spot (spread → up to
4 pops). The game's effect spawner (`func_80081468`) was investigated and
**abandoned** for the bypassed arena (effects invisible; two IDs crash — §8.9).
**Two big wins from the debugging:** (1) the **stochastic "black screen
selecting battle" crash is fixed** — an upstream debug print in
`load_from_rom_to_addr` racing the loader (named by a symbolized RWDI dump);
it had contaminated earlier findings, so the "pool ceiling" result is invalid.
(2) **Symbolized-dump tooling** now exists (`build-rwdi` + `playrwdi.bat`) and
a source-level **patch-machinery reference**
(`docs/bmhero-recomp-patch-machinery-reference.md`) corrects earlier folklore
(data symbols resolve from `data_dump.toml`; Release `0xC0000409` = uncaught
exception; patch `.data/.bss` is memory-backed). Also: the generic `[14..77]`
draw **ignores `Scale`**; the boss re-activates if the sweep stops (every-frame
sweep stays). Follow-ups: real explosion visual (effect-asset RE), real bomber
mesh (§8.5b), re-test larger bomb pools. Fork branch
`feature/a1.2b-spawn-bombers`.

**A1.2c slice 1 complete — bombs render + set/kick wired (2026-07-21).** The
sim's live bombs are drawn in the arena: a pool of 6 bomb actors
(`=TUNE_MAX_LIVE_BOMBS`) spawned once (proven `func_80027464` + `func_8001ABF4`
recipe), toggled active/hidden each frame from `bombs[].state`, positioned from
the sim **including Y** (throw arc shows; new `g_ref_sy` capture). Verified: a
throw + the 4-bomb spread appear and arc on screen. Input now folds
jump|bomb|set into a packed `buttons` arg (the export ABI caps at 4 args) and
wires **set/kick = `CONT_G`** (Z / Q key); set is log-confirmed (Q → live-bomb
count 0→4) — set bombs spawn at the player's feet so they're visually subtle.
The boss-suppression sweep now spares all actor slots via `arena_is_actor_slot`.
Fork branch `feature/a1.2b-spawn-bombers`.
- **Pool ceiling (finding):** 6 bomb actors stable, **8+ crash at spawn** — the
  suppressed Nitros boss holds model/anim-pool slots that `actionState=NONE`
  doesn't free, leaving little headroom. Raising the sim bomb-cap (A1.3) needs
  the boss's slots actually freed first (integration notes §8).
- **Deferred to A1.2c slice 2 — blasts/explosions:** detonation currently = the
  bomb vanishes (no explosion effect). The visual payoff (explosion, fuse blink,
  event feedback) is the next slice; candidates `gFileArray[0xA/0xB/0xC]`,
  `blasts[]` growth via `radius_t`/`ttl`.

**A1.2b complete — 4 actors on screen with bomb placeholders (2026-07-21).**
The full simulated roster is puppeted in the arena: player 0 (the campaign
object, A1.2a) + **3 extra actors spawned into `gObjects[14..77]`** via the
game's own `func_80027464`, positioned from the sim, on a **clean flat arena**
(`MAP_NITROS_1`, boss suppressed). Stable, no mirror, no crash — confirmed on
screen. The "animated models can't draw via the general spawn" wall (was
BLOCKED) is broken. Actors are **bomb placeholders**; the real bomber mesh is a
scoped follow-up (skeletal model — §8.5b). Full RE detail in **integration
notes §8** (rewritten). Fork branch `feature/a1.2b-spawn-bombers`.
- **The unlock:** `func_80027464` loads model parts (`Unk140`) but not the
  animation instance an animated model needs; adding **`func_8001ABF4(slot,0,0,
  cfg)`** binds it (`Unk148`/`D_8016C298` pool) and the draw stops aborting.
- **Two hard patch gotchas found:** (1) auto-named DATA symbols (`D_801163DC`)
  don't resolve via the patch reloc path and silently corrupt the whole patch →
  pass their **address as a literal** `((T*)0x801163DC)`; (2) the export ABI
  can't take float ARGS → pass floats as **`u32` bit patterns** through int args
  (union bitcast). Both cost hours; §8.2.
- **Positioning fix:** the earlier positioning crash was the **Battle Room's
  pits** — actors at the sim corners land off-platform, where per-object
  collision (`func_8001CD20`, runs on all active `[14..77]` regardless of objID)
  aborts. Fixed by warping to a **flat arena** (`ARENA_WARP_MAP`=15) + a
  **boss-suppression sweep** (deactivate non-puppet `[14..77]` before the update
  loop). §8.5.
- **Real bomber mesh — follow-up (§8.5b):** swapping bomb (`gFileArray[9]`) →
  bomber (`gFileArray[1]`, cfg `0x13`) spawns but **white-screens on draw** —
  the player-bomber is a multi-part **skeletal** model (`4DFF0.c` loads it in a
  loop w/ per-part offsets); needs per-part skeletal binds + an idle pose. A
  scoped rendering sub-task. Inert objID also still TBD (door behaviour is
  harmless now that neighbours are suppressed).
- **Tooling:** `PrintWindow` screenshots (occlusion-proof), `arena_dbg_u32` →
  `arena_bridge.log` markers; hands-off keyboard input to the game is unreliable
  (SDL focus) — human does the ~15s room nav, agent builds + verifies (§8.8).
- **Bugfix (earlier pass):** idle players 1–3 were fed raw-`0` `ArenaInput`
  (decodes to `(−32,−32)`); neutral is `arena_input_pack(0,…)`=`0x820`. Fixed in
  `arena_bridge.cpp`; changes the non-battle proof-of-life hash, not the pinned
  CI sim hash. Static patches must stay **stateless** (patch-local mutable
  statics abort `0xC0000409`) — keep state native (`cdb` forensics, §1).

## Current status (2026-07-18)

**A0 complete.** Headless deterministic arena sim in `src/arena/`, all tests
green (`tests/test_determinism.c`): bit-identical replay, GekkoNet-style
rollback stress, snapshot round-trip, liveness. Scripted-match hash pinned at
`4b6687d4` (re-pinned 2026-07-19 with the bomb-mechanics correction —
`TUNE_VERSION` 2, first intentional gameplay change; previously `a55aa9b1`)
— CI matrix on GitHub: https://github.com/dcmshi/bmhero-arena.

**A1.2a complete (2026-07-20).** First render bridge on screen: in the Battle
Room the campaign player object (`gPlayerObject`) is puppeted from our
fixed-point sim, drawn by Hero. A `RECOMP_PATCH` on the 6-line level-enter
`func_800824A8` routes the per-frame `gDebugRoutine2` through
`arena_render_routine`, which reads the controller, ticks the sim via native
`arena_export_tick_input`, and moves the player **by the sim's per-frame
displacement** (dx/dz added to live Pos; **Y left to the game** so it stays
grounded and the camera follows — no teleport, no fragile spawn-capture).
Native coord exports in `arena_bridge` (syms.ld `0x8F000128+`); VI-callback
tick gated off in battle so the patch drives the tick. Verified: bomberman
moves under our physics on screen. **Known item:** forward/back reads
"compressed" (camera foreshortening); the right fix is camera-relative input
using the real `gView` transform — deferred to the feel pass (blind
scale-tuning went the wrong way, so measure, don't guess). Fork branch
`feature/a1.2a-puppet-player`. Build gotcha: after editing any `patches/*.c`,
`make clean` in `patches/` before the cmake build (ninja doesn't reliably
re-run the patch make — stale patch = mismatch crash). Next: A1.2b spawn +
puppet all 4 bombers.

**A1.1b-ii complete (2026-07-20).** Battle launch warps into `MAP_BATTLE_ROOM`
(Hero's own dedicated arena — loads cleanly on direct entry, no fallback
needed) instead of the campaign level. Mechanism: `arena_bridge_is_battle`
native export (`syms.ld` `0x8F000124` + `REGISTER_FUNC`) lets
`patches/arena_warp.c` `RECOMP_PATCH` `func_80081C50` (the level-load prep
that seeds the next-level var + spawn from `gCurrentLevel`, called from 9
level-transition handlers) override `gCurrentLevel` before the loader reads
it; non-battle launches unchanged. First `patches/` change since A1.0
(LLVM-15 MIPS). Verified by boot gate (Battle → Battle Room). Fork branch
`feature/a1.1b-ii-map-warp`.

**A1.1c dropped/deferred (2026-07-20, decided during brainstorming).** Two
findings collapsed it: (1) the Battle Room is already a clean versus arena —
**no campaign enemies to suppress** (only the campaign player object + HUD,
which are A1.2 render-bridge concerns); (2) the intro→file-select frontend
skip is deep, fragile RE — it lives inside `func_80083180`, a giant
"not reducible" goto state machine with no clean patch seam, and it's a UX
nicety, not a blocker. Deferred in favor of A1.2. Revisit the frontend-skip
later (lightweight input-injection or state-machine RE) if the friction
matters. Next: **A1.2 render bridge** — write `ArenaState` into `gObjects`
so Hero draws our 4 bombers in the Battle Room.

**A1.1b complete (2026-07-19).** The recomp launcher has a **Battle** option
(`on_launcher_init`, `main.cpp`) that sets a native battle-mode flag
(`arena_bridge_set_battle_mode`) then launches via `recomp::start_game` — same
path as Start Game, gated on `recomp::is_rom_valid`. The per-VI proof log
distinguishes battle (`[arena] BATTLE MODE tick N …`) from normal; confirmed
by boot gate (Battle → BATTLE MODE, still lands in single-player campaign as
expected — the destination warp is A1.1b-ii). Convenience `play.bat` in the
fork root launches from the repo root so `assets/` resolves (raw double-click
of the exe crashes `0xC0000409`). Fork branch `feature/a1.1b-battle-menu`.
The battle flag is the seam A1.1b-ii (warp to a dedicated arena map) and A1.2
(render bridge) build on. Next: A1.1b-ii forced map-shell warp.

**A1.1a complete (2026-07-19).** `bmhero-arena` is a submodule of the fork at
`lib/bmhero-arena`; `arena_sim.c` compiles natively into `BMHeroRecompiled`
(clang-cl), and a fork-owned `src/arena_bridge/` ticks a silent-passenger
`ArenaState` from the recomp's native VI callback (`main.cpp`), logging
`[arena] tick N hash H` to console + `arena_bridge.log` each second —
verified advancing (60/120/180…) and deterministic across runs
(tick 120 → `b6272ae5`). **No MIPS patch needed** — the native VI hook is the
design's tick edge, dissolving the spec's "which per-frame function to patch"
risk. Fork branch `feature/a1.1a-arena-proof`. Next: A1.1b Battle menu entry
(+ warp-into-map), A1.1c spawn suppression, A1.2 render bridge.

**A1.0 complete (2026-07-19).** Fork `dcmshi/BMHeroRecomp` builds locally and
boots the campaign (intro + level play confirmed) at
`C:\Users\dshi\GitRepos\BMHeroRecomp`. Repo decision: `bmhero-arena` stays
canonical; the fork will consume it as a **submodule** in A1.1. Build recipe
(the toolchain fight is documented so A1.1+ can rebuild):
- N64Recomp built from source with MSYS2 gcc → `N64Recomp.exe`/`RSPRecomp.exe`
  staged in the fork root (they need `C:\msys64\ucrt64\bin` on PATH at run
  time for their runtime DLLs).
- Main build: VS 2022 clang-cl + `cmake --build build-cmake --target
  BMHeroRecompiled` from a VS dev shell.
- **`patches/` sub-build needs LLVM 15** (upstream CI uses clang-15): VS's
  clang-19 has no MIPS backend; MSYS2's LLVM-22 lld rejects the old linker
  flags. Fix: portable LLVM 15.0.7 extracted (no install) to
  `C:\Users\dshi\GitRepos\.tools\llvm15`. Zero fork source changes.
- Full-build PATH: `…\.tools\llvm15\bin` + VS dev-shell PATH +
  `C:\msys64\ucrt64\bin` + `C:\msys64\usr\bin` (order matters: LLVM15 first
  so patches' `clang`/`ld.lld` = v15; MSVC `link.exe` before msys; `make`
  from usr/bin). Your ROM's sha1 is already the recompiler-input image.
Default keyboard controls (recomp): WASD stick, Space=A, LShift=B, Q=Z, E=L,
R=R, Enter=Start, arrows=C, IJKL=D-pad, Esc=recomp menu. Next: A1.1 mod
scaffold — arena submodule + Battle menu entry + map-shell load.

**A2 SyncSession complete (2026-07-19).** `src/netplay/` wraps GekkoNet
(FetchContent, pinned tag `v20260629200724-02c447c`, BSD-2) behind one C
interface — couch/online/stress — the session owns `ArenaState` and is the
sole `arena_tick` caller. Gates: GekkoNet stress session (continuous
rollback re-sim, 3600 ticks clean) + two-process localhost match with
matching confirmed hashes (`p2p tick 600 hash bbf9c071`), both in ctest and
the `netcode` CI job (ubuntu+windows). Viewer drives matches through the
session (couch default; `--host <port> --peer <addr>` / `--join <addr>
--port <p>` for 2P online; `--frames` smoke stays sessionless, hash
`eeeb76f6`). Spec: `docs/superpowers/specs/2026-07-19-a2-syncsession-design.md`.

**Bomb mechanics are Hero-authentic (2026-07-19).** Fixed-arc throw (no
stick/momentum modifier — decomp-verified in `bmhero src/code/69AA0.c`:
speed 35 / pitch 80° / facing only; kick = flat launch at speed 30), 4-bomb
spread on ≥2s hold, set via input bit 14 (works mid-air), **walk-in kick**
(run into any settled bomb; setter immune until stepped clear;
`BSTATE_SLIDING`; detonates on first contact — owner-recalled, verify in
A1). Cap 6 live bombs. TDD'd in `tests/test_bomb_mechanics.c`; design doc
§2 records mechanics, sources, and decomp anchors for A1 calibration.

**SDL debug viewer complete (2026-07-19).** `tools/viewer/` (floats OK there;
spec + post-playtest addendum in `docs/superpowers/specs/`): camera modes on
F1 — FOLLOW default (fixed yaw; **verified: Hero's real camera never rotates
with facing**, see design doc §7 note), CHASE, ORBIT (battle-mode preview),
TOP — pause/step/slow-mo, HUD with live hash, checkerboard ground, translucent
walls, F2 sudden-death toggle (viewer-side; sim untouched), `--frames N`
deterministic smoke flag. Pure modules unit-tested (`tests/test_viewer_*.c`).
Keyboard playtested; **gamepad path written but not yet device-tested** (no
pad on hand). Toolchain: MSYS2 UCRT64 (gcc/CMake/Ninja/SDL3), README §Windows.
