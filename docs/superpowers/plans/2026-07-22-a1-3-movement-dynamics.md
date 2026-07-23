# A1.3 Movement Dynamics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the sim's placeholder movement with the campaign player's real ground+air dynamics (top speed, acceleration, **turn rate**, momentum, jump, gravity, air control) transcribed from the decomp — spec `docs/superpowers/specs/2026-07-22-a1-3-movement-dynamics-design.md`.

**Architecture:** Phase 1 RE documents the campaign player's exact update + the world-unit/Hz keystone (decomp-only, findings doc). Phase 2 ports the structure into `player_tick` — the headline change is replacing the instant `p->yaw = dir` snap with a bounded turn-toward-target. Phase 3 transcribes constants through the documented conversion and locks the port with model-consistency tests, then bumps `TUNE_VERSION` and re-pins the CI hash.

**Tech Stack:** C11, Q20.12 fixed-point (`arena_math.h` — `qmul/qsin/qcos/iatan2`, binary-angle u16), gcc one-line build + the determinism suite.

## Global Constraints

- Repo: **`bmhero-arena` only** (standalone). No fork changes (the render bridge already drives the tick + reads back displacement/yaw, so new dynamics appear on screen for free).
- **Hard invariants (breaking any breaks netplay — CLAUDE.md):** (1) NO floats in `src/arena/` — Q20.12 only, int64 intermediates; (2) sim reads nothing outside `ArenaState`, the tick's inputs, `static const`; (3) fixed iteration order (players 0..3) + fixed tick-phase order; (4) `ArenaState` layout change = version bump; (5) padding stays zeroed.
- The decomp's `sinf/cosf/DEG_TO_RAD` → binary-angle `qsin/qcos`; **degrees→binary-angle conversion required** (`bin = deg × 65536 / 360`).
- **The 30→60 Hz rescale is the top transcription hazard:** if the game updates player logic at 30 Hz, a per-tick velocity/accel constant must be halved for the sim's 60 Hz ticks (per-tick delta ÷2; per-tick² term ÷4). Verify the rate in Phase 1; comment the factor on every transcribed constant.
- **[test]** after every sim change:
  ```
  gcc -std=c11 -Wall -Wextra -O2 -o t src/arena/arena_sim.c tests/test_determinism.c && ./t
  ```
  Expected tail: `ALL TESTS PASSED`.
- The pinned CI hash (`.github/workflows/determinism.yml:50`, currently `4b6687d4`) changing is EXPECTED here and must be an intentional `TUNE_VERSION` bump (Task 5 only).
- **Test-responsibility split (state once, honor throughout):** the new movement tests derive their expectations FROM the `TUNE_*` constants — they guard the MODEL (a turn rate exists and converges; accel reaches the cap; gravity/impulse integrate correctly) and catch port/regression bugs. GAME-FIDELITY of the numbers is guarded separately by Phase 1's findings doc + the per-constant conversion comments. Do not conflate the two.

---

### Task 1: Phase 1 — RE the campaign player physics (decomp-only, no build)

**Files:**
- Read-only (fork decomp): `lib/bmhero/src/code/76640.c`, `lib/bmhero/src/boot/2BF00.c`, `lib/bmhero/src/boot/math_util.c` (in the fork `C:\Users\dshi\GitRepos\BMHeroRecomp`)
- Create (this repo): `docs/bmhero-player-movement-re.md`

**Interfaces:**
- Produces: the documented **turn rule**, **speed rule**, **airborne rule**, and **units keystone** (game logic Hz + world-unit→Q20.12 scale anchor) that Tasks 2–5 transcribe. Every downstream `FILL:` references a heading in this doc.

- [ ] **Step 1: Locate the campaign player update.** In the fork decomp, from the anchors `code/76640.c:442-443` (`moveAngle` current vs `Math_CalcAngleRotated(gActiveContStickX,-gActiveContStickY)` target) and `boot/2BF00.c:480` (`Vel.{x,z}={sin,cos}(moveAngle·DEG_TO_RAD)·moveSpeed`), trace the routine that owns `gPlayerObject->moveAngle`/`moveSpeed` per frame for NORMAL gameplay (NOT `Debug_HandleObjMovement`, the free-mover). Note its file/function and its call site in the object-update dispatch.

- [ ] **Step 2: Extract the turn rule.** Does `moveAngle` snap to the target instantly or rotate toward it at a bounded rate? If bounded: the max deg/frame, and whether it scales with speed. Read `Math_CalcAngleRotated` (`boot/math_util.c`) to confirm the stick→target-angle mapping (degrees, sign convention).

- [ ] **Step 3: Extract the speed rule.** How `moveSpeed` moves toward a stick-driven target each frame: accel per frame, top speed, decel/friction when neutral, and whether analog stick magnitude scales the target (the debug mover used `|stick|/2` capped 20 — confirm the campaign rule independently).

- [ ] **Step 4: Extract the airborne rule.** Jump impulse (`Vel.y` on A — the debug path used `33.3`), gravity per frame, terminal velocity, and any reduced steering while airborne.

- [ ] **Step 5: Pin the units keystone.** (a) **Logic Hz** — verify whether the player update runs at 30 or 60 Hz (check the game loop / VI cadence; N64 titles commonly tick logic at 30). (b) **World-unit anchor** — one physical constant shared by game and sim to pin Q-units-per-game-unit (candidates: a tile/grid size, the player collision extent vs the sim's `TUNE_PLAYER_RADIUS`=Q(0.35), or a known bomb-throw distance). State the chosen anchor and the resulting scale factor explicitly.

- [ ] **Step 6: Write `docs/bmhero-player-movement-re.md`** with one heading per rule (`## Turn`, `## Speed`, `## Airborne`, `## Units`), each giving the exact game numbers, source `file:line`, and the converted Q20.12@60Hz value with the conversion shown. Include a "Model-structure delta vs current `player_tick`" note (what the sim must add — at minimum the turn rate).

- [ ] **Step 7: DECISION GATE.** If the campaign update is unreadable or too scattered to transcribe faithfully → STOP, record what WAS established, and fall back to approach B (port only the established structure — at minimum the turn rate — transcribe what maps, document the residual gap). Re-plan with the user before continuing.

- [ ] **Step 8: Commit**
```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add docs/bmhero-player-movement-re.md
git commit -m "docs(A1.3): RE findings - campaign player movement physics + units keystone"
```

---

### Task 2: Turn rate — replace the instant yaw snap

**Files:**
- Modify: `src/arena/arena_sim.c` (`player_tick`, the stick→velocity block ~lines 127-140)
- Modify: `src/arena/arena_tuning.h` (add `TUNE_TURN_RATE`)
- Create: `tests/test_movement.c`

**Interfaces:**
- Consumes: Task 1 `## Turn` (the deg/frame rate). Sim binary-angle: `bin_per_tick = deg_per_frame × 65536/360 × HzFactor`.
- Produces: `p->yaw` now rotates toward the stick target at ≤ `TUNE_TURN_RATE` binary-units/tick (short-arc). Later tasks keep using `p->yaw` for velocity direction + facing.

- [ ] **Step 1: Add the constant** in `arena_tuning.h` under the movement block (value FILL from `## Turn`; the example below is a placeholder rate of ~11.25°/tick = 0x0800):
```c
#define TUNE_TURN_RATE       0x0800     /* binary-angle units/tick; FILL from movement-re.md ## Turn (deg/frame x 65536/360 x HzFactor) */
```

- [ ] **Step 2: Write the failing test** — `tests/test_movement.c`:
```c
/* A1.3 movement model-consistency tests. Expectations derive from TUNE_*
 * (guards the model structure, not game-fidelity — see plan Global Constraints). */
#include <stdio.h>
#include <string.h>
#include "../src/arena/arena_sim.h"
#include "../src/arena/arena_tuning.h"

static int failures = 0;
#define CHECK(c, ...) do { if(!(c)){ failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while(0)

/* Drive one player with a fixed stick until yaw settles; count ticks. */
static int ticks_to_turn(int sx, int sy, uint16_t start_yaw) {
    ArenaState s; arena_init(&s, 0, 1, 1);
    s.phase = PHASE_PLAY;                       /* skip countdown */
    s.players[0].yaw = start_yaw;
    ArenaInput in[ARENA_MAX_PLAYERS];
    for (int i=0;i<ARENA_MAX_PLAYERS;i++) in[i]=arena_input_pack(0,0,0,0,0);
    in[0]=arena_input_pack(sx,sy,0,0,0);
    uint16_t target = iatan2(Q(sx), Q(-sy));
    for (int t=1;t<=600;t++){ arena_tick(&s,in);
        if ((uint16_t)(s.players[0].yaw - target) <= 2 ||
            (uint16_t)(target - s.players[0].yaw) <= 2) return t; }
    return -1;
}

static void test_turn_is_gradual(void) {
    /* 180-degree flip from facing +Z(0x8000) to stick full-up(-Z, yaw 0). */
    int t = ticks_to_turn(0, 31, 0x8000);
    int expect = 0x8000 / TUNE_TURN_RATE;
    CHECK(t > 1, "turn must be gradual, not instant (got %d ticks)", t);
    CHECK(t >= expect-2 && t <= expect+2, "180deg turn took %d ticks, expected ~%d", t, expect);
}

int main(void){
    test_turn_is_gradual();
    if(!failures){ printf("ALL MOVEMENT TESTS PASSED\n"); return 0; }
    printf("%d FAILURE(S)\n", failures); return 1;
}
```

- [ ] **Step 3: Run it — expect FAIL** (current code snaps `p->yaw = dir` instantly, so `t == 1`):
```
gcc -std=c11 -Wall -Wextra -O2 -o tm src/arena/arena_sim.c tests/test_movement.c && ./tm
```
Expected: `FAIL: turn must be gradual ...` then `1 FAILURE(S)`.

- [ ] **Step 4: Implement the turn.** In `player_tick`, replace the direction handling. Currently (~line 134-138):
```c
            uint16_t dir = iatan2(Q(ix), Q(-iy));  /* stick up = -Z (into screen) */
            q32 spd = qmul(TUNE_RUN_SPEED, mag);
            sx = qmul(qsin(dir), spd);
            sz = -qmul(qcos(dir), spd);
            p->yaw = dir;
```
becomes — turn `p->yaw` toward the target by at most `TUNE_TURN_RATE`, then derive velocity from the (possibly still-turning) `p->yaw`:
```c
            uint16_t target = iatan2(Q(ix), Q(-iy));  /* stick up = -Z (into screen) */
            int16_t delta = (int16_t)(target - p->yaw);       /* short-arc signed */
            int16_t step  = (int16_t)TUNE_TURN_RATE;
            if (delta >  step) delta =  step;
            if (delta < -step) delta = -step;
            p->yaw = (uint16_t)(p->yaw + delta);
            q32 spd = qmul(TUNE_RUN_SPEED, mag);
            sx = qmul(qsin(p->yaw), spd);
            sz = -qmul(qcos(p->yaw), spd);
```

- [ ] **Step 5: Run the movement test — expect PASS.** `gcc ... -o tm ... && ./tm` → `ALL MOVEMENT TESTS PASSED`.

- [ ] **Step 6: Run the determinism suite — expect PASS** (structure changed, hash will differ but determinism/rollback/snapshot must still hold): run **[test]** → `ALL TESTS PASSED`.

- [ ] **Step 7: Commit**
```bash
git add src/arena/arena_sim.c src/arena/arena_tuning.h tests/test_movement.c
git commit -m "feat(A1.3): bounded turn rate replaces instant yaw snap (+model test)"
```

---

### Task 3: Speed / acceleration / momentum

**Files:**
- Modify: `src/arena/arena_sim.c` (`player_tick` accel block ~lines 142-147)
- Modify: `src/arena/arena_tuning.h` (`TUNE_RUN_SPEED/ACCEL/FRICTION`)
- Modify: `tests/test_movement.c` (add speed tests)

**Interfaces:**
- Consumes: Task 1 `## Speed`, `## Units`. Keeps the accel-to-target structure if Phase 1 confirms it matches; else restructures per findings (document the deviation inline).

- [ ] **Step 1: Add speed tests** to `tests/test_movement.c` before `main`, and call them from `main`:
```c
/* Full-forward hold: measure steady-state speed and ticks to reach 90% of it. */
static void run_forward(q32* out_top, int* out_ticks90) {
    ArenaState s; arena_init(&s, 0, 1, 1); s.phase = PHASE_PLAY;
    ArenaInput in[ARENA_MAX_PLAYERS];
    for(int i=0;i<ARENA_MAX_PLAYERS;i++) in[i]=arena_input_pack(0,0,0,0,0);
    in[0]=arena_input_pack(0,31,0,0,0);           /* full up = -Z */
    q32 top=0; int t90=-1;
    for(int t=1;t<=240;t++){ arena_tick(&s,in);
        q32 spd=qlen2(s.players[0].vel.x, s.players[0].vel.z);
        if(spd>top) top=spd;
    }
    /* second pass for t90 against measured top */
    arena_init(&s,0,1,1); s.phase=PHASE_PLAY;
    for(int t=1;t<=240;t++){ arena_tick(&s,in);
        q32 spd=qlen2(s.players[0].vel.x, s.players[0].vel.z);
        if(t90<0 && spd>=qmul(top,Q(0.9))){ t90=t; } }
    *out_top=top; *out_ticks90=t90;
}
static void test_top_speed_and_accel(void) {
    q32 top; int t90; run_forward(&top,&t90);
    /* top speed = TUNE_RUN_SPEED at full magnitude (mag clamps to 1.0) */
    q32 err = top>TUNE_RUN_SPEED ? top-TUNE_RUN_SPEED : TUNE_RUN_SPEED-top;
    CHECK(err <= Q(0.003), "top speed %d != TUNE_RUN_SPEED %d", top, TUNE_RUN_SPEED);
    /* linear accel-to-target reaches ~top in about RUN_SPEED/RUN_ACCEL ticks */
    int expect = TUNE_RUN_SPEED / TUNE_RUN_ACCEL;
    CHECK(t90 > 0 && t90 <= expect+3, "reached 90%% speed in %d ticks, expected <=~%d", t90, expect);
}
```
Add `test_top_speed_and_accel();` to `main` before the pass/fail print.

- [ ] **Step 2: Run — expect PASS already** if Phase 1 confirms the current accel-to-target structure matches the game (the test guards the model; constants set in Step 3). If Phase 1 mandates a structural change (e.g. speed-dependent accel), it will FAIL here first — implement the change in Step 3 to pass. Run: `gcc ... -o tm ... && ./tm`.

- [ ] **Step 3: Transcribe the constants** in `arena_tuning.h` (values FILL from `## Speed`+`## Units`; drop the `TODO(feel)` marker on each as sourced). Example structure with the conversion shown in the comment:
```c
/* -- movement -- A1.3: transcribed from docs/bmhero-player-movement-re.md.
 * Conversion: q = game_units_per_frame x (Q_per_gameunit) x (60/gameHz). */
#define TUNE_RUN_SPEED       Q(0.085)   /* FILL: top moveSpeed x scale (per-tick @60Hz) */
#define TUNE_RUN_ACCEL       Q(0.010)   /* FILL: moveSpeed accel/frame x scale x HzFactor */
#define TUNE_RUN_FRICTION    Q(0.008)   /* FILL: neutral decel/frame x scale x HzFactor */
```
If Phase 1 requires restructuring the accel block (`arena_sim.c` ~142-147), do it here with an inline comment citing the findings heading; keep it int64/fixed-point and branch-free where the current code is.

- [ ] **Step 4: Run movement + determinism — expect PASS.** `./tm` → `ALL MOVEMENT TESTS PASSED`; **[test]** → `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**
```bash
git add src/arena/arena_sim.c src/arena/arena_tuning.h tests/test_movement.c
git commit -m "feat(A1.3): transcribe run speed/accel/friction from campaign physics (+tests)"
```

---

### Task 4: Airborne — jump / gravity / air control

**Files:**
- Modify: `src/arena/arena_sim.c` (`player_tick` jump ~150-154; gravity is applied in the integrate phase — confirm its site)
- Modify: `src/arena/arena_tuning.h` (`TUNE_JUMP_IMPULSE/GRAVITY/AIR_CONTROL`)
- Modify: `tests/test_movement.c` (add airborne test)

**Interfaces:**
- Consumes: Task 1 `## Airborne`, `## Units`.

- [ ] **Step 1: Locate the gravity application.** Grep `TUNE_GRAVITY` in `arena_sim.c` to find where `vel.y` is decremented and `pos.y` integrated; the airborne test asserts against that integration.

- [ ] **Step 2: Add the airborne test** to `tests/test_movement.c` and call it from `main`:
```c
/* One jump from rest: measure airtime (ticks from launch to landing) and apex. */
static void test_jump_arc(void) {
    ArenaState s; arena_init(&s,0,1,1); s.phase=PHASE_PLAY;
    ArenaInput in[ARENA_MAX_PLAYERS];
    ArenaInput neutral=arena_input_pack(0,0,0,0,0);
    for(int i=0;i<ARENA_MAX_PLAYERS;i++) in[i]=neutral;
    q32 apex=0; int air=0; int launched=0;
    for(int t=1;t<=240;t++){
        in[0] = (t==1) ? arena_input_pack(0,0,1,0,0) /* jump edge */ : neutral;
        arena_tick(&s, in);
        if(s.players[0].pos.y>0){ launched=1; air++; if(s.players[0].pos.y>apex) apex=s.players[0].pos.y; }
        else if(launched) break;
    }
    /* apex ~ impulse^2/(2*gravity); airtime ~ 2*impulse/gravity (ticks) */
    q32 expect_apex = qdiv(qmul(TUNE_JUMP_IMPULSE,TUNE_JUMP_IMPULSE), qmul(Q(2),TUNE_GRAVITY));
    q32 aerr = apex>expect_apex?apex-expect_apex:expect_apex-apex;
    CHECK(launched, "jump produced no airborne frames");
    CHECK(aerr <= qmul(expect_apex,Q(0.15)), "apex %d vs expected %d (>15%%)", apex, expect_apex);
    int expect_air = (2*TUNE_JUMP_IMPULSE)/TUNE_GRAVITY;
    CHECK(air>=expect_air-4 && air<=expect_air+4, "airtime %d ticks vs ~%d", air, expect_air);
}
```

- [ ] **Step 3: Run — expect PASS against current constants** (model guards); if Phase 1 mandates a gravity/air-steering structural change it fails first, fix in Step 4.

- [ ] **Step 4: Transcribe** `TUNE_JUMP_IMPULSE`, `TUNE_GRAVITY`, `TUNE_AIR_CONTROL` in `arena_tuning.h` (FILL from `## Airborne`+`## Units`, conversion commented; drop `TODO(feel)`). Apply any air-steering structural change in `player_tick` if findings require, citing the heading.

- [ ] **Step 5: Run movement + determinism — expect PASS.** `./tm` and **[test]**.

- [ ] **Step 6: Commit**
```bash
git add src/arena/arena_sim.c src/arena/arena_tuning.h tests/test_movement.c
git commit -m "feat(A1.3): transcribe jump/gravity/air-control from campaign physics (+test)"
```

---

### Task 5: Version bump, re-pin hash, wire CI + docs

**Files:**
- Modify: `src/arena/arena_tuning.h` (`TUNE_VERSION`)
- Modify: `.github/workflows/determinism.yml:50` (new pinned hash)
- Modify: `CLAUDE.md`, `docs/bmhero-player-movement-re.md` (final numbers)

**Interfaces:**
- Consumes: all prior tasks. Produces the new pinned scripted-match hash.

- [ ] **Step 1: Bump the version** in `arena_tuning.h`:
```c
#define TUNE_VERSION         3      /* A1.3: movement dynamics transcribed (was 2) */
```

- [ ] **Step 2: Regenerate the scripted-match hash** exactly as CI computes it:
```bash
cat > /tmp/hash_main.c <<'EOF'
#include <stdio.h>
#include "src/arena/arena_sim.h"
int main(void){ ArenaState s; ArenaInput in[4];
  arena_init(&s,0,4,0xB0BB1E5);
  uint32_t r=0xC0FFEE01;
  for(uint32_t t=0;t<5400;t++){
    for(int i=0;i<4;i++){ r^=r<<13;r^=r>>17;r^=r<<5;
      int sx=(int)(r&63)-32; if(sx<-31)sx=-31;
      int sy=(int)((r>>6)&63)-32; if(sy<-31)sy=-31;
      int set=((t+i*53)%137)==0;
      int bomb=((t+i*37)%(90+i*80))<(30+i*40);
      in[i]=arena_input_pack(sx,sy,((r>>12)&31)==0,bomb,set);}
    arena_tick(&s,in);}
  printf("%08x\n",arena_hash(&s)); return 0;}
EOF
gcc -std=c11 -O2 -I. -o /tmp/hash_check /tmp/hash_main.c src/arena/arena_sim.c
/tmp/hash_check
```
Record the printed hash (call it `<NEWHASH>`).

- [ ] **Step 3: Update the pinned hash** in `.github/workflows/determinism.yml` — replace `test "$h" = "4b6687d4"` with `test "$h" = "<NEWHASH>"`.

- [ ] **Step 4: Wire the movement test into CI.** In `determinism.yml`, after the `test_det` build/run step (~line 24), add:
```yaml
      - name: movement model tests
        shell: bash
        run: |
          ${{ matrix.cc }} -std=c11 -Wall -Wextra -Werror ${{ matrix.opt }} \
            -o test_mv src/arena/arena_sim.c tests/test_movement.c
          ./test_mv
```

- [ ] **Step 5: Full local gate.** Run **[test]** (determinism → `ALL TESTS PASSED`), `./tm` (movement → `ALL MOVEMENT TESTS PASSED`), and re-run the hash generator to confirm it prints `<NEWHASH>` deterministically twice.

- [ ] **Step 6: Docs.** Update `CLAUDE.md`: A1.3 status (movement transcribed, `TUNE_VERSION` 3, new pinned hash `<NEWHASH>`, previous `4b6687d4`), and the milestone line (A1.3 done → next: player anims / A1.2g arena hardening / HUD). Ensure `docs/bmhero-player-movement-re.md` carries the final transcribed numbers.

- [ ] **Step 7: Commit + push**
```bash
git add src/arena/arena_tuning.h .github/workflows/determinism.yml CLAUDE.md docs/bmhero-player-movement-re.md
git commit -m "chore(A1.3): TUNE_VERSION 3, re-pin scripted hash, CI movement tests, docs"
git push
```

---

## Plan self-review notes

- **Spec coverage:** §3 RE + units keystone + decision gate → Task 1; §4 model port (turn rate = headline, float-free binary-angle, invariants) → Tasks 2–4; §5 transcribe via conversion + model tests + version bump + re-pin → Tasks 3/4 (constants+tests) & Task 5 (version/hash/CI); §6 non-goals untouched (no bombs, no fork); §7 build/gate → Global Constraints + [test].
- **Placeholder scan:** the `FILL:` constants (turn rate, speed/accel/friction, jump/gravity/air) are the plan's intentional unknowns — each cites the exact `movement-re.md` heading that supplies it (the A1.2d/A1.2e precedent), with placeholder values that keep the build compiling and the model tests self-consistent until sourced. `<NEWHASH>` is generated in Task 5 Step 2, not guessed. No stray TBDs.
- **Type/name consistency:** `TUNE_TURN_RATE` (binary-angle u16/tick) used identically in tuning + Task 2; `iatan2(Q(ix),Q(-iy))` matches `arena_sim.c:134`; `qlen2/qmul/qdiv/qsin/qcos` per `arena_math.h`; `arena_input_pack(sx,sy,jump,bomb,set)` signature matches `test_determinism.c:37`; `PHASE_PLAY`/`p->yaw`/`p->vel` match existing sim types; the CI hash generator is copied verbatim from `determinism.yml:31-45`.
- **Test-responsibility split** is stated in Global Constraints and honored: movement tests guard the model (expectations derived from `TUNE_*`), findings + conversion comments guard game-fidelity.
