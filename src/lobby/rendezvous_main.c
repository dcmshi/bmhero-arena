/* The deploy-anywhere rendezvous+relay binary. All logic is rendezvous.c;
 * this is the socket loop. Single-threaded, one socket, no allocation.
 *
 * The core is deliberately silent (printf-free, so it stays unit-testable), so
 * every log line is produced here by observing the core from outside. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rendezvous.h"
#include "../netplay/udp_socket.h"

static UdpSocket g_sock;
static void emit(void* ctx, LobbyEndpoint to, const uint8_t* buf, size_t len) {
    (void)ctx; udp_send(&g_sock, to, buf, len);
}

int main(int argc, char** argv) {
    uint16_t port = LOBBY_DEFAULT_PORT;
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--port")) port = (uint16_t)atoi(argv[i + 1]);
    if (udp_global_init() != 0 || udp_open(&g_sock, port) != 0) {
        fprintf(stderr, "arena_rendezvous: cannot bind UDP %u\n", port);
        return 1;
    }
    static Rendezvous rv;
    uint32_t boot = udp_now_ms();
    rv_init(&rv, boot, boot ^ 0xA3A3A3A3u);
    printf("arena_rendezvous: listening on UDP %u\n", udp_bound_port(&g_sock));
    fflush(stdout);

    uint8_t buf[LOBBY_MAX_PACKET];
    LobbyEndpoint from;
    uint32_t last_tick = boot;
    int last_sessions = 0;
    int relay_logged = 0;
    for (;;) {
        int n = udp_recv(&g_sock, &from, buf, sizeof buf);
        uint32_t now = udp_now_ms();
        if (n > 0) rv_handle(&rv, now, from, buf, (size_t)n, emit, NULL);
        if (now - last_tick >= 100u) { rv_tick(&rv, now, emit, NULL); last_tick = now; }

        /* The only observable the pure core offers; log on change so the output
         * is one line per lobby opening or closing, not one per packet. */
        int cur = rv_active_sessions(&rv);
        if (cur != last_sessions) {
            printf("arena_rendezvous: sessions=%d\n", cur);
            fflush(stdout);
            last_sessions = cur;
        }

        /* Latched, once: the difference between a mesh that punched through and
         * one that quietly fell back to relay is invisible from the outside
         * otherwise, and both look like a passing test. Printed once because a
         * relayed match forwards thousands of packets a second. */
        if (!relay_logged && rv_relay_forwards(&rv) > 0) {
            printf("relay active\n");
            fflush(stdout);
            relay_logged = 1;
        }
        if (n <= 0) udp_sleep_ms(1);
    }
}
