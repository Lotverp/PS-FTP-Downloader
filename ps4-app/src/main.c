#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "config.h"
#include "downloader.h"
#include "ui.h"
#include "net_client.h"
#include "debug_log.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Disable stdio buffering for immediate output */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Initialize Live Telemetry (sends UDP to PC and prints to GoldHEN Klog port 9998) */
    dbg_init("192.168.1.24", 9999);
    dbg_log("[PS4-App] Avvio PS4 Direct Downloader (PID boot)...\n");

    /* 1. INITIALIZE SDL VIDEO FIRST TO DISMISS PS4 SPLASH SCREEN */
#ifdef __ORBIS__
    dbg_log("[PS4-App] Inizializzazione SDL_Init(VIDEO | JOYSTICK)...\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) {
        dbg_log("[PS4-App] SDL_Init fallback solo VIDEO: %s\n", SDL_GetError());
        SDL_Init(SDL_INIT_VIDEO);
    }
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "[PS4-App] SDL_Init fallita: %s\n", SDL_GetError());
    }
#endif

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;

#ifdef __ORBIS__
    dbg_log("[PS4-App] Creazione finestra 1920x1080...\n");
    window = SDL_CreateWindow(
        "PS4 Direct Downloader",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        1920,
        1080,
        0
    );

    if (window) {
        dbg_log("[PS4-App] Ottenimento superficie finestra...\n");
        SDL_Surface *surface = SDL_GetWindowSurface(window);
        if (surface) {
            renderer = SDL_CreateSoftwareRenderer(surface);
            dbg_log("[PS4-App] Software renderer creato con successo!\n");
        }
    }
#else
    /* Desktop Simulator */
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
        dbg_log("[PS4-App] ERRORE: Creazione renderer fallita: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    /* 2. IMMEDIATELY DRAW AND FLIP FRAME 0 TO DISMISS PS4 BOOT LOGO */
    SDL_SetRenderDrawColor(renderer, 10, 14, 26, 255);
    SDL_RenderClear(renderer);
#ifdef __ORBIS__
    if (window) {
        SDL_UpdateWindowSurface(window);
        dbg_log("[PS4-App] Frame 0 inviato a schermo! Logo PS4 superato.\n");
    }
#endif

    /* 3. CONTROLLERS SETUP */
#ifdef __ORBIS__
    SDL_Joystick *joystick = NULL;
    if (SDL_NumJoysticks() > 0) {
        joystick = SDL_JoystickOpen(0);
        if (joystick) {
            dbg_log("[PS4-App] DualShock 4 collegato (Joystick 0).\n");
        }
    }
#else
    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameControllerOpen(i);
        }
    }
#endif

    /* 4. SUBSYSTEMS & APP LOGIC INITIALIZATION */
    dbg_log("[PS4-App] Inizializzazione configurazione e moduli...\n");
    config_init();
    downloader_init();
    net_init();

    /* Initialize UI */
    ui_init(renderer);
    dbg_log("[PS4-App] UI inizializzata. Avvio draw loop continuo...\n");

#ifdef __ORBIS__
    /* On PS4: run continuous draw loop without premature desktop exit triggers */
    Uint32 last_ticks = SDL_GetTicks();
    for (int frame = 0;; frame++) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ui_handle_input(&event);
        }

        downloader_update_metrics();
        ui_render(renderer);

        if (window) {
            SDL_UpdateWindowSurface(window);
        }

        if (frame % 120 == 0) {
            dbg_log("[PS4-App] Heartbeat attivo: Frame %d (Tick %u, 60 FPS)\n", frame, SDL_GetTicks());
        }

        Uint32 frame_time = SDL_GetTicks() - last_ticks;
        if (frame_time < 16) {
            SDL_Delay(16 - frame_time);
        }
        last_ticks = SDL_GetTicks();
    }
#else
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

        /* 60 FPS frame limiter */
        Uint32 frame_time = SDL_GetTicks() - last_ticks;
        if (frame_time < 16) {
            SDL_Delay(16 - frame_time);
        }
        last_ticks = SDL_GetTicks();
    }
#endif

    dbg_log("[PS4-App] Chiusura applicazione...\n");
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
