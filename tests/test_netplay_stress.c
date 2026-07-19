/* Determinism gate: GekkoNet stress session continuously rolls back and
 * re-simulates the scripted 4P match, cross-checking state checksums
 * (check_distance 8). Any divergence raises a desync -> fail. */
#include <stdio.h>
#include "../src/netplay/sync_session.h"

int main(void) {
    SyncConfig cfg = {0};
    cfg.mode = SYNC_STRESS;
    cfg.num_players = 4;
    cfg.local_mask = 0x0F;
    cfg.seed = 0xB0BB1E5;
    SyncSession* s = sync_create(&cfg);
    if (!s) { printf("FAIL: create\n"); return 1; }

    uint32_t r = 0xC0FFEE01;
    int advanced = 0;
    for (uint32_t t = 0; t < 3600; t++) {          /* one stressed minute */
        ArenaInput in[ARENA_MAX_PLAYERS];
        for (int i = 0; i < 4; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            int sx = (int)(r & 63) - 32; if (sx < -31) sx = -31;
            int sy = (int)((r >> 6) & 63) - 32; if (sy < -31) sy = -31;
            int bomb = (int)((t + (uint32_t)(i * 37)) % (uint32_t)(90 + i * 80))
                       < (30 + i * 40);
            int set = ((t + (uint32_t)(i * 53)) % 137u) == 0;
            in[i] = arena_input_pack(sx, sy, ((r >> 12) & 31) == 0, bomb, set);
        }
        advanced += sync_frame(s, in);
        if (sync_desynced(s)) {
            printf("FAIL: desync detected at frame %u\n", t);
            return 1;
        }
    }
    printf("stress: advanced %d, tick %u, hash %08x\n",
           advanced, sync_present_tick(s), sync_present_hash(s));
    if (advanced < 3000) { printf("FAIL: session barely advanced\n"); return 1; }
    sync_destroy(s);
    printf("netplay_stress: ALL TESTS PASSED\n");
    return 0;
}
