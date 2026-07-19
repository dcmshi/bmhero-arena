# Bomb Mechanics Correction — design spec

**Date:** 2026-07-19 · **Status:** approved (design review in-session)
**Purpose:** replace the sim's invented two-charge-tier throw with Bomberman
Hero's verified mechanics: single-arc throw with stick-tilt distance, 4-bomb
spread on long hold, set + kick. First intentional gameplay change to
`src/arena/` — pinned CI hash moves, `TUNE_VERSION` bumps.
**Sources:** GameFAQs guides (gamerman555 46669, marshmallow 3166),
StrategyWiki Bomberman Hero/Controls, Bomberman Wiki, TASVideos #8700, plus
owner playtest memory. Kicked-bomb-explodes-on-contact is owner-recalled and
not contradicted by sources; flagged `TODO(feel)` for A1 verification against
the real game.

---

## 1. Ruleset (replaces the bomb bullet of `docs/bmhero-battle-arena-design.md` §2)

- **Grab/throw (B, input bit 13):** press to pull out a bomb (can run and
  jump while holding). Release = **single-arc throw** in facing direction:
  forward speed scales with the stick tilt sampled at the release tick
  (neutral stick = short drop-lob at a minimum fraction). No charge tiers —
  Hero has no pump.
- **Spread (hold B ≥ `TUNE_SPREAD_TICKS`):** release throws **4 bombs in a
  forward fan** (yaw offsets ±5°, ±15°) at a fixed shorter trajectory (tilt
  does not scale the spread). Fewer than 4 available ⇒ throws
  `min(4, free slots, cap allowance)` using offsets in the fixed order
  +5°, −5°, +15°, −15°.
- **Set (new input bit 14, edge-triggered, on ground, not while holding):**
  - If a **settled** bomb (any owner) lies within `TUNE_KICK_RANGE` of the
    player and within ±45° of facing: **kick** it — the bomb slides flat at
    `TUNE_KICK_SPEED` along the player's facing.
  - Otherwise: **set** a bomb at the player's feet (SETTLED, full fuse),
    subject to cap and slot availability.
  - One press does exactly one action; kick has priority over set.
- **Sliding bombs detonate on first contact** — boundary wall, pillar,
  player (any, including owner), or another live bomb (chain via blast) — or
  when their fuse expires mid-slide.
- **Live-bomb cap:** `TUNE_MAX_LIVE_BOMBS` 2 → **6** (spread stays reliable
  while bounding spam).
- Unchanged: thrown bombs bounce once then settle with fuse; direct hit on a
  player detonates on impact; blasts chain-detonate and hurt the owner.

## 2. Sim changes (`src/arena/`)

- **`arena_state.h`:**
  - `BSTATE_SLIDING` appended to the bomb state enum (u8 value — **zero
    layout change**, static_asserts untouched, no wire-format bump).
  - Input bit 14 = set/kick: `arena_input_pack(sx, sy, jump, bomb, set)`
    (5th parameter) + `arena_input_set(i)`. All callers updated (tests, CI
    inline hash program, viewer).
- **`arena_sim.c` player phase** (inside the existing fixed player order):
  - Throw block rewritten: on release, tilt-scaled single throw, or spread
    when `p->timer >= TUNE_SPREAD_TICKS`. Spread spawns extra bombs via
    `find_free_bomb` in fixed slot order; `live_bombs` accounting per bomb.
  - New set/kick block after the throw block: kick scan over bombs 0..15 in
    fixed order (first settled bomb in range+cone wins), else set.
- **`arena_sim.c` bomb phase** (existing fixed bomb order):
  - `BSTATE_SLIDING`: integrate XZ at constant velocity (y pinned to 0, no
    gravity); detonate on wall clamp, pillar overlap, player overlap
    (players scanned 0..3), or bomb overlap (bombs scanned 0..15, self
    excluded); fuse continues and detonates at 0.
- **`arena_tuning.h`:** remove `TUNE_THROW_SPEED_2`, `TUNE_THROW_UP_2`,
  `TUNE_CHARGE_TICKS`; `TUNE_THROW_SPEED_1`/`TUNE_THROW_UP_1` become the
  single-throw base values below (all `TODO(feel)`
  placeholders): `TUNE_THROW_SPEED Q(0.18)`, `TUNE_THROW_UP Q(0.12)`,
  `TUNE_THROW_MIN_FRAC Q(0.35)`, `TUNE_SPREAD_TICKS 120`,
  `TUNE_SPREAD_SPEED Q(0.11)`, `TUNE_SPREAD_UP Q(0.10)`,
  `TUNE_KICK_SPEED Q(0.14)`, `TUNE_KICK_RANGE Q(0.9)`,
  `TUNE_KICK_CONE 0x2000` (±45° binary angle), `TUNE_MAX_LIVE_BOMBS 6`.
  **`TUNE_VERSION` 1 → 2.**
- Determinism invariants untouched: no floats, no new state reads, fixed
  iteration orders preserved, padding rules unchanged.

## 3. Verification

- **New `tests/test_bomb_mechanics.c`** (sim-only, deterministic, scripted
  `arena_tick` sequences): tilt scaling (full-tilt throw lands farther than
  neutral-release lob), spread count = 4 and cap/slot clamping (pre-seeded
  live bombs), set places a SETTLED bomb at the feet, kick triggers only in
  range+cone and sends SLIDING, sliding bomb detonates at the wall and on
  player contact, fuse pops mid-slide. Registered with CMake/ctest and the
  one-line gcc build.
- **`tests/test_determinism.c`:** scripted-match input generation gains set
  bits so the new paths are hash-covered. New pinned hash established
  locally (gcc `-O0` and `-O2` must agree), then updated in
  `.github/workflows/determinism.yml` (inline program gets the 5-arg pack +
  set bits). The hash change is intentional and called out in the commit.
- **Viewer:** keyboard `E`, pad EAST → set/kick; help line updated; smoke
  `--frames` hash re-pinned in the plan's verification steps.

## 4. Docs

- `docs/bmhero-battle-arena-design.md` §2 bomb bullet rewritten per §1 with
  a sources footnote; §5 note that spread/kick constants join the A1
  transcription list.
- `CLAUDE.md`: status updated, "pending design correction" paragraph
  removed, next-milestones list drops this item.
- Memory file `hero-bomb-mechanics-verified` retired once the repo records
  the mechanics (repo is the source of truth).

## 5. Risks / notes

- Kick-vs-wall explosion is owner-recalled, sources inconclusive — if A1
  feel-matching shows Hero stops instead, it's a one-line behavior swap
  (detonate → settle) plus hash/TUNE bump.
- All new constants are placeholders pending A1; the spread fan angles and
  arm time in particular will need feel passes.
- Slot pressure: 4 players × cap 6 = 24 potential > 16 bomb slots; spread
  already clamps on free slots, and `find_free_bomb` failing is handled
  (spawn skipped) — same behavior as today, now exercised more often.
