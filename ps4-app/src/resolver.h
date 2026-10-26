#ifndef RESOLVER_H
#define RESOLVER_H

#include <stdbool.h>

typedef struct {
    bool success;
    char direct_url[2048];
    char wait_msg[128];
    char error_msg[256];
} ResolveResult;

bool resolve_1fichier(const char *url, const char *password, ResolveResult *out);
bool resolve_mediafire(const char *url, ResolveResult *out);
bool resolve_realdebrid(const char *url, const char *password, const char *token, ResolveResult *out);
bool resolve_any_link(const char *url, const char *password, const char *rd_token, ResolveResult *out);

#endif /* RESOLVER_H */
