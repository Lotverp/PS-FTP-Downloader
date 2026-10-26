#include "dlps.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>

#define DLPS_API_ENDPOINT "https://dlpsgame.com/wp-json/wp/v2/posts"

typedef struct {
    char *data;
    size_t size;
} MemoryBuffer;

static size_t mem_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    int *totalPages = (int *)userdata;
    if (strncasecmp(buffer, "x-wp-totalpages:", 16) == 0) {
        *totalPages = atoi(buffer + 16);
    }
    return total;
}

static LinkType detect_link_type(const char *url) {
    if (!url) return LINK_UNKNOWN;
    if (strstr(url, ".pkg") || strstr(url, "download.akirabox.com") || strstr(url, "vikingfile.com/d/"))
        return LINK_DIRECT_PKG;
    if (strstr(url, "mediafire.com"))
        return LINK_MEDIAFIRE;
    if (strstr(url, "1fichier.com"))
        return LINK_1FICHIER;
    if (strstr(url, "filecrypt.cc") || strstr(url, "ouo.io") || strstr(url, "downloadgameps3.net"))
        return LINK_PROTECTOR;
    return LINK_OTHER;
}

static void clean_html_entities(char *str) {
    if (!str) return;
    char *p = str;
    while ((p = strstr(p, "&#8211;"))) {
        *p = '-';
        memmove(p + 1, p + 7, strlen(p + 7) + 1);
    }
    p = str;
    while ((p = strstr(p, "&amp;"))) {
        *p = '&';
        memmove(p + 1, p + 5, strlen(p + 5) + 1);
    }
    p = str;
    while ((p = strstr(p, "&#038;"))) {
        *p = '&';
        memmove(p + 1, p + 6, strlen(p + 6) + 1);
    }
}

/* Base64 decode table */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_decode(const char *input, size_t *out_len) {
    if (!input) return NULL;
    size_t in_len = strlen(input);
    if (in_len % 4 != 0) return NULL;

    size_t o_len = in_len / 4 * 3;
    if (input[in_len - 1] == '=') o_len--;
    if (input[in_len - 2] == '=') o_len--;

    char *out = malloc(o_len + 1);
    if (!out) return NULL;

    int val = 0, valb = -8;
    size_t cur = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = input[i];
        if (c == '=') break;
        const char *p = strchr(b64_table, c);
        if (!p) { free(out); return NULL; }
        val = (val << 6) + (int)(p - b64_table);
        valb += 6;
        if (valb >= 0) {
            out[cur++] = (char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    out[cur] = '\0';
    if (out_len) *out_len = cur;
    return out;
}

static void parse_game_payload(const char *html, GameItem *item) {
    if (!html || !item) return;

    /* 1. Extract password if present */
    const char *pw_tag = strcasestr(html, "Password:");
    if (!pw_tag) pw_tag = strcasestr(html, "Password :");
    if (pw_tag) {
        const char *p = pw_tag + strlen("Password:");
        while (*p == ' ' || *p == ':') p++;
        const char *end = strpbrk(p, "< \r\n");
        if (end) {
            size_t l = end - p;
            if (l < sizeof(item->password)) {
                strncpy(item->password, p, l);
                item->password[l] = '\0';
            }
        }
    }

    /* 2. Check for base64 encoded download blocks */
    const char *b64_start = strstr(html, "eyJ"); /* common JSON base64 */
    if (!b64_start) b64_start = strstr(html, "PGRpdi"); /* common <div> base64 */

    char *decoded_text = NULL;
    if (b64_start) {
        const char *b64_end = strpbrk(b64_start, "\"\' <");
        if (b64_end && (b64_end - b64_start) > 20) {
            size_t blen = b64_end - b64_start;
            char *tmp = malloc(blen + 1);
            if (tmp) {
                strncpy(tmp, b64_start, blen);
                tmp[blen] = '\0';
                decoded_text = base64_decode(tmp, NULL);
                free(tmp);
            }
        }
    }

    const char *search_text = decoded_text ? decoded_text : html;

    /* 3. Extract <a> links */
    const char *scan = search_text;
    while ((scan = strstr(scan, "<a ")) != NULL && item->n_links < DLPS_MAX_LINKS) {
        const char *href = strstr(scan, "href=\"");
        if (!href) href = strstr(scan, "href=\'");
        if (!href) { scan += 3; continue; }
        href += 6;
        const char *end_href = strpbrk(href, "\"\'");
        if (!end_href) { scan += 3; continue; }

        size_t ulen = end_href - href;
        if (ulen > 10 && ulen < sizeof(item->links[0].url)) {
            char u[1024];
            strncpy(u, href, ulen);
            u[ulen] = '\0';

            /* Filter out unrelated navigational links */
            if (strstr(u, "dlpsgame.com") == NULL && strncmp(u, "http", 4) == 0) {
                GameLink *gl = &item->links[item->n_links++];
                snprintf(gl->url, sizeof(gl->url), "%s", u);
                gl->type = detect_link_type(u);

                /* Extract anchor text */
                const char *close_tag = strchr(end_href, '>');
                if (close_tag) {
                    close_tag++;
                    const char *close_a = strstr(close_tag, "</a>");
                    if (close_a && (close_a - close_tag) < (int)sizeof(gl->name)) {
                        strncpy(gl->name, close_tag, close_a - close_tag);
                        gl->name[close_a - close_tag] = '\0';
                    }
                }
                if (!gl->name[0]) {
                    if (gl->type == LINK_1FICHIER) strcpy(gl->name, "1Fichier");
                    else if (gl->type == LINK_MEDIAFIRE) strcpy(gl->name, "Mediafire");
                    else if (gl->type == LINK_DIRECT_PKG) strcpy(gl->name, "Direct PKG");
                    else strcpy(gl->name, "Mirror");
                }
            }
        }
        scan = end_href;
    }

    if (decoded_text) free(decoded_text);
}

bool dlps_fetch_catalog(const char *query, int page, int console_filter, CatalogResult *out) {
    if (!out) return false;
    memset(out, 0, sizeof(CatalogResult));
    out->current_page = page > 0 ? page : 1;
    out->total_pages = 1;

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Impossibile inizializzare cURL");
        return false;
    }

    char url[1024];
    /* console_filter: 0=All, 1=PS4 (cat=11), 2=PS5 (cat=1224), 3=PS3 (cat=3) */
    const char *cat_param = "";
    if (console_filter == 1) cat_param = "&categories=11";
    else if (console_filter == 2) cat_param = "&categories=1224";
    else if (console_filter == 3) cat_param = "&categories=3";

    if (query && query[0]) {
        char *esc = curl_easy_escape(curl, query, 0);
        snprintf(url, sizeof(url), "%s?search=%s&page=%d&per_page=12%s", DLPS_API_ENDPOINT, esc ? esc : "", out->current_page, cat_param);
        if (esc) curl_free(esc);
    } else {
        snprintf(url, sizeof(url), "%s?page=%d&per_page=12%s", DLPS_API_ENDPOINT, out->current_page, cat_param);
    }

    MemoryBuffer chunk = { malloc(1), 0 };
    if (chunk.data) chunk.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (PlayStation 4 9.00) AppleWebKit/605.1.15 (KHTML, like Gecko)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out->total_pages);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !chunk.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Errore di rete: %s", curl_easy_strerror(res));
        if (chunk.data) free(chunk.data);
        return false;
    }

    cJSON *root = cJSON_Parse(chunk.data);
    free(chunk.data);
    if (!root || !cJSON_IsArray(root)) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Formato catalogo non valido");
        if (root) cJSON_Delete(root);
        return false;
    }

    int n_posts = cJSON_GetArraySize(root);
    for (int i = 0; i < n_posts && out->count < DLPS_MAX_GAMES; i++) {
        cJSON *post = cJSON_GetArrayItem(root, i);
        if (!post) continue;

        GameItem *item = &out->games[out->count];
        memset(item, 0, sizeof(GameItem));

        cJSON *id_item = cJSON_GetObjectItem(post, "id");
        if (id_item) item->id = id_item->valueint;

        /* Title */
        cJSON *title_obj = cJSON_GetObjectItem(post, "title");
        if (title_obj) {
            cJSON *rendered = cJSON_GetObjectItem(title_obj, "rendered");
            if (rendered && rendered->valuestring) {
                strncpy(item->title, rendered->valuestring, sizeof(item->title) - 1);
                clean_html_entities(item->title);
            }
        }

        /* Content / download links */
        cJSON *content_obj = cJSON_GetObjectItem(post, "content");
        if (content_obj) {
            cJSON *c_rend = cJSON_GetObjectItem(content_obj, "rendered");
            if (c_rend && c_rend->valuestring) {
                parse_game_payload(c_rend->valuestring, item);
            }
        }

        out->count++;
    }

    cJSON_Delete(root);
    return true;
}
