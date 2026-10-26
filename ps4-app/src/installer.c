#include "installer.h"
#include "net_client.h"
#include <stdio.h>
#include <string.h>

#define GOLDHEN_API_URL "http://127.0.0.1:12800/api/install"

bool installer_install_pkg(const char *pkg_path, char *out_msg, size_t msg_size) {
    if (!pkg_path || !pkg_path[0]) {
        if (out_msg) snprintf(out_msg, msg_size, "Percorso pacchetto non valido");
        return false;
    }

#if defined(__ORBIS__) || defined(__PS4__)
    /* On PS4: trigger GoldHEN internal Package Installer daemon */
    char payload[1024];
    snprintf(payload, sizeof(payload), "{\"type\": \"direct\", \"packages\": [\"%s\"]}", pkg_path);

    NetResponse resp;
    bool ok = net_post(GOLDHEN_API_URL, payload, "application/json", NULL, &resp);
    net_response_free(&resp);

    if (ok) {
        if (out_msg) snprintf(out_msg, msg_size, "Installazione avviata con successo in background!");
    } else {
        if (out_msg) snprintf(out_msg, msg_size, "PKG pronto in /data/pkg (Installa da GoldHEN menu)");
    }
    return true;
#else
    /* Desktop Simulation Mode */
    if (out_msg) snprintf(out_msg, msg_size, "Download completato (File: %s)", pkg_path);
    return true;
#endif
}
