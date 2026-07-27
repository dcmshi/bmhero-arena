# A1.5 Fixed Arena Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Own `gView` with a static 60°-pitch / 0°-yaw camera so feel-testing is trustworthy — killing both the rail camera's `rot.y` drift (which corrupts *input*, since the game rotates the stick by it) and most of the perspective foreshortening.

**Architecture:** Fork-side only. A pure-math header (`patches/arena_cam.h`) holds the pose constants and is included by **both** the MIPS patch and a **host-side test**, so the assumptions baked into precomputed trig literals are machine-checked. The override is written every frame at the top of `arena_render_routine`, *before* it calls `func_80024744`. Measurement comes first as its own shipped probe, because §5 records that blind camera tuning already went backwards once.

**Tech Stack:** C (MIPS patch via clang-15; host test via MSYS2 UCRT64 gcc), PowerShell, the existing `ARENA_AUTO_BATTLE` probe + `arena-soak.ps1` gate harness.

**Spec:** `docs/superpowers/specs/2026-07-26-a1-5-fixed-arena-camera-design.md`

## Global Constraints

- **No sim change.** `TUNE_VERSION` stays **7**, hash stays **`07fc6ade`**, and `lib/bmhero-arena` does **not** move. If any of those change, you did something wrong — stop.
- **Never call `sinf`/`cosf`/`sqrtf` in patch code.** An emitted math libcall links silently and jumps to 0 (§8.11). All trig is precomputed as literals and guarded by the host test. After building patches, verify: `llvm-nm patches\arena_render.o | findstr /R "sinf cosf sqrtf"` must be **empty**.
- **Export ABI: max 4 args, 32-bit, NO float arguments.** Floats must cross as `u32` bit patterns via a union bitcast (§8.2). `f32` *return* values are fine (`DECLARE_FUNC(f32, ...)` is used widely already).
- **Auto-named DATA symbols don't resolve through the patch reloc path** — pass their address as a literal (`((T*)0x801163DC)`) if you need one (§8.2). `gView` and `gCameraType` are declared in headers, so this does not apply to them.
- **Patches are stateless.** Patch-local mutable statics abort `0xC0000409`. Keep state native (in `arena_bridge.cpp`).
- **Next free `syms.ld` address is `0x8F0001D4`** (last used: `arena_export_dbg_anim = 0x8F0001D0`). Allocate upward.
- **Always build via `.\build.ps1`** — a bare `make` in `patches/` picks up MSYS2 gcc and rejects every MIPS flag; `build.ps1` sets `CC=clang LD=ld.lld` and does the mandatory `make clean`.
- **No build reaches the human without a green soak on that exact build** (`build.ps1 -Soak 5`).
- **Branch:** `feature/a1.5-fixed-camera` off `master` in `C:\Users\dshi\GitRepos\BMHeroRecomp`.

## Known struct facts (verified, do not re-derive)

```c
struct View {            /* lib/bmhero/include/types.h:257, size 0x34 */
    /* 0x00 */ Vec3f at;
    /* 0x0C */ Vec3f eye;
    /* 0x18 */ Vec3f rot;    /* rot.y = camera yaw IN DEGREES */
    /* 0x24 */ Vec3f up;
    /* 0x30 */ f32   dist;
};
extern struct View gView;    /* variables.h:609 */
extern s32 gCameraType;      /* variables.h:676 */
```

`arena_render_routine` lives at `patches/arena_render.c:118`; it runs the boss-suppression sweep, then calls `func_80024744()` at **line 136**. The camera write goes between those two.

## File Structure

| File | Responsibility |
|---|---|
| `patches/arena_cam.h` *(create)* | pose constants + inline pose math. **No game types** — compiles in both the MIPS patch and the host test |
| `tools/test_arena_cam.c` *(create)* | host test: asserts the precomputed literals match their declared angles, and that the design decisions still hold |
| `tools/run-cam-tests.ps1` *(create)* | builds + runs the host test with MSYS2 gcc |
| `patches/arena_render.c` *(modify)* | camera probe logging + the `gView` override |
| `src/arena_bridge/arena_bridge.cpp` *(modify)* | `arena_cam_at_x/y/z` + `arena_dbg_cam` native implementations |
| `patches/syms.ld` *(modify)* | four new export addresses |
| `src/main/main.cpp` *(modify)* | `ARENA_AUTO_BATTLE=6` probe mode |
| `tools/arena-soak.ps1` *(modify)* | `-Constant` gate |
| `build.ps1` *(modify)* | run the host cam tests as a pre-build gate |

---

## Task 1: Pose math header + host tests (the assumption guards)

This is the task that answers "capture assumptions with tests". The patch itself can't be unit-tested (it runs as MIPS inside the game), but the *pose math and its constants* are pure and portable — so they get real tests.

**Files:**
- Create: `patches/arena_cam.h`
- Create: `tools/test_arena_cam.c`
- Create: `tools/run-cam-tests.ps1`

**Interfaces:**
- Produces: `ARENA_CAM_PITCH_DEG`, `ARENA_CAM_YAW_DEG`, `ARENA_CAM_SIN_PITCH`, `ARENA_CAM_COS_PITCH`, `ARENA_CAM_SIN_YAW`, `ARENA_CAM_COS_YAW`, `ARENA_CAM_DIST`, `ARENA_CAM_AT_Y_LIFT`, `ARENA_CAM_Z_SIGN`; and `void arena_cam_eye_offset(float* ox, float* oy, float* oz)` plus `float arena_cam_foreshorten(void)`. Tasks 3–5 consume these.

- [ ] **Step 1: Write the failing test**

Create `tools/test_arena_cam.c`:

```c
/* Host-side guards for the fixed-camera pose. The patch itself runs as MIPS
 * inside the game and can't be unit-tested, but the pose CONSTANTS and math are
 * pure and portable - so the assumptions baked into them are machine-checked
 * here instead of living as comments.
 *
 * Build/run via tools\run-cam-tests.ps1 (also wired into build.ps1). */
#include <stdio.h>
#include <math.h>
#include "../patches/arena_cam.h"

static int failures = 0;
#define CHECK(c, ...) do { if(!(c)){ failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while(0)

#define PI 3.14159265358979323846
#define EPS 1e-5

int main(void) {
    /* ---- ASSUMPTION 1: the precomputed trig literals match their angles. ----
     * The patch must not call sinf/cosf (an emitted libcall links silently and
     * jumps to 0 - integration notes 8.11), so the trig is hardcoded. That makes
     * "changed the angle, forgot to update the literal" a silent, plausible bug
     * that would aim the camera somewhere arbitrary. This is the guard. */
    double rp = ARENA_CAM_PITCH_DEG * PI / 180.0;
    double ry = ARENA_CAM_YAW_DEG   * PI / 180.0;
    CHECK(fabs(ARENA_CAM_SIN_PITCH - sin(rp)) < EPS,
          "ARENA_CAM_SIN_PITCH %.7f != sin(%.1f deg) = %.7f - update the literal",
          ARENA_CAM_SIN_PITCH, ARENA_CAM_PITCH_DEG, sin(rp));
    CHECK(fabs(ARENA_CAM_COS_PITCH - cos(rp)) < EPS,
          "ARENA_CAM_COS_PITCH %.7f != cos(%.1f deg) = %.7f - update the literal",
          ARENA_CAM_COS_PITCH, ARENA_CAM_PITCH_DEG, cos(rp));
    CHECK(fabs(ARENA_CAM_SIN_YAW - sin(ry)) < EPS,
          "ARENA_CAM_SIN_YAW %.7f != sin(%.1f deg) = %.7f - update the literal",
          ARENA_CAM_SIN_YAW, ARENA_CAM_YAW_DEG, sin(ry));
    CHECK(fabs(ARENA_CAM_COS_YAW - cos(ry)) < EPS,
          "ARENA_CAM_COS_YAW %.7f != cos(%.1f deg) = %.7f - update the literal",
          ARENA_CAM_COS_YAW, ARENA_CAM_YAW_DEG, cos(ry));

    /* ---- ASSUMPTION 2: yaw 0 keeps the input mapping an identity. ----
     * The game rotates the stick in place by gView.rot.y (func_80024744,
     * camtype 6 - notes 8.11). Yaw 0 makes that rotation the identity, which is
     * the whole reason a held stick direction stops curving. A non-zero yaw
     * silently reintroduces the bug this slice exists to fix. */
    CHECK(ARENA_CAM_YAW_DEG == 0.0f,
          "ARENA_CAM_YAW_DEG is %.2f, not 0 - the game rotates the stick by "
          "rot.y, so a non-zero yaw reintroduces the curved-movement bug",
          ARENA_CAM_YAW_DEG);

    /* ---- ASSUMPTION 3: the pitch keeps foreshortening acceptable. ----
     * Screen travel for toward/away motion vs across motion is sin(pitch).
     * 60deg -> 0.87 (near-equal, the design choice); 45deg -> 0.71 (the artifact
     * we removed). This encodes the DECISION, so a future pitch change that
     * reintroduces bad foreshortening fails here instead of in a playtest. */
    CHECK(arena_cam_foreshorten() >= 0.85f,
          "foreshorten factor %.3f < 0.85 (pitch %.1f deg) - W/S will read "
          "noticeably slower than A/D again",
          arena_cam_foreshorten(), ARENA_CAM_PITCH_DEG);

    /* ---- ASSUMPTION 4: the eye pose is geometrically sane. ---- */
    float ox, oy, oz;
    arena_cam_eye_offset(&ox, &oy, &oz);
    CHECK(oy > 0.0f, "camera must sit ABOVE the arena (oy=%.1f)", oy);
    CHECK(fabs(ox) < EPS,
          "yaw 0 must give zero X offset (ox=%.4f) - otherwise the view is "
          "rotated and stick-up stops mapping to a fixed world axis", ox);
    double len = sqrt((double)ox*ox + (double)oy*oy + (double)oz*oz);
    CHECK(fabs(len - ARENA_CAM_DIST) < 1e-2,
          "eye offset length %.3f != ARENA_CAM_DIST %.3f", len, ARENA_CAM_DIST);
    CHECK(ARENA_CAM_Z_SIGN == 1.0f || ARENA_CAM_Z_SIGN == -1.0f,
          "ARENA_CAM_Z_SIGN must be exactly +1 or -1 (got %.3f)", ARENA_CAM_Z_SIGN);
    CHECK(ARENA_CAM_DIST > 0.0f, "ARENA_CAM_DIST must be positive");

    if (!failures) { printf("ALL CAMERA POSE TESTS PASSED\n"); return 0; }
    printf("%d FAILURE(S)\n", failures); return 1;
}
```

- [ ] **Step 2: Run it to verify it fails**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
C:\msys64\ucrt64\bin\gcc.exe -std=c11 -Wall -Wextra -Werror -O2 -o "$env:TEMP\tc.exe" tools\test_arena_cam.c -lm
```

Expected: **compile failure** — `patches/arena_cam.h` does not exist.

Note: invoke gcc by full path only if `C:\msys64\ucrt64\bin` is on PATH; MSYS2 gcc **fails silently (exit 1, no diagnostic)** when it can't load its own DLLs. `run-cam-tests.ps1` (Step 4) handles this.

- [ ] **Step 3: Write the header**

Create `patches/arena_cam.h`:

```c
/* Fixed arena camera pose (A1.5).
 *
 * Pure constants + math, NO game types - so this header compiles BOTH in the
 * MIPS patch and in the host test (tools/test_arena_cam.c).
 *
 * NEVER call sinf/cosf here. An emitted math libcall in patch code links
 * silently and jumps to 0 (integration notes 8.11). Pitch and yaw are
 * compile-time constants, so their trig is precomputed as literals below and
 * the host test asserts the literals still match the declared angles. */
#ifndef ARENA_CAM_H
#define ARENA_CAM_H

/* Pitch measured UP FROM THE GROUND PLANE: 90 = straight down, 0 = horizontal.
 * 60 chosen so toward/away motion reads at sin(60)=0.87 of across motion -
 * near-equal - while keeping enough depth to judge bomb arcs. See the spec. */
#define ARENA_CAM_PITCH_DEG   60.0f

/* MUST stay 0. The game rotates the stick in place by gView.rot.y for
 * gCameraType in {1,2,5,6,7,8} (the arena is type 6, notes 8.11). Yaw 0 makes
 * that rotation the identity, so stick-up maps to a fixed world axis. It also
 * puts the arena's long axis (half_x 7.9 vs half_z 3.87) horizontal on screen. */
#define ARENA_CAM_YAW_DEG      0.0f

/* Precomputed trig - guarded by tools/test_arena_cam.c. */
#define ARENA_CAM_SIN_PITCH   0.8660254f   /* sin(60) */
#define ARENA_CAM_COS_PITCH   0.5f         /* cos(60) */
#define ARENA_CAM_SIN_YAW     0.0f         /* sin(0)  */
#define ARENA_CAM_COS_YAW     1.0f         /* cos(0)  */

/* Hero world units. MEASURED, not guessed - we don't know the FOV, so Task 3's
 * probe establishes the starting value and Task 5 iterates it by screenshot.
 * The arena is 1896 x 928 Hero units (2 * half_x/half_z * g_scale 120). */
#define ARENA_CAM_DIST      1600.0f
#define ARENA_CAM_AT_Y_LIFT   60.0f   /* aim slightly above the floor, not at it */

/* Which side of the arena the eye sits on. Resolved by Task 3's probe: guessing
 * wrong yields a MIRRORED view rather than an obvious failure. */
#define ARENA_CAM_Z_SIGN    (-1.0f)

/* Eye position relative to `at`, for the fixed pose. */
static inline void arena_cam_eye_offset(float* ox, float* oy, float* oz) {
    *ox = ARENA_CAM_DIST * ARENA_CAM_COS_PITCH * ARENA_CAM_SIN_YAW;
    *oy = ARENA_CAM_DIST * ARENA_CAM_SIN_PITCH;
    *oz = ARENA_CAM_DIST * ARENA_CAM_COS_PITCH * ARENA_CAM_COS_YAW * ARENA_CAM_Z_SIGN;
}

/* Screen travel for toward/away motion relative to across motion.
 * 1.0 = no foreshortening at all (straight down). */
static inline float arena_cam_foreshorten(void) { return ARENA_CAM_SIN_PITCH; }

#endif
```

- [ ] **Step 4: Write the test runner**

Create `tools/run-cam-tests.ps1`:

```powershell
# Host-side camera pose tests. The patch runs as MIPS inside the game and can't
# be unit-tested, but the pose constants and math are pure - so the assumptions
# baked into the precomputed trig literals are machine-checked here.
# Wired into build.ps1 as a pre-build gate.
param([string]$Cc = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

# MSYS2 gcc fails SILENTLY (exit 1, no diagnostic) when invoked by absolute path
# without its own bin dir on PATH - it can't load libisl/libmpc. Prepend it.
function Resolve-Cc([string]$explicit) {
    $found = $null
    if ($explicit)          { $found = $explicit }
    elseif ($env:BMHERO_CC) { $found = $env:BMHERO_CC }
    else {
        $onPath = Get-Command gcc -ErrorAction SilentlyContinue
        if ($onPath) { return $onPath.Source }
        foreach ($c in @("C:\msys64\ucrt64\bin\gcc.exe", "C:\msys64\mingw64\bin\gcc.exe")) {
            if (Test-Path $c) { $found = $c; break }
        }
    }
    if (-not $found) { throw "no C compiler found. Pass -Cc <path> or set `$env:BMHERO_CC." }
    $bin = Split-Path $found -Parent
    if ($env:PATH -notlike "*$bin*") { $env:PATH = "$bin;" + $env:PATH }
    return $found
}
$CC  = Resolve-Cc $Cc
$exe = Join-Path ([IO.Path]::GetTempPath()) "test_arena_cam.exe"

& $CC -std=c11 -Wall -Wextra -Werror -O2 -o $exe (Join-Path $root "tools\test_arena_cam.c") -lm
if ($LASTEXITCODE -ne 0) { Write-Host "[cam-tests] BUILD FAILED" -ForegroundColor Red; exit 1 }
& $exe
if ($LASTEXITCODE -ne 0) { Write-Host "[cam-tests] FAILED" -ForegroundColor Red; exit 1 }
Write-Host "[cam-tests] OK" -ForegroundColor Green
exit 0
```

- [ ] **Step 5: Run to verify it passes**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
.\tools\run-cam-tests.ps1
```

Expected: `ALL CAMERA POSE TESTS PASSED` then `[cam-tests] OK`.

- [ ] **Step 6: Prove the guards actually catch regressions**

Temporarily change `ARENA_CAM_PITCH_DEG` to `45.0f` **without** touching the literals, and re-run. Expected: **two** failures — the sin/cos literal mismatch *and* the foreshortening guard (`0.866 >= 0.85` passes but the literal check fires; then set the literals to 45°'s values and the foreshorten check fires at 0.707). Confirm both messages are actionable, then revert to `60.0f`.

Also temporarily set `ARENA_CAM_YAW_DEG` to `30.0f` and confirm the identity-mapping guard fires. Revert.

- [ ] **Step 7: Commit**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git checkout -b feature/a1.5-fixed-camera
git add patches/arena_cam.h tools/test_arena_cam.c tools/run-cam-tests.ps1
git commit -m "test(A1.5): camera pose header + host guards for its assumptions

patches/arena_cam.h holds the fixed-camera pose as pure constants and math with
no game types, so it compiles both in the MIPS patch and in a host test.

The patch cannot call sinf/cosf (an emitted libcall links silently and jumps to
0, notes 8.11), so the trig is hardcoded - which makes 'changed the angle, forgot
the literal' a silent bug that aims the camera somewhere arbitrary.
tools/test_arena_cam.c guards that, plus three design assumptions: yaw must stay
0 (the game rotates the stick by rot.y, so non-zero yaw reintroduces the curved
movement bug), pitch must keep sin(pitch) >= 0.85 (foreshortening), and the eye
pose must be above the arena at the declared distance.

Verified the guards fire: perturbing pitch and yaw each produce actionable
failures."
```

---

## Task 2: `-Constant` soak gate

Drift is the bug, so **constancy** is the property to assert. Completes the trio with the existing `-Expect` (appears) and `-Rising` (increases).

**Files:**
- Modify: `tools/arena-soak.ps1`

**Interfaces:**
- Consumes: the existing `-Expect` / `-Rising` / `-Mode` parameter block.
- Produces: `-Constant '<regex with one capture group>'` — fails unless ≥2 samples are captured and **all** are identical. Task 4 uses it as the core camera gate.

- [ ] **Step 1: Add the parameter**

In `tools/arena-soak.ps1`, extend the `param(...)` block:

```powershell
param([int]$N = 10, [int]$TimeoutSec = 75, [switch]$Probe, [switch]$AnimProbe,
      [string]$Expect = "", [string]$Rising = "", [string]$Constant = "", [int]$Mode = 0)
```

Add to the comment header, after the `-Rising` line:

```powershell
# -Constant '<regex>': pattern must match >=2 times and its first capture group
#                      must be IDENTICAL every time. For values that must NOT
#                      change - e.g. a fixed camera's yaw (A1.5).
```

- [ ] **Step 2: Include it in the single-run and dwell conditions**

Change:

```powershell
if ($Probe -or $AnimProbe -or $Expect -or $Rising) { $N = 1 }
```

to:

```powershell
if ($Probe -or $AnimProbe -or $Expect -or $Rising -or $Constant) { $N = 1 }
```

and change the dwell condition:

```powershell
    if (($Probe -or $AnimProbe -or $Expect -or $Rising) -and $verdict -eq "PASS") {
```

to:

```powershell
    if (($Probe -or $AnimProbe -or $Expect -or $Rising -or $Constant) -and $verdict -eq "PASS") {
```

- [ ] **Step 3: Add the gate**

Immediately before the final `exit $fails`, after the `-Rising` block:

```powershell
if ($Constant) {
    # Inverse of -Rising: the captured value must NEVER change. Used for the
    # fixed camera, where drift IS the bug (A1.5).
    $vals = @()
    if (Test-Path $log) {
        $vals = @(Select-String -Path $log -Pattern $Constant |
                  ForEach-Object { $_.Matches[0].Groups[1].Value })
    }
    $uniq = @($vals | Select-Object -Unique)
    $ok = ($vals.Count -ge 2) -and ($uniq.Count -eq 1)
    Write-Host ("CONSTANT GATE: /{0}/ samples={1} distinct=[{2}] -> {3}" -f `
        $Constant, $vals.Count, ($uniq -join ','), $(if ($ok) { 'PASS' } else { 'FAIL' }))
    if (-not $ok) { $fails++ }
}
```

- [ ] **Step 4: Verify against the existing log (no rebuild needed)**

The last boot's `arena_bridge.log` still has `[anim] idx=N frame=M` lines. `idx=` values vary (0, 7, 8, 13), so a `-Constant` on idx must FAIL, proving the gate detects variance:

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
.\tools\arena-soak.ps1 -Mode 1 -Constant 'idx=(\d+)'
```

Expected: `CONSTANT GATE: ... distinct=[0,7,8,13] -> FAIL`, exit non-zero.

- [ ] **Step 5: Verify the positive case**

`[capture]` appears once with a fixed origin. Use a pattern with a genuinely constant capture across a run — the `arena` marker's fixed text:

```powershell
.\tools\arena-soak.ps1 -Mode 1 -Constant 'origin\(([0-9.]+),'
```

Expected: `CONSTANT GATE: ... -> PASS` if ≥2 samples, or FAIL with `samples=1` if `[capture]` logs only once. **Either result is informative** — if it reports `samples=1`, that confirms the gate correctly refuses to call a single sample "constant". Record which you saw.

- [ ] **Step 6: Confirm no regression in the existing gates**

```powershell
.\tools\arena-soak.ps1 -AnimProbe
```

Expected: `RISING GATE: /idx=29 frame=(\d+)/ ... -> PASS` exactly as before, exit 0.

- [ ] **Step 7: Commit**

```bash
git add tools/arena-soak.ps1
git commit -m "test(soak): add -Constant gate (value must never change)

Inverse of -Rising, for properties where variance IS the bug - the A1.5 fixed
camera's yaw being the motivating case. Completes the trio: -Expect (appears),
-Rising (increases), -Constant (never changes).

Verified both directions against a real log: a -Constant on the varying anim idx
FAILS with the distinct values listed, and the gate refuses to call a single
sample constant."
```

---

## Task 3: Measurement probe (`ARENA_AUTO_BATTLE=6`)

Ships and runs **before** any override exists. §5 records that A1.2a's blind camera compensation went backwards; this task exists so nothing in Task 4 is guessed.

**Files:**
- Modify: `patches/syms.ld`
- Modify: `src/arena_bridge/arena_bridge.cpp`
- Modify: `patches/arena_render.c`
- Modify: `src/main/main.cpp`

**Interfaces:**
- Consumes: `arena_bridge_is_battle()`, `gView`, `gCameraType`.
- Produces: native `void arena_dbg_cam(int tag, int xbits, int ybits, int zbits)` exported as `arena_export_dbg_cam` at `0x8F0001D4`. `tag` selects which vector: `0=at 1=eye 2=rot 3=up 4=misc`. For `tag==4`, `xbits` is `gCameraType` **as a plain int**, `ybits` is `dist` as float bits, `zbits` is unused (pass 0). All other tags carry three float bit patterns. The **native** side owns the probe-mode gate and the 30-frame throttle — the patch calls it unconditionally every frame and stays stateless. This is the only new export the task needs.

- [ ] **Step 1: Add the export address**

Append to `patches/syms.ld`:

```
arena_export_dbg_cam = 0x8F0001D4;
```

- [ ] **Step 2: Implement the native side**

In `src/arena_bridge/arena_bridge.cpp`, near the existing `arena_dbg_anim` (around line 401), add:

```cpp
/* A1.5 camera probe. The export ABI cannot take float ARGUMENTS (notes 8.2), so
 * the patch sends float BIT PATTERNS through int args and we reassemble here.
 * tag: 0=at 1=eye 2=rot 3=up 4=misc(x=gCameraType as int, y=dist bits). */
extern "C" void arena_dbg_cam(int tag, int xbits, int ybits, int zbits) {
    auto f = [](int bits) { float v; std::memcpy(&v, &bits, sizeof v); return v; };
    static const char* names[] = { "at", "eye", "rot", "up", "misc" };
    if (tag < 0 || tag > 4) return;
    if (tag == 4) {
        arena_log("[cam] type=%d dist=%.1f\n", xbits, f(ybits));
    } else {
        arena_log("[cam] %s=(%.2f,%.2f,%.2f)\n", names[tag], f(xbits), f(ybits), f(zbits));
    }
}
```

Register it alongside the other exports (match the surrounding `REGISTER_FUNC` style exactly):

```cpp
REGISTER_FUNC(arena_dbg_cam);
```

If `arena_log` is not the logger name used in this file, use whatever the neighbouring `arena_dbg_anim` uses — match it rather than introducing a second logging path. Add `#include <cstring>` if `std::memcpy` is not already available.

- [ ] **Step 3: Add the probe to the render routine**

In `patches/arena_render.c`, add near the other `DECLARE_FUNC` lines (~line 97):

```c
DECLARE_FUNC(void, arena_export_dbg_cam, s32 tag, s32 x, s32 y, s32 z);   /* A1.5 camera probe */
```

Add the include at the top with the other includes:

```c
#include "arena_cam.h"
```

Add a bitcast helper near the top of the file (file-scope, no mutable state — patches must stay stateless):

```c
/* The export ABI can't take float ARGS (notes 8.2) - ship the bit pattern. */
static s32 fbits(f32 v) { union { f32 f; s32 i; } u; u.f = v; return u.i; }
```

Then, inside the existing `if (arena_bridge_is_battle() && gPlayerObject != NULL)` block at the top of `arena_render_routine` (after the boss sweep loop, **before** `func_80024744()`), add:

```c
        /* A1.5 probe: sample the camera every frame and let the NATIVE side
         * throttle + gate on probe mode (same division of labour as
         * arena_dbg_anim's burst log). Patches are stateless - a patch-local
         * frame counter or mode flag would abort 0xC0000409 - so the patch just
         * reports and the bridge decides whether to write anything.
         *
         * This runs BEFORE the override exists, so it measures the game's real
         * rail camera. Once the override lands, this same log proves our values
         * stuck: entry values should equal what we wrote on the previous frame. */
        arena_export_dbg_cam(0, fbits(gView.at.x),  fbits(gView.at.y),  fbits(gView.at.z));
        arena_export_dbg_cam(1, fbits(gView.eye.x), fbits(gView.eye.y), fbits(gView.eye.z));
        arena_export_dbg_cam(2, fbits(gView.rot.x), fbits(gView.rot.y), fbits(gView.rot.z));
        arena_export_dbg_cam(3, fbits(gView.up.x),  fbits(gView.up.y),  fbits(gView.up.z));
        arena_export_dbg_cam(4, (s32)gCameraType,   fbits(gView.dist),  0);
```

**Verified prerequisite:** neither `arena_export_probe_mode` nor
`arena_export_frame_count` exists, and this design means neither is needed. The
native `arena_dbg_cam` from Step 2 owns both concerns — extend it with the
throttle and the mode gate:

```cpp
extern "C" void arena_dbg_cam(int tag, int xbits, int ybits, int zbits) {
    /* Gate + throttle live HERE, not in the patch: patches must stay stateless
     * (a patch-local counter aborts 0xC0000409), and the native side already
     * owns this pattern for arena_dbg_anim. Only mode 6 logs; one sample per
     * 30 frames keeps the log readable. */
    static const char* mode = std::getenv("ARENA_AUTO_BATTLE");
    if (mode == nullptr || std::atoi(mode) != 6) return;
    static int calls = 0;
    if (tag == 0) calls++;              /* tag 0 arrives once per frame */
    if ((calls % 30) != 0) return;

    auto f = [](int bits) { float v; std::memcpy(&v, &bits, sizeof v); return v; };
    static const char* names[] = { "at", "eye", "rot", "up", "misc" };
    if (tag < 0 || tag > 4) return;
    if (tag == 4) {
        arena_log("[cam] type=%d dist=%.1f\n", xbits, f(ybits));
    } else {
        arena_log("[cam] %s=(%.2f,%.2f,%.2f)\n", names[tag], f(xbits), f(ybits), f(zbits));
    }
}
```

This supersedes the simpler body shown in Step 2 — write this version. Add
`#include <cstdlib>` for `getenv`/`atoi` if not already present.

- [ ] **Step 4: Add probe mode 6 to the launcher**

In `src/main/main.cpp`, find the existing probe-mode block (search `ARENA_AUTO_BATTLE`, around line 633). Mode 6 needs **no input injection** — it only observes. So it only has to be an accepted value that reaches the arena: confirm the existing auto-battle path treats any non-empty value as "go to battle", and that mode 6 therefore just boots into the arena and idles. If the code switches on specific values, add `6` to the set that boots to battle without injection, alongside the existing `3`/`4` handling.

- [ ] **Step 5: Build and run the probe**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
.\build.ps1 -Config rwdi -Patches always
.\tools\arena-soak.ps1 -Mode 6 -Expect '\[cam\] type='
```

Expected: `EXPECT GATE: /\[cam\] type=/ -> PASS`.

- [ ] **Step 6: Read the measurements and record them**

```powershell
.\tools\arena-log.ps1 -Marker cam
```

Record and sanity-check **all four** of these — they are the inputs Task 4 depends on:

1. **`type=`** — must be one of `{1,2,5,6,7,8}` (expected `6`). **If it is not, STOP**: the stick is not being rotated at all, the input premise changes, and Task 4 needs re-planning before any code is written.
2. **`at=` / `eye=`** — plausible world coordinates, not garbage or NaN. Garbage means the `struct View` offsets are wrong; re-check `types.h:257` before continuing.
3. **`rot=`** — confirm `rot.y` varies across samples (the documented 25–89° swing). This is the bug, captured. Also note which component looks like pitch, to confirm the `rot` write order in Task 4.
4. **`D_8016E134`** — the gate on the game's own eye derivation (`func_8001994C`,
   `src/boot/17930.c:605`). It must be `0` for the game to derive `eye`/`up` from
   the `at`/`rot`/`dist` we write. If the probe shows `eye` failing to track a
   changing `at`, the derivation is not running and Task 4 must write `eye`/`up`
   explicitly. (There is no `ARENA_CAM_Z_SIGN` to resolve — the game's `+90` yaw
   offset settles the handedness; see `arena_cam.h`.)

Also compute a `ARENA_CAM_DIST` starting value: the distance from `eye` to `at` in the samples where the whole arena looks framed. The arena is 1896 × 928 Hero units.

- [ ] **Step 7: Update the header with the measured values**

Set `ARENA_CAM_Z_SIGN` and `ARENA_CAM_DIST` in `patches/arena_cam.h` from Step 6. Re-run the host tests — they must still pass:

```powershell
.\tools\run-cam-tests.ps1
```

- [ ] **Step 8: Commit**

```bash
git add patches/syms.ld patches/arena_render.c patches/arena_cam.h \
        src/arena_bridge/arena_bridge.cpp src/main/main.cpp
git commit -m "feat(A1.5): camera measurement probe (ARENA_AUTO_BATTLE=6)

Logs gView.at/eye/rot/up/dist + gCameraType every 30 frames, BEFORE any override
exists. Section 5 records that A1.2a's blind camera compensation went backwards
(a guessed 1.4x Z boost looked MORE compressed), so nothing in the override is
guessed - Z_SIGN and DIST both come from these measurements.

Float values cross the export boundary as bit patterns: the ABI takes no float
arguments (notes 8.2)."
```

---

## Task 4: The `gView` override

**Files:**
- Modify: `patches/arena_render.c`
- Modify: `src/arena_bridge/arena_bridge.cpp`
- Modify: `patches/syms.ld`

**Interfaces:**
- Consumes: Task 1's `arena_cam_eye_offset()` and constants; Task 3's probe.
- Produces: native `float arena_cam_at_x(void)`, `arena_cam_at_y(void)`, `arena_cam_at_z(void)` exported at `0x8F0001E0`, `0x8F0001E4`, `0x8F0001E8` — the arena centre in Hero world coords, computed with the **same frozen-origin mapping the puppets use**.

- [ ] **Step 1: Add the arena-centre exports**

Append to `patches/syms.ld`:

```
arena_cam_at_x = 0x8F0001E0;
arena_cam_at_y = 0x8F0001E4;
arena_cam_at_z = 0x8F0001E8;
```

In `arena_bridge.cpp`, near the existing `arena_puppet_wx/wy/wz` implementations (~line 271), add:

```cpp
/* A1.5: arena centre in Hero world coords. Deliberately reuses the SAME
 * frozen-origin mapping as the puppets - if the origin capture is ever wrong,
 * the camera is wrong in the same direction as the actors, which keeps the
 * picture self-consistent and makes the error obvious instead of confusing.
 * Sim (0,0) is the arena centre. */
extern "C" float arena_cam_at_x(void) { return g_origin_x + (0.0f - g_ref_sx) * g_scale;   }
extern "C" float arena_cam_at_z(void) { return g_origin_z + (0.0f - g_ref_sz) * g_scale_z; }
extern "C" float arena_cam_at_y(void) { return g_origin_y + ARENA_CAM_AT_Y_LIFT; }
```

`arena_cam_at_y` needs `ARENA_CAM_AT_Y_LIFT`; include the header at the top of `arena_bridge.cpp`:

```cpp
#include "../../patches/arena_cam.h"
```

Register all three:

```cpp
REGISTER_FUNC(arena_cam_at_x);
REGISTER_FUNC(arena_cam_at_y);
REGISTER_FUNC(arena_cam_at_z);
```

- [ ] **Step 2: Declare them in the patch**

In `patches/arena_render.c`, with the other `DECLARE_FUNC` lines:

```c
DECLARE_FUNC(f32, arena_cam_at_x);   /* A1.5 arena centre, Hero coords */
DECLARE_FUNC(f32, arena_cam_at_y);
DECLARE_FUNC(f32, arena_cam_at_z);
```

- [ ] **Step 3: Write the override**

In `arena_render_routine`, in the same battle block, **after** the probe logging from Task 3 and **before** `func_80024744()`:

```c
        /* A1.5 FIXED CAMERA. Re-asserted EVERY frame (the established idiom -
         * the boss re-activates if its sweep stops, notes 8.13).
         *
         * ORDERING IS LOAD-BEARING: func_80024744 (called just below) rotates
         * gActiveContStickX/Y in place by gView.rot.y. Writing the camera AFTER
         * that call would rotate this frame's stick by the game's swinging yaw
         * while the picture used ours - they'd disagree by up to 64deg, which is
         * worse than the drift we're fixing.
         *
         * We write ONLY at / rot / dist. func_8001994C (decomp src/boot/17930.c:605,
         * recovered with tools/decomp-func.ps1) derives eye AND up.y from exactly
         * these every frame, gated on D_8016E134 == 0. Writing eye ourselves would
         * simply be recomputed away - and the resulting "nothing changed" is a far
         * harder symptom to read than a wrong pose. Let the game finish the job. */
        {
            gView.at.x  = arena_cam_at_x();
            gView.at.y  = arena_cam_at_y();
            gView.at.z  = arena_cam_at_z();
            gView.rot.x = ARENA_CAM_PITCH_DEG;  /* pitch; game nudges 90/270 by 1 */
            gView.rot.y = ARENA_CAM_YAW_DEG;    /* the value that MUST stay fixed */
            gView.rot.z = 0.0f;
            gView.dist  = ARENA_CAM_DIST;
        }
```

**Do not write `gView.eye` or `gView.up`.** The game owns both. With yaw 0 and the
game's `+90` offset the eye lands at `at + (0, dist*sin(pitch), dist*cos(pitch))`
— i.e. **+Z, above** — which puts the arena's long axis (X) horizontal, as
intended. `arena_cam_eye_offset()` in the header models this for the host test
only; the patch never calls it.

**If `D_8016E134 != 0` in the arena**, the derivation does not run and the eye
will be stale. Task 3's probe reveals this immediately (eye won't track the `at`
we write). In that case, write `eye`/`up` explicitly using the model in
`arena_cam.h`, matching the game's formula exactly.

- [ ] **Step 4: Build, and verify no math libcall leaked in**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
.\build.ps1 -Config rwdi -Patches always
& "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\llvm-nm.exe" patches\arena_render.o | Select-String -Pattern "sinf|cosf|sqrtf"
```

Expected: **no output**. Any `U sinf`/`U cosf` here means a libcall leaked in and will jump to 0 at runtime (§8.11) — fix before proceeding. (The portable LLVM15 lacks `llvm-nm`; use the VS-bundled one as shown.)

- [ ] **Step 5: The core gate — prove the yaw is constant**

```powershell
.\tools\arena-soak.ps1 -Mode 6 -Constant 'rot=\([-0-9.]+,([-0-9.]+),'
```

Expected: `CONSTANT GATE: ... samples=N distinct=[0.00] -> PASS` with N ≥ 2.

**This is the whole point of the slice.** Before the override, that same command would list the rail camera's many distinct yaw values.

- [ ] **Step 6: Prove the override actually took (the named risk)**

The spec's main risk is that the game's camera routine runs *after* our hook and stomps the writes — which would look like "nothing changed" while we burn time tuning dead constants.

The probe logs `gView` at routine **entry**, i.e. before this frame's write but after the previous frame's. So:

```powershell
.\tools\arena-log.ps1 -Marker cam
```

Expected: `at=` and `eye=` at entry equal what the override writes (the arena centre and the computed eye), on every sample after the first.

**If they instead show the rail camera's values**, the override is being stomped. Do not tune constants. Apply the spec's fallback: patch the game's camera update to no-op while `arena_bridge_is_battle()`, the same shape as the existing `func_80081C50` warp patch in `patches/arena_warp.c`.

- [ ] **Step 7: Full regression**

```powershell
.\build.ps1 -Config rwdi -Soak 5
.\tools\arena-soak.ps1 -AnimProbe
```

Expected: `SOAK GREEN` (5/5) and the anim `RISING GATE` still PASS.

- [ ] **Step 8: Commit**

```bash
git add patches/arena_render.c patches/syms.ld src/arena_bridge/arena_bridge.cpp
git commit -m "feat(A1.5): fixed arena camera - own gView instead of the rail camera

Writes a static 60deg-pitch / 0deg-yaw pose every frame at the top of
arena_render_routine, BEFORE it calls func_80024744. That ordering is
load-bearing: func_80024744 rotates the stick in place by gView.rot.y, so
writing the camera after it would rotate the stick by the game's swinging yaw
while the picture used ours.

Fixing the camera fixes the INPUT too - the Nitros rail camera swings rot.y
25-89deg and cuts, which is why a held stick direction curved.

Arena centre reuses the puppets' frozen-origin mapping, so a bad origin makes
the camera wrong in the same direction as the actors rather than desynced.

Verified: -Constant gate shows a single distinct yaw; entry-logged gView matches
what we wrote (proving nothing stomps it); llvm-nm shows no math libcall leaked
into the patch; 5/5 soak; anim probe still green."
```

---

## Task 5: Framing iteration, build gate, and handoff

**Files:**
- Modify: `patches/arena_cam.h` (constants only)
- Modify: `build.ps1`

- [ ] **Step 1: Wire the host tests into the build**

In `build.ps1`, immediately after the toolchain checks and before the patches section, add:

```powershell
# --- 1b. host-side camera pose tests -----------------------------------------
# Cheap, and they catch the silent class of bug where an angle changed but its
# precomputed trig literal did not (the patch can't call sinf/cosf - notes 8.11).
$camTests = Join-Path $root "tools\run-cam-tests.ps1"
if (Test-Path $camTests) {
    & powershell -ExecutionPolicy Bypass -File $camTests
    if ($LASTEXITCODE -ne 0) { Fail "camera pose tests failed" }
}
```

- [ ] **Step 2: Verify the build gate works**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
.\build.ps1 -Config rwdi
```

Expected: `[cam-tests] OK` appears before the patches section, then a normal build.

Then temporarily break a literal (`ARENA_CAM_SIN_PITCH` → `0.5f`) and re-run. Expected: the build **stops** at `BUILD FAILED: camera pose tests failed` without compiling anything. Revert.

- [ ] **Step 3: Capture a screenshot and check the framing**

```powershell
.\tools\arena-soak.ps1 -Mode 6 -Expect '\[cam\] type=' 
```

While it dwells (10s after PASS), in a second shell:

```powershell
.\tools\capture-game.ps1
```

Then read `tools/game.png`. Check: the whole arena is in frame, the floor is not clipped, and the player is visible at a reasonable size.

- [ ] **Step 4: Iterate `ARENA_CAM_DIST` and `ARENA_CAM_AT_Y_LIFT`**

If the arena overflows the frame, increase `ARENA_CAM_DIST`; if it's tiny, decrease it. Change **only** `patches/arena_cam.h`, then:

```powershell
.\build.ps1 -Config rwdi -Patches always
```

and repeat Step 3. Each iteration is one command plus one screenshot. Stop when the arena fills a comfortable portion of the frame with margin on all sides.

- [ ] **Step 5: Final verification**

```powershell
.\build.ps1 -Config rwdi -Soak 5
.\tools\arena-soak.ps1 -Mode 6 -Constant 'rot=\([-0-9.]+,([-0-9.]+),'
.\tools\arena-soak.ps1 -AnimProbe
.\tools\run-cam-tests.ps1
```

Expected: `SOAK GREEN`, `CONSTANT GATE ... PASS`, anim `RISING GATE ... PASS`, `ALL CAMERA POSE TESTS PASSED`.

- [ ] **Step 6: Confirm the sim is untouched**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git submodule status lib/bmhero-arena
```

Expected: still `73402a3`. If it moved, revert it — this slice is fork-side only.

- [ ] **Step 7: Commit and merge**

```bash
git add build.ps1 patches/arena_cam.h
git commit -m "build(A1.5): gate builds on the camera pose tests; final framing

Constants tuned by screenshot to frame the 1896x928 arena."
git checkout master
git merge --no-ff feature/a1.5-fixed-camera -m "Merge A1.5: fixed arena camera

Static 60deg/yaw-0 camera replaces the Nitros rail camera, which swung rot.y
25-89deg and cut. Because the game rotates the stick by rot.y, this fixes the
input mapping and the picture together - a held direction no longer curves, and
foreshortening drops to sin(60)=0.87 (near-equal W/S vs A/D).

Sim untouched: TUNE_VERSION 7, hash 07fc6ade, submodule unmoved."
```

Then re-soak the merged tree before pushing (a merge commit is a different tree):

```powershell
.\build.ps1 -Config rwdi -Patches always -Soak 5
```

```bash
git push origin master
git push origin feature/a1.5-fixed-camera
```

- [ ] **Step 8: Hand off for the feel-boot**

Report to the user: the camera is fixed, and the **6°/frame turn rate now needs its real subjective confirm** — that is the reason this slice exists. Suggest `.\playrwdi.bat`. If 6° feels wrong, the loop is `tune-report.ps1 -Compare turn=1092,turn=1456` then `repin.ps1` in the canonical repo.

---

## Self-review notes

Checked against the spec:

- Spec Component 1 (measurement probe) → Task 3, including all four "questions we must not assume" as explicit recorded checks in Step 6, with a hard STOP if `gCameraType` is outside `{1,2,5,6,7,8}`.
- Spec Component 2 (override, ordering, re-assert each frame) → Task 4 Step 3, with the ordering rationale inline.
- Spec Component 3 (pose: yaw 0, pitch 60, no runtime trig, arena centre via frozen-origin mapping, tunables) → Task 1 (header) + Task 4 Step 1 (centre exports) + Task 5 (iteration).
- Spec "risk worth naming" (writes stomped) → Task 4 Step 6, with the detection method and the fallback both spelled out.
- Spec testing table → Task 1 (pose assumptions), Task 2 (`-Constant` gate), Task 3 Step 6 (offsets/type/handedness), Task 4 Steps 4–7 (libcall check, constancy, override-took, regression), Task 5 Step 3 (framing).
- Spec success criteria → Task 5 Step 5.

Two deliberate additions beyond the spec, both serving the user's "capture assumptions and regressions with tests" request:

1. **The host-test layer did not exist in the fork at all.** Task 1 creates it. This is what makes the precomputed-trig assumption machine-checked rather than a comment — the failure mode it guards (angle changed, literal not updated) is silent and would aim the camera at an arbitrary point.
2. **`build.ps1` gates on those tests** (Task 5 Step 1), so the guard cannot be bypassed by forgetting to run it.

Resolved while writing (so the plan carries no conditionals): neither
`arena_export_probe_mode` nor `arena_export_frame_count` exists in the fork.
Rather than adding two exports, the probe-mode gate and the frame throttle both
live in the native `arena_dbg_cam` — matching how `arena_dbg_anim` already
burst-logs. The patch calls it unconditionally and holds no state, which is
required anyway (a patch-local counter aborts `0xC0000409`). Net effect: one new
export instead of three.
