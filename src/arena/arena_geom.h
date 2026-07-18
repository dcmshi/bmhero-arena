/* Static arena geometry — NOT part of ArenaState (identical on all peers by
 * construction; hashed into the session version handshake).
 * v0: axis-aligned boxes only: floor at y=0, boundary walls, pillars. */
#ifndef ARENA_GEOM_H
#define ARENA_GEOM_H

#include "arena_math.h"

typedef struct { Vec3q min, max; } Aabb;

typedef struct {
    q32   half_extent;         /* square arena, walls at +/- half_extent */
    const Aabb* pillars;
    int   num_pillars;
    Vec3q spawns[4];
} ArenaGeom;

/* Arena 0: 12x12 ring, four pillars. */
static const Aabb arena0_pillars[] = {
    { { Q(-2.5), Q(0), Q(-2.5) }, { Q(-1.5), Q(1.5), Q(-1.5) } },
    { { Q( 1.5), Q(0), Q(-2.5) }, { Q( 2.5), Q(1.5), Q(-1.5) } },
    { { Q(-2.5), Q(0), Q( 1.5) }, { Q(-1.5), Q(1.5), Q( 2.5) } },
    { { Q( 1.5), Q(0), Q( 1.5) }, { Q( 2.5), Q(1.5), Q( 2.5) } },
};

static const ArenaGeom arena_geoms[] = {
    {
        Q(6.0), arena0_pillars, 4,
        { { Q(-4.5), 0, Q(-4.5) }, { Q(4.5), 0, Q(4.5) },
          { Q(-4.5), 0, Q( 4.5) }, { Q(4.5), 0, Q(-4.5) } },
    },
};

#define ARENA_GEOM_COUNT 1

#endif
