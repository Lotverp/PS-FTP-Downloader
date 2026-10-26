#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <SDL2/SDL.h>
#include "dlps.h"

typedef enum {
    VIEW_CATALOG = 0,
    VIEW_DETAILS,
    VIEW_SETTINGS,
    VIEW_SEARCH
} UIView;

typedef struct {
    UIView current_view;
    int selected_index;
    int selected_link_index;
    int current_page;
    int console_filter;
    
    char search_query[128];
    char toast_message[256];
    Uint32 toast_timer;
    
    bool is_searching;
    bool modal_open;
} UIState;

extern UIState g_ui;

bool ui_init(SDL_Renderer *renderer);
void ui_render(SDL_Renderer *renderer);
void ui_handle_input(SDL_Event *event);
void ui_show_toast(const char *msg);
void ui_destroy(void);

#endif /* UI_H */
