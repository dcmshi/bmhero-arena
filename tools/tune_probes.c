#include <string.h>
#include "tune_probes.h"
#include "../src/arena/arena_sim.h"
#include "../src/arena/arena_tuning.h"
#include "../src/arena/arena_geom.h"

#define TICK_HZ      60.0
#define CAP_TICKS    600      /* every probe's iteration ceiling */
#define SETTLE_TICKS 5        /* consecutive near-zero deltas => converged */
#define SETTLE_EPS   Q(0.0005)

static double q_to_f(q32 q) { return (double)q / (double)Q_ONE; }

/* Fresh match, player 0 at arena CENTRE, player 1 parked far away.
 * - num_players MUST be 2: with 1, the liveness check (alive<=1) flips phase to
 *   PHASE_ROUND_END on tick 1 and gates off all movement (test_movement.c:11).
 * - phase forced to PHASE_PLAY to skip the countdown.
 * - centre, not the spawn: spawn 0 is {-7.8,0,-3.8}, hard against two walls of a
 *   7.9 x 3.87 arena; a 180 turn at top speed sweeps ~6.8u and would hit a wall,
 *   contaminating the measurement. */
static void probe_init(ArenaState* s, ArenaInput in[ARENA_MAX_PLAYERS]) {
    arena_init(s, 0, 2, 1);
    s->phase = PHASE_PLAY;
    s->players[0].pos.x = 0; s->players[0].pos.y = 0; s->players[0].pos.z = 0;
    s->players[1].pos.x = Q(7.0); s->players[1].pos.y = 0; s->players[1].pos.z = Q(3.0);
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        in[i] = arena_input_pack(0, 0, 0, 0, 0);   /* neutral is 0x820, NOT raw 0 */
}

static q32 speed_of(const ArenaState* s) {
    return qlen2(s->players[0].vel.x, s->players[0].vel.z);
}

/* Drive full stick +X until speed converges. Returns top speed (q32/tick).
 * Leaves `s` at top speed so the caller can chain a probe onto it.
 * +X because the arena is wider on X (half_x 7.9 vs half_z 3.87). */
static q32 drive_to_top(ArenaState* s, ArenaInput in[ARENA_MAX_PLAYERS], int* capped) {
    in[0] = arena_input_pack(31, 0, 0, 0, 0);
    q32 prev = 0; int stable = 0; int t;
    for (t = 0; t < CAP_TICKS; t++) {
        arena_tick(s, in);
        q32 cur = speed_of(s);
        q32 d = cur > prev ? cur - prev : prev - cur;
        stable = (d <= SETTLE_EPS) ? stable + 1 : 0;
        prev = cur;
        if (stable >= SETTLE_TICKS) break;
    }
    if (capped) *capped = (t >= CAP_TICKS);
    return prev;
}

static void probe_ramp(TuneMetrics* out) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    q32 top = drive_to_top(&s, in, &out->ramp_capped);
    out->top_speed = q_to_f(top) * TICK_HZ;

    /* second pass: ticks + distance to 90% of the measured top */
    probe_init(&s, in);
    in[0] = arena_input_pack(31, 0, 0, 0, 0);
    q32 target = qmul(top, Q(0.9));
    q32 x0 = s.players[0].pos.x;
    out->ticks_to_90pct = 0; out->ramp_distance = 0.0;
    for (int t = 1; t <= CAP_TICKS; t++) {
        arena_tick(&s, in);
        if (speed_of(&s) >= target) {
            out->ticks_to_90pct = t;
            q32 dx = s.players[0].pos.x - x0;
            out->ramp_distance = q_to_f(dx < 0 ? -dx : dx);
            break;
        }
    }
}

static void probe_stop(TuneMetrics* out) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    drive_to_top(&s, in, NULL);

    /* release the stick; measure until at rest */
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++) in[i] = arena_input_pack(0, 0, 0, 0, 0);
    q32 x0 = s.players[0].pos.x, z0 = s.players[0].pos.z;
    int t;
    for (t = 1; t <= CAP_TICKS; t++) {
        arena_tick(&s, in);
        if (speed_of(&s) <= SETTLE_EPS) break;
    }
    out->stop_capped = (t > CAP_TICKS);
    out->stop_ticks  = out->stop_capped ? CAP_TICKS : t;
    q32 dx = s.players[0].pos.x - x0, dz = s.players[0].pos.z - z0;
    out->stop_distance = q_to_f(qlen2(dx, dz));
}

/* Ticks for yaw to reach the stick's target (within 1 degree = 0x00B6).
 * Also records the largest lateral excursion from the start Z, which is the
 * turn radius at whatever speed the caller left the player at. */
static int turn_probe(int sx, int sy, uint16_t start_yaw, double* out_radius, int* capped) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    drive_to_top(&s, in, NULL);
    s.players[0].yaw = start_yaw;
    in[0] = arena_input_pack(sx, sy, 0, 0, 0);
    uint16_t target = iatan2(Q(sx), Q(-sy));   /* the sim's own convention */
    q32 z0 = s.players[0].pos.z, maxlat = 0;
    int t;
    for (t = 1; t <= CAP_TICKS; t++) {
        arena_tick(&s, in);
        q32 lat = s.players[0].pos.z - z0; if (lat < 0) lat = -lat;
        if (lat > maxlat) maxlat = lat;
        uint16_t y = s.players[0].yaw;
        if ((uint16_t)(y - target) <= 0x00B6 || (uint16_t)(target - y) <= 0x00B6) break;
    }
    if (capped) *capped = (t > CAP_TICKS);
    if (out_radius) *out_radius = q_to_f(maxlat);
    return t > CAP_TICKS ? CAP_TICKS : t;
}

static void probe_turns(TuneMetrics* out) {
    /* 180: start facing +Z (0x8000), stick full "forward" (sy=-31 -> yaw 0).
     * sy MUST be -31, not +31: iatan2(Q(0),Q(-31)) resolves to 0x0000 (a real
     * reversal) whereas sy=+31 resolves to 0x8000 == start_yaw (no turn at
     * all). Same trap documented at tests/test_movement.c:33. */
    out->turn180_ticks = turn_probe(0, -31, 0x8000, &out->turn_radius, &out->turn_capped);
    /* 90: start facing +Z (0x8000), stick full +X (yaw 0x4000) */
    out->turn90_ticks  = turn_probe(31, 0, 0x8000, NULL, NULL);
}

/* One jump. `running` decides whether we launch from rest or from top speed. */
static void jump_probe(int running, double* out_apex, int* out_air, double* out_dist) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    if (running) drive_to_top(&s, in, NULL);
    ArenaInput hold = running ? arena_input_pack(31, 0, 0, 0, 0)
                              : arena_input_pack(0, 0, 0, 0, 0);
    q32 x0 = s.players[0].pos.x, z0 = s.players[0].pos.z;
    q32 apex = 0; int air = 0, launched = 0;
    for (int t = 1; t <= CAP_TICKS; t++) {
        /* jump is edge-triggered: press on the first tick only */
        in[0] = (t == 1) ? (ArenaInput)(hold | (1u << 12)) : hold;
        arena_tick(&s, in);
        if (s.players[0].pos.y > 0) {
            launched = 1; air++;
            if (s.players[0].pos.y > apex) apex = s.players[0].pos.y;
        } else if (launched) break;
    }
    if (out_apex) *out_apex = q_to_f(apex);
    if (out_air)  *out_air  = air;
    if (out_dist) {
        q32 dx = s.players[0].pos.x - x0, dz = s.players[0].pos.z - z0;
        *out_dist = q_to_f(qlen2(dx, dz));
    }
}

static void probe_jumps(TuneMetrics* out) {
    jump_probe(0, &out->jump_apex, &out->jump_airtime, NULL);
    jump_probe(1, NULL, &out->runjump_airtime, &out->runjump_distance);
}

/* Cross the arena's long axis from the -X wall to the +X wall, from rest.
 * The one probe that deliberately starts at a wall rather than the centre. */
static void probe_traverse(TuneMetrics* out) {
    ArenaState s; ArenaInput in[ARENA_MAX_PLAYERS];
    probe_init(&s, in);
    const ArenaGeom* g = arena_geoms[0];
    q32 startx = -g->half_x + Q(0.5), endx = g->half_x - Q(0.5);
    s.players[0].pos.x = startx;
    in[0] = arena_input_pack(31, 0, 0, 0, 0);
    int t;
    for (t = 1; t <= CAP_TICKS; t++) {
        arena_tick(&s, in);
        if (s.players[0].pos.x >= endx) break;
    }
    out->traverse_capped = (t > CAP_TICKS);
    out->traverse_ticks  = out->traverse_capped ? CAP_TICKS : t;
}

void tune_probes_run(TuneMetrics* out) {
    memset(out, 0, sizeof(*out));
    probe_ramp(out);
    probe_stop(out);
    probe_turns(out);
    probe_jumps(out);
    probe_traverse(out);
}
