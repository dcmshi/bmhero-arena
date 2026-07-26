# Tuning-Loop Toolkit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a four-part toolkit that makes every remaining tuning item fast to evaluate, mechanically safe to land, and cheap to build — objective feel metrics, a metrics regression test, a de-duplicated hash/version pin, and a one-command fork build.

**Architecture:** Components A/B/C live in the canonical `bmhero-arena` repo on one branch; component D lives in the `BMHeroRecomp` fork on its own branch (it touches no sim code and needs no submodule bump). The metrics harness is split into a probe *library* (`tools/tune_probes.c`) and a thin formatting *main* (`tools/tune_report.c`) so the unit test can link the probes directly. Tuning stays compile-time; variant sweeps happen by recompiling, which takes about a second because the sim is a single `.c` file.

**Tech Stack:** C11 (gcc/clang, `-Wall -Wextra -Werror`), PowerShell 5+ (Windows-only driver/build scripts), GitHub Actions.

**Source spec:** `docs/superpowers/specs/2026-07-26-tuning-loop-toolkit-design.md`

## Global Constraints

These apply to **every** task. Violating any of them fails the task.

- **No floats in `src/arena/`.** Q20.12 fixed-point only. All float code in this plan lives in `tools/` or `tests/`. `arena_tuning.h` gains *only* preprocessor guards — no arithmetic changes.
- **The sim must not change behavior.** After every task in this plan, the pinned scripted-match hash is still `18fbf1bb` and `TUNE_VERSION` is still `6`. If either moves, you broke something — stop and investigate; do not repin.
- **All C builds use the CI flags exactly:** `-std=c11 -Wall -Wextra -Werror`. The sim additionally uses `-Wconversion -Wno-sign-conversion` under CMake; new `tools/` code need not be `-Wconversion` clean but must be `-Werror` clean.
- **Probe harness invariants — these are non-obvious and cost hours if missed:**
  - **`num_players` must be 2, never 1.** With one player, `arena_sim`'s last-player-standing liveness check (`alive <= 1`) fires at the end of tick 1 and flips `phase` to `PHASE_ROUND_END`, which gates off all `player_tick` movement logic for every subsequent tick. Player 1 gets neutral input and never interferes. This is documented in `tests/test_movement.c:11-18`.
  - **Set `s.phase = PHASE_PLAY` immediately after `arena_init`** to skip the 180-tick countdown.
  - **Neutral input is `arena_input_pack(0,0,0,0,0)` (= `0x820`), never a raw `0`.** Raw `0` decodes to stick `(-32,-32)` — a full diagonal. Every entry of the input array must be set, not just player 0's.
  - **Stick convention:** `sy = -31` is "forward". The sim resolves the target angle as `iatan2(Q(sx), Q(-sy))`, so `sy=+31` maps to yaw `0x8000` and `sy=-31` maps to yaw `0x0000`. Getting this backwards produces a degenerate zero-turn test (see the note at `tests/test_movement.c:33-38`).
- **Arena geometry is small — reposition before measuring.** Map 0 (`arena_nitros_standin`) is `half_x = Q(7.9)`, `half_z = Q(3.87)`, and spawn 0 is `{Q(-7.8), 0, Q(-3.8)}` — hard against two walls. A 180° turn at top speed sweeps roughly 6.8 units and would hit the `±3.87` Z wall from the spawn. **Every probe except `traverse` must reposition player 0 to arena center `{0,0,0}` and player 1 to `{Q(7.0), 0, Q(3.0)}` before measuring.** Writing `s.players[i].pos` directly is legitimate here — the harness is a measurement tool, and `tests/test_movement.c` already sets `s.players[0].yaw` and `s.phase` the same way.
- **Fixed-precision reporting.** Distances and speeds print at exactly 3 decimal places; tick counts print as integers. This is what keeps the component-B baseline stable across the six CI matrix legs. Never print raw `double` with default precision.
- **Commit after every task.** Small, frequent commits.

## Branches

- Components A/B/C: branch `feature/tuning-loop-toolkit` off `main` in `C:\Users\dshi\GitRepos\bmhero-arena`.
- Component D: branch `feature/build-and-soak-tooling` off the current fork branch in `C:\Users\dshi\GitRepos\BMHeroRecomp`.

## File Structure

**`bmhero-arena`:**

| File | Responsibility |
|---|---|
| `src/arena/arena_tuning.h` *(modify)* | gains `#ifndef` guards; no other change |
| `tools/tune_probes.h` *(create)* | `TuneMetrics` struct + `tune_probes_run()` declaration |
| `tools/tune_probes.c` *(create)* | the seven probes; the only file that drives `arena_tick` for measurement |
| `tools/tune_report.c` *(create)* | thin `main()`: arg parsing + TSV/JSON formatting |
| `tests/test_tune_report.c` *(create)* | links `tune_probes.c`; asserts probe output against `TUNE_*` macros |
| `tools/arena_hash.c` *(create)* | the scripted-match hash generator, lifted out of the workflow heredoc |
| `tools/pinned_hash.txt` *(create)* | `<TUNE_VERSION> <hash>` on one line |
| `tools/tune_metrics.baseline` *(create)* | committed TSV at default tuning |
| `tools/tune-report.ps1` *(create)* | variant sweep + comparison table + `-SelfTest` |
| `tools/repin.ps1` *(create)* | regenerate hash + baseline, with the invariant-#4 refusal |
| `.github/workflows/determinism.yml` *(modify)* | heredoc → `tools/arena_hash.c`; add metrics diff step |
| `CMakeLists.txt` *(modify)* | add `test_tune_report` target + ctest entry |

**`BMHeroRecomp`:**

| File | Responsibility |
|---|---|
| `build.ps1` *(create)* | PATH composition + patch staleness + build + optional soak |
| `tools/arena-soak.ps1` *(modify)* | generic `-Expect` / `-Rising` gates; `-AnimProbe` becomes an alias |

---

## Task 1: Guard the tuning table

Makes every `TUNE_*` value overridable from the compiler command line, which every later task depends on. Must be provably inert.

**Files:**
- Modify: `src/arena/arena_tuning.h` (every `#define TUNE_*` in the file)

**Interfaces:**
- Consumes: nothing.
- Produces: the ability to pass `-DTUNE_RUN_FRICTION=Q(0.020)` (and the same for any other `TUNE_*` name) to any compile that includes `arena_tuning.h`. `Q(x)` is a compile-time macro (`src/arena/arena_math.h:11`), so `Q(...)` is valid inside a `-D`.

- [ ] **Step 1: Record the pre-change baseline**

Run from the repo root:

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_det src/arena/arena_sim.c tests/test_determinism.c && /tmp/t_det
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_mv  src/arena/arena_sim.c tests/test_movement.c  && /tmp/t_mv
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_bmb src/arena/arena_sim.c tests/test_bomb_mechanics.c && /tmp/t_bmb
```

Expected: `ALL TESTS PASSED`, `ALL MOVEMENT TESTS PASSED`, and the bomb suite's pass line. Write the three results down — they must be identical after the change.

- [ ] **Step 2: Add the guards**

For **every** `#define TUNE_*` in `src/arena/arena_tuning.h`, wrap it. Preserve the existing comment verbatim and in place — `git blame` readability matters here because this file is edited by every future tuning change. Example, for the first one:

```c
#ifndef TUNE_RUN_SPEED
#define TUNE_RUN_SPEED       Q(0.151)   /* game top 18 u/f x S (real walker; was auto-runner 10 -> Q(0.084)) */
#endif
```

Apply the same treatment to all of: `TUNE_RUN_SPEED`, `TUNE_RUN_ACCEL`, `TUNE_RUN_FRICTION`, `TUNE_JUMP_IMPULSE`, `TUNE_GRAVITY`, `TUNE_TERMINAL_VY`, `TUNE_AIR_CONTROL`, `TUNE_PLAYER_RADIUS`, `TUNE_PLAYER_HEIGHT`, `TUNE_TURN_RATE`, `TUNE_THROW_SPEED`, `TUNE_THROW_UP`, `TUNE_SPREAD_TICKS`, `TUNE_SPREAD_SPEED`, `TUNE_SPREAD_UP`, `TUNE_KICK_SPEED`, `TUNE_KICK_MIN_VEL`, `TUNE_BOMB_RADIUS`, `TUNE_BOMB_RESTITUTION`, `TUNE_BOMB_H_DAMP`, `TUNE_FUSE_TICKS`, `TUNE_MAX_LIVE_BOMBS`, `TUNE_BLAST_RADIUS`, `TUNE_BLAST_TTL`, `TUNE_BLAST_GROW_TICKS`, `TUNE_KNOCKBACK`, `TUNE_KNOCKBACK_UP`, `TUNE_INVULN_TICKS`, `TUNE_TUMBLE_TICKS`, `TUNE_START_HP`, `TUNE_ROUND_TICKS`, `TUNE_COUNTDOWN_TICKS`, `TUNE_ROUND_END_TICKS`, `TUNE_ROUNDS_TO_WIN`, `TUNE_VERSION`.

Multi-line comments (e.g. `TUNE_RUN_FRICTION`, `TUNE_TURN_RATE`, `TUNE_VERSION`) keep their full comment block inside the guard.

- [ ] **Step 3: Verify the change is inert**

Re-run all three commands from Step 1. Expected: byte-identical output to the baseline you recorded.

Then verify the hash is untouched:

```bash
gcc -std=c11 -O2 -I. -o /tmp/hc /tmp/hash_main.c src/arena/arena_sim.c && /tmp/hc
```

…where `/tmp/hash_main.c` is the heredoc currently embedded in `.github/workflows/determinism.yml` (lines under `pin scripted-match hash`). Expected output: `18fbf1bb`. *(Task 2 makes this a committed file so you stop copying it by hand.)*

- [ ] **Step 4: Verify an override actually works**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -DTUNE_RUN_FRICTION='Q(0.020)' \
    -o /tmp/t_mv_override src/arena/arena_sim.c tests/test_movement.c && /tmp/t_mv_override
```

Expected: compiles cleanly with **no** "macro redefined" warning (which would mean a guard was missed). The test may pass or fail — irrelevant here; a clean compile with the override accepted is the deliverable.

- [ ] **Step 5: Commit**

```bash
git checkout -b feature/tuning-loop-toolkit
git add src/arena/arena_tuning.h
git commit -m "refactor(tuning): guard TUNE_* defines so variants can override via -D

Mechanical, behavior-preserving: defaults, comments, and ordering unchanged.
Verified inert - test_determinism/test_movement/test_bomb_mechanics all
byte-identical, scripted-match hash still 18fbf1bb, TUNE_VERSION still 6.
Enables the tune_report variant sweep (component A)."
```

---

## Task 2: Lift the hash generator and pin both values

Removes the copy-compile-edit-YAML step, and makes invariant #4 (gameplay change ⇒ `TUNE_VERSION` bump) mechanically enforced for the first time.

**Files:**
- Create: `tools/arena_hash.c`
- Create: `tools/pinned_hash.txt`
- Modify: `.github/workflows/determinism.yml` (the `pin scripted-match hash` step)

**Interfaces:**
- Consumes: Task 1's guarded header (only incidentally — this task compiles the sim normally).
- Produces: `tools/arena_hash.c` prints exactly one line, the 8-hex-digit hash, to stdout and nothing else. `tools/pinned_hash.txt` contains one line: `<TUNE_VERSION> <hash>`, space-separated. Task 7 (`repin.ps1`) rewrites this file; Task 6's CI step reads it.

- [ ] **Step 1: Create the generator file**

Create `tools/arena_hash.c`. This is the heredoc from `.github/workflows/determinism.yml` lifted verbatim — same 5400 ticks, same seed `0xB0BB1E5`, same xorshift seed `0xC0FFEE01`, same input derivation. Do not "improve" it; any change here changes the hash.

```c
/* Scripted-match hash generator — the cross-platform determinism pin.
 * Lifted verbatim from .github/workflows/determinism.yml (2026-07-26) so the
 * generator has ONE home instead of living only inside a CI heredoc.
 * Prints the 8-hex-digit final-state hash and nothing else.
 * Any edit to this file changes the pinned hash: don't touch it casually. */
#include <stdio.h>
#include "../src/arena/arena_sim.h"

int main(void) {
    ArenaState s; ArenaInput in[4];
    arena_init(&s, 0, 4, 0xB0BB1E5);
    uint32_t r = 0xC0FFEE01;
    for (uint32_t t = 0; t < 5400; t++) {
        for (int i = 0; i < 4; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            int sx = (int)(r & 63) - 32;        if (sx < -31) sx = -31;
            int sy = (int)((r >> 6) & 63) - 32; if (sy < -31) sy = -31;
            int set  = ((t + i * 53) % 137) == 0;
            int bomb = ((t + i * 37) % (90 + i * 80)) < (30 + i * 40);
            in[i] = arena_input_pack(sx, sy, ((r >> 12) & 31) == 0, bomb, set);
        }
        arena_tick(&s, in);
    }
    printf("%08x\n", arena_hash(&s));
    return 0;
}
```

Note the include path is `../src/arena/arena_sim.h` — the file lives in `tools/`. The original heredoc used `-I.` with `"src/arena/arena_sim.h"`; the relative include makes the file compile without a `-I` flag.

- [ ] **Step 2: Verify it reproduces the pinned hash**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/ah tools/arena_hash.c src/arena/arena_sim.c && /tmp/ah
```

Expected: `18fbf1bb`. Also confirm under `-O0` and `-O3` — all three must agree.

**If this does not print `18fbf1bb`, stop.** The lift was not faithful; diff your file against the workflow heredoc line by line.

- [ ] **Step 3: Create the pin file**

Create `tools/pinned_hash.txt` with exactly one line (trailing newline, no comment — this file is machine-read):

```
6 18fbf1bb
```

- [ ] **Step 4: Rewrite the workflow step**

In `.github/workflows/determinism.yml`, replace the entire `pin scripted-match hash` step (the one containing `cat > hash_main.c <<'EOF'`) with:

```yaml
      # Cross-platform hash equality: every job must produce the pinned final
      # state hash for the scripted match. The generator lives in
      # tools/arena_hash.c and the pin in tools/pinned_hash.txt (TUNE_VERSION
      # then hash). Update BOTH via tools/repin.ps1 on an intentional change.
      - name: pin scripted-match hash
        shell: bash
        run: |
          read -r pinned_ver pinned_hash < tools/pinned_hash.txt
          cur_ver=$(sed -n 's/^#define[[:space:]]*TUNE_VERSION[[:space:]]*\([0-9][0-9]*\).*/\1/p' \
                    src/arena/arena_tuning.h)
          ${{ matrix.cc }} -std=c11 ${{ matrix.opt }} -o hash_check \
            tools/arena_hash.c src/arena/arena_sim.c
          h=$(./hash_check)
          echo "hash: $h (pinned $pinned_hash) | TUNE_VERSION: $cur_ver (pinned $pinned_ver)"
          if [ "$h" = "$pinned_hash" ]; then
            echo "OK - sim behavior unchanged"
          elif [ "$cur_ver" = "$pinned_ver" ]; then
            echo "::error::INVARIANT #4 VIOLATED - the sim hash changed ($pinned_hash -> $h)"
            echo "::error::but TUNE_VERSION is still $cur_ver. A gameplay change MUST bump"
            echo "::error::TUNE_VERSION (netcode version handshake). Bump it, then run tools/repin.ps1."
            exit 1
          else
            echo "::error::Sim hash changed ($pinned_hash -> $h) with TUNE_VERSION $pinned_ver -> $cur_ver."
            echo "::error::That looks intentional - run tools/repin.ps1 and commit the updated"
            echo "::error::tools/pinned_hash.txt and tools/tune_metrics.baseline."
            exit 1
          fi
```

The `sed` expression is deliberately POSIX-portable (`[0-9][0-9]*`, no `\+`, no `-P`) because this step runs on ubuntu, macOS (BSD sed), and Windows/MSYS legs alike. It captures `6` from `#define TUNE_VERSION 6 /* ...comment... */`.

- [ ] **Step 5: Verify the sed extraction locally**

```bash
sed -n 's/^#define[[:space:]]*TUNE_VERSION[[:space:]]*\([0-9][0-9]*\).*/\1/p' src/arena/arena_tuning.h
```

Expected: `6` — a single line. If it prints nothing, Task 1's guard indented the define; adjust the pattern to tolerate leading whitespace and re-verify.

- [ ] **Step 6: Verify all three CI branches by hand**

Branch 1 (matching) — already covered by Step 2.

Branch 2 (changed hash, no version bump ⇒ invariant violation):

```bash
gcc -std=c11 -O2 -DTUNE_RUN_FRICTION='Q(0.020)' -o /tmp/ah2 tools/arena_hash.c src/arena/arena_sim.c && /tmp/ah2
```

Expected: an 8-hex value that is **not** `18fbf1bb`. Confirm that with `TUNE_VERSION` unchanged this is the case the workflow's second branch catches.

Branch 3 (changed hash + bumped version) — same command plus `-DTUNE_VERSION=7`; confirm the third branch's message is the one that would fire.

You are verifying the *logic you wrote reaches the right branch*, not running Actions locally. Reason it through against the three observed hash/version combinations and state the conclusion in the commit.

- [ ] **Step 7: Commit**

```bash
git add tools/arena_hash.c tools/pinned_hash.txt .github/workflows/determinism.yml
git commit -m "build(ci): lift hash generator out of the workflow heredoc; pin version too

tools/arena_hash.c is now the single home of the scripted-match generator
(verified byte-faithful: still prints 18fbf1bb at -O0/-O2/-O3).
tools/pinned_hash.txt holds '<TUNE_VERSION> <hash>', so CI can distinguish
an intentional tune (hash+version both moved -> run repin) from an
invariant-#4 violation (hash moved, version didn't -> hard error)."
```

---

## Task 3: Probe library core — ramp and stop

The measurement engine. Split from the formatting `main` so the unit test can link it directly.

**Files:**
- Create: `tools/tune_probes.h`
- Create: `tools/tune_probes.c`
- Create: `tests/test_tune_report.c`

**Interfaces:**
- Consumes: Task 1's guarded `arena_tuning.h`; `arena_sim.h`, `arena_state.h`, `arena_math.h`, `arena_geom.h` from `src/arena/`.
- Produces: `TuneMetrics` (all fields listed below, populated by later tasks too) and `void tune_probes_run(TuneMetrics* out);`. Task 4 fills the remaining fields. Task 5's script and Task 6's baseline consume the printed output only. Field names here are load-bearing — Task 4 and Task 6 refer to them exactly.

- [ ] **Step 1: Write the header**

Create `tools/tune_probes.h`:

```c
/* Objective feel-metrics probes. Drives arena_tick with scripted inputs and
 * measures the quantities the open tuning knobs actually control.
 * Lives in tools/ (NOT src/arena/) so it may use floats freely — invariant #1.
 * Measurement only: reads ArenaState after each tick, never mutates the sim's
 * logic. Repositioning players before a probe is deliberate (see .c). */
#ifndef TUNE_PROBES_H
#define TUNE_PROBES_H

/* One row per metric in the report. Distances/speeds are world units (u) and
 * u/s; tick counts are integer 60Hz ticks.
 * `*_capped` flags mark a probe that hit its iteration cap before converging —
 * the value is still emitted so a pathological tune is visible, not silent. */
typedef struct {
    /* ramp */
    double top_speed;          /* u/s at convergence, full stick */
    int    ticks_to_90pct;     /* ticks to reach 90% of top_speed */
    double ramp_distance;      /* u travelled reaching 90% */
    int    ramp_capped;
    /* stop */
    double stop_distance;      /* u travelled after stick release from top speed */
    int    stop_ticks;
    int    stop_capped;
    /* turn (Task 4) */
    int    turn180_ticks;
    int    turn90_ticks;
    double turn_radius;        /* u, max lateral excursion during the 180 */
    int    turn_capped;
    /* jump (Task 4) */
    double jump_apex;          /* u */
    int    jump_airtime;       /* ticks */
    double runjump_distance;   /* u travelled airborne from top speed */
    int    runjump_airtime;
    /* traverse (Task 4) */
    int    traverse_ticks;
    int    traverse_capped;
} TuneMetrics;

/* Runs every probe. Zeroes `out` first. Deterministic: same build => same
 * numbers, every run. */
void tune_probes_run(TuneMetrics* out);

#endif
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_tune_report.c`. It links `tune_probes.c` and asserts the probes agree with the physics implied by the `TUNE_*` macros in the *same* binary — the same technique `tests/test_movement.c` uses. Cross-*tune* monotonicity is verified at the script level in Task 5, because compile-time constants cannot vary within one binary.

```c
/* Consistency tests for the feel-metrics probes. These assert the probe
 * OUTPUT matches the physics implied by TUNE_* in this same build, so they
 * hold at any tuning — they guard the harness, not a particular tune. */
#include <stdio.h>
#include "../tools/tune_probes.h"
#include "../src/arena/arena_tuning.h"
#include "../src/arena/arena_math.h"

static int failures = 0;
#define CHECK(c, ...) do { if(!(c)){ failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while(0)

static double q_to_f(q32 q) { return (double)q / (double)Q_ONE; }

int main(void) {
    TuneMetrics m;
    tune_probes_run(&m);

    /* top speed: TUNE_RUN_SPEED is per-tick; report is per-second (x60). */
    double expect_top = q_to_f(TUNE_RUN_SPEED) * 60.0;
    CHECK(m.top_speed > expect_top * 0.97 && m.top_speed < expect_top * 1.03,
          "top_speed %.3f u/s vs expected ~%.3f", m.top_speed, expect_top);
    CHECK(!m.ramp_capped, "ramp probe hit its cap - tune or harness is broken");

    /* linear accel to target: ~RUN_SPEED/RUN_ACCEL ticks, allow slack */
    int expect_t90 = TUNE_RUN_SPEED / TUNE_RUN_ACCEL;
    CHECK(m.ticks_to_90pct > 0 && m.ticks_to_90pct <= expect_t90 + 3,
          "ticks_to_90pct %d vs expected <=~%d", m.ticks_to_90pct, expect_t90);

    /* stop: decel from top at FRICTION per tick => ~RUN_SPEED/FRICTION ticks */
    int expect_stop_t = TUNE_RUN_SPEED / TUNE_RUN_FRICTION;
    CHECK(m.stop_ticks > 0 && m.stop_ticks <= expect_stop_t + 3,
          "stop_ticks %d vs expected <=~%d", m.stop_ticks, expect_stop_t);
    CHECK(m.stop_distance > 0.0, "stop_distance must be positive (got %.3f)", m.stop_distance);
    CHECK(!m.stop_capped, "stop probe hit its cap - player never came to rest");

    /* a player that stops instantly or never stops means the probe is wrong */
    CHECK(m.stop_distance < 20.0, "stop_distance %.3f implausibly large", m.stop_distance);

    if (!failures) { printf("ALL TUNE REPORT TESTS PASSED\n"); return 0; }
    printf("%d FAILURE(S)\n", failures); return 1;
}
```

- [ ] **Step 3: Run it to confirm it fails**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_tr \
    src/arena/arena_sim.c tools/tune_probes.c tests/test_tune_report.c && /tmp/t_tr
```

Expected: **compile failure** — `tools/tune_probes.c` does not exist yet.

- [ ] **Step 4: Implement the probe library core**

Create `tools/tune_probes.c` with the shared harness plus the ramp and stop probes. Task 4 appends the rest.

```c
#include <string.h>
#include "tune_probes.h"
#include "../src/arena/arena_sim.h"
#include "../src/arena/arena_tuning.h"
#include "../src/arena/arena_geom.h"

#define TICK_HZ      60.0
#define CAP_TICKS    600      /* every probe's iteration ceiling */
#define SETTLE_TICKS 5        /* consecutive near-zero deltas => converged */
#define SETTLE_EPS   Q(0.0005)

static double q_to_f(q32 q) { return (double)q / (double)Q_ONE; }

/* Fresh match, player 0 at arena CENTRE, player 1 parked far away.
 * - num_players MUST be 2: with 1, the liveness check (alive<=1) flips phase to
 *   PHASE_ROUND_END on tick 1 and gates off all movement (test_movement.c:11).
 * - phase forced to PHASE_PLAY to skip the countdown.
 * - centre, not the spawn: spawn 0 is {-7.8,0,-3.8}, hard against two walls of a
 *   7.9 x 3.87 arena; a 180 turn at top speed sweeps ~6.8u and would hit a wall,
 *   contaminating the measurement. */
static void probe_init(ArenaState* s, ArenaInput in[ARENA_MAX_PLAYERS]) {
    arena_init(s, 0, 2, 1);
    s->phase = PHASE_PLAY;
    s->players[0].pos.x = 0; s->players[0].pos.y = 0; s->players[0].pos.z = 0;
    s->players[1].pos.x = Q(7.0); s->players[1].pos.y = 0; s->players[1].pos.z = Q(3.0);
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        in[i] = arena_input_pack(0, 0, 0, 0, 0);   /* neutral is 0x820, NOT raw 0 */
}

static q32 speed_of(const ArenaState* s) {
    return qlen2(s->players[0].vel.x, s->players[0].vel.z);
}

/* Drive full stick +X until speed converges. Returns top speed (q32/tick).
 * Leaves `s` at top speed so the caller can chain a probe onto it.
 * +X because the arena is wider on X (half_x 7.9 vs half_z 3.87). */
static q32 drive_to_top(ArenaState* s, ArenaInput in[ARENA_MAX_PLAYERS], int* capped) {
    in[0] = arena_input_pack(31, 0, 0, 0, 0);
    q32 prev = 0; int stable = 0; int t;
    for (t = 0; t < CAP_TICKS; t++) {
        arena_tick(s, in);
        q32 cur = speed_of(s);
        q32 d = cur > prev ? cur - prev : prev - cur;
        stable = (d <= SETTLE_EPS) ? stable + 1 : 0;
        prev = cur;
        if (stable >= SETTLE_TICKS) break;
    }
    if (capped) *capped = (t >= CAP_TICKS);
    return prev;
}

static void probe_ramp(TuneMetrics* out) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    q32 top = drive_to_top(&s, in, &out->ramp_capped);
    out->top_speed = q_to_f(top) * TICK_HZ;

    /* second pass: ticks + distance to 90% of the measured top */
    probe_init(&s, in);
    in[0] = arena_input_pack(31, 0, 0, 0, 0);
    q32 target = qmul(top, Q(0.9));
    q32 x0 = s.players[0].pos.x;
    out->ticks_to_90pct = 0; out->ramp_distance = 0.0;
    for (int t = 1; t <= CAP_TICKS; t++) {
        arena_tick(&s, in);
        if (speed_of(&s) >= target) {
            out->ticks_to_90pct = t;
            q32 dx = s.players[0].pos.x - x0;
            out->ramp_distance = q_to_f(dx < 0 ? -dx : dx);
            break;
        }
    }
}

static void probe_stop(TuneMetrics* out) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    drive_to_top(&s, in, NULL);

    /* release the stick; measure until at rest */
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++) in[i] = arena_input_pack(0, 0, 0, 0, 0);
    q32 x0 = s.players[0].pos.x, z0 = s.players[0].pos.z;
    int t;
    for (t = 1; t <= CAP_TICKS; t++) {
        arena_tick(&s, in);
        if (speed_of(&s) <= SETTLE_EPS) break;
    }
    out->stop_capped = (t > CAP_TICKS);
    out->stop_ticks  = out->stop_capped ? CAP_TICKS : t;
    q32 dx = s.players[0].pos.x - x0, dz = s.players[0].pos.z - z0;
    out->stop_distance = q_to_f(qlen2(dx, dz));
}

void tune_probes_run(TuneMetrics* out) {
    memset(out, 0, sizeof(*out));
    probe_ramp(out);
    probe_stop(out);
}
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_tr \
    src/arena/arena_sim.c tools/tune_probes.c tests/test_tune_report.c && /tmp/t_tr
```

Expected: `ALL TUNE REPORT TESTS PASSED`.

If `top_speed` is far off, the most likely cause is the player hitting a wall — confirm `probe_init` is repositioning to `{0,0,0}` and that `drive_to_top` uses `+X`.

- [ ] **Step 6: Confirm the sim is still untouched**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/ah tools/arena_hash.c src/arena/arena_sim.c && /tmp/ah
```

Expected: `18fbf1bb`.

- [ ] **Step 7: Commit**

```bash
git add tools/tune_probes.h tools/tune_probes.c tests/test_tune_report.c
git commit -m "feat(tools): feel-metrics probe library - ramp and stop

Measures top speed, ramp-to-90%, stop distance and stop time by driving
arena_tick with scripted inputs. Probes reposition player 0 to arena centre
(spawn 0 sits against two walls of the 7.9x3.87 map, which would contaminate
any probe that sweeps). num_players=2 to keep the liveness check from ending
the round on tick 1. Guarded by tests/test_tune_report.c."
```

---

## Task 4: Remaining probes — turn, jump, traverse

**Files:**
- Modify: `tools/tune_probes.c` (append probes; extend `tune_probes_run`)
- Modify: `tests/test_tune_report.c` (add assertions)

**Interfaces:**
- Consumes: Task 3's `probe_init`, `speed_of`, `drive_to_top`, `q_to_f`, `CAP_TICKS`, `SETTLE_EPS`.
- Produces: populates the `turn*`, `jump*`, `runjump*`, and `traverse*` fields of `TuneMetrics`, which Task 6's baseline pins.

- [ ] **Step 1: Write the failing assertions**

Append to `tests/test_tune_report.c`, immediately before the `if (!failures)` line:

```c
    /* turn: bounded rate => 180deg takes ~0x8000/TUNE_TURN_RATE ticks */
    int expect_180 = 0x8000 / TUNE_TURN_RATE;
    CHECK(m.turn180_ticks >= expect_180 - 3 && m.turn180_ticks <= expect_180 + 3,
          "turn180_ticks %d vs expected ~%d", m.turn180_ticks, expect_180);
    CHECK(m.turn90_ticks > 0 && m.turn90_ticks < m.turn180_ticks,
          "turn90 (%d) must be positive and shorter than turn180 (%d)",
          m.turn90_ticks, m.turn180_ticks);
    CHECK(!m.turn_capped, "turn probe hit its cap - yaw never reached target");
    CHECK(m.turn_radius > 0.0, "turn_radius must be positive (got %.3f)", m.turn_radius);

    /* jump: apex ~ impulse^2/(2g), airtime ~ 2*impulse/g  (same model as
     * tests/test_movement.c test_jump_arc) */
    double imp = q_to_f(TUNE_JUMP_IMPULSE), grav = q_to_f(TUNE_GRAVITY);
    double expect_apex = (imp * imp) / (2.0 * grav);
    CHECK(m.jump_apex > expect_apex * 0.85 && m.jump_apex < expect_apex * 1.15,
          "jump_apex %.3f vs expected ~%.3f (>15%%)", m.jump_apex, expect_apex);
    int expect_air = (int)(2.0 * imp / grav);
    CHECK(m.jump_airtime >= expect_air - 4 && m.jump_airtime <= expect_air + 4,
          "jump_airtime %d vs expected ~%d", m.jump_airtime, expect_air);

    /* a running jump must cover ground; airtime should be close to standing */
    CHECK(m.runjump_distance > 0.0, "runjump_distance must be positive (got %.3f)",
          m.runjump_distance);
    CHECK(m.runjump_airtime >= expect_air - 6 && m.runjump_airtime <= expect_air + 6,
          "runjump_airtime %d vs expected ~%d", m.runjump_airtime, expect_air);

    /* traverse: crossing 2*half_x at top speed, plus the ramp-up */
    CHECK(m.traverse_ticks > 0, "traverse_ticks must be positive");
    CHECK(!m.traverse_capped, "traverse probe hit its cap - player never crossed");
```

- [ ] **Step 2: Run to confirm the new assertions fail**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_tr \
    src/arena/arena_sim.c tools/tune_probes.c tests/test_tune_report.c && /tmp/t_tr
```

Expected: FAIL — the new fields are all zero (`turn180_ticks 0 vs expected ~45`, etc.).

- [ ] **Step 3: Implement the probes**

Append to `tools/tune_probes.c`, before `tune_probes_run`:

```c
/* Ticks for yaw to reach `target` (within 1 degree = 0x00B6), driving `stick`.
 * Also records the largest lateral excursion from the start Z, which is the
 * turn radius at whatever speed the caller left the player at. */
static int turn_probe(int sx, int sy, uint16_t start_yaw, double* out_radius, int* capped) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    drive_to_top(&s, in, NULL);
    s.players[0].yaw = start_yaw;
    in[0] = arena_input_pack(sx, sy, 0, 0, 0);
    uint16_t target = iatan2(Q(sx), Q(-sy));   /* the sim's own convention */
    q32 z0 = s.players[0].pos.z, maxlat = 0;
    int t;
    for (t = 1; t <= CAP_TICKS; t++) {
        arena_tick(&s, in);
        q32 lat = s.players[0].pos.z - z0; if (lat < 0) lat = -lat;
        if (lat > maxlat) maxlat = lat;
        uint16_t y = s.players[0].yaw;
        if ((uint16_t)(y - target) <= 0x00B6 || (uint16_t)(target - y) <= 0x00B6) break;
    }
    if (capped) *capped = (t > CAP_TICKS);
    if (out_radius) *out_radius = q_to_f(maxlat);
    return t > CAP_TICKS ? CAP_TICKS : t;
}

static void probe_turns(TuneMetrics* out) {
    /* 180: start facing +Z (0x8000), stick full "forward" (sy=-31 -> yaw 0).
     * sy MUST be -31, not +31: iatan2(Q(0),Q(-31)) resolves to 0x0000 (a real
     * reversal) whereas sy=+31 resolves to 0x8000 == start_yaw (no turn at
     * all). Same trap documented at tests/test_movement.c:33. */
    out->turn180_ticks = turn_probe(0, -31, 0x8000, &out->turn_radius, &out->turn_capped);
    /* 90: start facing +Z (0x8000), stick full +X (yaw 0x4000) */
    out->turn90_ticks  = turn_probe(31, 0, 0x8000, NULL, NULL);
}

/* One jump. `running` decides whether we launch from rest or from top speed. */
static void jump_probe(int running, double* out_apex, int* out_air, double* out_dist) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    if (running) drive_to_top(&s, in, NULL);
    ArenaInput hold = running ? arena_input_pack(31, 0, 0, 0, 0)
                              : arena_input_pack(0, 0, 0, 0, 0);
    q32 x0 = s.players[0].pos.x, z0 = s.players[0].pos.z;
    q32 apex = 0; int air = 0, launched = 0;
    for (int t = 1; t <= CAP_TICKS; t++) {
        /* jump is edge-triggered: press on the first tick only */
        in[0] = (t == 1) ? (ArenaInput)(hold | (1u << 12)) : hold;
        arena_tick(&s, in);
        if (s.players[0].pos.y > 0) {
            launched = 1; air++;
            if (s.players[0].pos.y > apex) apex = s.players[0].pos.y;
        } else if (launched) break;
    }
    if (out_apex) *out_apex = q_to_f(apex);
    if (out_air)  *out_air  = air;
    if (out_dist) {
        q32 dx = s.players[0].pos.x - x0, dz = s.players[0].pos.z - z0;
        *out_dist = q_to_f(qlen2(dx, dz));
    }
}

static void probe_jumps(TuneMetrics* out) {
    jump_probe(0, &out->jump_apex, &out->jump_airtime, NULL);
    jump_probe(1, NULL, &out->runjump_airtime, &out->runjump_distance);
}

/* Cross the arena's long axis from the -X wall to the +X wall, from rest.
 * The one probe that deliberately starts at a wall rather than the centre. */
static void probe_traverse(TuneMetrics* out) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    const ArenaGeom* g = arena_geoms[0];
    q32 startx = -g->half_x + Q(0.5), endx = g->half_x - Q(0.5);
    s.players[0].pos.x = startx;
    in[0] = arena_input_pack(31, 0, 0, 0, 0);
    int t;
    for (t = 1; t <= CAP_TICKS; t++) {
        arena_tick(&s, in);
        if (s.players[0].pos.x >= endx) break;
    }
    out->traverse_capped = (t > CAP_TICKS);
    out->traverse_ticks  = out->traverse_capped ? CAP_TICKS : t;
}
```

Then extend `tune_probes_run`:

```c
void tune_probes_run(TuneMetrics* out) {
    memset(out, 0, sizeof(*out));
    probe_ramp(out);
    probe_stop(out);
    probe_turns(out);
    probe_jumps(out);
    probe_traverse(out);
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_tr \
    src/arena/arena_sim.c tools/tune_probes.c tests/test_tune_report.c && /tmp/t_tr
```

Expected: `ALL TUNE REPORT TESTS PASSED`.

If `turn180_ticks` comes back as 1 or 2, you have the `sy` sign backwards — re-read the stick-convention constraint in Global Constraints.

- [ ] **Step 5: Confirm the sim is still untouched**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/ah tools/arena_hash.c src/arena/arena_sim.c && /tmp/ah
```

Expected: `18fbf1bb`.

- [ ] **Step 6: Commit**

```bash
git add tools/tune_probes.c tests/test_tune_report.c
git commit -m "feat(tools): add turn, jump and traverse probes

turn180/turn90 + turn radius (the turn-rate knob), standing and running jump
arcs, and arena crossing time (the 60-vs-30Hz question). Turn probe uses
sy=-31 for the genuine reversal - sy=+31 resolves to the start yaw and would
silently measure a zero-tick turn."
```

---

## Task 5: Report `main` and the sweep driver

**Files:**
- Create: `tools/tune_report.c`
- Create: `tools/tune-report.ps1`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3/4's `tune_probes.h` + `tune_probes_run`.
- Produces: a `tune_report` executable emitting TSV (default) or JSON (`--json`). TSV format is exactly `name<TAB>value<TAB>unit`, one metric per line, no header row, plus `#`-prefixed comment lines echoing the tuning it was built with. Task 6 pins this output.

- [ ] **Step 1: Write the report main**

Create `tools/tune_report.c`:

```c
/* Thin formatter over tune_probes. One tune in (the one this binary was
 * compiled with), one table out. Sweeping across tunes is tools/tune-report.ps1's
 * job — this stays dumb on purpose.
 *
 * Output precision is FIXED (3dp for distances/speeds, integers for ticks)
 * because tools/tune_metrics.baseline is diffed across six CI legs and raw
 * double formatting would drift in the last digit. */
#include <stdio.h>
#include <string.h>
#include "tune_probes.h"
#include "../src/arena/arena_tuning.h"
#include "../src/arena/arena_math.h"

static int json = 0;

static void row_f(const char* name, double v, const char* unit) {
    if (json) printf("  \"%s\": %.3f,\n", name, v);
    else      printf("%s\t%.3f\t%s\n", name, v, unit);
}
static void row_i(const char* name, int v, const char* unit) {
    if (json) printf("  \"%s\": %d,\n", name, v);
    else      printf("%s\t%d\t%s\n", name, v, unit);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--tsv")) json = 0;
        else { fprintf(stderr, "usage: tune_report [--tsv|--json]\n"); return 2; }
    }

    TuneMetrics m;
    tune_probes_run(&m);

    if (json) printf("{\n");
    else {
        /* self-describing: the tune this binary measures */
        printf("# TUNE_VERSION\t%d\n", TUNE_VERSION);
        printf("# TUNE_RUN_SPEED\t%d\n", TUNE_RUN_SPEED);
        printf("# TUNE_RUN_ACCEL\t%d\n", TUNE_RUN_ACCEL);
        printf("# TUNE_RUN_FRICTION\t%d\n", TUNE_RUN_FRICTION);
        printf("# TUNE_TURN_RATE\t%d\n", TUNE_TURN_RATE);
        printf("# TUNE_JUMP_IMPULSE\t%d\n", TUNE_JUMP_IMPULSE);
        printf("# TUNE_GRAVITY\t%d\n", TUNE_GRAVITY);
        printf("# TUNE_AIR_CONTROL\t%d\n", TUNE_AIR_CONTROL);
    }

    row_f("top_speed",        m.top_speed,        "u/s");
    row_i("ticks_to_90pct",   m.ticks_to_90pct,   "ticks");
    row_f("ramp_distance",    m.ramp_distance,    "u");
    row_f("stop_distance",    m.stop_distance,    "u");
    row_i("stop_ticks",       m.stop_ticks,       "ticks");
    row_i("turn180_ticks",    m.turn180_ticks,    "ticks");
    row_i("turn90_ticks",     m.turn90_ticks,     "ticks");
    row_f("turn_radius",      m.turn_radius,      "u");
    row_f("jump_apex",        m.jump_apex,        "u");
    row_i("jump_airtime",     m.jump_airtime,     "ticks");
    row_f("runjump_distance", m.runjump_distance, "u");
    row_i("runjump_airtime",  m.runjump_airtime,  "ticks");
    row_i("traverse_ticks",   m.traverse_ticks,   "ticks");

    if (json) printf("  \"capped\": %d\n}\n",
                     m.ramp_capped || m.stop_capped || m.turn_capped || m.traverse_capped);
    else if (m.ramp_capped || m.stop_capped || m.turn_capped || m.traverse_capped)
        printf("# WARNING\tone or more probes hit the iteration cap\n");
    return 0;
}
```

- [ ] **Step 2: Build and eyeball it**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/tune_report \
    src/arena/arena_sim.c tools/tune_probes.c tools/tune_report.c && /tmp/tune_report
```

Expected: the `#`-comment block then 13 metric rows, no `WARNING` line. Sanity-check `top_speed` ≈ `0.151 × 60 = 9.06` u/s and `turn180_ticks` ≈ `0x8000/728 = 45`.

- [ ] **Step 3: Verify determinism of the output**

```bash
/tmp/tune_report > /tmp/r1.tsv && /tmp/tune_report > /tmp/r2.tsv && diff /tmp/r1.tsv /tmp/r2.tsv && echo IDENTICAL
```

Expected: `IDENTICAL`.

- [ ] **Step 4: Write the sweep driver**

Create `tools/tune-report.ps1`:

```powershell
# Feel-metrics sweep driver. Builds tune_report once per tuning variant and
# prints a side-by-side comparison table.
#
#   tools\tune-report.ps1
#   tools\tune-report.ps1 -Compare base,friction=0.020,friction=0.030,friction=0.045
#   tools\tune-report.ps1 -SelfTest
#
# Variant syntax is <knob>=<value>; "base" means default tuning. Values are
# wrapped in Q(...) automatically except for raw-integer knobs. This script
# owns the -D quoting so callers never fight PowerShell escaping.
param(
    [string[]] $Compare = @("base"),
    [string]   $Cc      = "gcc",
    [string]   $Csv     = "",
    [switch]   $SelfTest
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$work = Join-Path ([IO.Path]::GetTempPath()) "tune-report"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# short name -> macro. Raw-integer knobs are NOT wrapped in Q().
$KNOBS = @{
    friction = @{ macro = "TUNE_RUN_FRICTION"; raw = $false }
    accel    = @{ macro = "TUNE_RUN_ACCEL";    raw = $false }
    top      = @{ macro = "TUNE_RUN_SPEED";    raw = $false }
    air      = @{ macro = "TUNE_AIR_CONTROL";  raw = $false }
    gravity  = @{ macro = "TUNE_GRAVITY";      raw = $false }
    jump     = @{ macro = "TUNE_JUMP_IMPULSE"; raw = $false }
    turn     = @{ macro = "TUNE_TURN_RATE";    raw = $true  }
}

function Build-Variant([string]$variant) {
    $defs = @()
    if ($variant -ne "base") {
        foreach ($part in $variant.Split(",")) {
            $kv = $part.Split("=", 2)
            if ($kv.Count -ne 2) { throw "bad variant '$part' (expected knob=value)" }
            $k = $kv[0].Trim(); $v = $kv[1].Trim()
            if (-not $KNOBS.ContainsKey($k)) {
                throw "unknown knob '$k'. Valid: $($KNOBS.Keys -join ', ')"
            }
            $macro = $KNOBS[$k].macro
            $val   = if ($KNOBS[$k].raw) { $v } else { "Q($v)" }
            $defs += "-D$macro=$val"
        }
    }
    $exe = Join-Path $work ("tr_" + ($variant -replace '[^A-Za-z0-9]', '_') + ".exe")
    $args = @("-std=c11","-Wall","-Wextra","-Werror","-O2") + $defs + @(
        "-o", $exe,
        (Join-Path $root "src\arena\arena_sim.c"),
        (Join-Path $root "tools\tune_probes.c"),
        (Join-Path $root "tools\tune_report.c"))
    & $Cc @args
    if ($LASTEXITCODE -ne 0) { throw "build failed for variant '$variant'" }
    return $exe
}

function Get-Metrics([string]$exe) {
    $m = [ordered]@{}
    foreach ($line in (& $exe)) {
        if ($line -match '^#') { continue }
        $c = $line -split "`t"
        if ($c.Count -ge 2) { $m[$c[0]] = $c[1] }
    }
    return $m
}

if ($SelfTest) {
    # Cross-tune monotonicity: compile-time constants can't vary inside one
    # binary, so this property is only checkable at the script level.
    $lo = Get-Metrics (Build-Variant "friction=0.015")
    $hi = Get-Metrics (Build-Variant "friction=0.060")
    $ok = [double]$lo["stop_distance"] -gt [double]$hi["stop_distance"]
    Write-Host ("SELFTEST stop_distance: friction 0.015 -> {0}u, 0.060 -> {1}u : {2}" -f `
        $lo["stop_distance"], $hi["stop_distance"], $(if ($ok) { "PASS" } else { "FAIL" }))
    $g1 = Get-Metrics (Build-Variant "gravity=0.0175")
    $g2 = Get-Metrics (Build-Variant "gravity=0.0350")
    $ok2 = [double]$g1["jump_apex"] -gt [double]$g2["jump_apex"]
    Write-Host ("SELFTEST jump_apex: gravity 0.0175 -> {0}u, 0.0350 -> {1}u : {2}" -f `
        $g1["jump_apex"], $g2["jump_apex"], $(if ($ok2) { "PASS" } else { "FAIL" }))
    if ($ok -and $ok2) { Write-Host "SELFTEST PASS"; exit 0 }
    Write-Host "SELFTEST FAIL"; exit 1
}

$cols = [ordered]@{}
foreach ($v in $Compare) { $cols[$v] = Get-Metrics (Build-Variant $v) }

$metrics = @($cols[$Compare[0]].Keys)
$w = 18
$hdr = "metric".PadRight($w) + (($Compare | ForEach-Object { $_.PadLeft(12) }) -join "")
Write-Host $hdr
Write-Host ("-" * $hdr.Length)
foreach ($k in $metrics) {
    $row = $k.PadRight($w)
    foreach ($v in $Compare) { $row += ($cols[$v][$k]).PadLeft(12) }
    Write-Host $row
}

if ($Csv) {
    $out = @(("metric," + ($Compare -join ",")))
    foreach ($k in $metrics) {
        $out += (@($k) + @($Compare | ForEach-Object { $cols[$_][$k] })) -join ","
    }
    $out | Set-Content -Path $Csv -Encoding UTF8
    Write-Host "`nwrote $Csv"
}
```

- [ ] **Step 5: Run the motivating sweep**

```powershell
powershell -ExecutionPolicy Bypass -File tools\tune-report.ps1 -Compare base,friction=0.020,friction=0.030,friction=0.045
```

Expected: a four-column table. `stop_distance` must decrease left-to-right across the three friction values; every other metric should be identical across all columns (friction touches nothing else). `friction=0.030` must match `base` exactly, since `Q(0.030)` is the current default.

That last equality is a strong end-to-end check of the whole `-D` override path — if `base` and `friction=0.030` differ, the guards from Task 1 are not being honored.

- [ ] **Step 6: Run the self-test**

```powershell
powershell -ExecutionPolicy Bypass -File tools\tune-report.ps1 -SelfTest
```

Expected: `SELFTEST PASS`.

- [ ] **Step 7: Verify the unknown-knob error path**

```powershell
powershell -ExecutionPolicy Bypass -File tools\tune-report.ps1 -Compare base,wobble=1.0
```

Expected: a thrown error reading `unknown knob 'wobble'. Valid: friction, accel, top, air, gravity, jump, turn` (key order may vary).

- [ ] **Step 8: Wire the unit test into CMake**

In `CMakeLists.txt`, after the `test_bomb_mechanics` block, add:

```cmake
add_executable(test_tune_report tests/test_tune_report.c tools/tune_probes.c)
target_link_libraries(test_tune_report arena_sim)
add_test(NAME tune_report COMMAND test_tune_report)
```

Note: `tune_probes.c` is compiled into the test target directly rather than linked from a library — it is tools-side code with floats, deliberately kept out of the `arena_sim` target which carries `-Wconversion`.

- [ ] **Step 9: Verify via ctest**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target test_tune_report && ctest --test-dir build -R tune_report --output-on-failure
```

Expected: 1 test, passed.

- [ ] **Step 10: Commit**

```bash
git add tools/tune_report.c tools/tune-report.ps1 CMakeLists.txt
git commit -m "feat(tools): tune_report formatter + variant sweep driver

tune_report emits fixed-precision TSV/JSON for the tune it was built with;
tune-report.ps1 sweeps variants (knob=value, Q() wrapping and quoting handled
for you) and prints a comparison table. -SelfTest asserts cross-tune
monotonicity (lower friction => longer stop; higher gravity => lower apex),
which can't be checked inside a single binary. Wired into ctest."
```

---

## Task 6: Metrics baseline as a regression test

Turns an opaque hash change into a readable diff of what actually moved.

**Files:**
- Create: `tools/tune_metrics.baseline`
- Modify: `.github/workflows/determinism.yml`

**Interfaces:**
- Consumes: Task 5's `tune_report` TSV output; Task 2's pin-file convention.
- Produces: `tools/tune_metrics.baseline`, rewritten by Task 7's `repin.ps1`.

- [ ] **Step 1: Generate the baseline**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/tune_report \
    src/arena/arena_sim.c tools/tune_probes.c tools/tune_report.c
/tmp/tune_report > tools/tune_metrics.baseline
cat tools/tune_metrics.baseline
```

- [ ] **Step 2: Verify it is stable across optimization levels**

```bash
for o in -O0 -O2 -O3; do
  gcc -std=c11 -Werror $o -o /tmp/tr_$$ src/arena/arena_sim.c tools/tune_probes.c tools/tune_report.c
  /tmp/tr_$$ > /tmp/base_$$.tsv
  diff -q tools/tune_metrics.baseline /tmp/base_$$.tsv && echo "$o OK"
done
```

Expected: `-O0 OK`, `-O2 OK`, `-O3 OK`.

**If any leg differs**, the differing row is float-unstable. Do **not** loosen the comparison — remove that single row from the report (and from the baseline), and note why in the commit. Tick-count rows are integer-exact and must never be the unstable ones; if a tick row differs, that is a real determinism bug in the sim and must be investigated, not worked around.

- [ ] **Step 3: Add the CI step**

In `.github/workflows/determinism.yml`, immediately after the `pin scripted-match hash` step, add:

```yaml
      # Human-readable companion to the hash pin: the hash proves SOMETHING
      # changed, this shows WHAT. Regenerate via tools/repin.ps1 on an
      # intentional tune.
      - name: feel-metrics baseline
        shell: bash
        run: |
          ${{ matrix.cc }} -std=c11 -Wall -Wextra -Werror ${{ matrix.opt }} \
            -o tune_report src/arena/arena_sim.c tools/tune_probes.c tools/tune_report.c
          ./tune_report > metrics.now
          if diff -u tools/tune_metrics.baseline metrics.now; then
            echo "OK - feel metrics unchanged"
          else
            echo "::error::Feel metrics moved. If this was an intentional tune, run"
            echo "::error::tools/repin.ps1 and commit the updated tools/tune_metrics.baseline"
            echo "::error::(and tools/pinned_hash.txt). If not, you changed the movement"
            echo "::error::model by accident - the diff above shows exactly which metric."
            exit 1
          fi
```

- [ ] **Step 4: Add the unit test to CI too**

The `test_tune_report` suite is not yet in the workflow. After the `movement model tests` step, add:

```yaml
      - name: feel-metrics probe tests
        shell: bash
        run: |
          ${{ matrix.cc }} -std=c11 -Wall -Wextra -Werror ${{ matrix.opt }} \
            -o test_tr src/arena/arena_sim.c tools/tune_probes.c tests/test_tune_report.c
          ./test_tr
```

- [ ] **Step 5: Prove the guard actually catches a change**

Temporarily perturb a constant and confirm the diff fires:

```bash
gcc -std=c11 -Werror -O2 -DTUNE_RUN_FRICTION='Q(0.045)' \
    -o /tmp/tr_perturb src/arena/arena_sim.c tools/tune_probes.c tools/tune_report.c
/tmp/tr_perturb > /tmp/perturbed.tsv
diff -u tools/tune_metrics.baseline /tmp/perturbed.tsv
```

Expected: a non-zero exit with `stop_distance` and `stop_ticks` visibly smaller, and the `# TUNE_RUN_FRICTION` comment row changed. Nothing to revert — the perturbation only ever existed in a `-D` flag.

- [ ] **Step 6: Commit**

```bash
git add tools/tune_metrics.baseline .github/workflows/determinism.yml
git commit -m "test(ci): pin the feel-metrics baseline alongside the hash

The hash pin proves something changed; this shows WHAT (e.g. 'stop_distance
0.61 -> 0.41u'), which distinguishes a deliberate tune from a fat-fingered
constant. Verified byte-stable across -O0/-O2/-O3, and verified to fire on a
perturbed friction value. Also wires the probe unit tests into every matrix leg."
```

---

## Task 7: `repin.ps1`

**Files:**
- Create: `tools/repin.ps1`

**Interfaces:**
- Consumes: `tools/arena_hash.c`, `tools/tune_report.c`, `tools/pinned_hash.txt`, `tools/tune_metrics.baseline`, and `TUNE_VERSION` in `src/arena/arena_tuning.h`.
- Produces: rewritten `pinned_hash.txt` and `tune_metrics.baseline`.

- [ ] **Step 1: Write the script**

Create `tools/repin.ps1`:

```powershell
# Re-pin the scripted-match hash and the feel-metrics baseline after an
# INTENTIONAL gameplay change. Shows old -> new for both before writing.
#
#   tools\repin.ps1            # normal use
#   tools\repin.ps1 -Force     # tooling-only hash correction (no gameplay change)
#
# Refuses to run when the hash moved but TUNE_VERSION did not — that is exactly
# the invariant-#4 violation the pin exists to catch. Bump TUNE_VERSION first.
param([string]$Cc = "gcc", [switch]$Force)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$pinFile  = Join-Path $root "tools\pinned_hash.txt"
$baseFile = Join-Path $root "tools\tune_metrics.baseline"
$work = Join-Path ([IO.Path]::GetTempPath()) "repin"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$pin = (Get-Content $pinFile -Raw).Trim() -split '\s+'
$oldVer = $pin[0]; $oldHash = $pin[1]

$tuning = Get-Content (Join-Path $root "src\arena\arena_tuning.h") -Raw
if ($tuning -notmatch '(?m)^\s*#define\s+TUNE_VERSION\s+(\d+)') {
    throw "could not read TUNE_VERSION from src/arena/arena_tuning.h"
}
$newVer = $Matches[1]

$hashExe = Join-Path $work "arena_hash.exe"
& $Cc -std=c11 -Wall -Wextra -Werror -O2 -o $hashExe `
    (Join-Path $root "tools\arena_hash.c") (Join-Path $root "src\arena\arena_sim.c")
if ($LASTEXITCODE -ne 0) { throw "hash generator build failed" }
$newHash = (& $hashExe).Trim()

Write-Host "TUNE_VERSION : $oldVer -> $newVer"
Write-Host "hash         : $oldHash -> $newHash"

if ($newHash -eq $oldHash -and -not $Force) {
    Write-Host "`nHash unchanged - nothing to repin. (Metrics baseline left alone.)"
    exit 0
}
if ($newHash -ne $oldHash -and $newVer -eq $oldVer -and -not $Force) {
    Write-Error @"

REFUSING TO REPIN: the sim hash changed but TUNE_VERSION is still $oldVer.

A gameplay change must bump TUNE_VERSION - it is folded into the netcode
version handshake, and peers on different tuning must not be able to match.
Bump TUNE_VERSION in src/arena/arena_tuning.h, then re-run this script.

(-Force overrides, for a tooling-only hash-generator correction with no
gameplay change.)
"@
    exit 1
}

$reportExe = Join-Path $work "tune_report.exe"
& $Cc -std=c11 -Wall -Wextra -Werror -O2 -o $reportExe `
    (Join-Path $root "src\arena\arena_sim.c") `
    (Join-Path $root "tools\tune_probes.c") `
    (Join-Path $root "tools\tune_report.c")
if ($LASTEXITCODE -ne 0) { throw "tune_report build failed" }
$newMetrics = & $reportExe

Write-Host "`n--- feel-metrics diff ---"
$diff = Compare-Object (Get-Content $baseFile) $newMetrics
if ($diff) { $diff | Format-Table -AutoSize | Out-String | Write-Host }
else { Write-Host "(no metric changed)" }

"$newVer $newHash" | Set-Content -Path $pinFile -Encoding ASCII
$newMetrics | Set-Content -Path $baseFile -Encoding ASCII

Write-Host "`nRepinned. Commit tools/pinned_hash.txt and tools/tune_metrics.baseline."
```

- [ ] **Step 2: Verify the no-op path**

```powershell
powershell -ExecutionPolicy Bypass -File tools\repin.ps1
```

Expected: `TUNE_VERSION : 6 -> 6`, `hash : 18fbf1bb -> 18fbf1bb`, then `Hash unchanged - nothing to repin.` and exit 0. Confirm `git status` shows **no** modification to `pinned_hash.txt` or `tune_metrics.baseline`.

- [ ] **Step 3: Verify the refusal path**

Temporarily edit `src/arena/arena_tuning.h` to change `TUNE_RUN_FRICTION` to `Q(0.045)` **without** touching `TUNE_VERSION`, then:

```powershell
powershell -ExecutionPolicy Bypass -File tools\repin.ps1
```

Expected: exit 1 with the `REFUSING TO REPIN` message. Confirm neither pin file was modified, then **revert the tuning edit**:

```bash
git checkout src/arena/arena_tuning.h
```

- [ ] **Step 4: Verify the accept path**

Make the same friction edit **and** bump `TUNE_VERSION` to `7`, then run `repin.ps1`. Expected: the metrics diff shows `stop_distance`/`stop_ticks` moving, and both pin files are rewritten. Then revert everything:

```bash
git checkout src/arena/arena_tuning.h tools/pinned_hash.txt tools/tune_metrics.baseline
```

Confirm `tools/pinned_hash.txt` reads `6 18fbf1bb` again.

- [ ] **Step 5: Full local gate**

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_det src/arena/arena_sim.c tests/test_determinism.c && /tmp/t_det
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_mv  src/arena/arena_sim.c tests/test_movement.c && /tmp/t_mv
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/t_tr  src/arena/arena_sim.c tools/tune_probes.c tests/test_tune_report.c && /tmp/t_tr
gcc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/ah    tools/arena_hash.c src/arena/arena_sim.c && /tmp/ah
```

Expected: all pass, hash `18fbf1bb`.

- [ ] **Step 6: Commit**

```bash
git add tools/repin.ps1
git commit -m "feat(tools): repin.ps1 - regenerate hash + metrics baseline safely

Shows old->new for both pins before writing. Refuses to repin when the hash
moved but TUNE_VERSION did not (invariant #4), with -Force for a tooling-only
generator correction. Verified on all three paths: no-op, refusal, accept."
```

---

## Task 8: Fork one-command build

**Files:**
- Create: `C:\Users\dshi\GitRepos\BMHeroRecomp\build.ps1`

**Interfaces:**
- Consumes: nothing from Tasks 1–7 (the fork build is independent).
- Produces: `build.ps1` exiting 0 only on a successful build (and, with `-Soak N`, a green soak on that exact build).

- [ ] **Step 1: Create the branch**

```bash
cd C:/Users/dshi/GitRepos/BMHeroRecomp
git checkout -b feature/build-and-soak-tooling
```

- [ ] **Step 2: Write the script**

Create `build.ps1` in the fork root:

```powershell
# One-command build for the recomp fork. Composes the three-toolchain PATH,
# rebuilds patches/ when stale, builds, and optionally soaks.
#
#   .\build.ps1                     # rwdi build, auto patch rebuild
#   .\build.ps1 -Soak 5             # ...then 5 boot-soak iterations
#   .\build.ps1 -Config cmake       # the plain Release build dir
#   .\build.ps1 -Patches always     # force the patches rebuild
#
# PATH ORDER IS LOAD-BEARING (see CLAUDE.md build recipe):
#   LLVM15 first  - patches/ needs clang/ld.lld v15; VS clang-19 has no MIPS
#                   backend and MSYS2's LLVM-22 lld rejects the old flags.
#   VS dev shell  - MSVC link.exe must precede msys.
#   MSYS2 last    - make from usr/bin, plus the N64Recomp runtime DLLs.
param(
    [ValidateSet("rwdi","cmake")] [string] $Config  = "rwdi",
    [ValidateSet("auto","always","never")] [string] $Patches = "auto",
    [int] $Soak = 0,
    [string] $Llvm15 = $(if ($env:BMHERO_LLVM15) { $env:BMHERO_LLVM15 } else { "C:\Users\dshi\GitRepos\.tools\llvm15" }),
    [string] $Msys   = $(if ($env:BMHERO_MSYS)   { $env:BMHERO_MSYS   } else { "C:\msys64" })
)
$ErrorActionPreference = "Stop"
$root    = $PSScriptRoot
$buildDir = Join-Path $root $(if ($Config -eq "rwdi") { "build-rwdi" } else { "build-cmake" })

function Fail($msg) { Write-Host "`nBUILD FAILED: $msg" -ForegroundColor Red; exit 1 }

# --- 1. toolchains -----------------------------------------------------------
if (-not (Test-Path (Join-Path $Llvm15 "bin\clang.exe"))) {
    Fail "LLVM 15 not found at '$Llvm15' (need bin\clang.exe). patches/ requires clang-15 - VS clang has no MIPS backend. Override with -Llvm15 or `$env:BMHERO_LLVM15."
}
if (-not (Test-Path (Join-Path $Msys "usr\bin\make.exe"))) {
    Fail "MSYS2 not found at '$Msys' (need usr\bin\make.exe). Override with -Msys or `$env:BMHERO_MSYS."
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "vswhere.exe not found - is Visual Studio 2022 installed?" }
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { Fail "no VS installation with the x64 C++ toolset found" }

Write-Host "toolchains: LLVM15=$Llvm15  VS=$vsPath  MSYS2=$Msys"

$devShell = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
Set-Location $root

$env:PATH = (Join-Path $Llvm15 "bin") + ";" + $env:PATH + ";" +
            (Join-Path $Msys "ucrt64\bin") + ";" + (Join-Path $Msys "usr\bin")

# --- 2. patches --------------------------------------------------------------
$patchDir = Join-Path $root "patches"
$patchBin = Join-Path $patchDir "patches.bin"
$doPatches = switch ($Patches) {
    "always" { $true }
    "never"  { $false }
    default  {
        if (-not (Test-Path $patchBin)) { $true }
        else {
            $binTime = (Get-Item $patchBin).LastWriteTime
            $newer = Get-ChildItem "$patchDir\*" -Include *.c,*.h -File |
                     Where-Object { $_.LastWriteTime -gt $binTime }
            if ($newer) {
                Write-Host "stale patch sources: $(($newer | ForEach-Object Name) -join ', ')"
                $true
            } else { $false }
        }
    }
}
if ($doPatches) {
    # ninja does NOT reliably re-run the patch make; a stale patches.bin is a
    # mismatch crash at boot, so clean is mandatory here, not just make.
    Write-Host "`n=== patches: make clean && make ==="
    Push-Location $patchDir
    & make clean; if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "make clean failed" }
    & make;       if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "patches make failed" }
    Pop-Location
} else {
    Write-Host "patches up to date - skipping (use -Patches always to force)"
}

# --- 3. build ----------------------------------------------------------------
if (-not (Test-Path $buildDir)) { Fail "build dir '$buildDir' does not exist - configure it with cmake first" }
Write-Host "`n=== cmake --build $buildDir --target BMHeroRecompiled ==="
& cmake --build $buildDir --target BMHeroRecompiled
if ($LASTEXITCODE -ne 0) { Fail "cmake build failed" }

$exe = Join-Path $buildDir "BMHeroRecompiled.exe"
if (-not (Test-Path $exe)) { Fail "build reported success but $exe is missing" }
Write-Host "`nBUILD OK: $exe ($([math]::Round((Get-Item $exe).Length / 1MB, 1)) MB)"

# --- 4. soak -----------------------------------------------------------------
if ($Soak -gt 0) {
    if ($Config -ne "rwdi") { Fail "-Soak requires -Config rwdi (arena-soak.ps1 launches build-rwdi)" }
    Write-Host "`n=== boot soak: $Soak iterations ==="
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\arena-soak.ps1") -N $Soak
    $soakFails = $LASTEXITCODE
    if ($soakFails -ne 0) { Fail "$soakFails soak iteration(s) failed - do NOT hand this build over" }
    Write-Host "`nSOAK GREEN - this exact build is cleared for handoff." -ForegroundColor Green
}
exit 0
```

- [ ] **Step 3: Verify a clean build**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Expected: the toolchain line naming all three paths, `patches up to date - skipping`, a successful cmake build, and `BUILD OK`.

- [ ] **Step 4: Verify the stale-patch path fires**

```powershell
(Get-Item patches\arena_render.c).LastWriteTime = Get-Date
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Expected: `stale patch sources: arena_render.c` followed by the `make clean && make` block, then a successful build.

- [ ] **Step 5: Verify the missing-toolchain error path**

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Llvm15 C:\nope
```

Expected: exit 1 with the message naming LLVM 15 and explaining why clang-15 specifically is required. No build attempted.

- [ ] **Step 6: Verify the soak chain**

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Soak 3
```

Expected: build, then three soak iterations, then `SOAK GREEN`. Exit code 0.

**This step needs the ROM and a working display** — it launches the game three times. If the soak reports failures, that is a real regression in the current fork branch, not a bug in `build.ps1`; report it rather than working around it.

- [ ] **Step 7: Commit**

```bash
git add build.ps1
git commit -m "build: one-command fork build (PATH + patches + build + soak)

Composes the load-bearing three-toolchain PATH (LLVM15 first for the MIPS
patches, VS dev shell before msys for link.exe, MSYS2 last for make),
auto-detects stale patches/*.c and does the mandatory make clean (a stale
patches.bin is a boot-time mismatch crash), builds, and with -Soak N chains
arena-soak.ps1 and fails unless it is green.

That last part makes 'no build reaches the human without a green soak on that
exact build' a property of the tool rather than a rule to remember."
```

---

## Task 9: Generic soak gates

**Files:**
- Modify: `C:\Users\dshi\GitRepos\BMHeroRecomp\tools\arena-soak.ps1`

**Interfaces:**
- Consumes: the existing `arena_bridge.log` marker convention.
- Produces: `-Expect '<regex>'` and `-Rising '<regex with one capture group>'` parameters. `-AnimProbe` is preserved as an alias so the A1.4 gate and any existing invocation keep working.

- [ ] **Step 1: Add the parameters**

In `tools/arena-soak.ps1`, extend the `param(...)` block:

```powershell
param([int]$N = 10, [int]$TimeoutSec = 75, [switch]$Probe, [switch]$AnimProbe,
      [string]$Expect = "", [string]$Rising = "", [int]$Mode = 0)
```

and add to the comment header:

```powershell
# -Expect '<regex>'  : fail the run unless the pattern appears in arena_bridge.log.
# -Rising '<regex>'  : pattern must match >=2 times with its first capture group
#                      strictly increasing — the "it actually PLAYED, not just got
#                      set for one frame" check.
# -Mode <n>          : ARENA_AUTO_BATTLE value (default 1, or 3/4 for -Probe/-AnimProbe).
```

- [ ] **Step 2: Make `-AnimProbe` an alias**

Immediately after the `param(...)` block, before `if ($Probe -or $AnimProbe) { $N = 1 }`, insert:

```powershell
# -AnimProbe is now sugar over the generic gates: ARENA_AUTO_BATTLE=4 plus a
# rising-frame assertion on the set-bomb pose (code_extra_0 anim 29). Kept as a
# named switch because it is the A1.4 objective gate and is referenced in docs.
if ($AnimProbe -and -not $Rising) { $Rising = 'idx=29 frame=(\d+)' }
if ($AnimProbe -and $Mode -eq 0)  { $Mode = 4 }
if ($Probe     -and $Mode -eq 0)  { $Mode = 3 }
if ($Mode -eq 0) { $Mode = 1 }
if ($Expect -or $Rising) { $N = 1 }
```

- [ ] **Step 3: Use `$Mode` when launching**

Replace the three-line mode selection:

```powershell
    if ($AnimProbe)  { $env:ARENA_AUTO_BATTLE = "4" }
    elseif ($Probe)  { $env:ARENA_AUTO_BATTLE = "3" }
    else             { $env:ARENA_AUTO_BATTLE = "1" }
```

with:

```powershell
    $env:ARENA_AUTO_BATTLE = "$Mode"
```

- [ ] **Step 4: Generalize the dwell condition**

Replace:

```powershell
    if (($Probe -or $AnimProbe) -and $verdict -eq "PASS") { Start-Sleep -Seconds 10 }
```

with:

```powershell
    if (($Probe -or $AnimProbe -or $Expect -or $Rising) -and $verdict -eq "PASS") {
        Start-Sleep -Seconds 10   # let the injected input sample land in the log
    }
```

- [ ] **Step 5: Replace the AnimProbe gate block with the generic gates**

Replace the entire `if ($AnimProbe) { ... }` block at the end of the file with:

```powershell
if ($Expect) {
    $hit = (Test-Path $log) -and (Select-String -Path $log -Pattern $Expect -Quiet)
    Write-Host ("EXPECT GATE: /{0}/ -> {1}" -f $Expect, $(if ($hit) { 'PASS' } else { 'FAIL' }))
    if (-not $hit) { $fails++ }
}

if ($Rising) {
    # The pattern's first capture group must appear >=2 times and strictly
    # increase — proves the thing kept advancing rather than firing once.
    $vals = @()
    if (Test-Path $log) {
        $vals = @(Select-String -Path $log -Pattern $Rising |
                  ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    }
    $ok = ($vals.Count -ge 2) -and ($vals[-1] -gt $vals[0])
    Write-Host ("RISING GATE: /{0}/ samples={1} values=[{2}] -> {3}" -f `
        $Rising, $vals.Count, ($vals -join ','), $(if ($ok) { 'PASS' } else { 'FAIL' }))
    if (-not $ok) { $fails++ }
}

exit $fails
```

Delete the now-duplicated trailing `exit $fails` if one remains.

- [ ] **Step 6: Verify the `-AnimProbe` alias still passes**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
powershell -ExecutionPolicy Bypass -File tools\arena-soak.ps1 -AnimProbe
```

Expected: one iteration, `PASS`, then `RISING GATE: /idx=29 frame=(\d+)/ samples=N values=[...] -> PASS`. This must reproduce the A1.4 gate result on the current build — same outcome as before the refactor, just reported through the generic path.

- [ ] **Step 7: Verify a plain soak is unaffected**

```powershell
powershell -ExecutionPolicy Bypass -File tools\arena-soak.ps1 -N 2
```

Expected: two iterations, `SUMMARY: 2/2 PASS`, no gate lines, exit 0.

- [ ] **Step 8: Verify a fresh gate needs no harness edit**

```powershell
powershell -ExecutionPolicy Bypass -File tools\arena-soak.ps1 -Mode 4 -Expect '\[capture\]'
```

Expected: `EXPECT GATE: /\[capture\]/ -> PASS`. This demonstrates the point of the task — a new objective check took a command-line argument instead of edits to two files.

- [ ] **Step 9: Commit**

```bash
git add tools/arena-soak.ps1
git commit -m "test(soak): generic -Expect / -Rising gates; -AnimProbe becomes an alias

A new objective probe now needs only its ARENA_AUTO_BATTLE mode in main.cpp,
not edits to the harness as well. -AnimProbe is preserved as sugar over
-Mode 4 -Rising 'idx=29 frame=(\d+)' and reproduces the A1.4 gate unchanged."
```

---

## Wrap-up

After Task 9:

- [ ] Push both branches. Confirm the `determinism` and `netcode` CI jobs are green on `feature/tuning-loop-toolkit` — in particular that all six matrix legs agree on both the hash pin and the metrics baseline.
- [ ] Update `CLAUDE.md`'s Current status with a short paragraph recording the toolkit and the new loop (`tune-report.ps1 -Compare …` → `repin.ps1` → fork `build.ps1 -Soak 5`), and note that `tools/pinned_hash.txt` now holds the version alongside the hash.
- [ ] Report the sweep table for the open knobs (friction, turn rate) so the next tuning decision starts from numbers rather than a boot.

## Self-review notes

Checked against the spec:

- Spec A.1 guards → Task 1. A.2 tool → Tasks 3+4 (split: core+2 probes, then 5 probes). A.3 driver → Task 5.
- Spec B baseline → Task 6, including the fixed-precision requirement and the "drop the row, don't loosen the diff" instruction.
- Spec C.1/C.2/C.3/C.4 → Tasks 2 and 7.
- Spec D.1 `build.ps1` → Task 8. D.2 soak gates → Task 9.
- Spec's testing table → each row appears as a numbered verification step in its task.

One deviation from the spec, recorded deliberately: the spec put monotonicity assertions in `tests/test_tune_report.c`, but compile-time constants cannot vary within a single binary, so cross-tune monotonicity moved to `tune-report.ps1 -SelfTest` (Task 5, Step 6) and the C test asserts same-binary consistency against the `TUNE_*` macros instead. This is why the probe library is split from the report `main`.
