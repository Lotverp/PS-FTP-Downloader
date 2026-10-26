#include "ui.h"
#include "downloader.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

UIState g_ui = {
    .current_view = VIEW_CATALOG,
    .selected_index = 0,
    .selected_link_index = 0,
    .current_page = 1,
    .console_filter = 1, /* PS4 default */
    .search_query = "",
    .toast_message = "",
    .toast_timer = 0,
    .is_searching = false,
    .modal_open = false
};

static CatalogResult s_catalog = { 0 };
static bool s_loading = false;

/* Embedded 8x8 bitmap font glyphs (ASCII 32 to 126) for self-contained 1080p rendering */
static const unsigned char font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3c,0x3c,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x6c,0x6c,0xfe,0x6c,0xfe,0x6c,0x6c,0x00}, /* # */
    {0x18,0x3e,0x60,0x3c,0x06,0x7c,0x18,0x00}, /* $ */
    {0x00,0x63,0x66,0x0c,0x18,0x33,0x63,0x00}, /* % */
    {0x38,0x6c,0x38,0x76,0xdc,0xcc,0x76,0x00}, /* & */
    {0x30,0x30,0x10,0x20,0x00,0x00,0x00,0x00}, /* ' */
    {0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00}, /* ( */
    {0x30,0x18,0x0c,0x0c,0x0c,0x18,0x30,0x00}, /* ) */
    {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00}, /* * */
    {0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
    {0x00,0x00,0x00,0x7e,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00}, /* / */
    {0x7c,0xc6,0xce,0xd6,0xe6,0xc6,0x7c,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7e,0x00}, /* 1 */
    {0x7c,0xc6,0x06,0x1c,0x30,0x60,0xfe,0x00}, /* 2 */
    {0x7c,0xc6,0x06,0x3c,0x06,0xc6,0x7c,0x00}, /* 3 */
    {0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x1e,0x00}, /* 4 */
    {0xfe,0xc0,0xfc,0x06,0x06,0xc6,0x7c,0x00}, /* 5 */
    {0x7c,0xc6,0xc0,0xfc,0xc6,0xc6,0x7c,0x00}, /* 6 */
    {0xfe,0x06,0x0c,0x18,0x30,0x60,0x60,0x00}, /* 7 */
    {0x7c,0xc6,0xc6,0x7c,0xc6,0xc6,0x7c,0x00}, /* 8 */
    {0x7c,0xc6,0xc6,0x7e,0x06,0xc6,0x7c,0x00}, /* 9 */
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, /* : */
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, /* ; */
    {0x06,0x0c,0x18,0x30,0x18,0x0c,0x06,0x00}, /* < */
    {0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00}, /* = */
    {0x60,0x30,0x18,0x0c,0x18,0x30,0x60,0x00}, /* > */
    {0x7c,0xc6,0x0c,0x18,0x18,0x00,0x18,0x00}, /* ? */
    {0x7c,0xc6,0xde,0xde,0xdc,0xc0,0x7c,0x00}, /* @ */
    {0x38,0x6c,0xc6,0xfe,0xc6,0xc6,0xc6,0x00}, /* A */
    {0xfc,0x66,0x66,0x7c,0x66,0x66,0xfc,0x00}, /* B */
    {0x3c,0x66,0xc0,0xc0,0xc0,0x66,0x3c,0x00}, /* C */
    {0xf8,0x6c,0x66,0x66,0x66,0x6c,0xf8,0x00}, /* D */
    {0xfe,0x62,0x68,0x78,0x68,0x62,0xfe,0x00}, /* E */
    {0xfe,0x62,0x68,0x78,0x68,0x60,0xf0,0x00}, /* F */
    {0x3c,0x66,0xc0,0xc0,0xce,0x66,0x3e,0x00}, /* G */
    {0xc6,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0x00}, /* H */
    {0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, /* I */
    {0x1e,0x0c,0x0c,0x0c,0xcc,0xcc,0x78,0x00}, /* J */
    {0xe6,0x66,0x6c,0x78,0x6c,0x66,0xe6,0x00}, /* K */
    {0xf0,0x60,0x60,0x60,0x62,0x66,0xfe,0x00}, /* L */
    {0xc6,0xee,0xfe,0xd6,0xc6,0xc6,0xc6,0x00}, /* M */
    {0xc6,0xe6,0xf6,0xde,0xce,0xc6,0xc6,0x00}, /* N */
    {0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00}, /* O */
    {0xfc,0x66,0x66,0x7c,0x60,0x60,0xf0,0x00}, /* P */
    {0x7c,0xc6,0xc6,0xc6,0xd6,0xde,0x7c,0x06}, /* Q */
    {0xfc,0x66,0x66,0x7c,0x6c,0x66,0xe6,0x00}, /* R */
    {0x7c,0xc6,0x60,0x38,0x0c,0xc6,0x7c,0x00}, /* S */
    {0x7e,0x7e,0x18,0x18,0x18,0x18,0x3c,0x00}, /* T */
    {0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00}, /* U */
    {0xc6,0xc6,0xc6,0xc6,0x6c,0x38,0x10,0x00}, /* V */
    {0xc6,0xc6,0xc6,0xd6,0xfe,0xee,0xc6,0x00}, /* W */
    {0xc6,0x6c,0x38,0x38,0x6c,0xc6,0xc6,0x00}, /* X */
    {0x66,0x66,0x66,0x3c,0x18,0x18,0x3c,0x00}, /* Y */
    {0xfe,0xc6,0x0c,0x18,0x30,0x66,0xfe,0x00}, /* Z */
    {0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0x00}, /* [ */
    {0xc0,0x60,0x30,0x18,0x0c,0x06,0x02,0x00}, /* \ */
    {0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00}, /* ] */
    {0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff}, /* _ */
    {0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x78,0x0c,0x7c,0xcc,0x76,0x00}, /* a */
    {0xe0,0x60,0x7c,0x66,0x66,0x66,0x7c,0x00}, /* b */
    {0x00,0x00,0x7c,0xc6,0xc0,0xc6,0x7c,0x00}, /* c */
    {0x1c,0x0c,0x7c,0xcc,0xcc,0xcc,0x76,0x00}, /* d */
    {0x00,0x00,0x7c,0xc6,0xfe,0xc0,0x7c,0x00}, /* e */
    {0x38,0x6c,0x60,0xf0,0x60,0x60,0xf0,0x00}, /* f */
    {0x00,0x00,0x76,0xcc,0xcc,0x7c,0x0c,0xf8}, /* g */
    {0xe0,0x60,0x6c,0x76,0x66,0x66,0xe6,0x00}, /* h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3c,0x00}, /* i */
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3c}, /* j */
    {0xe0,0x60,0x66,0x6c,0x78,0x6c,0xe6,0x00}, /* k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, /* l */
    {0x00,0x00,0xe6,0xff,0xdb,0xc9,0xc9,0x00}, /* m */
    {0x00,0x00,0xdc,0x66,0x66,0x66,0x66,0x00}, /* n */
    {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0x7c,0x00}, /* o */
    {0x00,0x00,0xdc,0x66,0x66,0x7c,0x60,0xf0}, /* p */
    {0x00,0x00,0x76,0xcc,0xcc,0x7c,0x0c,0x1e}, /* q */
    {0x00,0x00,0xdc,0x76,0x60,0x60,0xf0,0x00}, /* r */
    {0x00,0x00,0x7c,0xc0,0x7c,0x06,0xfc,0x00}, /* s */
    {0x30,0x30,0xfc,0x30,0x30,0x34,0x18,0x00}, /* t */
    {0x00,0x00,0xcc,0xcc,0xcc,0xcc,0x76,0x00}, /* u */
    {0x00,0x00,0xc6,0xc6,0xc6,0x6c,0x38,0x00}, /* v */
    {0x00,0x00,0xc6,0xd6,0xfe,0xfe,0x6c,0x00}, /* w */
    {0x00,0x00,0xc6,0x6c,0x38,0x6c,0xc6,0x00}, /* x */
    {0x00,0x00,0xc6,0xc6,0xc6,0x7e,0x06,0xfc}, /* y */
    {0x00,0x00,0xfe,0x8c,0x18,0x32,0xfe,0x00}, /* z */
    {0x0e,0x18,0x18,0x70,0x18,0x18,0x0e,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x70,0x18,0x18,0x0e,0x18,0x18,0x70,0x00}, /* } */
    {0x76,0xdc,0x00,0x00,0x00,0x00,0x00,0x00}  /* ~ */
};

static void draw_char(SDL_Renderer *r, char c, int x, int y, int scale, SDL_Color col) {
    if (c < 32 || c > 126) c = '?';
    int idx = c - 32;
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);

    for (int row = 0; row < 8; row++) {
        unsigned char bits = font8x8[idx][row];
        for (int col_i = 0; col_i < 8; col_i++) {
            if (bits & (1 << (7 - col_i))) {
                SDL_Rect px = { x + col_i * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}

static void draw_text(SDL_Renderer *r, const char *str, int x, int y, int scale, SDL_Color col) {
    if (!str) return;
    int cur_x = x;
    for (int i = 0; str[i]; i++) {
        if (str[i] == '\n') {
            cur_x = x;
            y += 9 * scale;
            continue;
        }
        draw_char(r, str[i], cur_x, y, scale, col);
        cur_x += 8 * scale;
    }
}

static void load_catalog_async(void) {
    s_loading = true;
    dlps_fetch_catalog(g_ui.search_query, g_ui.current_page, g_ui.console_filter, &s_catalog);
    s_loading = false;
    if (g_ui.selected_index >= s_catalog.count) {
        g_ui.selected_index = s_catalog.count > 0 ? s_catalog.count - 1 : 0;
    }
}

bool ui_init(SDL_Renderer *renderer) {
    (void)renderer;
    load_catalog_async();
    return true;
}

void ui_show_toast(const char *msg) {
    if (!msg) return;
    strncpy(g_ui.toast_message, msg, sizeof(g_ui.toast_message) - 1);
    g_ui.toast_timer = SDL_GetTicks() + 4000; /* 4 seconds */
}

static void render_header(SDL_Renderer *r) {
    /* Top Bar background */
    SDL_Rect bar = { 0, 0, 1920, 90 };
    SDL_SetRenderDrawColor(r, 15, 23, 42, 255); /* Slate 900 */
    SDL_RenderFillRect(r, &bar);

    SDL_Color cyan = { 56, 189, 248, 255 };
    SDL_Color white = { 248, 250, 252, 255 };
    SDL_Color gray = { 148, 163, 184, 255 };

    draw_text(r, "PS4 DIRECT GAME DOWNLOADER", 60, 28, 3, cyan);
    draw_text(r, "v1.0.0 (Native PS4)", 620, 36, 2, gray);

    /* Console filter pill */
    const char *f_str = "[L2/R2] CONSOLE: TUTTE";
    if (g_ui.console_filter == 1) f_str = "[L2/R2] CONSOLE: PS4";
    else if (g_ui.console_filter == 2) f_str = "[L2/R2] CONSOLE: PS5";
    else if (g_ui.console_filter == 3) f_str = "[L2/R2] CONSOLE: PS3";

    SDL_Rect pill = { 1420, 24, 440, 42 };
    SDL_SetRenderDrawColor(r, 30, 41, 59, 255);
    SDL_RenderFillRect(r, &pill);
    draw_text(r, f_str, 1440, 34, 2, white);
}

static void render_catalog_grid(SDL_Renderer *r) {
    int start_x = 60;
    int start_y = 120;
    int card_w = 430;
    int card_h = 175;
    int gap_x = 26;
    int gap_y = 20;

    SDL_Color white = { 248, 250, 252, 255 };
    SDL_Color gray = { 148, 163, 184, 255 };
    SDL_Color cyan = { 56, 189, 248, 255 };
    SDL_Color green = { 52, 211, 153, 255 };

    if (s_loading) {
        draw_text(r, "CARICAMENTO CATALOGO IN CORSO...", 700, 450, 3, cyan);
        return;
    }

    if (s_catalog.count == 0) {
        draw_text(r, "NESSUN GIOCO TROVATO PER LA RICERCA", 650, 450, 3, gray);
        return;
    }

    for (int i = 0; i < s_catalog.count; i++) {
        int col = i % 4;
        int row = i / 4;
        int x = start_x + col * (card_w + gap_x);
        int y = start_y + row * (card_h + gap_y);

        bool selected = (i == g_ui.selected_index);
        SDL_Rect card = { x, y, card_w, card_h };

        /* Background */
        if (selected) {
            SDL_SetRenderDrawColor(r, 30, 58, 138, 255); /* Deep Blue Active */
        } else {
            SDL_SetRenderDrawColor(r, 30, 41, 59, 255);  /* Slate 800 */
        }
        SDL_RenderFillRect(r, &card);

        /* Border */
        if (selected) {
            SDL_SetRenderDrawColor(r, 56, 189, 248, 255); /* Glowing Cyan */
            for (int b = 0; b < 4; b++) {
                SDL_Rect outline = { x - b, y - b, card_w + b * 2, card_h + b * 2 };
                SDL_RenderDrawRect(r, &outline);
            }
        }

        /* Title truncate */
        char title_buf[48];
        strncpy(title_buf, s_catalog.games[i].title, sizeof(title_buf) - 1);
        title_buf[sizeof(title_buf) - 1] = '\0';
        draw_text(r, title_buf, x + 16, y + 20, 2, selected ? white : gray);

        /* Links badge */
        char links_buf[32];
        snprintf(links_buf, sizeof(links_buf), "%d Mirror disp.", s_catalog.games[i].n_links);
        draw_text(r, links_buf, x + 16, y + 130, 2, green);

        if (selected) {
            draw_text(r, "[X] SCARICA SUBITO", x + 230, y + 130, 2, cyan);
        }
    }
}

static void render_footer(SDL_Renderer *r) {
    /* Bottom Bar */
    SDL_Rect bar = { 0, 960, 1920, 120 };
    SDL_SetRenderDrawColor(r, 15, 23, 42, 255);
    SDL_RenderFillRect(r, &bar);

    SDL_Color white = { 248, 250, 252, 255 };
    SDL_Color cyan = { 56, 189, 248, 255 };
    SDL_Color yellow = { 251, 191, 36, 255 };

    /* Active download mini-player */
    if (g_active_dl.status == STATUS_DOWNLOADING || g_active_dl.status == STATUS_RESOLVING) {
        char prog_str[256];
        snprintf(prog_str, sizeof(prog_str), "SCARICAMENTO: %.40s | %.1f MB/s | %d%% | ETA: %ds",
                 g_active_dl.game_title, g_active_dl.speed_mbps, (int)(g_active_dl.progress * 100), g_active_dl.eta_seconds);
        draw_text(r, prog_str, 60, 975, 2, cyan);

        /* Progress Bar */
        SDL_Rect p_bg = { 60, 1005, 1800, 14 };
        SDL_SetRenderDrawColor(r, 51, 65, 85, 255);
        SDL_RenderFillRect(r, &p_bg);

        SDL_Rect p_fill = { 60, 1005, (int)(1800 * g_active_dl.progress), 14 };
        SDL_SetRenderDrawColor(r, 56, 189, 248, 255);
        SDL_RenderFillRect(r, &p_fill);
    } else if (g_active_dl.status == STATUS_COMPLETED) {
        char comp_str[512];
        snprintf(comp_str, sizeof(comp_str), "COMPLETATO: %.480s", g_active_dl.status_message);
        draw_text(r, comp_str, 60, 985, 2, yellow);
    } else {
        /* Legend of controller inputs */
        draw_text(r, "[X] Scarica  [O] Esci  [Q] Dettagli  [/\\] Cerca  [L1/R1] Pagina  [OPTIONS] Impostazioni", 260, 990, 2, white);
    }
}

static void render_toast(SDL_Renderer *r) {
    if (SDL_GetTicks() > g_ui.toast_timer || !g_ui.toast_message[0]) return;

    SDL_Rect toast = { 460, 20, 1000, 60 };
    SDL_SetRenderDrawColor(r, 30, 41, 59, 240);
    SDL_RenderFillRect(r, &toast);
    SDL_SetRenderDrawColor(r, 56, 189, 248, 255);
    SDL_RenderDrawRect(r, &toast);

    SDL_Color white = { 255, 255, 255, 255 };
    draw_text(r, g_ui.toast_message, 480, 38, 2, white);
}

void ui_render(SDL_Renderer *renderer) {
    /* Background */
    SDL_SetRenderDrawColor(renderer, 10, 14, 26, 255);
    SDL_RenderClear(renderer);

    render_header(renderer);
    render_catalog_grid(renderer);
    render_footer(renderer);
    render_toast(renderer);

    SDL_RenderPresent(renderer);
}

void ui_handle_input(SDL_Event *e) {
    if (e->type == SDL_KEYDOWN) {
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_RIGHT) {
            if (g_ui.selected_index < s_catalog.count - 1) g_ui.selected_index++;
        } else if (k == SDLK_LEFT) {
            if (g_ui.selected_index > 0) g_ui.selected_index--;
        } else if (k == SDLK_DOWN) {
            if (g_ui.selected_index + 4 < s_catalog.count) g_ui.selected_index += 4;
        } else if (k == SDLK_UP) {
            if (g_ui.selected_index - 4 >= 0) g_ui.selected_index -= 4;
        } else if (k == SDLK_RETURN || k == SDLK_x) {
            /* Download selected */
            if (s_catalog.count > 0 && g_ui.selected_index < s_catalog.count) {
                GameItem *game = &s_catalog.games[g_ui.selected_index];
                if (game->n_links > 0) {
                    ui_show_toast("Avvio download verso /data/pkg...");
                    downloader_start(game->links[0].url, game->title, game->password);
                }
            }
        } else if (k == SDLK_RIGHTBRACKET || k == SDLK_r) {
            /* Next page */
            g_ui.current_page++;
            load_catalog_async();
        } else if (k == SDLK_LEFTBRACKET || k == SDLK_l) {
            /* Prev page */
            if (g_ui.current_page > 1) {
                g_ui.current_page--;
                load_catalog_async();
            }
        }
    } else if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        Uint8 b = e->cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            if (g_ui.selected_index < s_catalog.count - 1) g_ui.selected_index++;
        } else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
            if (g_ui.selected_index > 0) g_ui.selected_index--;
        } else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            if (g_ui.selected_index + 4 < s_catalog.count) g_ui.selected_index += 4;
        } else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            if (g_ui.selected_index - 4 >= 0) g_ui.selected_index -= 4;
        } else if (b == SDL_CONTROLLER_BUTTON_A) { /* Cross on PS4 */
            if (s_catalog.count > 0 && g_ui.selected_index < s_catalog.count) {
                GameItem *game = &s_catalog.games[g_ui.selected_index];
                if (game->n_links > 0) {
                    ui_show_toast("Avvio download verso /data/pkg...");
                    downloader_start(game->links[0].url, game->title, game->password);
                }
            }
        } else if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) { /* R1 */
            g_ui.current_page++;
            load_catalog_async();
        } else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) { /* L1 */
            if (g_ui.current_page > 1) {
                g_ui.current_page--;
                load_catalog_async();
            }
        }
    }
}

void ui_destroy(void) {
}
