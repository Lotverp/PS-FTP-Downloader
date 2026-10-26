#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

typedef enum {
    STATUS_IDLE = 0,
    STATUS_RESOLVING,
    STATUS_DOWNLOADING,
    STATUS_VERIFYING,
    STATUS_INSTALLING,
    STATUS_COMPLETED,
    STATUS_ERROR
} DownloadStatus;

typedef struct {
    char url[2048];
    char game_title[256];
    char dest_path[512];
    char password[128];
    
    DownloadStatus status;
    int64_t total_size;
    int64_t downloaded_size;
    double  speed_mbps;
    int     eta_seconds;
    float   progress; /* 0.0 - 1.0 */
    
    char status_message[256];
    bool stop_requested;
    pthread_t thread;
} DownloadTask;

extern DownloadTask g_active_dl;

void downloader_init(void);
bool downloader_start(const char *url, const char *game_title, const char *password);
void downloader_stop(void);
void downloader_update_metrics(void);

#endif /* DOWNLOADER_H */
