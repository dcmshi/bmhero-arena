/* arena_viewer — SDL3 debug viewer over the deterministic arena sim.
 * Dev tool: floats allowed here; the sim (src/arena/) stays pure.
 *
 * The match runs through SyncSession (couch by default — same code path as
 * online, per the design's local==online insurance):
 *   --host <port> --peer <ip:port>            2P online, we are player 0
 *   --join <ip:port> [--port P] [--player K]  2P online, we are player K (1)
 * --frames N : deterministic smoke run — sessionless, exactly one tick per
 *              frame with neutral inputs, prints "frames N tick T hash H".
 * --seed X   : match seed (couch/smoke), default 0xC0FFEE. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/arena_sim.h"
#include "viewer_cam.h"
#include "viewer_clock.h"
#include "viewer_draw.h"
#include "sync_session.h"

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
    int jump = 0, bomb = 0, set = 0;
    if (pads->pad[player]) {
        SDL_Gamepad* gp = pads->pad[player];
        ix =  (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        iy = -(float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        jump = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH);
        bomb = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST);
        set  = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST);
    } else if (player == keyboard_player(pads)) {
        const bool* k = SDL_GetKeyboardState(NULL);
        ix = (float)((k[SDL_SCANCODE_D] ? 1 : 0) - (k[SDL_SCANCODE_A] ? 1 : 0));
        iy = (float)((k[SDL_SCANCODE_W] ? 1 : 0) - (k[SDL_SCANCODE_S] ? 1 : 0));
        jump = k[SDL_SCANCODE_SPACE] ? 1 : 0;
        bomb = k[SDL_SCANCODE_LSHIFT] ? 1 : 0;
        set  = k[SDL_SCANCODE_E] ? 1 : 0;
    }
    int sx, sy;
    vcam_stick_to_world(cam, ix, iy, &sx, &sy);
    return arena_input_pack(sx, sy, jump, bomb, set);
}

static SyncSession* make_couch(int players, uint32_t seed) {
    SyncConfig c = {0};
    c.mode = SYNC_COUCH;
    c.num_players = (uint8_t)(players < 2 ? 2 : players);
    c.local_mask = (uint8_t)((1u << c.num_players) - 1u);
    c.seed = seed;
    return sync_create(&c);
}

int main(int argc, char** argv) {
    int frames_limit = -1;
    uint32_t seed = 0xC0FFEE;
    int host_port = 0, my_port = 7102, join_player = 1;
    const char* join_addr = NULL;
    const char* peer_addr = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--frames") == 0) frames_limit = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--seed") == 0) seed = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        if (strcmp(argv[i], "--host") == 0) host_port = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--peer") == 0) peer_addr = argv[i + 1];
        if (strcmp(argv[i], "--join") == 0) join_addr = argv[i + 1];
        if (strcmp(argv[i], "--port") == 0) my_port = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--player") == 0) join_player = atoi(argv[i + 1]);
    }
    const int smoke = frames_limit >= 0;
    const int online = !smoke && (host_port != 0 || join_addr != NULL);
    if (host_port && !peer_addr) {
        fprintf(stderr, "--host needs --peer <joiner ip:port> (A2: manual addressing)\n");
        return 2;
    }

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

    /* smoke keeps a sessionless sim so its pinned hash never moves */
    ArenaState state;
    arena_init(&state, 0, 2, seed);

    SyncSession* session = NULL;
    int local_player = 0;
    if (!smoke) {
        if (online) {
            SyncConfig c = {0};
            c.mode = SYNC_ONLINE;
            c.num_players = 2;
            c.input_delay = 1;
            c.seed = 0xB0BB1E5;         /* fixed online seed until A3 lobby */
            if (host_port) {
                local_player = 0;
                c.local_mask = 0x01;
                c.local_port = (uint16_t)host_port;
                c.peer_addr[1] = peer_addr;
            } else {
                local_player = join_player;
                c.local_mask = (uint8_t)(1u << join_player);
                c.local_port = (uint16_t)my_port;
                c.peer_addr[join_player ? 0 : 1] = join_addr;
            }
            session = sync_create(&c);
        } else {
            session = make_couch(2, seed);
        }
        if (!session) { fprintf(stderr, "sync_create failed\n"); return 1; }
    }

    Pads pads = {0};
    ViewerCam cam;   vcam_init(&cam);
    ViewerClock clk; vclock_init(&clk);
    int cam_target = 0, show_grid = 1, running = 1, frame = 0;
    int allow_sd = 0;   /* sudden-death walls off by default (F2), couch only */
    uint64_t t_prev = SDL_GetTicksNS();

    while (running) {
        const ArenaState* rs = smoke ? &state : sync_state(session);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_GAMEPAD_ADDED:   pads_add(&pads, e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: pads_remove(&pads, e.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.key) {
                case SDLK_ESCAPE:       running = 0; break;
                case SDLK_P:            if (!online) vclock_toggle_pause(&clk); break;
                case SDLK_RIGHTBRACKET: if (!online) vclock_queue_step(&clk); break;
                case SDLK_LEFTBRACKET:  if (!online) vclock_cycle_rate(&clk); break;
                case SDLK_TAB:          cam_target = (cam_target + 1) % rs->num_players; break;
                case SDLK_F1:           cam.mode = (cam.mode + 1) % VCAM_NUM_MODES; break;
                case SDLK_F2:           allow_sd = !allow_sd; break;
                case SDLK_G:            show_grid = !show_grid; break;
                case SDLK_R:
                    if (!smoke && !online) {
                        if (e.key.mod & SDL_KMOD_SHIFT)
                            seed = seed * 1664525u + 1013904223u;
                        sync_destroy(session);
                        session = make_couch(rs->num_players, seed);
                        if (!session) { fprintf(stderr, "sync_create failed\n"); return 1; }
                    }
                    break;
                default: break;
                }
                break;
            default: break;
            }
        }
        rs = smoke ? &state : sync_state(session);   /* session may be new */

        /* couch: players follow devices (floor 2); recreate on change */
        if (!smoke && !online) {
            int devices = 0;
            for (int i = 0; i < ARENA_MAX_PLAYERS; i++)
                if (pads.pad[i]) devices++;
            if (keyboard_player(&pads) >= 0) devices++;
            int want = devices < 2 ? 2 : devices;
            if (want != rs->num_players) {
                sync_destroy(session);
                session = make_couch(want, seed);
                if (!session) { fprintf(stderr, "sync_create failed\n"); return 1; }
                rs = sync_state(session);
            }
        }

        uint64_t t_now = SDL_GetTicksNS();
        double ms = (double)(t_now - t_prev) / 1e6;
        t_prev = t_now;

        int n = smoke ? 1 : vclock_advance(&clk, ms);
        for (int t = 0; t < n; t++) {
            ArenaInput in[ARENA_MAX_PLAYERS] = {0, 0, 0, 0};
            if (smoke) {
                arena_tick(&state, in);
            } else if (online) {
                in[local_player] = read_input(&pads, 0, &cam);
                sync_frame(session, in);
            } else {
                for (int i = 0; i < rs->num_players; i++)
                    in[i] = read_input(&pads, i, &cam);
                sync_frame(session, in);
            }
        }

        /* debug-only state surgery, couch only (would desync a peer):
         * hold the walls open so sudden death can't cut a session short */
        if (!smoke && !online) {
            ArenaState* ms = sync_state_debug_mut(session);
            if (ms && !allow_sd && ms->phase == PHASE_SUDDEN_DEATH) {
                ms->phase = PHASE_PLAY;
                ms->phase_timer = 0;
                ms->shrink_step = 0;
            }
        }

        rs = smoke ? &state : sync_state(session);
        if (cam_target >= rs->num_players) cam_target = 0;
        const ArenaPlayer* tp = &rs->players[cam_target];
        vcam_update(&cam,
                    (Vf3){QF(tp->pos.x), QF(tp->pos.y), QF(tp->pos.z)},
                    VCAM_BINANG_TO_RAD(tp->yaw));

        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);
        draw_scene(ren, &cam, rs, w, h, show_grid);
        draw_facing(ren, &cam, rs, w, h);
        draw_hud(ren, rs, &clk, &cam, cam_target, w, h);
        if (!smoke) {
            char net[96];
            snprintf(net, sizeof net, "NET %s %s  P%d",
                     online ? "ONLINE" : "COUCH",
                     sync_connected(session) ? "SYNCED"
                     : (sync_desynced(session) ? "DESYNC!" : "CONNECTING"),
                     local_player);
            draw_text(ren, 8, 8 + 20.0f * (float)(ARENA_MAX_PLAYERS + 1), 2, net);
        }
        SDL_RenderPresent(ren);
        frame++;
        if (frames_limit >= 0 && frame >= frames_limit) running = 0;
    }
    const ArenaState* fs = smoke ? &state : sync_state(session);
    printf("frames %d tick %u hash %08x\n", frame, fs->tick, arena_hash(fs));
    if (session) sync_destroy(session);
    SDL_Quit();
    return 0;
}
