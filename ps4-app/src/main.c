#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "config.h"
#include "downloader.h"
#include "ui.h"
#include "net_client.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("[PS4-App] Inizializzazione PS4 Direct Downloader...\n");

    /* Initialize Networking (curl on desktop, SceHttp/SceNet on PS4) */
    if (!net_init()) {
        fprintf(stderr, "[PS4-App] Errore inizializzazione rete!\n");
        return 1;
    }

    /* Initialize app configuration and download subsystem */
    config_init();
    downloader_init();

    /* Initialize SDL2 */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "[PS4-App] SDL_Init fallita: %s\n", SDL_GetError());
        net_cleanup();
        return 1;
    }

    /* Open available game controllers (DualShock 4 / DualSense / XInput) */
    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController *pad = SDL_GameControllerOpen(i);
            if (pad) {
                printf("[PS4-App] Controller rilevato: %s\n", SDL_GameControllerName(pad));
            }
        }
    }

    /* Create Window & Renderer */
    Uint32 window_flags = SDL_WINDOW_SHOWN;
#ifdef __ORBIS__
    /* On PS4 OrbisOS, run fullscreen 1080p */
    window_flags |= SDL_WINDOW_FULLSCREEN;
    int win_w = 1920;
    int win_h = 1080;
#else
    /* On Desktop Simulator, default to 1280x720 resizable window */
    window_flags |= SDL_WINDOW_RESIZABLE;
    int win_w = 1280;
    int win_h = 720;
#endif

    SDL_Window *window = SDL_CreateWindow(
        "PS4 Direct Downloader (Simulation & Native)",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        win_w,
        win_h,
        window_flags
    );

    if (!window) {
        fprintf(stderr, "[PS4-App] Creazione finestra fallita: %s\n", SDL_GetError());
        SDL_Quit();
        net_cleanup();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (!renderer) {
        fprintf(stderr, "[PS4-App] Creazione renderer fallita: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        net_cleanup();
        return 1;
    }

    /* Logical 1080p resolution ensures uniform rendering across any TV/monitor */
    SDL_RenderSetLogicalSize(renderer, 1920, 1080);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    /* Initialize UI */
    ui_init(renderer);

    printf("[PS4-App] Loop principale avviato (60 FPS)...\n");

    bool running = true;
    SDL_Event event;
    Uint32 last_ticks = SDL_GetTicks();

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
                SDL_GameControllerOpen(event.cdevice.which);
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else {
                    ui_handle_input(&event);
                }
            } else {
                ui_handle_input(&event);
            }
        }

        downloader_update_metrics();
        ui_render(renderer);

        /* 60 FPS frame limiter */
        Uint32 frame_time = SDL_GetTicks() - last_ticks;
        if (frame_time < 16) {
            SDL_Delay(16 - frame_time);
        }
        last_ticks = SDL_GetTicks();
    }

    printf("[PS4-App] Chiusura applicazione...\n");
    downloader_stop();
    ui_destroy();
    config_save();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    net_cleanup();

    return 0;
}
