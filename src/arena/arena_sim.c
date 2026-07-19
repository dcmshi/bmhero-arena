/* Battle arena simulation — deterministic 60Hz tick pipeline.
 * Tick order is part of the determinism contract (see design doc §4):
 *   1. phase logic  2. players (0..3)  3. player pushout (fixed pair order)
 *   4. bombs (0..15)  5. detonations & blasts  6. deaths / round end
 * NO floats. NO iteration-order dependence. NO reads outside ArenaState,
 * inputs, and static const data. */
#include <string.h>
#include "arena_sim.h"
#include "arena_tuning.h"
#include "arena_geom.h"

/* ------------------------------------------------------------------ init */

void arena_init(ArenaState* s, uint8_t arena_id, uint8_t num_players, uint32_t seed) {
    memset(s, 0, sizeof(*s));               /* zeroes padding: hash stability */
    s->arena_id    = arena_id;
    s->num_players = num_players;
    s->rng         = seed ? seed : 1u;      /* xorshift must not be 0 */
    s->phase       = PHASE_COUNTDOWN;
    s->phase_timer = TUNE_COUNTDOWN_TICKS;

    const ArenaGeom* g = &arena_geoms[arena_id];
    for (int i = 0; i < num_players; i++) {
        ArenaPlayer* p = &s->players[i];
        p->pos   = g->spawns[i];
        p->hp    = TUNE_START_HP;
        p->state = PSTATE_IDLE;
        p->yaw   = (uint16_t)(iatan2(-p->pos.x, -p->pos.z)); /* face center */
    }
    for (int i = num_players; i < ARENA_MAX_PLAYERS; i++)
        s->players[i].state = PSTATE_DEAD;
}

/* ------------------------------------------------------- collision utils */

static int on_ground(const ArenaPlayer* p) { return p->pos.y <= 0 && p->vel.y <= 0; }

/* Push a vertical-cylinder player (radius r) out of an AABB in XZ if within
 * its Y span. Axis of minimum penetration; ties resolved X-then-Z (fixed). */
static void pushout_aabb(Vec3q* pos, q32 r, const Aabb* b) {
    if (pos->y >= b->max.y || pos->y + TUNE_PLAYER_HEIGHT <= b->min.y) return;
    q32 nx = qclamp(pos->x, b->min.x, b->max.x);
    q32 nz = qclamp(pos->z, b->min.z, b->max.z);
    q32 dx = pos->x - nx, dz = pos->z - nz;
    q32 d  = qlen2(dx, dz);
    if (d >= r) return;
    if (d > 0) {                            /* outside face: push along normal */
        q32 push = r - d;
        pos->x += qmul(qdiv(dx, d), push);
        pos->z += qmul(qdiv(dz, d), push);
    } else {                                /* center inside: min-axis eject */
        q32 lx = pos->x - b->min.x, rx = b->max.x - pos->x;
        q32 lz = pos->z - b->min.z, rz = b->max.z - pos->z;
        q32 mx = qmin(lx, rx), mz = qmin(lz, rz);
        if (mx <= mz) pos->x += (lx < rx) ? -(lx + r) : (rx + r);
        else          pos->z += (lz < rz) ? -(lz + r) : (rz + r);
    }
}

static void collide_static(Vec3q* pos, Vec3q* vel, q32 radius, const ArenaGeom* g,
                           q32 wall_extent) {
    /* floor */
    if (pos->y < 0) { pos->y = 0; if (vel->y < 0) vel->y = 0; }
    /* boundary walls (shrinkable via wall_extent) */
    q32 lim = wall_extent - radius;
    if (pos->x < -lim) { pos->x = -lim; if (vel->x < 0) vel->x = 0; }
    if (pos->x >  lim) { pos->x =  lim; if (vel->x > 0) vel->x = 0; }
    if (pos->z < -lim) { pos->z = -lim; if (vel->z < 0) vel->z = 0; }
    if (pos->z >  lim) { pos->z =  lim; if (vel->z > 0) vel->z = 0; }
    /* pillars */
    for (int i = 0; i < g->num_pillars; i++)
        pushout_aabb(pos, radius, &g->pillars[i]);
}

/* ----------------------------------------------------------- bomb spawn */

static int find_free_bomb(const ArenaState* s) {
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s->bombs[i].state == BSTATE_FREE) return i;
    return -1;
}

static int find_free_blast(const ArenaState* s) {
    for (int i = 0; i < ARENA_MAX_BLASTS; i++)
        if (s->blasts[i].ttl == 0) return i;
    return -1;
}

static void detonate(ArenaState* s, int bi) {
    ArenaBomb* b = &s->bombs[bi];
    int sl = find_free_blast(s);
    if (sl >= 0) {
        ArenaBlast* bl = &s->blasts[sl];
        bl->center   = b->pos;
        bl->radius_t = 0;
        bl->owner    = b->owner;
        bl->ttl      = TUNE_BLAST_TTL;
    }
    if (s->players[b->owner].live_bombs > 0) s->players[b->owner].live_bombs--;
    b->state = BSTATE_FREE;
}

/* Launch bomb b from player pi's hands along binary-angle dir.
 * Fixed launch — no stick or momentum term (decomp 69AA0.c: velocity is
 * moveSpeed x pitch x facing only; jump-throws go farther via HEIGHT). */
static void throw_bomb(ArenaState* s, int pi, ArenaBomb* b, uint16_t dir,
                       q32 fwd, q32 up) {
    const ArenaPlayer* p = &s->players[pi];
    b->pos = p->pos; b->pos.y += TUNE_PLAYER_HEIGHT;
    b->vel.x =  qmul(qsin(dir), fwd);
    b->vel.z = -qmul(qcos(dir), fwd);
    b->vel.y = up;
    b->state   = BSTATE_AIRBORNE;
    b->bounced = 0;
}

/* --------------------------------------------------------- player tick */

static void player_tick(ArenaState* s, int pi, ArenaInput in, const ArenaGeom* g,
                        q32 wall_extent) {
    ArenaPlayer* p = &s->players[pi];
    if (p->state == PSTATE_DEAD) { p->last_input = in; return; }

    int gameplay = (s->phase == PHASE_PLAY || s->phase == PHASE_SUDDEN_DEATH);
    ArenaInput prev = p->last_input;

    /* stick -> world-XZ target velocity (fixed camera: stick maps directly) */
    q32 sx = 0, sz = 0;
    if (gameplay && p->state != PSTATE_TUMBLE) {
        int ix = arena_input_sx(in), iy = arena_input_sy(in);
        q32 mag = qlen2(Q(ix) / AIN_STICK_MAX, Q(iy) / AIN_STICK_MAX);
        if (mag > Q_ONE) mag = Q_ONE;
        if (mag > Q(0.10)) {                       /* deadzone */
            uint16_t dir = iatan2(Q(ix), Q(-iy));  /* stick up = -Z (into screen) */
            q32 spd = qmul(TUNE_RUN_SPEED, mag);
            sx = qmul(qsin(dir), spd);
            sz = -qmul(qcos(dir), spd);
            p->yaw = dir;
        }
    }

    /* accel toward target vel (ground) / drift (air) */
    q32 acc = on_ground(p) ? TUNE_RUN_ACCEL : TUNE_AIR_CONTROL;
    if (sx == 0 && sz == 0 && on_ground(p) && p->state != PSTATE_TUMBLE)
        acc = TUNE_RUN_FRICTION;
    p->vel.x += qclamp(sx - p->vel.x, -acc, acc);
    p->vel.z += qclamp(sz - p->vel.z, -acc, acc);

    /* jump (edge) */
    if (gameplay && p->state != PSTATE_TUMBLE && on_ground(p)
        && arena_input_jump(in) && !arena_input_jump(prev)) {
        p->vel.y = TUNE_JUMP_IMPULSE;
        p->state = PSTATE_JUMP;
    }

    /* bomb grab / hold / throw — single arc, or 4-bomb spread on long hold */
    if (gameplay && p->state != PSTATE_TUMBLE) {
        int bomb_now = arena_input_bomb(in), bomb_prev = arena_input_bomb(prev);
        if (bomb_now && !bomb_prev && p->held_bomb == 0
            && p->live_bombs < TUNE_MAX_LIVE_BOMBS) {
            int bi = find_free_bomb(s);
            if (bi >= 0) {
                ArenaBomb* b = &s->bombs[bi];
                memset(b, 0, sizeof(*b));
                b->owner = (uint8_t)pi;
                b->state = BSTATE_HELD;
                p->held_bomb = (uint8_t)(bi + 1);
                p->live_bombs++;
                p->timer = 0;                       /* hold counter */
            }
        } else if (bomb_now && p->held_bomb) {
            if (p->timer < 0xFFFF) p->timer++;      /* holding (spread arms) */
        } else if (!bomb_now && bomb_prev && p->held_bomb) {
            ArenaBomb* held = &s->bombs[p->held_bomb - 1];
            if (p->timer < TUNE_SPREAD_TICKS) {
                /* single fixed arc along facing */
                throw_bomb(s, pi, held, p->yaw, TUNE_THROW_SPEED, TUNE_THROW_UP);
            } else {
                /* spread: forward fan with Hero's ROM-extracted angle rows
                 * (D_8010C7E4): n=1 {0}, n=2 {-10,+10}, n=3 {0,-20,+20},
                 * n=4 {-10,+10,-30,+30} degrees. A cap/slot-clamped spread
                 * uses the authentic row for its actual count. */
                static const int16_t fan[4][4] = {
                    {       0,      0,       0,      0 },
                    { -0x071C, 0x071C,       0,      0 },
                    {       0, -0xE38,   0xE38,      0 },
                    { -0x071C, 0x071C, -0x1555, 0x1555 },
                };
                int free_slots = 0;
                for (int fi = 0; fi < ARENA_MAX_BOMBS; fi++)
                    if (s->bombs[fi].state == BSTATE_FREE) free_slots++;
                int extras = TUNE_MAX_LIVE_BOMBS - p->live_bombs;
                if (extras > free_slots) extras = free_slots;
                if (extras > 3) extras = 3;
                if (extras < 0) extras = 0;
                const int16_t* row = fan[extras];       /* n = extras + 1 */
                throw_bomb(s, pi, held, (uint16_t)(p->yaw + (uint16_t)row[0]),
                           TUNE_SPREAD_SPEED, TUNE_SPREAD_UP);
                for (int k = 1; k <= extras; k++) {
                    int bi = find_free_bomb(s);
                    if (bi < 0) break;
                    ArenaBomb* nb = &s->bombs[bi];
                    memset(nb, 0, sizeof(*nb));
                    nb->owner = (uint8_t)pi;
                    p->live_bombs++;
                    throw_bomb(s, pi, nb, (uint16_t)(p->yaw + (uint16_t)row[k]),
                               TUNE_SPREAD_SPEED, TUNE_SPREAD_UP);
                }
            }
            p->held_bomb = 0;
            p->timer = 0;
        }
    }

    /* set (edge on bit 14): lay a bomb at the ground under the player.
     * Works mid-air (authentic: Hero speedruns lay bombs in midair). The
     * setter gets grace (bounced = idx+1) so standing on it doesn't
     * immediately walk-in kick it. Kicking = running into a settled bomb
     * (see BSTATE_SETTLED in the bomb phase). */
    if (gameplay && p->state != PSTATE_TUMBLE && p->held_bomb == 0
        && arena_input_set(in) && !arena_input_set(prev)
        && p->live_bombs < TUNE_MAX_LIVE_BOMBS) {
        int bi = find_free_bomb(s);
        if (bi >= 0) {
            ArenaBomb* b = &s->bombs[bi];
            memset(b, 0, sizeof(*b));
            b->owner   = (uint8_t)pi;
            b->state   = BSTATE_SETTLED;
            b->fuse    = TUNE_FUSE_TICKS;
            b->pos     = p->pos; b->pos.y = 0;
            b->bounced = (uint8_t)(pi + 1);     /* setter grace */
            p->live_bombs++;
        }
    }

    /* gravity + integrate */
    p->vel.y -= TUNE_GRAVITY;
    p->pos.x += p->vel.x; p->pos.y += p->vel.y; p->pos.z += p->vel.z;
    collide_static(&p->pos, &p->vel, TUNE_PLAYER_RADIUS, g, wall_extent);

    /* state upkeep */
    if (p->state == PSTATE_TUMBLE) {
        if (p->timer > 0) p->timer--;
        if (p->timer == 0 && on_ground(p)) p->state = PSTATE_IDLE;
    } else if (p->state == PSTATE_JUMP && on_ground(p)) {
        p->state = PSTATE_IDLE;
    } else if (p->state == PSTATE_IDLE || p->state == PSTATE_RUN) {
        p->state = (qlen2(p->vel.x, p->vel.z) > Q(0.01)) ? PSTATE_RUN : PSTATE_IDLE;
        if (p->timer > 0 && p->held_bomb == 0) p->timer--;  /* invuln decays */
    }

    p->last_input = in;
}

/* ------------------------------------------------------------ main tick */

void arena_tick(ArenaState* s, const ArenaInput inputs[ARENA_MAX_PLAYERS]) {
    const ArenaGeom* g = &arena_geoms[s->arena_id];

    /* 1. phase logic */
    q32 wall_extent = g->half_extent;
    switch (s->phase) {
    case PHASE_COUNTDOWN:
        if (s->phase_timer > 0) s->phase_timer--;
        if (s->phase_timer == 0) { s->phase = PHASE_PLAY; s->phase_timer = 0; }
        break;
    case PHASE_PLAY:
        s->phase_timer++;
        if (s->phase_timer >= TUNE_ROUND_TICKS) {
            s->phase = PHASE_SUDDEN_DEATH; s->shrink_step = 0;
        }
        break;
    case PHASE_SUDDEN_DEATH:
        /* walls creep inward: one step every 60 ticks, up to half the arena */
        s->phase_timer++;
        if ((s->phase_timer % 60) == 0 && s->shrink_step < 96) s->shrink_step++;
        break;
    case PHASE_ROUND_END:
        if (s->phase_timer > 0) s->phase_timer--;
        break;
    }
    if (s->phase == PHASE_SUDDEN_DEATH)
        wall_extent -= (q32)s->shrink_step * Q(0.03);

    /* 2. players, fixed order */
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        player_tick(s, i, inputs[i], g, wall_extent);

    /* 3. player-vs-player pushout, fixed pair order */
    for (int a = 0; a < ARENA_MAX_PLAYERS; a++) {
        for (int b = a + 1; b < ARENA_MAX_PLAYERS; b++) {
            ArenaPlayer *pa = &s->players[a], *pb = &s->players[b];
            if (pa->state == PSTATE_DEAD || pb->state == PSTATE_DEAD) continue;
            q32 dx = pb->pos.x - pa->pos.x, dz = pb->pos.z - pa->pos.z;
            q32 d = qlen2(dx, dz), min_d = 2 * TUNE_PLAYER_RADIUS;
            if (d >= min_d) continue;
            q32 push = (min_d - d) / 2;
            q32 ux, uz;
            if (d > 0) { ux = qdiv(dx, d); uz = qdiv(dz, d); }
            else       { ux = Q_ONE; uz = 0; }      /* exact overlap: fixed axis */
            pa->pos.x -= qmul(ux, push); pa->pos.z -= qmul(uz, push);
            pb->pos.x += qmul(ux, push); pb->pos.z += qmul(uz, push);
        }
    }

    /* 4. bombs, fixed order */
    for (int i = 0; i < ARENA_MAX_BOMBS; i++) {
        ArenaBomb* b = &s->bombs[i];
        switch (b->state) {
        case BSTATE_HELD: {
            const ArenaPlayer* p = &s->players[b->owner];
            b->pos = p->pos; b->pos.y += TUNE_PLAYER_HEIGHT;
            break;
        }
        case BSTATE_AIRBORNE: {
            b->vel.y -= TUNE_GRAVITY;
            b->pos.x += b->vel.x; b->pos.y += b->vel.y; b->pos.z += b->vel.z;
            /* direct hit on a player -> detonate on impact */
            for (int pi = 0; pi < s->num_players; pi++) {
                const ArenaPlayer* p = &s->players[pi];
                if (pi == b->owner && !b->bounced) continue;  /* grace vs self */
                if (p->state == PSTATE_DEAD) continue;
                Vec3q d = { b->pos.x - p->pos.x,
                            b->pos.y - (p->pos.y + TUNE_PLAYER_HEIGHT / 2),
                            b->pos.z - p->pos.z };
                if (qlen3(d) < TUNE_PLAYER_RADIUS + TUNE_BOMB_RADIUS) {
                    b->state = BSTATE_EXPLODING;
                    break;
                }
            }
            if (b->state != BSTATE_AIRBORNE) break;
            /* walls stop bombs */
            {
                Vec3q v = b->vel;
                collide_static(&b->pos, &v, TUNE_BOMB_RADIUS, g, wall_extent);
                b->vel.x = v.x; b->vel.z = v.z;
            }
            /* floor bounce */
            if (b->pos.y <= 0 && b->vel.y < 0) {
                if (!b->bounced) {
                    b->pos.y = 0;
                    b->vel.y = qmul(-b->vel.y, TUNE_BOMB_RESTITUTION);
                    b->vel.x = qmul(b->vel.x, TUNE_BOMB_H_DAMP);
                    b->vel.z = qmul(b->vel.z, TUNE_BOMB_H_DAMP);
                    b->bounced = 1;
                } else {
                    b->pos.y   = 0;
                    b->state   = BSTATE_SETTLED;
                    b->fuse    = TUNE_FUSE_TICKS;
                    b->bounced = 0;   /* bounce flag done; field now = kick grace */
                }
            }
            break;
        }
        case BSTATE_SLIDING: {
            b->pos.x += b->vel.x; b->pos.z += b->vel.z;
            /* players: contact detonates (kicker skipped until clear) */
            for (int pj = 0; pj < s->num_players; pj++) {
                const ArenaPlayer* p = &s->players[pj];
                if (p->state == PSTATE_DEAD) continue;
                if (p->pos.y > 2 * TUNE_BOMB_RADIUS) continue;   /* jumped over */
                q32 dist = qlen2(b->pos.x - p->pos.x, b->pos.z - p->pos.z);
                q32 touch = TUNE_PLAYER_RADIUS + TUNE_BOMB_RADIUS;
                if (b->bounced == (uint8_t)(pj + 1)) {           /* kicker grace */
                    if (dist >= touch + Q(0.1)) b->bounced = 0;
                    continue;
                }
                if (dist < touch) { b->state = BSTATE_EXPLODING; break; }
            }
            if (b->state != BSTATE_SLIDING) break;
            /* other live ground bombs: contact detonates (chain) */
            for (int bj = 0; bj < ARENA_MAX_BOMBS; bj++) {
                const ArenaBomb* ob = &s->bombs[bj];
                if (bj == i) continue;
                if (ob->state != BSTATE_SETTLED && ob->state != BSTATE_SLIDING)
                    continue;
                if (qlen2(b->pos.x - ob->pos.x, b->pos.z - ob->pos.z)
                    < 2 * TUNE_BOMB_RADIUS) {
                    b->state = BSTATE_EXPLODING;
                    break;
                }
            }
            if (b->state != BSTATE_SLIDING) break;
            /* walls / pillars: any pushback = contact = detonate */
            {
                Vec3q pre_p = b->pos, pre_v = b->vel;
                collide_static(&b->pos, &b->vel, TUNE_BOMB_RADIUS, g, wall_extent);
                if (b->pos.x != pre_p.x || b->pos.z != pre_p.z
                    || b->vel.x != pre_v.x || b->vel.z != pre_v.z)
                    b->state = BSTATE_EXPLODING;
            }
            /* fuse keeps burning while sliding */
            if (b->state == BSTATE_SLIDING) {
                if (b->fuse > 0) b->fuse--;
                if (b->fuse == 0) b->state = BSTATE_EXPLODING;
            }
            break;
        }
        case BSTATE_SETTLED: {
            /* walk-in kick: a moving grounded player touching the bomb sends
             * it sliding along their facing. Setter grace (bounced = idx+1)
             * holds until they step clear, so setting isn't an insta-kick. */
            q32 touch = TUNE_PLAYER_RADIUS + TUNE_BOMB_RADIUS;
            for (int pj = 0; pj < s->num_players; pj++) {   /* fixed order */
                ArenaPlayer* p = &s->players[pj];
                if (p->state == PSTATE_DEAD || p->state == PSTATE_TUMBLE) continue;
                q32 dist = qlen2(b->pos.x - p->pos.x, b->pos.z - p->pos.z);
                if (b->bounced == (uint8_t)(pj + 1)) {      /* setter grace */
                    if (dist >= touch + Q(0.1)) b->bounced = 0;
                    continue;
                }
                if (dist >= touch) continue;
                if (p->pos.y > TUNE_PLAYER_HEIGHT / 2) continue;  /* jumped over */
                if (qlen2(p->vel.x, p->vel.z) < TUNE_KICK_MIN_VEL) continue;
                b->state   = BSTATE_SLIDING;
                b->pos.y   = 0;
                b->vel.x   =  qmul(qsin(p->yaw), TUNE_KICK_SPEED);
                b->vel.y   = 0;
                b->vel.z   = -qmul(qcos(p->yaw), TUNE_KICK_SPEED);
                b->bounced = (uint8_t)(pj + 1);             /* kicker grace */
                break;
            }
            if (b->state != BSTATE_SETTLED) break;
            if (b->fuse > 0) b->fuse--;
            if (b->fuse == 0) b->state = BSTATE_EXPLODING;
            break;
        }
        default: break;
        }
    }

    /* 5. detonations, then blast update (grow, chain, damage) */
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s->bombs[i].state == BSTATE_EXPLODING) detonate(s, i);

    for (int i = 0; i < ARENA_MAX_BLASTS; i++) {
        ArenaBlast* bl = &s->blasts[i];
        if (bl->ttl == 0) continue;
        bl->ttl--;
        if (bl->radius_t < TUNE_BLAST_GROW_TICKS) bl->radius_t++;
        q32 radius = (q32)(((int64_t)TUNE_BLAST_RADIUS * bl->radius_t) / TUNE_BLAST_GROW_TICKS);

        /* chain-detonate bombs in radius (next pass picks up EXPLODING) */
        for (int bi = 0; bi < ARENA_MAX_BOMBS; bi++) {
            ArenaBomb* b = &s->bombs[bi];
            if (b->state != BSTATE_SETTLED && b->state != BSTATE_AIRBORNE
                && b->state != BSTATE_SLIDING) continue;
            Vec3q d = { b->pos.x - bl->center.x, b->pos.y - bl->center.y,
                        b->pos.z - bl->center.z };
            if (qlen3(d) < radius + TUNE_BOMB_RADIUS) b->state = BSTATE_EXPLODING;
        }

        /* damage players (owner included). invuln timer gates re-hits. */
        for (int pi = 0; pi < s->num_players; pi++) {
            ArenaPlayer* p = &s->players[pi];
            if (p->state == PSTATE_DEAD || p->hp == 0) continue;
            if (p->state != PSTATE_TUMBLE && p->timer > 0) continue;  /* invuln */
            if (p->state == PSTATE_TUMBLE) continue;
            Vec3q d = { p->pos.x - bl->center.x,
                        (p->pos.y + TUNE_PLAYER_HEIGHT / 2) - bl->center.y,
                        p->pos.z - bl->center.z };
            q32 dist = qlen3(d);
            if (dist >= radius + TUNE_PLAYER_RADIUS) continue;
            p->hp--;
            q32 ux = Q_ONE, uz = 0;
            q32 dxz = qlen2(d.x, d.z);
            if (dxz > 0) { ux = qdiv(d.x, dxz); uz = qdiv(d.z, dxz); }
            p->vel.x = qmul(ux, TUNE_KNOCKBACK);
            p->vel.z = qmul(uz, TUNE_KNOCKBACK);
            p->vel.y = TUNE_KNOCKBACK_UP;
            p->state = PSTATE_TUMBLE;
            p->timer = (uint16_t)(TUNE_TUMBLE_TICKS + TUNE_INVULN_TICKS);
            /* drop held bomb in place */
            if (p->held_bomb) {
                ArenaBomb* hb = &s->bombs[p->held_bomb - 1];
                hb->state = BSTATE_SETTLED; hb->fuse = TUNE_FUSE_TICKS;
                hb->pos = p->pos; hb->pos.y = 0;
                hb->vel.x = hb->vel.y = hb->vel.z = 0;
                hb->bounced = (uint8_t)(pi + 1);   /* grace vs the hit player */
                p->held_bomb = 0;
            }
            if (p->hp == 0) p->state = PSTATE_DEAD;
        }
    }

    /* 6. round end */
    if (s->phase == PHASE_PLAY || s->phase == PHASE_SUDDEN_DEATH) {
        int alive = 0, last = -1;
        for (int i = 0; i < s->num_players; i++)
            if (s->players[i].state != PSTATE_DEAD) { alive++; last = i; }
        if (alive <= 1) {
            if (last >= 0) s->players[last].stocks_won++;
            s->phase = PHASE_ROUND_END;
            s->phase_timer = TUNE_ROUND_END_TICKS;
        }
    }

    s->tick++;
}
