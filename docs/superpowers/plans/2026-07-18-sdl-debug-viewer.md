# SDL Debug Viewer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ROM-free SDL3 viewer over the A0 headless arena sim (`src/arena/`) with a Hero-style chase camera, gamepad/keyboard play, and pause/step/slow-mo feel tools.

**Architecture:** The viewer (`tools/viewer/`) is a strict client of the sim's public API (`arena_init`/`arena_tick`/`arena_hash` + read-only `ArenaState`/`arena_geoms`). Pure-logic modules (`viewer_clock`, `viewer_cam`) are float math with zero SDL dependency and are unit-tested headlessly; SDL-dependent code (`viewer_draw`, `viewer_main`) is verified by smoke-test flag + human play. Spec: `docs/superpowers/specs/2026-07-18-sdl-debug-viewer-design.md`.

**Tech Stack:** C11, SDL3 (MSYS2 UCRT64 prebuilt), CMake + Ninja, gcc (MSYS2).

## Global Constraints

- **`src/arena/` must not change.** Any diff under `src/arena/` is a plan violation. Pinned scripted-match hash: `a55aa9b1`.
- Floats are allowed ONLY in `tools/viewer/` and `tests/test_viewer_*.c`.
- Repo root: `C:\Users\dshi\GitRepos\bmhero-arena`. All commands below run from there.
- MSYS2 UCRT64 commands run as: PowerShell `$env:MSYSTEM='UCRT64'; C:\msys64\usr\bin\bash.exe -lc '<command>'` (bash login shell puts `/ucrt64/bin` on PATH). Referred to below as **[ucrt64]**.
- SDL3, not SDL2. SDL3 API notes used in this plan: `SDL_Init` returns bool; `SDL_CreateWindow(title, w, h, flags)`; `SDL_CreateRenderer(win, NULL)`; keycodes are uppercase (`SDLK_P`); key events use `e.key.key` / `e.key.mod`; gamepad hotplug events are `SDL_EVENT_GAMEPAD_ADDED/REMOVED` with `e.gdevice.which`; `SDL_GetKeyboardState` returns `const bool*`; `SDL_GetTicksNS()` for time.
- The viewer always initializes the match with `num_players >= 2` — the sim ends a round immediately when ≤1 player is alive (`arena_sim.c` step 6), so a 1-player state is unusable for feel testing.
- Commit after every task with the message given in the task.

---

### Task 1: Toolchain bootstrap (MSYS2 + SDL3) and native baseline

**Files:**
- Modify: `README.md` (append toolchain section)

**Interfaces:**
- Consumes: nothing
- Produces: working `gcc`, `cmake`, `ninja`, SDL3 dev package under `C:\msys64\ucrt64`; all later tasks build with these.

- [ ] **Step 1: Install MSYS2**

Run (PowerShell):
```powershell
winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements
```
Expected: install completes; `C:\msys64\usr\bin\bash.exe` exists.
If winget requires elevation and fails, STOP and ask the user to run `! winget install MSYS2.MSYS2` themselves, then continue.

- [ ] **Step 2: Find the SDL3 package name**

Run **[ucrt64]**:
```
pacman -Sy && pacman -Ss sdl3
```
Expected: a line like `ucrt64/mingw-w64-ucrt-x86_64-SDL3 <version>` (case may be `SDL3` or `sdl3`). Use the exact name printed in the next step.

- [ ] **Step 3: Install packages**

Run **[ucrt64]** (substitute the SDL3 package name found above):
```
pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL3
```
Expected: all four install. Verify:
```
gcc --version && cmake --version && ninja --version && pkg-config --modversion sdl3
```
Expected: version strings; sdl3 ≥ 3.0.

- [ ] **Step 4: Re-verify the sim baseline natively**

Run **[ucrt64]** from the repo root:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O2 -o test_det src/arena/arena_sim.c tests/test_determinism.c && ./test_det && rm test_det.exe
```
Expected output ends with: `ALL TESTS PASSED`

- [ ] **Step 5: Document the toolchain in README**

Append to `README.md`:
```markdown
## Windows toolchain (dev machine)

MSYS2 UCRT64: `winget install MSYS2.MSYS2`, then in a UCRT64 shell:

    pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
        mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL3

Build everything (sim, tests, viewer): `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`
```
(Adjust the SDL3 package name to whatever Step 2 found.)

- [ ] **Step 6: Commit**

```bash
git add README.md
git commit -m "docs: Windows toolchain setup (MSYS2 UCRT64 + SDL3)"
```

---

### Task 2: viewer_clock — fixed-timestep scheduler with pause/step/slow-mo (TDD)

**Files:**
- Create: `tools/viewer/viewer_clock.h`, `tools/viewer/viewer_clock.c`
- Test: `tests/test_viewer_clock.c`
- Modify: `CMakeLists.txt` (append test target)

**Interfaces:**
- Consumes: nothing (pure C, no SDL)
- Produces:
  - `void vclock_init(ViewerClock* c)`
  - `void vclock_toggle_pause(ViewerClock* c)`
  - `void vclock_queue_step(ViewerClock* c)` — queues exactly one tick, only while paused
  - `void vclock_cycle_rate(ViewerClock* c)` — 1x → 1/4x → 1/16x → 1x
  - `double vclock_tick_period_ms(const ViewerClock* c)`
  - `int vclock_advance(ViewerClock* c, double elapsed_ms)` — returns ticks to run now, 0..`VCLOCK_MAX_TICKS` (8)
  - struct fields read by HUD: `c->paused` (int), `c->rate` (int 0..2)

- [ ] **Step 1: Write the failing test**

Create `tests/test_viewer_clock.c`:
```c
/* Unit tests for the viewer's fixed-timestep scheduler. Floats OK (not sim). */
#include <stdio.h>
#include <math.h>
#include "../tools/viewer/viewer_clock.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    ViewerClock c;

    /* accumulation */
    vclock_init(&c);
    CHECK(vclock_advance(&c, 8.0) == 0, "8ms < one 60Hz period: no tick");
    CHECK(vclock_advance(&c, 9.0) == 1, "17ms accumulated: one tick");
    CHECK(vclock_advance(&c, 3 * VCLOCK_TICK_MS) == 3, "3 periods: 3 ticks");

    /* spiral-of-death cap drains the accumulator */
    vclock_init(&c);
    CHECK(vclock_advance(&c, 5000.0) == VCLOCK_MAX_TICKS, "huge elapsed capped");
    CHECK(vclock_advance(&c, VCLOCK_TICK_MS + 0.01) == 1, "accumulator drained after cap");

    /* pause and single-step */
    vclock_init(&c);
    vclock_toggle_pause(&c);
    CHECK(vclock_advance(&c, 1000.0) == 0, "paused: no ticks");
    vclock_queue_step(&c);
    CHECK(vclock_advance(&c, 0.0) == 1, "queued step fires exactly once");
    CHECK(vclock_advance(&c, 1000.0) == 0, "step does not repeat");
    vclock_toggle_pause(&c);
    CHECK(vclock_advance(&c, VCLOCK_TICK_MS * 0.5) == 0, "unpause resets accumulator");

    /* slow-mo rates */
    vclock_init(&c);
    vclock_cycle_rate(&c);
    CHECK(fabs(vclock_tick_period_ms(&c) - 4 * VCLOCK_TICK_MS) < 1e-9, "1/4x period");
    CHECK(vclock_advance(&c, 4 * VCLOCK_TICK_MS) == 1, "1/4x: 4 real periods = 1 tick");
    vclock_cycle_rate(&c);
    CHECK(vclock_advance(&c, 16 * VCLOCK_TICK_MS) == 1, "1/16x: 16 real periods = 1 tick");
    vclock_cycle_rate(&c);
    CHECK(c.rate == 0, "rate cycles back to 1x");

    /* step is pause-only */
    vclock_init(&c);
    vclock_queue_step(&c);
    CHECK(vclock_advance(&c, 0.0) == 0, "queue_step ignored while running");

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("viewer_clock: ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O2 -o t_clk tests/test_viewer_clock.c tools/viewer/viewer_clock.c
```
Expected: FAIL — the module files don't exist yet (gcc reports `viewer_clock.c`/`viewer_clock.h` not found).

- [ ] **Step 3: Write the implementation**

Create `tools/viewer/viewer_clock.h`:
```c
#ifndef VIEWER_CLOCK_H
#define VIEWER_CLOCK_H

/* Fixed-timestep tick scheduler with pause / single-step / slow-motion.
 * Pure logic, no SDL — unit-tested headlessly (tests/test_viewer_clock.c). */

#define VCLOCK_TICK_MS   (1000.0 / 60.0)
#define VCLOCK_MAX_TICKS 8   /* per advance; excess wall time is dropped */
#define VCLOCK_NUM_RATES 3   /* 1x, 1/4x, 1/16x */

typedef struct {
    double acc_ms;
    int    paused;
    int    step_queued;
    int    rate;             /* 0 = 1x, 1 = 1/4x, 2 = 1/16x */
} ViewerClock;

void   vclock_init(ViewerClock* c);
void   vclock_toggle_pause(ViewerClock* c);
void   vclock_queue_step(ViewerClock* c);   /* one tick next advance; paused only */
void   vclock_cycle_rate(ViewerClock* c);
double vclock_tick_period_ms(const ViewerClock* c);
/* Feed elapsed wall ms; returns sim ticks to run now (0..VCLOCK_MAX_TICKS). */
int    vclock_advance(ViewerClock* c, double elapsed_ms);

#endif
```

Create `tools/viewer/viewer_clock.c`:
```c
#include "viewer_clock.h"

void vclock_init(ViewerClock* c) {
    c->acc_ms = 0; c->paused = 0; c->step_queued = 0; c->rate = 0;
}

void vclock_toggle_pause(ViewerClock* c) {
    c->paused = !c->paused;
    c->acc_ms = 0;           /* no burst of ticks on unpause */
    c->step_queued = 0;
}

void vclock_queue_step(ViewerClock* c) { if (c->paused) c->step_queued = 1; }

void vclock_cycle_rate(ViewerClock* c) { c->rate = (c->rate + 1) % VCLOCK_NUM_RATES; }

double vclock_tick_period_ms(const ViewerClock* c) {
    double p = VCLOCK_TICK_MS;
    for (int i = 0; i < c->rate; i++) p *= 4.0;
    return p;
}

int vclock_advance(ViewerClock* c, double elapsed_ms) {
    if (c->paused) {
        int n = c->step_queued;
        c->step_queued = 0;
        return n;
    }
    c->acc_ms += elapsed_ms;
    double period = vclock_tick_period_ms(c);
    int n = 0;
    while (c->acc_ms >= period && n < VCLOCK_MAX_TICKS) { c->acc_ms -= period; n++; }
    if (c->acc_ms >= period) c->acc_ms = 0;   /* fell behind: drop, don't spiral */
    return n;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O2 -o t_clk tests/test_viewer_clock.c tools/viewer/viewer_clock.c && ./t_clk && rm t_clk.exe
```
Expected: `viewer_clock: ALL TESTS PASSED`

- [ ] **Step 5: Register with CMake/ctest**

Append to `CMakeLists.txt`:
```cmake
# --- viewer pure-logic modules (no SDL; always built and tested) ---
add_executable(test_viewer_clock tests/test_viewer_clock.c tools/viewer/viewer_clock.c)
add_test(NAME viewer_clock COMMAND test_viewer_clock)
```

- [ ] **Step 6: Verify the CMake build end-to-end**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: `100% tests passed` (determinism + viewer_clock).

- [ ] **Step 7: Commit**

```bash
git add tools/viewer/viewer_clock.h tools/viewer/viewer_clock.c tests/test_viewer_clock.c CMakeLists.txt
git commit -m "feat(viewer): fixed-timestep clock with pause/step/slow-mo (TDD)"
```

---

### Task 3: viewer_cam — chase/top-down camera, projection, stick mapping (TDD)

**Files:**
- Create: `tools/viewer/viewer_cam.h`, `tools/viewer/viewer_cam.c`
- Test: `tests/test_viewer_cam.c`
- Modify: `CMakeLists.txt` (append test target)

**Interfaces:**
- Consumes: nothing (pure C floats, no SDL, no sim headers)
- Produces (used by Tasks 5–8):
  - `typedef struct { float x, y, z; } Vf3;`
  - `typedef struct { float dist, height, look_up, smooth, fov_deg, ortho_halfspan; float yaw; int topdown; Vf3 pos, target; } ViewerCam;`
  - `void vcam_init(ViewerCam* c)`
  - `void vcam_update(ViewerCam* c, Vf3 player_pos, float player_yaw_rad)`
  - `int  vcam_project(const ViewerCam* c, Vf3 p, int w, int h, float* sx, float* sy, float* depth)` — returns 0 when behind camera
  - `float vcam_screen_radius(const ViewerCam* c, Vf3 p, float world_r, int w, int h)` — projected pixel radius, 0 when behind
  - `void vcam_stick_to_world(const ViewerCam* c, float in_x, float in_y, int* out_sx, int* out_sy)` — camera-relative stick → sim ints in [-31, 31]
  - `VCAM_BINANG_TO_RAD(a)` — sim binary angle (u16) → radians
- Conventions (locked by tests): world +Y up; sim "stick up" = world −Z; camera yaw 0 looks along −Z, so it tracks the sim's binary-angle facing directly.

- [ ] **Step 1: Write the failing test**

Create `tests/test_viewer_cam.c`:
```c
/* Unit tests for the viewer camera: rig, smoothing, projection, stick map. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../tools/viewer/viewer_cam.h"

#define PI_F 3.14159265f
static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

int main(void) {
    ViewerCam c;
    float sx, sy, d;

    /* rig: instant follow puts the camera behind and above a -Z-facing player */
    vcam_init(&c);
    c.smooth = 1.0f;
    vcam_update(&c, (Vf3){0, 0, 0}, 0.0f);
    CHECK(NEAR(c.pos.x, 0, 1e-4f) && NEAR(c.pos.y, 5.0f, 1e-4f) && NEAR(c.pos.z, 4.0f, 1e-4f),
          "camera at (0, height, +dist) behind -Z-facing player");
    CHECK(NEAR(c.target.y, 1.0f, 1e-4f), "look target lifted by look_up");

    /* yaw smoothing takes the short way across +/-pi */
    vcam_init(&c);
    c.yaw = 2.8f; c.smooth = 0.25f;
    vcam_update(&c, (Vf3){0, 0, 0}, -2.8f);
    CHECK(c.yaw > 2.8f || c.yaw < -2.8f, "yaw wraps across pi, no long-way spin");

    /* perspective projection */
    vcam_init(&c);
    c.smooth = 1.0f;
    vcam_update(&c, (Vf3){0, 0, 0}, 0.0f);
    CHECK(vcam_project(&c, c.target, 1280, 720, &sx, &sy, &d) == 1, "target visible");
    CHECK(NEAR(sx, 640, 0.5f) && NEAR(sy, 360, 0.5f), "look target projects to center");
    CHECK(d > 0, "depth positive");
    CHECK(vcam_project(&c, (Vf3){1, 1, 0}, 1280, 720, &sx, &sy, &d) == 1, "offset visible");
    CHECK(sx > 640.0f, "point right of view projects right of center");
    CHECK(vcam_project(&c, (Vf3){0, 9, 8}, 1280, 720, &sx, &sy, &d) == 0, "behind camera culled");

    float d_near, d_far;
    vcam_project(&c, (Vf3){0, 1, 0}, 1280, 720, &sx, &sy, &d_near);
    vcam_project(&c, (Vf3){0, 1, -3}, 1280, 720, &sx, &sy, &d_far);
    CHECK(d_near < d_far, "painter depth ordering");

    /* screen radius shrinks with distance */
    float r_near = vcam_screen_radius(&c, (Vf3){0, 1, 0}, 0.35f, 1280, 720);
    float r_far  = vcam_screen_radius(&c, (Vf3){0, 1, -3}, 0.35f, 1280, 720);
    CHECK(r_near > r_far && r_far > 0, "screen radius perspective-scales");

    /* top-down ortho */
    vcam_init(&c);
    c.topdown = 1;
    CHECK(vcam_project(&c, (Vf3){0, 0, 0}, 1280, 720, &sx, &sy, &d) == 1, "topdown visible");
    CHECK(NEAR(sx, 640, 0.5f) && NEAR(sy, 360, 0.5f), "topdown origin centers");
    vcam_project(&c, (Vf3){1, 0, 0}, 1280, 720, &sx, &sy, &d);
    CHECK(NEAR(sx, 640 + 48, 0.5f), "topdown scale: 720/(2*7.5) = 48 px per unit");

    /* camera-relative stick mapping */
    int ix, iy;
    vcam_init(&c);
    c.yaw = 0.0f;
    vcam_stick_to_world(&c, 0, 1, &ix, &iy);
    CHECK(ix == 0 && iy == -31, "yaw 0: stick up = world -Z");
    vcam_stick_to_world(&c, 1, 0, &ix, &iy);
    CHECK(ix == 31 && iy == 0, "yaw 0: stick right = world +X");
    c.yaw = PI_F / 2;
    vcam_stick_to_world(&c, 0, 1, &ix, &iy);
    CHECK(ix == 31 && abs(iy) <= 1, "yaw 90: stick up = world +X");
    c.yaw = PI_F;
    vcam_stick_to_world(&c, 0, 1, &ix, &iy);
    CHECK(abs(ix) <= 1 && iy == 31, "yaw 180: stick up = world +Z");
    c.yaw = 0.0f;
    vcam_stick_to_world(&c, 1, 1, &ix, &iy);
    CHECK(ix * ix + iy * iy <= 32 * 32, "diagonal magnitude clamped to 1.0");
    /* top-down bypasses camera yaw */
    c.topdown = 1; c.yaw = PI_F / 2;
    vcam_stick_to_world(&c, 0, 1, &ix, &iy);
    CHECK(ix == 0 && iy == -31, "topdown: identity mapping regardless of yaw");

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("viewer_cam: ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O2 -o t_cam tests/test_viewer_cam.c tools/viewer/viewer_cam.c -lm
```
Expected: FAIL — `viewer_cam.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `tools/viewer/viewer_cam.h`:
```c
#ifndef VIEWER_CAM_H
#define VIEWER_CAM_H

/* Chase / top-down camera, projection, and camera-relative stick mapping.
 * Pure float math, no SDL — unit-tested headlessly (tests/test_viewer_cam.c).
 * Conventions: world +Y up; sim "stick up" = world -Z; camera yaw 0 looks
 * along -Z, so camera yaw tracks the sim's binary-angle facing directly. */

typedef struct { float x, y, z; } Vf3;

typedef struct {
    /* rig constants — viewer-only, tune freely */
    float dist, height, look_up;   /* boom length, eye height, aim height */
    float smooth;                  /* yaw lerp per update, 0..1 */
    float fov_deg;
    float ortho_halfspan;          /* top-down: world units center -> edge */
    /* state */
    float yaw;                     /* radians */
    int   topdown;
    Vf3   pos, target;             /* derived by vcam_update */
} ViewerCam;

void  vcam_init(ViewerCam* c);
void  vcam_update(ViewerCam* c, Vf3 player_pos, float player_yaw_rad);
/* World -> screen. 0 if behind camera (chase mode). depth: view-space
 * distance for painter sorting (bigger = farther). */
int   vcam_project(const ViewerCam* c, Vf3 p, int w, int h,
                   float* sx, float* sy, float* depth);
/* Projected pixel radius of a world-space sphere; 0 if behind camera. */
float vcam_screen_radius(const ViewerCam* c, Vf3 p, float world_r, int w, int h);
/* Camera-relative stick (in_x right+, in_y forward+, [-1,1]) -> sim stick
 * ints in [-31,31] on world X/Z. Identity mapping in top-down mode. */
void  vcam_stick_to_world(const ViewerCam* c, float in_x, float in_y,
                          int* out_sx, int* out_sy);

#define VCAM_BINANG_TO_RAD(a) ((float)(a) * 9.5873799e-5f) /* 2*pi / 65536 */

#endif
```

Create `tools/viewer/viewer_cam.c`:
```c
#include <math.h>
#include "viewer_cam.h"

#define PIF 3.14159265f

static float wrap_pi(float a) {
    while (a >  PIF) a -= 2 * PIF;
    while (a < -PIF) a += 2 * PIF;
    return a;
}

void vcam_init(ViewerCam* c) {
    c->dist = 4.0f; c->height = 5.0f; c->look_up = 1.0f;   /* ~45 deg pitch */
    c->smooth = 0.08f; c->fov_deg = 60.0f; c->ortho_halfspan = 7.5f;
    c->yaw = 0.0f; c->topdown = 0;
    c->pos = (Vf3){0, c->height, c->dist};
    c->target = (Vf3){0, c->look_up, 0};
}

void vcam_update(ViewerCam* c, Vf3 p, float yaw_rad) {
    c->yaw = wrap_pi(c->yaw + c->smooth * wrap_pi(yaw_rad - c->yaw));
    c->target = (Vf3){ p.x, p.y + c->look_up, p.z };
    float fx = sinf(c->yaw), fz = -cosf(c->yaw);   /* look dir on XZ */
    c->pos = (Vf3){ p.x - fx * c->dist, p.y + c->height, p.z - fz * c->dist };
}

static Vf3   vsub(Vf3 a, Vf3 b) { return (Vf3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static float vdot(Vf3 a, Vf3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vf3   vcross(Vf3 a, Vf3 b) {
    return (Vf3){a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static Vf3 vnorm(Vf3 a) {
    float l = sqrtf(vdot(a, a));
    return (l > 1e-6f) ? (Vf3){a.x / l, a.y / l, a.z / l} : (Vf3){0, 0, 0};
}

int vcam_project(const ViewerCam* c, Vf3 p, int w, int h,
                 float* sx, float* sy, float* depth) {
    if (c->topdown) {
        float scale = (float)(w < h ? w : h) / (2.0f * c->ortho_halfspan);
        *sx = (float)w * 0.5f + p.x * scale;
        *sy = (float)h * 0.5f + p.z * scale;
        *depth = 1000.0f - p.y;   /* higher = nearer */
        return 1;
    }
    Vf3 F = vnorm(vsub(c->target, c->pos));
    Vf3 R = vnorm(vcross(F, (Vf3){0, 1, 0}));
    Vf3 U = vcross(R, F);
    Vf3 v = vsub(p, c->pos);
    float dz = vdot(v, F);
    if (dz < 0.1f) return 0;
    float f = ((float)h * 0.5f) / tanf(c->fov_deg * 0.5f * PIF / 180.0f);
    *sx = (float)w * 0.5f + f * vdot(v, R) / dz;
    *sy = (float)h * 0.5f - f * vdot(v, U) / dz;
    *depth = dz;
    return 1;
}

float vcam_screen_radius(const ViewerCam* c, Vf3 p, float world_r, int w, int h) {
    float sx0, sy0, sx1, sy1, d;
    if (!vcam_project(c, p, w, h, &sx0, &sy0, &d)) return 0;
    if (c->topdown) {
        float scale = (float)(w < h ? w : h) / (2.0f * c->ortho_halfspan);
        return world_r * scale;
    }
    Vf3 F = vnorm(vsub(c->target, c->pos));
    Vf3 R = vnorm(vcross(F, (Vf3){0, 1, 0}));
    Vf3 q = { p.x + R.x * world_r, p.y + R.y * world_r, p.z + R.z * world_r };
    if (!vcam_project(c, q, w, h, &sx1, &sy1, &d)) return 0;
    float dx = sx1 - sx0, dy = sy1 - sy0;
    return sqrtf(dx * dx + dy * dy);
}

void vcam_stick_to_world(const ViewerCam* c, float in_x, float in_y,
                         int* out_sx, int* out_sy) {
    float wx, wz;
    if (c->topdown) {
        wx = in_x; wz = -in_y;
    } else {
        float fx = sinf(c->yaw), fz = -cosf(c->yaw);   /* camera forward, XZ */
        float rx = -fz,          rz = fx;              /* camera right, XZ */
        wx = rx * in_x + fx * in_y;
        wz = rz * in_x + fz * in_y;
    }
    float m = sqrtf(wx * wx + wz * wz);
    if (m > 1.0f) { wx /= m; wz /= m; }
    *out_sx = (int)lroundf(wx * 31.0f);
    *out_sy = (int)lroundf(wz * 31.0f);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O2 -o t_cam tests/test_viewer_cam.c tools/viewer/viewer_cam.c -lm && ./t_cam && rm t_cam.exe
```
Expected: `viewer_cam: ALL TESTS PASSED`

- [ ] **Step 5: Register with CMake/ctest**

Append to `CMakeLists.txt` (right after the `test_viewer_clock` block):
```cmake
add_executable(test_viewer_cam tests/test_viewer_cam.c tools/viewer/viewer_cam.c)
if(NOT MSVC)
  target_link_libraries(test_viewer_cam m)
endif()
add_test(NAME viewer_cam COMMAND test_viewer_cam)
```

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: `100% tests passed` (3 tests).

- [ ] **Step 6: Commit**

```bash
git add tools/viewer/viewer_cam.h tools/viewer/viewer_cam.c tests/test_viewer_cam.c CMakeLists.txt
git commit -m "feat(viewer): chase/top-down camera, projection, stick mapping (TDD)"
```

---

### Task 4: CMake viewer target + SDL3 boot window

**Files:**
- Modify: `CMakeLists.txt` (scope strict warnings to the sim; add guarded viewer target)
- Create: `tools/viewer/viewer_main.c` (minimal boot version — replaced wholesale in Task 6)

**Interfaces:**
- Consumes: nothing yet (boots SDL only)
- Produces: `arena_viewer` CMake target; windowed app skeleton with event loop and `--frames N` smoke flag that later tasks extend.

- [ ] **Step 1: Restructure warnings in CMakeLists.txt**

Replace the whole `CMakeLists.txt` with:
```cmake
cmake_minimum_required(VERSION 3.15)
project(bmhero_arena C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(arena_sim STATIC src/arena/arena_sim.c)
target_include_directories(arena_sim PUBLIC src)
if(NOT MSVC)
  # determinism bugs love to hide in conversion warnings — sim stays strictest
  target_compile_options(arena_sim PRIVATE -Wconversion -Wno-sign-conversion)
endif()

enable_testing()
add_executable(test_determinism tests/test_determinism.c)
target_link_libraries(test_determinism arena_sim)
if(NOT MSVC)
  target_compile_options(test_determinism PRIVATE -Wconversion -Wno-sign-conversion)
endif()
add_test(NAME determinism COMMAND test_determinism)

# --- viewer pure-logic modules (no SDL; always built and tested) ---
add_executable(test_viewer_clock tests/test_viewer_clock.c tools/viewer/viewer_clock.c)
add_test(NAME viewer_clock COMMAND test_viewer_clock)

add_executable(test_viewer_cam tests/test_viewer_cam.c tools/viewer/viewer_cam.c)
if(NOT MSVC)
  target_link_libraries(test_viewer_cam m)
endif()
add_test(NAME viewer_cam COMMAND test_viewer_cam)

# --- debug viewer (dev tool; requires SDL3, skipped when absent) ---
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_executable(arena_viewer
    tools/viewer/viewer_main.c
    tools/viewer/viewer_cam.c
    tools/viewer/viewer_clock.c)
  target_link_libraries(arena_viewer arena_sim SDL3::SDL3)
  if(NOT MSVC)
    target_link_libraries(arena_viewer m)
  endif()
  message(STATUS "arena_viewer: SDL3 found - building viewer")
else()
  message(STATUS "arena_viewer: SDL3 not found - viewer skipped (sim + tests unaffected)")
endif()
```
(Note: `tools/viewer/viewer_draw.c` is added to this target in Task 5.)

- [ ] **Step 2: Write the minimal boot main**

Create `tools/viewer/viewer_main.c`:
```c
/* arena_viewer — SDL3 debug viewer over the deterministic arena sim.
 * Dev tool: floats allowed here; the sim (src/arena/) stays pure.
 * --frames N : run N frames then exit (smoke test; deterministic in Task 6). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    int frames_limit = -1;
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--frames") == 0) frames_limit = atoi(argv[i + 1]);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("bmhero arena viewer", 1280, 720,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!win || !ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    int running = 1, frame = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = 0;
        }
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        frame++;
        if (frames_limit >= 0 && frame >= frames_limit) running = 0;
    }
    printf("frames %d\n", frame);
    SDL_Quit();
    return 0;
}
```

- [ ] **Step 3: Build and smoke-test**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake -S . -B build -G Ninja && cmake --build build && ./build/arena_viewer.exe --frames 60
```
Expected: configure prints `arena_viewer: SDL3 found - building viewer`; a dark window flashes ~1s; output `frames 60`; exit code 0. Run `ctest --test-dir build` — still `100% tests passed`.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt tools/viewer/viewer_main.c
git commit -m "feat(viewer): CMake target + SDL3 boot window with --frames smoke flag"
```

---

### Task 5: viewer_draw — static arena scene (geometry, grid, painter)

**Files:**
- Create: `tools/viewer/viewer_draw.h`, `tools/viewer/viewer_draw.c`
- Modify: `CMakeLists.txt` (add `viewer_draw.c` to `arena_viewer` sources)
- Modify: `tools/viewer/viewer_main.c` (render the scene with a fixed camera)

**Interfaces:**
- Consumes: `ViewerCam`/`vcam_project`/`vcam_screen_radius` (Task 3), sim headers (read-only)
- Produces (final signatures; Task 6 fills in entities, Task 8 fills in HUD):
  - `#define QF(v) ((float)(v) / 4096.0f)` — Q20.12 → float
  - `void draw_scene(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s, int w, int h, int show_grid)`
  - `void draw_hud(SDL_Renderer* r, const ArenaState* s, const ViewerClock* clk, const ViewerCam* cam, int cam_target, int w, int h)` (stub until Task 8)
  - `void draw_text(SDL_Renderer* r, float x, float y, float scale, const char* str)` (stub until Task 8)

- [ ] **Step 1: Write the draw module (geometry only)**

Create `tools/viewer/viewer_draw.h`:
```c
#ifndef VIEWER_DRAW_H
#define VIEWER_DRAW_H

#include <SDL3/SDL.h>
#include "arena/arena_sim.h"
#include "arena/arena_geom.h"
#include "arena/arena_tuning.h"
#include "viewer_cam.h"
#include "viewer_clock.h"

#define QF(v) ((float)(v) / 4096.0f)   /* Q20.12 -> float (render side only) */

void draw_scene(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s,
                int w, int h, int show_grid);
void draw_hud(SDL_Renderer* r, const ArenaState* s, const ViewerClock* clk,
              const ViewerCam* cam, int cam_target, int w, int h);
void draw_text(SDL_Renderer* r, float x, float y, float scale, const char* str);

#endif
```

Create `tools/viewer/viewer_draw.c`:
```c
/* All rendering: world geometry + entities through a unified painter list,
 * then screen-space overlays. Floats OK — this is viewer code. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "viewer_draw.h"

/* ------------------------------------------------- unified painter list */

#define MAX_DRAW 1024

typedef struct {
    int        kind;        /* 0 = quad face, 1 = circle */
    float      depth;
    SDL_FColor col;
    SDL_FPoint p[4];        /* face: projected corners */
    float      cx, cy, rad; /* circle: projected center + pixel radius */
} Drawable;

static Drawable g_draw[MAX_DRAW];
static int      g_ndraw;

static int draw_cmp(const void* pa, const void* pb) {
    float da = ((const Drawable*)pa)->depth, db = ((const Drawable*)pb)->depth;
    return (da < db) - (da > db);   /* descending: far first */
}

static void add_face(const ViewerCam* cam, int w, int h,
                     Vf3 a, Vf3 b, Vf3 c, Vf3 d, SDL_FColor col) {
    if (g_ndraw >= MAX_DRAW) return;
    Vf3 corners[4] = {a, b, c, d};
    Drawable* dr = &g_draw[g_ndraw];
    float depth_sum = 0;
    for (int i = 0; i < 4; i++) {
        float sx, sy, dep;
        if (!vcam_project(cam, corners[i], w, h, &sx, &sy, &dep)) return;
        dr->p[i] = (SDL_FPoint){sx, sy};
        depth_sum += dep;
    }
    /* flat shade from the face normal */
    Vf3 e1 = {b.x - a.x, b.y - a.y, b.z - a.z};
    Vf3 e2 = {d.x - a.x, d.y - a.y, d.z - a.z};
    Vf3 n  = {e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x};
    float l = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
    float lit = 0;
    if (l > 1e-6f) lit = (n.x*0.45f + n.y*0.80f + n.z*0.30f) / l;
    if (lit < 0) lit = -lit;               /* winding-agnostic */
    float shade = 0.6f + 0.4f * lit;
    dr->kind  = 0;
    dr->depth = depth_sum * 0.25f;
    dr->col   = (SDL_FColor){col.r * shade, col.g * shade, col.b * shade, col.a};
    g_ndraw++;
}

static void add_box(const ViewerCam* cam, int w, int h,
                    Vf3 mn, Vf3 mx, SDL_FColor col) {
    /* top + 4 sides (bottom never visible) */
    add_face(cam, w, h, (Vf3){mn.x,mx.y,mn.z}, (Vf3){mx.x,mx.y,mn.z},
                        (Vf3){mx.x,mx.y,mx.z}, (Vf3){mn.x,mx.y,mx.z}, col);
    add_face(cam, w, h, (Vf3){mn.x,mn.y,mn.z}, (Vf3){mx.x,mn.y,mn.z},
                        (Vf3){mx.x,mx.y,mn.z}, (Vf3){mn.x,mx.y,mn.z}, col);
    add_face(cam, w, h, (Vf3){mn.x,mn.y,mx.z}, (Vf3){mx.x,mn.y,mx.z},
                        (Vf3){mx.x,mx.y,mx.z}, (Vf3){mn.x,mx.y,mx.z}, col);
    add_face(cam, w, h, (Vf3){mn.x,mn.y,mn.z}, (Vf3){mn.x,mn.y,mx.z},
                        (Vf3){mn.x,mx.y,mx.z}, (Vf3){mn.x,mx.y,mn.z}, col);
    add_face(cam, w, h, (Vf3){mx.x,mn.y,mn.z}, (Vf3){mx.x,mn.y,mx.z},
                        (Vf3){mx.x,mx.y,mx.z}, (Vf3){mx.x,mx.y,mn.z}, col);
}

static void add_circle(const ViewerCam* cam, int w, int h,
                       Vf3 center, float world_r, SDL_FColor col, float depth_bias) {
    if (g_ndraw >= MAX_DRAW) return;
    float sx, sy, dep;
    if (!vcam_project(cam, center, w, h, &sx, &sy, &dep)) return;
    float rad = vcam_screen_radius(cam, center, world_r, w, h);
    if (rad < 0.5f) return;
    Drawable* dr = &g_draw[g_ndraw++];
    dr->kind = 1; dr->depth = dep + depth_bias; dr->col = col;
    dr->cx = sx; dr->cy = sy; dr->rad = rad;
}

static void draw_circle(SDL_Renderer* r, float cx, float cy, float rad, SDL_FColor col) {
    enum { SEG = 20 };
    SDL_Vertex vtx[SEG + 2];
    int idx[SEG * 3];
    vtx[0].position = (SDL_FPoint){cx, cy};
    for (int i = 0; i <= SEG; i++) {
        float a = (float)i * (2.0f * 3.14159265f / SEG);
        vtx[i + 1].position = (SDL_FPoint){cx + cosf(a) * rad, cy + sinf(a) * rad};
    }
    for (int i = 0; i < SEG + 2; i++) {
        vtx[i].color = col;
        vtx[i].tex_coord = (SDL_FPoint){0, 0};
    }
    int n = 0;
    for (int i = 0; i < SEG; i++) { idx[n++] = 0; idx[n++] = i + 1; idx[n++] = i + 2; }
    SDL_RenderGeometry(r, NULL, vtx, SEG + 2, idx, SEG * 3);
}

static void flush_drawables(SDL_Renderer* r) {
    qsort(g_draw, (size_t)g_ndraw, sizeof(Drawable), draw_cmp);
    for (int i = 0; i < g_ndraw; i++) {
        Drawable* d = &g_draw[i];
        if (d->kind == 0) {
            SDL_Vertex v[4];
            int idx[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 4; k++) {
                v[k].position  = d->p[k];
                v[k].color     = d->col;
                v[k].tex_coord = (SDL_FPoint){0, 0};
            }
            SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
        } else {
            draw_circle(r, d->cx, d->cy, d->rad, d->col);
        }
    }
}

static void draw_world_line(SDL_Renderer* r, const ViewerCam* cam,
                            Vf3 a, Vf3 b, int w, int h) {
    float ax, ay, bx, by, d;
    if (!vcam_project(cam, a, w, h, &ax, &ay, &d)) return;
    if (!vcam_project(cam, b, w, h, &bx, &by, &d)) return;
    SDL_RenderLine(r, ax, ay, bx, by);
}

/* ---------------------------------------------------------------- scene */

static void add_entities(const ViewerCam* cam, const ArenaState* s, int w, int h);

void draw_scene(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s,
                int w, int h, int show_grid) {
    const ArenaGeom* g = &arena_geoms[s->arena_id];
    float full = QF(g->half_extent);
    float ext  = full;
    if (s->phase == PHASE_SUDDEN_DEATH)
        ext -= (float)s->shrink_step * 0.03f;   /* mirrors sim wall shrink */

    /* grid first: always behind everything */
    if (show_grid) {
        SDL_SetRenderDrawColor(r, 45, 50, 62, 255);
        for (int i = -(int)full; i <= (int)full; i++) {
            draw_world_line(r, cam, (Vf3){(float)i, 0, -full}, (Vf3){(float)i, 0, full}, w, h);
            draw_world_line(r, cam, (Vf3){-full, 0, (float)i}, (Vf3){full, 0, (float)i}, w, h);
        }
    }

    g_ndraw = 0;

    /* floor slab, slightly below y=0 so ground entities draw on top */
    add_box(cam, w, h, (Vf3){-full, -0.15f, -full}, (Vf3){full, -0.02f, full},
            (SDL_FColor){0.16f, 0.18f, 0.22f, 1});

    /* boundary walls at the (possibly shrunken) extent; red in sudden death */
    SDL_FColor wallc = (s->phase == PHASE_SUDDEN_DEATH)
                     ? (SDL_FColor){0.55f, 0.25f, 0.25f, 1}
                     : (SDL_FColor){0.30f, 0.34f, 0.42f, 1};
    float wt = 0.20f, wh = 1.2f;
    add_box(cam, w, h, (Vf3){-ext - wt, 0, -ext - wt}, (Vf3){ ext + wt, wh, -ext}, wallc);
    add_box(cam, w, h, (Vf3){-ext - wt, 0,  ext},      (Vf3){ ext + wt, wh,  ext + wt}, wallc);
    add_box(cam, w, h, (Vf3){-ext - wt, 0, -ext},      (Vf3){-ext,      wh,  ext}, wallc);
    add_box(cam, w, h, (Vf3){ ext,      0, -ext},      (Vf3){ ext + wt, wh,  ext}, wallc);

    /* pillars */
    for (int i = 0; i < g->num_pillars; i++) {
        const Aabb* b = &g->pillars[i];
        add_box(cam, w, h,
                (Vf3){QF(b->min.x), QF(b->min.y), QF(b->min.z)},
                (Vf3){QF(b->max.x), QF(b->max.y), QF(b->max.z)},
                (SDL_FColor){0.42f, 0.40f, 0.36f, 1});
    }

    add_entities(cam, s, w, h);
    flush_drawables(r);
}

/* Entities are added in Task 6; keep an empty hook until then. */
static void add_entities(const ViewerCam* cam, const ArenaState* s, int w, int h) {
    (void)cam; (void)s; (void)w; (void)h;
}

/* HUD is implemented in Task 8; stubs keep the link happy. */
void draw_text(SDL_Renderer* r, float x, float y, float scale, const char* str) {
    (void)r; (void)x; (void)y; (void)scale; (void)str;
}
void draw_hud(SDL_Renderer* r, const ArenaState* s, const ViewerClock* clk,
              const ViewerCam* cam, int cam_target, int w, int h) {
    (void)r; (void)s; (void)clk; (void)cam; (void)cam_target; (void)w; (void)h;
}
```

- [ ] **Step 2: Add to the viewer target**

In `CMakeLists.txt`, change the `arena_viewer` sources to:
```cmake
  add_executable(arena_viewer
    tools/viewer/viewer_main.c
    tools/viewer/viewer_cam.c
    tools/viewer/viewer_clock.c
    tools/viewer/viewer_draw.c)
```

- [ ] **Step 3: Render the static scene from main**

In `tools/viewer/viewer_main.c`, replace the includes and the main loop body so the file becomes:
```c
/* arena_viewer — SDL3 debug viewer over the deterministic arena sim.
 * Dev tool: floats allowed here; the sim (src/arena/) stays pure.
 * --frames N : run N frames then exit (smoke test). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/arena_sim.h"
#include "viewer_cam.h"
#include "viewer_clock.h"
#include "viewer_draw.h"

int main(int argc, char** argv) {
    int frames_limit = -1;
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--frames") == 0) frames_limit = atoi(argv[i + 1]);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("bmhero arena viewer", 1280, 720,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!win || !ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    ArenaState state;
    arena_init(&state, 0, 2, 0xC0FFEE);

    ViewerCam cam;
    vcam_init(&cam);

    int running = 1, frame = 0, show_grid = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE) running = 0;
                if (e.key.key == SDLK_F1) cam.topdown = !cam.topdown;
                if (e.key.key == SDLK_G)  show_grid = !show_grid;
            }
        }

        /* fixed camera over spawn 0 until the sim is wired in (Task 6) */
        vcam_update(&cam, (Vf3){-4.5f, 0, -4.5f}, 0.0f);

        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        draw_scene(ren, &cam, &state, w, h, show_grid);
        SDL_RenderPresent(ren);
        frame++;
        if (frames_limit >= 0 && frame >= frames_limit) running = 0;
    }
    printf("frames %d\n", frame);
    SDL_Quit();
    return 0;
}
```

- [ ] **Step 4: Build and verify**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake --build build && ./build/arena_viewer.exe --frames 180
```
Expected: ~3s window showing the arena floor grid, four grey-brown pillars, blue-grey boundary walls, viewed from behind spawn 0 at ~45°; `F1` flips to top-down; prints `frames 180`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add tools/viewer/viewer_draw.h tools/viewer/viewer_draw.c tools/viewer/viewer_main.c CMakeLists.txt
git commit -m "feat(viewer): static arena scene - painter renderer, grid, walls, pillars"
```

---

### Task 6: Sim wiring — keyboard play, entities, feel keys

**Files:**
- Modify: `tools/viewer/viewer_main.c` (full replacement below)
- Modify: `tools/viewer/viewer_draw.c` (replace the `add_entities` stub)

**Interfaces:**
- Consumes: everything from Tasks 2–5; sim API `arena_init(&s, arena_id, num_players, seed)`, `arena_tick(&s, inputs[4])`, `arena_input_pack(sx, sy, jump, bomb)`, `arena_hash(&s)`
- Produces: playable viewer. `--frames N` becomes deterministic (1 tick/frame, forced-neutral inputs, prints final tick + hash) — later tasks must not break this.

- [ ] **Step 1: Implement entity rendering**

In `tools/viewer/viewer_draw.c`, replace the empty `add_entities` stub with:
```c
static void add_entities(const ViewerCam* cam, const ArenaState* s, int w, int h) {
    static const SDL_FColor pcol[4] = {
        {0.95f, 0.95f, 0.95f, 1}, {0.25f, 0.25f, 0.30f, 1},
        {0.90f, 0.30f, 0.25f, 1}, {0.30f, 0.42f, 0.90f, 1},
    };
    static const SDL_FColor shadow = {0, 0, 0, 0.35f};

    for (int i = 0; i < s->num_players; i++) {
        const ArenaPlayer* p = &s->players[i];
        Vf3 pos = {QF(p->pos.x), QF(p->pos.y), QF(p->pos.z)};
        SDL_FColor c = pcol[i];
        if (p->state == PSTATE_DEAD) {
            c.a = 0.25f;
        } else {
            if (p->state == PSTATE_TUMBLE)
                c = (SDL_FColor){1.0f, 0.60f, 0.15f, 1};
            else if (p->timer > 0 && ((s->tick >> 2) & 1))
                c.a = 0.45f;                     /* post-hit invuln flash */
            add_circle(cam, w, h, (Vf3){pos.x, 0.01f, pos.z},
                       QF(TUNE_PLAYER_RADIUS), shadow, 0.01f);
        }
        add_circle(cam, w, h, (Vf3){pos.x, pos.y + 0.45f, pos.z},
                   QF(TUNE_PLAYER_RADIUS), c, 0);
    }

    for (int i = 0; i < ARENA_MAX_BOMBS; i++) {
        const ArenaBomb* b = &s->bombs[i];
        if (b->state == BSTATE_FREE || b->state == BSTATE_EXPLODING) continue;
        Vf3 pos = {QF(b->pos.x), QF(b->pos.y), QF(b->pos.z)};
        SDL_FColor c = {0.16f, 0.16f, 0.18f, 1};
        if (b->state == BSTATE_SETTLED && ((b->fuse / 10) & 1))
            c = (SDL_FColor){0.85f, 0.20f, 0.15f, 1};    /* fuse flash */
        if (b->state != BSTATE_HELD)
            add_circle(cam, w, h, (Vf3){pos.x, 0.01f, pos.z},
                       QF(TUNE_BOMB_RADIUS), shadow, 0.01f);
        add_circle(cam, w, h, (Vf3){pos.x, pos.y + 0.3f, pos.z},
                   QF(TUNE_BOMB_RADIUS), c, 0);
    }

    for (int i = 0; i < ARENA_MAX_BLASTS; i++) {
        const ArenaBlast* bl = &s->blasts[i];
        if (bl->ttl == 0) continue;
        float fr = (float)bl->radius_t / (float)TUNE_BLAST_GROW_TICKS;
        if (fr > 1) fr = 1;
        add_circle(cam, w, h,
                   (Vf3){QF(bl->center.x), QF(bl->center.y) + 0.2f, QF(bl->center.z)},
                   fr * QF(TUNE_BLAST_RADIUS),
                   (SDL_FColor){1.0f, 0.55f, 0.10f,
                                0.55f * (float)bl->ttl / (float)TUNE_BLAST_TTL},
                   -0.02f);
    }
}
```
Also add facing indicators: append this function at the end of `viewer_draw.c` and declare it in `viewer_draw.h` as `void draw_facing(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s, int w, int h);`
```c
void draw_facing(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s,
                 int w, int h) {
    SDL_SetRenderDrawColor(r, 250, 250, 250, 255);
    for (int i = 0; i < s->num_players; i++) {
        const ArenaPlayer* p = &s->players[i];
        if (p->state == PSTATE_DEAD) continue;
        float yr = VCAM_BINANG_TO_RAD(p->yaw);
        Vf3 a = {QF(p->pos.x), QF(p->pos.y) + 0.45f, QF(p->pos.z)};
        Vf3 b = {a.x + 0.55f * sinf(yr), a.y, a.z - 0.55f * cosf(yr)};
        draw_world_line(r, cam, a, b, w, h);
    }
}
```

- [ ] **Step 2: Full main with sim, keyboard input, and feel keys**

Replace `tools/viewer/viewer_main.c` entirely with:
```c
/* arena_viewer — SDL3 debug viewer over the deterministic arena sim.
 * Dev tool: floats allowed here; the sim (src/arena/) stays pure.
 * --frames N : deterministic smoke run — exactly one tick per frame with
 *              neutral inputs, prints "frames N tick T hash H", then exits.
 * --seed X   : match seed (hex ok), default 0xC0FFEE. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/arena_sim.h"
#include "viewer_cam.h"
#include "viewer_clock.h"
#include "viewer_draw.h"

typedef struct { SDL_Gamepad* pad[ARENA_MAX_PLAYERS]; } Pads;

static void pads_add(Pads* p, SDL_JoystickID id) {
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        if (!p->pad[i]) { p->pad[i] = SDL_OpenGamepad(id); return; }
}
static void pads_remove(Pads* p, SDL_JoystickID id) {
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        if (p->pad[i] && SDL_GetGamepadID(p->pad[i]) == id) {
            SDL_CloseGamepad(p->pad[i]);
            p->pad[i] = NULL;
            return;
        }
}
/* keyboard drives the lowest player slot without a pad */
static int keyboard_player(const Pads* p) {
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        if (!p->pad[i]) return i;
    return -1;
}

static ArenaInput read_input(const Pads* pads, int player, const ViewerCam* cam) {
    float ix = 0, iy = 0;
    int jump = 0, bomb = 0;
    if (pads->pad[player]) {
        SDL_Gamepad* gp = pads->pad[player];
        ix =  (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        iy = -(float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        jump = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH);
        bomb = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST);
    } else if (player == keyboard_player(pads)) {
        const bool* k = SDL_GetKeyboardState(NULL);
        ix = (float)((k[SDL_SCANCODE_D] ? 1 : 0) - (k[SDL_SCANCODE_A] ? 1 : 0));
        iy = (float)((k[SDL_SCANCODE_W] ? 1 : 0) - (k[SDL_SCANCODE_S] ? 1 : 0));
        jump = k[SDL_SCANCODE_SPACE] ? 1 : 0;
        bomb = k[SDL_SCANCODE_LSHIFT] ? 1 : 0;
    }
    int sx, sy;
    vcam_stick_to_world(cam, ix, iy, &sx, &sy);
    return arena_input_pack(sx, sy, jump, bomb);
}

int main(int argc, char** argv) {
    int frames_limit = -1;
    uint32_t seed = 0xC0FFEE;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--frames") == 0) frames_limit = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--seed") == 0) seed = (uint32_t)strtoul(argv[i + 1], NULL, 0);
    }
    const int smoke = frames_limit >= 0;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("bmhero arena viewer", 1280, 720,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!win || !ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    ArenaState state;
    arena_init(&state, 0, 2, seed);   /* min 2 players: solo round-ends instantly */

    Pads pads = {0};
    ViewerCam cam;   vcam_init(&cam);
    ViewerClock clk; vclock_init(&clk);
    int cam_target = 0, show_grid = 1, running = 1, frame = 0;
    uint64_t t_prev = SDL_GetTicksNS();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_GAMEPAD_ADDED:   pads_add(&pads, e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: pads_remove(&pads, e.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.key) {
                case SDLK_ESCAPE:       running = 0; break;
                case SDLK_P:            vclock_toggle_pause(&clk); break;
                case SDLK_RIGHTBRACKET: vclock_queue_step(&clk); break;
                case SDLK_LEFTBRACKET:  vclock_cycle_rate(&clk); break;
                case SDLK_TAB:          cam_target = (cam_target + 1) % state.num_players; break;
                case SDLK_F1:           cam.topdown = !cam.topdown; break;
                case SDLK_G:            show_grid = !show_grid; break;
                case SDLK_R:
                    if (e.key.mod & SDL_KMOD_SHIFT)
                        seed = seed * 1664525u + 1013904223u;
                    arena_init(&state, 0, state.num_players, seed);
                    break;
                default: break;
                }
                break;
            default: break;
            }
        }

        /* players = connected devices, floor 2 (hotplug restarts the match) */
        if (!smoke) {
            int devices = 0;
            for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
                if (pads.pad[i]) devices++;
            if (keyboard_player(&pads) >= 0) devices++;
            int want = devices < 2 ? 2 : devices;
            if (want != state.num_players)
                arena_init(&state, 0, (uint8_t)want, seed);
        }

        uint64_t t_now = SDL_GetTicksNS();
        double ms = (double)(t_now - t_prev) / 1e6;
        t_prev = t_now;

        int n = smoke ? 1 : vclock_advance(&clk, ms);
        for (int t = 0; t < n; t++) {
            ArenaInput in[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
            if (!smoke)
                for (int i = 0; i < state.num_players; i++)
                    in[i] = read_input(&pads, i, &cam);
            arena_tick(&state, in);
        }

        const ArenaPlayer* tp = &state.players[cam_target];
        vcam_update(&cam,
                    (Vf3){QF(tp->pos.x), QF(tp->pos.y), QF(tp->pos.z)},
                    VCAM_BINANG_TO_RAD(tp->yaw));

        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        draw_scene(ren, &cam, &state, w, h, show_grid);
        draw_facing(ren, &cam, &state, w, h);
        draw_hud(ren, &state, &clk, &cam, cam_target, w, h);
        SDL_RenderPresent(ren);
        frame++;
        if (frames_limit >= 0 && frame >= frames_limit) running = 0;
    }
    printf("frames %d tick %u hash %08x\n", frame, state.tick, arena_hash(&state));
    SDL_Quit();
    return 0;
}
```

- [ ] **Step 3: Build, smoke-test determinism of the smoke flag**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake --build build && ./build/arena_viewer.exe --frames 300 > run1.txt && ./build/arena_viewer.exe --frames 300 > run2.txt && diff run1.txt run2.txt && cat run1.txt && rm run1.txt run2.txt
```
Expected: `diff` silent (identical), output like `frames 300 tick 300 hash <8 hex digits>` — same hash both runs.

- [ ] **Step 4: Human verification checkpoint (do not skip)**

Ask the user to run `./build/arena_viewer.exe` and confirm: WASD runs the player camera-relative (W moves away from camera); Space jumps with a visible arc + moving shadow; holding Shift grabs a bomb, releasing throws it forward with an arc; bombs bounce once, settle, flash, explode orange; blasts knock players into orange tumble; `P`/`]`/`[` pause/step/slow-mo; `R` restarts; `F1` top-down; camera follows turns smoothly. Collect any feel complaints as future tuning notes — do not tune `src/arena/` in this plan.

- [ ] **Step 5: Commit**

```bash
git add tools/viewer/viewer_main.c tools/viewer/viewer_draw.c tools/viewer/viewer_draw.h
git commit -m "feat(viewer): playable - sim wiring, entities, keyboard, feel keys"
```

---

### Task 7: Gamepad support verification

**Files:**
- Modify: none expected (hotplug code shipped in Task 6); fixes only if verification fails

**Interfaces:**
- Consumes: Task 6's `Pads` handling
- Produces: verified pad play

- [ ] **Step 1: Human verification checkpoint (needs a physical gamepad)**

Ask the user to run the viewer, connect a gamepad (Xbox-style), and confirm: left stick moves the player camera-relative with analog speed (slight tilt = slow walk); A (south) jumps; X (west) grabs/throws; hot-plugging the pad mid-session restarts the match with the pad as P0 and keyboard as P1; unplugging reverts. Two-player: pad drives P0, WASD drives P1, Tab switches the camera between them.

- [ ] **Step 2: Fix anything that fails verification, then commit**

If fixes were needed:
```bash
git add -A tools/viewer
git commit -m "fix(viewer): gamepad handling issues found in device testing"
```
If nothing failed, skip the commit.

---

### Task 8: HUD — embedded font, text, state readouts

**Files:**
- Create: `tools/viewer/viewer_font8.h` (fetched, public domain)
- Modify: `tools/viewer/viewer_draw.c` (replace `draw_text`/`draw_hud` stubs)

**Interfaces:**
- Consumes: `font8x8_basic[128][8]` glyph array; `ViewerClock` fields `paused`/`rate`; `arena_hash`
- Produces: on-screen readouts per spec §4

- [ ] **Step 1: Fetch the public-domain 8×8 font**

Run:
```bash
curl -fsSL https://raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h -o tools/viewer/viewer_font8.h
```
Then open `tools/viewer/viewer_font8.h` and verify the header comment states it is public domain (author Daniel Hepper). The array is `char font8x8_basic[128][8]`. Include it ONLY from `viewer_draw.c` (it defines the array, not just declares it). If the fetch fails (offline), stop and tell the user rather than substituting different font data.

- [ ] **Step 2: Implement draw_text and draw_hud**

In `tools/viewer/viewer_draw.c`, add `#include "viewer_font8.h"` after the other includes, then replace the two stubs with:
```c
void draw_text(SDL_Renderer* r, float x, float y, float scale, const char* str) {
    SDL_SetRenderDrawColor(r, 235, 235, 235, 255);
    SDL_FRect px = {0, 0, scale, scale};
    for (; *str; str++, x += 8 * scale) {
        unsigned ch = (unsigned char)*str;
        if (ch >= 128) continue;
        for (int row = 0; row < 8; row++) {
            unsigned bits = (unsigned char)font8x8_basic[ch][row];
            for (int col = 0; col < 8; col++)
                if (bits & (1u << col)) {
                    px.x = x + (float)col * scale;
                    px.y = y + (float)row * scale;
                    SDL_RenderFillRect(r, &px);
                }
        }
    }
}

void draw_hud(SDL_Renderer* r, const ArenaState* s, const ViewerClock* clk,
              const ViewerCam* cam, int cam_target, int w, int h) {
    static const char* pstate[] = {"IDLE", "RUN ", "JUMP", "TUMB", "DEAD"};
    static const char* phase[]  = {"COUNTDOWN", "PLAY", "SUDDEN-DEATH", "ROUND-END"};
    static const char* rate[]   = {"1x", "1/4x", "1/16x"};
    char line[160];
    (void)w;

    snprintf(line, sizeof line, "TICK %-8u HASH %08x  %s %d  RATE %s%s  CAM P%d %s",
             s->tick, arena_hash(s), phase[s->phase], (int)s->phase_timer,
             rate[clk->rate], clk->paused ? " PAUSED" : "",
             cam_target, cam->topdown ? "TOP" : "CHASE");
    draw_text(r, 8, 8, 2, line);

    for (int i = 0; i < s->num_players; i++) {
        const ArenaPlayer* p = &s->players[i];
        snprintf(line, sizeof line,
                 "P%d %s hp%d pos %+6.2f %+6.2f %+6.2f vel %+5.2f %+5.2f %+5.2f bombs %d t%d",
                 i, pstate[p->state], (int)p->hp,
                 QF(p->pos.x), QF(p->pos.y), QF(p->pos.z),
                 QF(p->vel.x), QF(p->vel.y), QF(p->vel.z),
                 (int)p->live_bombs, (int)p->timer);
        draw_text(r, 8, 8 + 20 * (float)(i + 1), 2, line);
    }

    draw_text(r, 8, (float)h - 24, 2,
              "P pause  ] step  [ rate  R reset  TAB cam  F1 view  G grid  ESC quit");
}
```

- [ ] **Step 3: Build and verify**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake --build build && ./build/arena_viewer.exe --frames 300
```
Expected: HUD visible during the run; prints the same `frames 300 tick 300 hash ...` as Task 6 Step 3 (HUD must not change sim behavior — the hash proves it). Then a quick human check: pause with `P` — TICK and HASH freeze; one `]` advances TICK by exactly 1.

- [ ] **Step 4: Commit**

```bash
git add tools/viewer/viewer_font8.h tools/viewer/viewer_draw.c
git commit -m "feat(viewer): HUD - embedded 8x8 font, tick/hash/player readouts"
```

---

### Task 9: Acceptance — sim untouched, docs updated

**Files:**
- Modify: `README.md` (viewer section), `CLAUDE.md` (status)

**Interfaces:**
- Consumes: everything
- Produces: verified milestone

- [ ] **Step 1: Prove the sim is untouched**

Run:
```bash
git diff 396e5e6 --stat -- src/arena/
```
Expected: empty output. (396e5e6 = the A0 root commit.)

- [ ] **Step 2: Re-run the full test suite**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && ctest --test-dir build --output-on-failure && gcc -std=c11 -Wall -Wextra -O2 -o test_det src/arena/arena_sim.c tests/test_determinism.c && ./test_det && rm test_det.exe
```
Expected: `100% tests passed` and `ALL TESTS PASSED`.

- [ ] **Step 3: Update README**

In `README.md`, under `## Layout`, add:
```
    tools/viewer/             SDL3 debug viewer (dev tool; floats OK here) — chase-cam
                              play over the sim: pause/step/slow-mo, HUD, gamepad+keyboard
```
And under `## Build & test`, add:
```
    # viewer (needs SDL3; see Windows toolchain below):
    cmake -S . -B build -G Ninja && cmake --build build && ./build/arena_viewer
```

- [ ] **Step 4: Update CLAUDE.md status**

In `CLAUDE.md`, replace the `## Current status (2026-07-18)` section body with:
```markdown
**A0 complete.** Headless deterministic arena sim in `src/arena/`, all tests
green (`tests/test_determinism.c`): bit-identical replay, GekkoNet-style
rollback stress, snapshot round-trip, liveness. Scripted-match hash pinned at
`a55aa9b1` (gcc -O0/-O2 verified locally; `.github/workflows/determinism.yml`
extends to clang/MSVC-runner/ARM64 on push).

**SDL debug viewer complete** (`tools/viewer/`, spec
`docs/superpowers/specs/2026-07-18-sdl-debug-viewer-design.md`): chase-cam
(Hero-style, camera-relative input) + top-down toggle, pause/step/slow-mo,
HUD with live state hash, gamepad + keyboard, `--frames N` deterministic
smoke flag. Pure-logic modules unit-tested (`tests/test_viewer_clock.c`,
`tests/test_viewer_cam.c`). Toolchain: MSYS2 UCRT64 (gcc/CMake/Ninja/SDL3).
```
Also update the `## Next milestones` list: remove item 1 (SDL debug viewer), renumber so A2 netcode is item 1 and A1 render bridge is item 2.

- [ ] **Step 5: Final commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs: SDL debug viewer milestone complete"
```

---

## Plan self-review notes

- Spec coverage: goals→Tasks 2–8; camera/input→3+6; rendering→5+6; feel tools→2+6+8; build/toolchain→1+4; testing/acceptance→every task + 9; non-goals respected (no interpolation, no replay, no sim changes).
- The `--frames` smoke flag (forced-neutral inputs, 1 tick/frame) exists so agents can regression-check the viewer without a human; human checkpoints are explicit steps in Tasks 6–8.
- Known accepted limitation: painter sorting is per-drawable (avg depth) — large overlapping faces can sort wrong at grazing angles; irrelevant for this arena's blocky geometry, revisit only if it visibly misdraws.
