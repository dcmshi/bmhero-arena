/* arena_viewer — SDL3 debug viewer over the deterministic arena sim.
 * Dev tool: floats allowed here; the sim (src/arena/) stays pure.
 * --frames N : run N frames then exit (smoke test). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/arena_sim.h"
#include "viewer_cam.h"
#include "viewer_clock.h"
#include "viewer_draw.h"

int main(int argc, char** argv) {
    int frames_limit = -1;
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--frames") == 0) frames_limit = atoi(argv[i + 1]);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("bmhero arena viewer", 1280, 720,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!win || !ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    ArenaState state;
    arena_init(&state, 0, 2, 0xC0FFEE);

    ViewerCam cam;
    vcam_init(&cam);

    int running = 1, frame = 0, show_grid = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE) running = 0;
                if (e.key.key == SDLK_F1) cam.topdown = !cam.topdown;
                if (e.key.key == SDLK_G)  show_grid = !show_grid;
            }
        }

        /* fixed camera over spawn 0 until the sim is wired in (Task 6) */
        vcam_update(&cam, (Vf3){-4.5f, 0, -4.5f}, 0.0f);

        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        draw_scene(ren, &cam, &state, w, h, show_grid);
        SDL_RenderPresent(ren);
        frame++;
        if (frames_limit >= 0 && frame >= frames_limit) running = 0;
    }
    printf("frames %d\n", frame);
    SDL_Quit();
    return 0;
}
