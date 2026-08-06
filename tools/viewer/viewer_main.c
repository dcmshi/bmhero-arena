/* arena_viewer — SDL3 debug viewer over the deterministic arena sim.
 * Dev tool: floats allowed here; the sim (src/arena/) stays pure.
 *
 * The match runs through SyncSession (couch by default — same code path as
 * online, per the design's local==online insurance). Online goes through the A3
 * lobby: no manual addressing, no hand-matched ports, no fixed seed.
 *   --host <server_ip[:port]> [--players N]   host a match; prints a lobby code
 *   --join <CODE>                             join that code
 * --frames N : deterministic smoke run — sessionless, exactly one tick per
 *              frame with neutral inputs, prints "frames N tick T hash H".
 * --seed X   : match seed (couch/smoke only; online seeds come from the lobby).
 * --inject N : DEBUG, online only — corrupt our own sim at tick N to demonstrate
 *              the desync UX on demand (same one-shot hook as the mesh client).
 *
 * On a desync the match stops being fed but keeps rendering the frozen state,
 * and a bundle is written for tools/replay_bundle. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "arena/arena_sim.h"
#include "viewer_cam.h"
#include "viewer_clock.h"
#include "viewer_draw.h"
#include "sync_session.h"
#include "lobby_client.h"
#include "net_adapter.h"

/* The lobby bootstrap runs before SDL_Init, so SDL_Delay is not available yet. */
#ifdef _WIN32
#include <windows.h>
static void sleep_ms_portable(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms_portable(int ms) { usleep(ms * 1000); }
#endif

/* "a.b.c.d[:port]" -> host-order LobbyEndpoint; missing port = the default.
 * Dotted quad only — no DNS, per spec. 0 = ok. */
static int viewer_parse_server(const char* s, LobbyEndpoint* out) {
    unsigned a, b, c, d, p = LOBBY_DEFAULT_PORT;
    if (!s || !out) return -1;
    if (sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p) != 5) {
        p = LOBBY_DEFAULT_PORT;
        if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65535) return -1;
    out->ip   = (a << 24) | (b << 16) | (c << 8) | d;
    out->port = (uint16_t)p;
    return 0;
}

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
    int players_arg = 2;
    int inject = -1;
    const char* host_arg = NULL;
    const char* join_code = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--frames") == 0) frames_limit = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--seed") == 0) seed = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        if (strcmp(argv[i], "--host") == 0) host_arg = argv[i + 1];
        if (strcmp(argv[i], "--join") == 0) join_code = argv[i + 1];
        if (strcmp(argv[i], "--players") == 0) players_arg = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--inject") == 0) inject = atoi(argv[i + 1]);
    }
    const int smoke = frames_limit >= 0;
    const int online = !smoke && (host_arg != NULL || join_code != NULL);
    if (!online) inject = -1;   /* debug hook is online-only; no-op elsewhere */
    if (players_arg < 2) players_arg = 2;
    if (players_arg > ARENA_MAX_PLAYERS) players_arg = ARENA_MAX_PLAYERS;

    /* --- lobby bootstrap, BEFORE SDL_Init: it blocks in the console, which is
     * fine for a debug tool and keeps the code path identical to the mesh
     * client's. The lobby carries the seed, the slot and the routes. --- */
    SyncSession* session = NULL;
    int local_player = 0;
    LobbyClient lc; memset(&lc, 0, sizeof lc);
    UdpSocket sock;
    GekkoNetAdapter* ga = NULL;
    if (online) {
        if (udp_global_init() != 0 || udp_open(&sock, 0) != 0) {
            fprintf(stderr, "viewer: udp init failed\n"); return 1;
        }
        if (host_arg) {
            LobbyEndpoint srv;
            if (viewer_parse_server(host_arg, &srv) != 0) {
                fprintf(stderr, "bad --host (want a.b.c.d[:port])\n"); return 2;
            }
            /* seed varies per match so consecutive hosts are not identical runs */
            lobby_host_begin(&lc, &sock, srv, (uint8_t)players_arg,
                             0xB0BB1E5u ^ (uint32_t)time(NULL), 0, 1, udp_now_ms());
        } else if (lobby_join_begin(&lc, &sock, join_code, udp_now_ms()) != 0) {
            fprintf(stderr, "bad code\n"); return 2;
        }
        LobbyClientStage st; int shown = 0;
        do {
            st = lobby_poll(&lc, udp_now_ms());
            if (host_arg && !shown && lobby_code(&lc)[0]) {
                printf("lobby code: %s  (waiting for %d players...)\n",
                       lobby_code(&lc), players_arg); fflush(stdout);
                shown = 1;
            }
            sleep_ms_portable(5);
        } while (st != LOBBY_C_READY && st != LOBBY_C_FAILED);
        if (st == LOBBY_C_FAILED) {
            fprintf(stderr, "lobby failed at stage %s\n", lobby_fail_stage(&lc));
            return 1;
        }
        const LobbyResult* lr = lobby_result(&lc);
        if (!lr) { fprintf(stderr, "lobby: no result\n"); return 1; }
        ga = net_adapter_init(&sock, lr->session_id, lr->local_slot);
        if (!ga) { fprintf(stderr, "net adapter init failed\n"); return 1; }
        for (int i = 0; i < lr->num_players; i++)
            if (i != lr->local_slot) net_adapter_set_route((uint8_t)i, lr->route[i]);
        net_adapter_set_relay(lr->server);
        SyncConfig c = {0};
        c.mode = SYNC_ONLINE; c.num_players = lr->num_players;
        c.local_mask = (uint8_t)(1u << lr->local_slot);
        c.seed = lr->seed; c.arena_id = lr->arena_id;
        c.input_delay = lr->input_delay; c.adapter = ga;
        local_player = lr->local_slot;
        session = sync_create(&c);
        if (!session) { fprintf(stderr, "sync_create failed\n"); return 1; }
        printf("match: %dP, slot %d, seed %08x, input_delay %u\n",
               lr->num_players, lr->local_slot, lr->seed, lr->input_delay);
        fflush(stdout);
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

    if (!smoke && !online) {
        session = make_couch(2, seed);
        if (!session) { fprintf(stderr, "sync_create failed\n"); return 1; }
    }

    Pads pads = {0};
    ViewerCam cam;   vcam_init(&cam);
    ViewerClock clk; vclock_init(&clk);
    int cam_target = 0, show_grid = 1, running = 1, frame = 0;
    int allow_sd = 0;   /* sudden-death walls off by default (F2), couch only */
    int desync_latched = 0;
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

        if (online) {
            lobby_post_poll(&lc, udp_now_ms());
            /* DEBUG one-shot: arm the corruption once we reach the tick, so the
             * desync UX below can be demonstrated during a real-WAN checkpoint. */
            if (inject >= 0 && (int)sync_present_tick(session) >= inject) {
                sync_debug_corrupt(session);
                inject = -1;
            }
            /* First desync only: say what happened, leave a bundle behind, and
             * stop feeding the match. Rendering continues on the frozen state so
             * the last frame before the divergence stays on screen. */
            if (sync_desynced(session) && !desync_latched) {
                desync_latched = 1;
                SyncDesyncInfo di;
                if (sync_desync_info(session, &di))
                    fprintf(stderr, "DESYNC tick=%u local=%08x remote=%08x\n",
                            di.tick, di.local_hash, di.remote_hash);
                char p[128];
                snprintf(p, sizeof p, "desync_viewer_slot%d.bin", local_player);
                if (sync_dump_bundle(session, p) == 0)
                    fprintf(stderr, "bundle written: %s (run replay_bundle on it)\n", p);
                SDL_SetWindowTitle(win, "bmhero arena viewer - DESYNC (match stopped)");
            }
        }

        int n = smoke ? 1 : vclock_advance(&clk, ms);
        if (!desync_latched) for (int t = 0; t < n; t++) {
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
            /* desync is checked FIRST: connected stays true through one, so the
             * old order printed SYNCED on a desynced session */
            snprintf(net, sizeof net, "NET %s %s  P%d",
                     online ? "ONLINE" : "COUCH",
                     sync_desynced(session) ? "DESYNC!"
                     : (sync_connected(session) ? "SYNCED" : "CONNECTING"),
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
    if (online) net_adapter_shutdown();
    SDL_Quit();
    return 0;
}
