/* All rendering: world geometry + entities through a unified painter list,
 * then screen-space overlays. Floats OK — this is viewer code. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "viewer_draw.h"

/* ------------------------------------------------- unified painter list */

#define MAX_DRAW 1024

typedef struct {
    int        kind;        /* 0 = quad face, 1 = circle */
    float      depth;
    SDL_FColor col;
    SDL_FPoint p[4];        /* face: projected corners */
    float      cx, cy, rad; /* circle: projected center + pixel radius */
} Drawable;

static Drawable g_draw[MAX_DRAW];
static int      g_ndraw;

static int draw_cmp(const void* pa, const void* pb) {
    float da = ((const Drawable*)pa)->depth, db = ((const Drawable*)pb)->depth;
    return (da < db) - (da > db);   /* descending: far first */
}

static void add_face(const ViewerCam* cam, int w, int h,
                     Vf3 a, Vf3 b, Vf3 c, Vf3 d, SDL_FColor col) {
    if (g_ndraw >= MAX_DRAW) return;
    Vf3 corners[4] = {a, b, c, d};
    Drawable* dr = &g_draw[g_ndraw];
    float depth_sum = 0;
    for (int i = 0; i < 4; i++) {
        float sx, sy, dep;
        if (!vcam_project(cam, corners[i], w, h, &sx, &sy, &dep)) return;
        dr->p[i] = (SDL_FPoint){sx, sy};
        depth_sum += dep;
    }
    /* flat shade from the face normal */
    Vf3 e1 = {b.x - a.x, b.y - a.y, b.z - a.z};
    Vf3 e2 = {d.x - a.x, d.y - a.y, d.z - a.z};
    Vf3 n  = {e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x};
    float l = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
    float lit = 0;
    if (l > 1e-6f) lit = (n.x*0.45f + n.y*0.80f + n.z*0.30f) / l;
    if (lit < 0) lit = -lit;               /* winding-agnostic */
    float shade = 0.6f + 0.4f * lit;
    dr->kind  = 0;
    dr->depth = depth_sum * 0.25f;
    dr->col   = (SDL_FColor){col.r * shade, col.g * shade, col.b * shade, col.a};
    g_ndraw++;
}

static void add_box(const ViewerCam* cam, int w, int h,
                    Vf3 mn, Vf3 mx, SDL_FColor col) {
    /* top + 4 sides (bottom never visible) */
    add_face(cam, w, h, (Vf3){mn.x,mx.y,mn.z}, (Vf3){mx.x,mx.y,mn.z},
                        (Vf3){mx.x,mx.y,mx.z}, (Vf3){mn.x,mx.y,mx.z}, col);
    add_face(cam, w, h, (Vf3){mn.x,mn.y,mn.z}, (Vf3){mx.x,mn.y,mn.z},
                        (Vf3){mx.x,mx.y,mn.z}, (Vf3){mn.x,mx.y,mn.z}, col);
    add_face(cam, w, h, (Vf3){mn.x,mn.y,mx.z}, (Vf3){mx.x,mn.y,mx.z},
                        (Vf3){mx.x,mx.y,mx.z}, (Vf3){mn.x,mx.y,mx.z}, col);
    add_face(cam, w, h, (Vf3){mn.x,mn.y,mn.z}, (Vf3){mn.x,mn.y,mx.z},
                        (Vf3){mn.x,mx.y,mx.z}, (Vf3){mn.x,mx.y,mn.z}, col);
    add_face(cam, w, h, (Vf3){mx.x,mn.y,mn.z}, (Vf3){mx.x,mn.y,mx.z},
                        (Vf3){mx.x,mx.y,mx.z}, (Vf3){mx.x,mx.y,mn.z}, col);
}

static void add_circle(const ViewerCam* cam, int w, int h,
                       Vf3 center, float world_r, SDL_FColor col, float depth_bias) {
    if (g_ndraw >= MAX_DRAW) return;
    float sx, sy, dep;
    if (!vcam_project(cam, center, w, h, &sx, &sy, &dep)) return;
    float rad = vcam_screen_radius(cam, center, world_r, w, h);
    if (rad < 0.5f) return;
    Drawable* dr = &g_draw[g_ndraw++];
    dr->kind = 1; dr->depth = dep + depth_bias; dr->col = col;
    dr->cx = sx; dr->cy = sy; dr->rad = rad;
}

static void draw_circle(SDL_Renderer* r, float cx, float cy, float rad, SDL_FColor col) {
    enum { SEG = 20 };
    SDL_Vertex vtx[SEG + 2];
    int idx[SEG * 3];
    vtx[0].position = (SDL_FPoint){cx, cy};
    for (int i = 0; i <= SEG; i++) {
        float a = (float)i * (2.0f * 3.14159265f / SEG);
        vtx[i + 1].position = (SDL_FPoint){cx + cosf(a) * rad, cy + sinf(a) * rad};
    }
    for (int i = 0; i < SEG + 2; i++) {
        vtx[i].color = col;
        vtx[i].tex_coord = (SDL_FPoint){0, 0};
    }
    int n = 0;
    for (int i = 0; i < SEG; i++) { idx[n++] = 0; idx[n++] = i + 1; idx[n++] = i + 2; }
    SDL_RenderGeometry(r, NULL, vtx, SEG + 2, idx, SEG * 3);
}

static void flush_drawables(SDL_Renderer* r) {
    qsort(g_draw, (size_t)g_ndraw, sizeof(Drawable), draw_cmp);
    for (int i = 0; i < g_ndraw; i++) {
        Drawable* d = &g_draw[i];
        if (d->kind == 0) {
            SDL_Vertex v[4];
            int idx[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 4; k++) {
                v[k].position  = d->p[k];
                v[k].color     = d->col;
                v[k].tex_coord = (SDL_FPoint){0, 0};
            }
            SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
        } else {
            draw_circle(r, d->cx, d->cy, d->rad, d->col);
        }
    }
}

static void draw_world_line(SDL_Renderer* r, const ViewerCam* cam,
                            Vf3 a, Vf3 b, int w, int h) {
    float ax, ay, bx, by, d;
    if (!vcam_project(cam, a, w, h, &ax, &ay, &d)) return;
    if (!vcam_project(cam, b, w, h, &bx, &by, &d)) return;
    SDL_RenderLine(r, ax, ay, bx, by);
}

/* ---------------------------------------------------------------- scene */

static void add_entities(const ViewerCam* cam, const ArenaState* s, int w, int h);

void draw_scene(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s,
                int w, int h, int show_grid) {
    const ArenaGeom* g = &arena_geoms[s->arena_id];
    float full = QF(g->half_extent);
    float ext  = full;
    if (s->phase == PHASE_SUDDEN_DEATH)
        ext -= (float)s->shrink_step * 0.03f;   /* mirrors sim wall shrink */

    /* grid first: always behind everything */
    if (show_grid) {
        SDL_SetRenderDrawColor(r, 45, 50, 62, 255);
        for (int i = -(int)full; i <= (int)full; i++) {
            draw_world_line(r, cam, (Vf3){(float)i, 0, -full}, (Vf3){(float)i, 0, full}, w, h);
            draw_world_line(r, cam, (Vf3){-full, 0, (float)i}, (Vf3){full, 0, (float)i}, w, h);
        }
    }

    g_ndraw = 0;

    /* floor slab, slightly below y=0 so ground entities draw on top */
    add_box(cam, w, h, (Vf3){-full, -0.15f, -full}, (Vf3){full, -0.02f, full},
            (SDL_FColor){0.16f, 0.18f, 0.22f, 1});

    /* boundary walls at the (possibly shrunken) extent; red in sudden death */
    SDL_FColor wallc = (s->phase == PHASE_SUDDEN_DEATH)
                     ? (SDL_FColor){0.55f, 0.25f, 0.25f, 1}
                     : (SDL_FColor){0.30f, 0.34f, 0.42f, 1};
    float wt = 0.20f, wh = 1.2f;
    add_box(cam, w, h, (Vf3){-ext - wt, 0, -ext - wt}, (Vf3){ ext + wt, wh, -ext}, wallc);
    add_box(cam, w, h, (Vf3){-ext - wt, 0,  ext},      (Vf3){ ext + wt, wh,  ext + wt}, wallc);
    add_box(cam, w, h, (Vf3){-ext - wt, 0, -ext},      (Vf3){-ext,      wh,  ext}, wallc);
    add_box(cam, w, h, (Vf3){ ext,      0, -ext},      (Vf3){ ext + wt, wh,  ext}, wallc);

    /* pillars */
    for (int i = 0; i < g->num_pillars; i++) {
        const Aabb* b = &g->pillars[i];
        add_box(cam, w, h,
                (Vf3){QF(b->min.x), QF(b->min.y), QF(b->min.z)},
                (Vf3){QF(b->max.x), QF(b->max.y), QF(b->max.z)},
                (SDL_FColor){0.42f, 0.40f, 0.36f, 1});
    }

    add_entities(cam, s, w, h);
    flush_drawables(r);
}

/* Entities are added in Task 6; keep an empty hook until then. */
static void add_entities(const ViewerCam* cam, const ArenaState* s, int w, int h) {
    (void)cam; (void)s; (void)w; (void)h;
}

/* HUD is implemented in Task 8; stubs keep the link happy. */
void draw_text(SDL_Renderer* r, float x, float y, float scale, const char* str) {
    (void)r; (void)x; (void)y; (void)scale; (void)str;
}
void draw_hud(SDL_Renderer* r, const ArenaState* s, const ViewerClock* clk,
              const ViewerCam* cam, int cam_target, int w, int h) {
    (void)r; (void)s; (void)clk; (void)cam; (void)cam_target; (void)w; (void)h;
}
