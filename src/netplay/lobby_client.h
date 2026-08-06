/* Lobby bootstrap state machine. Non-blocking: caller pumps lobby_poll every
 * few ms. During bootstrap the client reads the socket directly; after READY
 * the net adapter owns the socket and lobby_post_poll drains stray lobby
 * packets (START refans) from the adapter's queue. */
#ifndef LOBBY_CLIENT_H
#define LOBBY_CLIENT_H

#include "../arena/arena_state.h"     /* ARENA_MAX_PLAYERS */
#include "../lobby/lobby_proto.h"
#include "udp_socket.h"

#define LOBBY_RETRY_MS        500u
#define LOBBY_RETRY_MAX       10
#define LOBBY_PUNCH_PERIOD_MS 250u
#define LOBBY_PUNCH_BUDGET_MS 3000u
#define LOBBY_TIMEOUT_MS      30000u
#define LOBBY_KEEPALIVE_MS    10000u

typedef enum { LOBBY_C_IDLE = 0, LOBBY_C_REGISTERING, LOBBY_C_JOINING,
               LOBBY_C_WAITING_PEERS, LOBBY_C_PUNCHING, LOBBY_C_AWAITING_START,
               LOBBY_C_READY, LOBBY_C_FAILED } LobbyClientStage;

typedef struct {
    uint8_t       local_slot, num_players, arena_id, input_delay;
    uint16_t      session_id;
    uint32_t      seed;
    NetRoute      route[ARENA_MAX_PLAYERS];   /* [local_slot] = NET_ROUTE_NONE */
    LobbyEndpoint server;
} LobbyResult;

typedef struct {
    uint8_t  stage, is_host, forced_relay;
    UdpSocket* sock;
    LobbyEndpoint server, private_ep;
    uint16_t session_id;
    uint8_t  local_slot, num_players;
    uint32_t net_ver, seed; uint8_t arena_id, input_delay;
    uint32_t begin_ms, last_send_ms, rng;
    uint8_t  retries;                          /* of the outstanding HOST/JOIN_REQ */
    uint32_t last_keepalive_ms, last_start_ms;
    uint8_t  start_seen, host_start_sent;
    /* every pair the server has decided, so the host can tell when ALL
     * n(n-1)/2 of them are in and the match may start */
    uint8_t  route_known[ARENA_MAX_PLAYERS][ARENA_MAX_PLAYERS];
    struct {
        uint8_t  present, acked, reported;
        LobbyEndpoint pub, priv, chosen;
        uint32_t nonce, punch_begin_ms, last_punch_ms, last_report_ms;
        int8_t   route;                        /* -1 unknown, else LobbyRoute */
        uint8_t  punch_alt;                    /* alternates priv/pub probes */
    } peer[ARENA_MAX_PLAYERS];
    LobbyResult result;
    char code[LOBBY_CODE_LEN + 1];
    char fail_stage[24];
} LobbyClient;

int  lobby_host_begin(LobbyClient* lc, UdpSocket* sock, LobbyEndpoint server,
                      uint8_t num_players, uint32_t seed, uint8_t arena_id,
                      uint8_t input_delay, uint32_t now_ms);
int  lobby_join_begin(LobbyClient* lc, UdpSocket* sock, const char* code,
                      uint32_t now_ms);
LobbyClientStage lobby_poll(LobbyClient* lc, uint32_t now_ms);
void lobby_post_poll(LobbyClient* lc, uint32_t now_ms);
const LobbyResult* lobby_result(const LobbyClient* lc);
const char* lobby_code(const LobbyClient* lc);
const char* lobby_fail_stage(const LobbyClient* lc);
void lobby_set_forced_relay(LobbyClient* lc);

#endif
