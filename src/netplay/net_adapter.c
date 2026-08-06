/* The custom GekkoNetAdapter. See net_adapter.h for the contract.
 *
 * Why this exists at all: gekko_default_adapter opens its OWN socket, so the
 * NAT mapping the rendezvous server observed while punching would belong to a
 * different socket than the one carrying game packets — the hole would be
 * punched in the wrong wall. Here the lobby client, the punch traffic and
 * GekkoNet all share one UdpSocket, and every datagram on it is a lobby_proto
 * packet, so a single recvfrom loop can sort game payloads from control
 * packets without a second port or a second thread.
 *
 * Allocation policy: the module state is static and the send path never
 * allocates. The RECEIVE path must, and there is no way around it — GekkoNet
 * takes ownership of each result and frees it as three separate blocks
 * (result, addr bytes, payload) via free_data. That contract is the reason
 * free_data is plain `free` and the reason the three mallocs are not one.
 *
 * Impairment shapes OUTGOING traffic only. Shaping the receive side would be
 * the same thing done twice (both ends of a link run this code), and delaying
 * a packet we already hold tells us nothing a delayed send does not. */
#include <stdlib.h>
#include <string.h>
#include "net_adapter.h"
#include "arena/arena_state.h"

#define NA_DELAY_QUEUE  512
#define NA_LOBBY_RING   16
#define NA_MAX_RESULTS  64      /* also the per-pump recvfrom bound */

typedef struct {
    uint32_t      due_ms;
    LobbyEndpoint to;
    uint16_t      len;
    uint8_t       buf[LOBBY_MAX_PACKET];
} DelayedPkt;

static struct {
    int             active;
    UdpSocket*      sock;
    uint16_t        session_id;
    uint8_t         local_slot;
    NetRoute        route[ARENA_MAX_PLAYERS];
    LobbyEndpoint   relay;
    NetImpairment   imp;
    int             imp_on;
    uint32_t        imp_rng;
    DelayedPkt      dq[NA_DELAY_QUEUE];
    int             dq_n;
    uint8_t         lobbyq[NA_LOBBY_RING][LOBBY_MAX_PACKET];
    uint16_t        lobbyq_len[NA_LOBBY_RING];
    LobbyEndpoint   lobbyq_from[NA_LOBBY_RING];
    int             lobbyq_head, lobbyq_count;
    GekkoNetResult* results[NA_MAX_RESULTS];
    uint32_t        drop_count, loss_count, overflow_count;
    GekkoNetAdapter iface;
} g;

/* ---------- impairment ---------- */
static uint32_t na_rand(void) {
    uint32_t x = g.imp_rng ? g.imp_rng : 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g.imp_rng = x;
    return x;
}

/* Deliver, or lose it, or park it in the delay queue. A full queue sends
 * immediately rather than dropping: an impairment profile that silently turned
 * into a packet loss storm would look like a netcode bug. */
static void impaired_send(LobbyEndpoint to, const uint8_t* buf, size_t len) {
    if (!g.imp_on) { udp_send(g.sock, to, buf, len); return; }

    if ((na_rand() % 100u) < g.imp.loss_pct) { g.loss_count++; return; }

    int32_t d = (int32_t)g.imp.delay_ms;
    if (g.imp.jitter_ms) {
        uint32_t span = 2u * (uint32_t)g.imp.jitter_ms + 1u;
        d += (int32_t)(na_rand() % span) - (int32_t)g.imp.jitter_ms;
        if (d < 0) d = 0;
    }
    int reorder = ((na_rand() % 100u) < g.imp.reorder_pct);

    if (d == 0) { udp_send(g.sock, to, buf, len); return; }
    if (g.dq_n >= NA_DELAY_QUEUE) {
        g.overflow_count++;
        udp_send(g.sock, to, buf, len);
        return;
    }
    DelayedPkt* p = &g.dq[g.dq_n++];
    p->due_ms = udp_now_ms() + (uint32_t)d;
    p->to     = to;
    p->len    = (uint16_t)len;
    memcpy(p->buf, buf, len);
    /* cheap reorder: push the packet queued BEFORE this one 1ms later, so the
     * pair leaves in the opposite order to the one the session asked for */
    if (reorder && g.dq_n >= 2) g.dq[g.dq_n - 2].due_ms += 1u;
}

static void flush_due(void) {
    if (!g.dq_n) return;
    uint32_t now = udp_now_ms();
    int w = 0;
    for (int i = 0; i < g.dq_n; i++) {
        /* signed difference: correct across the ms clock's 49-day wrap */
        if ((int32_t)(now - g.dq[i].due_ms) >= 0) {
            udp_send(g.sock, g.dq[i].to, g.dq[i].buf, g.dq[i].len);
        } else {
            if (w != i) g.dq[w] = g.dq[i];
            w++;
        }
    }
    g.dq_n = w;
}

/* ---------- the GekkoNetAdapter vtable ---------- */
static void na_send(GekkoNetAddress* addr, const char* data, int length) {
    if (!g.active || !addr || !addr->data || addr->size < 1) return;
    if (length < 0 || (size_t)length > LOBBY_MAX_PAYLOAD) { g.drop_count++; return; }

    uint8_t slot = *(const uint8_t*)addr->data;
    if (slot >= ARENA_MAX_PLAYERS) { g.drop_count++; return; }

    LobbyMsg m;
    LobbyEndpoint dest;
    memset(&m, 0, sizeof m);
    if (g.route[slot].kind == NET_ROUTE_DIRECT) {
        m.type = LOBBY_GAME; dest = g.route[slot].ep;
    } else if (g.route[slot].kind == NET_ROUTE_RELAY) {
        m.type = LOBBY_RELAY; dest = g.relay;
    } else {
        g.drop_count++; return;      /* no route decided yet: nothing to aim at */
    }
    m.u.relay.session_id  = g.session_id;
    m.u.relay.from_slot   = g.local_slot;
    m.u.relay.to_slot     = slot;
    m.u.relay.payload_len = (uint16_t)length;
    m.u.relay.payload     = (const uint8_t*)data;

    uint8_t buf[LOBBY_MAX_PACKET];
    size_t n = lobby_pack(&m, buf);
    if (!n) { g.drop_count++; return; }
    impaired_send(dest, buf, n);
}

static void lobby_ring_push(LobbyEndpoint from, const uint8_t* buf, size_t len) {
    int idx;
    if (g.lobbyq_count < NA_LOBBY_RING) {
        idx = (g.lobbyq_head + g.lobbyq_count) % NA_LOBBY_RING;
        g.lobbyq_count++;
    } else {                                     /* overwrite oldest */
        idx = g.lobbyq_head;
        g.lobbyq_head = (g.lobbyq_head + 1) % NA_LOBBY_RING;
    }
    memcpy(g.lobbyq[idx], buf, len);
    g.lobbyq_len[idx]  = (uint16_t)len;
    g.lobbyq_from[idx] = from;
}

static GekkoNetResult** na_recv(int* length) {
    int n = 0;
    if (length) *length = 0;
    if (!g.active) return g.results;

    flush_due();                                 /* the per-frame pump */

    uint8_t buf[LOBBY_MAX_PACKET];
    LobbyEndpoint from;
    for (int i = 0; i < NA_MAX_RESULTS; i++) {
        int rn = udp_recv(g.sock, &from, buf, sizeof buf);
        if (rn <= 0) break;

        LobbyMsg m;
        if (lobby_unpack(buf, (size_t)rn, &m) != 0) { g.drop_count++; continue; }

        if (m.type == LOBBY_GAME || m.type == LOBBY_RELAY) {
            /* Either wrapping is accepted: a peer whose route flipped mid-match
             * must not have its packets discarded for arriving the other way. */
            if (m.u.relay.session_id != g.session_id ||
                m.u.relay.to_slot != g.local_slot) { g.drop_count++; continue; }

            size_t pl = m.u.relay.payload_len;
            GekkoNetResult* r  = malloc(sizeof *r);
            uint8_t*        ad = malloc(1);
            void*           pd = malloc(pl ? pl : 1);
            if (!r || !ad || !pd) { free(r); free(ad); free(pd); g.drop_count++; continue; }
            *ad = m.u.relay.from_slot;
            if (pl) memcpy(pd, m.u.relay.payload, pl);
            r->addr.data = ad;
            r->addr.size = 1;
            r->data      = pd;
            r->data_len  = (unsigned int)pl;
            g.results[n++] = r;
        } else {
            lobby_ring_push(from, buf, (size_t)rn);
        }
    }
    if (length) *length = n;
    return g.results;
}

/* ---------- public API ---------- */
GekkoNetAdapter* net_adapter_init(UdpSocket* sock, uint16_t session_id, uint8_t local_slot) {
    if (g.active || !sock || local_slot >= ARENA_MAX_PLAYERS) return NULL;
    memset(&g, 0, sizeof g);
    g.active     = 1;
    g.sock       = sock;
    g.session_id = session_id;
    g.local_slot = local_slot;
    g.iface.send_data    = na_send;
    g.iface.receive_data = na_recv;
    g.iface.free_data    = free;
    return &g.iface;
}

void net_adapter_set_route(uint8_t slot, NetRoute route) {
    if (!g.active || slot >= ARENA_MAX_PLAYERS) return;
    g.route[slot] = route;
}

void net_adapter_set_relay(LobbyEndpoint relay) {
    if (!g.active) return;
    g.relay = relay;
}

void net_adapter_impair(const NetImpairment* imp) {
    if (!g.active) return;
    if (!imp) { g.imp_on = 0; return; }
    g.imp     = *imp;
    g.imp_on  = 1;
    g.imp_rng = imp->seed ? imp->seed : 1u;
}

int net_adapter_take_lobby_packet(LobbyEndpoint* from, uint8_t* buf, size_t cap) {
    if (!g.active || g.lobbyq_count <= 0 || !buf) return 0;
    int idx = g.lobbyq_head;
    uint16_t len = g.lobbyq_len[idx];
    if (cap < (size_t)len) return 0;             /* caller's buffer is too small */
    memcpy(buf, g.lobbyq[idx], len);
    if (from) *from = g.lobbyq_from[idx];
    g.lobbyq_head = (g.lobbyq_head + 1) % NA_LOBBY_RING;
    g.lobbyq_count--;
    return (int)len;
}

/* Frees nothing: gekko has already freed every result it was handed, and an
 * undelivered one exists only between na_recv and gekko's own loop over the
 * array — a window inside a single pump, which shutdown is never called in. */
void net_adapter_shutdown(void) { memset(&g, 0, sizeof g); }
