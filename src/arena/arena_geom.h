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

/* Arena 0 (2026-07-24): matched to the MAP_NITROS_1 render stand-in — the sim's
 * collidable bounds MUST track the rendered map's real floor or the player hits
 * invisible walls (integration notes 8.5a). Measured Nitros floor ~1900x900 Hero
 * @ g_scale 120 => half_x ~7.9, half_z ~3.87 sim units; the player spawns near a
 * corner, so spawns sit near the corners (the render maps distance-to-walls,
 * frozen at the spawn). NO pillars (open floor). The original 12x12 + 4-pillar
 * battle arena is deferred until a matching map is built/rendered. Extents +
 * spawns are measurement-derived — tune by feel. */
static const ArenaGeom arena_geoms[] = {
    {
        Q(7.9), Q(3.87), NULL, 0,
        { { Q(-7.8), 0, Q(-3.8) }, { Q(7.7), 0, Q(3.8) },
          { Q(-7.8), 0, Q( 3.8) }, { Q(7.7), 0, Q(-3.8) } },
    },
};

#define ARENA_GEOM_COUNT 1

#endif
