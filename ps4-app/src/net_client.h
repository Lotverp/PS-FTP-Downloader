#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t size;
    int status_code;
    bool success;
    char error_msg[256];
} NetResponse;

typedef size_t (*NetWriteChunkCb)(const void *ptr, size_t size, size_t nmemb, void *userdata);

bool net_init(void);
void net_cleanup(void);

bool net_get(const char *url, const char *auth_bearer, NetResponse *out);
bool net_post(const char *url, const char *post_data, const char *content_type, const char *auth_bearer, NetResponse *out);
bool net_download_file(const char *url, const char *dest_path, NetWriteChunkCb chunk_cb, void *userdata, char *err_msg, size_t err_len);
void net_response_free(NetResponse *resp);

#endif /* NET_CLIENT_H */
