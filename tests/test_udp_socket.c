/* A3 task 3: the UDP wrapper. Loopback only — no external host is contacted,
 * so this is safe to run in CI and offline. */
#include <stdio.h>
#include <string.h>
#include "../src/netplay/udp_socket.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    CHECK(udp_global_init() == 0);
    CHECK(udp_global_init() == 0);                    /* idempotent */
    UdpSocket a, b;
    CHECK(udp_open(&a, 0) == 0);
    CHECK(udp_open(&b, 0) == 0);
    CHECK(udp_bound_port(&a) != 0 && udp_bound_port(&b) != 0);
    CHECK(udp_bound_port(&a) != udp_bound_port(&b));  /* two distinct ephemerals */

    LobbyEndpoint to = { 0x7F000001u, udp_bound_port(&b) };   /* 127.0.0.1 */
    CHECK(udp_send(&a, to, "ping", 4) == 0);

    uint8_t buf[64]; LobbyEndpoint from = { 0, 0 };
    int n = 0;
    for (int i = 0; i < 200 && n <= 0; i++) n = udp_recv(&b, &from, buf, sizeof buf);
    CHECK(n == 4 && memcmp(buf, "ping", 4) == 0);
    CHECK(from.port == udp_bound_port(&a));
    CHECK(from.ip == 0x7F000001u);                    /* host byte order, not network */

    CHECK(udp_recv(&b, &from, buf, sizeof buf) == 0); /* nonblocking empty */

    /* Sending zero bytes is legal and must not error. Deliberately NOT asserting
     * anything about receiving it: udp_recv reports 0 for both "empty datagram"
     * and "nothing pending", so no assertion here could distinguish them or fail.
     * The header documents that ambiguity; the loop below just drains it. */
    CHECK(udp_send(&a, to, "", 0) == 0);
    CHECK(udp_send(&a, to, NULL, 4) == -1);           /* NULL with a length is an error */

    /* a full-size lobby packet survives the round trip intact */
    {
        static uint8_t big[LOBBY_MAX_PACKET], got[LOBBY_MAX_PACKET];
        for (int i = 0; i < LOBBY_MAX_PACKET; i++) big[i] = (uint8_t)(i * 7 + 1);
        CHECK(udp_send(&a, to, big, sizeof big) == 0);
        n = 0;
        for (int i = 0; i < 500 && n <= 0; i++) n = udp_recv(&b, &from, got, sizeof got);
        CHECK(n == LOBBY_MAX_PACKET && memcmp(got, big, LOBBY_MAX_PACKET) == 0);
    }

    LobbyEndpoint priv;
    CHECK(udp_local_endpoint_for(to, udp_bound_port(&a), &priv) == 0);
    CHECK(priv.port == udp_bound_port(&a) && priv.ip != 0);

    uint32_t t0 = udp_now_ms(); (void)t0;             /* compiles, monotonic-ish */
    CHECK(udp_now_ms() - t0 < 60000u);                /* and not wildly wrong */

    udp_close(&a); udp_close(&b);
    if (fails) { printf("test_udp_socket: %d FAILED\n", fails); return 1; }
    printf("test_udp_socket: all passed\n");
    return 0;
}
