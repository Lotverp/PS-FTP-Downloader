#include "resolver.h"
#include "net_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void url_encode(const char *src, char *dst, size_t dst_len) {
    static const char *hex = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 3 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[(c >> 4) & 0x0F];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
}

bool resolve_1fichier(const char *url, const char *password, ResolveResult *out) {
    if (!out) return false;
    memset(out, 0, sizeof(ResolveResult));

    char postfields[512] = "submitz=1";
    if (password && password[0]) {
        char enc_pass[256];
        url_encode(password, enc_pass, sizeof(enc_pass));
        snprintf(postfields, sizeof(postfields), "submitz=1&pass=%s", enc_pass);
    }

    NetResponse resp;
    if (!net_post(url, postfields, "application/x-www-form-urlencoded", NULL, &resp) || !resp.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Connessione 1Fichier fallita: %.200s", resp.error_msg);
        net_response_free(&resp);
        return false;
    }

    /* Check for IP wait cooldown */
    if (strstr(resp.data, "You must wait") || strstr(resp.data, "must wait between downloads") || strstr(resp.data, "devez attendre")) {
        snprintf(out->wait_msg, sizeof(out->wait_msg), "Limite IP 1Fichier attivo. Attendi il cooldown o usa Real-Debrid.");
        snprintf(out->error_msg, sizeof(out->error_msg), "Limite IP 1Fichier: download temporaneamente bloccato");
        net_response_free(&resp);
        return false;
    }

    /* Look for direct storage CDN link: e.g. https://a-XX.1fichier.com/c... */
    const char *scan = resp.data;
    while ((scan = strstr(scan, "https://")) != NULL) {
        const char *quote = strpbrk(scan, "\"\' \r\n<>");
        if (!quote) quote = scan + strlen(scan);
        size_t len = quote - scan;
        if (len > 10 && len < 2048) {
            char u[2048];
            strncpy(u, scan, len);
            u[len] = '\0';

            if (strstr(u, ".1fichier.com/c") && !strstr(u, ".html") && !strstr(u, ".pl")) {
                snprintf(out->direct_url, sizeof(out->direct_url), "%s", u);
                out->success = true;
                net_response_free(&resp);
                return true;
            }
        }
        scan = quote;
    }

    net_response_free(&resp);
    snprintf(out->error_msg, sizeof(out->error_msg), "Link di download 1Fichier non trovato");
    return false;
}

bool resolve_mediafire(const char *url, ResolveResult *out) {
    if (!out) return false;
    memset(out, 0, sizeof(ResolveResult));

    NetResponse resp;
    if (!net_get(url, NULL, &resp) || !resp.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Connessione Mediafire fallita: %.200s", resp.error_msg);
        net_response_free(&resp);
        return false;
    }

    if (strstr(resp.data, "error.php") || strstr(resp.data, "errno=")) {
        snprintf(out->error_msg, sizeof(out->error_msg), "File Mediafire rimosso per DMCA o eliminato");
        net_response_free(&resp);
        return false;
    }

    const char *dl_btn = strstr(resp.data, "href=\"https://download");
    if (!dl_btn) dl_btn = strstr(resp.data, "href=\'https://download");
    if (dl_btn) {
        dl_btn += 6;
        const char *end_q = strpbrk(dl_btn, "\"\' ");
        if (end_q) {
            size_t len = end_q - dl_btn;
            if (len < sizeof(out->direct_url)) {
                strncpy(out->direct_url, dl_btn, len);
                out->direct_url[len] = '\0';
                out->success = true;
                net_response_free(&resp);
                return true;
            }
        }
    }

    net_response_free(&resp);
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

    char enc_link[2048];
    url_encode(url, enc_link, sizeof(enc_link));

    char postfields[4096];
    if (password && password[0]) {
        char enc_pass[256];
        url_encode(password, enc_pass, sizeof(enc_pass));
        snprintf(postfields, sizeof(postfields), "link=%s&password=%s", enc_link, enc_pass);
    } else {
        snprintf(postfields, sizeof(postfields), "link=%s", enc_link);
    }

    NetResponse resp;
    if (!net_post("https://api.real-debrid.com/rest/1.0/unrestrict/link", postfields, "application/x-www-form-urlencoded", token, &resp) || !resp.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Connessione Real-Debrid fallita: %.200s", resp.error_msg);
        net_response_free(&resp);
        return false;
    }

    cJSON *json = cJSON_Parse(resp.data);
    net_response_free(&resp);
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
