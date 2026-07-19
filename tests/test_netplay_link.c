/* Link gate: GekkoNet fetched, built, and callable through the wrapper. */
#include <stdio.h>
#include "../src/netplay/sync_session.h"

int main(void) {
    SyncConfig cfg = {0};
    cfg.mode = SYNC_STRESS;
    cfg.num_players = 2;
    cfg.local_mask = 0x03;
    cfg.seed = 0xBEEF;
    SyncSession* s = sync_create(&cfg);
    if (!s) { printf("FAIL: sync_create\n"); return 1; }
    if (sync_state(s)->num_players != 2) { printf("FAIL: state init\n"); return 1; }
    sync_destroy(s);
    printf("netplay_link: ALL TESTS PASSED\n");
    return 0;
}
