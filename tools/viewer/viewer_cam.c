#include <math.h>
#include "viewer_cam.h"

#define PIF 3.14159265f

static float wrap_pi(float a) {
    while (a >  PIF) a -= 2 * PIF;
    while (a < -PIF) a += 2 * PIF;
    return a;
}

void vcam_init(ViewerCam* c) {
    c->dist = 4.0f; c->height = 5.0f; c->look_up = 1.0f;   /* ~45 deg pitch */
    c->smooth = 0.08f; c->max_turn = 0.045f;   /* ~2.6 deg/frame swing cap */
    c->fov_deg = 60.0f; c->ortho_halfspan = 7.5f;
    c->yaw = 0.0f; c->topdown = 0;
    c->pos = (Vf3){0, c->height, c->dist};
    c->target = (Vf3){0, c->look_up, 0};
}

void vcam_update(ViewerCam* c, Vf3 p, float yaw_rad) {
    float diff = wrap_pi(yaw_rad - c->yaw);
    /* Near-opposition (player running at the camera) the swing direction is
     * ambiguous and diff's sign flips tick-to-tick -> jitter. Hold instead. */
    if (fabsf(diff) < 2.9f) {
        float step = c->smooth * diff;
        /* proportional follow whips on big diffs (deadband recovery): cap it */
        if (step >  c->max_turn) step =  c->max_turn;
        if (step < -c->max_turn) step = -c->max_turn;
        c->yaw = wrap_pi(c->yaw + step);
    }
    c->target = (Vf3){ p.x, p.y + c->look_up, p.z };
    float fx = sinf(c->yaw), fz = -cosf(c->yaw);   /* look dir on XZ */
    c->pos = (Vf3){ p.x - fx * c->dist, p.y + c->height, p.z - fz * c->dist };
}

static Vf3   vsub(Vf3 a, Vf3 b) { return (Vf3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static float vdot(Vf3 a, Vf3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vf3   vcross(Vf3 a, Vf3 b) {
    return (Vf3){a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static Vf3 vnorm(Vf3 a) {
    float l = sqrtf(vdot(a, a));
    return (l > 1e-6f) ? (Vf3){a.x / l, a.y / l, a.z / l} : (Vf3){0, 0, 0};
}

int vcam_project(const ViewerCam* c, Vf3 p, int w, int h,
                 float* sx, float* sy, float* depth) {
    if (c->topdown) {
        float scale = (float)(w < h ? w : h) / (2.0f * c->ortho_halfspan);
        *sx = (float)w * 0.5f + p.x * scale;
        *sy = (float)h * 0.5f + p.z * scale;
        *depth = 1000.0f - p.y;   /* higher = nearer */
        return 1;
    }
    Vf3 F = vnorm(vsub(c->target, c->pos));
    Vf3 R = vnorm(vcross(F, (Vf3){0, 1, 0}));
    Vf3 U = vcross(R, F);
    Vf3 v = vsub(p, c->pos);
    float dz = vdot(v, F);
    if (dz < 0.1f) return 0;
    float f = ((float)h * 0.5f) / tanf(c->fov_deg * 0.5f * PIF / 180.0f);
    *sx = (float)w * 0.5f + f * vdot(v, R) / dz;
    *sy = (float)h * 0.5f - f * vdot(v, U) / dz;
    *depth = dz;
    return 1;
}

float vcam_screen_radius(const ViewerCam* c, Vf3 p, float world_r, int w, int h) {
    float sx0, sy0, sx1, sy1, d;
    if (!vcam_project(c, p, w, h, &sx0, &sy0, &d)) return 0;
    if (c->topdown) {
        float scale = (float)(w < h ? w : h) / (2.0f * c->ortho_halfspan);
        return world_r * scale;
    }
    Vf3 F = vnorm(vsub(c->target, c->pos));
    Vf3 R = vnorm(vcross(F, (Vf3){0, 1, 0}));
    Vf3 q = { p.x + R.x * world_r, p.y + R.y * world_r, p.z + R.z * world_r };
    if (!vcam_project(c, q, w, h, &sx1, &sy1, &d)) return 0;
    float dx = sx1 - sx0, dy = sy1 - sy0;
    return sqrtf(dx * dx + dy * dy);
}

void vcam_stick_to_world(const ViewerCam* c, float in_x, float in_y,
                         int* out_sx, int* out_sy) {
    float wx, wz;
    if (c->topdown) {
        wx = in_x; wz = -in_y;
    } else {
        float fx = sinf(c->yaw), fz = -cosf(c->yaw);   /* camera forward, XZ */
        float rx = -fz,          rz = fx;              /* camera right, XZ */
        wx = rx * in_x + fx * in_y;
        wz = rz * in_x + fz * in_y;
    }
    float m = sqrtf(wx * wx + wz * wz);
    if (m > 1.0f) { wx /= m; wz /= m; }
    *out_sx = (int)lroundf(wx * 31.0f);
    *out_sy = (int)lroundf(wz * 31.0f);
}
