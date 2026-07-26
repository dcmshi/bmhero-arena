/* Objective feel-metrics probes. Drives arena_tick with scripted inputs and
 * measures the quantities the open tuning knobs actually control.
 * Lives in tools/ (NOT src/arena/) so it may use floats freely — invariant #1.
 * Measurement only: reads ArenaState after each tick, never mutates the sim's
 * logic. Repositioning players before a probe is deliberate (see the .c). */
#ifndef TUNE_PROBES_H
#define TUNE_PROBES_H

/* One row per metric in the report. Distances/speeds are world units (u) and
 * u/s; tick counts are integer 60Hz ticks.
 * `*_capped` flags mark a probe that hit its iteration cap before converging —
 * the value is still emitted so a pathological tune is visible, not silent. */
typedef struct {
    /* ramp */
    double top_speed;          /* u/s at convergence, full stick */
    int    ticks_to_90pct;     /* ticks to reach 90% of top_speed */
    double ramp_distance;      /* u travelled reaching 90% */
    int    ramp_capped;
    /* stop */
    double stop_distance;      /* u travelled after stick release from top speed */
    int    stop_ticks;
    int    stop_capped;
    /* turn */
    int    turn180_ticks;
    int    turn90_ticks;
    double turn_radius;        /* u, max lateral excursion during the 180 */
    int    turn_capped;
    /* jump */
    double jump_apex;          /* u */
    int    jump_airtime;       /* ticks */
    double runjump_distance;   /* u travelled airborne from top speed */
    int    runjump_airtime;
    /* traverse */
    int    traverse_ticks;
    int    traverse_capped;
} TuneMetrics;

/* Runs every probe. Zeroes `out` first. Deterministic: same build => same
 * numbers, every run. */
void tune_probes_run(TuneMetrics* out);

#endif
