/* Unit tests for the viewer camera: rig, smoothing, projection, stick map. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../tools/viewer/viewer_cam.h"

#define PI_F 3.14159265f
static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

int main(void) {
    ViewerCam c;
    float sx, sy, d;

    /* rig: instant follow puts the camera behind and above a -Z-facing player */
    vcam_init(&c);
    c.smooth = 1.0f;
    vcam_update(&c, (Vf3){0, 0, 0}, 0.0f);
    CHECK(NEAR(c.pos.x, 0, 1e-4f) && NEAR(c.pos.y, 5.0f, 1e-4f) && NEAR(c.pos.z, 4.0f, 1e-4f),
          "camera at (0, height, +dist) behind -Z-facing player");
    CHECK(NEAR(c.target.y, 1.0f, 1e-4f), "look target lifted by look_up");

    /* yaw smoothing takes the short way across +/-pi */
    vcam_init(&c);
    c.yaw = 2.8f; c.smooth = 0.25f;
    vcam_update(&c, (Vf3){0, 0, 0}, -2.8f);
    CHECK(c.yaw > 2.8f || c.yaw < -2.8f, "yaw wraps across pi, no long-way spin");

    /* opposition: walking straight at the camera must not swing the yaw —
     * the swing direction is ambiguous at ~180 and flips sign every tick */
    vcam_init(&c);
    c.yaw = 0.0f; c.smooth = 0.25f;
    vcam_update(&c, (Vf3){0, 0, 0}, PI_F);
    float y_opp1 = c.yaw;
    vcam_update(&c, (Vf3){0, 0, 0}, -PI_F);   /* pi's sign flips tick-to-tick */
    CHECK(NEAR(y_opp1, 0, 1e-4f) && NEAR(c.yaw, 0, 1e-4f),
          "opposition deadband: camera holds when player runs at it");
    vcam_update(&c, (Vf3){0, 0, 0}, 2.0f);
    CHECK(c.yaw > 0.01f && c.yaw <= c.max_turn + 1e-4f,
          "large diffs still move, but rate-limited");

    /* big swings are rate-limited: recovering from near-opposition must not
     * whip the camera around at smooth*diff speed */
    vcam_init(&c);
    c.yaw = 0.0f; c.smooth = 0.25f;
    vcam_update(&c, (Vf3){0, 0, 0}, 2.5f);
    CHECK(NEAR(c.yaw, c.max_turn, 1e-4f), "turn speed capped at max_turn/update");
    /* small corrections (normal forward running) are NOT rate-limited */
    vcam_init(&c);
    c.yaw = 0.0f; c.smooth = 0.25f; c.max_turn = 1.0f;  /* isolate proportional */
    vcam_update(&c, (Vf3){0, 0, 0}, 0.1f);
    CHECK(NEAR(c.yaw, 0.025f, 1e-4f), "small diffs use proportional follow");

    /* perspective projection */
    vcam_init(&c);
    c.smooth = 1.0f;
    vcam_update(&c, (Vf3){0, 0, 0}, 0.0f);
    CHECK(vcam_project(&c, c.target, 1280, 720, &sx, &sy, &d) == 1, "target visible");
    CHECK(NEAR(sx, 640, 0.5f) && NEAR(sy, 360, 0.5f), "look target projects to center");
    CHECK(d > 0, "depth positive");
    CHECK(vcam_project(&c, (Vf3){1, 1, 0}, 1280, 720, &sx, &sy, &d) == 1, "offset visible");
    CHECK(sx > 640.0f, "point right of view projects right of center");
    CHECK(vcam_project(&c, (Vf3){0, 9, 8}, 1280, 720, &sx, &sy, &d) == 0, "behind camera culled");

    float d_near, d_far;
    vcam_project(&c, (Vf3){0, 1, 0}, 1280, 720, &sx, &sy, &d_near);
    vcam_project(&c, (Vf3){0, 1, -3}, 1280, 720, &sx, &sy, &d_far);
    CHECK(d_near < d_far, "painter depth ordering");

    /* screen radius shrinks with distance */
    float r_near = vcam_screen_radius(&c, (Vf3){0, 1, 0}, 0.35f, 1280, 720);
    float r_far  = vcam_screen_radius(&c, (Vf3){0, 1, -3}, 0.35f, 1280, 720);
    CHECK(r_near > r_far && r_far > 0, "screen radius perspective-scales");

    /* top-down ortho */
    vcam_init(&c);
    c.topdown = 1;
    CHECK(vcam_project(&c, (Vf3){0, 0, 0}, 1280, 720, &sx, &sy, &d) == 1, "topdown visible");
    CHECK(NEAR(sx, 640, 0.5f) && NEAR(sy, 360, 0.5f), "topdown origin centers");
    vcam_project(&c, (Vf3){1, 0, 0}, 1280, 720, &sx, &sy, &d);
    CHECK(NEAR(sx, 640 + 48, 0.5f), "topdown scale: 720/(2*7.5) = 48 px per unit");

    /* camera-relative stick mapping */
    int ix, iy;
    vcam_init(&c);
    c.yaw = 0.0f;
    vcam_stick_to_world(&c, 0, 1, &ix, &iy);
    CHECK(ix == 0 && iy == -31, "yaw 0: stick up = world -Z");
    vcam_stick_to_world(&c, 1, 0, &ix, &iy);
    CHECK(ix == 31 && iy == 0, "yaw 0: stick right = world +X");
    c.yaw = PI_F / 2;
    vcam_stick_to_world(&c, 0, 1, &ix, &iy);
    CHECK(ix == 31 && abs(iy) <= 1, "yaw 90: stick up = world +X");
    c.yaw = PI_F;
    vcam_stick_to_world(&c, 0, 1, &ix, &iy);
    CHECK(abs(ix) <= 1 && iy == 31, "yaw 180: stick up = world +Z");
    c.yaw = 0.0f;
    vcam_stick_to_world(&c, 1, 1, &ix, &iy);
    CHECK(ix * ix + iy * iy <= 32 * 32, "diagonal magnitude clamped to 1.0");
    /* top-down bypasses camera yaw */
    c.topdown = 1; c.yaw = PI_F / 2;
    vcam_stick_to_world(&c, 0, 1, &ix, &iy);
    CHECK(ix == 0 && iy == -31, "topdown: identity mapping regardless of yaw");

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("viewer_cam: ALL TESTS PASSED\n");
    return 0;
}
