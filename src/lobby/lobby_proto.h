/* A3 lobby/rendezvous wire format — shared by server, client, and adapter.
 * Pure codec: no sockets, no allocation; unpack is bounds-checked and safe
 * on hostile input. All multi-byte wire fields little-endian. LobbyEndpoint
 * is HOST byte order in memory (converted at the socket boundary and in the
 * lobby-code bytes, which are big-endian). */
#ifndef LOBBY_PROTO_H
#define LOBBY_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define LOBBY_MAGIC        0x41335256u  /* "VR3A" in LE bytes = "A3RV" tag */
#define LOBBY_PROTO_VER    1
#define LOBBY_HDR_SIZE     6            /* u32 magic + u8 ver + u8 type */
#define LOBBY_MAX_PACKET   1200
#define LOBBY_MAX_PAYLOAD  1024         /* RELAY / GAME payload cap */
#define LOBBY_DEFAULT_PORT 40064
#define LOBBY_REQ_PAD_SIZE 32           /* HOST_REQ/JOIN_REQ padded size:
                                           every response must be smaller
                                           (anti-amplification) */

typedef struct { uint32_t ip; uint16_t port; } LobbyEndpoint;

typedef enum {
    LOBBY_HOST_REQ = 1, LOBBY_HOST_RESP, LOBBY_JOIN_REQ, LOBBY_JOIN_RESP,
    LOBBY_REJECT, LOBBY_PEER_INTRO, LOBBY_PUNCH, LOBBY_PUNCH_ACK,
    LOBBY_PUNCH_REPORT, LOBBY_PAIR_ROUTE, LOBBY_RELAY, LOBBY_GAME,
    LOBBY_START, LOBBY_START_ACK, LOBBY_KEEPALIVE,
    LOBBY_MSG_MAX
} LobbyMsgType;

typedef enum { LOBBY_REJ_BAD_SESSION = 1, LOBBY_REJ_FULL,
               LOBBY_REJ_VERSION_MISMATCH, LOBBY_REJ_EXPIRED } LobbyRejectReason;

typedef enum { LOBBY_ROUTE_DIRECT = 0, LOBBY_ROUTE_RELAY = 1 } LobbyRoute;

/* Client-side route to one peer (filled by the lobby client, consumed by the
 * net adapter). Lives here so lobby_client.h need not include gekkonet.h. */
typedef enum { NET_ROUTE_NONE = 0, NET_ROUTE_DIRECT, NET_ROUTE_RELAY } NetRouteKind;
typedef struct { uint8_t kind; LobbyEndpoint ep; } NetRoute; /* ep unused for RELAY */

typedef struct {
    uint8_t type;                       /* LobbyMsgType */
    union {
        struct { uint32_t net_version; uint8_t num_players;
                 LobbyEndpoint private_ep; } host_req;
        struct { uint16_t session_id; } host_resp;
        struct { uint16_t session_id; uint32_t net_version;
                 LobbyEndpoint private_ep; } join_req;
        struct { uint8_t slot; uint8_t num_players; } join_resp;
        struct { uint8_t reason; } reject;
        struct { uint8_t slot; LobbyEndpoint public_ep, private_ep; } peer_intro;
        struct { uint16_t session_id; uint8_t from_slot; uint32_t nonce; } punch;
                                        /* PUNCH and PUNCH_ACK (ack echoes nonce) */
        struct { uint16_t session_id; uint8_t slot_a, slot_b, ok; } punch_report;
        struct { uint8_t slot_a, slot_b, route; } pair_route;  /* LobbyRoute */
        struct { uint16_t session_id; uint8_t from_slot, to_slot;
                 uint16_t payload_len; const uint8_t* payload; } relay;
                                        /* RELAY (via server) and GAME (direct) */
        struct { uint32_t seed; uint8_t arena_id, num_players,
                 input_delay; } start;
        struct { uint16_t session_id; uint8_t slot; } start_ack;
                                        /* START_ACK and KEEPALIVE */
    } u;
} LobbyMsg;

size_t lobby_pack(const LobbyMsg* m, uint8_t out[LOBBY_MAX_PACKET]);
int    lobby_unpack(const uint8_t* buf, size_t len, LobbyMsg* out);

#define LOBBY_CODE_LEN 17               /* "XXXXX-XXXXX-XXXXX" */
void lobby_code_encode(LobbyEndpoint server, uint16_t session_id,
                       char out[LOBBY_CODE_LEN + 1]);
int  lobby_code_decode(const char* str, LobbyEndpoint* server,
                       uint16_t* session_id);

/* fnv1a over {LOBBY_PROTO_VER, TUNE_VERSION, sizeof(ArenaState),
 * arena_scripted_match_hash()}. First call replays the 5400-tick scripted
 * match (~0.3s); result is cached. */
uint32_t arena_net_version(void);

#endif
