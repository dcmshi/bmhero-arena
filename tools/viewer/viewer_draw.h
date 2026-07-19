#ifndef VIEWER_DRAW_H
#define VIEWER_DRAW_H

#include <SDL3/SDL.h>
#include "arena/arena_sim.h"
#include "arena/arena_geom.h"
#include "arena/arena_tuning.h"
#include "viewer_cam.h"
#include "viewer_clock.h"

#define QF(v) ((float)(v) / 4096.0f)   /* Q20.12 -> float (render side only) */

void draw_scene(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s,
                int w, int h, int show_grid);
void draw_facing(SDL_Renderer* r, const ViewerCam* cam, const ArenaState* s,
                 int w, int h);
void draw_hud(SDL_Renderer* r, const ArenaState* s, const ViewerClock* clk,
              const ViewerCam* cam, int cam_target, int w, int h);
void draw_text(SDL_Renderer* r, float x, float y, float scale, const char* str);

#endif
