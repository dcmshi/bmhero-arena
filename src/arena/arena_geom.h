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

/* Map 0 — "nitros-standin" (2026-07-24): matched to the MAP_NITROS_1 render
 * stand-in. Measured floor ~1900x900 Hero @ g_scale 120 => half_x 7.9, half_z
 * 3.87 sim units; the player spawns near a corner so the spawns sit near the
 * corners (the render maps distance-to-walls, frozen at the spawn). NO pillars
 * (open floor). Extents + spawns are measurement-derived — tune by feel. */
static const ArenaGeom arena_nitros_standin = {
    Q(7.9), Q(3.87), NULL, 0,
    { { Q(-7.8), 0, Q(-3.8) }, { Q(7.7), 0, Q(3.8) },
      { Q(-7.8), 0, Q( 3.8) }, { Q(7.7), 0, Q(-3.8) } },
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
