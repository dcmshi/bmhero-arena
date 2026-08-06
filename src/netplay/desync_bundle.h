/* On-disk desync bundle. Little-endian, packed by explicit field writes (no
 * struct dumping — no padding on disk).
 *
 * Everything needed to reproduce a desync offline, without the peer and without
 * the game: the match parameters, the confirmed input history, the writer's own
 * per-tick hash ring, and the state it held when the detector fired.
 * tools/replay_bundle re-simulates from seed+inputs and asks where the LIVE ring
 * stopped agreeing with it — which is the tick the divergence entered the sim,
 * as opposed to the (later) tick a checksum exchange noticed.
 *
 * Layout after the header fields below:
 *     256 x { u32 tick, u32 hash }      the writer's hash ring, raw ring order
 *     sizeof(ArenaState) bytes          the state at detection (in-memory bytes)
 *     u32 input_count
 *     input_count x 4 x u16             inputs, tick-major, player 0..3
 *
 * The ArenaState snapshot is the ONE part written as raw in-memory bytes rather
 * than field by field, because the struct is already the project's snapshot and
 * wire format (arena_state.h). That makes a bundle readable only on a
 * little-endian host with the same layout — acceptable, since net_version pins
 * the layout and every current target is LE. */
#ifndef DESYNC_BUNDLE_H
#define DESYNC_BUNDLE_H

#include <stdint.h>
#include "../arena/arena_state.h"

#define BUNDLE_MAGIC   0x41334442u   /* "A3DB" */
#define BUNDLE_VERSION 1u
#define BUNDLE_MAX_TICKS 36000u      /* 10 min @ 60Hz */
#define BUNDLE_RING 256u             /* must match SyncSession's hash ring */

typedef struct {
    uint32_t magic, version, net_version, seed;
    uint8_t  arena_id, num_players, local_slot, pad;
    uint32_t detect_tick, local_checksum, remote_checksum;
    int32_t  remote_slot;            /* -1 if unknown */
} BundleHeader;

#endif
