# A1.2c (slice 2) — blast/explosion effects — design spec

**Date:** 2026-07-21 · **Status:** approved (design review in-session)
**Purpose:** the detonation payoff — when a sim bomb detonates, show an
explosion at each blast site. Completes A1.2c (slice 1 shipped bombs; this
slice ships blasts). Rendering only — blast *damage* already exists in the sim.
**Builds on:** A1.2c slice 1 (bombs render; pool machinery), A1.2b (frozen-origin
mapping, boss-suppression sweep, patch/export bridge).
**Companion:** integration notes §8, `arena_state.h` (`ArenaBlast`),
`arena_tuning.h` (`TUNE_BLAST_*`).

## 1. Goal and exit criterion

**Exit criterion (human boot gate):** set/throw a bomb → on detonation an
explosion effect appears at the blast center; a 4-bomb spread detonating →
multiple explosions; no crash; player actors + bombs unaffected. Sim untouched
(pinned hash `4b6687d4`).

## 2. Sim source of truth

`ArenaState.blasts[16]`: `center` (Q20.12), `radius_t` (grows 0..12 ticks to
`TUNE_BLAST_RADIUS` = 1.60 sim units ≈ 190 Hero units), `ttl` (20 ticks ≈ ⅓ s;
`ttl == 0` = slot free). A spread can produce 4+ simultaneous blasts.

## 3. Mechanism — the game's effect spawner (spike-verified), fallback pool

**Primary: `func_80081468(s32 id, f32 x, f32 y, f32 z)`** — the game's
spawn-effect-by-ID-at-position API (`GLOBAL_ASM`, but function symbols resolve
in patches; float args are fine MIPS-to-MIPS — only the native-export ABI has
the float restriction). Known call sites use IDs `0x2BC..0x2CD` (bomb trail
puffs `0x2C5`, splashes `0x2CB/2CC`, ambient rain `0x2C1`/706). Rain calls it
every frame without flooding the 64-slot actor pool → effects very likely live
OUTSIDE `gObjects[14..77]`, i.e. sweep-safe. Which ID is an explosion is
unknown → the spike.

### 3.1 Phase 1 — effect-ID spike (one boot, all 18 candidates)

On a one-shot trigger after arena entry (a native latch export — the patch must
stay stateless), call `func_80081468(id, ...)` for **every** `id` in
`0x2BC..0x2CD`, laid out in a spaced row near the player (≈80 Hero units apart,
at ground height). One screenshot + human eyes answer both unknowns at once:
which ID reads as an explosion, and whether effects render at all (proving
sweep-safety). **If nothing appears:** one retry with the boss sweep gated off
isolates whether the sweep kills effects; contingency is switching the sweep
from every-frame to an entry-window (only if the boss stays down — verify).

### 3.2 Phase 2 — wiring detonations

- **Native (`arena_bridge.cpp`):** per-blast edge detection + placement, via the
  existing frozen frame:
  - `arena_blast_new(i)` → 1 exactly once when `blasts[i]` transitions to alive
    (native tracks previous liveness per index; resets when the slot frees);
  - `arena_blast_wx/wy/wz(i)` → world coords of `blasts[i].center` (same
    mapping as bombs, incl. Y).
  Exports + `REGISTER_FUNC` + `syms.ld` per the proven 4-step bridge; int args /
  float returns only.
- **Patch (`arena_render.c`):** each frame after the tick, for `i` in `0..15`:
  `if (arena_export_blast_new(i)) func_80081468(EXPLOSION_ID, wx, wy, wz);`
  (`EXPLOSION_ID` = the spike's winner, a `#define` with the spike date noted).

**Fire-and-forget timing:** the effect plays its own animation; the sim's
20-tick damage window stays authoritative for gameplay. A cosmetic duration
mismatch between the visual and the damage window is **accepted for v1**.
Sound: if the effect carries its own SFX, we get it free; not a requirement.

### 3.3 Fallback — pooled blast actors (only if the spike fails)

Pool ~4 blast actors at blast centers, `Scale` driven by `radius_t`, hidden at
`ttl` end. Gated on its own two mini-spikes first (we are near the spawn-pool
ceiling — 9 actors OK, 11 crashed): (a) does a static mesh draw via the general
spawn *without* an anim bind (saves anim-pool slots); (b) does the generic
`[14..77]` draw path respect `Scale` (the bomb-pool `[2..5]` path does;
generic unverified).

## 4. Non-goals (deferred, with owner)

- Fuse blink / bomb flash → visual-feel fast-follow.
- Camera shake, HUD, per-player tint → later A1.2 items.
- Real bomber mesh → §8.5b follow-up (unrelated).
- Blast visual tracking the exact damage sphere → only if the fire-and-forget
  effect reads poorly (the rejected "both" option).
- No `bmhero-arena` sim changes (pinned hash `4b6687d4`).

## 5. Build & testing

- Fork branch `feature/a1.2b-spawn-bombers` (continue). Touches:
  `src/arena_bridge/*` (blast edge/placement exports + spike latch),
  `src/main/main.cpp`, `patches/arena_render.c`, `patches/syms.ld`. Any
  `patches/*.c` edit → `make -C patches clean` before the cmake build.
- Verification: build-exit gate + **human boot gates** (spike: identify the
  explosion ID on screen; final: detonations show explosions). Agent verifies
  via `capture-game.ps1` + `arena_bridge.log`; human does the ~15s nav.
- `bmhero-arena` repo: this spec + CLAUDE.md status on completion.
