/* Fixed-point math for the arena sim. Q20.12 in int32_t.
 * Rules: NO floats anywhere in the sim. All products via int64_t then shift.
 * Part of the determinism contract — do not "optimize" into float. */
#ifndef ARENA_MATH_H
#define ARENA_MATH_H

#include <stdint.h>

#define Q_SHIFT 12
#define Q_ONE   (1 << Q_SHIFT)           /* 1.0  */
#define Q(x)    ((int32_t)((x) * Q_ONE)) /* compile-time constants only (int result) */

typedef int32_t q32;

typedef struct { q32 x, y, z; } Vec3q;   /* 12 bytes */

static inline q32 qmul(q32 a, q32 b) { return (q32)(((int64_t)a * (int64_t)b) >> Q_SHIFT); }
static inline q32 qdiv(q32 a, q32 b) { return (q32)(((int64_t)a << Q_SHIFT) / (int64_t)b); }

static inline q32 qmin(q32 a, q32 b) { return a < b ? a : b; }
static inline q32 qmax(q32 a, q32 b) { return a > b ? a : b; }
static inline q32 qabs(q32 a)        { return a < 0 ? -a : a; }
static inline q32 qclamp(q32 v, q32 lo, q32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Integer sqrt of a uint64, returns floor(sqrt(v)). Deterministic bit-shift method. */
static inline uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* Length of (x,z) in Q20.12. */
static inline q32 qlen2(q32 x, q32 z) {
    uint64_t s = (uint64_t)((int64_t)x * x) + (uint64_t)((int64_t)z * z);
    return (q32)isqrt64(s); /* sqrt(Q24) = Q12: shifts cancel */
}

static inline q32 qlen3(Vec3q v) {
    uint64_t s = (uint64_t)((int64_t)v.x * v.x) + (uint64_t)((int64_t)v.y * v.y)
               + (uint64_t)((int64_t)v.z * v.z);
    return (q32)isqrt64(s);
}

/* Binary angle: u16, 65536 == full turn. */
#include "arena_sintab.h"
static inline q32 qsin(uint16_t a) { return arena_sintab[a >> 8]; }
static inline q32 qcos(uint16_t a) { return arena_sintab[(uint16_t)(a + 0x4000) >> 8]; }

/* Integer atan2 -> binary angle. Octant decomposition + linear approx on the
 * ratio; error < ~1 deg, plenty for facing/render. Integer-only. */
static inline uint16_t iatan2(q32 y, q32 x) {
    if (x == 0 && y == 0) return 0;
    q32 ax = qabs(x), ay = qabs(y);
    /* t in [0, Q_ONE]: min/max ratio */
    q32 t = (ax > ay) ? qdiv(ay, ax) : qdiv(ax, ay);
    /* angle in [0, 0x2000] (0..45deg): a = t * (0x2000 - (0x0500 * (Q_ONE - t) * t >> 12)) —
       simple corrected linear fit, integer-only. */
    q32 corr = qmul(qmul(Q(0.3125), Q_ONE - t), t);   /* 0x0500/0x4000 = 0.3125 rad-ish fit */
    uint16_t a = (uint16_t)((((int64_t)t * (0x2000 - ((int64_t)corr * 0x2000 >> Q_SHIFT))) >> Q_SHIFT));
    if (ax <= ay) a = (uint16_t)(0x4000 - a);
    if (x < 0)    a = (uint16_t)(0x8000 - a);
    if (y < 0)    a = (uint16_t)(-a);
    return a;
}

#endif
