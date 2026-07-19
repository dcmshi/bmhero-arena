#ifndef VIEWER_CAM_H
#define VIEWER_CAM_H

/* Chase / top-down camera, projection, and camera-relative stick mapping.
 * Pure float math, no SDL — unit-tested headlessly (tests/test_viewer_cam.c).
 * Conventions: world +Y up; sim "stick up" = world -Z; camera yaw 0 looks
 * along -Z, so camera yaw tracks the sim's binary-angle facing directly. */

typedef struct { float x, y, z; } Vf3;

typedef struct {
    /* rig constants — viewer-only, tune freely */
    float dist, height, look_up;   /* boom length, eye height, aim height */
    float smooth;                  /* yaw lerp per update, 0..1 */
    float fov_deg;
    float ortho_halfspan;          /* top-down: world units center -> edge */
    /* state */
    float yaw;                     /* radians */
    int   topdown;
    Vf3   pos, target;             /* derived by vcam_update */
} ViewerCam;

void  vcam_init(ViewerCam* c);
void  vcam_update(ViewerCam* c, Vf3 player_pos, float player_yaw_rad);
/* World -> screen. 0 if behind camera (chase mode). depth: view-space
 * distance for painter sorting (bigger = farther). */
int   vcam_project(const ViewerCam* c, Vf3 p, int w, int h,
                   float* sx, float* sy, float* depth);
/* Projected pixel radius of a world-space sphere; 0 if behind camera. */
float vcam_screen_radius(const ViewerCam* c, Vf3 p, float world_r, int w, int h);
/* Camera-relative stick (in_x right+, in_y forward+, [-1,1]) -> sim stick
 * ints in [-31,31] on world X/Z. Identity mapping in top-down mode. */
void  vcam_stick_to_world(const ViewerCam* c, float in_x, float in_y,
                          int* out_sx, int* out_sy);

#define VCAM_BINANG_TO_RAD(a) ((float)(a) * 9.5873799e-5f) /* 2*pi / 65536 */

#endif
