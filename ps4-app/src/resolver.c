#include "resolver.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

typedef struct {
    char *data;
    size_t size;
} MemChunk;

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemChunk *mem = (MemChunk *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

bool resolve_1fichier(const char *url, const char *password, ResolveResult *out) {
    if (!out) return false;
    memset(out, 0, sizeof(ResolveResult));

    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(out->error_msg, sizeof(out->error_msg), "cURL init failed");
        return false;
    }

    MemChunk chunk = { malloc(1), 0 };
    if (chunk.data) chunk.data[0] = '\0';

    char postfields[512] = "submitz=1";
    if (password && password[0]) {
        char *esc = curl_easy_escape(c, password, 0);
        if (esc) {
            snprintf(postfields, sizeof(postfields), "submitz=1&pass=%s", esc);
            curl_free(esc);
        }
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, postfields);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || !chunk.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Connessione 1Fichier fallita");
        if (chunk.data) free(chunk.data);
        return false;
    }

    /* 1. Check IP cooldown wait limit */
    const char *w_pos = strstr(chunk.data, "You must wait");
    if (!w_pos) w_pos = strstr(chunk.data, "you must wait");
    if (w_pos) {
        const char *end_w = strstr(w_pos, "…");
        if (!end_w) end_w = strstr(w_pos, "<");
        size_t w_len = end_w ? (size_t)(end_w - w_pos) : 24;
        if (w_len < sizeof(out->wait_msg)) {
            strncpy(out->wait_msg, w_pos, w_len);
            out->wait_msg[w_len] = '\0';
        }
        snprintf(out->error_msg, sizeof(out->error_msg), "1Fichier: %s", out->wait_msg);
        free(chunk.data);
        return false;
    }

    /* 2. Search for genuine CDN storage link */
    const char *scan = chunk.data;
    while ((scan = strstr(scan, "href=\"http")) != NULL) {
        scan += 6;
        const char *quote = strchr(scan, '\"');
        if (!quote) break;

        size_t len = quote - scan;
        if (len < sizeof(out->direct_url)) {
            char u[2048];
            strncpy(u, scan, len);
            u[len] = '\0';

            /* Must be storage server: e.g. https://a-XX.1fichier.com/c... */
            if (strstr(u, ".1fichier.com/c") && !strstr(u, ".html") && !strstr(u, ".pl")) {
                snprintf(out->direct_url, sizeof(out->direct_url), "%s", u);
                out->success = true;
                free(chunk.data);
                return true;
            }
        }
        scan = quote;
    }

    free(chunk.data);
    snprintf(out->error_msg, sizeof(out->error_msg), "Link di download 1Fichier non trovato");
    return false;
}

bool resolve_mediafire(const char *url, ResolveResult *out) {
    if (!out) return false;
    memset(out, 0, sizeof(ResolveResult));

    CURL *c = curl_easy_init();
    if (!c) return false;

    MemChunk chunk = { malloc(1), 0 };
    if (chunk.data) chunk.data[0] = '\0';

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);

    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || !chunk.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Connessione Mediafire fallita");
        if (chunk.data) free(chunk.data);
        return false;
    }

    if (strstr(chunk.data, "error.php") || strstr(chunk.data, "errno=")) {
        snprintf(out->error_msg, sizeof(out->error_msg), "File Mediafire rimosso per DMCA o eliminato");
        free(chunk.data);
        return false;
    }

    const char *dl_btn = strstr(chunk.data, "href=\"https://download");
    if (!dl_btn) dl_btn = strstr(chunk.data, "href=\'https://download");
    if (dl_btn) {
        dl_btn += 6;
        const char *end_q = strpbrk(dl_btn, "\"\' ");
        if (end_q) {
            size_t len = end_q - dl_btn;
            if (len < sizeof(out->direct_url)) {
                strncpy(out->direct_url, dl_btn, len);
                out->direct_url[len] = '\0';
                out->success = true;
                free(chunk.data);
                return true;
            }
        }
    }

    free(chunk.data);
    snprintf(out->error_msg, sizeof(out->error_msg), "Link download diretto Mediafire non trovato");
    return false;
}

bool resolve_realdebrid(const char *url, const char *password, const char *token, ResolveResult *out) {
    if (!out) return false;
    memset(out, 0, sizeof(ResolveResult));
    if (!token || !token[0]) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Token Real-Debrid mancante");
        return false;
    }

    CURL *c = curl_easy_init();
    if (!c) return false;

    MemChunk chunk = { malloc(1), 0 };
    if (chunk.data) chunk.data[0] = '\0';

    char auth_header[300];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
    struct curl_slist *headers = curl_slist_append(NULL, auth_header);

    char *esc_link = curl_easy_escape(c, url, 0);
    char *esc_pass = password && password[0] ? curl_easy_escape(c, password, 0) : NULL;
    char postfields[2048];
    if (esc_pass) {
        snprintf(postfields, sizeof(postfields), "link=%s&password=%s", esc_link, esc_pass);
        curl_free(esc_pass);
    } else {
        snprintf(postfields, sizeof(postfields), "link=%s", esc_link);
    }
    if (esc_link) curl_free(esc_link);

    curl_easy_setopt(c, CURLOPT_URL, "https://api.real-debrid.com/rest/1.0/unrestrict/link");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, postfields);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || !chunk.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Connessione Real-Debrid fallita");
        if (chunk.data) free(chunk.data);
        return false;
    }

    cJSON *json = cJSON_Parse(chunk.data);
    free(chunk.data);
    if (!json) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Risposta Real-Debrid non valida");
        return false;
    }

    cJSON *dl_item = cJSON_GetObjectItem(json, "download");
    if (dl_item && dl_item->valuestring) {
        strncpy(out->direct_url, dl_item->valuestring, sizeof(out->direct_url) - 1);
        out->success = true;
    } else {
        cJSON *err_item = cJSON_GetObjectItem(json, "error");
        snprintf(out->error_msg, sizeof(out->error_msg), "Real-Debrid: %s",
                 err_item && err_item->valuestring ? err_item->valuestring : "Sblocco fallito");
    }

    cJSON_Delete(json);
    return out->success;
}

bool resolve_any_link(const char *url, const char *password, const char *rd_token, ResolveResult *out) {
    if (!url || !out) return false;

    /* Direct PKG / CDN */
    if (strstr(url, ".pkg") || strstr(url, "download.akirabox") || strstr(url, "vikingfile.com/d/")) {
        memset(out, 0, sizeof(ResolveResult));
        strncpy(out->direct_url, url, sizeof(out->direct_url) - 1);
        out->success = true;
        return true;
    }

    /* 1Fichier */
    if (strstr(url, "1fichier.com/")) {
        if (resolve_1fichier(url, password, out)) return true;
        if (rd_token && rd_token[0] && resolve_realdebrid(url, password, rd_token, out)) return true;
        return false;
    }

    /* Mediafire */
    if (strstr(url, "mediafire.com/file/")) {
        if (resolve_mediafire(url, out)) return true;
        if (rd_token && rd_token[0] && resolve_realdebrid(url, password, rd_token, out)) return true;
        return false;
    }

    /* Protectors (FileCrypt, etc.) via Real-Debrid */
    if (rd_token && rd_token[0]) {
        return resolve_realdebrid(url, password, rd_token, out);
    }

    snprintf(out->error_msg, sizeof(out->error_msg), "Link protetto: richiede Real-Debrid");
    return false;
}
