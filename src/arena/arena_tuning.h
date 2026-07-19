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

/* -- movement -- TODO(feel): transcribe from decomp player update */
#define TUNE_RUN_SPEED       Q(0.085)   /* max ground speed per tick */
#define TUNE_RUN_ACCEL       Q(0.010)   /* toward target vel per tick */
#define TUNE_RUN_FRICTION    Q(0.008)   /* decel when stick neutral */
#define TUNE_AIR_CONTROL     Q(0.004)   /* accel while airborne */
#define TUNE_JUMP_IMPULSE    Q(0.140)   /* TODO(feel) */
#define TUNE_GRAVITY         Q(0.0075)  /* TODO(feel) */
#define TUNE_PLAYER_RADIUS   Q(0.35)
#define TUNE_PLAYER_HEIGHT   Q(1.0)

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
#define TUNE_VERSION         2

#endif
