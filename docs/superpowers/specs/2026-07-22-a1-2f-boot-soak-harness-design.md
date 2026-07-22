# A1.2f — boot-soak smoke harness — design spec

**Date:** 2026-07-22 · **Status:** approved (user-directed, pulled forward
mid-A1.2e after the 5th stochastic load-window crash burned another playtest)
**Purpose:** make arena-boot stability machine-checkable: N unattended boots
that each reach the in-arena spawn point, with crash/hang classification — so
load-window land mines (4 found so far, one fix unproven) are caught and fixes
are PROVEN without human boots.

## 1. Exit criterion

`tools/arena-soak.ps1 -N 10` runs unattended and prints a per-iteration
PASS/CRASH/HANG table + summary; a PASS = the log's `[capture]` marker
appeared (battle mode + arena loaded + warmup elapsed + spawn block ran —
past every land mine observed). Nonzero exit if any iteration failed.

## 2. Mechanism (fork side, `src/main/main.cpp` + `src/arena_bridge/`)

- **Auto-battle:** env `ARENA_AUTO_BATTLE=1` → at launcher init, run the
  Battle menu option's exact body (`arena_bridge_set_battle_mode(1)` +
  `recomp::start_game`) without a click. (If start-at-init races UI setup,
  fall back to deferring one launcher frame — empirically decided by the
  harness itself.)
- **Frontend mash:** wrap the registered input callback
  (`recompinput::profiles::get_n64_input`, `main.cpp:868`) with a fork shim:
  while auto-battle is set AND `arena_puppet_ready() == 0`, OR in synthetic
  START/A presses (4 polls on / 4 off, alternating buttons) on controller 0.
  Injection is below SDL — **no window focus needed**. Mash self-stops once
  the spawn block runs. In-level A/START jitter before that is harmless to
  the pass signal.
- **Soak script `tools/arena-soak.ps1`:** per iteration: kill stray game
  processes; record `arena_bridge.log` length + CrashDumps count; launch
  `build-rwdi\BMHeroRecompiled.exe` with the env var (repo-root CWD so
  `assets/` resolves); poll the log DELTA for `[capture]` (timeout 75 s);
  classify PASS / CRASH (process died; report new dump name) / HANG (timeout,
  process alive); kill; summary + exit code. Log appends across runs — delta
  scan, not whole-file.

## 3. Non-goals

- Not a gameplay/feel test (that stays human).
- No CI wiring this slice (local tool first; CI job later if it earns it).
- No patches/*.c changes; sim untouched (pinned hash `4b6687d4`).

## 4. Use immediately after landing

1. Baseline soak on the current A1.2e branch → measures the real failure rate
   incl. the unproven stale-routine fix (`func_8001D9E4` land mine).
2. Iterate that fix (and any future load-window fix) against 10–20 boots.
3. Gate A1.2e's feel-test request on a clean soak — no more human crash
   detection.
