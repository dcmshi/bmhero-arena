/* Tuning table v0 — every gameplay constant in one place.
 * STATUS: placeholder values chosen for sane behavior. Each marked TODO(feel)
 * must be transcribed from the bomberhackers/bmhero decomp (or measured
 * empirically in the recomp) during A1 feel-matching.
 * This table is hashed into the netcode version handshake: peers with
 * different tuning cannot play together.
 *
 * Every value is wrapped in #ifndef so a build can override it from the command
 * line (-DTUNE_RUN_FRICTION='Q(0.020)'). That is what lets tools/tune-report.ps1
 * sweep variants without editing this file. Defaults are unchanged by the
 * guards — overriding is opt-in, per translation unit. */
#ifndef ARENA_TUNING_H
#define ARENA_TUNING_H

#include "arena_math.h"

/* Units: 1.0q ~= 1 world unit; ticks are 1/60s. */

/* -- movement -- A1.3: transcribed from docs/bmhero-player-movement-re.md
 * ## Speed (60 Hz => Hz factor 1.0; height anchor S=0.0084034; q = game_u/frame x S).
 * 2026-07-24: horizontal constants RE-DERIVED from the REAL standard walker
 * (code_extra_0, recovered from RecompiledFuncs — movement-re.md UPDATE block).
 * A1.3 shipped the auto-runner's numbers (10 / 0.2); the real walker is
 * snappier: top 18, accel 1.5 (~12 frames to top vs ~50), air accel 1.0.
 * Stick->target-speed stays continuous mag-scaling (the doc's sanctioned
 * approximation of the game's discrete 0/6/12/18 tiers). */
#ifndef TUNE_RUN_SPEED
#define TUNE_RUN_SPEED       Q(0.151)   /* game top 18 u/f x S (real walker; was auto-runner 10 -> Q(0.084)) */
#endif
#ifndef TUNE_RUN_ACCEL
#define TUNE_RUN_ACCEL       Q(0.0126)  /* game 1.5 u/f x S (real walker; was 0.2 -> Q(0.00168)) */
#endif
#ifndef TUNE_RUN_FRICTION
#define TUNE_RUN_FRICTION    Q(0.030)   /* A1.4 feel: DECOUPLED from accel (~2.4x) to cut the
                                         * "slidey" coast on stick-release (was Q(0.0126)=accel,
                                         * the authentic single-rate -> ~0.2s coast; now ~0.08s).
                                         * Momentum ramp-up (accel) kept; stop is snappier. FEEL knob. */
#endif
#ifndef TUNE_JUMP_IMPULSE
#define TUNE_JUMP_IMPULSE    Q(0.280)    /* game 33.333 u/f x S (was Q(0.140)) */
#endif
#ifndef TUNE_GRAVITY
#define TUNE_GRAVITY         Q(0.0175)   /* game 2.0833 u/f^2 x S (was Q(0.0075)) */
#endif
#ifndef TUNE_TERMINAL_VY
#define TUNE_TERMINAL_VY     Q(-0.403)   /* game -48 u/f x S (NEW — sim had no clamp) */
#endif
#ifndef TUNE_AIR_CONTROL
#define TUNE_AIR_CONTROL     Q(0.0084)   /* game air accel 1.0 u/f x S = 67% of ground (real walker; was Q(0.00168)) */
#endif
#ifndef TUNE_PLAYER_RADIUS
#define TUNE_PLAYER_RADIUS   Q(0.35)
#endif
#ifndef TUNE_PLAYER_HEIGHT
#define TUNE_PLAYER_HEIGHT   Q(1.0)
#endif
#ifndef TUNE_TURN_RATE
#define TUNE_TURN_RATE       0x02D8     /* 728 = 4.0deg/frame, bounded. RECOVERED from the real
                                         * standard walker (code_extra_0 func_80281E50, step
                                         * 0x40800000; RecompiledFuncs, movement-re.md ## Turn +
                                         * UPDATE). Was 0x0889 (~12deg) empirical seed — the real
                                         * rate is 3x slower (more turn momentum). */
#endif

/* -- bombs -- TODO(feel): calibrate against decomp bmhero src/code/69AA0.c
 * during A1. Verified there: throw is a FIXED launch (pitch 80deg, speed 35,
 * dir = facing; no stick/momentum term), kicked/rolled bombs go flat at
 * speed 30 (throw:kick = 7:6), bomb gravity 2.0/frame, terminal -48 (30Hz
 * world units — unit scale needs the player run-speed constant).
 * Kick-vs-wall detonation still owner-recalled; confirm in the recomp. */
#ifndef TUNE_THROW_SPEED
#define TUNE_THROW_SPEED     Q(0.18)    /* fixed arc, forward component */
#endif
#ifndef TUNE_THROW_UP
#define TUNE_THROW_UP        Q(0.12)
#endif
#ifndef TUNE_SPREAD_TICKS
#define TUNE_SPREAD_TICKS    120        /* hold >= this arms the 4-bomb spread */
#endif
/* Spread launch, ROM-extracted (table D_8010C7E4 @ ROM 0xFED04, used by
 * 69AA0.c func_8007A488/func_8007A620): Hero spread = speed 28 pitch 30deg
 * (vs single throw 35 @ 80deg) — flat and quick. Fan rows are in
 * arena_sim.c. Magnitudes below are feel-scaled, ratio keeps the 30deg
 * pitch (up/fwd = tan30). Alt table bank (speed 60, half angles) looks
 * like a powerup variant — v2 items territory. */
#ifndef TUNE_SPREAD_SPEED
#define TUNE_SPREAD_SPEED    Q(0.095)   /* forward component */
#endif
#ifndef TUNE_SPREAD_UP
#define TUNE_SPREAD_UP       Q(0.055)   /* 30deg pitch ratio */
#endif
#ifndef TUNE_KICK_SPEED
#define TUNE_KICK_SPEED      Q(0.14)
#endif
#ifndef TUNE_KICK_MIN_VEL
#define TUNE_KICK_MIN_VEL    Q(0.02)    /* walk-in kick needs real movement */
#endif
#ifndef TUNE_BOMB_RADIUS
#define TUNE_BOMB_RADIUS     Q(0.30)
#endif
#ifndef TUNE_BOMB_RESTITUTION
#define TUNE_BOMB_RESTITUTION Q(0.40)   /* single bounce */
#endif
#ifndef TUNE_BOMB_H_DAMP
#define TUNE_BOMB_H_DAMP     Q(0.55)    /* horizontal damping on bounce */
#endif
#ifndef TUNE_FUSE_TICKS
#define TUNE_FUSE_TICKS      150        /* settled -> boom */
#endif
#ifndef TUNE_MAX_LIVE_BOMBS
#define TUNE_MAX_LIVE_BOMBS  6          /* raised from 2: spread stays reliable */
#endif

/* -- blasts -- */
#ifndef TUNE_BLAST_RADIUS
#define TUNE_BLAST_RADIUS    Q(1.60)    /* full radius */
#endif
#ifndef TUNE_BLAST_TTL
#define TUNE_BLAST_TTL       20         /* ticks alive, radius grows over first 12 */
#endif
#ifndef TUNE_BLAST_GROW_TICKS
#define TUNE_BLAST_GROW_TICKS 12
#endif
#ifndef TUNE_KNOCKBACK
#define TUNE_KNOCKBACK       Q(0.16)
#endif
#ifndef TUNE_KNOCKBACK_UP
#define TUNE_KNOCKBACK_UP    Q(0.10)
#endif
#ifndef TUNE_INVULN_TICKS
#define TUNE_INVULN_TICKS    60
#endif
#ifndef TUNE_TUMBLE_TICKS
#define TUNE_TUMBLE_TICKS    30
#endif

/* -- match rules -- */
#ifndef TUNE_START_HP
#define TUNE_START_HP        2
#endif
#ifndef TUNE_ROUND_TICKS
#define TUNE_ROUND_TICKS     (120 * 60) /* 2:00 */
#endif
#ifndef TUNE_COUNTDOWN_TICKS
#define TUNE_COUNTDOWN_TICKS (3 * 60)
#endif
#ifndef TUNE_ROUND_END_TICKS
#define TUNE_ROUND_END_TICKS (3 * 60)
#endif
#ifndef TUNE_ROUNDS_TO_WIN
#define TUNE_ROUNDS_TO_WIN   3
#endif

/* Bump when any value changes; folded into the session version hash. */
#ifndef TUNE_VERSION
#define TUNE_VERSION         6      /* 2026-07-24: (v6) TUNE_RUN_FRICTION decoupled to Q(0.030)
                                     * (cut the slidey coast on release); (v4) real code_extra_0
                                     * walker constants (turn 4deg, top 18, accel 1.5, air 1.0); (v5) arena0 ->
                                     * rectangular Nitros-matched geom (half_x/half_z, no pillars,
                                     * corner spawns) so the sim bounds track the rendered map
                                     * (integration notes 8.5a). Was 3 (A1.3 auto-runner). */
#endif

#endif
