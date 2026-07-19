/* Two-process localhost rollback match. The harness runs this twice:
 *   test_netplay_p2p --port 7101 --peer 127.0.0.1:7102 --player 0
 *   test_netplay_p2p --port 7102 --peer 127.0.0.1:7101 --player 1
 * Each side scripts only ITS player's inputs; identical final
 * "p2p tick T hash H" lines prove both simulations converged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/netplay/sync_session.h"

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep(ms * 1000); }
#endif

int main(int argc, char** argv) {
    int port = 0, player = 0, ticks = 600;
    const char* peer = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--port"))   port = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--peer"))   peer = argv[i + 1];
        if (!strcmp(argv[i], "--player")) player = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--ticks"))  ticks = atoi(argv[i + 1]);
    }
    if (!port || !peer || player < 0 || player > 1) {
        fprintf(stderr, "usage: --port P --peer ip:port --player 0|1 [--ticks T]\n");
        return 2;
    }

    SyncConfig cfg = {0};
    cfg.mode = SYNC_ONLINE;
    cfg.num_players = 2;
    cfg.local_mask = (uint8_t)(1u << player);
    cfg.local_port = (uint16_t)port;
    cfg.peer_addr[1 - player] = peer;
    cfg.seed = 0xB0BB1E5;
    cfg.input_delay = 1;
    SyncSession* s = sync_create(&cfg);
    if (!s) { printf("FAIL: create\n"); return 1; }

    /* run until the target tick is well behind us, then read its hash */
    const uint32_t target = (uint32_t)ticks;
    for (int frame = 0; frame < ticks * 20; frame++) {
        uint32_t t = sync_present_tick(s);
        if (t >= target + 90) break;               /* target long confirmed */
        ArenaInput in[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
        int sx = (int)((t / 8 + (uint32_t)(player * 16)) % 63) - 31;
        int bomb = (int)((t + (uint32_t)(player * 37)) % 150) < 40;
        in[player] = arena_input_pack(sx, 10, (t % 120) == 0, bomb,
                                      (t % 137) == 0);
        sync_frame(s, in);
        if (sync_desynced(s)) { printf("FAIL: desync\n"); return 1; }
        sleep_ms(2);
    }

    uint32_t h = sync_hash_at(s, target);
    if (h == 0) { printf("FAIL: never reached tick %u\n", target); return 1; }
    printf("p2p tick %u hash %08x\n", target, h);
    sync_destroy(s);
    return 0;
}
