# Bomb Mechanics Correction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the invented charge-tier throw with Hero-verified mechanics — single-arc throw with stick-tilt distance, 4-bomb spread on long hold, set + kick with contact-detonating slides.

**Architecture:** All gameplay changes live in `src/arena/` (fixed-point, fixed iteration order, zero `ArenaState` layout change — `BSTATE_SLIDING` is a new u8 enum value and set/kick is free input bit 14). TDD via a new sim-only test binary. This is the project's first intentional hash change: the pinned scripted-match hash moves and `TUNE_VERSION` bumps to 2. Spec: `docs/superpowers/specs/2026-07-19-bomb-mechanics-correction-design.md`.

**Tech Stack:** C11, gcc (MSYS2 UCRT64), CMake/Ninja/ctest.

## Global Constraints

- **No floats in `src/arena/`** — Q20.12 only, int64 intermediates (repo invariant 1).
- **Fixed iteration orders preserved** — players 0..3, bombs 0..15; new scans use the same orders (invariant 3).
- **Zero `ArenaState` layout change** — static_asserts must not move; padding rules unchanged (invariants 4, 5).
- `TUNE_VERSION` goes 1 → 2 exactly once (Task 2).
- The old pinned hash `a55aa9b1` must survive Task 1 (behavior-neutral input-bit plumbing); Tasks 2+ intentionally change it; Task 4 re-pins.
- Branch: all work on `feature/bomb-mechanics` (created in Task 1).
- **[ucrt64]** = run via PowerShell `$env:MSYSTEM='UCRT64'; C:\msys64\usr\bin\bash.exe -lc '<command>'` from repo root `C:\Users\dshi\GitRepos\bmhero-arena`.
- Repo root for git commands: `C:\Users\dshi\GitRepos\bmhero-arena`.

---

### Task 1: Input bit 14 plumbing (behavior-neutral)

**Files:**
- Modify: `src/arena/arena_state.h:16-37` (input docs + pack/unpack)
- Modify: `tests/test_determinism.c:33` (caller)
- Modify: `tools/viewer/viewer_main.c:54` (caller)
- Modify: `.github/workflows/determinism.yml:41` (caller in inline program)

**Interfaces:**
- Produces: `arena_input_pack(int sx, int sy, int jump, int bomb, int set)` and `static inline int arena_input_set(ArenaInput i)`. Every later task uses the 5-arg form.

- [ ] **Step 1: Create the branch**

```bash
git checkout -b feature/bomb-mechanics
```

- [ ] **Step 2: Add bit 14 to the input word**

In `src/arena/arena_state.h`, update the input comment block and pack/unpack:
```c
/* ---- input: 16 bits per player per tick ----
 * bits 0-5  : stick X, signed 6-bit stored offset-by-32 (1..63, 32 = neutral; 0 unused)
 * bits 6-11 : stick Y, same encoding
 * bit 12    : jump
 * bit 13    : bomb (hold to grab, release to throw; long hold arms the spread)
 * bit 14    : set / kick (edge-triggered)
 * Quantizing analog to 6 bits bounds prediction entropy and packet size. */
```
```c
static inline ArenaInput arena_input_pack(int sx, int sy, int jump, int bomb, int set) {
    if (sx < -AIN_STICK_MAX) sx = -AIN_STICK_MAX;
    if (sx >  AIN_STICK_MAX) sx =  AIN_STICK_MAX;
    if (sy < -AIN_STICK_MAX) sy = -AIN_STICK_MAX;
    if (sy >  AIN_STICK_MAX) sy =  AIN_STICK_MAX;
    return (ArenaInput)(((sx + 32) & 63) | (((sy + 32) & 63) << 6)
                        | ((jump ? 1 : 0) << 12) | ((bomb ? 1 : 0) << 13)
                        | ((set ? 1 : 0) << 14));
}
```
After `arena_input_bomb`, add:
```c
static inline int arena_input_set(ArenaInput i)  { return (i >> 14) & 1; }
```

- [ ] **Step 3: Update the three existing callers with `set = 0`**

`tests/test_determinism.c:33`:
```c
        out[i] = arena_input_pack(sx, sy, jump, bomb, 0);
```
`tools/viewer/viewer_main.c:54`:
```c
    return arena_input_pack(sx, sy, jump, bomb, 0);
```
`.github/workflows/determinism.yml:41` (inside the heredoc):
```c
                in[i]=arena_input_pack(sx,sy,((r>>12)&31)==0,((t+i*37)%90)<30,0);}
```

- [ ] **Step 4: Verify behavior-neutrality — old hash must hold**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O2 -o t src/arena/arena_sim.c tests/test_determinism.c && ./t && rm t.exe
```
Expected: `ALL TESTS PASSED`. Then replicate the CI hash program (same generator, set=0) — save as `hash_main.c` at repo root (it is .gitignored by `/t`? it is NOT — delete it after):
```c
#include <stdio.h>
#include "src/arena/arena_sim.h"
int main(void){ ArenaState s; ArenaInput in[4];
  arena_init(&s,0,4,0xB0BB1E5);
  uint32_t r=0xC0FFEE01;
  for(uint32_t t=0;t<5400;t++){
    for(int i=0;i<4;i++){ r^=r<<13;r^=r>>17;r^=r<<5;
      int sx=(int)(r&63)-32; if(sx<-31)sx=-31;
      int sy=(int)((r>>6)&63)-32; if(sy<-31)sy=-31;
      in[i]=arena_input_pack(sx,sy,((r>>12)&31)==0,((t+i*37)%90)<30,0);}
    arena_tick(&s,in);}
  printf("%08x\n",arena_hash(&s)); return 0;}
```
Run **[ucrt64]**:
```
gcc -std=c11 -O2 -I. -o hc hash_main.c src/arena/arena_sim.c && ./hc && rm hc.exe hash_main.c
```
Expected output: `a55aa9b1` (unchanged — bit 14 is ignored by the sim so far).

- [ ] **Step 5: Commit**

```bash
git add src/arena/arena_state.h tests/test_determinism.c tools/viewer/viewer_main.c .github/workflows/determinism.yml
git commit -m "feat(arena): input bit 14 (set/kick) plumbing - behavior-neutral, hash a55aa9b1 holds"
```

---

### Task 2: Single-arc throw with tilt distance + 4-bomb spread (TDD)

**Files:**
- Modify: `src/arena/arena_tuning.h` (bomb constants block + TUNE_VERSION)
- Modify: `src/arena/arena_sim.c:142-172` (throw block) + new `throw_bomb` helper after `detonate`
- Modify: `src/arena/arena_state.h:50` (timer field comment only)
- Create: `tests/test_bomb_mechanics.c`
- Modify: `CMakeLists.txt` (register test)

**Interfaces:**
- Consumes: 5-arg `arena_input_pack` (Task 1).
- Produces: `static void throw_bomb(ArenaState* s, int pi, ArenaBomb* b, uint16_t dir, q32 fwd, q32 up)` in `arena_sim.c` (file-local); tuning names `TUNE_THROW_SPEED`, `TUNE_THROW_UP`, `TUNE_THROW_MIN_FRAC`, `TUNE_SPREAD_TICKS`, `TUNE_SPREAD_SPEED`, `TUNE_SPREAD_UP`, `TUNE_KICK_SPEED`, `TUNE_KICK_RANGE`, `TUNE_KICK_CONE`, `TUNE_MAX_LIVE_BOMBS`(=6). Test harness helpers `start2/start4/run` reused by Task 3.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_bomb_mechanics.c`:
```c
/* Deterministic behavior tests for Hero-authentic bomb mechanics.
 * Drives arena_tick with scripted inputs; sim-only, no floats. */
#include <stdio.h>
#include <string.h>
#include "../src/arena/arena_sim.h"
#include "../src/arena/arena_tuning.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define NEUTRAL arena_input_pack(0, 0, 0, 0, 0)

/* run n ticks: player 0 gets in0, everyone else neutral */
static void run(ArenaState* s, ArenaInput in0, int n) {
    ArenaInput in[ARENA_MAX_PLAYERS] = { in0, NEUTRAL, NEUTRAL, NEUTRAL };
    for (int t = 0; t < n; t++) arena_tick(s, in);
}

/* fresh match with countdown skipped (play phase active) */
static void start2(ArenaState* s) {
    arena_init(s, 0, 2, 0xBEEF);
    run(s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);
}
static void start4(ArenaState* s) {
    arena_init(s, 0, 4, 0xBEEF);
    run(s, NEUTRAL, TUNE_COUNTDOWN_TICKS + 1);
}

static q32 bomb_xz_dist(const ArenaBomb* b, Vec3q from) {
    return qlen2(b->pos.x - from.x, b->pos.z - from.z);
}

static void test_throw_tilt(void) {
    /* Both throws aim +X from spawn (-4.5,-4.5): ~10 clear units along
     * z=-4.5 (walls at +/-6, pillars only within |z|<=2.5), so neither
     * trajectory clips geometry and distances compare cleanly. */
    ArenaState s;
    /* neutral-stick release = short lob */
    start2(&s);
    Vec3q origin = s.players[0].pos;
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 5);      /* grab */
    run(&s, arena_input_pack(31, 0, 0, 1, 0), 3);     /* face +X while holding */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 2);      /* stick back to neutral
                                                         (deadzone keeps yaw) */
    run(&s, NEUTRAL, 120);                            /* release neutral: lob */
    CHECK(s.bombs[0].state == BSTATE_SETTLED, "lob settled");
    q32 lob = bomb_xz_dist(&s.bombs[0], origin);

    /* full-tilt release = farther throw */
    start2(&s);
    origin = s.players[0].pos;
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 5);      /* grab */
    run(&s, arena_input_pack(31, 0, 0, 1, 0), 3);     /* face +X, full tilt */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);     /* release at full tilt */
    run(&s, NEUTRAL, 120);
    CHECK(s.bombs[0].state == BSTATE_SETTLED, "throw settled");
    q32 thrown = bomb_xz_dist(&s.bombs[0], origin);
    CHECK(thrown > lob + Q(0.5),
          "full tilt lands farther than neutral lob (thrown=%d lob=%d)", thrown, lob);
}

static void test_spread(void) {
    ArenaState s;
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 1, 0), TUNE_SPREAD_TICKS + 5); /* arm */
    run(&s, NEUTRAL, 1);                                             /* release */
    int airborne = 0;
    q32 seen_vx[4]; int n_vx = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_AIRBORNE) {
            if (n_vx < 4) seen_vx[n_vx++] = s.bombs[i].vel.x;
            airborne++;
        }
    CHECK(airborne == 4, "spread throws 4 bombs (got %d)", airborne);
    CHECK(s.players[0].live_bombs == 4, "live_bombs == 4 (got %d)",
          s.players[0].live_bombs);
    int distinct = 1;
    for (int a = 0; a < n_vx && distinct; a++)
        for (int b = a + 1; b < n_vx; b++)
            if (seen_vx[a] == seen_vx[b]) distinct = 0;
    CHECK(distinct, "fan headings are distinct (vel.x all differ)");
}

static void test_spread_cap(void) {
    ArenaState s;
    start2(&s);
    /* first spread: 4 live bombs on the floor */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), TUNE_SPREAD_TICKS + 5);
    run(&s, NEUTRAL, 1);
    /* second spread armed while 4 still live: cap 6 clamps it to 2 */
    run(&s, arena_input_pack(0, 0, 0, 1, 0), TUNE_SPREAD_TICKS + 5);
    run(&s, NEUTRAL, 1);
    CHECK(s.players[0].live_bombs == TUNE_MAX_LIVE_BOMBS,
          "cap reached exactly (live=%d)", s.players[0].live_bombs);
    int airborne = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_AIRBORNE) airborne++;
    CHECK(airborne == 2, "second spread clamped to 2 bombs (got %d)", airborne);
}

/* Task 3 appends set/kick tests here */

int main(void) {
    test_throw_tilt();
    test_spread();
    test_spread_cap();
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("bomb_mechanics: ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify failure**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O2 -o t_bomb tests/test_bomb_mechanics.c src/arena/arena_sim.c && ./t_bomb; rm -f t_bomb.exe
```
Expected: compile FAILS — `TUNE_SPREAD_TICKS` undeclared (constants don't exist yet).

- [ ] **Step 3: Rewrite the tuning block**

In `src/arena/arena_tuning.h`, replace the `-- bombs --` block:
```c
/* -- bombs -- TODO(feel): verify every value in A1 (throw arc from decomp;
 * kick-vs-wall detonation is owner-recalled, confirm in the recomp) */
#define TUNE_THROW_SPEED     Q(0.18)    /* single throw, full stick tilt */
#define TUNE_THROW_UP        Q(0.12)
#define TUNE_THROW_MIN_FRAC  Q(0.35)    /* neutral-stick lob = this fraction */
#define TUNE_SPREAD_TICKS    120        /* hold >= this arms the 4-bomb spread */
#define TUNE_SPREAD_SPEED    Q(0.11)    /* spread: fixed shorter trajectory */
#define TUNE_SPREAD_UP       Q(0.10)
#define TUNE_KICK_SPEED      Q(0.14)
#define TUNE_KICK_RANGE      Q(0.9)     /* settled bomb within this can be kicked */
#define TUNE_KICK_CONE       0x2000     /* +/-45 deg of facing, binary angle */
#define TUNE_BOMB_RADIUS     Q(0.30)
#define TUNE_BOMB_RESTITUTION Q(0.40)   /* single bounce */
#define TUNE_BOMB_H_DAMP     Q(0.55)    /* horizontal damping on bounce */
#define TUNE_FUSE_TICKS      150        /* settled -> boom */
#define TUNE_MAX_LIVE_BOMBS  6          /* raised from 2: spread stays reliable */
```
(`TUNE_THROW_SPEED_1/2`, `TUNE_THROW_UP_1/2`, `TUNE_CHARGE_TICKS` are deleted.)
At the bottom, bump:
```c
/* Bump when any value changes; folded into the session version hash. */
#define TUNE_VERSION         2
```

- [ ] **Step 4: Implement throw_bomb + the new throw block**

In `src/arena/arena_sim.c`, after `detonate()` (line ~101), add:
```c
/* Launch bomb b from player pi's hands along binary-angle dir. */
static void throw_bomb(ArenaState* s, int pi, ArenaBomb* b, uint16_t dir,
                       q32 fwd, q32 up) {
    const ArenaPlayer* p = &s->players[pi];
    b->pos = p->pos; b->pos.y += TUNE_PLAYER_HEIGHT;
    b->vel.x =  qmul(qsin(dir), fwd) + p->vel.x;
    b->vel.z = -qmul(qcos(dir), fwd) + p->vel.z;
    b->vel.y = up;
    b->state   = BSTATE_AIRBORNE;
    b->bounced = 0;
}
```
Replace the whole `/* bomb grab / charge / throw */` block in `player_tick` with:
```c
    /* bomb grab / hold / throw — single arc, or 4-bomb spread on long hold */
    if (gameplay && p->state != PSTATE_TUMBLE) {
        int bomb_now = arena_input_bomb(in), bomb_prev = arena_input_bomb(prev);
        if (bomb_now && !bomb_prev && p->held_bomb == 0
            && p->live_bombs < TUNE_MAX_LIVE_BOMBS) {
            int bi = find_free_bomb(s);
            if (bi >= 0) {
                ArenaBomb* b = &s->bombs[bi];
                memset(b, 0, sizeof(*b));
                b->owner = (uint8_t)pi;
                b->state = BSTATE_HELD;
                p->held_bomb = (uint8_t)(bi + 1);
                p->live_bombs++;
                p->timer = 0;                       /* hold counter */
            }
        } else if (bomb_now && p->held_bomb) {
            if (p->timer < 0xFFFF) p->timer++;      /* holding (spread arms) */
        } else if (!bomb_now && bomb_prev && p->held_bomb) {
            ArenaBomb* held = &s->bombs[p->held_bomb - 1];
            if (p->timer < TUNE_SPREAD_TICKS) {
                /* single arc: distance scales with stick tilt at release */
                int ix = arena_input_sx(in), iy = arena_input_sy(in);
                q32 tilt = qlen2(Q(ix) / AIN_STICK_MAX, Q(iy) / AIN_STICK_MAX);
                if (tilt > Q_ONE) tilt = Q_ONE;
                if (tilt < TUNE_THROW_MIN_FRAC) tilt = TUNE_THROW_MIN_FRAC;
                throw_bomb(s, pi, held, p->yaw,
                           qmul(TUNE_THROW_SPEED, tilt), TUNE_THROW_UP);
            } else {
                /* spread: forward fan, fixed short trajectory. Offsets in
                 * fixed order so a clamped spread stays centered. */
                static const int16_t fan[4] = { 0x038E, -0x038E, 0x0AAA, -0x0AAA };
                throw_bomb(s, pi, held, (uint16_t)(p->yaw + (uint16_t)fan[0]),
                           TUNE_SPREAD_SPEED, TUNE_SPREAD_UP);
                for (int k = 1; k < 4; k++) {
                    if (p->live_bombs >= TUNE_MAX_LIVE_BOMBS) break;
                    int bi = find_free_bomb(s);
                    if (bi < 0) break;
                    ArenaBomb* nb = &s->bombs[bi];
                    memset(nb, 0, sizeof(*nb));
                    nb->owner = (uint8_t)pi;
                    p->live_bombs++;
                    throw_bomb(s, pi, nb, (uint16_t)(p->yaw + (uint16_t)fan[k]),
                               TUNE_SPREAD_SPEED, TUNE_SPREAD_UP);
                }
            }
            p->held_bomb = 0;
            p->timer = 0;
        }
    }
```
In `src/arena/arena_state.h:50`, update the field comment only:
```c
    uint16_t timer;              /* invuln / tumble / bomb-hold, per state */
```

- [ ] **Step 5: Run tests to verify pass**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -Wconversion -Wno-sign-conversion -O2 -o t_bomb tests/test_bomb_mechanics.c src/arena/arena_sim.c && ./t_bomb && rm t_bomb.exe
```
Expected: `bomb_mechanics: ALL TESTS PASSED`. Also re-run the determinism suite (hash-agnostic gates must still pass):
```
gcc -std=c11 -Wall -Wextra -O2 -o t src/arena/arena_sim.c tests/test_determinism.c && ./t && rm t.exe
```
Expected: `ALL TESTS PASSED`.

- [ ] **Step 6: Register with CMake**

Append to `CMakeLists.txt` after the `test_determinism` block:
```cmake
add_executable(test_bomb_mechanics tests/test_bomb_mechanics.c)
target_link_libraries(test_bomb_mechanics arena_sim)
if(NOT MSVC)
  target_compile_options(test_bomb_mechanics PRIVATE -Wconversion -Wno-sign-conversion)
endif()
add_test(NAME bomb_mechanics COMMAND test_bomb_mechanics)
```
Run **[ucrt64]**: `cd /c/Users/dshi/GitRepos/bmhero-arena && cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed out of 4`.

- [ ] **Step 7: Commit**

```bash
git add src/arena/arena_tuning.h src/arena/arena_sim.c src/arena/arena_state.h tests/test_bomb_mechanics.c CMakeLists.txt
git commit -m "feat(arena)!: Hero-authentic throw - single arc w/ tilt distance, 4-bomb spread

Intentional gameplay change: replaces invented charge tiers (verified vs
GameFAQs/StrategyWiki). TUNE_VERSION 1->2, cap 2->6. Pinned hash re-set in
a later commit."
```

---

### Task 3: Set + kick + BSTATE_SLIDING (TDD)

**Files:**
- Modify: `src/arena/arena_state.h:41` (enum)
- Modify: `src/arena/arena_sim.c` (player-phase set/kick block; bomb-phase SLIDING case; blast chain line)
- Test: `tests/test_bomb_mechanics.c` (append tests)

**Interfaces:**
- Consumes: Task 2 harness (`start2/start4/run/NEUTRAL`), `arena_input_set`.
- Produces: `BSTATE_SLIDING` enum value; `bounced` field doubles as kicker-grace (kicker index + 1) while SLIDING.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_bomb_mechanics.c` (before `main`), and add the four calls to `main` before the failures check:
```c
static void test_set_and_kick_wall(void) {
    ArenaState s;
    start2(&s);
    /* set: bit-14 edge with nothing nearby places a bomb at the feet */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);
    run(&s, NEUTRAL, 1);
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SETTLED) { bi = i; break; }
    CHECK(bi >= 0, "set places a settled bomb");
    CHECK(s.players[0].live_bombs == 1, "set counts toward the cap");
    CHECK(bomb_xz_dist(&s.bombs[bi], s.players[0].pos) < Q(0.2),
          "bomb set at the feet");

    /* face -Z (wall 1.5 units away at spawn), then kick */
    run(&s, arena_input_pack(0, -31, 0, 0, 0), 1);    /* turn */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* kick press */
    CHECK(s.bombs[bi].state == BSTATE_SLIDING, "second set-press kicks");
    /* slides into the -Z boundary wall and detonates on contact */
    run(&s, NEUTRAL, 40);
    CHECK(s.bombs[bi].state == BSTATE_FREE, "kicked bomb detonated at wall");
    CHECK(s.players[0].live_bombs == 0, "live count back to 0");
}

static void test_kick_hits_player(void) {
    ArenaState s;
    start4(&s);                       /* P3 spawns at (+4.5, -4.5) */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* set */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);     /* face +X toward P3 */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* kick */
    int bi = -1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SLIDING) { bi = i; break; }
    CHECK(bi >= 0, "bomb sliding toward P3");
    run(&s, NEUTRAL, 80);             /* 9 units at TUNE_KICK_SPEED ~= 65 ticks */
    CHECK(s.bombs[bi].state == BSTATE_FREE, "detonated on player contact");
    CHECK(s.players[3].hp < TUNE_START_HP, "P3 took blast damage");
}

static void test_fuse_pops_mid_slide(void) {
    ArenaState s;
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* set */
    run(&s, NEUTRAL, TUNE_FUSE_TICKS - 10);           /* burn fuse to ~10 */
    run(&s, arena_input_pack(31, 0, 0, 0, 0), 1);     /* face +X (10 units of room) */
    run(&s, arena_input_pack(0, 0, 0, 0, 1), 1);      /* kick */
    run(&s, NEUTRAL, 20);
    /* far from any obstacle after ~2.8 units: only the fuse can have popped */
    int gone = 1;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SLIDING) gone = 0;
    CHECK(gone, "fuse detonates a sliding bomb");
}

static void test_set_ignored_while_holding(void) {
    ArenaState s;
    start2(&s);
    run(&s, arena_input_pack(0, 0, 0, 1, 0), 3);      /* grab + hold */
    run(&s, arena_input_pack(0, 0, 0, 1, 1), 1);      /* set press while holding */
    int settled = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s.bombs[i].state == BSTATE_SETTLED) settled++;
    CHECK(settled == 0 && s.players[0].live_bombs == 1,
          "set is ignored while holding a bomb");
}
```
`main` additions:
```c
    test_set_and_kick_wall();
    test_kick_hits_player();
    test_fuse_pops_mid_slide();
    test_set_ignored_while_holding();
```

- [ ] **Step 2: Run to verify failure**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O2 -o t_bomb tests/test_bomb_mechanics.c src/arena/arena_sim.c && ./t_bomb; rm -f t_bomb.exe
```
Expected: compile FAILS — `BSTATE_SLIDING` undeclared.

- [ ] **Step 3: Implement**

`src/arena/arena_state.h:41` — extend the enum (append only; values of existing states must not change):
```c
enum { BSTATE_FREE, BSTATE_HELD, BSTATE_AIRBORNE, BSTATE_SETTLED, BSTATE_EXPLODING,
       BSTATE_SLIDING };
```
`src/arena/arena_sim.c` — in `player_tick`, immediately after the bomb grab/hold/throw block, add:
```c
    /* set / kick (edge on bit 14): kick a settled bomb in front, else set.
     * Kick wins; one press = one action. Ground-only, not while holding. */
    if (gameplay && p->state != PSTATE_TUMBLE && p->held_bomb == 0
        && on_ground(p) && arena_input_set(in) && !arena_input_set(prev)) {
        int kicked = 0;
        for (int bi = 0; bi < ARENA_MAX_BOMBS; bi++) {     /* fixed order */
            ArenaBomb* b = &s->bombs[bi];
            if (b->state != BSTATE_SETTLED) continue;
            q32 dx = b->pos.x - p->pos.x, dz = b->pos.z - p->pos.z;
            q32 dist = qlen2(dx, dz);
            if (dist > TUNE_KICK_RANGE) continue;
            if (dist >= TUNE_PLAYER_RADIUS) {   /* cone check unless underfoot */
                int16_t rel = (int16_t)(uint16_t)(iatan2(dx, -dz) - p->yaw);
                if (rel > TUNE_KICK_CONE || rel < -TUNE_KICK_CONE) continue;
            }
            b->state   = BSTATE_SLIDING;
            b->pos.y   = 0;
            b->vel.x   =  qmul(qsin(p->yaw), TUNE_KICK_SPEED);
            b->vel.y   = 0;
            b->vel.z   = -qmul(qcos(p->yaw), TUNE_KICK_SPEED);
            b->bounced = (uint8_t)(pi + 1);     /* kicker grace, cleared on exit */
            kicked = 1;
            break;
        }
        if (!kicked && p->live_bombs < TUNE_MAX_LIVE_BOMBS) {
            int bi = find_free_bomb(s);
            if (bi >= 0) {
                ArenaBomb* b = &s->bombs[bi];
                memset(b, 0, sizeof(*b));
                b->owner = (uint8_t)pi;
                b->state = BSTATE_SETTLED;
                b->fuse  = TUNE_FUSE_TICKS;
                b->pos   = p->pos; b->pos.y = 0;
                p->live_bombs++;
            }
        }
    }
```
In the bomb-phase `switch` (before `case BSTATE_SETTLED:`), add:
```c
        case BSTATE_SLIDING: {
            b->pos.x += b->vel.x; b->pos.z += b->vel.z;
            /* players: contact detonates (kicker skipped until clear) */
            for (int pj = 0; pj < s->num_players; pj++) {
                const ArenaPlayer* p = &s->players[pj];
                if (p->state == PSTATE_DEAD) continue;
                if (p->pos.y > 2 * TUNE_BOMB_RADIUS) continue;   /* jumped over */
                q32 dist = qlen2(b->pos.x - p->pos.x, b->pos.z - p->pos.z);
                q32 touch = TUNE_PLAYER_RADIUS + TUNE_BOMB_RADIUS;
                if (b->bounced == (uint8_t)(pj + 1)) {           /* kicker grace */
                    if (dist >= touch + Q(0.1)) b->bounced = 0;
                    continue;
                }
                if (dist < touch) { b->state = BSTATE_EXPLODING; break; }
            }
            if (b->state != BSTATE_SLIDING) break;
            /* other live ground bombs: contact detonates (chain) */
            for (int bj = 0; bj < ARENA_MAX_BOMBS; bj++) {
                const ArenaBomb* ob = &s->bombs[bj];
                if (bj == i) continue;
                if (ob->state != BSTATE_SETTLED && ob->state != BSTATE_SLIDING)
                    continue;
                if (qlen2(b->pos.x - ob->pos.x, b->pos.z - ob->pos.z)
                    < 2 * TUNE_BOMB_RADIUS) {
                    b->state = BSTATE_EXPLODING;
                    break;
                }
            }
            if (b->state != BSTATE_SLIDING) break;
            /* walls / pillars: any pushback = contact = detonate */
            {
                Vec3q pre_p = b->pos, pre_v = b->vel;
                collide_static(&b->pos, &b->vel, TUNE_BOMB_RADIUS, g, wall_extent);
                if (b->pos.x != pre_p.x || b->pos.z != pre_p.z
                    || b->vel.x != pre_v.x || b->vel.z != pre_v.z)
                    b->state = BSTATE_EXPLODING;
            }
            /* fuse keeps burning while sliding */
            if (b->state == BSTATE_SLIDING) {
                if (b->fuse > 0) b->fuse--;
                if (b->fuse == 0) b->state = BSTATE_EXPLODING;
            }
            break;
        }
```
And extend the blast chain condition (currently `SETTLED/AIRBORNE`, line ~314):
```c
            if (b->state != BSTATE_SETTLED && b->state != BSTATE_AIRBORNE
                && b->state != BSTATE_SLIDING) continue;
```

- [ ] **Step 4: Run tests to verify pass**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -Wconversion -Wno-sign-conversion -O2 -o t_bomb tests/test_bomb_mechanics.c src/arena/arena_sim.c && ./t_bomb && rm t_bomb.exe && gcc -std=c11 -Wall -Wextra -O2 -o t src/arena/arena_sim.c tests/test_determinism.c && ./t && rm t.exe
```
Expected: `bomb_mechanics: ALL TESTS PASSED` and `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add src/arena/arena_state.h src/arena/arena_sim.c tests/test_bomb_mechanics.c
git commit -m "feat(arena)!: set + kick with contact-detonating slides (BSTATE_SLIDING)

Kick-vs-wall detonation is owner-recalled, marked TODO(feel) for A1
verification. Kicker grace via bounced field; no ArenaState layout change."
```

---

### Task 4: Determinism coverage + hash re-pin

**Files:**
- Modify: `tests/test_determinism.c:32-33` (set bits in the input script)
- Modify: `.github/workflows/determinism.yml:41,48` (script + pinned hash)

**Interfaces:**
- Consumes: everything; Produces: the new pinned hash `<NEWHASH>` used by CI and docs (Task 6).

- [ ] **Step 1: Add set presses to the scripted match**

`tests/test_determinism.c` — replace lines 32–33 with:
```c
        int bomb = ((tick + i * 37) % 90) < 30;         /* held 0.5s of every 1.5s */
        int set  = ((tick + i * 53) % 137) == 0;        /* sparse set/kick presses */
        out[i] = arena_input_pack(sx, sy, jump, bomb, set);
```
`.github/workflows/determinism.yml` — the inline program's input line becomes:
```c
                int set=((t+i*53)%137)==0;
                in[i]=arena_input_pack(sx,sy,((r>>12)&31)==0,((t+i*37)%90)<30,set);}
```

- [ ] **Step 2: Run the suite at -O0 and -O2 (both must pass)**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && gcc -std=c11 -Wall -Wextra -O0 -o t0 src/arena/arena_sim.c tests/test_determinism.c && ./t0 && gcc -std=c11 -Wall -Wextra -O2 -o t2 src/arena/arena_sim.c tests/test_determinism.c && ./t2 && rm t0.exe t2.exe
```
Expected: `ALL TESTS PASSED` twice.

- [ ] **Step 3: Compute the new pinned hash at both opt levels**

Recreate `hash_main.c` at repo root exactly as in Task 1 Step 4 but with the Task-4 input lines (set expression included). Run **[ucrt64]**:
```
gcc -std=c11 -O0 -I. -o h0 hash_main.c src/arena/arena_sim.c && gcc -std=c11 -O2 -I. -o h2 hash_main.c src/arena/arena_sim.c && ./h0 && ./h2 && rm h0.exe h2.exe hash_main.c
```
Expected: two identical 8-hex-digit lines. Record the value as `<NEWHASH>`.
If they differ, STOP — that is a determinism bug in the new code; return to Task 3 with superpowers:systematic-debugging.

- [ ] **Step 4: Pin it in CI**

`.github/workflows/determinism.yml:48`:
```
          test "$h" = "<NEWHASH>"
```
(substitute the recorded value.)

- [ ] **Step 5: Commit**

```bash
git add tests/test_determinism.c .github/workflows/determinism.yml
git commit -m "test(arena): exercise set/kick in scripted match; re-pin hash to <NEWHASH>

Intentional gameplay change (Hero-authentic bomb mechanics), TUNE_VERSION 2."
```

---

### Task 5: Viewer set/kick binding

**Files:**
- Modify: `tools/viewer/viewer_main.c` (read_input)
- Modify: `tools/viewer/viewer_draw.c` (help line)

**Interfaces:**
- Consumes: 5-arg pack. Keyboard `E`, pad EAST → set.

- [ ] **Step 1: Wire the button**

In `tools/viewer/viewer_main.c` `read_input`, change the declarations and both device branches:
```c
    float ix = 0, iy = 0;
    int jump = 0, bomb = 0, set = 0;
    if (pads->pad[player]) {
        SDL_Gamepad* gp = pads->pad[player];
        ix =  (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        iy = -(float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        jump = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH);
        bomb = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST);
        set  = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST);
    } else if (player == keyboard_player(pads)) {
        const bool* k = SDL_GetKeyboardState(NULL);
        ix = (float)((k[SDL_SCANCODE_D] ? 1 : 0) - (k[SDL_SCANCODE_A] ? 1 : 0));
        iy = (float)((k[SDL_SCANCODE_W] ? 1 : 0) - (k[SDL_SCANCODE_S] ? 1 : 0));
        jump = k[SDL_SCANCODE_SPACE] ? 1 : 0;
        bomb = k[SDL_SCANCODE_LSHIFT] ? 1 : 0;
        set  = k[SDL_SCANCODE_E] ? 1 : 0;
    }
    int sx, sy;
    vcam_stick_to_world(cam, ix, iy, &sx, &sy);
    return arena_input_pack(sx, sy, jump, bomb, set);
```
In `tools/viewer/viewer_draw.c`, the help line becomes (must stay ≤ ~79
chars: 2x-scaled 8px glyphs in a 1280px window):
```c
    draw_text(r, 8, (float)h - 24, 2,
              "SHIFT throw(hold=spread) E set/kick P pause ] step [ rate R reset F1 cam");
```

- [ ] **Step 2: Build, smoke, verify determinism of smoke**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && cmake --build build && ctest --test-dir build --output-on-failure && ./build/arena_viewer.exe --frames 300 > r1.txt && ./build/arena_viewer.exe --frames 300 > r2.txt && if [ "$(cat r1.txt)" = "$(cat r2.txt)" ]; then echo IDENTICAL; cat r1.txt; else echo MISMATCH; fi; rm -f r1.txt r2.txt
```
Expected: `100% tests passed out of 4`, `IDENTICAL`, and a `frames 300 tick 300 hash ...` line (value may differ from the old `eeeb76f6` — record it in the commit).

- [ ] **Step 3: Commit**

```bash
git add tools/viewer/viewer_main.c tools/viewer/viewer_draw.c
git commit -m "feat(viewer): E / pad-EAST set+kick binding; help line updated"
```

---

### Task 6: Docs, acceptance, PR

**Files:**
- Modify: `docs/bmhero-battle-arena-design.md` (§2 bombs bullet, §3 timer comment, §5 constants note)
- Modify: `CLAUDE.md` (status, hash mentions, milestones)
- Delete: memory file `hero-bomb-mechanics-verified.md` + its `MEMORY.md` line (repo now records the mechanics)

- [ ] **Step 1: Rewrite design doc §2 bombs bullet**

Replace the `- **Bombs:** hold to grab...` bullet with:
```markdown
- **Bombs:** press B to pull one out (can run/jump while holding); release =
  single-arc throw in facing direction, distance scaling with stick tilt at
  release (neutral stick = short lob). Hold ≥ ~2s to arm the **4-bomb
  spread** — a forward fan (±5°, ±15°) at a fixed shorter trajectory. A
  separate **set** button lays a bomb at the feet; pressing it with a
  settled bomb in front (any owner) **kicks** it — kicked bombs slide flat
  and **detonate on first contact** (wall, pillar, player, bomb) or on fuse
  expiry *(kick-vs-wall detonation owner-recalled; verify in A1)*. Thrown
  bombs bounce once, then sit with a fuse (~150 ticks); direct hit on a
  player detonates on impact. Max 6 live bombs per player. *(Mechanics
  verified 2026-07-19 vs GameFAQs/StrategyWiki/Bomberman Wiki — replaces
  the earlier invented charge-tier throw.)*
```
In §3's `ArenaPlayer` snippet comment, change `charge / tumble+invuln` to `bomb-hold / tumble+invuln`. In §5, extend the constants sentence to mention "spread fan angles/arm time and kick speed/range" among the values to transcribe or fit empirically.

- [ ] **Step 2: Update CLAUDE.md**

- Replace both `a55aa9b1` mentions with `<NEWHASH>` and note: "re-pinned 2026-07-19 with the bomb-mechanics correction (TUNE_VERSION 2; first intentional gameplay change)".
- Delete the "**Pending design correction**" paragraph.
- In "Next milestones", remove item 1 (bomb-mechanics correction) and renumber so A2 GekkoNet is 1 and A1 render bridge is 2.

- [ ] **Step 3: Retire the memory file**

Delete `C:\Users\dshi\.claude\projects\C--Users-dshi-GitRepos-bmhero-arena\memory\hero-bomb-mechanics-verified.md` and remove its line from `MEMORY.md` in the same directory (the repo's design doc now records the mechanics).

- [ ] **Step 4: Full acceptance run**

Run **[ucrt64]**:
```
cd /c/Users/dshi/GitRepos/bmhero-arena && ctest --test-dir build --output-on-failure && gcc -std=c11 -Wall -Wextra -O0 -o t src/arena/arena_sim.c tests/test_determinism.c && ./t && rm t.exe
```
Expected: `100% tests passed out of 4` and `ALL TESTS PASSED`.

- [ ] **Step 5: Human checkpoint — playtest the new mechanics**

Ask the user to run `./build/arena_viewer.exe` and verify: Shift-release throws one bomb whose distance follows stick tilt; holding Shift ~2s then releasing fans 4 bombs; `E` sets a bomb; `E` again kicks it (slides, pops on the wall); kicking toward the dummy player detonates on them. Collect feel notes as tuning follow-ups.

- [ ] **Step 6: Commit, push, PR**

```bash
git add docs/bmhero-battle-arena-design.md CLAUDE.md
git commit -m "docs: record Hero-authentic bomb mechanics (design s2), new pinned hash"
git push -u origin feature/bomb-mechanics
gh pr create --base main --head feature/bomb-mechanics --title "Hero-authentic bomb mechanics: single-arc throw, 4-bomb spread, set + kick" --body "First intentional sim change. TUNE_VERSION 2, pinned hash a55aa9b1 -> <NEWHASH>. Zero ArenaState layout change (SLIDING is a new u8 enum value; set/kick is free input bit 14). TDD'd in tests/test_bomb_mechanics.c; determinism script now exercises set/kick. Spec: docs/superpowers/specs/2026-07-19-bomb-mechanics-correction-design.md. Kick-vs-wall detonation flagged TODO(feel) for A1 verification."
```

---

## Plan self-review notes

- Spec coverage: §1 ruleset → Tasks 2–3; §2 sim changes → Tasks 1–3; §3 verification → Tasks 2–5 (unit tests, determinism script, hash re-pin at -O0/-O2, viewer binding + smoke); §4 docs/memory → Task 6. Slot-pressure note (spec §5) covered by the clamped-spread loop + `test_spread_cap`.
- `<NEWHASH>` is not a placeholder in the plan sense: it is *measured* in Task 4 Step 3 and substituted in Steps 4–5 and Task 6; it cannot be known before the sim change exists.
- Type consistency: `throw_bomb(ArenaState*, int, ArenaBomb*, uint16_t, q32, q32)` defined Task 2, unused elsewhere; harness helpers defined Task 2, reused Task 3; enum append keeps existing BSTATE values stable (wire compat).
- Test-geometry assumptions used: spawns at (±4.5, ±4.5) (`arena_geom.h`), pillars within |x|,|z| ≤ 2.5, walls at ±6, `TUNE_KICK_SPEED Q(0.14)` ⇒ ~9 units ≈ 65 ticks; P3 spawn (+4.5, −4.5) is a clear straight shot along z = −4.5.
