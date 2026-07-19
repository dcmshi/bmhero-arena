/* arena_viewer — SDL3 debug viewer over the deterministic arena sim.
 * Dev tool: floats allowed here; the sim (src/arena/) stays pure.
 * --frames N : run N frames then exit (smoke test; deterministic in Task 6). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    int running = 1, frame = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = 0;
        }
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        frame++;
        if (frames_limit >= 0 && frame >= frames_limit) running = 0;
    }
    printf("frames %d\n", frame);
    SDL_Quit();
    return 0;
}
