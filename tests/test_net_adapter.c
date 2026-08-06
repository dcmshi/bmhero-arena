/* The adapter, alone in one process, with a raw udp_socket peer playing "the
 * network": socket A carries the adapter, socket B is both the direct peer and
 * (for the RELAY case) the server. Everything the adapter puts on the wire is
 * inspected as a lobby packet, and everything B sends is checked for how the
 * adapter classified it — game result, lobby-ring packet, or drop. */
#include <stdio.h>
#include <string.h>
#include "../src/netplay/net_adapter.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* drain B until a packet arrives (bounded), unpack it */
static int recv_msg(UdpSocket* b, LobbyMsg* m, uint8_t* raw, LobbyEndpoint* from) {
    for (int i = 0; i < 500; i++) {
        int n = udp_recv(b, from, raw, LOBBY_MAX_PACKET);
        if (n > 0) return lobby_unpack(raw, (size_t)n, m) == 0 ? n : -1;
        udp_sleep_ms(1);
    }
    return 0;
}

int main(void) {
    CHECK(udp_global_init() == 0);
    UdpSocket a, b;
    CHECK(udp_open(&a, 0) == 0 && udp_open(&b, 0) == 0);
    LobbyEndpoint ep_b = { 0x7F000001u, udp_bound_port(&b) };
    LobbyEndpoint ep_a = { 0x7F000001u, udp_bound_port(&a) };

    GekkoNetAdapter* ga = net_adapter_init(&a, 0x1234, /*local_slot*/ 0);
    CHECK(ga != NULL);
    CHECK(net_adapter_init(&a, 0x1234, 0) == NULL);    /* singleton */
    if (!ga) { printf("test_net_adapter: %d FAILED\n", ++fails); return 1; }

    /* DIRECT send: gekko hands a 1-byte slot address; wire shows LOBBY_GAME */
    NetRoute r = { NET_ROUTE_DIRECT, ep_b };
    net_adapter_set_route(1, r);
    uint8_t slot1 = 1;
    GekkoNetAddress ad = { &slot1, 1 };
    ga->send_data(&ad, "hi", 2);
    LobbyMsg m; uint8_t raw[LOBBY_MAX_PACKET]; LobbyEndpoint from;
    CHECK(recv_msg(&b, &m, raw, &from) > 0);
    CHECK(m.type == LOBBY_GAME && m.u.relay.session_id == 0x1234);
    CHECK(m.u.relay.from_slot == 0 && m.u.relay.to_slot == 1);
    CHECK(m.u.relay.payload_len == 2 && memcmp(m.u.relay.payload, "hi", 2) == 0);

    /* RELAY send: route kind RELAY -> wrapped LOBBY_RELAY to the relay ep */
    NetRoute rr = { NET_ROUTE_RELAY, { 0, 0 } };
    net_adapter_set_route(2, rr);
    net_adapter_set_relay(ep_b);                       /* B doubles as "server" */
    uint8_t slot2 = 2; GekkoNetAddress ad2 = { &slot2, 1 };
    ga->send_data(&ad2, "yo", 2);
    CHECK(recv_msg(&b, &m, raw, &from) > 0);
    CHECK(m.type == LOBBY_RELAY && m.u.relay.to_slot == 2);

    /* a slot with no route at all must not reach the wire */
    uint8_t slot3 = 3; GekkoNetAddress ad3 = { &slot3, 1 };
    ga->send_data(&ad3, "no", 2);
    {
        int leaked = 0; uint8_t t3[LOBBY_MAX_PACKET]; LobbyEndpoint f3;
        for (int i = 0; i < 50; i++) if (udp_recv(&b, &f3, t3, sizeof t3) > 0) leaked = 1;
        CHECK(!leaked);
    }

    /* receive: B sends LOBBY_GAME from slot 1 -> one result, addr = {1} */
    LobbyMsg gm; memset(&gm, 0, sizeof gm);
    gm.type = LOBBY_GAME; gm.u.relay.session_id = 0x1234;
    gm.u.relay.from_slot = 1; gm.u.relay.to_slot = 0;
    gm.u.relay.payload_len = 3; gm.u.relay.payload = (const uint8_t*)"abc";
    uint8_t pk[LOBBY_MAX_PACKET]; size_t pn = lobby_pack(&gm, pk);
    udp_send(&b, ep_a, pk, pn);
    GekkoNetResult** res = NULL; int nres = 0;
    for (int i = 0; i < 500 && nres == 0; i++) {
        res = ga->receive_data(&nres);
        if (nres == 0) udp_sleep_ms(1);
    }
    CHECK(nres == 1);
    if (nres == 1) {
        CHECK(res[0]->addr.size == 1 && *(uint8_t*)res[0]->addr.data == 1);
        CHECK(res[0]->data_len == 3 && memcmp(res[0]->data, "abc", 3) == 0);
        ga->free_data(res[0]->addr.data); ga->free_data(res[0]->data); ga->free_data(res[0]);
    }

    /* wrong session id -> dropped */
    gm.u.relay.session_id = 0x9999; pn = lobby_pack(&gm, pk);
    udp_send(&b, ep_a, pk, pn);
    /* non-game lobby packet -> take_lobby_packet, not a gekko result */
    LobbyMsg hm; memset(&hm, 0, sizeof hm);
    hm.type = LOBBY_START_ACK; hm.u.start_ack.session_id = 0x1234; hm.u.start_ack.slot = 2;
    pn = lobby_pack(&hm, pk);
    udp_send(&b, ep_a, pk, pn);
    /* Pump until the lobby ring yields the START_ACK, watching that NEITHER of
     * those two packets ever became a game result. (The brief's loop spun on
     * receive_data alone; draining the ring in the same loop lets it exit as
     * soon as the evidence has arrived instead of always burning the budget.) */
    int saw_game = 0, ln = 0;
    uint8_t lb[LOBBY_MAX_PACKET]; LobbyEndpoint lfrom;
    for (int i = 0; i < 500 && !ln; i++) {
        nres = 0;
        res = ga->receive_data(&nres);
        for (int k = 0; k < nres; k++) {
            saw_game = 1;
            ga->free_data(res[k]->addr.data); ga->free_data(res[k]->data); ga->free_data(res[k]);
        }
        ln = net_adapter_take_lobby_packet(&lfrom, lb, sizeof lb);
        if (!ln) udp_sleep_ms(1);
    }
    CHECK(!saw_game);                                  /* neither became a game result */
    CHECK(ln > 0);
    CHECK(lobby_unpack(lb, (size_t)ln, &m) == 0 && m.type == LOBBY_START_ACK);
    CHECK(net_adapter_take_lobby_packet(&lfrom, lb, sizeof lb) == 0);   /* ring drained */

    /* impairment: 100% loss drops everything */
    NetImpairment imp = { 7u, 0, 0, 100, 0 };
    net_adapter_impair(&imp);
    ga->send_data(&ad, "xx", 2);
    int got = 0; uint8_t tmp[LOBBY_MAX_PACKET]; LobbyEndpoint f2;
    for (int i = 0; i < 100; i++) if (udp_recv(&b, &f2, tmp, sizeof tmp) > 0) got = 1;
    CHECK(!got);
    /* delay 50ms: not immediate, arrives after pumping past the deadline */
    NetImpairment imp2 = { 7u, 50, 0, 0, 0 };
    net_adapter_impair(&imp2);
    ga->send_data(&ad, "zz", 2);
    got = 0;
    for (int i = 0; i < 5; i++) if (udp_recv(&b, &f2, tmp, sizeof tmp) > 0) got = 1;
    CHECK(!got);                                       /* not yet */
    uint32_t t0 = udp_now_ms();
    while (udp_now_ms() - t0 < 80) { int k = 0; ga->receive_data(&k); }  /* pump flushes due sends */
    for (int i = 0; i < 500 && !got; i++) {
        if (udp_recv(&b, &f2, tmp, sizeof tmp) > 0) got = 1; else udp_sleep_ms(1);
    }
    CHECK(got);

    /* impairment off again: immediate delivery resumes */
    net_adapter_impair(NULL);
    ga->send_data(&ad, "ok", 2);
    CHECK(recv_msg(&b, &m, raw, &from) > 0);
    CHECK(m.type == LOBBY_GAME && m.u.relay.payload_len == 2);

    net_adapter_shutdown();
    CHECK(net_adapter_init(&a, 0x1234, 0) != NULL);    /* shutdown released it */
    net_adapter_shutdown();
    udp_close(&a); udp_close(&b);
    if (fails) { printf("test_net_adapter: %d FAILED\n", fails); return 1; }
    printf("test_net_adapter: all passed\n");
    return 0;
}
