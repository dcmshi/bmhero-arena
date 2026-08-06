/* Minimal portable nonblocking UDP, in the terms the lobby speaks.
 *
 * All platform headers stay inside udp_socket.c — including this header does
 * NOT drag in windows.h. Endpoints are `LobbyEndpoint` in HOST byte order;
 * htonl/htons happen here at the socket boundary and nowhere else, so no other
 * file in the project has to think about network byte order. */
#ifndef UDP_SOCKET_H
#define UDP_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "../lobby/lobby_proto.h"

/* An OS socket handle, widened so a Win64 SOCKET fits. -1 = closed. */
typedef struct { intptr_t fd; } UdpSocket;

/* WSAStartup on Windows, nothing elsewhere. Idempotent; 0 = ok. udp_open()
 * also calls it, so an explicit call is optional. */
int udp_global_init(void);

/* Bind a nonblocking UDP socket. bind_port 0 = an ephemeral port. 0 = ok. */
int udp_open(UdpSocket* s, uint16_t bind_port);

/* The port actually bound (resolves an ephemeral). 0 = error. */
uint16_t udp_bound_port(const UdpSocket* s);

/* 0 = the datagram was handed to the OS. UDP gives no delivery guarantee. */
int udp_send(UdpSocket* s, LobbyEndpoint to, const void* buf, size_t len);

/* >0 = datagram length; 0 = nothing pending; -1 = error.
 * NOTE: a genuine zero-length datagram also reads as 0 and is therefore
 * indistinguishable from "nothing pending". That is fine for this protocol —
 * the smallest legal lobby packet is LOBBY_HDR_SIZE bytes, so a zero-length
 * datagram is never ours and dropping it silently is the correct handling. */
int udp_recv(UdpSocket* s, LobbyEndpoint* from, void* buf, size_t cap);

/* The private endpoint to advertise for reaching `remote`: UDP-connect a
 * throwaway socket (which sends nothing) so the OS picks the outbound
 * interface, read its local IP, and pair that with `bound_port`. 0 = ok. */
int udp_local_endpoint_for(LobbyEndpoint remote, uint16_t bound_port, LobbyEndpoint* out);

/* Monotonic milliseconds, wrapping every ~49 days. Compare only as unsigned
 * differences (now - then), never as absolutes, and the wrap is harmless. */
uint32_t udp_now_ms(void);

/* Yield the CPU briefly — so callers need no platform headers of their own. */
void udp_sleep_ms(uint32_t ms);

void udp_close(UdpSocket* s);

#endif
