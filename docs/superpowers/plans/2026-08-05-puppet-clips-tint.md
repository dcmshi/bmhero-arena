# Puppet Clips + Tint Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Puppets 1–3 play the right vanilla clip for their sim state (idle/run/jump/carry/hit/dead), plus a timeboxed spike at per-puppet tint identity.

**Architecture:** A pure native chooser (`arena_puppet_anim(i)`: sim state → clip index) feeds a patch that re-triggers the anim funnel `func_8001C0EC` only on change — the game's engine advances frames (walker-gate principle, §8.23). A probe-only bot walks player 1 so soak gates can assert transitions. Spec: `docs/superpowers/specs/2026-08-05-puppet-clips-tint-design.md`.

**Tech Stack:** MIPS patch C (`patches/arena_render.c`), native bridge C++ (`src/arena_bridge/`), PowerShell soak tooling. All work on fork `dcmshi/BMHeroRecomp` branch **`master`** at `C:\Users\dshi\GitRepos\BMHeroRecomp`, except spec/doc commits in canonical `C:\Users\dshi\GitRepos\bmhero-arena` (`main`).

## Global Constraints

- **No sim changes.** Pinned hash stays `fbdb0d08`, TUNE_VERSION stays 21. Nothing in `lib/bmhero-arena/src/arena/` is touched. Bot input goes through `arena_tick`'s input argument — the legitimate interface.
- **No statics in patch C** (`patches/*.c`) — all state lives native-side (`arena_bridge.cpp`). Empirically crashed before; convention is hard.
- **No printf-class I/O from patches**; logging only via `arena_export_dbg_*` native entries (machinery-ref rule D/F; recomp_printf in the load window was the ⅓-of-boots crash).
- **Export ABI:** max 4 args, 32-bit each; floats cross as bit patterns (`fbits()` patch-side, union native-side).
- **Campaign/oracle path byte-identical:** every behavior change is battle-gated (`arena_bridge_is_battle()` paths only) or default-off env. `tools\oracle-gate.ps1` must stay 18/18 with zero golden edits.
- **Env knobs:** new behavior defaults ON with `=0` restoring old (`ARENA_PUPPET_ANIM`, matching `ARENA_PUPPET_MESH`); probe-only behavior defaults OFF (`ARENA_PUPPET_BOT`, `ARENA_PUPPET_TINT` experiment).
- **Fail-open + unconditional failure logging:** any guard failure leaves the spawn idle bind and logs a tag (the boot-2 trap: silence looked like success).
- **Build only via `.\build.ps1`** (it composes PATH for MSYS2 gcc — absolute-path gcc fails silently). First `-Patches always` link may fail on a regen race; the script retries once and **the retry is authoritative**. Builds+soaks are slow: use 600000ms tool timeouts. A failed build leaves the previous exe — confirm `BUILD OK` before trusting any probe.
- **Soak-before-handoff:** the human never boots a build without a green soak on that exact build.
- New `syms.ld` export addresses are sequential: next free is `0x8F000274`.

---

### Task 1: Puppet anim evidence channel + the self-advance measurement (spec Part A)

Puppets hold one idle bind from spawn; whether that instance's frame counter advances on a generic-pool slot is unmeasured and decides Task 4's existence.

**Files:**
- Modify: `src/arena_bridge/arena_bridge.h` (decl, near `arena_dbg_anim` ~line 62)
- Modify: `src/arena_bridge/arena_bridge.cpp` (logger, near the other dbg entries)
- Modify: `src/arena_bridge/arena_bridge_export.cpp` (wrapper)
- Modify: `src/main/main.cpp` (`REGISTER_FUNC`, in the arena block ~line 1147+)
- Modify: `patches/syms.ld` (append)
- Modify: `patches/arena_render.c` (DECLARE_FUNC ~line 108; call in the per-frame puppet loop ~line 920–943)

**Interfaces:**
- Produces: native `void arena_dbg_panim(int i, int idx, int framebits);` — patch-callable as `arena_export_dbg_panim(s32 i, s32 idx, s32 framebits)`. Log channels `[panim] p<i> idx=<idx> st=<state> t<tick>` (on idx change, any puppet) and `[pframe] p1 idx=<idx> f=<frame>` (10-frame burst after each idx change, puppet 1 only). Task 3 reuses both as its probe surface; idx `-3` is the reserved "no anim instance" failure sentinel.

- [ ] **Step 1: Native logger.** In `arena_bridge.cpp` near `arena_dbg_anim`:

```cpp
/* Puppet anim evidence (spec 2026-08-05 Parts A/E): the patch reports each
 * puppet's live anim (idx, frame) once per frame. Two channels, both bounded:
 * [panim] on idx CHANGE for every puppet (rare; idx=-3 = the patch found no
 * anim instance - the unconditional failure tag), and [pframe], a 10-frame
 * burst for puppet 1 after each change. 10 frames stays inside one clip loop
 * so a -Rising gate on the capture group is honest (a wrap would re-enter
 * low values and break monotonicity). frame crosses as float BITS. */
extern "C" void arena_dbg_panim(int i, int idx, int framebits) {
    static int last_idx[ARENA_MAX_PLAYERS] = { -1, -1, -1, -1 };
    static int burst = 0;
    union { int i; float f; } fb; fb.i = framebits;
    if (i < 1 || i >= ARENA_MAX_PLAYERS) return;
    if (idx != last_idx[i]) {
        last_idx[i] = idx;
        if (i == 1) burst = 10;
        if (g_log) { std::fprintf(g_log, "[panim] p%d idx=%d st=%d t%u\n",
                                  i, idx, (int)g_state.players[i].state,
                                  g_state.tick); std::fflush(g_log); }
    }
    if (i == 1 && burst > 0) {
        burst--;
        if (g_log) { std::fprintf(g_log, "[pframe] p1 idx=%d f=%d\n",
                                  idx, (int)fb.f); std::fflush(g_log); }
    }
}
```

- [ ] **Step 2: Header decl.** In `arena_bridge.h` after `arena_dbg_anim`:

```c
void  arena_dbg_panim(int i, int idx, int framebits); /* [panim]/[pframe]; framebits = f32 bits */
```

- [ ] **Step 3: Export wrapper.** In `arena_bridge_export.cpp` (copy the style of `arena_export_dbg_anim`):

```cpp
extern "C" void arena_export_dbg_panim(uint8_t* rdram, recomp_context* ctx) {
    arena_dbg_panim(_arg<0, int>(rdram, ctx), _arg<1, int>(rdram, ctx),
                    _arg<2, int>(rdram, ctx));
}
```

- [ ] **Step 4: Register + syms.** `main.cpp`: `REGISTER_FUNC(arena_export_dbg_panim);` beside the other arena exports. `patches/syms.ld` append: `arena_export_dbg_panim = 0x8F000274;`

- [ ] **Step 5: Patch call.** `patches/arena_render.c`: add near the other DECLAREs (~line 108):

```c
DECLARE_FUNC(void, arena_export_dbg_panim, s32 i, s32 idx, s32 framebits);
```

In the per-frame puppet loop (~line 925, inside `if (slot >= 0)`, after the `actionState = ACTION_IDLE` write):

```c
                    /* Anim evidence (spec 2026-08-05 Part A): live idx+frame
                     * per frame; -3 = no anim instance on this slot (the
                     * fail-open tag - logged, never silent). */
                    if (gObjects[slot].Unk140[0] >= 0)
                        arena_export_dbg_panim(i, func_8001B880(slot, 0),
                                               fbits(func_8001B62C(slot, 0)));
                    else
                        arena_export_dbg_panim(i, -3, 0);
```

(`fbits` already exists at ~line 292; the getters host-AV on a negative model slot, hence the guard — same rule as player 0's `Unk140[0] >= 0` gate.)

- [ ] **Step 6: Build.** `.\build.ps1 -Config rwdi` (fork root, 600000ms timeout). Confirm `BUILD OK`.

- [ ] **Step 7: Measure.** Run one boot: `.\tools\arena-soak.ps1 -Expect '\[pframe\] p1 idx=0 f=\d+'` — asserts the channel itself works (PASS = lines exist). Then read the evidence:

```powershell
Select-String -Path arena_bridge.log -Pattern '\[pframe\]' | Select-Object -First 12
Select-String -Path arena_bridge.log -Pattern '\[panim\]'  | Select-Object -First 8
```

**Decision (record it in the commit message and the session notes):**
- `f=` values RISE across the burst → **ADVANCES**: Task 4 is SKIPPED entirely.
- `f=` constant (all `f=0` or all equal) → **FROZEN**: Task 4 is required and blocks Task 3's rising gate (do Task 4 between 3's implementation and 3's probes).
- Any `[panim] p<i> idx=-3` on a default boot → the bomber bind failed; stop and diagnose before proceeding (fail-open tag doing its job).

- [ ] **Step 8: Commit (fork).**

```powershell
git -C C:\Users\dshi\GitRepos\BMHeroRecomp add patches/arena_render.c patches/syms.ld src/arena_bridge/arena_bridge.h src/arena_bridge/arena_bridge.cpp src/arena_bridge/arena_bridge_export.cpp src/main/main.cpp
git -C C:\Users\dshi\GitRepos\BMHeroRecomp commit -m "puppets: [panim]/[pframe] anim evidence channel; self-advance measured: <ADVANCES|FROZEN>"
```

---

### Task 2: Probe bot for player 1 + [pstate] channel + spec correction

Players 1–3 receive hard neutral every tick (`arena_bridge.cpp:564-565`) — they never move, so no anim transition is assertable without this. The spec's Part E line "sim bots do move" was wrong and gets corrected here.

**Files:**
- Modify: `src/arena_bridge/arena_bridge.cpp` (`arena_bridge_tick_input`, the `in[]` block at ~line 564, and post-`arena_tick` at ~line 568)
- Modify (canonical): `docs/superpowers/specs/2026-08-05-puppet-clips-tint-design.md` (Part E)

**Interfaces:**
- Consumes: `arena_input_pack(sx, sy, jump, bomb, set)` — stick range ±32 (`arena_state.h:27`).
- Produces: env `ARENA_PUPPET_BOT=1` → player 1 runs a canned 600-tick cycle: idle [0,180), full-forward run [180,420), jump tap [420,428), idle to 600. Log channel `[pstate] p<i> st=<state> t<tick>` on any sim-state change of players 1–3 (always on, bounded by state-change rate). Task 3's probes and Task 5's screenshots rely on both.

- [ ] **Step 1: Bot + [pstate].** In `arena_bridge_tick_input`, immediately after `in[0] = ...` (~line 566):

```cpp
    /* Probe bot (spec 2026-08-05): ARENA_PUPPET_BOT=1 drives player 1 on a
     * canned 600-tick cycle (idle / run forward / jump tap / idle) so soak
     * gates can assert clip transitions. Inputs only - the sim's legitimate
     * interface - and default OFF: every existing probe and netplay sees a
     * byte-identical input stream. */
    static const bool bot = []() {
        const char* v = std::getenv("ARENA_PUPPET_BOT");
        return v && v[0] == '1'; }();
    if (bot) {
        uint32_t c = g_state.tick % 600u;
        int run  = (c >= 180u && c < 420u) ? 1 : 0;
        int jump = (c >= 420u && c < 428u) ? 1 : 0;
        in[1] = arena_input_pack(0, run ? 32 : 0, jump, 0, 0);
    }
```

After the `arena_tick(&g_state, in);` call (~line 567), add:

```cpp
    /* [pstate]: sim-state edges for players 1-3 - the probe surface that says
     * the bot MOVED the sim (independent of any anim plumbing above it). */
    {
        static uint8_t prev_st[ARENA_MAX_PLAYERS] = { 255, 255, 255, 255 };
        for (int pi = 1; pi < ARENA_MAX_PLAYERS; pi++) {
            if (g_state.players[pi].state != prev_st[pi]) {
                prev_st[pi] = g_state.players[pi].state;
                if (g_log) { std::fprintf(g_log, "[pstate] p%d st=%d t%u\n",
                                          pi, (int)prev_st[pi],
                                          g_state.tick); std::fflush(g_log); }
            }
        }
    }
```

- [ ] **Step 2: Build.** `.\build.ps1 -Config rwdi`. Confirm `BUILD OK`.

- [ ] **Step 3: Red-then-green probe.** Bot OFF first — the pattern must be absent (this falsifies the gate):

```powershell
.\tools\arena-soak.ps1 -Absent '\[pstate\] p1 st=1'
```
Expected: PASS (no RUN state without the bot). Then bot ON:

```powershell
$env:ARENA_PUPPET_BOT='1'
.\tools\arena-soak.ps1 -Expect '\[pstate\] p1 st=1'
Remove-Item Env:\ARENA_PUPPET_BOT
```
Expected: PASS. Also confirm the jump landed: `Select-String arena_bridge.log -Pattern '\[pstate\] p1 st=2'` shows at least one line (PSTATE_JUMP=2). If RUN never appears, the bot input isn't reaching the tick — check the block sits AFTER `in[0]` assignment and BEFORE `arena_tick`.

- [ ] **Step 4: Spec correction (canonical).** In the spec's Part E, replace the sentence claiming transitions can be asserted because "sim bots do move" with: probes drive player 1 via `ARENA_PUPPET_BOT=1` (canned input cycle; players otherwise receive neutral every tick and never move).

- [ ] **Step 5: Commit both repos.**

```powershell
git -C C:\Users\dshi\GitRepos\BMHeroRecomp add src/arena_bridge/arena_bridge.cpp
git -C C:\Users\dshi\GitRepos\BMHeroRecomp commit -m "puppets: ARENA_PUPPET_BOT probe bot (player 1 canned cycle) + [pstate] channel"
git -C C:\Users\dshi\GitRepos\bmhero-arena add docs/superpowers/specs/2026-08-05-puppet-clips-tint-design.md
git -C C:\Users\dshi\GitRepos\bmhero-arena commit -m "spec: puppet clips - correct Part E (players 1-3 get neutral input; probes need ARENA_PUPPET_BOT)"
```

---

### Task 3: Clip chooser + trigger-on-change patch + probes (spec Parts B/C/E)

**Files:**
- Modify: `src/arena_bridge/arena_bridge.h`
- Modify: `src/arena_bridge/arena_bridge.cpp` (place AFTER `arena_hit_anim_index`'s definition — the chooser calls it)
- Modify: `src/arena_bridge/arena_bridge_export.cpp`
- Modify: `src/main/main.cpp`
- Modify: `patches/syms.ld`
- Modify: `patches/arena_render.c` (spawn block ~line 873; per-frame loop from Task 1)

**Interfaces:**
- Consumes: `[panim]`/`[pframe]` channel (Task 1), `ARENA_PUPPET_BOT` (Task 2), existing `arena_hit_anim_index()`.
- Produces: native `int arena_puppet_anim(int i)` → clip index, `-1` = hide (DEAD only), `-2` = leave the spawn bind alone (knob off / unbound / bad i). Native `void arena_puppet_bound(int i)` marks puppet i as bomber-bound. Patch-callable: `arena_export_puppet_anim(s32 i)`, `arena_export_puppet_bound(s32 i)`. Env `ARENA_PUPPET_ANIM=0` = A/B off switch.

- [ ] **Step 1: Native chooser + bound flag.** In `arena_bridge.cpp`, after `arena_hit_anim_index`:

```cpp
/* Puppet clip chooser (spec 2026-08-05 Part B): a PURE function of the sim
 * player's state - no edges, no windows (those stay player-0 pose-window
 * territory). Clip indices are the measured vanilla vocabulary
 * (tools/oracle/timelines.json): idle 0, run 3, jump 6 (rising) / 7 (falling),
 * carry 14/17/20, hit = the same clip player 0 uses. -1 is reserved for DEAD
 * (patch hides the actor); a disabled hit clip falls back to 0, never to -1.
 * -2 = leave the spawn bind alone: knob off, puppet never bomber-bound, or
 * bad index. ARENA_PUPPET_ANIM=0 restores the 8.40 hold-idle behaviour
 * (one-binary A/B, 8.18 rule). */
static bool g_puppet_bound[ARENA_MAX_PLAYERS] = { false, false, false, false };
extern "C" void arena_puppet_bound(int i) {
    if (i >= 1 && i < ARENA_MAX_PLAYERS) g_puppet_bound[i] = true;
}
extern "C" int arena_puppet_anim(int i) {
    static const int on = []() {
        const char* v = std::getenv("ARENA_PUPPET_ANIM");
        return (v && v[0] == '0') ? 0 : 1;
    }();
    if (!on || i < 1 || i >= ARENA_MAX_PLAYERS || !g_puppet_bound[i]) return -2;
    const ArenaPlayer* p = &g_state.players[i];
    const int held = (p->held_bomb != 0);
    switch (p->state) {
    case PSTATE_IDLE:   return held ? 14 : 0;
    case PSTATE_RUN:    return held ? 17 : 3;
    case PSTATE_JUMP:   return held ? 20 : ((p->vel.y > 0) ? 6 : 7);
    case PSTATE_TUMBLE: { const int h = arena_hit_anim_index(); return (h >= 0) ? h : 0; }
    case PSTATE_DEAD:   return -1;
    default:            return -2;
    }
}
```

- [ ] **Step 2: Decls + wrappers + registration.** `arena_bridge.h`:

```c
int   arena_puppet_anim(int i);   /* clip for puppet i; -1 hide, -2 leave alone */
void  arena_puppet_bound(int i);  /* mark puppet i bomber-bound (spawn block) */
```

`arena_bridge_export.cpp`:

```cpp
extern "C" void arena_export_puppet_anim(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, arena_puppet_anim(_arg<0, int>(rdram, ctx)));
}
extern "C" void arena_export_puppet_bound(uint8_t* rdram, recomp_context* ctx) {
    arena_puppet_bound(_arg<0, int>(rdram, ctx));
}
```

`main.cpp`: `REGISTER_FUNC(arena_export_puppet_anim); REGISTER_FUNC(arena_export_puppet_bound);`
`syms.ld`: `arena_export_puppet_anim = 0x8F000278;` and `arena_export_puppet_bound = 0x8F00027C;`

- [ ] **Step 3: Patch.** `arena_render.c` DECLAREs:

```c
DECLARE_FUNC(s32,  arena_export_puppet_anim, s32 i);
DECLARE_FUNC(void, arena_export_puppet_bound, s32 i);
```

Spawn block (~line 873): immediately after the existing `func_8001C0EC(slot, 0, 0, 1, (u32*)D_80115808);` bind, add `arena_export_puppet_bound(i);` (the bomb-placeholder branch does NOT get it — the chooser then returns −2 for that puppet and its texanim is never touched).

Per-frame loop: replace Task 1's evidence block with the drive + evidence:

```c
                    /* Clip drive (spec 2026-08-05 Part C): trigger the funnel
                     * ONLY on change; the engine owns the frames (walker-gate
                     * principle). -1 = dead -> hidden (overrides this frame's
                     * ACTION_IDLE write above; respawn un-hides automatically).
                     * -2 = leave the spawn bind alone (knob off / unbound).
                     * -3 in [panim] = no anim instance: fail-open, logged. */
                    if (gObjects[slot].Unk140[0] >= 0) {
                        s32 want = arena_export_puppet_anim(i);
                        if (want == -1) {
                            gObjects[slot].actionState = ACTION_NONE;
                        } else if (want >= 0 && func_8001B880(slot, 0) != want) {
                            func_8001C0EC(slot, 0, want, 1, (u32*)D_80115808);
                        }
                        arena_export_dbg_panim(i, func_8001B880(slot, 0),
                                               fbits(func_8001B62C(slot, 0)));
                    } else {
                        arena_export_dbg_panim(i, -3, 0);
                    }
```

- [ ] **Step 4: Build.** `.\build.ps1 -Config rwdi`. Confirm `BUILD OK`.

- [ ] **Step 5: Probes (green).** One bot boot, three assertions from it:

```powershell
$env:ARENA_PUPPET_BOT='1'
.\tools\arena-soak.ps1 -Expect '\[panim\] p1 idx=3 st=1'
Remove-Item Env:\ARENA_PUPPET_BOT
Select-String arena_bridge.log -Pattern '\[panim\] p1 idx=[67] st=2' | Select-Object -First 4
Select-String arena_bridge.log -Pattern '\[pframe\] p1 idx=3 f=\d+'  | Select-Object -First 10
```

Expected: soak PASS; at least one jump-clip line (6 and/or 7); `[pframe]` idx=3 samples present. If Task 1 measured ADVANCES, also run the rising gate on a fresh boot:

```powershell
$env:ARENA_PUPPET_BOT='1'
.\tools\arena-soak.ps1 -Rising '\[pframe\] p1 idx=3 f=(\d+)'
Remove-Item Env:\ARENA_PUPPET_BOT
```

Expected: PASS (10-sample burst, one clip loop — see Task 1's bound rationale). If the values in the burst wrap (clip shorter than the burst), shrink the burst constant in `arena_dbg_panim` until the gate is honest, rebuild, re-run.

- [ ] **Step 6: Falsification (the gate must be breakable).** Same probe with the knob off must FAIL:

```powershell
$env:ARENA_PUPPET_BOT='1'; $env:ARENA_PUPPET_ANIM='0'
.\tools\arena-soak.ps1 -Expect '\[panim\] p1 idx=3 st=1'
Remove-Item Env:\ARENA_PUPPET_BOT; Remove-Item Env:\ARENA_PUPPET_ANIM
```

Expected: **FAIL** (exit code 1, pattern absent — puppet stays on idle 0). A PASS here means the knob or the gate is broken; stop and fix.

- [ ] **Step 7: Full gate.** `.\build.ps1 -Config rwdi -Soak 5` (600000ms; runs soak + oracle-gate). Expected: SOAK GREEN, oracle-gate **18/18**. The campaign path saw no change; if any oracle check moved, something leaked outside the battle gate — fix before committing.

- [ ] **Step 8: Commit (fork).**

```powershell
git -C C:\Users\dshi\GitRepos\BMHeroRecomp add patches/arena_render.c patches/syms.ld src/arena_bridge/arena_bridge.h src/arena_bridge/arena_bridge.cpp src/arena_bridge/arena_bridge_export.cpp src/main/main.cpp
git -C C:\Users\dshi\GitRepos\BMHeroRecomp commit -m "puppets: per-state clips - native chooser + trigger-on-change through the 8001C0EC funnel (spec Parts B/C/E)"
```

---

### Task 4: Frame-write fallback (spec Part D) — ONLY if Task 1 measured FROZEN

Skip this task entirely (and say so in the session notes) if Task 1 measured ADVANCES.

**Files:**
- Modify: `src/arena_bridge/arena_bridge.h` / `arena_bridge.cpp` / `arena_bridge_export.cpp`, `src/main/main.cpp`, `patches/syms.ld`, `patches/arena_render.c`

**Interfaces:**
- Consumes: the chooser (Task 3); `func_8001B6BC(s32 obj, s32 part, f32 frame)` (decomp `boot/17930.c:1075` — frame is a FLOAT).
- Produces: native `int arena_puppet_frame(int i, int idx)` → next frame value for puppet i (int; patch converts to f32), advancing +1 per call, resetting to 0 when `idx` differs from the last call's, wrapping at a per-clip length table.

- [ ] **Step 1: Measure clip lengths.** Lengths are NOT in `timelines.json` (verb timelines, not clip lengths). Boot once with the bot and read puppet 1's `[pframe]` bursts per clip (Task 1's channel shows the counter the engine holds); where that is insufficient, read player 0's `[animw]` lines from a `tools\play.ps1` or soak log for the same clips (0, 3, 6, 7, 14, 17, 20, hit). Record a `static const` table `{idx, len}` in the bridge.

- [ ] **Step 2: Native counter.**

```cpp
/* Part D fallback (built because Part A measured FROZEN): generic-pool
 * instances hold their frame, so the bridge steps it - the Mirror-Bomber way
 * (ED210.c func_800FBCB0 + func_8001B6BC). +1 per render frame, reset on
 * clip change, wrap at the measured clip length. */
extern "C" int arena_puppet_frame(int i, int idx) {
    static int last[ARENA_MAX_PLAYERS] = { -1, -1, -1, -1 };
    static int fr[ARENA_MAX_PLAYERS]   = { 0, 0, 0, 0 };
    if (i < 1 || i >= ARENA_MAX_PLAYERS) return 0;
    if (idx != last[i]) { last[i] = idx; fr[i] = 0; return 0; }
    fr[i] = (fr[i] + 1) % clip_len(idx);   /* clip_len: the Step 1 table, default 30 */
    return fr[i];
}
```

- [ ] **Step 3: Wire it** (wrapper `arena_export_puppet_frame` = `0x8F000280`, REGISTER_FUNC, DECLARE, and in the patch's drive block after the trigger/no-trigger decision):

```c
                        if (want >= 0)
                            func_8001B6BC(slot, 0, (f32)arena_export_puppet_frame(i, want));
```

- [ ] **Step 4: Build + rerun Task 3 Step 5's rising gate.** Expected: PASS now. Then the falsification: with `ARENA_PUPPET_ANIM='0'` the rising gate must FAIL.

- [ ] **Step 5: Full gate + commit (fork).** `.\build.ps1 -Config rwdi -Soak 5` green, then:

```powershell
git -C C:\Users\dshi\GitRepos\BMHeroRecomp add patches/arena_render.c patches/syms.ld src/arena_bridge/arena_bridge.h src/arena_bridge/arena_bridge.cpp src/arena_bridge/arena_bridge_export.cpp src/main/main.cpp
git -C C:\Users\dshi\GitRepos\BMHeroRecomp commit -m "puppets: frame-write fallback (Part D) - generic-pool instances measured frozen; Mirror-Bomber 8001B6BC stepping"
```

---

### Task 5: Tint spike (spec Part F) — timeboxed: one session, max 2 instrumented boots

Goal: three visibly distinct puppets. Both outcomes are deliverables; the failure outcome is a recorded negative finding, not a silent stop.

**Files:**
- Read: decomp under `C:\Users\dshi\GitRepos\BMHeroRecomp\lib\bmhero\src\` (readable C — grep it before instrumenting anything)
- Possibly modify: `patches/arena_render.c`, bridge files (knob export `arena_export_puppet_tint_on` = `0x8F000284` if the experiment lands)

**Interfaces:**
- Consumes: puppet slots (`arena_export_puppet_get_slot`), spawn block.
- Produces (success): `ARENA_PUPPET_TINT=0` A/B knob + per-puppet color writes. Produces (failure): a §8.41 paragraph recording exactly what was ruled out.

- [ ] **Step 1: Read first (free).** The draw chain from the dispatcher: `func_8001CEF4` / `func_8001CD20` / `func_8001AD6C` (the three per-frame calls every active object gets, `boot/26CE0.c` / `boot/17930.c`), and the skeletal model consumer `func_8001191C`. Look for: per-object or per-part RGBA fields consumed during draw (prim/env color writes, light/ambient setup keyed on `gObjects` or the model-pool record `D_80165290`), and any existing game object that renders the SAME mesh in a different color (that object's class code names the field). Reject any path that writes the shared file-1 model data — that tints all four bombers at once.

- [ ] **Step 2: Decide.** If no per-draw color path surfaced from reading, STOP the spike here (0 boots spent): record the negative finding (which functions were read, why each candidate was rejected) for §8.41, and identity is formally deferred to after A3. Do not burn boots fishing.

- [ ] **Step 3 (only with a concrete candidate): experiment.** Write the candidate field on puppet slots 1–3 at spawn (three distinct values, e.g. red/blue/green-ish), behind `ARENA_PUPPET_TINT` (default ON, `=0` restores). Build, one soak boot for stability, then `.\tools\capture-game.ps1` and READ the screenshot (`tools\game.png`). Distinct tints = success. Wrong/no effect = one adjustment allowed (second boot); then stop either way.

- [ ] **Step 4: Commit (fork) — either outcome.**

```powershell
git -C C:\Users\dshi\GitRepos\BMHeroRecomp add -A
git -C C:\Users\dshi\GitRepos\BMHeroRecomp commit -m "puppets: tint spike - <landed: per-object color via <path>, ARENA_PUPPET_TINT A/B | negative: no per-draw color path; findings recorded, identity deferred to post-A3>"
```

---

### Task 6: Docs, submodule bump, final gate, human-boot checklist

**Files:**
- Modify (canonical): `docs/bmhero-recomp-integration-notes.md` (append §8.41), `docs/HANDOFF-2026-08-05.md` (new, supersedes 08-04), `CLAUDE.md` (Where-things-stand + A1-remaining paragraphs)
- Modify (fork): submodule `lib/bmhero-arena` pointer

**Interfaces:** none — this is the record.

- [ ] **Step 1: §8.41.** Append to the integration notes, in the house style (mechanism + evidence + what's deferred): the self-advance measurement and its answer; the chooser table and its -1/-2 sentinels; the bot knob; trigger-on-change through the funnel; the tint spike outcome (either way); knobs (`ARENA_PUPPET_ANIM`, `ARENA_PUPPET_BOT`, `ARENA_PUPPET_TINT` if it exists).

- [ ] **Step 2: Handoff.** Write `docs/HANDOFF-2026-08-05.md` superseding 08-04: item 1 = the COMBINED human boot (feel-round-13 checklist from 08-04 item 1, verbatim, PLUS: puppets run when their sim players are driven / a `$env:ARENA_PUPPET_BOT='1'; .\tools\play.ps1` variant shows puppet 1 running and jumping; `ARENA_PUPPET_ANIM=0` A/B; tint check if the spike landed); item 2 = A3 (promoted); carry forward items 4–6 from 08-04 (battle map post-A3, respawn accommodation, minors). Update `CLAUDE.md`'s state paragraphs to match (A1 bullet: per-state puppet clips DONE, identity per spike outcome; read-first table points at the new handoff).

- [ ] **Step 3: Commit canonical, push, bump submodule.**

```powershell
git -C C:\Users\dshi\GitRepos\bmhero-arena add docs/ CLAUDE.md
git -C C:\Users\dshi\GitRepos\bmhero-arena commit -m "docs: 8.41 puppet clips (+tint spike outcome); HANDOFF-2026-08-05 - combined boot on top, A3 next"
git -C C:\Users\dshi\GitRepos\bmhero-arena push
git -C C:\Users\dshi\GitRepos\BMHeroRecomp submodule update --remote lib/bmhero-arena
git -C C:\Users\dshi\GitRepos\BMHeroRecomp add lib/bmhero-arena
git -C C:\Users\dshi\GitRepos\BMHeroRecomp commit -m "chore: submodule -> canonical (8.41 puppet clips)"
```

- [ ] **Step 4: Final full gate on the exact handoff build.** `.\build.ps1 -Config rwdi -Soak 5` → SOAK GREEN + 18/18, THEN push the fork: `git -C C:\Users\dshi\GitRepos\BMHeroRecomp push`. (Soak-before-handoff: this build is the one the human boots.)

- [ ] **Step 5: Hand the human the boot checklist** (in the final session message, not just the handoff file): the 08-04 feel-round items verbatim + the new puppet checks + the knobs to A/B.

---

## Self-Review (done at write time)

- **Spec coverage:** Part A → Task 1; Part B/C/E → Task 3 (probes split across 2+3); Part D → Task 4 (conditional, matching the spec's "built ONLY if"); Part F → Task 5; Rollout → Task 6. Spec's Part E falsification requirement → Task 2 Step 3 (bot gate) and Task 3 Step 6 (knob gate).
- **Placeholders:** commit messages contain `<ADVANCES|FROZEN>`-style choose-one markers — those are recorded decisions, not TBDs; each has a decision step that produces the value. Task 4 Step 1's length table is data-dependent by nature; the procedure to obtain every entry is specified.
- **Type consistency:** `arena_puppet_anim(int)->int`, `arena_puppet_bound(int)->void`, `arena_dbg_panim(int,int,int)->void`, `arena_puppet_frame(int,int)->int` used identically in decls, wrappers, DECLAREs, and call sites; syms addresses 0x274/0x278/0x27C/0x280/0x284 unique and sequential.
