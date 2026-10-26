#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define CONFIG_TOKEN_LEN 256
#define CONFIG_PATH_LEN  512

typedef struct {
    char realdebrid_token[CONFIG_TOKEN_LEN];
    char download_dir[CONFIG_PATH_LEN];
    bool auto_install;
    int  console_filter; /* 0=All, 1=PS4, 2=PS5, 3=PS3 */
} AppConfig;

extern AppConfig g_config;

void config_init(void);
void config_save(void);
const char *config_get_base_dir(void);

#endif /* CONFIG_H */
