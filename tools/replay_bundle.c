/* Offline desync localization.
 *   replay_bundle a.bin          -> replay a's inputs; compare to a's hash
 *                                   ring; report where the LIVE session
 *                                   diverged from its own confirmed inputs.
 *   replay_bundle a.bin b.bin    -> also compare the two input histories and
 *                                   both rings; name the culprit peer and the
 *                                   first diverging tick; dump sim state
 *                                   fields at that tick (arena_trace-style).
 * Exit 0 = analysis printed; 1 = unreadable/invalid bundle.
 *
 * The distinction that makes this worth having: the detector reports the tick a
 * checksum EXCHANGE noticed a disagreement, which is later than the tick the
 * divergence entered the sim. Re-simulating from seed + confirmed inputs and
 * diffing against the writer's own ring recovers the entry tick, and — with two
 * bundles — says which peer's sim was the one that drifted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/netplay/desync_bundle.h"
#include "../src/arena/arena_sim.h"

typedef struct {
    const char* path;
    BundleHeader h;
    uint32_t ring_tick[BUNDLE_RING], ring_hash[BUNDLE_RING];
    ArenaState  snap;
    uint32_t    rec_count;
    ArenaInput* rec;                 /* rec_count * ARENA_MAX_PLAYERS */
} Bundle;

static int rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n ? 0 : -1; }
static int rd32(FILE* f, uint32_t* out) {
    uint8_t b[4];
    if (rd(f, b, 4) != 0) return -1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 0;
}
static int rd16(FILE* f, uint16_t* out) {
    uint8_t b[2];
    if (rd(f, b, 2) != 0) return -1;
    *out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return 0;
}

static int bundle_load(const char* path, Bundle* b) {
    memset(b, 0, sizeof *b);
    b->path = path;
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "replay_bundle: cannot open %s\n", path); return -1; }
    uint32_t rs;
    uint8_t hb[4];
    if (rd32(f, &b->h.magic) || rd32(f, &b->h.version) || rd32(f, &b->h.net_version)
        || rd32(f, &b->h.seed) || rd(f, hb, 4)
        || rd32(f, &b->h.detect_tick) || rd32(f, &b->h.local_checksum)
        || rd32(f, &b->h.remote_checksum) || rd32(f, &rs)) {
        fprintf(stderr, "replay_bundle: %s: short header\n", path); fclose(f); return -1;
    }
    b->h.arena_id = hb[0]; b->h.num_players = hb[1]; b->h.local_slot = hb[2];
    b->h.remote_slot = (int32_t)rs;
    if (b->h.magic != BUNDLE_MAGIC) {
        fprintf(stderr, "replay_bundle: %s: bad magic %08x\n", path, b->h.magic);
        fclose(f); return -1;
    }
    if (b->h.version != BUNDLE_VERSION) {
        fprintf(stderr, "replay_bundle: %s: version %u, this tool speaks %u\n",
                path, b->h.version, BUNDLE_VERSION);
        fclose(f); return -1;
    }
    if (b->h.num_players < 2 || b->h.num_players > ARENA_MAX_PLAYERS) {
        fprintf(stderr, "replay_bundle: %s: num_players %u\n", path, b->h.num_players);
        fclose(f); return -1;
    }
    for (uint32_t i = 0; i < BUNDLE_RING; i++)
        if (rd32(f, &b->ring_tick[i]) || rd32(f, &b->ring_hash[i])) {
            fprintf(stderr, "replay_bundle: %s: short ring\n", path);
            fclose(f); return -1;
        }
    if (rd(f, &b->snap, sizeof(ArenaState)) != 0) {
        fprintf(stderr, "replay_bundle: %s: short state\n", path); fclose(f); return -1;
    }
    if (rd32(f, &b->rec_count) != 0 || b->rec_count > BUNDLE_MAX_TICKS) {
        fprintf(stderr, "replay_bundle: %s: bad input_count\n", path);
        fclose(f); return -1;
    }
    b->rec = calloc((size_t)b->rec_count * ARENA_MAX_PLAYERS + 4, sizeof(ArenaInput));
    if (!b->rec) { fprintf(stderr, "replay_bundle: out of memory\n"); fclose(f); return -1; }
    for (uint32_t t = 0; t < b->rec_count; t++)
        for (int p = 0; p < ARENA_MAX_PLAYERS; p++)
            if (rd16(f, &b->rec[t * ARENA_MAX_PLAYERS + p]) != 0) {
                fprintf(stderr, "replay_bundle: %s: short inputs at tick %u\n", path, t);
                fclose(f); return -1;
            }
    fclose(f);
    printf("bundle %s: net_version=%08x seed=%08x arena=%u players=%u slot=%u\n",
           path, b->h.net_version, b->h.seed, b->h.arena_id, b->h.num_players,
           b->h.local_slot);
    printf("  detector: tick=%u local=%08x remote=%08x remote_slot=%d  inputs=%u ticks\n",
           b->h.detect_tick, b->h.local_checksum, b->h.remote_checksum,
           (int)b->h.remote_slot, b->rec_count);
    return 0;
}

/* Replay the bundle's own inputs from its own seed. hashes[T] = arena_hash after
 * tick T-1 executed, i.e. keyed exactly the way sync_session's ring is (post-tick
 * hash stored under the NEW state.tick). hashes[0] is the initial state. */
static uint32_t* replay(const Bundle* b) {
    uint32_t* hashes = calloc((size_t)b->rec_count + 2, sizeof(uint32_t));
    if (!hashes) return NULL;
    ArenaState s;
    arena_init(&s, b->h.arena_id, b->h.num_players, b->h.seed);
    hashes[0] = arena_hash(&s);
    for (uint32_t t = 0; t < b->rec_count; t++) {
        arena_tick(&s, &b->rec[t * ARENA_MAX_PLAYERS]);
        hashes[s.tick] = arena_hash(&s);
    }
    return hashes;
}

/* Earliest ring entry whose hash disagrees with the replay. 0 = none (the ring
 * never records tick 0, so 0 is a safe "no divergence" sentinel). */
static uint32_t first_divergence(const Bundle* b, const uint32_t* hashes,
                                 uint32_t* ring_h, uint32_t* replay_h) {
    uint32_t best = 0;
    for (uint32_t i = 0; i < BUNDLE_RING; i++) {
        uint32_t t = b->ring_tick[i];
        if (t == 0 || t > b->rec_count) continue;      /* empty or beyond replay */
        if (b->ring_hash[i] == hashes[t]) continue;
        if (best == 0 || t < best) {
            best = t; *ring_h = b->ring_hash[i]; *replay_h = hashes[t];
        }
    }
    return best;
}

/* Same columns as tools/arena_trace.c, so a divergence here reads like a trace row. */
static void print_fields(const char* label, const ArenaState* s) {
    printf("%s tick=%u hash=%08x", label, (unsigned)s->tick, arena_hash(s));
    for (int p = 0; p < s->num_players; p++) {
        const ArenaPlayer* pl = &s->players[p];
        printf("  p%d_x=%d p%d_y=%d p%d_z=%d p%d_vx=%d p%d_vz=%d p%d_yaw=%u p%d_state=%u p%d_hp=%u",
               p, pl->pos.x, p, pl->pos.y, p, pl->pos.z, p, pl->vel.x, p, pl->vel.z,
               p, (unsigned)pl->yaw, p, (unsigned)pl->state, p, (unsigned)pl->hp);
    }
    int live = 0;
    for (int i = 0; i < ARENA_MAX_BOMBS; i++)
        if (s->bombs[i].state != BSTATE_FREE) live++;
    printf("  live_bombs=%d phase=%u rng=%08x\n", live, (unsigned)s->phase,
           (unsigned)s->rng);
}

/* Re-run to N-1 and to N and print both, so the field that moved is visible. */
static void dump_around(const Bundle* b, uint32_t n) {
    if (n == 0 || n > b->rec_count) return;
    ArenaState s;
    arena_init(&s, b->h.arena_id, b->h.num_players, b->h.seed);
    for (uint32_t t = 0; t + 1 < n; t++) arena_tick(&s, &b->rec[t * ARENA_MAX_PLAYERS]);
    printf("replayed state around tick %u (from %s's confirmed inputs)\n", n, b->path);
    print_fields("  before", &s);
    arena_tick(&s, &b->rec[(n - 1) * ARENA_MAX_PLAYERS]);
    print_fields("  after ", &s);
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: replay_bundle <a.bin> [b.bin]\n");
        return 1;
    }
    Bundle ba, bb;
    if (bundle_load(argv[1], &ba) != 0) return 1;
    int two = (argc == 3);
    if (two) {
        if (bundle_load(argv[2], &bb) != 0) return 1;
        if (ba.h.net_version != bb.h.net_version) {
            fprintf(stderr, "replay_bundle: net_version mismatch - %s=%08x %s=%08x\n"
                            "  different builds cannot be compared\n",
                    argv[1], ba.h.net_version, argv[2], bb.h.net_version);
            return 1;
        }
        /* Confirmed inputs are supposed to be IDENTICAL on every peer once past
         * the confirmed frontier. A difference here is an adapter/GekkoNet bug,
         * a different failure than a sim divergence - so say so and keep going. */
        uint32_t n = ba.rec_count < bb.rec_count ? ba.rec_count : bb.rec_count;
        int said = 0;
        for (uint32_t t = 0; t < n && !said; t++)
            for (int p = 0; p < ARENA_MAX_PLAYERS; p++) {
                ArenaInput x = ba.rec[t * ARENA_MAX_PLAYERS + p];
                ArenaInput y = bb.rec[t * ARENA_MAX_PLAYERS + p];
                if (x != y) {
                    printf("CONFIRMED INPUTS DIFFER tick=%u player=%d a=%04x b=%04x\n",
                           t, p, x, y);
                    said = 1; break;
                }
            }
        if (!said) printf("confirmed inputs agree over %u shared ticks\n", n);
    }

    uint32_t* ha = replay(&ba);
    uint32_t* hb = NULL;
    if (!ha) { fprintf(stderr, "replay_bundle: out of memory\n"); return 1; }
    if (two && !(hb = replay(&bb))) { fprintf(stderr, "replay_bundle: out of memory\n"); return 1; }

    uint32_t ta = 0, tb = 0, rh = 0, ph = 0;
    ta = first_divergence(&ba, ha, &rh, &ph);
    if (ta) printf("DIVERGED bundle=%s tick=%u ring=%08x replay=%08x\n", ba.path, ta, rh, ph);
    else    printf("CONSISTENT bundle=%s\n", ba.path);
    if (two) {
        uint32_t rh2 = 0, ph2 = 0;
        tb = first_divergence(&bb, hb, &rh2, &ph2);
        if (tb) printf("DIVERGED bundle=%s tick=%u ring=%08x replay=%08x\n", bb.path, tb, rh2, ph2);
        else    printf("CONSISTENT bundle=%s\n", bb.path);
        if (ta && !tb)      printf("CULPRIT %s\n", ba.path);
        else if (tb && !ta) printf("CULPRIT %s\n", bb.path);
        else if (ta && tb)  printf("CULPRIT both diverged from their own inputs\n");
        else                printf("CULPRIT none - both sims match their confirmed inputs\n");
    }

    /* field-level dump at the earliest divergence either bundle reports */
    const Bundle* who = NULL; uint32_t n = 0;
    if (ta && (!tb || ta <= tb)) { who = &ba; n = ta; }
    else if (tb)                 { who = &bb; n = tb; }
    if (who) dump_around(who, n);

    free(ha); free(hb); free(ba.rec); if (two) free(bb.rec);
    return 0;
}
