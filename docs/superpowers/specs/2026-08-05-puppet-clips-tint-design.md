# Puppet clips + tint spike (handoff item 2) — design

**Date:** 2026-08-05
**Status:** approved in-conversation (post-§8.40 session); this document is the
reviewable record.
**Scope:** fork-side only (`dcmshi/BMHeroRecomp`). No sim changes; the pinned
hash `fbdb0d08` (v21) must not move. `anim_hint` stays unused.

## Problem

§8.40 put the real bomber mesh on puppets 1–3, but each holds the single idle
bind from spawn (`func_8001C0EC(slot, 0, 0, 1, D_80115808)`), forever. Three
gaps, from the 2026-08-04 handoff item 2:

1. Whether a generic-pool anim instance even advances its frames is
   **unmeasured** — a still frame cannot say (§8.40 "deliberately deferred").
2. Puppets show idle while their sim players run, jump, carry, and get hit.
3. All four bombers share the one resident skin — P2–P4 have no visual
   identity.

## User decisions (2026-08-05)

- **Sequencing:** build this slice first; ONE human boot afterwards covers the
  feel-round-13 checklist AND this work (A/B knobs isolate it).
- **Clip scope:** locomotion + hit + hide-on-death, **carry variants IN**
  (stateless rows, same mechanism). Edge-triggered verb poses (set/kick/throw,
  landing clip 8) stay deferred.
- **Identity:** tint spike NOW, timeboxed; on failure identity is formally
  deferred to after A3.

## Non-goals

- Per-puppet pose windows (set/kick/throw/landing). Deferred: they are
  edge-triggered, i.e. real state machines, not pure functions of sim state.
- Any campaign/oracle-path behaviour change. Everything is battle-gated; the
  18-check oracle-gate must stay green with zero golden edits.
- Sim changes of any kind (invariant #4: the hash moves only with a
  TUNE_VERSION bump, and nothing here needs sim state).

## Part A — measurement first: do generic-pool instances self-advance?

Before any clip logic. Per-frame `[panim]` log line for puppet 1 from the
patch's existing per-frame puppet loop: `func_8001B880(slot,0)` (idx) +
`func_8001B62C(slot,0)` (frame), through `arena_export_dbg_*` (native I/O; the
§machinery-ref D/F rule — no printf-class work patch-side). Soak probe asserts
`-Rising 'pframe=(\d+)'` over the idle clip.

- **Advances** → Part C triggers-on-change and the engine owns frames (the
  walker-gate principle, §8.18/§8.23).
- **Frozen** → Part D (Mirror-Bomber frame writes) becomes load-bearing.

The `[panim]` line stays after the slice ships — it is the probe surface for
Part E's assertions.

## Part B — native clip chooser

`arena_puppet_anim(i)` in `arena_bridge.cpp`, a **pure function** of
`g_state.players[i]` (state, vel.y, held_bomb). Clip indices are the measured
vanilla vocabulary (`tools/oracle/timelines.json`), not guesses:

| sim state                | free hands | holding (held_bomb != 0) |
|--------------------------|-----------:|-------------------------:|
| PSTATE_IDLE              |          0 |                       14 |
| PSTATE_RUN               |          3 |                       17 |
| PSTATE_JUMP, vel.y > 0   |          6 |                       20 |
| PSTATE_JUMP, vel.y <= 0  |          7 |                       20 |
| PSTATE_TUMBLE            | `arena_hit_anim_index()`; if env-disabled (< 0), fall back to 0 — never to "hide" ||
| PSTATE_DEAD              | −1 = hide (patch sets ACTION_NONE, like inactive bombs) ||

−1 is reserved for DEAD alone; a disabled hit clip must not read as a hidden
puppet. Hide is a per-frame override of the loop's ACTION_IDLE write, so a
respawned player (state leaves DEAD) un-hides automatically the same frame.

Exported as `arena_export_puppet_anim(i)` (int in, int out — plain export ABI,
no float crossing). Env knob `ARENA_PUPPET_ANIM=0` makes it return a sentinel
(−2 = "leave the spawn bind alone") for the one-binary A/B (§8.18 rule).

## Part C — patch: trigger on change only

In the existing per-frame puppet positioning loop (`arena_render.c`), only for
puppets that got the bomber bind (`mesh_cfg >= 0` path — placeholder bombs
keep their texanim untouched):

```c
s32 want = arena_export_puppet_anim(i);
if (want == -1)      gObjects[slot].actionState = ACTION_NONE;   /* dead */
else if (want >= 0 && func_8001B880(slot, 0) != want)
    func_8001C0EC(slot, 0, want, 1, (u32*)D_80115808);
```

One trigger per state change; the engine advances the clip (stop the fight,
don't win it every frame). No patch statics (safe-convention memory rule);
whatever bookkeeping Part D needs lives native-side.

## Part D — frame-advance fallback (built ONLY if Part A says frozen)

`func_8001B6BC(slot, 0, frame)` per frame, the Mirror-Bomber way
(`ED210.c:func_800FBCB0`). Frame counters live native-side keyed by puppet
index, reset on clip change, wrapping at the clip length. Lengths are NOT in
`timelines.json` (those are verb timelines); take them from goldens where one
exists, else measure live via `func_8001B62C` on player 0 playing the same
clip (one log run). If Part A shows self-advance, this part is skipped and
the measurement is recorded in §8.x.

## Part E — verification

- **Fail-open + log the failure path unconditionally** (the boot-2 trap:
  absence of a tag was the diagnosis). Any guard failure leaves the spawn
  idle bind in place and logs why.
- Soak: new probes on `[panim]` — clip transitions occur (`-Expect` on
  idx 0→3 for a moving puppet) and frames rise. Moving a puppet takes a
  deliberate driver: players 1–3 receive hard neutral input every tick
  (`arena_bridge.cpp`) and never move on their own, so probes drive player 1
  via `ARENA_PUPPET_BOT=1` (a canned input cycle: idle / run forward / jump
  tap / idle). Existing soak + oracle-gate 18/18 must stay green
  **untouched**.
- Falsifiability check (gates-must-be-falsifiable memory): before trusting the
  new probe, break it once on purpose (`ARENA_PUPPET_ANIM=0` must make the
  transition probe FAIL, not silently pass).

## Part F — tint spike (timeboxed)

Goal: three visibly distinct puppets via a per-object color path on the
skeletal draw. **Read before instrumenting** (reading the decomp is free):
the object-draw path from `func_8001191C` down, grepping `lib/bmhero/` for
prim/env-color writes keyed on `gObjects` fields or per-part color state.

- Timebox: one session, max **2 instrumented boots**.
- Success: screenshot (capture-game.ps1 → RenderDoc if it disagrees) showing
  distinct P1–P3 tints, behind `ARENA_PUPPET_TINT=0` A/B.
- Failure: identity **formally deferred to after A3** (the user's named
  fallback), and the negative finding recorded in §8.x with what was ruled
  out, so the next attempt doesn't restart from zero.

The spike is independent of Parts A–E and lands as its own commit either way.

## Rollout

1. Parts A–E as one fork slice on `master`; `build.ps1 -Config rwdi -Soak 5`
   green before anything reaches the human (soak-before-handoff rule).
2. Part F commit (or its recorded failure).
3. §8.41 integration note + handoff update.
4. ONE human boot: feel-round-13 checklist (v19–v21 items) + "puppets
   run/jump/react/carry" + `ARENA_PUPPET_MESH` / `ARENA_PUPPET_ANIM` /
   `ARENA_PUPPET_TINT` A/Bs as needed.
