#ifndef DLPS_H
#define DLPS_H

#include <stdbool.h>

#define DLPS_MAX_LINKS 16
#define DLPS_MAX_GAMES 30

typedef enum {
    LINK_UNKNOWN = 0,
    LINK_DIRECT_PKG,
    LINK_MEDIAFIRE,
    LINK_1FICHIER,
    LINK_PROTECTOR,
    LINK_OTHER
} LinkType;

typedef struct {
    char name[128];
    char url[1024];
    LinkType type;
} GameLink;

typedef struct {
    int id;
    char title[256];
    char image_url[512];
    char password[128];
    char release_date[64];
    int  n_links;
    GameLink links[DLPS_MAX_LINKS];
} GameItem;

typedef struct {
    GameItem games[DLPS_MAX_GAMES];
    int count;
    int total_pages;
    int current_page;
    char error_msg[256];
} CatalogResult;

bool dlps_fetch_catalog(const char *query, int page, int console_filter, CatalogResult *out);

#endif /* DLPS_H */
