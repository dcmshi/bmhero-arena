/* Custom GekkoNetAdapter: ONE socket for punching, game traffic, and relay,
 * so the NAT mapping the rendezvous observed is the mapping game packets
 * use. Every outgoing game packet is wrapped LOBBY_GAME (direct) or
 * LOBBY_RELAY (via server); every datagram received is a lobby_proto packet.
 *
 * SINGLETON: GekkoNetAdapter carries no user context (same limitation as
 * gekko_default_adapter) — one adapter per process. Memory contract: each
 * received result is THREE heap blocks (result, addr bytes, payload) because
 * GekkoNet frees each via free_data (backend.cpp:158-160). free_data = free. */
#ifndef NET_ADAPTER_H
#define NET_ADAPTER_H

#include "gekkonet.h"
#include "../lobby/lobby_proto.h"
#include "udp_socket.h"

typedef struct { uint32_t seed; uint16_t delay_ms, jitter_ms;
                 uint8_t loss_pct, reorder_pct; } NetImpairment;

GekkoNetAdapter* net_adapter_init(UdpSocket* sock, uint16_t session_id, uint8_t local_slot);
void net_adapter_set_route(uint8_t slot, NetRoute route);
void net_adapter_set_relay(LobbyEndpoint relay);
void net_adapter_impair(const NetImpairment* imp);
int  net_adapter_take_lobby_packet(LobbyEndpoint* from, uint8_t* buf, size_t cap);
void net_adapter_shutdown(void);

#endif
