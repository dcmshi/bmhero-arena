/* Portable nonblocking UDP. See udp_socket.h.
 *
 * The only place in the project that knows about network byte order, socket
 * handles, or errno/WSAGetLastError. Everything above this file works in
 * LobbyEndpoint host-order terms. */
#include <string.h>
#include "udp_socket.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>          /* must precede windows.h */
#  include <ws2tcpip.h>
#  include <windows.h>
   typedef SOCKET udp_sock;
   typedef int    udp_socklen;
#  define UDP_BAD        INVALID_SOCKET
#  define udp_last_err() WSAGetLastError()
#  define udp_closesock  closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <time.h>
   typedef int       udp_sock;
   typedef socklen_t udp_socklen;
#  define UDP_BAD        (-1)
#  define udp_last_err() errno
#  define udp_closesock  close
#endif

static int g_inited = 0;

int udp_global_init(void) {
    if (g_inited) return 0;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#endif
    g_inited = 1;
    return 0;
}

static int set_nonblocking(udp_sock fd) {
#ifdef _WIN32
    u_long on = 1;
    return (ioctlsocket(fd, FIONBIO, &on) == 0) ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return (fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0) ? 0 : -1;
#endif
}

int udp_open(UdpSocket* s, uint16_t bind_port) {
    if (!s) return -1;
    s->fd = -1;
    if (udp_global_init() != 0) return -1;

    udp_sock fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == UDP_BAD) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons(bind_port);
    if (bind(fd, (struct sockaddr*)&sa, (udp_socklen)sizeof sa) != 0) {
        udp_closesock(fd); return -1;
    }
    if (set_nonblocking(fd) != 0) { udp_closesock(fd); return -1; }

    s->fd = (intptr_t)fd;
    return 0;
}

uint16_t udp_bound_port(const UdpSocket* s) {
    if (!s || s->fd == -1) return 0;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    udp_socklen sl = (udp_socklen)sizeof sa;
    if (getsockname((udp_sock)s->fd, (struct sockaddr*)&sa, &sl) != 0) return 0;
    return ntohs(sa.sin_port);
}

int udp_send(UdpSocket* s, LobbyEndpoint to, const void* buf, size_t len) {
    if (!s || s->fd == -1) return -1;
    if (len && !buf) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(to.ip);
    sa.sin_port        = htons(to.port);
    int n = (int)sendto((udp_sock)s->fd, (const char*)buf, (int)len, 0,
                        (const struct sockaddr*)&sa, (udp_socklen)sizeof sa);
    return (n == (int)len) ? 0 : -1;
}

int udp_recv(UdpSocket* s, LobbyEndpoint* from, void* buf, size_t cap) {
    if (!s || s->fd == -1 || !buf) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    udp_socklen sl = (udp_socklen)sizeof sa;
    int n = (int)recvfrom((udp_sock)s->fd, (char*)buf, (int)cap, 0,
                          (struct sockaddr*)&sa, &sl);
    if (n < 0) {
        int e = udp_last_err();
#ifdef _WIN32
        /* WSAECONNRESET/WSAEMSGSIZE arrive on a UDP socket when a previous
         * datagram was refused or oversized. Neither is fatal to the socket:
         * report "nothing this time" and keep serving. */
        if (e == WSAEWOULDBLOCK || e == WSAECONNRESET || e == WSAEMSGSIZE) return 0;
#else
        if (e == EAGAIN || e == EINTR) return 0;
#endif
        return -1;
    }
    if (from) {
        from->ip   = ntohl(sa.sin_addr.s_addr);
        from->port = ntohs(sa.sin_port);
    }
    return n;
}

int udp_local_endpoint_for(LobbyEndpoint remote, uint16_t bound_port, LobbyEndpoint* out) {
    if (!out) return -1;
    if (udp_global_init() != 0) return -1;

    udp_sock t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t == UDP_BAD) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(remote.ip);
    sa.sin_port        = htons(remote.port);

    int rc = -1;
    /* connect() on a UDP socket sends NOTHING — it only fixes the default peer,
     * which is enough for the OS to commit to an outbound interface. */
    if (connect(t, (const struct sockaddr*)&sa, (udp_socklen)sizeof sa) == 0) {
        struct sockaddr_in me;
        memset(&me, 0, sizeof me);
        udp_socklen sl = (udp_socklen)sizeof me;
        if (getsockname(t, (struct sockaddr*)&me, &sl) == 0) {
            out->ip   = ntohl(me.sin_addr.s_addr);
            out->port = bound_port;   /* the caller's real listening port */
            rc = 0;
        }
    }
    udp_closesock(t);
    return rc;
}

uint32_t udp_now_ms(void) {
#ifdef _WIN32
    return (uint32_t)(GetTickCount64() & 0xFFFFFFFFu);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

void udp_sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
#endif
}

void udp_close(UdpSocket* s) {
    if (!s || s->fd == -1) return;
    udp_closesock((udp_sock)s->fd);
    s->fd = -1;
}
