/* arena_viewer — SDL3 debug viewer over the deterministic arena sim.
 * Dev tool: floats allowed here; the sim (src/arena/) stays pure.
 * --frames N : deterministic smoke run — exactly one tick per frame with
 *              neutral inputs, prints "frames N tick T hash H", then exits.
 * --seed X   : match seed (hex ok), default 0xC0FFEE. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/arena_sim.h"
#include "viewer_cam.h"
#include "viewer_clock.h"
#include "viewer_draw.h"

typedef struct { SDL_Gamepad* pad[ARENA_MAX_PLAYERS]; } Pads;

static void pads_add(Pads* p, SDL_JoystickID id) {
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        if (!p->pad[i]) { p->pad[i] = SDL_OpenGamepad(id); return; }
}
static void pads_remove(Pads* p, SDL_JoystickID id) {
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        if (p->pad[i] && SDL_GetGamepadID(p->pad[i]) == id) {
            SDL_CloseGamepad(p->pad[i]);
            p->pad[i] = NULL;
            return;
        }
}
/* keyboard drives the lowest player slot without a pad */
static int keyboard_player(const Pads* p) {
    for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
        if (!p->pad[i]) return i;
    return -1;
}

static ArenaInput read_input(const Pads* pads, int player, const ViewerCam* cam) {
    float ix = 0, iy = 0;
    int jump = 0, bomb = 0;
    if (pads->pad[player]) {
        SDL_Gamepad* gp = pads->pad[player];
        ix =  (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        iy = -(float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        jump = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH);
        bomb = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST);
    } else if (player == keyboard_player(pads)) {
        const bool* k = SDL_GetKeyboardState(NULL);
        ix = (float)((k[SDL_SCANCODE_D] ? 1 : 0) - (k[SDL_SCANCODE_A] ? 1 : 0));
        iy = (float)((k[SDL_SCANCODE_W] ? 1 : 0) - (k[SDL_SCANCODE_S] ? 1 : 0));
        jump = k[SDL_SCANCODE_SPACE] ? 1 : 0;
        bomb = k[SDL_SCANCODE_LSHIFT] ? 1 : 0;
    }
    int sx, sy;
    vcam_stick_to_world(cam, ix, iy, &sx, &sy);
    return arena_input_pack(sx, sy, jump, bomb, 0);
}

int main(int argc, char** argv) {
    int frames_limit = -1;
    uint32_t seed = 0xC0FFEE;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--frames") == 0) frames_limit = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--seed") == 0) seed = (uint32_t)strtoul(argv[i + 1], NULL, 0);
    }
    const int smoke = frames_limit >= 0;

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
    arena_init(&state, 0, 2, seed);   /* min 2 players: solo round-ends instantly */

    Pads pads = {0};
    ViewerCam cam;   vcam_init(&cam);
    ViewerClock clk; vclock_init(&clk);
    int cam_target = 0, show_grid = 1, running = 1, frame = 0;
    int allow_sd = 0;   /* sudden-death walls off by default (F2): they cut
                           long feel-testing sessions short */
    uint64_t t_prev = SDL_GetTicksNS();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_GAMEPAD_ADDED:   pads_add(&pads, e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: pads_remove(&pads, e.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.key) {
                case SDLK_ESCAPE:       running = 0; break;
                case SDLK_P:            vclock_toggle_pause(&clk); break;
                case SDLK_RIGHTBRACKET: vclock_queue_step(&clk); break;
                case SDLK_LEFTBRACKET:  vclock_cycle_rate(&clk); break;
                case SDLK_TAB:          cam_target = (cam_target + 1) % state.num_players; break;
                case SDLK_F1:           cam.mode = (cam.mode + 1) % VCAM_NUM_MODES; break;
                case SDLK_F2:           allow_sd = !allow_sd; break;
                case SDLK_G:            show_grid = !show_grid; break;
                case SDLK_R:
                    if (e.key.mod & SDL_KMOD_SHIFT)
                        seed = seed * 1664525u + 1013904223u;
                    arena_init(&state, 0, state.num_players, seed);
                    break;
                default: break;
                }
                break;
            default: break;
            }
        }

        /* players = connected devices, floor 2 (hotplug restarts the match) */
        if (!smoke) {
            int devices = 0;
            for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
                if (pads.pad[i]) devices++;
            if (keyboard_player(&pads) >= 0) devices++;
            int want = devices < 2 ? 2 : devices;
            if (want != state.num_players)
                arena_init(&state, 0, (uint8_t)want, seed);
        }

        uint64_t t_now = SDL_GetTicksNS();
        double ms = (double)(t_now - t_prev) / 1e6;
        t_prev = t_now;

        int n = smoke ? 1 : vclock_advance(&clk, ms);
        for (int t = 0; t < n; t++) {
            ArenaInput in[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
            if (!smoke)
                for (int i = 0; i < state.num_players; i++)
                    in[i] = read_input(&pads, i, &cam);
            arena_tick(&state, in);
        }

        /* Debug-only state surgery (never in smoke runs, sim code untouched):
         * hold the walls open so sudden death can't cut a test session short. */
        if (!smoke && !allow_sd && state.phase == PHASE_SUDDEN_DEATH) {
            state.phase = PHASE_PLAY;
            state.phase_timer = 0;      /* restart the 2:00 round clock */
            state.shrink_step = 0;
        }

        const ArenaPlayer* tp = &state.players[cam_target];
        vcam_update(&cam,
                    (Vf3){QF(tp->pos.x), QF(tp->pos.y), QF(tp->pos.z)},
                    VCAM_BINANG_TO_RAD(tp->yaw));

        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        draw_scene(ren, &cam, &state, w, h, show_grid);
        draw_facing(ren, &cam, &state, w, h);
        draw_hud(ren, &state, &clk, &cam, cam_target, w, h);
        SDL_RenderPresent(ren);
        frame++;
        if (frames_limit >= 0 && frame >= frames_limit) running = 0;
    }
    printf("frames %d tick %u hash %08x\n", frame, state.tick, arena_hash(&state));
    SDL_Quit();
    return 0;
}
