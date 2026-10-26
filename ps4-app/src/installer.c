#include "installer.h"
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>

#define GOLDHEN_API_URL "http://127.0.0.1:12800/api/install"

bool installer_install_pkg(const char *pkg_path, char *out_msg, size_t msg_size) {
    if (!pkg_path || !pkg_path[0]) {
        if (out_msg) snprintf(out_msg, msg_size, "Percorso pacchetto non valido");
        return false;
    }

#ifdef __PS4__
    /* On PS4: trigger GoldHEN internal Package Installer daemon */
    CURL *c = curl_easy_init();
    if (!c) {
        if (out_msg) snprintf(out_msg, msg_size, "Download completato in /data/pkg");
        return true;
    }

    char payload[1024];
    snprintf(payload, sizeof(payload), "{\"type\": \"direct\", \"packages\": [\"%s\"]}", pkg_path);

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, GOLDHEN_API_URL);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (res == CURLE_OK) {
        if (out_msg) snprintf(out_msg, msg_size, "Installazione avviata con successo in background!");
        return true;
    } else {
        if (out_msg) snprintf(out_msg, msg_size, "PKG pronto in /data/pkg (Installa da GoldHEN menu)");
        return true;
    }
#else
    /* Desktop Simulation Mode */
    if (out_msg) snprintf(out_msg, msg_size, "Download completato (File: %s)", pkg_path);
    return true;
#endif
}
