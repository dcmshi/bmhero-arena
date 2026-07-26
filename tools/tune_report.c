/* Thin formatter over tune_probes. One tune in (the one this binary was
 * compiled with), one table out. Sweeping across tunes is tools/tune-report.ps1's
 * job — this stays dumb on purpose.
 *
 * Output precision is FIXED (3dp for distances/speeds, integers for ticks)
 * because tools/tune_metrics.baseline is diffed across six CI legs and raw
 * double formatting would drift in the last digit. */
#include <stdio.h>
#include <string.h>
#include "tune_probes.h"
#include "../src/arena/arena_tuning.h"
#include "../src/arena/arena_math.h"

static int json = 0;

static void row_f(const char* name, double v, const char* unit) {
    if (json) printf("  \"%s\": %.3f,\n", name, v);
    else      printf("%s\t%.3f\t%s\n", name, v, unit);
}
static void row_i(const char* name, int v, const char* unit) {
    if (json) printf("  \"%s\": %d,\n", name, v);
    else      printf("%s\t%d\t%s\n", name, v, unit);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--tsv")) json = 0;
        else { fprintf(stderr, "usage: tune_report [--tsv|--json]\n"); return 2; }
    }

    TuneMetrics m;
    tune_probes_run(&m);

    if (json) printf("{\n");
    else {
        /* self-describing: the tune this binary measures */
        printf("# TUNE_VERSION\t%d\n", TUNE_VERSION);
        printf("# TUNE_RUN_SPEED\t%d\n", TUNE_RUN_SPEED);
        printf("# TUNE_RUN_ACCEL\t%d\n", TUNE_RUN_ACCEL);
        printf("# TUNE_RUN_FRICTION\t%d\n", TUNE_RUN_FRICTION);
        printf("# TUNE_TURN_RATE\t%d\n", TUNE_TURN_RATE);
        printf("# TUNE_JUMP_IMPULSE\t%d\n", TUNE_JUMP_IMPULSE);
        printf("# TUNE_GRAVITY\t%d\n", TUNE_GRAVITY);
        printf("# TUNE_AIR_CONTROL\t%d\n", TUNE_AIR_CONTROL);
    }

    row_f("top_speed",        m.top_speed,        "u/s");
    row_i("ticks_to_90pct",   m.ticks_to_90pct,   "ticks");
    row_f("ramp_distance",    m.ramp_distance,    "u");
    row_f("stop_distance",    m.stop_distance,    "u");
    row_i("stop_ticks",       m.stop_ticks,       "ticks");
    row_i("turn180_ticks",    m.turn180_ticks,    "ticks");
    row_i("turn90_ticks",     m.turn90_ticks,     "ticks");
    row_f("turn_radius",      m.turn_radius,      "u");
    row_f("jump_apex",        m.jump_apex,        "u");
    row_i("jump_airtime",     m.jump_airtime,     "ticks");
    row_f("runjump_distance", m.runjump_distance, "u");
    row_i("runjump_airtime",  m.runjump_airtime,  "ticks");
    row_i("traverse_ticks",   m.traverse_ticks,   "ticks");

    if (json) printf("  \"capped\": %d\n}\n",
                     m.ramp_capped || m.stop_capped || m.turn_capped || m.traverse_capped);
    else if (m.ramp_capped || m.stop_capped || m.turn_capped || m.traverse_capped)
        printf("# WARNING\tone or more probes hit the iteration cap\n");
    return 0;
}
