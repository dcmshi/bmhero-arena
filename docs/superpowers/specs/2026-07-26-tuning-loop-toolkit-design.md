# Tuning-loop toolkit — design

**Date:** 2026-07-26
**Status:** approved, pending implementation
**Repos:** `bmhero-arena` (canonical: A, B, C) + `dcmshi/BMHeroRecomp` (fork: D)

## Problem

Several tuning items remain open (turn rate, friction, the 60-vs-30 Hz
question) plus A1.2g arena hardening. Each one currently runs this loop:

```
edit arena_tuning.h
  -> gcc one-liners for test_determinism + test_movement
  -> regenerate the CI hash (generator exists ONLY as a heredoc in determinism.yml)
  -> bump TUNE_VERSION by hand
  -> commit + push canonical
  -> fork: submodule bump
  -> patches/ make clean (forget it => stale patch => mismatch crash)
  -> compose PATH (LLVM15 + VS dev shell + MSYS2, order-sensitive)
  -> cmake build build-rwdi
  -> arena-soak.ps1 (green required before any human handoff)
  -> human feel-boot
```

Four distinct frictions, all confirmed as real costs by the user:

1. **Fork build dance** — PATH composition, the `make clean` gotcha, wall-clock.
2. **Feel eval needs a boot** — no way to compare two candidate tunes cheaply,
   and the Nitros stand-in adds camera-drift/foreshortening noise to any
   feel read taken in the fork today.
3. **Hash re-pin / version bump** — manual copy-compile-edit-YAML.
4. **Soak/verify turnaround** — each objective gate is a bespoke
   `ARENA_AUTO_BATTLE` mode requiring edits in both the harness and `main.cpp`.

## Non-goals

- No playable viewer sandbox / hot-swap tuning in `arena_viewer` (considered,
  explicitly deferred — objective metrics chosen instead).
- No change to sim behavior. Defaults stay byte-identical; hash `18fbf1bb` and
  `TUNE_VERSION` 6 must be unchanged when this work lands.
- No new gameplay features. This is tooling only.

## Invariant compliance

The five hard invariants in `CLAUDE.md` are load-bearing here:

- **#1 (no floats in `src/arena/`)** — all new float code lives in `tools/`.
  `arena_tuning.h` gains only preprocessor guards, no float arithmetic.
- **#2 (sim reads nothing outside `ArenaState`/inputs/`static const`)** — the
  metrics tool measures by *reading* `ArenaState` after `arena_tick`. Tuning
  stays compile-time; nothing is loaded at runtime.
- **#4 (layout change = version bump)** — component C enforces this
  mechanically for the first time.

---

## A. Feel metrics — `tools/tune_report.c` + `tools/tune-report.ps1`

### A.1 Enabler: `#ifndef` guards in `arena_tuning.h`

Wrap every `#define` in the table:

```c
#ifndef TUNE_RUN_FRICTION
#define TUNE_RUN_FRICTION    Q(0.030)   /* ...existing comment preserved... */
#endif
```

Purely mechanical; defaults and comments unchanged. This lets the driver
recompile a variant with `-DTUNE_RUN_FRICTION=Q(0.020)`. `Q(x)` is a
compile-time macro (`arena_math.h:11`), so command-line overrides work.

**Verification that this is behavior-preserving:** `test_determinism`,
`test_movement`, `test_bomb_mechanics` all pass and the pinned hash is still
`18fbf1bb` after the guard change alone.

### A.2 The C tool — one tune in, one TSV out

`tools/tune_report.c` links `arena_sim.c`, runs scripted probes on player 0,
and prints a TSV of `metric<TAB>value<TAB>unit`. It measures ONE tune: the one
it was compiled with. It does not sweep. Floats are fine here (tools/).

Probes, each starting from a fresh `arena_init` and driving player 0 only
(other players fed neutral `arena_input_pack(0,0,0,0,0)`):

| probe | inputs | metrics emitted |
|---|---|---|
| **ramp** | full stick +Z, hold until speed converges (or 600-tick cap) | `top_speed` (u/s), `ticks_to_90pct`, `ramp_distance` (u) |
| **stop** | reach top speed, then neutral stick | `stop_distance` (u), `stop_ticks` |
| **turn180** | reach top speed, then full stick reversed | `turn180_ticks`, `turn_radius` (u, max lateral excursion) |
| **turn90** | reach top speed, then full stick at 90° | `turn90_ticks` |
| **jump_standing** | neutral stick, jump | `jump_apex` (u), `jump_airtime` (ticks) |
| **jump_running** | top speed, then jump | `runjump_distance` (u), `runjump_airtime` |
| **traverse** | full stick across the arena's long axis | `traverse_ticks` |

Convergence rule for "top speed": speed delta below `Q(0.0005)` for 5
consecutive ticks, or the 600-tick cap (cap reached ⇒ emit the value and mark
the row `CAP` so a pathological tune is visible rather than silently wrong).

Angles are read from `ArenaPlayer.yaw` (binary angle); "turn complete" means
yaw is within 1° of target and the sign of the delta has not flipped.

The tool also emits a header block echoing the tuning values it was built with,
so a TSV is self-describing.

Flags: `--tsv` (default), `--json`, `--probe <name>` to run one probe.

### A.3 The driver — `tools/tune-report.ps1`

Handles sweeping and table joining so the C stays dumb:

```
tools\tune-report.ps1 -Compare "base","friction=0.020","friction=0.030","friction=0.045"
```

For each variant: build `tune_report` with the matching `-D` overrides into the
scratch dir, run it, collect the TSV. Then print an aligned comparison table
(metrics as rows, variants as columns) with a `--Csv` option for a file.

Variant syntax is `knob=value`, mapping short names to the full macro:
`friction` → `TUNE_RUN_FRICTION`, `turn` → `TUNE_TURN_RATE`, `accel` →
`TUNE_RUN_ACCEL`, `top` → `TUNE_RUN_SPEED`, `air` → `TUNE_AIR_CONTROL`,
`gravity` → `TUNE_GRAVITY`, `jump` → `TUNE_JUMP_IMPULSE`. Values are wrapped in
`Q(...)` automatically unless the knob is a raw integer (`turn`), and the script
owns the quoting so callers never fight PowerShell escaping. An unknown knob
name is a hard error listing the valid ones.

A `bash` sibling (`tools/tune-report.sh`) is **out of scope** — CI needs only the
C tool (component B drives it directly), and the user's environment is Windows.

---

## B. Metrics snapshot test — `tools/tune_metrics.baseline`

The committed TSV output of `tune_report` at default tuning. CI adds a step
(both workflows already build the sim; this goes in `determinism.yml`) that
regenerates and diffs.

**Why this is worth a test slot:** today an intentional change reports
`hash 77bae30e -> 18fbf1bb` — opaque. The snapshot turns it into:

```
stop_distance   0.61 -> 0.41 u   (-33%)
stop_ticks        12 -> 8
```

The hash proves *something* changed; the metrics say *what*. It distinguishes
"I tuned friction deliberately" from "I fat-fingered a constant" — a
distinction the hash structurally cannot make.

**Failure mode + escape hatch:** a diff fails the build with the table and the
instruction to run `tools/repin.ps1` (component C) which updates the baseline
alongside the hash. Baseline updates are therefore always deliberate and always
reviewable in the diff.

**Float determinism caveat:** the metrics tool uses floats for *reporting*
(unit conversion, percentages), so values could differ in the last digit across
compilers. The baseline is therefore compared at **fixed precision — 3 decimal
places for distances, exact integers for tick counts** — and the tool rounds
before printing. Tick counts and raw q-values are the load-bearing columns;
they are integer-exact and compiler-independent.

---

## C. Hash + version pin, de-duplicated

### C.1 `tools/arena_hash.c`

The generator currently inlined as a heredoc in
`.github/workflows/determinism.yml` becomes a real committed file, byte-for-byte
the same scripted match (5400 ticks, seed `0xB0BB1E5`, xorshift `0xC0FFEE01`,
same input derivation). It prints the hash to stdout and nothing else.

**Verification it was lifted faithfully:** it must print `18fbf1bb` before any
other change in this work lands.

### C.2 `tools/pinned_hash.txt`

Holds both values on one line: `6 18fbf1bb` (`TUNE_VERSION` then hash).

CI reads both and checks:

- hash matches → pass
- hash differs **and** `TUNE_VERSION` differs → still fail, but with the
  "intentional change: run `tools/repin.ps1` and commit" message
- hash differs **and** `TUNE_VERSION` is unchanged → fail with the loud
  message: *invariant #4 violated — a gameplay change must bump `TUNE_VERSION`*

That third case is the point: invariant #4 is enforced mechanically instead of
by discipline.

### C.3 `tools/repin.ps1`

Rebuilds `arena_hash` + `tune_report`, prints old→new for both the hash and the
metrics table, and rewrites `pinned_hash.txt` + `tune_metrics.baseline`. It
**refuses to run** if `TUNE_VERSION` in `arena_tuning.h` still equals the version
in `pinned_hash.txt` while the hash has changed — the fix is to bump the version
first. `-Force` overrides for the rare legitimate case (e.g. a tooling-only hash
generator correction).

### C.4 Workflow edit

`determinism.yml`'s `pin scripted-match hash` step loses its heredoc and instead
compiles `tools/arena_hash.c` and compares against `pinned_hash.txt`, then runs
the metrics diff (component B). Every matrix leg keeps running it, so the
cross-arch/cross-compiler guarantee is unchanged.

---

## D. Fork tooling — `build.ps1` + generic soak gate

### D.1 `build.ps1` (in `BMHeroRecomp` root)

One command replacing the documented manual dance:

```
.\build.ps1 -Config rwdi -Soak 5
```

Responsibilities, in order:

1. **Compose PATH** in the order `CLAUDE.md` records as load-bearing:
   `.tools\llvm15\bin` first (so `clang`/`ld.lld` resolve to v15), then the VS
   dev-shell environment (located via `vswhere`, imported through
   `Launch-VsDevShell.ps1` / `vcvars64.bat` so MSVC `link.exe` precedes msys),
   then `C:\msys64\ucrt64\bin` and `C:\msys64\usr\bin` (for `make` and the
   N64Recomp runtime DLLs). Fail with a clear message naming the missing
   component if any of the three toolchains is absent.
2. **Patch staleness check** — if any `patches/*.c` or `patches/*.h` is newer
   than `patches/patches.bin`, run `make clean && make` in `patches/`. This is
   the gotcha that otherwise produces a mismatch crash. `-Patches always|never|auto`
   (default `auto`) overrides.
3. **Build** `build-rwdi` (default, has the PDB for symbolized dumps) or
   `build-cmake` via `cmake --build <dir> --target BMHeroRecompiled`.
4. **Soak** — with `-Soak N`, chain `tools\arena-soak.ps1 -N N` and propagate
   its exit code.

Exits nonzero on any failure. With `-Soak`, a zero exit means "this exact build
booted green N times", which makes the standing rule (*no build reaches the
human without a green soak on that exact build*) a property of the tool rather
than something to remember.

### D.2 Generic soak gate

`arena-soak.ps1` gains `-Expect '<regex>'` and `-Rising '<regex-with-capture>'`:

- `-Expect` — the run fails unless the pattern appears in `arena_bridge.log`.
- `-Rising` — the pattern must match at least twice with its first capture
  group strictly increasing (the "it actually played, not just got set for one
  frame" check that `-AnimProbe` hardcodes today).

`-AnimProbe` is reimplemented in terms of these (`-Rising 'idx=29 frame=(\d+)'`
plus `ARENA_AUTO_BATTLE=4`) and kept as an alias so existing invocations and the
A1.4 gate keep working unchanged. The win: the next objective probe needs no
harness edit, only its `ARENA_AUTO_BATTLE` mode in `main.cpp`.

---

## Testing

| component | how it's verified |
|---|---|
| A.1 guards | full test suite green + hash still `18fbf1bb` after the guard-only change |
| A.2 tool | unit-ish: assert known-physics relationships hold — e.g. halving `TUNE_RUN_FRICTION` strictly increases `stop_distance`; doubling `TUNE_GRAVITY` strictly decreases `jump_apex`. These are monotonicity assertions, robust to exact values, and become `tests/test_tune_report.c` |
| A.3 driver | manual run of the 4-variant friction sweep from the design's motivating example; unknown-knob error path checked |
| B baseline | deliberately perturb a constant → CI diff shows the expected metric moving; revert |
| C pin | all three CI branches exercised: matching, changed-with-bump, changed-without-bump |
| D build.ps1 | clean-tree build, stale-patch build (touch a `patches/*.c`, confirm the rebuild fires), missing-toolchain error path |
| D soak gate | `-AnimProbe` alias reproduces the existing A1.4 PASS on the current build |

Everything in A/B/C runs under the CI flags exactly (`-Wall -Wextra -Werror`).

## Order of work

1. A.1 guards (unblocks everything, must be provably inert)
2. C.1/C.2 hash lift + pin file (small, removes the duplication before B depends on it)
3. A.2 the C tool, TDD'd against the monotonicity assertions
4. A.3 driver script
5. B baseline + CI wiring (needs A.2 and C.2 in place)
6. D fork `build.ps1`, then the soak gate generalization

Components A/B/C land in `bmhero-arena` on one branch; D lands in the fork
separately since it touches no sim code and needs no submodule bump.

## Risks

- **Guarding the tuning table touches the file every future tune edits.** Kept
  mechanical and comment-preserving so `git blame` stays readable.
- **The metrics baseline could become noise** if probes are unstable across
  compilers. Mitigated by the fixed-precision comparison and by tick counts
  (integer-exact) being the primary columns. If a probe proves flaky in the CI
  matrix, drop that row from the baseline rather than loosening the whole diff.
- **`build.ps1` encodes machine-specific paths** (`.tools\llvm15`, `C:\msys64`).
  Made overridable by environment variable with the documented defaults, so it
  degrades to a clear error rather than a wrong build on another machine.
