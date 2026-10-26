#include "downloader.h"
#include "resolver.h"
#include "installer.h"
#include "config.h"
#include "net_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define PKG_MAGIC "\x7f\x43\x4e\x54" /* \x7fCNT */

DownloadTask g_active_dl = { 0 };

typedef struct {
    FILE *fp;
    bool checked_magic;
    struct timespec last_time;
    int64_t last_bytes;
} StreamContext;

static size_t dl_write_cb(const void *ptr, size_t size, size_t nmemb, void *userdata) {
    StreamContext *ctx = (StreamContext *)userdata;
    size_t total = size * nmemb;
    if (g_active_dl.stop_requested) return 0;

    /* Magic byte check on first chunk */
    if (!ctx->checked_magic && total >= 4) {
        ctx->checked_magic = true;
        if (memcmp(ptr, PKG_MAGIC, 4) != 0) {
            if (strncasecmp((const char *)ptr, "<!doc", 5) == 0 || strncasecmp((const char *)ptr, "<html", 5) == 0) {
                snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message),
                         "Errore: il server ha restituito una pagina HTML anziché il file .pkg");
            } else {
                snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message),
                         "Attenzione: intestazione non valida (file non è un .pkg)");
            }
            g_active_dl.status = STATUS_ERROR;
            return 0; /* abort transfer */
        }
    }

    size_t written = fwrite(ptr, 1, total, ctx->fp);
    g_active_dl.downloaded_size += written;

    /* Metrics calculation */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - ctx->last_time.tv_sec) + (now.tv_nsec - ctx->last_time.tv_nsec) / 1e9;
    if (elapsed >= 0.5) {
        int64_t diff = g_active_dl.downloaded_size - ctx->last_bytes;
        g_active_dl.speed_mbps = (diff / (1024.0 * 1024.0)) / elapsed;
        ctx->last_bytes = g_active_dl.downloaded_size;
        ctx->last_time = now;

        if (g_active_dl.total_size > 0) {
            g_active_dl.progress = (float)g_active_dl.downloaded_size / (float)g_active_dl.total_size;
            if (g_active_dl.speed_mbps > 0.05) {
                int64_t remain = g_active_dl.total_size - g_active_dl.downloaded_size;
                g_active_dl.eta_seconds = (int)((remain / (1024.0 * 1024.0)) / g_active_dl.speed_mbps);
            }
        }
    }

    return written;
}

static void *downloader_thread_fn(void *arg) {
    (void)arg;
    g_active_dl.status = STATUS_RESOLVING;
    snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message), "Risoluzione link in corso...");

    ResolveResult res;
    if (!resolve_any_link(g_active_dl.url, g_active_dl.password, g_config.realdebrid_token, &res)) {
        g_active_dl.status = STATUS_ERROR;
        snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message), "%.250s",
                 res.error_msg[0] ? res.error_msg : "Impossibile risolvere il link");
        return NULL;
    }

    /* Prepare sanitized destination path */
    char safe_title[128] = "game";
    size_t si = 0;
    for (size_t i = 0; g_active_dl.game_title[i] && si < sizeof(safe_title) - 1; i++) {
        char c = g_active_dl.game_title[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') {
            safe_title[si++] = c;
        } else if (c == ' ') {
            safe_title[si++] = '_';
        }
    }
    safe_title[si] = '\0';
    snprintf(g_active_dl.dest_path, sizeof(g_active_dl.dest_path), "%.300s/%.150s.pkg", g_config.download_dir, safe_title);

    FILE *fp = fopen(g_active_dl.dest_path, "wb");
    if (!fp) {
        g_active_dl.status = STATUS_ERROR;
        snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message), "Impossibile aprire il file per la scrittura locale");
        return NULL;
    }

    g_active_dl.status = STATUS_DOWNLOADING;
    snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message), "Download in corso...");
    g_active_dl.downloaded_size = 0;
    g_active_dl.total_size = -1;
    g_active_dl.progress = 0.0f;
    g_active_dl.speed_mbps = 0.0;

    StreamContext ctx = { 0 };
    ctx.fp = fp;
    clock_gettime(CLOCK_MONOTONIC, &ctx.last_time);

    char err_buf[256] = "";
    bool success = net_download_file(res.direct_url, g_active_dl.dest_path, dl_write_cb, &ctx, err_buf, sizeof(err_buf));

    fclose(fp);

    if (g_active_dl.stop_requested) {
        g_active_dl.status = STATUS_IDLE;
        snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message), "Download annullato");
        unlink(g_active_dl.dest_path);
        return NULL;
    }

    if (!success && g_active_dl.status != STATUS_ERROR) {
        g_active_dl.status = STATUS_ERROR;
        snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message), "Errore scaricamento: %s", err_buf[0] ? err_buf : "Connessione interrotta");
        unlink(g_active_dl.dest_path);
        return NULL;
    }

    if (g_active_dl.status == STATUS_ERROR) {
        unlink(g_active_dl.dest_path);
        return NULL;
    }

    /* Success! */
    g_active_dl.progress = 1.0f;
    if (g_config.auto_install) {
        g_active_dl.status = STATUS_INSTALLING;
        snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message), "Avvio installazione pacchetto...");
        installer_install_pkg(g_active_dl.dest_path, g_active_dl.status_message, sizeof(g_active_dl.status_message));
    } else {
        snprintf(g_active_dl.status_message, sizeof(g_active_dl.status_message), "PKG pronto in %.230s", g_active_dl.dest_path);
    }
    g_active_dl.status = STATUS_COMPLETED;
    return NULL;
}

void downloader_init(void) {
    memset(&g_active_dl, 0, sizeof(DownloadTask));
}

bool downloader_start(const char *url, const char *game_title, const char *password) {
    if (g_active_dl.status == STATUS_DOWNLOADING || g_active_dl.status == STATUS_RESOLVING) {
        return false;
    }

    memset(&g_active_dl, 0, sizeof(DownloadTask));
    strncpy(g_active_dl.url, url, sizeof(g_active_dl.url) - 1);
    strncpy(g_active_dl.game_title, game_title, sizeof(g_active_dl.game_title) - 1);
    if (password) strncpy(g_active_dl.password, password, sizeof(g_active_dl.password) - 1);

    g_active_dl.stop_requested = false;
    pthread_create(&g_active_dl.thread, NULL, downloader_thread_fn, NULL);
    pthread_detach(g_active_dl.thread);
    return true;
}

void downloader_stop(void) {
    g_active_dl.stop_requested = true;
}

void downloader_update_metrics(void) {
    /* Metrics are continuously updated inside dl_write_cb */
}
