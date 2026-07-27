/* Static arena geometry — NOT part of ArenaState (identical on all peers by
 * construction; hashed into the session version handshake).
 * v0: axis-aligned boxes only: floor at y=0, boundary walls, pillars. */
#ifndef ARENA_GEOM_H
#define ARENA_GEOM_H

#include "arena_math.h"

typedef struct { Vec3q min, max; } Aabb;

typedef struct {
    q32   half_x, half_z;      /* rectangular arena, walls at +/- these per axis */
    const Aabb* pillars;
    int   num_pillars;
    Vec3q spawns[4];
} ArenaGeom;

/* ---- battle maps: each stored SEPARATELY so more can be added later ----
 * The sim's collidable bounds must track the RENDERED map (integration notes
 * 8.5a). `arena_geoms[]` below is the registry, indexed by ArenaState.arena_id.
 * To add a map: define a named `static const ArenaGeom` here + append &it to the
 * registry. Keep index 0 = the map the fork currently renders. */

/* Map 0 — "nitros-standin" (2026-07-26): matched to the MAP_NITROS_1 render
 * stand-in by DIRECT MEASUREMENT of the floor geometry, not by walking a player.
 * Probe mode 7 asks the game's OWN ground query (func_80078168) on a grid and
 * records where it reports floor; integration notes §8.15.
 *
 * Result: the floor is a filled SQUARE, 1900 x 1900 Hero units centred on Hero
 * (0,0), flat at y=240 — no holes, no pillars, no steps. Two independent passes
 * (50-unit and 10-unit grids; 6,561 and 40,401 samples) agree on the extent
 * EXACTLY, so the ±950 edge is measured to within 10 Hero units, not inferred.
 *
 * At the render bridge's scale of 120 Hero units per sim unit, that is
 * half = 950/120 = 7.9167 sim units on BOTH axes. collide_static already insets
 * the player by TUNE_PLAYER_RADIUS, so these are the floor edge itself.
 *
 * This SUPERSEDES the v5 "~1900x900" figure. That one came from walking a player
 * and logging where it stopped — which measures how far the player could GO, not
 * where the floor IS. The player was stopped by the sim's own z wall, so the
 * measurement confirmed the very bound it was meant to check, and the arena was
 * modelled a factor of two too narrow in z. */
static const ArenaGeom arena_nitros_standin = {
    Q(7.9167), Q(7.9167), NULL, 0,
    /* Corner spawns, symmetric now that the arena is square, and pulled IN to
     * +-5.5 (660 Hero) to clear the room's DAMAGE TILES.
     *
     * The Nitros room has hazard tiles - surface type 0xF7, which the game keys
     * its damage flag off (bmhero 69AA0.c:411) - in four 250x250 corner blocks
     * covering |x| >= 750 AND |z| >= 750 Hero. Measured directly with the
     * fork's surface-type raster (probe mode 7, 2026-07-27). The old +-6.5
     * spawns were 780 Hero out: INSIDE those blocks, so every player spawned
     * standing on a damage tile and took a hit and a stun on arrival. The sim
     * does not model the tiles, so nothing here could have caught it - it was
     * found by playing.
     *
     * 5.5 leaves 0.75 units (90 Hero) of clearance, comfortably more than
     * TUNE_PLAYER_RADIUS. Guarded by test_arena_cam.c in the fork, which checks
     * the spawns against the measured hazard bound. */
    { { Q(-5.5), 0, Q(-5.5) }, { Q(5.5), 0, Q(5.5) },
      { Q(-5.5), 0, Q( 5.5) }, { Q(5.5), 0, Q(-5.5) } },
};

/* Map 1 — "classic": the original designed battle arena (12x12 square ring +
 * four pillars). Kept for when a matching arena is actually built/rendered
 * in-game; NOT selected now (the Nitros render stand-in is flat/open, no pillars
 * to collide with — see 8.5a). */
static const Aabb classic_pillars[] = {
    { { Q(-2.5), Q(0), Q(-2.5) }, { Q(-1.5), Q(1.5), Q(-1.5) } },
    { { Q( 1.5), Q(0), Q(-2.5) }, { Q( 2.5), Q(1.5), Q(-1.5) } },
    { { Q(-2.5), Q(0), Q( 1.5) }, { Q(-1.5), Q(1.5), Q( 2.5) } },
    { { Q( 1.5), Q(0), Q( 1.5) }, { Q( 2.5), Q(1.5), Q( 2.5) } },
};
static const ArenaGeom arena_classic = {
    Q(6.0), Q(6.0), classic_pillars, 4,
    { { Q(-4.5), 0, Q(-4.5) }, { Q(4.5), 0, Q(4.5) },
      { Q(-4.5), 0, Q( 4.5) }, { Q(4.5), 0, Q(-4.5) } },
};

/* Registry — arena_id indexes this (0 = currently rendered). */
static const ArenaGeom* const arena_geoms[] = {
    &arena_nitros_standin,   /* 0 — used now (matches the Nitros render) */
    &arena_classic,          /* 1 — deferred (needs a matching rendered arena) */
};

#define ARENA_GEOM_COUNT ((int)(sizeof(arena_geoms) / sizeof(arena_geoms[0])))

#endif
