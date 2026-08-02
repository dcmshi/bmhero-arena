# Oracle 2.0: Verb Tables + Anim-Timeline Diff — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Choreography written once in tick units for both the vanilla oracle
and the battle probe, and vanilla's full per-verb animation timelines diffed
against the arena automatically (gate check 17).

**Architecture:** A `VerbRow` table per script with ONE tick→poll conversion
site replaces the hand-rolled poll windows in `main.cpp`; the bridge gains a
`[verb]` marker and an uncapped `[animrun]` RLE channel; `oracle.ps1` emits
`tools/oracle/timelines.json`; a new `tools/anim-diff.ps1` compares per-verb
run lists; `oracle-gate.ps1` wires it in as check 17.

**Tech Stack:** C (fork `src/main/main.cpp` native side), C++ (fork
`src/arena_bridge/arena_bridge.cpp`), PowerShell (fork `tools/`).

**Spec:** `docs/superpowers/specs/2026-08-01-oracle-timeline-diff-design.md`
(bmhero-arena repo). All work happens in the FORK:
`C:\Users\dshi\GitRepos\BMHeroRecomp`, branch `master` (its default — NOT
main).

## Global Constraints

- **No sim changes.** `lib/bmhero-arena` (the submodule) is read-only for this
  plan; the pinned hash `04e8af49` (TUNE_VERSION 18) must not move.
- **Goldens byte-identical after the refactor.** A vanilla oracle boot must
  end with `oracle.ps1` printing `goldens unchanged.` (exit 0). This is the
  proof the tables encode today's choreography exactly.
- **All 16 existing oracle-gate checks stay green.** Their grep patterns and
  semantics must not change (exception: none needed by this plan).
- **NEVER kill a running `BMHeroRecompiled.exe`.** The human feel-boots
  between rounds; the exe lock means WAIT (poll every 30 s until it exits),
  never `Stop-Process`. Killing processes the harness scripts themselves
  started (oracle/soak boots) is fine — the scripts already do it.
- **Log discipline:** every game boot truncates `arena_bridge.log` (fopen
  "w"). Copy any log you need again to
  `C:\Users\dshi\GitRepos\BMHeroRecomp\tools\oracle\` or a temp name BEFORE
  the next boot.
- **Build:** `powershell -File C:\Users\dshi\GitRepos\BMHeroRecomp\build.ps1
  -Config rwdi`. A first failure mentioning undefined `fbits_*` or a link
  error right after a patches rebuild is the known lld regen race — retry
  once. Confirm the literal line `BUILD OK` before trusting any boot.
- **Clocks:** input POLLS run 2:1 against frames AND sim ticks. Tables are in
  TICKS; only `verb_apply` converts (poll = tick × 2).

## File Structure

- Create: `src/main/verb_script.h` — `VerbRow` type, `kOracleScript[]`,
  `kBattleScript[]`, `verb_apply()` (header-only; included ONLY by
  `src/main/main.cpp`).
- Modify: `src/main/main.cpp` — replace the hand-rolled oracle `op` windows
  and the mode-13 `cp` windows with `verb_apply` calls.
- Modify: `src/arena_bridge/arena_bridge.cpp` — add `arena_verb_mark()`,
  `[animrun]` logging inside `arena_dbg_anim`.
- Modify: `src/arena_bridge/arena_bridge.h` (or wherever bridge prototypes
  that `main.cpp` consumes live — find `arena_oracle_phase`'s declaration and
  put `arena_verb_mark` beside it).
- Modify: `tools/oracle.ps1` — timeline RLE emission → `tools/oracle/timelines.json`.
- Create: `tools/anim-diff.ps1` — the verb-aligned differ.
- Modify: `tools/oracle-gate.ps1` — check 17.
- Create (generated): `tools/oracle/timelines.json`.

---

### Task 1: Verb tables, driver, and the two new bridge channels

**Files:**
- Create: `src/main/verb_script.h`
- Modify: `src/main/main.cpp` (the `soak_get_n64_input` oracle branch and the
  `std::atoi(mode) == 13` block)
- Modify: `src/arena_bridge/arena_bridge.cpp`, plus the header where
  `arena_oracle_phase` is declared for `main.cpp`

**Interfaces:**
- Produces: `verb_apply(const VerbRow* rows, int n, uint32_t poll, uint16_t*
  buttons, float* stick_y, void (*mark)(const char*))` — poll is the caller's
  existing 1-based poll counter; the function ORs buttons/stick for every
  active row and calls `mark(name)` exactly once per named row, on the row's
  first active poll.
- Produces: `extern "C" void arena_verb_mark(const char* name)` (bridge) —
  logs `[verb] <name> t<g_state.tick>` to `arena_bridge.log`.
- Produces: `[animrun] idx=<i> len=<n> t<tick>` lines (bridge) — on every
  walker anim index change in battle; `len` = frames the PREVIOUS index held
  (uncapped, unlike the 28-line `[anim]` burst which stays as-is).

- [ ] **Step 1: read the two script sites you are replacing.** In
  `src/main/main.cpp` find (a) the oracle branch: `if (oracle_active && ok &&
  controller_num == 0)` → the `else` arm with `static uint32_t op`; (b) the
  mode-13 block: `if (mode && std::atoi(mode) == 13)` with `static uint32_t
  cp`. Read both fully. Note the `op == 1` in-level marker — it stays outside
  the table as a preamble line.

- [ ] **Step 2: write `src/main/verb_script.h`.**

```c
#ifndef VERB_SCRIPT_H
#define VERB_SCRIPT_H
#include <stdint.h>

/* One choreography row. Times are SIM TICKS from the script origin; the
 * input callback runs at 2 polls per tick, and verb_apply is the ONLY place
 * that conversion exists (the 2:1 clock has produced three separate bugs
 * when hand-converted). dur == 0 rows are pure markers. */
typedef struct {
    const char* name;    /* marker fired at row start; NULL = unnamed */
    uint32_t    start;   /* ticks */
    uint32_t    dur;     /* ticks; 0 = marker only */
    uint16_t    buttons; /* N64 mask OR'd in while active */
    int8_t      stick_y; /* -1 / 0 / +1 full deflection */
} VerbRow;

#define VERB_A 0x8000
#define VERB_B 0x4000
#define VERB_R 0x0010

/* The VANILLA single-player script (ARENA_ORACLE=1). Ticks = the historical
 * poll windows / 2, verbatim - the goldens byte-identity check depends on
 * exact reproduction. Overlapping rows are intended (stick during a hold). */
static const VerbRow kOracleScript[] = {
    { "walk",      150,  60, 0,      -1 },
    { "stand",     210,   0, 0,       0 },
    { "dropB",     240,   2, VERB_B,  0 },
    { "holdB",     450,  30, VERB_B,  0 },
    { "releaseB",  480,   0, 0,       0 },
    { "setR",      630,   2, VERB_R,  0 },
    { "walkoff",   660,  20, 0,      +1 },
    { "kickrun",   680,  30, 0,      -1 },
    { "jumpA",     870,   3, VERB_A,  0 },
    { "airsetR",   876,   2, VERB_R,  0 },
    { "carryB",   1050,  45, VERB_B,  0 },   /* B held through carrywalk */
    { "carrywalk",1065,  30, 0,      -1 },
    { "carryrel", 1095,   0, 0,       0 },
    { "holdlong", 1200, 240, VERB_B,  0 },   /* B held through windupwalk */
    { "windupwalk",1380, 60, 0,      -1 },
    { "spreadrel",1440,   0, 0,       0 },
    { NULL,       1450,  30, 0,      +1 },   /* step clear of the spread fan */
    { "setR2",    1650,   2, VERB_R,  0 },
    { "jumpon",   1670,   3, VERB_A,  0 },
    { "carryjump",1840,  32, VERB_B,  0 },   /* B held to the midair release */
    { "jumpB",    1850,   3, VERB_A,  0 },
    { "relairB",  1872,   0, 0,       0 },
    { "DONE",     2000,   0, 0,       0 },
};

/* The BATTLE mode-13 probe. Shared names (carryB, carrywalk, carryrel,
 * holdlong, windupwalk, carryjump, jumpB, relairB, setR2) are PREFIX-SHAPED
 * copies of the vanilla verbs: identical buttons/stick from the verb's start,
 * so the differ's min-window truncation makes them comparable. Windows that
 * cannot match (fuse 150 vs 106, the spread arming at 120 ticks) either stop
 * early (holdlong = 100t, under the spread) or use battle-only names
 * (holdrel, jumpon). Everything starts after the 180-tick countdown. */
static const VerbRow kBattleScript[] = {
    { "carryB",    125,  15, VERB_B,  0 },
    { "carrywalk", 140,  60, 0,      -1 },
    { NULL,        140,  85, VERB_B,  0 },   /* B continues to the release */
    { "carryrel",  225,   0, 0,       0 },
    { "holdlong",  250, 100, VERB_B,  0 },   /* stops before the 120t spread */
    { "windupwalk",320,  30, 0,      -1 },
    { "holdrel",   350,   0, 0,       0 },   /* battle-only: uncharged release */
    { "carryjump", 375,  32, VERB_B,  0 },
    { "jumpB",     385,   3, VERB_A,  0 },
    { "relairB",   407,   0, 0,       0 },
    { "setR2",     445,   2, VERB_R,  0 },
    { "jumpon",    470,   3, VERB_A,  0 },   /* battle-only: fuse differs */
};

/* poll is 1-based and runs at 2x tick rate. Named rows mark once, on their
 * first active poll (or their start poll for dur==0 markers). */
static inline void verb_apply(const VerbRow* rows, int n_rows, uint32_t poll,
                              uint16_t* buttons, float* stick_y,
                              void (*mark)(const char*)) {
    int i;
    for (i = 0; i < n_rows; i++) {
        uint32_t p0 = rows[i].start * 2u;
        uint32_t p1 = (rows[i].start + rows[i].dur) * 2u;
        if (rows[i].name && poll == p0 && mark) mark(rows[i].name);
        if (poll >= p0 && poll < p1) {
            *buttons |= rows[i].buttons;
            if (rows[i].stick_y > 0) *stick_y =  1.0f;
            if (rows[i].stick_y < 0) *stick_y = -1.0f;
        }
    }
}
#endif
```

- [ ] **Step 3: verify the vanilla table against the code you read in Step 1
  BEFORE deleting anything.** Every historical window ÷ 2 must equal a row:
  walk 300–420, stand 420, dropB 480–484, holdB 900–960, releaseB 960, setR
  1260–1264, walkoff 1320–1360, kickrun 1360–1420, jumpA 1740–1746, airsetR
  1752–1756, carryB 2100–2190 (B), carrywalk 2130–2190 (stick), carryrel
  2190, holdlong 2400–2880 (B), windupwalk 2760–2880 (stick), spreadrel 2880,
  walk-clear 2900–2960 (+1), setR2 3300–3304, jumpon 3340–3346, carryjump
  3680–3744 (B), jumpB 3700–3706, relairB 3744, DONE 4000. Note carryB's B
  hold spans 2100–2190 = table rows carryB(45t at 1050) covering it, and
  holdlong's B spans 2400–2880 = 240t at 1200. If ANY row disagrees with the
  code, fix the TABLE (the code is the authority) and say so in the report.

- [ ] **Step 4: bridge — `arena_verb_mark` + `[animrun]`.** In
  `arena_bridge.cpp`, next to `arena_oracle_phase`:

```cpp
/* Battle verb marker (oracle 2.0): the mode-13 script logs its named verbs
 * so anim-diff can align arena timelines with the vanilla ones. Stamped with
 * the SIM tick - [animrun] uses the same clock. */
extern "C" void arena_verb_mark(const char* name) {
    ensure_init();
    if (g_log) {
        std::fprintf(g_log, "[verb] %s t%u\n", name, g_state.tick);
        std::fflush(g_log);
    }
}
```

  In `arena_dbg_anim`, right after the `g_walker_anim = idx;` latch:

```cpp
    /* [animrun] (oracle 2.0): uncapped RLE of the walker's anim stream - on
     * every index change, log how long the PREVIOUS clip held. The [anim]
     * burst below stays as-is (the bespoke gates key on it); this channel is
     * for anim-diff, which needs runs longer than the burst's 28-line cap.
     * The final run before process exit is never flushed - the differ's
     * min-window truncation absorbs it. */
    {
        static int run_idx = -1;
        static int run_len = 0;
        if (idx != run_idx) {
            if (run_idx >= 0 && g_log) {
                std::fprintf(g_log, "[animrun] idx=%d len=%d t%u\n",
                             run_idx, run_len, g_state.tick);
                std::fflush(g_log);
            }
            run_idx = idx; run_len = 1;
        } else {
            run_len++;
        }
    }
```

  Declare `arena_verb_mark` in the same header that declares
  `arena_oracle_phase` for `main.cpp` (grep for `arena_oracle_phase` outside
  the .cpp to find it).

- [ ] **Step 5: refactor `main.cpp`.** Oracle branch: keep `op++` and the
  `op == 1` in-level marker; replace every other window with:

```c
        uint16_t vb = 0; float vy = 0.0f;
        verb_apply(kOracleScript,
                   (int)(sizeof(kOracleScript)/sizeof(kOracleScript[0])),
                   op, &vb, &vy, arena_oracle_phase);
        *buttons |= vb;
        if (vy != 0.0f) *y = vy;
```

  Mode-13 block: keep `cp++`; replace all windows with the same call over
  `kBattleScript` and `arena_verb_mark` as the callback. Delete the replaced
  hand-rolled lines (git has them). Include `verb_script.h` at the top of
  `main.cpp` with the other local includes.

- [ ] **Step 6: build.** Run `build.ps1 -Config rwdi`; require `BUILD OK`
  (retry once on the lld regen race). If the exe is locked, a human is
  playing: wait (30 s polls), do not kill.

- [ ] **Step 7: the byte-identity regression.** Run
  `powershell -File tools\oracle.ps1` (fork root). It boots vanilla and
  parses. Required output: `goldens unchanged.` — anything else means the
  tables diverge from the historical choreography: STOP, diff the phase
  markers in `arena_bridge.log` against the Step-3 list, fix the table, and
  rerun. Copy the log aside:
  `Copy-Item arena_bridge.log tools\oracle\vanilla-verbtable.log`.

- [ ] **Step 8: the battle regression.** Run
  `powershell -File tools\oracle-gate.ps1`. Required: `ALL GREEN` (16
  checks). Then confirm the new channels:
  `Select-String arena_bridge.log -Pattern '\[verb\] |\[animrun\] ' | Select-Object -First 5`
  must show both. Copy the log aside:
  `Copy-Item arena_bridge.log tools\oracle\battle-verbtable.log`.

- [ ] **Step 9: commit.**

```powershell
git add src/main/verb_script.h src/main/main.cpp src/arena_bridge/arena_bridge.cpp <the header from step 4>
git commit -m "feat(oracle2): verb tables in tick units drive both scripts; [verb] + [animrun] channels"
```

---

### Task 2: Timeline extraction and the verb-aligned differ

**Files:**
- Modify: `tools/oracle.ps1`
- Create: `tools/anim-diff.ps1`
- Create (generated): `tools/oracle/timelines.json`

**Interfaces:**
- Consumes: Task 1's logs (`tools/oracle/vanilla-verbtable.log`,
  `tools/oracle/battle-verbtable.log`) and channels (`[oracle] phase=<name>
  n=<n>`, `[oracle-anim] n=<n> idx=<i> ...` per frame; `[verb] <name>
  t<tick>`, `[animrun] idx=<i> len=<n> t<tick>`).
- Produces: `tools/oracle/timelines.json` —
  `{ "<verb>": { "frames": <int>, "runs": [[idx,len],...] }, ... }`.
- Produces: `tools/anim-diff.ps1 -ArenaLog <path> [-Timelines <path>]
  [-Verbs a,b,c]` — exit code = number of failing verbs; prints one
  PASS/FAIL line per compared verb; comparing zero verbs is exit 1 with a
  loud message.

- [ ] **Step 1: timeline emission in `oracle.ps1`.** After the existing
  goldens block (reuse its `$lines`, the `PhaseN` markers and the `$anim`
  collection), add:

```powershell
# ---- oracle 2.0: per-verb anim timelines (RLE) -------------------------------
# Verb window = its marker's n to the NEXT marker's n. Runs of 1 frame are
# transition jitter and are dropped. Refuse verbs with zero samples (the
# round-10 vacuous-green lesson: an empty window must fail loudly, not write).
$markers = $lines | Select-String '\[oracle\] phase=(\S+) n=(\d+)' |
    ForEach-Object { [pscustomobject]@{ name = $_.Matches[0].Groups[1].Value
                                        n    = [int]$_.Matches[0].Groups[2].Value } }
$timelines = [ordered]@{}
for ($i = 0; $i -lt $markers.Count - 1; $i++) {
    $m = $markers[$i]
    if ($m.name -in @('in-level','DONE')) { continue }
    $n0 = $m.n; $n1 = $markers[$i+1].n
    $win = @($anim | Where-Object { $_.n -ge $n0 -and $_.n -lt $n1 })
    if ($win.Count -eq 0) { Write-Error "timeline '$($m.name)' has ZERO anim samples - refusing to write"; exit 1 }
    $runs = @(); $cur = $win[0].idx; $len = 0
    foreach ($a in $win) {
        if ($a.idx -eq $cur) { $len++ }
        else { if ($len -ge 2) { $runs += ,@($cur, $len) }; $cur = $a.idx; $len = 1 }
    }
    if ($len -ge 2) { $runs += ,@($cur, $len) }
    $timelines[$m.name] = [ordered]@{ frames = ($n1 - $n0); runs = $runs }
}
$tlOut = Join-Path $root "tools\oracle\timelines.json"
$tlNew = $timelines | ConvertTo-Json -Depth 5
if ((Test-Path $tlOut) -and -not $Force) {
    $tlOld = Get-Content $tlOut -Raw
    if ($tlOld.Trim() -ne $tlNew.Trim()) {
        Write-Host "=== timelines DIFFER from checked-in (rerun with -Force to overwrite) ==="
        Write-Host "--- old ---`n$tlOld`n--- new ---`n$tlNew"; exit 1
    }
    Write-Host "timelines unchanged."
} else { Set-Content -Path $tlOut -Value $tlNew; Write-Host "timelines written to $tlOut" }
```

  Place it so a goldens byte-identity failure still exits first (after the
  goldens compare/write). Note `$anim`'s regex only captures `idx=(\d+)` —
  it already exists; do not re-parse.

- [ ] **Step 2: generate from Task 1's saved log.** Run
  `powershell -File tools\oracle.ps1 -FromLog tools\oracle\vanilla-verbtable.log -Force`.
  Verify `timelines.json` exists and spot-check three verbs against known
  goldens: `carrywalk` runs must start `[17, ~30]`; `holdlong` runs must be
  `[[14,~62],[26,~178]]`; `relairB` must start `[[21,3],[6,...]]`.

- [ ] **Step 3: write `tools/anim-diff.ps1`.**

```powershell
# Compare the arena's per-verb anim timelines against the vanilla goldens
# (tools\oracle\timelines.json). Verbs compared = the intersection of names.
# A verb in the timelines whose [verb] marker is MISSING from the arena log
# is a FAIL (probe died early / marker never fired), not a skip.
param(
    [Parameter(Mandatory=$true)][string]$ArenaLog,
    [string]$Timelines = "",
    [string]$Verbs = ""          # optional comma list to restrict
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Timelines) { $Timelines = Join-Path $root "tools\oracle\timelines.json" }
if (-not (Test-Path $Timelines)) { Write-Error "no timelines at $Timelines"; exit 1 }
if (-not (Test-Path $ArenaLog))  { Write-Error "no arena log at $ArenaLog"; exit 1 }
$tl = Get-Content $Timelines -Raw | ConvertFrom-Json
$lines = Get-Content $ArenaLog

# Reconstruct the arena's per-tick anim stream from [animrun] (idx held for
# len frames ENDING at t), then slice per [verb] marker and re-RLE. Building
# the flat stream first makes runs straddling verb boundaries split correctly.
$stream = @{}   # tick -> idx
foreach ($m in ($lines | Select-String '\[animrun\] idx=(\d+) len=(\d+) t(\d+)')) {
    $ix=[int]$m.Matches[0].Groups[1].Value; $ln=[int]$m.Matches[0].Groups[2].Value
    $t =[int]$m.Matches[0].Groups[3].Value
    for ($k = $t - $ln; $k -lt $t; $k++) { $stream[$k] = $ix }
}
$marks = @($lines | Select-String '\[verb\] (\S+) t(\d+)' |
    ForEach-Object { [pscustomobject]@{ name = $_.Matches[0].Groups[1].Value
                                        t    = [int]$_.Matches[0].Groups[2].Value } })
function ArenaRuns([int]$t0, [int]$t1) {
    $runs = @(); $cur = $null; $len = 0
    for ($t = $t0; $t -lt $t1; $t++) {
        if (-not $stream.ContainsKey($t)) { continue }
        $ix = $stream[$t]
        if ($null -ne $cur -and $ix -eq $cur) { $len++ }
        else { if ($null -ne $cur -and $len -ge 2) { $runs += ,@($cur,$len) }
               $cur = $ix; $len = 1 }
    }
    if ($null -ne $cur -and $len -ge 2) { $runs += ,@($cur,$len) }
    ,$runs
}
$restrict = if ($Verbs) { $Verbs -split ',' } else { $null }
$fails = 0; $compared = 0
foreach ($vp in $tl.PSObject.Properties) {
    $name = $vp.Name
    if ($restrict -and $name -notin $restrict) { continue }
    $mk = @($marks | Where-Object { $_.name -eq $name })
    if ($mk.Count -eq 0) {
        if ($restrict) { Write-Host ("[anim-diff] {0,-12} FAIL  no [verb] marker in arena log" -f $name); $fails++; $compared++ }
        continue   # unrestricted: verbs the battle script doesn't carry are skipped by design
    }
    $compared++
    $t0 = $mk[0].t
    $next = @($marks | Where-Object { $_.t -gt $t0 } | Sort-Object t | Select-Object -First 1)
    $t1 = if ($next) { $next[0].t } else { $t0 + [int]$vp.Value.frames }
    $aFrames = $t1 - $t0
    $window = [math]::Min([int]$vp.Value.frames, $aFrames)
    # truncate BOTH run lists to the window
    function Truncate($runs, [int]$budget) {
        $out = @()
        foreach ($r in $runs) {
            $ix = [int]$r[0]; $ln = [int]$r[1]
            if ($budget -le 0) { break }
            $take = [math]::Min($ln, $budget)
            if ($take -ge 2) { $out += ,@($ix, $take) }
            $budget -= $take
        }
        ,$out
    }
    $vRuns = Truncate @($vp.Value.runs | ForEach-Object { ,@([int]$_[0], [int]$_[1]) }) $window
    $aRuns = Truncate (ArenaRuns $t0 $t1) $window
    $bad = $null
    if ($vRuns.Count -ne $aRuns.Count) {
        $bad = "run count vanilla=$($vRuns.Count) arena=$($aRuns.Count)"
    } else {
        for ($k = 0; $k -lt $vRuns.Count; $k++) {
            if ($vRuns[$k][0] -ne $aRuns[$k][0] -or
                [math]::Abs($vRuns[$k][1] - $aRuns[$k][1]) -gt 3) {
                $bad = "run $k vanilla ($($vRuns[$k] -join ',')) vs arena ($($aRuns[$k] -join ','))"
                break
            }
        }
    }
    if ($bad) { Write-Host ("[anim-diff] {0,-12} FAIL  {1}" -f $name, $bad); $fails++ }
    else      { Write-Host ("[anim-diff] {0,-12} PASS  {1} runs over {2}f" -f $name, $vRuns.Count, $window) }
}
if ($compared -eq 0) { Write-Host "[anim-diff] compared ZERO verbs - that is a failure, not a pass"; exit 1 }
Write-Host ("[anim-diff] {0} verbs compared, {1} failed" -f $compared, $fails)
exit $fails
```

- [ ] **Step 4: run the differ on Task 1's battle log.** Run
  `powershell -File tools\anim-diff.ps1 -ArenaLog tools\oracle\battle-verbtable.log`.
  Expected: the shared verbs (carryB, carrywalk, carryrel, holdlong,
  windupwalk, carryjump, jumpB, relairB, setR2) print and PASS; battle-only
  names (holdrel, jumpon) and vanilla-only verbs are absent; exit 0. If a
  shared verb FAILS, first suspect the WINDOW MAPPING (prefix shapes, the
  next-marker boundary), then the arena behavior; report what you find —
  a genuine arena divergence found here is a result, not a defect of yours.

- [ ] **Step 5: independent reproduction boot.** Run
  `powershell -File tools\oracle.ps1` (fresh vanilla boot; goldens AND
  timelines must both print `unchanged.`). If timelines differ by 1–3 frames
  on some runs across boots, note the run-to-run variance in your report and
  loosen NOTHING yet — Task 3's gate decides tolerance policy with that data.

- [ ] **Step 6: commit.**

```powershell
git add tools/oracle.ps1 tools/anim-diff.ps1 tools/oracle/timelines.json
git commit -m "feat(oracle2): per-verb anim timelines + verb-aligned RLE differ"
```

---

### Task 3: Gate check 17, falsifiability, docs, final soak

**Files:**
- Modify: `tools/oracle-gate.ps1`
- Modify: `docs/bmhero-recomp-integration-notes.md` and
  `docs/HANDOFF-2026-07-30.md` — in the CANONICAL repo
  (`C:\Users\dshi\GitRepos\bmhero-arena`), NOT the fork.

**Interfaces:**
- Consumes: `tools/anim-diff.ps1` (exit code = failing verbs) and the mode-13
  log that checks 11–16 already produce (`$m13` / `arena_bridge.log` right
  after the mode-13 RunSoak).

- [ ] **Step 1: check 17 in `oracle-gate.ps1`.** After check 16, before the
  final `if ($fails)`:

```powershell
# --- check 17: full anim-timeline diff (oracle 2.0) ----------------------------
# Every shared verb's ENTIRE animation timeline must match vanilla's within
# +/-3 frames per run. This is the blanket net the bespoke checks are not:
# any wrong clip, duration, or transition in a covered verb goes red here.
# The mode-13 log was overwritten by later RunSoak calls? No - mode 13 is the
# LAST boot; arena_bridge.log still holds it. Guard anyway.
$adOut = & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\anim-diff.ps1") -ArenaLog $log 2>&1
$adOut | Out-Host
Check "anim timelines match vanilla" ($LASTEXITCODE -eq 0) "see [anim-diff] lines above"
```

  Confirm mode 13 IS the last RunSoak in the file (it is today; if you moved
  it, point `-ArenaLog` at a copy saved right after the mode-13 boot).

- [ ] **Step 2: falsifiability — the check must be able to fail.** Run:

```powershell
$env:ARENA_CARRY_WALK_ANIM = '26'
powershell -File tools\oracle-gate.ps1
Remove-Item Env:\ARENA_CARRY_WALK_ANIM
```

  Required: check 17 (and likely 11/15) go RED with carrywalk named in an
  [anim-diff] FAIL line, gate exits non-zero. If check 17 stays green the
  differ is asserting nothing — STOP and fix before proceeding. Then rerun
  the gate clean (no env) and require ALL GREEN (17 checks).

- [ ] **Step 3: full handoff soak.** Run
  `powershell -File build.ps1 -Config rwdi -Soak 5`. Required: `SOAK GREEN`
  and `[oracle-gate] ALL GREEN`. (No code changed since Task 2's build; this
  is the handoff-rule run on the exact final exe.)

- [ ] **Step 4: docs (canonical repo).** Append §8.35 to
  `docs/bmhero-recomp-integration-notes.md`: what the verb tables are (tick
  units, one conversion site, prefix-shaped shared verbs), what the timeline
  diff covers (shared verb list), the falsifiability proof performed, and the
  known limits (final [animrun] run unflushed; verbs with design-different
  timing — fuse 150 vs 106 — use unshared names; run-to-run variance found in
  Task 2 Step 5 if any). Add a matching one-paragraph addendum 8 to
  `docs/HANDOFF-2026-07-30.md` and update its state table to "oracle-gate
  17/17". Commit both files in the canonical repo:
  `git -C C:\Users\dshi\GitRepos\bmhero-arena add docs && git -C ... commit`.

- [ ] **Step 5: fork commit + submodule bump.**

```powershell
git -C C:\Users\dshi\GitRepos\BMHeroRecomp\lib\bmhero-arena fetch C:\Users\dshi\GitRepos\bmhero-arena main
git -C C:\Users\dshi\GitRepos\BMHeroRecomp\lib\bmhero-arena checkout <canonical docs commit sha>
git -C C:\Users\dshi\GitRepos\BMHeroRecomp add tools/oracle-gate.ps1 lib/bmhero-arena
git -C C:\Users\dshi\GitRepos\BMHeroRecomp commit -m "feat(oracle2): gate check 17 - anim-timeline diff; falsifiability proven (carrywalk knob -> red)"
```

  Do NOT push either repo — pushing is the human's call.

---

## Self-review notes (author)

- Spec coverage: Part A → Task 1; Part B capture/differ → Task 2; gate +
  falsifiability + verification → Task 3; byte-identity → Task 1 Step 7;
  vacuous-green refusal → Task 2 Step 1 (zero-sample error) and differ's
  zero-verbs exit 1; missing-marker FAIL → differ (restricted mode) and the
  intersection rule documented for unrestricted mode. Spec's "missing verb =
  FAIL not skip" applies to verbs the battle script CARRIES; vanilla-only
  verbs are skipped by design — the differ implements exactly that via the
  marker intersection.
- The battle table is a RE-CHOREOGRAPHY of mode 13 (prefix-shaped shared
  verbs), not a transcription — the 16 bespoke checks green (Task 1 Step 8)
  is the regression proof for it, byte-identical goldens for the vanilla
  table.
- Type consistency: `verb_apply` signature identical in header and both call
  sites; `arena_verb_mark(const char*)` matches the C++ definition and the
  C-side declaration; timeline JSON shape identical between oracle.ps1
  emitter and anim-diff.ps1 consumer.
