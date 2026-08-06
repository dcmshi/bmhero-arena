/* 4-process rendezvous+mesh convergence test client. The harness
 * (run_mesh_test.sh) launches arena_rendezvous, one --host and three --join
 * of the printed code, and asserts all four "mesh " lines match.
 *   test_netplay_mesh --server 127.0.0.1:PORT --host 4 [--ticks N] [--forced-relay] [--impair PROFILE]
 *   test_netplay_mesh --server 127.0.0.1:PORT --join CODE [...]
 * Exit: 0 converged, 1 failure, 2 usage, 3 desync (used by later variants). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/netplay/lobby_client.h"
#include "../src/netplay/net_adapter.h"
#include "../src/netplay/sync_session.h"

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep(ms * 1000); }
#endif

/* "a.b.c.d:port" -> host-order LobbyEndpoint. 0 = ok. */
static int parse_ep(const char* s, LobbyEndpoint* out) {
    unsigned a, b, c, d, p;
    if (!s || !out) return -1;
    if (sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p) != 5) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65535) return -1;
    out->ip   = (a << 24) | (b << 16) | (c << 8) | d;
    out->port = (uint16_t)p;
    return 0;
}

int main(int argc, char** argv) {
    const char* server_s = NULL; const char* join_code = NULL;
    int host_players = 0, ticks = 600, forced_relay = 0;
    const char* impair = NULL;
    int inject = -1;                        /* corrupt at present tick >= N */
    const char* bundle_dir = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server") && i + 1 < argc) server_s = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host_players = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join_code = argv[++i];
        else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--forced-relay")) forced_relay = 1;
        else if (!strcmp(argv[i], "--impair") && i + 1 < argc) impair = argv[++i];
        else if (!strcmp(argv[i], "--inject") && i + 1 < argc) inject = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bundle-dir") && i + 1 < argc) bundle_dir = argv[++i];
    }
    if ((!host_players && !join_code) || (host_players && !server_s)) {
        fprintf(stderr, "usage: --server ip:port --host N | --join CODE [--ticks N] [--forced-relay]\n"
                        "       [--impair lan0|wan100|rough200] [--inject TICK] [--bundle-dir DIR]\n");
        return 2;
    }

    if (udp_global_init() != 0) { printf("FAIL udp_init\n"); return 1; }
    UdpSocket sock;
    if (udp_open(&sock, 0) != 0) { printf("FAIL udp_open\n"); return 1; }

    static LobbyClient lc;                       /* zero-init */
    if (host_players) {
        LobbyEndpoint srv;
        if (parse_ep(server_s, &srv) != 0) { printf("FAIL server addr\n"); return 2; }
        lobby_host_begin(&lc, &sock, srv, (uint8_t)host_players,
                         0xB0BB1E5, 0, 1, udp_now_ms());
    } else {
        if (lobby_join_begin(&lc, &sock, join_code, udp_now_ms()) != 0) {
            printf("FAIL bad code\n"); return 2;
        }
    }
    if (forced_relay) lobby_set_forced_relay(&lc);

    LobbyClientStage st;
    int code_printed = 0;
    do {
        st = lobby_poll(&lc, udp_now_ms());
        if (host_players && !code_printed && lobby_code(&lc)[0]) {
            printf("code %s\n", lobby_code(&lc)); fflush(stdout);
            code_printed = 1;
        }
        sleep_ms(2);
    } while (st != LOBBY_C_READY && st != LOBBY_C_FAILED);
    if (st == LOBBY_C_FAILED) { printf("FAIL stage=%s\n", lobby_fail_stage(&lc)); return 1; }

    const LobbyResult* lr = lobby_result(&lc);
    if (!lr) { printf("FAIL no result\n"); return 1; }
    printf("routes slot=%u", lr->local_slot);
    for (int i = 0; i < lr->num_players; i++) {
        if (i == lr->local_slot) continue;
        printf(" %d=%s", i, lr->route[i].kind == NET_ROUTE_DIRECT ? "direct" :
                            lr->route[i].kind == NET_ROUTE_RELAY  ? "relay"  : "none");
    }
    printf("\n"); fflush(stdout);

    GekkoNetAdapter* ga = net_adapter_init(&sock, lr->session_id, lr->local_slot);
    if (!ga) { printf("FAIL adapter\n"); return 1; }
    for (int i = 0; i < lr->num_players; i++)
        if (i != lr->local_slot) net_adapter_set_route((uint8_t)i, lr->route[i]);
    net_adapter_set_relay(lr->server);
    if (impair) {
        NetImpairment imp = { 0xA3A3A3A3u ^ lr->local_slot, 0, 0, 0, 0 };
        if (!strcmp(impair, "wan100"))  { imp.delay_ms = 50;  imp.jitter_ms = 10; imp.loss_pct = 1; imp.reorder_pct = 1; }
        if (!strcmp(impair, "rough200")){ imp.delay_ms = 100; imp.jitter_ms = 25; imp.loss_pct = 3; imp.reorder_pct = 2; }
        net_adapter_impair(&imp);       /* lan0 = all zero = effectively off */
    }

    SyncConfig cfg = {0};
    cfg.mode = SYNC_ONLINE;
    cfg.num_players = lr->num_players;
    cfg.local_mask = (uint8_t)(1u << lr->local_slot);
    cfg.seed = lr->seed;
    cfg.arena_id = lr->arena_id;
    cfg.input_delay = lr->input_delay;
    cfg.adapter = ga;
    SyncSession* s = sync_create(&cfg);
    if (!s) { printf("FAIL sync_create\n"); return 1; }

    const uint32_t target = (uint32_t)ticks;
    const int me = lr->local_slot;
    for (int frame = 0; frame < ticks * 40; frame++) {
        uint32_t t = sync_present_tick(s);
        if (t >= target + 90) break;
        ArenaInput in[ARENA_MAX_PLAYERS] = { 0, 0, 0, 0 };
        /* per-slot deterministic script, same family as test_netplay_p2p */
        int sx = (int)((t / 8 + (uint32_t)(me * 16)) % 63) - 31;
        int bomb = (int)((t + (uint32_t)(me * 37)) % 150) < 40;
        in[me] = arena_input_pack(sx, 10, (t % 120) == (uint32_t)me, bomb,
                                  (t % 137) == 0);
        if (inject >= 0 && (int)t >= inject) { sync_debug_corrupt(s); inject = -1; }
        sync_frame(s, in);
        lobby_post_poll(&lc, udp_now_ms());
        if (sync_desynced(s)) {
            SyncDesyncInfo di;
            if (sync_desync_info(s, &di))
                printf("desync tick=%u local=%08x remote=%08x\n",
                       di.tick, di.local_hash, di.remote_hash);
            char path[512];
            snprintf(path, sizeof path, "%s/desync_slot%d.bin", bundle_dir, me);
            if (sync_dump_bundle(s, path) == 0) printf("bundle %s\n", path);
            fflush(stdout);
            return 3;
        }
        sleep_ms(2);
    }
    uint32_t h = sync_hash_at(s, target);
    if (h == 0) { printf("FAIL: never confirmed tick %u\n", target); return 1; }
    printf("mesh slot=%d tick=%u hash=%08x\n", me, target, h);
    /* Timing evidence for the soak: impairment must move these and never the
     * hash above. tools/net-soak.ps1 aggregates both lines across matches. */
    SyncStats st_; sync_stats(s, &st_);
    printf("metrics slot=%d stalls=%u rb_ticks=%u rb_max=%u pumps=%u\n",
           me, st_.stall_frames, st_.rollback_ticks, st_.max_rollback_depth,
           st_.pumps);
    printf("rbhist");
    for (int i = 0; i <= 8; i++) printf(" %d:%u", i, st_.rbhist[i]);
    printf("\n");
    fflush(stdout);
    sync_destroy(s);
    net_adapter_shutdown();
    return 0;
}
