/* Tuning table v0 — every gameplay constant in one place.
 * STATUS: placeholder values chosen for sane behavior. Each marked TODO(feel)
 * must be transcribed from the bomberhackers/bmhero decomp (or measured
 * empirically in the recomp) during A1 feel-matching.
 * This table is hashed into the netcode version handshake: peers with
 * different tuning cannot play together. */
#ifndef ARENA_TUNING_H
#define ARENA_TUNING_H

#include "arena_math.h"

/* Units: 1.0q ~= 1 world unit; ticks are 1/60s. */

/* -- movement -- A1.3: transcribed from docs/bmhero-player-movement-re.md
 * ## Speed (60 Hz => Hz factor 1.0; height anchor S=0.0084034; q = game_u/frame x S). */
#define TUNE_RUN_SPEED       Q(0.084)   /* game top 10 u/f x S */
#define TUNE_RUN_ACCEL       Q(0.00168) /* game 0.2 u/f x S — ~7x lower than the old placeholder = the momentum feel */
#define TUNE_RUN_FRICTION    Q(0.00168) /* game friction == accel (single 0.2 u/f rate) */
#define TUNE_JUMP_IMPULSE    Q(0.280)    /* game 33.333 u/f x S (was Q(0.140)) */
#define TUNE_GRAVITY         Q(0.0175)   /* game 2.0833 u/f^2 x S (was Q(0.0075)) */
#define TUNE_TERMINAL_VY     Q(-0.403)   /* game -48 u/f x S (NEW — sim had no clamp) */
#define TUNE_AIR_CONTROL     Q(0.00168)  /* = TUNE_RUN_ACCEL: full air control per RE (low-confidence/empirical) */
#define TUNE_PLAYER_RADIUS   Q(0.35)
#define TUNE_PLAYER_HEIGHT   Q(1.0)
#define TUNE_TURN_RATE       0x0889     /* binary-angle units/tick (~12deg/frame). EMPIRICAL seed:
                                         * campaign turn rate is undecompiled asm (movement-re.md
                                         * ## Turn). Finalized by feel; structure is transcribed,
                                         * rate is not. */

/* -- bombs -- TODO(feel): calibrate against decomp bmhero src/code/69AA0.c
 * during A1. Verified there: throw is a FIXED launch (pitch 80deg, speed 35,
 * dir = facing; no stick/momentum term), kicked/rolled bombs go flat at
 * speed 30 (throw:kick = 7:6), bomb gravity 2.0/frame, terminal -48 (30Hz
 * world units — unit scale needs the player run-speed constant).
 * Kick-vs-wall detonation still owner-recalled; confirm in the recomp. */
#define TUNE_THROW_SPEED     Q(0.18)    /* fixed arc, forward component */
#define TUNE_THROW_UP        Q(0.12)
#define TUNE_SPREAD_TICKS    120        /* hold >= this arms the 4-bomb spread */
/* Spread launch, ROM-extracted (table D_8010C7E4 @ ROM 0xFED04, used by
 * 69AA0.c func_8007A488/func_8007A620): Hero spread = speed 28 pitch 30deg
 * (vs single throw 35 @ 80deg) — flat and quick. Fan rows are in
 * arena_sim.c. Magnitudes below are feel-scaled, ratio keeps the 30deg
 * pitch (up/fwd = tan30). Alt table bank (speed 60, half angles) looks
 * like a powerup variant — v2 items territory. */
#define TUNE_SPREAD_SPEED    Q(0.095)   /* forward component */
#define TUNE_SPREAD_UP       Q(0.055)   /* 30deg pitch ratio */
#define TUNE_KICK_SPEED      Q(0.14)
#define TUNE_KICK_MIN_VEL    Q(0.02)    /* walk-in kick needs real movement */
#define TUNE_BOMB_RADIUS     Q(0.30)
#define TUNE_BOMB_RESTITUTION Q(0.40)   /* single bounce */
#define TUNE_BOMB_H_DAMP     Q(0.55)    /* horizontal damping on bounce */
#define TUNE_FUSE_TICKS      150        /* settled -> boom */
#define TUNE_MAX_LIVE_BOMBS  6          /* raised from 2: spread stays reliable */

/* -- blasts -- */
#define TUNE_BLAST_RADIUS    Q(1.60)    /* full radius */
#define TUNE_BLAST_TTL       20         /* ticks alive, radius grows over first 12 */
#define TUNE_BLAST_GROW_TICKS 12
#define TUNE_KNOCKBACK       Q(0.16)
#define TUNE_KNOCKBACK_UP    Q(0.10)
#define TUNE_INVULN_TICKS    60
#define TUNE_TUMBLE_TICKS    30

/* -- match rules -- */
#define TUNE_START_HP        2
#define TUNE_ROUND_TICKS     (120 * 60) /* 2:00 */
#define TUNE_COUNTDOWN_TICKS (3 * 60)
#define TUNE_ROUND_END_TICKS (3 * 60)
#define TUNE_ROUNDS_TO_WIN   3

/* Bump when any value changes; folded into the session version hash. */
#define TUNE_VERSION         3      /* A1.3: movement dynamics transcribed (was 2) */

#endif
