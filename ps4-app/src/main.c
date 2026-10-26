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

    /* Disable stdio buffering for immediate output */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("[PS4-App] Avvio PS4 Direct Downloader...\n");

    /* Initialize Networking (non-fatal if network takes time to connect) */
    if (!net_init()) {
        printf("[PS4-App] Inizializzazione rete in corso...\n");
    }

    config_init();
    downloader_init();

    /* Initialize SDL2 subsystems */
#ifdef __ORBIS__
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) {
        printf("[PS4-App] SDL_Init fallback a VIDEO: %s\n", SDL_GetError());
        SDL_Init(SDL_INIT_VIDEO);
    }
    SDL_Joystick *joystick = NULL;
    if (SDL_NumJoysticks() > 0) {
        joystick = SDL_JoystickOpen(0);
        if (joystick) {
            printf("[PS4-App] Controller Joystick PS4 aperto con successo.\n");
        }
    }
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "[PS4-App] SDL_Init fallita: %s\n", SDL_GetError());
    }
    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameControllerOpen(i);
        }
    }
#endif

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;

#ifdef __ORBIS__
    /* On PS4: create full HD 1080p window and software renderer on the window surface */
    window = SDL_CreateWindow(
        "PS4 Direct Downloader",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        1920,
        1080,
        0
    );

    if (window) {
        SDL_Surface *surface = SDL_GetWindowSurface(window);
        if (surface) {
            renderer = SDL_CreateSoftwareRenderer(surface);
        }
    }
#else
    /* On Desktop Simulator */
    window = SDL_CreateWindow(
        "PS4 Direct Downloader (Simulation & Native)",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (window) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer) {
            SDL_RenderSetLogicalSize(renderer, 1920, 1080);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        }
    }
#endif

    if (!renderer) {
        printf("[PS4-App] Errore creazione renderer! Uscita controllata...\n");
        for (int i = 0; i < 30; i++) {
            SDL_Delay(100);
        }
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        net_cleanup();
        return 0;
    }

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

#ifdef __ORBIS__
        if (window) {
            SDL_UpdateWindowSurface(window);
        }
#endif

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

#ifdef __ORBIS__
    if (joystick) SDL_JoystickClose(joystick);
#endif

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    net_cleanup();

    return 0;
}
