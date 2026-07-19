/* ArenaState — the entire battle-mode simulation state.
 * POD, fixed-size, no pointers, no heap. The struct IS the snapshot format
 * and the wire format: memcpy to save, memcpy to restore.
 * Layout is locked by static_asserts below; changing it is a netcode-version bump. */
#ifndef ARENA_STATE_H
#define ARENA_STATE_H

#include <stdint.h>
#include <assert.h>
#include "arena_math.h"

#define ARENA_MAX_PLAYERS 4
#define ARENA_MAX_BOMBS   16
#define ARENA_MAX_BLASTS  16

/* ---- input: 16 bits per player per tick ----
 * bits 0-5  : stick X, signed 6-bit stored offset-by-32 (1..63, 32 = neutral; 0 unused)
 * bits 6-11 : stick Y, same encoding
 * bit 12    : jump
 * bit 13    : bomb (hold to grab, release to throw; long hold arms the spread)
 * bit 14    : set / kick (edge-triggered)
 * Quantizing analog to 6 bits bounds prediction entropy and packet size. */
typedef uint16_t ArenaInput;

#define AIN_STICK_MAX 31

static inline ArenaInput arena_input_pack(int sx, int sy, int jump, int bomb, int set) {
    if (sx < -AIN_STICK_MAX) sx = -AIN_STICK_MAX;
    if (sx >  AIN_STICK_MAX) sx =  AIN_STICK_MAX;
    if (sy < -AIN_STICK_MAX) sy = -AIN_STICK_MAX;
    if (sy >  AIN_STICK_MAX) sy =  AIN_STICK_MAX;
    return (ArenaInput)(((sx + 32) & 63) | (((sy + 32) & 63) << 6)
                        | ((jump ? 1 : 0) << 12) | ((bomb ? 1 : 0) << 13)
                        | ((set ? 1 : 0) << 14));
}
static inline int arena_input_sx(ArenaInput i)   { return (int)(i & 63) - 32; }
static inline int arena_input_sy(ArenaInput i)   { return (int)((i >> 6) & 63) - 32; }
static inline int arena_input_jump(ArenaInput i) { return (i >> 12) & 1; }
static inline int arena_input_bomb(ArenaInput i) { return (i >> 13) & 1; }
static inline int arena_input_set(ArenaInput i)  { return (i >> 14) & 1; }

/* ---- enums (u8 in state) ---- */
enum { PSTATE_IDLE, PSTATE_RUN, PSTATE_JUMP, PSTATE_TUMBLE, PSTATE_DEAD };
enum { BSTATE_FREE, BSTATE_HELD, BSTATE_AIRBORNE, BSTATE_SETTLED, BSTATE_EXPLODING };
enum { PHASE_COUNTDOWN, PHASE_PLAY, PHASE_SUDDEN_DEATH, PHASE_ROUND_END };

typedef struct {                 /* 44 bytes */
    Vec3q    pos;                /* 12 */
    Vec3q    vel;                /* 12 */
    uint16_t yaw;                /* facing, binary angle */
    uint8_t  state;              /* PSTATE_* */
    uint8_t  hp;
    uint16_t timer;              /* invuln / tumble / bomb-hold, per state */
    uint8_t  stocks_won;
    uint8_t  held_bomb;          /* bomb index + 1, 0 = none */
    uint8_t  live_bombs;
    uint8_t  pad0;
    uint16_t last_input;         /* previous tick's input (edge detection) */
    uint32_t anim_hint;          /* render-bridge hint; still deterministic state */
} ArenaPlayer;

typedef struct {                 /* 32 bytes */
    Vec3q    pos;                /* 12 */
    Vec3q    vel;                /* 12 */
    uint8_t  owner;              /* player index */
    uint8_t  state;              /* BSTATE_* */
    uint16_t fuse;               /* ticks until detonation once SETTLED */
    uint8_t  bounced;
    uint8_t  pad0[3];
} ArenaBomb;

typedef struct {                 /* 16 bytes */
    Vec3q    center;             /* 12 */
    uint16_t radius_t;           /* growth progress 0..blast_ttl */
    uint8_t  owner;
    uint8_t  ttl;                /* ticks remaining */
} ArenaBlast;

typedef struct {
    uint32_t    tick;
    uint32_t    rng;             /* xorshift32 — the only RNG in the sim */
    uint8_t     round;
    uint8_t     phase;           /* PHASE_* */
    uint16_t    phase_timer;
    uint8_t     arena_id;
    uint8_t     num_players;
    uint16_t    shrink_step;     /* sudden-death progress */
    ArenaPlayer players[ARENA_MAX_PLAYERS];
    ArenaBomb   bombs[ARENA_MAX_BOMBS];
    ArenaBlast  blasts[ARENA_MAX_BLASTS];
} ArenaState;

static_assert(sizeof(ArenaPlayer) == 40,  "ArenaPlayer layout drifted");
static_assert(sizeof(ArenaBomb)   == 32,  "ArenaBomb layout drifted");
static_assert(sizeof(ArenaBlast)  == 16,  "ArenaBlast layout drifted");
static_assert(sizeof(ArenaState)  == 16 + 4*40 + 16*32 + 16*16,
              "ArenaState layout drifted"); /* 944 bytes */

/* xorshift32. Advances state->rng. Never call outside the tick pipeline. */
static inline uint32_t arena_rand(ArenaState* s) {
    uint32_t x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x;
    return x;
}

/* FNV-1a over the whole state — desync checksum. Padding is zeroed at init and
 * only whole-struct memcpys are used, so padding bytes are stable. */
static inline uint32_t arena_hash(const ArenaState* s) {
    const uint8_t* p = (const uint8_t*)s;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < sizeof(ArenaState); i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

#endif
