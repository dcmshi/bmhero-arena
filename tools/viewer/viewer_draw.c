/* All rendering: world geometry + entities through a unified painter list,
 * then screen-space overlays. Floats OK — this is viewer code. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "viewer_draw.h"
#include "viewer_font8.h"   /* font8x8_basic[128][8], public domain */

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

static void add_entities(const ViewerCam* cam, const ArenaState* s, int w, int h) {
    static const SDL_FColor pcol[4] = {
        {0.95f, 0.95f, 0.95f, 1}, {0.25f, 0.25f, 0.30f, 1},
        {0.90f, 0.30f, 0.25f, 1}, {0.30f, 0.42f, 0.90f, 1},
    };
    static const SDL_FColor shadow = {0, 0, 0, 0.35f};

    for (int i = 0; i < s->num_players; i++) {
        const ArenaPlayer* p = &s->players[i];
        Vf3 pos = {QF(p->pos.x), QF(p->pos.y), QF(p->pos.z)};
        SDL_FColor c = pcol[i];
        if (p->state == PSTATE_DEAD) {
            c.a = 0.25f;
        } else {
            if (p->state == PSTATE_TUMBLE)
                c = (SDL_FColor){1.0f, 0.60f, 0.15f, 1};
            else if (p->timer > 0 && ((s->tick >> 2) & 1))
                c.a = 0.45f;                     /* post-hit invuln flash */
            add_circle(cam, w, h, (Vf3){pos.x, 0.01f, pos.z},
                       QF(TUNE_PLAYER_RADIUS), shadow, 0.01f);
        }
        add_circle(cam, w, h, (Vf3){pos.x, pos.y + 0.45f, pos.z},
                   QF(TUNE_PLAYER_RADIUS), c, 0);
    }

    for (int i = 0; i < ARENA_MAX_BOMBS; i++) {
        const ArenaBomb* b = &s->bombs[i];
        if (b->state == BSTATE_FREE || b->state == BSTATE_EXPLODING) continue;
        Vf3 pos = {QF(b->pos.x), QF(b->pos.y), QF(b->pos.z)};
        SDL_FColor c = {0.16f, 0.16f, 0.18f, 1};
        if (b->state == BSTATE_SETTLED && ((b->fuse / 10) & 1))
            c = (SDL_FColor){0.85f, 0.20f, 0.15f, 1};    /* fuse flash */
        if (b->state != BSTATE_HELD)
            add_circle(cam, w, h, (Vf3){pos.x, 0.01f, pos.z},
                       QF(TUNE_BOMB_RADIUS), shadow, 0.01f);
        add_circle(cam, w, h, (Vf3){pos.x, pos.y + 0.3f, pos.z},
                   QF(TUNE_BOMB_RADIUS), c, 0);
    }

    for (int i = 0; i < ARENA_MAX_BLASTS; i++) {
        const ArenaBlast* bl = &s->blasts[i];
        if (bl->ttl == 0) continue;
        float fr = (float)bl->radius_t / (float)TUNE_BLAST_GROW_TICKS;
        if (fr > 1) fr = 1;
        add_circle(cam, w, h,
                   (Vf3){QF(bl->center.x), QF(bl->center.y) + 0.2f, QF(bl->center.z)},
                   fr * QF(TUNE_BLAST_RADIUS),
                   (SDL_FColor){1.0f, 0.55f, 0.10f,
                                0.55f * (float)bl->ttl / (float)TUNE_BLAST_TTL},
                   -0.02f);
    }
}

void draw_facing(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s,
                 int w, int h) {
    SDL_SetRenderDrawColor(r, 250, 250, 250, 255);
    for (int i = 0; i < s->num_players; i++) {
        const ArenaPlayer* p = &s->players[i];
        if (p->state == PSTATE_DEAD) continue;
        float yr = VCAM_BINANG_TO_RAD(p->yaw);
        Vf3 a = {QF(p->pos.x), QF(p->pos.y) + 0.45f, QF(p->pos.z)};
        Vf3 b = {a.x + 0.55f * sinf(yr), a.y, a.z - 0.55f * cosf(yr)};
        draw_world_line(r, cam, a, b, w, h);
    }
}

void draw_text(SDL_Renderer* r, float x, float y, float scale, const char* str) {
    SDL_SetRenderDrawColor(r, 235, 235, 235, 255);
    SDL_FRect px = {0, 0, scale, scale};
    for (; *str; str++, x += 8 * scale) {
        unsigned ch = (unsigned char)*str;
        if (ch >= 128) continue;
        for (int row = 0; row < 8; row++) {
            unsigned bits = (unsigned char)font8x8_basic[ch][row];
            for (int col = 0; col < 8; col++)
                if (bits & (1u << col)) {
                    px.x = x + (float)col * scale;
                    px.y = y + (float)row * scale;
                    SDL_RenderFillRect(r, &px);
                }
        }
    }
}

void draw_hud(SDL_Renderer* r, const ArenaState* s, const ViewerClock* clk,
              const ViewerCam* cam, int cam_target, int w, int h) {
    static const char* pstate[] = {"IDLE", "RUN ", "JUMP", "TUMB", "DEAD"};
    static const char* phase[]  = {"COUNTDOWN", "PLAY", "SUDDEN-DEATH", "ROUND-END"};
    static const char* rate[]   = {"1x", "1/4x", "1/16x"};
    char line[160];
    (void)w;

    snprintf(line, sizeof line, "TICK %-8u HASH %08x  %s %d  RATE %s%s  CAM P%d %s",
             s->tick, arena_hash(s), phase[s->phase], (int)s->phase_timer,
             rate[clk->rate], clk->paused ? " PAUSED" : "",
             cam_target, cam->topdown ? "TOP" : "CHASE");
    draw_text(r, 8, 8, 2, line);

    for (int i = 0; i < s->num_players; i++) {
        const ArenaPlayer* p = &s->players[i];
        snprintf(line, sizeof line,
                 "P%d %s hp%d pos %+6.2f %+6.2f %+6.2f vel %+5.2f %+5.2f %+5.2f bombs %d t%d",
                 i, pstate[p->state], (int)p->hp,
                 QF(p->pos.x), QF(p->pos.y), QF(p->pos.z),
                 QF(p->vel.x), QF(p->vel.y), QF(p->vel.z),
                 (int)p->live_bombs, (int)p->timer);
        draw_text(r, 8, 8 + 20 * (float)(i + 1), 2, line);
    }

    draw_text(r, 8, (float)h - 24, 2,
              "P pause  ] step  [ rate  R reset  TAB cam  F1 view  G grid  ESC quit");
}
