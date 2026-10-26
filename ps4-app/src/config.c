#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifdef __PS4__
#define DEFAULT_SETTINGS_DIR "/data/ps4-downloader"
#define DEFAULT_DOWNLOAD_DIR "/data/pkg"
#else
#define DEFAULT_SETTINGS_DIR ".config/ps4-downloader"
#define DEFAULT_DOWNLOAD_DIR "./downloaded_pkgs"
#endif

AppConfig g_config = {
    .realdebrid_token = "",
    .download_dir = DEFAULT_DOWNLOAD_DIR,
    .auto_install = true,
    .console_filter = 1 /* PS4 by default */
};

static char s_config_path[512] = "";

static void resolve_config_path(void) {
#ifdef __PS4__
    snprintf(s_config_path, sizeof(s_config_path), "%s/settings.ini", DEFAULT_SETTINGS_DIR);
    mkdir(DEFAULT_SETTINGS_DIR, 0777);
#else
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", home, DEFAULT_SETTINGS_DIR);
    mkdir(dir, 0755);
    snprintf(s_config_path, sizeof(s_config_path), "%.480s/settings.ini", dir);
    mkdir(DEFAULT_DOWNLOAD_DIR, 0755);
#endif
}

const char *config_get_base_dir(void) {
    return g_config.download_dir;
}

void config_init(void) {
    resolve_config_path();
    FILE *f = fopen(s_config_path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        while (*k == ' ') k++;
        char *endk = k + strlen(k) - 1;
        while (endk > k && (*endk == ' ' || *endk == '\r' || *endk == '\n')) *endk-- = '\0';
        while (*v == ' ') v++;
        char *endv = v + strlen(v) - 1;
        while (endv > v && (*endv == ' ' || *endv == '\r' || *endv == '\n')) *endv-- = '\0';

        if (strcmp(k, "realdebrid_token") == 0) {
            strncpy(g_config.realdebrid_token, v, sizeof(g_config.realdebrid_token) - 1);
        } else if (strcmp(k, "download_dir") == 0 && v[0]) {
            strncpy(g_config.download_dir, v, sizeof(g_config.download_dir) - 1);
        } else if (strcmp(k, "auto_install") == 0) {
            g_config.auto_install = (atoi(v) != 0);
        } else if (strcmp(k, "console_filter") == 0) {
            g_config.console_filter = atoi(v);
        }
    }
    fclose(f);
}

void config_save(void) {
    if (!s_config_path[0]) resolve_config_path();
    FILE *f = fopen(s_config_path, "w");
    if (!f) return;

    fprintf(f, "# PS4 Direct Downloader Configuration\n");
    fprintf(f, "realdebrid_token=%s\n", g_config.realdebrid_token);
    fprintf(f, "download_dir=%s\n", g_config.download_dir);
    fprintf(f, "auto_install=%d\n", g_config.auto_install ? 1 : 0);
    fprintf(f, "console_filter=%d\n", g_config.console_filter);
    fclose(f);
}
