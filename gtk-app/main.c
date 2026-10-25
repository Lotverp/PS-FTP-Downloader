/*
 * PS FTP Downloader — GTK4 + libcurl
 *
 * Streams files directly from any HTTP(S) URL to a PlayStation console via FTP,
 * using a lock-based ring-buffer pipe — zero local disk usage.
 *
 * Split-archive mode:
 *   Downloads multi-part archives (.rar/.zip/etc.) to a local temp dir,
 *   extracts them with 7zip, then uploads the resulting .pkg via FTP.
 *   All temporary files are deleted after the upload.
 *
 * Architecture:
 *   - Main thread: GTK4 UI only (all widget mutations happen here via g_idle_add)
 *   - Download thread: orchestrates probing + pipe + FTP upload
 *   - HTTP thread: fills the ring-buffer from the HTTP source
 *   - Archive thread: download parts → 7zip extract → FTP upload → cleanup
 *
 * Build:    make
 * Requires: libgtk-4-dev, libcurl4-openssl-dev, build-essential, p7zip-full
 */

#include <gtk/gtk.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════ */

#define APP_ID           "io.psftpdownloader.app"
#define APP_VERSION      "1.2.9"
#define APP_GITHUB_URL   "https://github.com/Lotverp/PS-FTP-Downloader"

#define FTP_HOST_DEFAULT "192.168.1.195"
#define FTP_PORT_DEFAULT  2121
#define FTP_DIR_DEFAULT  "data/pkg"
#define FTP_USER_DEFAULT "anonymous"
#define FTP_PASS_DEFAULT "anonymous@"

#define MAX_DOWNLOADS    32
#define PIPE_BUF_SIZE    (2 * 1024 * 1024)   /* 2 MB lock-based ring-buffer   */
#define SPEED_SAMPLE_HZ  0.35                 /* progress update interval (s)  */
#define SPEED_ALPHA      0.4                  /* EMA weight for old speed       */
#define URL_MIN_LEN      5                    /* sanity threshold for URLs      */
#define EXT_MAX_LEN      11                   /* "." + up to 10-char extension  */

#define MAX_ARCHIVE_PARTS 64                  /* max parts for a split archive  */

/* Convenience: set a curl option and silently discard the return value.
   All options used here are compile-time constants that cannot fail. */
#define CURL_SET(h, opt, val) (void)curl_easy_setopt((h), (opt), (val))

/* ═══════════════════════════════════════════════════════════
 * Download state machine
 * ═══════════════════════════════════════════════════════════ */

typedef enum {
    DL_QUEUED,      /* waiting to be picked up by a thread  */
    DL_PROBING,     /* HEAD request + FTP size probe        */
    DL_ACTIVE,      /* HTTP→pipe→FTP streaming in progress  */
    DL_PAUSED,      /* user-requested pause                 */
    DL_DONE,        /* fully transferred                    */
    DL_ERROR,       /* unrecoverable error (resumable)      */
    DL_CANCELLED,   /* user-cancelled                       */
    /* ── Split-archive specific states ── */
    DL_DOWNLOADING, /* downloading archive parts locally    */
    DL_EXTRACTING,  /* 7zip merge+extract in progress       */
    DL_UPLOADING    /* FTP upload of extracted .pkg         */
} DlStatus;

/* request_stop values written by the main thread, read by the worker */
#define STOP_NONE    0
#define STOP_CANCEL  1
#define STOP_PAUSE   2

typedef struct {
    /* ── Identity ─────────────────────────────────────────── */
    int        id;
    char       url[4096];
    char       dest_name[512];
    char       ext[32];

    /* ── State ─────────────────────────────────────────────── */
    DlStatus   status;
    volatile int request_stop;  /* STOP_* — written by main, read by worker */

    /* ── Transfer metrics ──────────────────────────────────── */
    curl_off_t total_size;
    curl_off_t downloaded;
    curl_off_t resume_offset;
    double     speed;           /* exponential moving average (B/s) */
    double     eta;             /* seconds remaining, or -1         */
    double     progress;        /* 0-100, or -1 if size unknown     */

    /* ── Timing (monotonic) ─────────────────────────────────── */
    struct timespec start_time;
    struct timespec last_sample_time;
    curl_off_t      last_sample_bytes;

    /* ── Error detail ──────────────────────────────────────── */
    char error_msg[256];       /* current error; cleared on resume  */
    char last_error_msg[256];  /* preserved across resumes for popup */

    /* ── GTK widgets (main thread only) ────────────────────── */
    GtkWidget *row_box;
    GtkWidget *lbl_name;
    GtkWidget *lbl_status;
    GtkWidget *lbl_url;
    GtkWidget *lbl_info;
    GtkWidget *progressbar;
    GtkWidget *btn_pause;
    GtkWidget *btn_resume;
    GtkWidget *btn_edit;
    GtkWidget *btn_cancel;
    GtkWidget *btn_remove;

    pthread_t thread;
    void      *sa_args;
    volatile int thread_active;
    int          removed;
} Download;



#define SA_MAX_URLS  MAX_ARCHIVE_PARTS
#define SA_URL_LEN   4096
#define SA_NAME_LEN  512
#define SA_PASS_LEN  256

typedef struct {
    int    dl_id;
    int    current_part_idx;
    int    n_urls;
    char   urls[SA_MAX_URLS][SA_URL_LEN];
    char   pkg_name[SA_NAME_LEN];
    char   password[SA_PASS_LEN];
    char   tmp_dir[512];
} SplitArchiveArgs;

static void free_download_if_ready(Download *dl) {
    if (!dl->thread_active && dl->removed) {
        if (dl->sa_args) {
            SplitArchiveArgs *sa = (SplitArchiveArgs *)dl->sa_args;
            if (sa->tmp_dir[0]) {
                char *argv[] = { "rm", "-rf", sa->tmp_dir, NULL };
                g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
            }
            free(sa);
        }
        free(dl);
    }
}

static void *split_archive_thread_fn(void *arg);

/* ═══════════════════════════════════════════════════════════
 * Global state
 * ═══════════════════════════════════════════════════════════ */

/* Protected by dl_mutex; all array access must hold the lock. */
static Download        *downloads[MAX_DOWNLOADS];
static int              n_downloads = 0;
static pthread_mutex_t  dl_mutex    = PTHREAD_MUTEX_INITIALIZER;
static int              next_id     = 1;

/* FTP connection settings — written only via the Settings dialog. */
static char g_ftp_host[256] = FTP_HOST_DEFAULT;
static int  g_ftp_port      = FTP_PORT_DEFAULT;
static char g_ftp_dir[512]  = FTP_DIR_DEFAULT;
static char g_ftp_user[256] = FTP_USER_DEFAULT;
static char g_ftp_pass[256] = FTP_PASS_DEFAULT;
static char g_tmp_dir[512]  = ""; /* if empty, use g_get_user_cache_dir() */
static char g_saved_passwords[1024] = "SuperPSX,DLPSGAME.COM";

/* Root-level GTK widgets kept for cross-function access. */
static GtkWidget *g_window;
static GtkWidget *g_list_box;
static GtkWidget *g_lbl_stats;
static GtkWidget *g_lbl_sub;
static GtkWidget *g_stack;

/* Dialog entry refs (valid only while the respective dialog is open). */
static GtkWidget *g_entry_url;
static GtkWidget *g_entry_name;
static GtkWidget *g_entry_pass_single;
static GtkWidget *g_lbl_detected;
static GtkWidget *g_set_host, *g_set_port, *g_set_dir, *g_set_user, *g_set_pass, *g_set_tmp, *g_set_saved_pass;

/* Split-archive dialog refs. */
static GtkWidget *g_arch_urls_box;   /* VBox containing one GtkEntry per URL  */
static GtkWidget *g_arch_entry_name; /* destination .pkg filename             */
static GtkWidget *g_arch_entry_pass; /* archive password (may be empty)       */

/* ═══════════════════════════════════════════════════════════
 * Utility: human-readable formatting
 * ═══════════════════════════════════════════════════════════ */

static void fmt_size(curl_off_t bytes, char *out, size_t n) {
    if (bytes <= 0)         { snprintf(out, n, "?");          return; }
    if (bytes < 1024)       { snprintf(out, n, "%lld B",   (long long)bytes); return; }
    if (bytes < 1048576)    { snprintf(out, n, "%.1f KB",  bytes / 1024.0);   return; }
    if (bytes < 1073741824) { snprintf(out, n, "%.1f MB",  bytes / 1048576.0); return; }
    snprintf(out, n, "%.2f GB", bytes / 1073741824.0);
}

static void fmt_speed(double bps, char *out, size_t n) {
    if (bps <= 0)          { snprintf(out, n, "--");          return; }
    if (bps < 1024)        { snprintf(out, n, "%.0f B/s",  bps);          return; }
    if (bps < 1048576)     { snprintf(out, n, "%.1f KB/s", bps / 1024.0); return; }
    if (bps < 1073741824)  { snprintf(out, n, "%.1f MB/s", bps / 1048576.0); return; }
    snprintf(out, n, "%.2f GB/s", bps / 1073741824.0);
}

static void fmt_eta(double secs, char *out, size_t n) {
    if (secs < 0) { snprintf(out, n, "ETA: --"); return; }
    int s = (int)secs;
    if (s < 60)   { snprintf(out, n, "ETA: %ds",      s);            return; }
    if (s < 3600) { snprintf(out, n, "ETA: %dm %ds",  s/60, s%60);  return; }
    snprintf(out, n, "ETA: %dh %dm", s/3600, (s%3600)/60);
}

/* ═══════════════════════════════════════════════════════════
 * Utility: find a Download by ID (caller must hold dl_mutex)
 * ═══════════════════════════════════════════════════════════ */

static Download *find_download(int id) {
    for (int i = 0; i < n_downloads; i++)
        if (downloads[i] && downloads[i]->id == id)
            return downloads[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 * Status-bar statistics (main thread)
 * ═══════════════════════════════════════════════════════════ */

static void update_stats(void) {
    int total = 0, active = 0, queued = 0, done = 0, errors = 0;

    pthread_mutex_lock(&dl_mutex);
    for (int i = 0; i < n_downloads; i++) {
        if (!downloads[i]) continue;
        total++;
        switch (downloads[i]->status) {
            case DL_ACTIVE:
            case DL_DOWNLOADING:
            case DL_EXTRACTING:
            case DL_UPLOADING:                active++; break;
            case DL_QUEUED: case DL_PROBING:
            case DL_PAUSED:                   queued++; break;
            case DL_DONE:                     done++;   break;
            case DL_ERROR:  case DL_CANCELLED: errors++; break;
        }
    }
    pthread_mutex_unlock(&dl_mutex);

    char buf[256];
    if (total == 0)
        snprintf(buf, sizeof(buf), "No transfers");
    else
        snprintf(buf, sizeof(buf),
                 "Total: %d  Active: %d  Queued/Paused: %d  Completed: %d%s",
                 total, active, queued, done,
                 errors ? "  ⚠ Errors" : "");

    gtk_label_set_text(GTK_LABEL(g_lbl_stats), buf);
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack),
                                     total == 0 ? "empty" : "list");
}

/* ═══════════════════════════════════════════════════════════
 * UI update payload — crosses the thread boundary via g_idle_add
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    int        id;
    DlStatus   status;
    curl_off_t total_size;
    curl_off_t downloaded;
    double     speed;
    double     eta;
    double     progress;
    char       error_msg[256];
    char       dest_name[512];
    struct timespec start_time;
} UpdatePayload;

/* Apply the correct CSS badge class for the current download status. */
static void apply_status_badge(Download *dl) {
    static const char *ALL_BADGE_CLASSES[] = {
        "status-queued", "status-active", "status-paused",
        "status-done",   "status-error",  "status-error-clickable",
        "status-extracting", "status-uploading", NULL
    };
    /* Strip every known badge class before re-applying the correct one. */
    for (int i = 0; ALL_BADGE_CLASSES[i]; i++)
        gtk_widget_remove_css_class(dl->lbl_status, ALL_BADGE_CLASSES[i]);

    switch (dl->status) {
        case DL_QUEUED:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "QUEUED");
            gtk_widget_add_css_class(dl->lbl_status, "status-queued");
            break;
        case DL_PROBING:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "PROBING");
            gtk_widget_add_css_class(dl->lbl_status, "status-active");
            break;
        case DL_ACTIVE:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "ACTIVE");
            gtk_widget_add_css_class(dl->lbl_status, "status-active");
            break;
        case DL_PAUSED:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "PAUSED");
            gtk_widget_add_css_class(dl->lbl_status, "status-paused");
            break;
        case DL_DONE:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "DONE");
            gtk_widget_add_css_class(dl->lbl_status, "status-done");
            break;
        case DL_ERROR:
            /* Extra class enables the pointer cursor + hover glow via CSS. */
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "⚠ ERROR");
            gtk_widget_add_css_class(dl->lbl_status, "status-error");
            gtk_widget_add_css_class(dl->lbl_status, "status-error-clickable");
            break;
        case DL_CANCELLED:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "CANCELLED");
            gtk_widget_add_css_class(dl->lbl_status, "status-error");
            break;
        case DL_DOWNLOADING:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "DOWNLOAD");
            gtk_widget_add_css_class(dl->lbl_status, "status-active");
            break;
        case DL_EXTRACTING:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "EXTRACT");
            gtk_widget_add_css_class(dl->lbl_status, "status-extracting");
            break;
        case DL_UPLOADING:
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "UPLOAD");
            gtk_widget_add_css_class(dl->lbl_status, "status-uploading");
            break;
    }
}

/* update_row_ui — runs on the main thread, dispatched by post_update(). */
static gboolean update_row_ui(gpointer user_data) {
    UpdatePayload *p = (UpdatePayload *)user_data;

    /* Merge payload into the canonical Download struct under the lock. */
    Download *dl = NULL;
    pthread_mutex_lock(&dl_mutex);
    dl = find_download(p->id);
    if (dl) {
        dl->status     = p->status;
        dl->total_size = p->total_size;
        dl->downloaded = p->downloaded;
        dl->speed      = p->speed;
        dl->eta        = p->eta;
        dl->progress   = p->progress;
        dl->start_time = p->start_time;
        if (p->error_msg[0]) {
            /* Keep last_error_msg intact so the popup works after a resume. */
            strncpy(dl->error_msg,      p->error_msg, sizeof(dl->error_msg)      - 1);
            strncpy(dl->last_error_msg, p->error_msg, sizeof(dl->last_error_msg) - 1);
        }
    }
    pthread_mutex_unlock(&dl_mutex);

    free(p);

    if (!dl || !dl->row_box) {
        update_stats();
        return G_SOURCE_REMOVE;
    }

    /* ── Widgets ── */
    gtk_label_set_text(GTK_LABEL(dl->lbl_name), dl->dest_name);
    apply_status_badge(dl);

    /* Button visibility: exactly which controls make sense for each state. */
    gboolean running  = (dl->status == DL_QUEUED      ||
                         dl->status == DL_PROBING      ||
                         dl->status == DL_ACTIVE       ||
                         dl->status == DL_DOWNLOADING  ||
                         dl->status == DL_EXTRACTING   ||
                         dl->status == DL_UPLOADING);
    gboolean stalled  = (dl->status == DL_PAUSED  || dl->status == DL_ERROR);
    gboolean terminal = (dl->status == DL_DONE    ||
                         dl->status == DL_ERROR    ||
                         dl->status == DL_CANCELLED);

    gtk_widget_set_visible(dl->btn_pause,  running && dl->status != DL_DOWNLOADING &&
                                           dl->status != DL_EXTRACTING &&
                                           dl->status != DL_UPLOADING);
    gtk_widget_set_visible(dl->btn_resume, stalled);
    gtk_widget_set_visible(dl->btn_edit, stalled);
    gtk_widget_set_visible(dl->btn_cancel, running || dl->status == DL_PAUSED);
    gtk_widget_set_visible(dl->btn_remove, terminal);

    /* ── Progress bar + info line ── */
    char sz_dl[32], sz_tot[32], spd[32], eta_s[32], info[512];
    fmt_size(dl->downloaded, sz_dl,  sizeof(sz_dl));
    fmt_size(dl->total_size,  sz_tot, sizeof(sz_tot));
    fmt_speed(dl->speed, spd, sizeof(spd));
    fmt_eta(dl->eta, eta_s,   sizeof(eta_s));

    gboolean has_size = (dl->progress >= 0.0 && dl->total_size > 0);

    switch (dl->status) {
        case DL_QUEUED:
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progressbar), 0.0);
            snprintf(info, sizeof(info), "Waiting for available slot...");
            break;

        case DL_PROBING:
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dl->progressbar));
            snprintf(info, sizeof(info), "Retrieving file info & checking FTP...");
            break;

        case DL_ACTIVE:
            if (has_size) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progressbar),
                    CLAMP(dl->progress / 100.0, 0.0, 1.0));
                snprintf(info, sizeof(info), "%s / %s  —  %s  —  %s",
                         sz_dl, sz_tot, spd, eta_s);
            } else {
                gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dl->progressbar));
                snprintf(info, sizeof(info), "%s transferred  —  %s", sz_dl, spd);
            }
            break;

        case DL_PAUSED:
            if (has_size) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progressbar),
                    CLAMP(dl->progress / 100.0, 0.0, 1.0));
                snprintf(info, sizeof(info), "Paused  —  %s / %s transferred",
                         sz_dl, sz_tot);
            } else {
                snprintf(info, sizeof(info), "Paused  —  %s transferred", sz_dl);
            }
            break;

        case DL_DONE:
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progressbar), 1.0);
            snprintf(info, sizeof(info), "%s transferred successfully", sz_tot);
            break;

        case DL_ERROR:
            snprintf(info, sizeof(info), "Error: %s  (click the badge for details)",
                     dl->error_msg);
            break;

        case DL_CANCELLED:
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progressbar), 0.0);
            snprintf(info, sizeof(info), "Transfer cancelled.");
            break;

        case DL_DOWNLOADING:
            if (has_size) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progressbar),
                    CLAMP(dl->progress / 100.0, 0.0, 1.0));
                snprintf(info, sizeof(info), "Downloading parts: %s / %s  —  %s  —  %s",
                         sz_dl, sz_tot, spd, eta_s);
            } else {
                gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dl->progressbar));
                snprintf(info, sizeof(info), "Downloading parts: %s  —  %s", sz_dl, spd);
            }
            break;

        case DL_EXTRACTING:
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dl->progressbar));
            snprintf(info, sizeof(info), "Extracting archive with 7zip…");
            break;

        case DL_UPLOADING:
            if (has_size) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dl->progressbar),
                    CLAMP(dl->progress / 100.0, 0.0, 1.0));
                snprintf(info, sizeof(info), "Uploading .pkg via FTP: %s / %s  —  %s  —  %s",
                         sz_dl, sz_tot, spd, eta_s);
            } else {
                gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dl->progressbar));
                snprintf(info, sizeof(info), "Uploading .pkg via FTP…  %s", sz_dl);
            }
            break;
    }

    gtk_label_set_text(GTK_LABEL(dl->lbl_info), info);
    update_stats();
    return G_SOURCE_REMOVE;
}

/* post_update — called from worker threads; marshals state to the main thread. */
static void post_update(Download *dl) {
    UpdatePayload *p = calloc(1, sizeof(UpdatePayload));
    if (!p) return;   /* extremely unlikely; skip the update rather than crash */

    p->id         = dl->id;
    p->status     = dl->status;
    p->total_size = dl->total_size;
    p->downloaded = dl->downloaded;
    p->speed      = dl->speed;
    p->eta        = dl->eta;
    p->progress   = dl->progress;
    p->start_time = dl->start_time;
    strncpy(p->error_msg, dl->error_msg, sizeof(p->error_msg) - 1);
    strncpy(p->dest_name, dl->dest_name, sizeof(p->dest_name) - 1);

    g_idle_add(update_row_ui, p);
}

/* ═══════════════════════════════════════════════════════════
 * Ring-buffer pipe  (HTTP producer ↔ FTP consumer)
 *
 * A fixed-size circular byte buffer protected by a mutex and two
 * condition variables: cond_space (wakes producer when space opens)
 * and cond_data (wakes consumer when data arrives).
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    char   *buf;
    size_t  buf_size;
    size_t  buf_used;       /* bytes currently in the buffer            */
    size_t  buf_read_pos;   /* consumer read-head (wraps at buf_size)   */
    int     http_done;      /* set by HTTP thread after curl finishes   */
    int     cancel;         /* set to drain & abort both sides          */
    curl_off_t http_fetched;/* bytes successfully written to pipe       */
    pthread_mutex_t mutex;
    pthread_cond_t  cond_data;   /* signalled when buf_used > 0         */
    pthread_cond_t  cond_space;  /* signalled when buf_used < buf_size  */
} Pipe;

/* Allocate and initialise a Pipe; returns NULL on allocation failure. */
static Pipe *pipe_create(void) {
    Pipe *p = calloc(1, sizeof(Pipe));
    if (!p) return NULL;
    p->buf = malloc(PIPE_BUF_SIZE);
    if (!p->buf) { free(p); return NULL; }
    p->buf_size = PIPE_BUF_SIZE;
    pthread_mutex_init(&p->mutex,      NULL);
    pthread_cond_init (&p->cond_data,  NULL);
    pthread_cond_init (&p->cond_space, NULL);
    return p;
}

/* Signal both threads to abort and wait for nothing more to drain. */
static void pipe_cancel(Pipe *p) {
    pthread_mutex_lock(&p->mutex);
    p->cancel = 1;
    pthread_cond_broadcast(&p->cond_data);
    pthread_cond_broadcast(&p->cond_space);
    pthread_mutex_unlock(&p->mutex);
}

static void pipe_destroy(Pipe *p) {
    free(p->buf);
    pthread_mutex_destroy(&p->mutex);
    pthread_cond_destroy(&p->cond_data);
    pthread_cond_destroy(&p->cond_space);
    free(p);
}

typedef struct {
    Pipe *pipe;
    CURL *c;
    Download *dl;
    int checked_206;
} HttpWriteCtx;

static size_t null_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; (void)userdata;
    return size * nmemb;
}

static size_t header_parse_range_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    curl_off_t *out_size = (curl_off_t *)userdata;
    if (strncasecmp(buffer, "Content-Range:", 14) == 0) {
        char *slash = strchr(buffer, '/');
        if (slash) *out_size = atoll(slash + 1);
    }
    return total;
}

/* libcurl write callback — HTTP thread calls this to push bytes into the pipe. */
static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    HttpWriteCtx *ctx = (HttpWriteCtx *)userdata;
    Pipe  *pipe  = ctx->pipe;
    size_t total = size * nmemb;
    size_t written = 0;

    if (!ctx->checked_206) {
        ctx->checked_206 = 1;
        long code = 0;
        curl_easy_getinfo(ctx->c, CURLINFO_RESPONSE_CODE, &code);
        if ((ctx->dl->resume_offset + pipe->http_fetched) > 0 && code == 200) {
            snprintf(ctx->dl->error_msg, sizeof(ctx->dl->error_msg),
                     "Server ignored Range request (returned 200). Cannot resume.");
            ctx->dl->status = DL_ERROR;
            post_update(ctx->dl);
            pipe->cancel = 1;
            return 0; /* abort transfer */
        }
    }

    pthread_mutex_lock(&pipe->mutex);
    while (written < total && !pipe->cancel) {
        size_t space = pipe->buf_size - pipe->buf_used;
        if (space == 0) {
            /* Buffer full — wait for the FTP side to consume some data. */
            pthread_cond_wait(&pipe->cond_space, &pipe->mutex);
            continue;
        }
        size_t to_write   = MIN(total - written, space);
        size_t write_pos  = (pipe->buf_read_pos + pipe->buf_used) % pipe->buf_size;
        size_t tail_space = pipe->buf_size - write_pos;
        size_t chunk1     = MIN(to_write, tail_space);

        memcpy(pipe->buf + write_pos, ptr + written, chunk1);
        if (chunk1 < to_write)          /* wrap-around: write remainder at [0] */
            memcpy(pipe->buf, ptr + written + chunk1, to_write - chunk1);

        pipe->buf_used += to_write;
        pipe->http_fetched += to_write;
        written        += to_write;
        pthread_cond_signal(&pipe->cond_data);
    }
    pthread_mutex_unlock(&pipe->mutex);

    /* Returning fewer than `total` bytes tells libcurl to abort. */
    return pipe->cancel ? 0 : written;
}

/* libcurl read callback — FTP thread calls this to pull bytes from the pipe. */
static size_t ftp_read_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Pipe  *pipe = (Pipe *)userdata;
    size_t want = size * nmemb;

    pthread_mutex_lock(&pipe->mutex);
    /* Wait until data arrives or the HTTP side is done (or we are cancelled). */
    while (pipe->buf_used == 0 && !pipe->http_done && !pipe->cancel)
        pthread_cond_wait(&pipe->cond_data, &pipe->mutex);

    if (pipe->cancel) {
        pthread_mutex_unlock(&pipe->mutex);
        return CURL_READFUNC_ABORT;
    }
    if (pipe->buf_used == 0) {
        /* http_done and buffer empty → EOF signalled to libcurl FTP. */
        pthread_mutex_unlock(&pipe->mutex);
        return 0;
    }

    size_t to_read    = MIN(want, pipe->buf_used);
    size_t tail_avail = pipe->buf_size - pipe->buf_read_pos;
    size_t chunk1     = MIN(to_read, tail_avail);

    memcpy(ptr, pipe->buf + pipe->buf_read_pos, chunk1);
    if (chunk1 < to_read)
        memcpy(ptr + chunk1, pipe->buf, to_read - chunk1);

    pipe->buf_read_pos = (pipe->buf_read_pos + to_read) % pipe->buf_size;
    pipe->buf_used    -= to_read;

    pthread_cond_signal(&pipe->cond_space);
    pthread_mutex_unlock(&pipe->mutex);
    return to_read;
}

/* ═══════════════════════════════════════════════════════════
 * HTTP thread — fills the pipe from the HTTP source
 * ═══════════════════════════════════════════════════════════ */

static void curl_common_opts(CURL *c, const char *url) {
    CURL_SET(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    CURL_SET(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURL_SET(c, CURLOPT_MAXREDIRS, 15L);
    CURL_SET(c, CURLOPT_COOKIEFILE, "");        /* abilita il cookie engine */
    CURL_SET(c, CURLOPT_ACCEPT_ENCODING, "");
    CURL_SET(c, CURLOPT_AUTOREFERER, 1L);
    GUri *u = g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (u) {
        char *ref = g_strdup_printf("%s://%s/", g_uri_get_scheme(u), g_uri_get_host(u));
        CURL_SET(c, CURLOPT_REFERER, ref);      /* curl copia la stringa */
        g_free(ref); g_uri_unref(u);
    }
    CURL_SET(c, CURLOPT_LOW_SPEED_LIMIT, 10L);
    CURL_SET(c, CURLOPT_LOW_SPEED_TIME,  60L);
}

typedef struct { Pipe *pipe; Download *dl; } HttpThreadArgs;

static void *http_thread_fn(void *arg) {
    HttpThreadArgs *a    = (HttpThreadArgs *)arg;
    Pipe           *pipe = a->pipe;
    Download       *dl   = a->dl;
    free(a);

    CURL *c = curl_easy_init();
    curl_common_opts(c, dl->url);
    CURL_SET(c, CURLOPT_NOSIGNAL,        1L);
    CURL_SET(c, CURLOPT_URL,            dl->url);
    HttpWriteCtx wc = { pipe, c, dl, 0 };
    CURL_SET(c, CURLOPT_WRITEFUNCTION,  http_write_cb);
    CURL_SET(c, CURLOPT_WRITEDATA,      &wc);
    CURL_SET(c, CURLOPT_SSL_VERIFYPEER, 1L);
    CURL_SET(c, CURLOPT_CONNECTTIMEOUT, 30L);
    CURL_SET(c, CURLOPT_FAILONERROR,    1L);

    CURLcode res = CURLE_OK;
    int max_retries = 30;
    int retries = 0;

    while (retries < max_retries && !dl->request_stop && !pipe->cancel) {
        if (dl->total_size > 0 && (dl->resume_offset + pipe->http_fetched) >= dl->total_size) {
            res = CURLE_OK;
            break;
        }

        if (dl->resume_offset + pipe->http_fetched > 0)
            CURL_SET(c, CURLOPT_RESUME_FROM_LARGE, dl->resume_offset + pipe->http_fetched);

        wc.checked_206 = 0;
        res = curl_easy_perform(c);

        if (res == CURLE_OK || res == CURLE_WRITE_ERROR || pipe->cancel || dl->request_stop)
            break; /* Success or user cancelled */

        gboolean transient = FALSE;
        if (res == CURLE_COULDNT_CONNECT || res == CURLE_RECV_ERROR || 
            res == CURLE_PARTIAL_FILE || res == CURLE_OPERATION_TIMEDOUT) {
            transient = TRUE;
        } else if (res == CURLE_HTTP_RETURNED_ERROR) {
            long code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            if (code == 429 || (code >= 500 && code < 600)) transient = TRUE;
        }

        if (!transient) break;

        /* Transient error -> wait and retry */
        retries++;
        sleep(2);
    }

    pthread_mutex_lock(&pipe->mutex);
    pipe->http_done = 1;
    /* CURLE_WRITE_ERROR means http_write_cb returned 0 (cancelled) — not an error. */
    if (res != CURLE_OK && res != CURLE_WRITE_ERROR &&
        !dl->request_stop && dl->error_msg[0] == '\0') {
        if (res == CURLE_HTTP_RETURNED_ERROR) {
            long code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            snprintf(dl->error_msg, sizeof(dl->error_msg), "HTTP error %ld", code);
        } else {
            strncpy(dl->error_msg, curl_easy_strerror(res), sizeof(dl->error_msg) - 1);
        }
    }
    pthread_cond_broadcast(&pipe->cond_data);
    pthread_mutex_unlock(&pipe->mutex);

    curl_easy_cleanup(c);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 * FTP progress callback — throttled speed/ETA + UI dispatch
 * ═══════════════════════════════════════════════════════════ */

static int xfer_progress_cb(void *clientp,
                             curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)ultotal;
    Download *dl = (Download *)clientp;

    if (dl->request_stop)
        return 1;   /* non-zero aborts the transfer */

    /* libcurl provides ulnow for FTP uploads; dlnow is used as a fallback. */
    curl_off_t session_bytes = ulnow > 0 ? ulnow : dlnow;
    if (session_bytes <= 0) return 0;

    curl_off_t transferred = dl->resume_offset + session_bytes;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec  - dl->last_sample_time.tv_sec) +
                     (now.tv_nsec - dl->last_sample_time.tv_nsec) / 1e9;

    if (elapsed < SPEED_SAMPLE_HZ)
        return 0;   /* not yet time to sample — skip UI update */

    /* Exponential moving average: (1-α)·old + α·new */
    double instant_speed = (double)(transferred - dl->last_sample_bytes) / elapsed;
    dl->speed = (dl->speed > 0)
                ? (SPEED_ALPHA * dl->speed + (1.0 - SPEED_ALPHA) * instant_speed)
                : instant_speed;

    dl->last_sample_time  = now;
    dl->last_sample_bytes = transferred;
    dl->downloaded        = transferred;

    if (dl->total_size > 0) {
        dl->progress = (transferred * 100.0) / dl->total_size;
        dl->eta      = dl->speed > 0
                       ? (double)(dl->total_size - transferred) / dl->speed
                       : -1.0;
    } else {
        dl->progress = -1.0;
        dl->eta      = -1.0;
    }

    post_update(dl);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * Main download thread
 *
 * Sequence:
 *   1. HEAD request  → resolve total file size
 *   2. FTP SIZE probe → determine resume offset (partial remote file)
 *   3. Spawn HTTP thread that fills the ring-buffer
 *   4. FTP STOR (append if resuming) drains the ring-buffer
 *   5. Cleanup and post final status
 * ═══════════════════════════════════════════════════════════ */

/* Build clean FTP URL preserving path slashes (e.g. data/pkg) and escaping components.
   Ensures missing subdirectories like 'pkg' can be created automatically by libcurl. */
static void build_ftp_url(char *out, size_t out_sz,
                          const char *host, int port,
                          const char *dir, const char *filename) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';

    CURL *esc_c = curl_easy_init();
    const char *use_dir = (dir && dir[0]) ? dir : FTP_DIR_DEFAULT;

    /* Trim leading slashes from use_dir */
    while (*use_dir == '/') use_dir++;

    /* Copy and trim trailing slashes */
    char dir_copy[1024];
    strncpy(dir_copy, use_dir, sizeof(dir_copy) - 1);
    dir_copy[sizeof(dir_copy) - 1] = '\0';

    size_t dlen = strlen(dir_copy);
    while (dlen > 0 && dir_copy[dlen - 1] == '/') {
        dir_copy[--dlen] = '\0';
    }

    char clean_dir[2048] = "";
    char *saveptr = NULL;
    char *token = strtok_r(dir_copy, "/", &saveptr);
    gboolean first = TRUE;

    while (token) {
        char *esc_tok = esc_c ? curl_easy_escape(esc_c, token, 0) : NULL;
        if (!first) {
            strncat(clean_dir, "/", sizeof(clean_dir) - strlen(clean_dir) - 1);
        }
        strncat(clean_dir, esc_tok ? esc_tok : token, sizeof(clean_dir) - strlen(clean_dir) - 1);
        if (esc_tok) curl_free(esc_tok);
        first = FALSE;
        token = strtok_r(NULL, "/", &saveptr);
    }

    if (clean_dir[0] == '\0') {
        strncpy(clean_dir, "data/pkg", sizeof(clean_dir) - 1);
    }

    char *esc_name = (esc_c && filename) ? curl_easy_escape(esc_c, filename, 0) : NULL;

    if (filename && filename[0]) {
        snprintf(out, out_sz, "ftp://%s:%d/%s/%s",
                 host, port, clean_dir, esc_name ? esc_name : filename);
    } else {
        snprintf(out, out_sz, "ftp://%s:%d/%s/",
                 host, port, clean_dir);
    }

    if (esc_name) curl_free(esc_name);
    if (esc_c) curl_easy_cleanup(esc_c);
}

/* ═══════════════════════════════════════════════════════════
 * Settings persistence & Remote Directory Management
 * ═══════════════════════════════════════════════════════════ */

static char *get_config_path(void) {
    const char *config_dir = g_get_user_config_dir();
    char *dir = g_build_filename(config_dir, "ps-ftp-downloader", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *file = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);
    return file;
}

static void save_settings(void);

static void load_settings(void) {
    char *path = get_config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *host = g_key_file_get_string(kf, "FTP", "host", NULL);
        if (host && host[0]) {
            strncpy(g_ftp_host, host, sizeof(g_ftp_host) - 1);
            g_free(host);
        }
        int port = g_key_file_get_integer(kf, "FTP", "port", NULL);
        if (port > 0) g_ftp_port = port;

        char *dir = g_key_file_get_string(kf, "FTP", "dir", NULL);
        if (dir && dir[0]) {
            strncpy(g_ftp_dir, dir, sizeof(g_ftp_dir) - 1);
            g_free(dir);
        }
        char *user = g_key_file_get_string(kf, "FTP", "user", NULL);
        if (user && user[0]) {
            strncpy(g_ftp_user, user, sizeof(g_ftp_user) - 1);
            g_free(user);
        }
        char *pass = g_key_file_get_string(kf, "FTP", "pass", NULL);
        if (pass && pass[0]) {
            strncpy(g_ftp_pass, pass, sizeof(g_ftp_pass) - 1);
            g_free(pass);
        }
        char *tmp = g_key_file_get_string(kf, "General", "tmp_dir", NULL);
        if (tmp && tmp[0]) {
            strncpy(g_tmp_dir, tmp, sizeof(g_tmp_dir) - 1);
            g_free(tmp);
        }
        char *saved_pass = g_key_file_get_string(kf, "General", "saved_passwords", NULL);
        if (saved_pass && saved_pass[0]) {
            strncpy(g_saved_passwords, saved_pass, sizeof(g_saved_passwords) - 1);
            g_free(saved_pass);
        }
    } else {
        save_settings();
    }
    g_key_file_free(kf);
    g_free(path);
}

static void save_settings(void) {
    char *path = get_config_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "FTP", "host", g_ftp_host);
    g_key_file_set_integer(kf, "FTP", "port", g_ftp_port);
    g_key_file_set_string(kf, "FTP", "dir", g_ftp_dir);
    g_key_file_set_string(kf, "FTP", "user", g_ftp_user);
    g_key_file_set_string(kf, "FTP", "pass", g_ftp_pass);
    g_key_file_set_string(kf, "General", "tmp_dir", g_tmp_dir);
    g_key_file_set_string(kf, "General", "saved_passwords", g_saved_passwords);

    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}

/* Ensure the target directory structure exists on the FTP server (e.g. /data/pkg)
   by sequentially checking/creating each path segment. */
static gboolean ftp_ensure_dir(const char *host, int port,
                               const char *user, const char *pass,
                               const char *dir, char *err_buf, size_t err_sz) {
    if (!host || !host[0]) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "Host FTP non configurato");
        return FALSE;
    }
    const char *target_dir = (dir && dir[0]) ? dir : FTP_DIR_DEFAULT;

    /* Trim leading slashes */
    while (*target_dir == '/') target_dir++;

    char dir_copy[512];
    strncpy(dir_copy, target_dir, sizeof(dir_copy) - 1);
    dir_copy[sizeof(dir_copy) - 1] = '\0';
    size_t dlen = strlen(dir_copy);
    while (dlen > 0 && dir_copy[dlen - 1] == '/') dir_copy[--dlen] = '\0';

    char full_dir[512];
    strncpy(full_dir, dir_copy, sizeof(full_dir) - 1);
    full_dir[sizeof(full_dir) - 1] = '\0';

    CURL *c = curl_easy_init();
    if (!c) {
        if (err_buf && err_sz > 0) snprintf(err_buf, err_sz, "Impossibile inizializzare cURL");
        return FALSE;
    }

    char url[512];
    snprintf(url, sizeof(url), "ftp://%s:%d/", host, port);

    struct curl_slist *quotes = NULL;
    char *saveptr = NULL;
    char *tok = strtok_r(dir_copy, "/", &saveptr);
    while (tok) {
        char mkd_cmd[512];
        char cwd_cmd[512];
        snprintf(mkd_cmd, sizeof(mkd_cmd), "*MKD %s", tok);
        snprintf(cwd_cmd, sizeof(cwd_cmd), "*CWD %s", tok);
        quotes = curl_slist_append(quotes, mkd_cmd);
        quotes = curl_slist_append(quotes, cwd_cmd);
        tok = strtok_r(NULL, "/", &saveptr);
    }

    CURL_SET(c, CURLOPT_NOSIGNAL, 1L);
    CURL_SET(c, CURLOPT_URL, url);
    CURL_SET(c, CURLOPT_USERNAME, (user && user[0]) ? user : FTP_USER_DEFAULT);
    CURL_SET(c, CURLOPT_PASSWORD, (pass && pass[0]) ? pass : FTP_PASS_DEFAULT);
    CURL_SET(c, CURLOPT_QUOTE, quotes);
    CURL_SET(c, CURLOPT_NOBODY, 1L);
    CURL_SET(c, CURLOPT_CONNECTTIMEOUT, 4L);
    CURL_SET(c, CURLOPT_TIMEOUT, 6L);

    CURLcode res = curl_easy_perform(c);
    gboolean ok = (res == CURLE_OK);

    if (err_buf && err_sz > 0) {
        if (ok) {
            snprintf(err_buf, err_sz, "Connessione riuscita! Cartella /%s pronta.", full_dir);
        } else {
            snprintf(err_buf, err_sz, "Errore FTP (%d): %s", res, curl_easy_strerror(res));
        }
    }

    curl_slist_free_all(quotes);
    curl_easy_cleanup(c);
    return ok;
}

static void *ftp_ensure_dir_bg_thread(void *arg) {
    (void)arg;
    ftp_ensure_dir(g_ftp_host, g_ftp_port, g_ftp_user, g_ftp_pass, g_ftp_dir, NULL, 0);
    return NULL;
}

static void trigger_ftp_ensure_dir_async(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, ftp_ensure_dir_bg_thread, NULL) == 0) {
        pthread_detach(th);
    }
}

typedef struct {
    char host[256];
    int port;
    char dir[512];
    char user[256];
    char pass[256];
    GtkWidget *lbl_result;
    GtkWidget *btn;
} TestFtpData;

static gboolean on_test_done_idle(gpointer user_data) {
    struct { gboolean ok; char msg[512]; GtkWidget *lbl; GtkWidget *btn; } *res = user_data;
    if (res->lbl && GTK_IS_WIDGET(res->lbl)) {
        gtk_label_set_text(GTK_LABEL(res->lbl), res->msg);
        if (res->ok) {
            gtk_widget_remove_css_class(res->lbl, "error-label");
            gtk_widget_add_css_class(res->lbl, "success-label");
        } else {
            gtk_widget_remove_css_class(res->lbl, "success-label");
            gtk_widget_add_css_class(res->lbl, "error-label");
        }
    }
    if (res->btn && GTK_IS_WIDGET(res->btn)) {
        gtk_widget_set_sensitive(res->btn, TRUE);
    }
    free(res);
    return G_SOURCE_REMOVE;
}

static void *test_ftp_worker(void *arg) {
    TestFtpData *data = (TestFtpData *)arg;
    char err[512] = "";
    gboolean ok = ftp_ensure_dir(data->host, data->port, data->user, data->pass, data->dir, err, sizeof(err));

    struct { gboolean ok; char msg[512]; GtkWidget *lbl; GtkWidget *btn; } *res = malloc(sizeof(*res));
    res->ok = ok;
    strncpy(res->msg, err, sizeof(res->msg) - 1);
    res->lbl = data->lbl_result;
    res->btn = data->btn;
    g_idle_add(on_test_done_idle, res);

    free(data);
    return NULL;
}

static void on_test_clicked(GtkButton *btn, gpointer user_data) {
    GtkWidget *lbl_result = GTK_WIDGET(user_data);
    const char *h = gtk_editable_get_text(GTK_EDITABLE(g_set_host));
    const char *p = gtk_editable_get_text(GTK_EDITABLE(g_set_port));
    const char *d = gtk_editable_get_text(GTK_EDITABLE(g_set_dir));
    const char *u = gtk_editable_get_text(GTK_EDITABLE(g_set_user));
    const char *pass = gtk_editable_get_text(GTK_EDITABLE(g_set_pass));

    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    gtk_label_set_text(GTK_LABEL(lbl_result), "Connessione in corso a PS FTP...");
    gtk_widget_remove_css_class(lbl_result, "success-label");
    gtk_widget_remove_css_class(lbl_result, "error-label");

    TestFtpData *td = malloc(sizeof(TestFtpData));
    strncpy(td->host, (h && h[0]) ? h : g_ftp_host, sizeof(td->host) - 1);
    td->port = (p && atoi(p) > 0) ? atoi(p) : g_ftp_port;
    strncpy(td->dir, (d && d[0]) ? d : g_ftp_dir, sizeof(td->dir) - 1);
    strncpy(td->user, (u && u[0]) ? u : g_ftp_user, sizeof(td->user) - 1);
    strncpy(td->pass, (pass && pass[0]) ? pass : g_ftp_pass, sizeof(td->pass) - 1);
    td->lbl_result = lbl_result;
    td->btn = GTK_WIDGET(btn);

    pthread_t th;
    pthread_create(&th, NULL, test_ftp_worker, td);
    pthread_detach(th);
}

static void *download_thread_fn_internal(void *arg) {
    Download *dl = (Download *)arg;
    dl->request_stop = STOP_NONE;

    /* Ensure target remote folder exists on PS4 */
    ftp_ensure_dir(g_ftp_host, g_ftp_port, g_ftp_user, g_ftp_pass, g_ftp_dir, NULL, 0);

    /* ── Step 1: HTTP HEAD — get content length ── */
    dl->status = DL_PROBING;
    post_update(dl);

    {
        CURL *head = curl_easy_init();
        curl_common_opts(head, dl->url);
        CURL_SET(head, CURLOPT_NOSIGNAL,       1L);
        CURL_SET(head, CURLOPT_URL,            dl->url);
        CURL_SET(head, CURLOPT_NOBODY,         1L);
        CURL_SET(head, CURLOPT_CONNECTTIMEOUT, 15L);
        CURL_SET(head, CURLOPT_FAILONERROR,    1L);
        CURLcode res = curl_easy_perform(head);
        if (res == CURLE_HTTP_RETURNED_ERROR) {
            long code = 0;
            curl_easy_getinfo(head, CURLINFO_RESPONSE_CODE, &code);
            fprintf(stderr, "HEAD probe failed: HTTP %ld\n", code);
        }
        curl_off_t cl = 0;
        curl_easy_getinfo(head, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        curl_easy_cleanup(head);
        if (cl > 0) dl->total_size = cl;
    }

    if (dl->total_size <= 0) {
        /* Fallback: GET with Range: bytes=0-0 to read Content-Range */
        CURL *get = curl_easy_init();
        curl_common_opts(get, dl->url);
        CURL_SET(get, CURLOPT_NOSIGNAL,       1L);
        CURL_SET(get, CURLOPT_URL,            dl->url);
        CURL_SET(get, CURLOPT_RANGE,          "0-0");
        CURL_SET(get, CURLOPT_CONNECTTIMEOUT, 15L);
        CURL_SET(get, CURLOPT_FAILONERROR,    1L);
        CURL_SET(get, CURLOPT_WRITEFUNCTION,  null_write_cb);
        CURL_SET(get, CURLOPT_HEADERFUNCTION, header_parse_range_cb);
        curl_off_t parsed_size = 0;
        CURL_SET(get, CURLOPT_HEADERDATA,     &parsed_size);
        
        if (curl_easy_perform(get) == CURLE_OK && parsed_size > 0) {
            dl->total_size = parsed_size;
        }
        curl_easy_cleanup(get);
    }

    /* ── Step 2: FTP SIZE probe — find resume offset ── */
    char ftp_url[2048];
    build_ftp_url(ftp_url, sizeof(ftp_url), g_ftp_host, g_ftp_port, g_ftp_dir, dl->dest_name);

    {
        curl_off_t remote_size = 0;
        CURL *probe = curl_easy_init();
        CURL_SET(probe, CURLOPT_NOSIGNAL,      1L);
        CURL_SET(probe, CURLOPT_URL,            ftp_url);
        CURL_SET(probe, CURLOPT_USERNAME,       g_ftp_user);
        CURL_SET(probe, CURLOPT_PASSWORD,       g_ftp_pass);
        CURL_SET(probe, CURLOPT_NOBODY,         1L);
        CURL_SET(probe, CURLOPT_CONNECTTIMEOUT, 10L);
        if (curl_easy_perform(probe) == CURLE_OK)
            curl_easy_getinfo(probe, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &remote_size);
        curl_easy_cleanup(probe);

        /* Sanity: reject a remote size that exceeds the known total. */
        if (remote_size < 0 ||
            (dl->total_size > 0 && remote_size > dl->total_size))
            remote_size = 0;

        dl->resume_offset = remote_size;
    }

    /* ── Early exit: file is already fully present on the PS ── */
    if (dl->resume_offset > 0 &&
        dl->total_size   > 0 &&
        dl->resume_offset == dl->total_size) {
        dl->status     = DL_DONE;
        dl->downloaded = dl->total_size;
        dl->progress   = 100.0;
        dl->speed      = 0.0;
        post_update(dl);
        return NULL;
    }

    /* ── Step 3: transition to ACTIVE, reset per-session metrics ── */
    dl->status = DL_ACTIVE;
    clock_gettime(CLOCK_MONOTONIC, &dl->start_time);
    dl->last_sample_time  = dl->start_time;
    dl->last_sample_bytes = dl->resume_offset;
    dl->downloaded        = dl->resume_offset;
    dl->speed             = 0.0;
    post_update(dl);

    /* ── Step 4: create pipe, spawn HTTP thread ── */
    Pipe *pipe = pipe_create();
    if (!pipe) {
        strncpy(dl->error_msg, "Out of memory", sizeof(dl->error_msg) - 1);
        dl->status = DL_ERROR;
        post_update(dl);
        return NULL;
    }

    pthread_t http_th;
    {
        HttpThreadArgs *ha = malloc(sizeof(HttpThreadArgs));
        ha->pipe = pipe;
        ha->dl   = dl;
        pthread_create(&http_th, NULL, http_thread_fn, ha);
    }

    /* ── Step 5: FTP STOR (streaming upload from pipe) ── */
    {
        CURL *ftp = curl_easy_init();
        CURL_SET(ftp, CURLOPT_NOSIGNAL,        1L);
        CURL_SET(ftp, CURLOPT_URL,                      ftp_url);
        CURL_SET(ftp, CURLOPT_UPLOAD,                   1L);
        CURL_SET(ftp, CURLOPT_READFUNCTION,             ftp_read_cb);
        CURL_SET(ftp, CURLOPT_READDATA,                 pipe);
        CURL_SET(ftp, CURLOPT_USERNAME,                 g_ftp_user);
        CURL_SET(ftp, CURLOPT_PASSWORD,                 g_ftp_pass);
        CURL_SET(ftp, CURLOPT_XFERINFOFUNCTION,         xfer_progress_cb);
        CURL_SET(ftp, CURLOPT_XFERINFODATA,             dl);
        CURL_SET(ftp, CURLOPT_NOPROGRESS,               0L);
        CURL_SET(ftp, CURLOPT_FTP_CREATE_MISSING_DIRS,  (long)CURLFTP_CREATE_DIR_RETRY);
        CURL_SET(ftp, CURLOPT_FTP_FILEMETHOD,           (long)CURLFTPMETHOD_MULTICWD);
        CURL_SET(ftp, CURLOPT_CONNECTTIMEOUT,           30L);

        if (dl->resume_offset > 0)
            CURL_SET(ftp, CURLOPT_APPEND, 1L);  /* append to partial file */
        else if (dl->total_size > 0)
            CURL_SET(ftp, CURLOPT_INFILESIZE_LARGE, (curl_off_t)dl->total_size);

        CURLcode ftp_res = curl_easy_perform(ftp);
        curl_easy_cleanup(ftp);

        /* Drain the pipe and wake the HTTP thread so it can exit cleanly. */
        pipe_cancel(pipe);
        pthread_join(http_th, NULL);

        /* ── Step 6: resolve final status ── */
        if (dl->request_stop == STOP_CANCEL) {
            dl->status = DL_CANCELLED;
        } else if (dl->request_stop == STOP_PAUSE) {
            dl->status = DL_PAUSED;
        } else if (ftp_res == CURLE_OK && dl->error_msg[0] == '\0') {
            dl->status     = DL_DONE;
            dl->progress   = 100.0;
            dl->downloaded = dl->total_size;
        } else {
            dl->status = DL_ERROR;
            if (dl->error_msg[0] == '\0')
                strncpy(dl->error_msg, curl_easy_strerror(ftp_res),
                        sizeof(dl->error_msg) - 1);
        }
    }

    pipe_destroy(pipe);
    post_update(dl);
    return NULL;
}

static void *download_thread_fn(void *arg) {
    Download *dl = (Download *)arg;
    pthread_mutex_lock(&dl_mutex);
    dl->thread_active = 1;
    pthread_mutex_unlock(&dl_mutex);

    void *ret = download_thread_fn_internal(arg);

    pthread_mutex_lock(&dl_mutex);
    dl->thread_active = 0;
    free_download_if_ready(dl);
    pthread_mutex_unlock(&dl_mutex);
    return ret;
}

/* ═══════════════════════════════════════════════════════════
 * CSS loader
 *
 * Searches for style.css in:
 *   1. Directory of the running executable  (dev / portable)
 *   2. /usr/share/ps-ftp-downloader/        (installed .deb)
 *   3. /usr/local/share/ps-ftp-downloader/  (manual install)
 *   4. CWD "style.css"                      (last resort)
 * ═══════════════════════════════════════════════════════════ */

static void load_css(void) {
    char exe_dir_css[4096] = "";

    /* Resolve path of the running binary to derive its directory. */
    char exe_path[4096] = "";
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *slash = strrchr(exe_path, '/');
        if (slash) {
            *slash = '\0';
            snprintf(exe_dir_css, sizeof(exe_dir_css), "%s/style.css", exe_path);
        }
    }

    const char *candidates[] = {
        exe_dir_css[0] ? exe_dir_css : NULL,
        "/usr/share/ps-ftp-downloader/style.css",
        "/usr/local/share/ps-ftp-downloader/style.css",
        "style.css",
        NULL
    };

    GtkCssProvider *provider = gtk_css_provider_new();

    for (int i = 0; candidates[i]; i++) {
        if (!candidates[i][0]) continue;
        if (g_file_test(candidates[i], G_FILE_TEST_EXISTS)) {
            GFile *file = g_file_new_for_path(candidates[i]);
            gtk_css_provider_load_from_file(provider, file);
            g_object_unref(file);
            break;
        }
    }

    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ═══════════════════════════════════════════════════════════
 * Error popup (main thread) — shown when the user clicks ⚠ ERROR
 * ═══════════════════════════════════════════════════════════ */

static void show_error_popup(int dl_id) {
    char msg[512] = "No additional error information available.";

    pthread_mutex_lock(&dl_mutex);
    Download *dl = find_download(dl_id);
    if (dl) {
        /* Prefer last_error_msg: still set after the user hits Resume. */
        const char *src = dl->last_error_msg[0] ? dl->last_error_msg
                                                 : dl->error_msg;
        if (src && src[0])
            snprintf(msg, sizeof(msg), "%s", src);
    }
    pthread_mutex_unlock(&dl_mutex);

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Error Details");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(g_window));
    gtk_widget_set_size_request(dialog, 400, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);

    GtkWidget *lbl = gtk_label_new(msg);
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_box_append(GTK_BOX(box), lbl);

    GtkWidget *btn = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(btn, "btn-primary");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_box_append(GTK_BOX(box), btn);

    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_window_present(GTK_WINDOW(dialog));
}

/* ═══════════════════════════════════════════════════════════
 * Download row button callbacks
 * ═══════════════════════════════════════════════════════════ */

/* Retrieve the download ID stored on a button via g_object_set_data. */
static int btn_dl_id(GtkButton *btn) {
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "dl-id"));
}

static void on_pause_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    int id = btn_dl_id(btn);
    pthread_mutex_lock(&dl_mutex);
    Download *dl = find_download(id);
    if (dl) dl->request_stop = STOP_PAUSE;
    pthread_mutex_unlock(&dl_mutex);
}

static void on_resume_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    int id = btn_dl_id(btn);

    pthread_mutex_lock(&dl_mutex);
    Download *dl = find_download(id);
    if (dl && (dl->status == DL_PAUSED || dl->status == DL_ERROR)) {
        dl->status       = DL_QUEUED;
        dl->request_stop = STOP_NONE;
        /* Clear the active error string; last_error_msg is retained for
           the popup.  resume_offset is NOT cleared — the download thread
           will re-probe FTP and pick up from the last committed byte. */
        dl->error_msg[0] = '\0';
        if (dl->sa_args) {
            pthread_create(&dl->thread, NULL, split_archive_thread_fn, dl->sa_args);
        } else {
            pthread_create(&dl->thread, NULL, download_thread_fn, dl);
        }
        pthread_detach(dl->thread);
        post_update(dl);
    }
    pthread_mutex_unlock(&dl_mutex);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    int id = btn_dl_id(btn);
    pthread_mutex_lock(&dl_mutex);
    Download *dl = find_download(id);
    if (dl) dl->request_stop = STOP_CANCEL;
    pthread_mutex_unlock(&dl_mutex);
}

static void on_edit_save_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GtkWidget *dialog = GTK_WIDGET(user_data);
    Download *dl = g_object_get_data(G_OBJECT(dialog), "dl");
    GtkTextBuffer *buf = g_object_get_data(G_OBJECT(dialog), "buf");
    GtkWidget *entry_pass = g_object_get_data(G_OBJECT(dialog), "entry_pass");

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

    pthread_mutex_lock(&dl_mutex);
    if (dl->sa_args) {
        SplitArchiveArgs *sa = dl->sa_args;
        sa->n_urls = 0;
        char *line = strtok(text, "\n");
        while (line && sa->n_urls < SA_MAX_URLS) {
            g_strstrip(line);
            if (strlen(line) > 0) {
                strncpy(sa->urls[sa->n_urls++], line, SA_URL_LEN - 1);
            }
            line = strtok(NULL, "\n");
        }
        if (entry_pass) {
            const char *pass = gtk_editable_get_text(GTK_EDITABLE(entry_pass));
            if (pass) strncpy(sa->password, pass, sizeof(sa->password) - 1);
        }
    } else {
        char *line = strtok(text, "\n");
        if (line) {
            g_strstrip(line);
            strncpy(dl->url, line, sizeof(dl->url) - 1);
        }
    }
    pthread_mutex_unlock(&dl_mutex);
    g_free(text);
    
    if (!dl->sa_args) {
        gtk_label_set_text(GTK_LABEL(dl->lbl_url), dl->url);
    }
    
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void on_edit_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    int id = btn_dl_id(btn);
    pthread_mutex_lock(&dl_mutex);
    Download *dl = find_download(id);
    pthread_mutex_unlock(&dl_mutex);
    if (!dl) return;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Edit Transfer");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(g_window));
    gtk_widget_set_size_request(dialog, 500, 300);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);

    GtkWidget *lbl = gtk_label_new("Edit URLs (one per line):");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(box), lbl);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    GtkWidget *tv = gtk_text_view_new();
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), tv);
    gtk_box_append(GTK_BOX(box), scrolled);

    GtkWidget *entry_pass = NULL;
    if (dl->sa_args) {
        SplitArchiveArgs *sa = dl->sa_args;
        for (int i = 0; i < sa->n_urls; i++) {
            gtk_text_buffer_insert_at_cursor(buf, sa->urls[i], -1);
            gtk_text_buffer_insert_at_cursor(buf, "\n", -1);
        }
        
        GtkWidget *lbl_p = gtk_label_new("Password:");
        gtk_label_set_xalign(GTK_LABEL(lbl_p), 0.0);
        gtk_box_append(GTK_BOX(box), lbl_p);
        
        entry_pass = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry_pass), sa->password);
        gtk_box_append(GTK_BOX(box), entry_pass);
    } else {
        gtk_text_buffer_insert_at_cursor(buf, dl->url, -1);
    }

    GtkWidget *btn_save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(btn_save, "btn-primary");
    g_object_set_data(G_OBJECT(dialog), "dl", dl);
    g_object_set_data(G_OBJECT(dialog), "buf", buf);
    g_object_set_data(G_OBJECT(dialog), "entry_pass", entry_pass);
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_edit_save_clicked), dialog);
    gtk_box_append(GTK_BOX(box), btn_save);

    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_remove_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    int id = btn_dl_id(btn);

    pthread_mutex_lock(&dl_mutex);
    for (int i = 0; i < n_downloads; i++) {
        Download *dl = downloads[i];
        if (!dl || dl->id != id) continue;

        /* Remove the GTK row from the list box. */
        if (dl->row_box) {
            GtkWidget *row = gtk_widget_get_parent(dl->row_box);
            if (row) gtk_list_box_remove(GTK_LIST_BOX(g_list_box), row);
        }

        /* Compact the array: shift subsequent entries left. */
        for (int j = i; j < n_downloads - 1; j++)
            downloads[j] = downloads[j + 1];
        downloads[--n_downloads] = NULL;
        
        dl->removed = 1;
        free_download_if_ready(dl);
        break;
    }
    pthread_mutex_unlock(&dl_mutex);
    update_stats();
}

/* Gesture callback — only shows the popup when status is DL_ERROR. */
static void on_error_badge_clicked(GtkGestureClick *gesture, int n_press,
                                    double x, double y, gpointer user_data)
{
    (void)gesture; (void)n_press; (void)x; (void)y;

    int id = GPOINTER_TO_INT(user_data);
    pthread_mutex_lock(&dl_mutex);
    Download *dl = find_download(id);
    gboolean is_error = dl && dl->status == DL_ERROR;
    pthread_mutex_unlock(&dl_mutex);

    if (is_error)
        show_error_popup(id);
}

/* ═══════════════════════════════════════════════════════════
 * Download row factory
 * ═══════════════════════════════════════════════════════════ */

/* Helper: create a small action button, bind its dl-id, attach a signal. */
static GtkWidget *make_action_btn(const char *label, const char *css_class,
                                   int dl_id, GCallback cb)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "btn-icon");
    gtk_widget_add_css_class(btn, css_class);
    g_object_set_data(G_OBJECT(btn), "dl-id", GINT_TO_POINTER(dl_id));
    g_signal_connect(btn, "clicked", cb, NULL);
    return btn;
}

static GtkWidget *create_download_row(Download *dl) {
    /* ── Card (vertical container) ── */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "dl-card");
    dl->row_box = card;

    /* ── Top row: name | badge | buttons ── */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(top, 16);
    gtk_widget_set_margin_end  (top, 16);
    gtk_widget_set_margin_top  (top, 14);

    dl->lbl_name = gtk_label_new(dl->dest_name);
    gtk_label_set_ellipsize(GTK_LABEL(dl->lbl_name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(dl->lbl_name), 0.0);
    gtk_widget_add_css_class(dl->lbl_name, "dl-name");
    gtk_widget_set_hexpand(dl->lbl_name, TRUE);

    /* Status badge — clickable to reveal error details. */
    dl->lbl_status = gtk_label_new("QUEUED");
    gtk_widget_add_css_class(dl->lbl_status, "status-badge");
    gtk_widget_add_css_class(dl->lbl_status, "status-queued");

    GtkGesture *badge_click = gtk_gesture_click_new();
    g_signal_connect(badge_click, "released",
                     G_CALLBACK(on_error_badge_clicked), GINT_TO_POINTER(dl->id));
    gtk_widget_add_controller(dl->lbl_status, GTK_EVENT_CONTROLLER(badge_click));

    dl->btn_resume = make_action_btn("Resume", "btn-resume", dl->id,
                                      G_CALLBACK(on_resume_clicked));
    dl->btn_edit   = make_action_btn("Edit", "btn-edit", dl->id,
                                      G_CALLBACK(on_edit_clicked));
    dl->btn_pause  = make_action_btn("Pause",  "btn-pause",  dl->id,
                                      G_CALLBACK(on_pause_clicked));
    dl->btn_cancel = make_action_btn("Cancel", "btn-cancel", dl->id,
                                      G_CALLBACK(on_cancel_clicked));
    dl->btn_remove = make_action_btn("Remove", "btn-remove", dl->id,
                                      G_CALLBACK(on_remove_clicked));

    /* Resume, Edit and Remove are hidden initially (revealed by update_row_ui). */
    gtk_widget_set_visible(dl->btn_resume, FALSE);
    gtk_widget_set_visible(dl->btn_edit, FALSE);
    gtk_widget_set_visible(dl->btn_remove, FALSE);

    gtk_box_append(GTK_BOX(top), dl->lbl_name);
    gtk_box_append(GTK_BOX(top), dl->lbl_status);
    gtk_box_append(GTK_BOX(top), dl->btn_resume);
    gtk_box_append(GTK_BOX(top), dl->btn_edit);
    gtk_box_append(GTK_BOX(top), dl->btn_pause);
    gtk_box_append(GTK_BOX(top), dl->btn_cancel);
    gtk_box_append(GTK_BOX(top), dl->btn_remove);

    /* ── URL label ── */
    dl->lbl_url = gtk_label_new(dl->url);
    gtk_label_set_ellipsize(GTK_LABEL(dl->lbl_url), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(dl->lbl_url), 0.0);
    gtk_widget_add_css_class(dl->lbl_url, "dl-url");
    gtk_widget_set_margin_start(dl->lbl_url, 16);
    gtk_widget_set_margin_end  (dl->lbl_url, 16);
    gtk_widget_set_margin_top  (dl->lbl_url, 2);

    /* ── Progress bar ── */
    dl->progressbar = gtk_progress_bar_new();
    gtk_widget_add_css_class(dl->progressbar, "dl-progress");
    gtk_widget_set_margin_start(dl->progressbar, 16);
    gtk_widget_set_margin_end  (dl->progressbar, 16);
    gtk_widget_set_margin_top  (dl->progressbar, 10);

    /* ── Info line ── */
    dl->lbl_info = gtk_label_new("Waiting...");
    gtk_label_set_xalign(GTK_LABEL(dl->lbl_info), 0.0);
    gtk_widget_add_css_class(dl->lbl_info, "dl-info");
    gtk_widget_set_margin_start (dl->lbl_info, 16);
    gtk_widget_set_margin_end   (dl->lbl_info, 16);
    gtk_widget_set_margin_top   (dl->lbl_info, 4);
    gtk_widget_set_margin_bottom(dl->lbl_info, 14);

    gtk_box_append(GTK_BOX(card), top);
    gtk_box_append(GTK_BOX(card), dl->lbl_url);
    gtk_box_append(GTK_BOX(card), dl->progressbar);
    gtk_box_append(GTK_BOX(card), dl->lbl_info);

    return card;
}

/* ═══════════════════════════════════════════════════════════
 * Split-archive worker thread
 *
 * Sequence:
 *   1. Create a unique temp directory under ARCHIVE_TMP_DIR
 *   2. Download every part URL into that directory (curl)
 *   3. Run 7zip to extract the first part (auto-resolves all parts)
 *   4. Find the resulting .pkg file
 *   5. Upload the .pkg via FTP using the existing streaming logic
 *   6. Delete the temp directory and all its contents
 * ═══════════════════════════════════════════════════════════ */


/* ── Minimal file-write callback used by curl during part download ── */
typedef struct {
    FILE *fp;
    CURL *curl;
    int checked_first;
    curl_off_t offset;
    int is_html;
    int bad_resume;
} FileWriteCtx;

static size_t file_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    FileWriteCtx *ctx = (FileWriteCtx *)ud;
    size_t total_bytes = size * nmemb;

    if (!ctx->checked_first) {
        ctx->checked_first = 1;
        char *ct = NULL;
        curl_easy_getinfo(ctx->curl, CURLINFO_CONTENT_TYPE, &ct);
        if (ct && strstr(ct, "text/html")) {
            ctx->is_html = 1;
            return 0; /* abort download */
        }
        if (ctx->offset > 0) {
            long code = 0;
            curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &code);
            if (code != 206) {
                ctx->bad_resume = 1;
                return 0; /* abort download */
            }
        }
    }

    size_t written = fwrite(ptr, 1, total_bytes, ctx->fp);
    return written;
}

static size_t file_read_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    FILE *fp = (FILE *)ud;
    return fread(ptr, size, nmemb, fp);
}

/* ── Part download progress (updates the Download row while in DL_DOWNLOADING) ── */
typedef struct {
    Download   *dl;
    int         part_idx;   /* 0-based index of the part being downloaded  */
    int         n_parts;
    curl_off_t  part_total; /* expected size of this part (may be 0)       */
} PartProgressCtx;

static int part_progress_cb(void *clientp,
                             curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    PartProgressCtx *ctx = (PartProgressCtx *)clientp;
    Download *dl = ctx->dl;

    if (dl->request_stop) return 1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec  - dl->last_sample_time.tv_sec) +
                     (now.tv_nsec - dl->last_sample_time.tv_nsec) / 1e9;
    if (elapsed < SPEED_SAMPLE_HZ) return 0;

    double instant_speed = (double)(dlnow - dl->last_sample_bytes) / elapsed;
    dl->speed = (dl->speed > 0)
                ? (SPEED_ALPHA * dl->speed + (1.0 - SPEED_ALPHA) * instant_speed)
                : instant_speed;
    dl->last_sample_time  = now;
    dl->last_sample_bytes = dlnow;
    dl->downloaded        = dlnow;

    if (dltotal > 0) {
        /* Overall progress: parts already done + fraction of current part */
        double overall = (double)ctx->part_idx / ctx->n_parts
                       + (dlnow * 1.0 / dltotal) / ctx->n_parts;
        dl->progress  = overall * 100.0;
        dl->total_size = (curl_off_t)(dltotal * ctx->n_parts); /* rough estimate */
        dl->eta = dl->speed > 0
                  ? (double)(dl->total_size - dl->downloaded) / dl->speed
                  : -1.0;
    } else {
        dl->progress   = -1.0;
        dl->total_size = -1;
        dl->eta        = -1.0;
    }

    post_update(dl);
    return 0;
}

/* ── FTP upload progress reused from the main flow ── */
static int sa_ftp_progress_cb(void *clientp,
                               curl_off_t dltotal, curl_off_t dlnow,
                               curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow;
    return xfer_progress_cb(clientp, 0, 0, ultotal, ulnow);
}

/* ── Recursive directory removal ── */
static void rm_dir_recursive(const char *path) {
    char *argv[] = { "rm", "-rf", (char *)path, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
}

static int run_extract(const char *bin, const char *dir, const char *pass,
                       const char *archive, char *err, size_t errsz) {
    char *o = NULL;
    char *p = (pass && pass[0]) ? g_strdup_printf("-p%s", pass) : NULL;
    char *argv[9]; int n = 0;
    
    argv[n++] = (char *)bin; argv[n++] = "x"; argv[n++] = "-y";
    if (strcmp(bin, "unrar") == 0) {
        if (p) argv[n++] = p;
        argv[n++] = (char *)archive;
        o = g_strdup_printf("%s/", dir);
        argv[n++] = o;
    } else {
        o = g_strdup_printf("-o%s", dir);
        argv[n++] = o;
        if (p) argv[n++] = p;
        argv[n++] = "--"; argv[n++] = (char *)archive;
    }
    argv[n] = NULL;

    char *out = NULL, *serr = NULL; int st = 0; GError *e = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, &out, &serr, &st, &e);
    int rc = (!ok) ? -1 : (WIFEXITED(st) ? WEXITSTATUS(st) : -2);
    if (rc != 0) snprintf(err, errsz, "%s exit %d: %.150s", bin, rc,
                          ok ? (serr && *serr ? serr : out) : (e ? e->message : "spawn failed"));
    g_free(o); g_free(p); g_free(out); g_free(serr); if (e) g_error_free(e);
    return rc;
}

static void find_largest_pkg_recursive(const char *dir, char *best_path, off_t *best_size) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        char *path = g_build_filename(dir, name, NULL);
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                find_largest_pkg_recursive(path, best_path, best_size);
            } else if (S_ISREG(st.st_mode) && g_str_has_suffix(name, ".pkg")) {
                if (st.st_size > *best_size) {
                    *best_size = st.st_size;
                    strcpy(best_path, path);
                }
            }
        }
        g_free(path);
    }
    g_dir_close(d);
}

/* ── Worker thread entry point ── */
static void *split_archive_thread_fn_internal(void *arg) {
    SplitArchiveArgs *sa = (SplitArchiveArgs *)arg;

    /* Locate the Download struct for status updates. */
    pthread_mutex_lock(&dl_mutex);
    Download *dl = find_download(sa->dl_id);
    pthread_mutex_unlock(&dl_mutex);

    if (!dl) { free(sa); return NULL; }

    /* Ensure target remote folder exists on PS4 */
    ftp_ensure_dir(g_ftp_host, g_ftp_port, g_ftp_user, g_ftp_pass, g_ftp_dir, NULL, 0);

    /* ────────────────────────────────────────────────────────
     * Phase 1: create temp directory
     * ──────────────────────────────────────────────────────── */
    const char *base_cache_dir = g_tmp_dir[0] ? g_tmp_dir : g_get_user_cache_dir();
    char base_dir[512];
    snprintf(base_dir, sizeof(base_dir), "%s/ps-ftp-arch", base_cache_dir);
    
    if (mkdir(base_dir, 0755) != 0 && errno != EEXIST) {
        snprintf(dl->error_msg, sizeof(dl->error_msg),
                 "Cannot create base temp dir %s: %s", base_dir, strerror(errno));
        dl->status = DL_ERROR;
        post_update(dl);
        return NULL;
    }
    if (mkdir(sa->tmp_dir, 0755) != 0 && errno != EEXIST) {
        snprintf(dl->error_msg, sizeof(dl->error_msg),
                 "Cannot create job dir %s: %s", sa->tmp_dir, strerror(errno));
        dl->status = DL_ERROR;
        post_update(dl);
        return NULL;
    }

    /* ────────────────────────────────────────────────────────
     * Phase 2: download every part
     * ──────────────────────────────────────────────────────── */
    dl->status        = DL_DOWNLOADING;
    dl->downloaded    = 0;
    dl->total_size    = -1;
    dl->speed         = 0.0;
    dl->eta           = -1.0;
    dl->progress      = -1.0;
    clock_gettime(CLOCK_MONOTONIC, &dl->start_time);
    dl->last_sample_time  = dl->start_time;
    dl->last_sample_bytes = 0;
    post_update(dl);

    char part_paths[SA_MAX_URLS][SA_URL_LEN]; /* local paths of downloaded parts */

    for (int i = 0; i < sa->n_urls; i++) {
        char basename[512] = "";
        char tmp[SA_URL_LEN];
        strncpy(tmp, sa->urls[i], sizeof(tmp) - 1);
        char *q = strchr(tmp, '?');
        if (q) *q = '\0';
        char *sl = strrchr(tmp, '/');
        const char *raw_base = sl ? sl + 1 : "part";
        if (!raw_base[0]) {
            snprintf(basename, sizeof(basename), "part%02d", i + 1);
        } else {
            char *decoded = curl_easy_unescape(NULL, raw_base, 0, NULL);
            if (decoded) {
                snprintf(basename, sizeof(basename), "%s", decoded);
                curl_free(decoded);
            } else {
                snprintf(basename, sizeof(basename), "%s", raw_base);
            }
        }
        snprintf(part_paths[i], sizeof(part_paths[i]), "%s/%s", sa->tmp_dir, basename);
    }

    for (int i = sa->current_part_idx; i < sa->n_urls && !dl->request_stop; i++) {
        curl_off_t offset = 0;
        struct stat st;
        if (stat(part_paths[i], &st) == 0) {
            offset = st.st_size;
        }

        FILE *fp = fopen(part_paths[i], "ab");
        if (!fp) {
            snprintf(dl->error_msg, sizeof(dl->error_msg),
                     "Cannot open %s for appending", part_paths[i]);
            dl->status = DL_ERROR;
            post_update(dl);
            rm_dir_recursive(sa->tmp_dir);
            return NULL;
        }

        CURL *c = curl_easy_init();
        FileWriteCtx fw = { fp, c, 0, offset, 0, 0 };
        PartProgressCtx pc = { dl, i, sa->n_urls, 0 };
        dl->last_sample_bytes = 0;
        curl_common_opts(c, sa->urls[i]);
        if (offset > 0) {
            CURL_SET(c, CURLOPT_RESUME_FROM_LARGE, offset);
        }
        CURL_SET(c, CURLOPT_NOSIGNAL,        1L);
        CURL_SET(c, CURLOPT_URL,             sa->urls[i]);
        CURL_SET(c, CURLOPT_WRITEFUNCTION,   file_write_cb);
        CURL_SET(c, CURLOPT_WRITEDATA,       &fw);
        CURL_SET(c, CURLOPT_SSL_VERIFYPEER,  1L);
        CURL_SET(c, CURLOPT_CONNECTTIMEOUT,  30L);
        CURL_SET(c, CURLOPT_FAILONERROR,     1L);
        CURL_SET(c, CURLOPT_XFERINFOFUNCTION, part_progress_cb);
        CURL_SET(c, CURLOPT_XFERINFODATA,    &pc);
        CURL_SET(c, CURLOPT_NOPROGRESS,      0L);

        CURLcode res = curl_easy_perform(c);
        
        if (fw.is_html) {
            snprintf(dl->error_msg, sizeof(dl->error_msg),
                     "URL points to a web page (HTML), not a direct file.");
            res = CURLE_WRITE_ERROR;
        } else if (fw.bad_resume) {
            snprintf(dl->error_msg, sizeof(dl->error_msg),
                     "Server ignored resume and returned full file (HTTP 200).");
            res = CURLE_WRITE_ERROR;
        } else if (res == CURLE_HTTP_RETURNED_ERROR) {
            long code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            snprintf(dl->error_msg, sizeof(dl->error_msg),
                     "Part %d: HTTP %ld", i + 1, code);
        } else if (res != CURLE_OK) {
            snprintf(dl->error_msg, sizeof(dl->error_msg),
                     "Part %d download failed: %s", i + 1, curl_easy_strerror(res));
        }

        curl_easy_cleanup(c);
        fclose(fp);

        if (dl->request_stop) break;

        if (res != CURLE_OK) {
            dl->status = DL_ERROR;
            post_update(dl);
            return NULL;
        }
        sa->current_part_idx = i + 1;
    }

    /* Check cancellation after the loop. */
    if (dl->request_stop) {
        dl->status = (dl->request_stop == STOP_CANCEL) ? DL_CANCELLED : DL_PAUSED;
        post_update(dl);
        if (dl->request_stop == STOP_CANCEL) rm_dir_recursive(sa->tmp_dir);
        return NULL;
    }

    /* ────────────────────────────────────────────────────────
     * Phase 3: extract
     * ──────────────────────────────────────────────────────── */
    dl->status   = DL_EXTRACTING;
    dl->progress = -1.0;
    dl->speed    = 0.0;
    dl->eta      = -1.0;
    post_update(dl);

    {
        const char *bins[] = { "7zz", "7z", "7za", "unrar", NULL };
        int ret = -1;
        int last_meaningful_ret = -1;
        for (int b = 0; bins[b]; b++) {
            char tmp_err[512] = "";
            ret = run_extract(bins[b], sa->tmp_dir, sa->password, part_paths[0],
                              tmp_err, sizeof(tmp_err));
            if (ret == 0) break;
            if (ret != -1) {
                last_meaningful_ret = ret;
                strncpy(dl->error_msg, tmp_err, sizeof(dl->error_msg) - 1);
            }
        }
        if (ret != 0) {
            if (last_meaningful_ret == -1) {
                snprintf(dl->error_msg, sizeof(dl->error_msg), "No extraction tool found. Please install p7zip-full or unrar.");
            }
            dl->status = DL_ERROR;
            post_update(dl);
            rm_dir_recursive(sa->tmp_dir);
            return NULL;
        }
    }

    /* ────────────────────────────────────────────────────────
     * Phase 4: locate the extracted .pkg
     * ──────────────────────────────────────────────────────── */
    char pkg_path[1024] = "";
    off_t best_size = -1;
    find_largest_pkg_recursive(sa->tmp_dir, pkg_path, &best_size);

    if (pkg_path[0] == '\0') {
        snprintf(dl->error_msg, sizeof(dl->error_msg),
                 "No .pkg file found after extraction.");
        dl->status = DL_ERROR;
        post_update(dl);
        rm_dir_recursive(sa->tmp_dir);
        return NULL;
    }

    /* ────────────────────────────────────────────────────────
     * Phase 5: FTP upload of the extracted .pkg
     *
     * We open the file, get its size, then stream it via curl
     * using a simple file-read callback.
     * ──────────────────────────────────────────────────────── */
    dl->status        = DL_UPLOADING;
    dl->downloaded    = 0;
    dl->speed         = 0.0;
    dl->eta           = -1.0;
    dl->progress      = -1.0;
    dl->resume_offset = 0;
    clock_gettime(CLOCK_MONOTONIC, &dl->start_time);
    dl->last_sample_time  = dl->start_time;
    dl->last_sample_bytes = 0;
    post_update(dl);

    FILE *pkg_fp = fopen(pkg_path, "rb");
    if (!pkg_fp) {
        snprintf(dl->error_msg, sizeof(dl->error_msg),
                 "Cannot open extracted .pkg: %s", strerror(errno));
        dl->status = DL_ERROR;
        post_update(dl);
        return NULL;
    }

    /* Determine file size. */
    fseeko(pkg_fp, 0, SEEK_END);
    off_t pkg_size = ftello(pkg_fp);
    fseeko(pkg_fp, 0, SEEK_SET);
    dl->total_size = (curl_off_t)pkg_size;

    /* Build FTP destination URL. */
    char ftp_url[2048];
    build_ftp_url(ftp_url, sizeof(ftp_url), g_ftp_host, g_ftp_port, g_ftp_dir, sa->pkg_name);

    /* Reuse the xfer_progress_cb; it reads dl->resume_offset + ulnow. */
    CURL *ftp = curl_easy_init();
    CURL_SET(ftp, CURLOPT_NOSIGNAL,        1L);
    CURL_SET(ftp, CURLOPT_URL,                      ftp_url);
    CURL_SET(ftp, CURLOPT_UPLOAD,                   1L);
    CURL_SET(ftp, CURLOPT_READFUNCTION,             file_read_cb);
    CURL_SET(ftp, CURLOPT_READDATA,                 pkg_fp);
    CURL_SET(ftp, CURLOPT_INFILESIZE_LARGE,         (curl_off_t)pkg_size);
    CURL_SET(ftp, CURLOPT_USERNAME,                 g_ftp_user);
    CURL_SET(ftp, CURLOPT_PASSWORD,                 g_ftp_pass);
    CURL_SET(ftp, CURLOPT_XFERINFOFUNCTION,         xfer_progress_cb);
    CURL_SET(ftp, CURLOPT_XFERINFODATA,             dl);
    CURL_SET(ftp, CURLOPT_NOPROGRESS,               0L);
    CURL_SET(ftp, CURLOPT_FTP_CREATE_MISSING_DIRS,  (long)CURLFTP_CREATE_DIR_RETRY);
    CURL_SET(ftp, CURLOPT_FTP_FILEMETHOD,           (long)CURLFTPMETHOD_MULTICWD);
    CURL_SET(ftp, CURLOPT_CONNECTTIMEOUT,           30L);

    CURLcode ftp_res = curl_easy_perform(ftp);
    curl_easy_cleanup(ftp);
    fclose(pkg_fp);

    /* ────────────────────────────────────────────────────────
     * Phase 6: cleanup temp directory
     * ──────────────────────────────────────────────────────── */
    if (ftp_res == CURLE_OK && dl->error_msg[0] == '\0') {
        rm_dir_recursive(sa->tmp_dir);
        sa->tmp_dir[0] = '\0';
    }

    /* ────────────────────────────────────────────────────────
     * Final status
     * ──────────────────────────────────────────────────────── */
    if (dl->request_stop == STOP_CANCEL) {
        dl->status = DL_CANCELLED;
    } else if (ftp_res == CURLE_OK && dl->error_msg[0] == '\0') {
        dl->status     = DL_DONE;
        dl->progress   = 100.0;
        dl->downloaded = dl->total_size;
    } else {
        dl->status = DL_ERROR;
        if (dl->error_msg[0] == '\0')
            strncpy(dl->error_msg, curl_easy_strerror(ftp_res),
                    sizeof(dl->error_msg) - 1);
    }

    post_update(dl);
    return NULL;
}

static void *split_archive_thread_fn(void *arg) {
    SplitArchiveArgs *sa = (SplitArchiveArgs *)arg;
    Download *dl = NULL;
    pthread_mutex_lock(&dl_mutex);
    for (int i = 0; i < n_downloads; i++) {
        if (downloads[i] && downloads[i]->id == sa->dl_id) {
            dl = downloads[i];
            break;
        }
    }
    if (dl) dl->thread_active = 1;
    pthread_mutex_unlock(&dl_mutex);

    void *ret = split_archive_thread_fn_internal(arg);

    if (dl) {
        pthread_mutex_lock(&dl_mutex);
        dl->thread_active = 0;
        free_download_if_ready(dl);
        pthread_mutex_unlock(&dl_mutex);
    }
    return ret;
}

/* ═══════════════════════════════════════════════════════════
 * URL probe (background thread)
 *
 * Fires a HEAD request against the URL entered by the user
 * and posts back the detected extension and file size.
 * ═══════════════════════════════════════════════════════════ */

typedef struct { char url[4096]; } ProbeArgs;
typedef struct { char ext[32]; curl_off_t size; } ProbeResult;

static gboolean do_probe_idle(gpointer user_data) {
    ProbeResult *r = (ProbeResult *)user_data;

    char sz[32], info[256];
    fmt_size(r->size, sz, sizeof(sz));

    if      (r->ext[0] && r->size > 0)
        snprintf(info, sizeof(info), "Extension: .%s  —  Size: %s", r->ext, sz);
    else if (r->ext[0])
        snprintf(info, sizeof(info), "Extension: .%s  —  Size: unknown", r->ext);
    else if (r->size > 0)
        snprintf(info, sizeof(info), "Size: %s", sz);
    else
        snprintf(info, sizeof(info), "Unable to detect file information");

    if (g_lbl_detected)
        gtk_label_set_text(GTK_LABEL(g_lbl_detected), info);

    if (r->ext[0] && g_entry_name) {
        char ph[128];
        snprintf(ph, sizeof(ph), "e.g. my_game  (.%s will be appended)", r->ext);
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_entry_name), ph);
    }

    free(r);
    return G_SOURCE_REMOVE;
}

static void *probe_thread_fn(void *arg) {
    ProbeArgs *a = (ProbeArgs *)arg;

    CURL *c = curl_easy_init();
    curl_common_opts(c, a->url);
    CURL_SET(c, CURLOPT_NOSIGNAL,          1L);
    CURL_SET(c, CURLOPT_URL,            a->url);
    CURL_SET(c, CURLOPT_NOBODY,         1L);
    CURL_SET(c, CURLOPT_CONNECTTIMEOUT, 10L);
    CURL_SET(c, CURLOPT_FAILONERROR,    1L);
    CURLcode res = curl_easy_perform(c);
    
    if (res == CURLE_HTTP_RETURNED_ERROR) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        fprintf(stderr, "Probe failed: HTTP %ld for URL: %s\n", code, a->url);
    }

    curl_off_t cl       = 0;
    char      *eff_url  = NULL;
    curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff_url);

    /* Derive extension from the effective (post-redirect) URL. */
    char ext[32] = "";
    if (eff_url) {
        char tmp[4096];
        strncpy(tmp, eff_url, sizeof(tmp) - 1);
        char *q = strchr(tmp, '?');
        if (q) *q = '\0';
        char *dot = strrchr(tmp, '.');
        if (dot && strlen(dot) > 1 && strlen(dot) <= EXT_MAX_LEN)
            strncpy(ext, dot + 1, sizeof(ext) - 1);
    }

    curl_easy_cleanup(c);
    free(a);

    ProbeResult *r = malloc(sizeof(ProbeResult));
    strncpy(r->ext, ext, sizeof(r->ext) - 1);
    r->size = cl;
    g_idle_add(do_probe_idle, r);
    return NULL;
}

static guint g_probe_timeout_id = 0;
static char *g_probe_pending_url = NULL;

static gboolean do_probe_timeout(gpointer user_data) {
    if (!g_probe_pending_url) return G_SOURCE_REMOVE;

    ProbeArgs *a = malloc(sizeof(ProbeArgs));
    strncpy(a->url, g_probe_pending_url, sizeof(a->url) - 1);
    
    pthread_t th;
    pthread_create(&th, NULL, probe_thread_fn, a);
    pthread_detach(th);

    g_free(g_probe_pending_url);
    g_probe_pending_url = NULL;
    g_probe_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

/* Fired when the URL entry changes — debounce via minimum length check. */
static void on_url_changed(GtkEditable *editable, gpointer user_data) {
    (void)user_data;
    const char *url = gtk_editable_get_text(editable);

    if (g_probe_timeout_id > 0) {
        g_source_remove(g_probe_timeout_id);
        g_probe_timeout_id = 0;
    }

    if (!url || strlen(url) < (size_t)URL_MIN_LEN) {
        if (g_lbl_detected)
            gtk_label_set_text(GTK_LABEL(g_lbl_detected),
                               "Enter a URL for automatic detection");
        return;
    }

    if (g_lbl_detected)
        gtk_label_set_text(GTK_LABEL(g_lbl_detected), "Analyzing URL…");

    if (g_probe_pending_url) g_free(g_probe_pending_url);
    g_probe_pending_url = g_strdup(url);
    g_probe_timeout_id = g_timeout_add(600, do_probe_timeout, NULL);
}

static gboolean is_known_ext(const char *ext) {
    if (!ext) return FALSE;
    if (*ext == '.') ext++;
    const char *known[] = {"pkg", "rar", "zip", "7z", "001", "tar", "gz", "bz2", "xz", "002", "part1", "part01", NULL};
    for (int i = 0; known[i]; i++) {
        if (strcasecmp(ext, known[i]) == 0) return TRUE;
    }
    return FALSE;
}

/* ═══════════════════════════════════════════════════════════
 * Shared helper: extract a file extension from a URL path
 * ═══════════════════════════════════════════════════════════ */

static void extract_url_ext(const char *url, char *ext, size_t ext_size) {
    char tmp[4096];
    strncpy(tmp, url, sizeof(tmp) - 1);
    char *q = strchr(tmp, '?');
    if (q) *q = '\0';
    char *dot = strrchr(tmp, '.');
    if (dot && strlen(dot) > 1 && strlen(dot) <= EXT_MAX_LEN)
        strncpy(ext, dot + 1, ext_size - 1);
}

/* ═══════════════════════════════════════════════════════════
 * "New Download" dialog
 * ═══════════════════════════════════════════════════════════ */

static void on_add_dialog_response(GtkDialog *dialog, int response,
                                    gpointer user_data) {
    (void)user_data;

    if (response != GTK_RESPONSE_OK) {
        g_entry_url = g_entry_name = g_entry_pass_single = g_lbl_detected = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    const char *raw_url = gtk_editable_get_text(GTK_EDITABLE(g_entry_url));
    const char *name = gtk_editable_get_text(GTK_EDITABLE(g_entry_name));

    if (!raw_url || strlen(raw_url) < (size_t)URL_MIN_LEN) {
        g_entry_url = g_entry_name = g_entry_pass_single = g_lbl_detected = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    char url[4096] = "";
    strncpy(url, raw_url, sizeof(url) - 1);
    g_strstrip(url);

    if (strlen(url) < (size_t)URL_MIN_LEN) {
        g_entry_url = g_entry_name = g_entry_pass_single = g_lbl_detected = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    /* Detect extension from URL. */
    char det_ext[32] = "";
    extract_url_ext(url, det_ext, sizeof(det_ext));

    /* Build destination filename. */
    char full_name[512] = "";
    if (name && strlen(name) > 0) {
        const char *dot = strrchr(name, '.');
        if (dot && is_known_ext(dot))
            snprintf(full_name, sizeof(full_name), "%s", name);   /* has extension */
        else if (det_ext[0])
            snprintf(full_name, sizeof(full_name), "%s.%s", name, det_ext);
        else
            snprintf(full_name, sizeof(full_name), "%s", name);
    } else {
        /* Fall back to the last path component of the URL. */
        char tmp[4096];
        strncpy(tmp, url, sizeof(tmp) - 1);
        char *q = strchr(tmp, '?');
        if (q) *q = '\0';
        char *slash = strrchr(tmp, '/');
        char *raw_name = slash ? slash + 1 : "file";
        char *unescaped = g_uri_unescape_string(raw_name, NULL);
        snprintf(full_name, sizeof(full_name), "%s", unescaped ? unescaped : raw_name);
        if (unescaped) g_free(unescaped);
    }

    /* Allocate and register the new Download. */
    pthread_mutex_lock(&dl_mutex);
    if (n_downloads >= MAX_DOWNLOADS) {
        pthread_mutex_unlock(&dl_mutex);
        g_entry_url = g_entry_name = g_entry_pass_single = g_lbl_detected = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    Download *dl = calloc(1, sizeof(Download));
    dl->id         = next_id++;
    dl->status     = DL_QUEUED;
    dl->total_size = -1;
    strncpy(dl->url,       url,       sizeof(dl->url)       - 1);
    strncpy(dl->dest_name, full_name, sizeof(dl->dest_name) - 1);
    strncpy(dl->ext,       det_ext,   sizeof(dl->ext)       - 1);
    downloads[n_downloads++] = dl;
    pthread_mutex_unlock(&dl_mutex);

    GtkWidget *row = create_download_row(dl);
    gtk_list_box_append(GTK_LIST_BOX(g_list_box), row);
    update_stats();

    trigger_ftp_ensure_dir_async();

    if (strcasecmp(det_ext, "rar") == 0 || strcasecmp(det_ext, "zip") == 0 || strcasecmp(det_ext, "7z") == 0) {
        SplitArchiveArgs *sa = calloc(1, sizeof(SplitArchiveArgs));
        sa->n_urls = 1;
        strncpy(sa->urls[0], url, SA_URL_LEN - 1);
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        char *pass = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_entry_pass_single));
G_GNUC_END_IGNORE_DEPRECATIONS
        if (pass) {
            strncpy(sa->password, pass, sizeof(sa->password) - 1);
            g_free(pass);
        }

        char dest_pkg[SA_NAME_LEN];
        snprintf(dest_pkg, sizeof(dest_pkg), "%s", full_name);
        char *dot = strrchr(dest_pkg, '.');
        if (dot) snprintf(dot, sizeof(dest_pkg) - (dot - dest_pkg), ".pkg");
        else snprintf(dest_pkg + strlen(dest_pkg), sizeof(dest_pkg) - strlen(dest_pkg), ".pkg");
        
        strncpy(sa->pkg_name, dest_pkg, sizeof(sa->pkg_name) - 1);
        strncpy(dl->dest_name, dest_pkg, sizeof(dl->dest_name) - 1);
        sa->dl_id = dl->id;
        dl->sa_args = sa;

        const char *base_cache_dir = g_tmp_dir[0] ? g_tmp_dir : g_get_user_cache_dir();
        char safe[64] = "";
        strncpy(safe, dest_pkg, sizeof(safe) - 1);
        for (char *p = safe; *p; p++)
            if (*p == '/' || *p == ' ' || *p == '\'' || *p == '"') *p = '_';
        snprintf(sa->tmp_dir, sizeof(sa->tmp_dir),
                 "%s/ps-ftp-arch/%ld-%s", base_cache_dir, (long)time(NULL), safe);

        pthread_create(&dl->thread, NULL, split_archive_thread_fn, sa);
    } else {
        pthread_create(&dl->thread, NULL, download_thread_fn, dl);
    }
    pthread_detach(dl->thread);

    g_entry_url = g_entry_name = g_entry_pass_single = g_lbl_detected = NULL;
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void populate_password_combo(GtkWidget *combo) {
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    char *copy = strdup(g_saved_passwords);
    if (!copy) return;
    char *token = strtok(copy, ",");
    while (token) {
        while (*token == ' ') token++;
        if (*token) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), token);
        }
        token = strtok(NULL, ",");
    }
    free(copy);
G_GNUC_END_IGNORE_DEPRECATIONS
}

static void show_add_dialog(GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "New Transfer", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Start",  GTK_RESPONSE_OK, NULL);

    gtk_widget_set_size_request(dialog, 580, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_orientable_set_orientation(GTK_ORIENTABLE(content), GTK_ORIENTATION_VERTICAL);

    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (form, 24);
    gtk_widget_set_margin_end   (form, 24);
    gtk_widget_set_margin_top   (form, 20);
    gtk_widget_set_margin_bottom(form, 20);

    /* URL field */
    GtkWidget *lbl_u = gtk_label_new("File URL");
    gtk_label_set_xalign(GTK_LABEL(lbl_u), 0.0);
    gtk_widget_add_css_class(lbl_u, "form-label");

    g_entry_url = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_entry_url),
                                    "https://example.com/file.pkg");
    gtk_widget_add_css_class(g_entry_url, "form-entry");
    g_signal_connect(g_entry_url, "changed", G_CALLBACK(on_url_changed), NULL);

    g_lbl_detected = gtk_label_new("Enter a URL for automatic detection");
    gtk_label_set_xalign(GTK_LABEL(g_lbl_detected), 0.0);
    gtk_widget_add_css_class(g_lbl_detected, "detect-label");

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(sep, "form-sep");

    /* Filename field */
    GtkWidget *lbl_n = gtk_label_new("Destination filename");
    gtk_label_set_xalign(GTK_LABEL(lbl_n), 0.0);
    gtk_widget_add_css_class(lbl_n, "form-label");

    GtkWidget *hint = gtk_label_new(
        "Leave blank to use the filename from the URL. "
        "The extension is added automatically.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_add_css_class(hint, "form-hint");

    g_entry_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_entry_name), "e.g. my_game");
    gtk_widget_add_css_class(g_entry_name, "form-entry");

    /* Password field (for compressed files) */
    GtkWidget *lbl_p = gtk_label_new("Password (only if compressed .rar/.zip)");
    gtk_label_set_xalign(GTK_LABEL(lbl_p), 0.0);
    gtk_widget_add_css_class(lbl_p, "form-label");

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_entry_pass_single = gtk_combo_box_text_new_with_entry();
    populate_password_combo(g_entry_pass_single);
    GtkWidget *child_entry = gtk_combo_box_get_child(GTK_COMBO_BOX(g_entry_pass_single));
    if (child_entry && GTK_IS_ENTRY(child_entry)) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(child_entry), "Leave blank if no password");
    }
G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_widget_add_css_class(g_entry_pass_single, "form-entry");

    /* FTP destination info */
    char ftp_info[512];
    snprintf(ftp_info, sizeof(ftp_info), "Destination → ftp://%s:%d/%s/",
             g_ftp_host, g_ftp_port, g_ftp_dir);
    GtkWidget *lbl_ftp = gtk_label_new(ftp_info);
    gtk_label_set_xalign(GTK_LABEL(lbl_ftp), 0.0);
    gtk_widget_add_css_class(lbl_ftp, "detect-label");

    gtk_box_append(GTK_BOX(form), lbl_u);
    gtk_box_append(GTK_BOX(form), g_entry_url);
    gtk_box_append(GTK_BOX(form), g_lbl_detected);
    gtk_box_append(GTK_BOX(form), sep);
    gtk_box_append(GTK_BOX(form), lbl_n);
    gtk_box_append(GTK_BOX(form), hint);
    gtk_box_append(GTK_BOX(form), g_entry_name);
    gtk_box_append(GTK_BOX(form), lbl_p);
    gtk_box_append(GTK_BOX(form), g_entry_pass_single);
    gtk_box_append(GTK_BOX(form), lbl_ftp);
    gtk_box_append(GTK_BOX(content), form);

    GtkWidget *ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                        GTK_RESPONSE_OK);
    gtk_widget_add_css_class(ok, "btn-primary");

    g_signal_connect(dialog, "response", G_CALLBACK(on_add_dialog_response), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(g_entry_url);
}

/* ═══════════════════════════════════════════════════════════
 * Settings dialog
 * ═══════════════════════════════════════════════════════════ */

static void on_settings_response(GtkDialog *dialog, int response,
                                   gpointer user_data) {
    (void)user_data;
    if (response == GTK_RESPONSE_OK) {
        const char *host = gtk_editable_get_text(GTK_EDITABLE(g_set_host));
        const char *port = gtk_editable_get_text(GTK_EDITABLE(g_set_port));
        const char *dir  = gtk_editable_get_text(GTK_EDITABLE(g_set_dir));
        const char *user = gtk_editable_get_text(GTK_EDITABLE(g_set_user));
        const char *pass = gtk_editable_get_text(GTK_EDITABLE(g_set_pass));
        const char *tmp  = gtk_editable_get_text(GTK_EDITABLE(g_set_tmp));
        const char *saved_pass = gtk_editable_get_text(GTK_EDITABLE(g_set_saved_pass));

        if (host && host[0]) strncpy(g_ftp_host, host, sizeof(g_ftp_host) - 1);
        if (port && atoi(port) > 0) g_ftp_port = atoi(port);
        if (dir && dir[0]) {
            const char *d = dir;
            while (*d == '/') d++;
            char buf[sizeof(g_ftp_dir)];
            strncpy(buf, d, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            size_t blen = strlen(buf);
            while (blen > 0 && buf[blen - 1] == '/') buf[--blen] = '\0';
            if (buf[0])
                strncpy(g_ftp_dir, buf, sizeof(g_ftp_dir) - 1);
            else
                strncpy(g_ftp_dir, FTP_DIR_DEFAULT, sizeof(g_ftp_dir) - 1);
        }

        strncpy(g_ftp_user,
                (user && user[0]) ? user : FTP_USER_DEFAULT,
                sizeof(g_ftp_user) - 1);
        strncpy(g_ftp_pass,
                (pass && pass[0]) ? pass : FTP_PASS_DEFAULT,
                sizeof(g_ftp_pass) - 1);
        if (tmp && tmp[0]) strncpy(g_tmp_dir, tmp, sizeof(g_tmp_dir) - 1);
        if (saved_pass) strncpy(g_saved_passwords, saved_pass, sizeof(g_saved_passwords) - 1);

        save_settings();

        if (g_lbl_sub && GTK_IS_WIDGET(g_lbl_sub)) {
            char sub[256];
            snprintf(sub, sizeof(sub), "ftp://%s:%d/%s", g_ftp_host, g_ftp_port, g_ftp_dir);
            gtk_label_set_text(GTK_LABEL(g_lbl_sub), sub);
        }

        trigger_ftp_ensure_dir_async();
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void show_settings_dialog(GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "FTP Settings", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Save",   GTK_RESPONSE_OK, NULL);

    gtk_widget_set_size_request(dialog, 440, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_orientable_set_orientation(GTK_ORIENTABLE(content), GTK_ORIENTATION_VERTICAL);

    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start (form, 24);
    gtk_widget_set_margin_end   (form, 24);
    gtk_widget_set_margin_top   (form, 20);
    gtk_widget_set_margin_bottom(form, 20);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", g_ftp_port);

    /* Field descriptors — iterated to build the form programmatically. */
    struct { const char *label; GtkWidget **entry; const char *val; gboolean secret; }
    fields[] = {
        { "FTP Host",  &g_set_host, g_ftp_host, FALSE },
        { "FTP Port",  &g_set_port, port_str,   FALSE },
        { "Directory", &g_set_dir,  g_ftp_dir,  FALSE },
        { "Username",  &g_set_user, g_ftp_user, FALSE },
        { "Password",  &g_set_pass, g_ftp_pass, TRUE  },
        { "Temp Dir (Arch)", &g_set_tmp, g_tmp_dir[0] ? g_tmp_dir : g_get_user_cache_dir(), FALSE },
        { "Saved Passwords", &g_set_saved_pass, g_saved_passwords, FALSE },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(fields); i++) {
        GtkWidget *lbl = gtk_label_new(fields[i].label);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_widget_add_css_class(lbl, "form-label");

        *fields[i].entry = gtk_entry_new();
        gtk_widget_add_css_class(*fields[i].entry, "form-entry");
        if (fields[i].val)
            gtk_editable_set_text(GTK_EDITABLE(*fields[i].entry), fields[i].val);
        if (fields[i].secret)
            gtk_entry_set_visibility(GTK_ENTRY(*fields[i].entry), FALSE);

        gtk_box_append(GTK_BOX(form), lbl);
        gtk_box_append(GTK_BOX(form), *fields[i].entry);
    }

    /* Test Connection button and status feedback */
    GtkWidget *btn_test = gtk_button_new_with_label("Test Connessione & Crea Cartella");
    gtk_widget_add_css_class(btn_test, "btn-settings");
    gtk_widget_set_margin_top(btn_test, 4);

    GtkWidget *lbl_test_result = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(lbl_test_result), 0.0);
    g_signal_connect(btn_test, "clicked", G_CALLBACK(on_test_clicked), lbl_test_result);

    gtk_box_append(GTK_BOX(form), btn_test);
    gtk_box_append(GTK_BOX(form), lbl_test_result);

    gtk_box_append(GTK_BOX(content), form);

    GtkWidget *ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                        GTK_RESPONSE_OK);
    gtk_widget_add_css_class(ok, "btn-primary");

    g_signal_connect(dialog, "response", G_CALLBACK(on_settings_response), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
}

/* ═══════════════════════════════════════════════════════════
 * Split-archive dialog
 * ═══════════════════════════════════════════════════════════ */

static void on_arch_dialog_response(GtkDialog *dialog, int response,
                                     gpointer user_data)
{
    (void)user_data;

    if (response != GTK_RESPONSE_OK) {
        g_arch_urls_box = g_arch_entry_name = g_arch_entry_pass = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    /* ── Collect URLs from the individual entry widgets ── */
    const char *pkg_name_input = gtk_editable_get_text(GTK_EDITABLE(g_arch_entry_name));
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    char *password = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_arch_entry_pass));
G_GNUC_END_IGNORE_DEPRECATIONS

    /* Build destination .pkg name. */
    char pkg_name[SA_NAME_LEN] = "";
    if (pkg_name_input && pkg_name_input[0]) {
        const char *dot = strrchr(pkg_name_input, '.');
        if (dot && strcasecmp(dot, ".pkg") == 0)
            snprintf(pkg_name, sizeof(pkg_name), "%s", pkg_name_input);
        else
            snprintf(pkg_name, sizeof(pkg_name), "%s.pkg", pkg_name_input);
    } else {
        snprintf(pkg_name, sizeof(pkg_name), "archive.pkg");
    }

    SplitArchiveArgs *sa = calloc(1, sizeof(SplitArchiveArgs));
    strncpy(sa->pkg_name, pkg_name, sizeof(sa->pkg_name) - 1);
    if (password) {
        strncpy(sa->password, password, sizeof(sa->password) - 1);
        g_free(password);
    }

    /* Iterate over children of g_arch_urls_box.
     * Each child is an HBox: [label "N."] [entry].
     * We walk the HBox siblings to find the GtkEntry. */
    if (g_arch_urls_box) {
        GtkWidget *row = gtk_widget_get_first_child(g_arch_urls_box);
        while (row && sa->n_urls < SA_MAX_URLS) {
            /* Skip the label (first child), grab the entry (second child). */
            GtkWidget *entry = gtk_widget_get_last_child(row);
            if (entry && GTK_IS_ENTRY(entry)) {
                const char *raw_url = gtk_editable_get_text(GTK_EDITABLE(entry));
                if (raw_url) {
                    char url[SA_URL_LEN];
                    strncpy(url, raw_url, sizeof(url) - 1);
                    url[sizeof(url) - 1] = '\0';
                    g_strstrip(url);
                    if (strlen(url) >= (size_t)URL_MIN_LEN) {
                        strncpy(sa->urls[sa->n_urls++], url, SA_URL_LEN - 1);
                    }
                }
            }
            row = gtk_widget_get_next_sibling(row);
        }
    }

    if (sa->n_urls == 0) {
        free(sa);
        g_arch_urls_box = g_arch_entry_name = g_arch_entry_pass = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    const char *base_cache_dir = g_tmp_dir[0] ? g_tmp_dir : g_get_user_cache_dir();

    /* Create unique tmp dir path: base_cache_dir/ps-ftp-arch/<timestamp>-<pkg_name> */
    {
        char safe[64] = "";
        strncpy(safe, pkg_name, sizeof(safe) - 1);
        /* Replace unsafe characters with underscores. */
        for (char *p = safe; *p; p++)
            if (*p == '/' || *p == ' ' || *p == '\'' || *p == '"') *p = '_';
        snprintf(sa->tmp_dir, sizeof(sa->tmp_dir),
                 "%s/ps-ftp-arch/%ld-%s", base_cache_dir, (long)time(NULL), safe);
    }

    /* Register a new Download entry. */
    pthread_mutex_lock(&dl_mutex);
    if (n_downloads >= MAX_DOWNLOADS) {
        pthread_mutex_unlock(&dl_mutex);
        free(sa);
        g_arch_urls_box = g_arch_entry_name = g_arch_entry_pass = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    Download *dl = calloc(1, sizeof(Download));
    dl->id         = next_id++;
    dl->status     = DL_DOWNLOADING;
    dl->total_size = -1;
    dl->progress   = -1.0;
    dl->eta        = -1.0;

    /* Use the pkg name as the display name; URL field shows part count. */
    strncpy(dl->dest_name, pkg_name, sizeof(dl->dest_name) - 1);
    snprintf(dl->url, sizeof(dl->url), "[Split archive — %d part%s]",
             sa->n_urls, sa->n_urls == 1 ? "" : "s");

    sa->dl_id = dl->id;
    dl->sa_args = sa;
    downloads[n_downloads++] = dl;
    pthread_mutex_unlock(&dl_mutex);

    GtkWidget *row = create_download_row(dl);
    gtk_list_box_append(GTK_LIST_BOX(g_list_box), row);
    update_stats();

    trigger_ftp_ensure_dir_async();

    pthread_create(&dl->thread, NULL, split_archive_thread_fn, sa);
    pthread_detach(dl->thread);

    g_arch_urls_box = g_arch_entry_name = g_arch_entry_pass = NULL;
    gtk_window_destroy(GTK_WINDOW(dialog));
}

/* ── Build a numbered URL row: ["N."] + [GtkEntry] ── */
static GtkWidget *make_url_row(int n) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(row, TRUE);

    char num_str[8];
    snprintf(num_str, sizeof(num_str), "%d.", n);
    GtkWidget *lbl = gtk_label_new(num_str);
    gtk_widget_add_css_class(lbl, "url-slot-num");
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(lbl, 26, -1);  /* fixed width for alignment */
    gtk_label_set_xalign(GTK_LABEL(lbl), 1.0); /* right-align the number   */

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "https://...");
    gtk_widget_add_css_class(entry, "form-entry");
    gtk_widget_set_hexpand(entry, TRUE);

    gtk_box_append(GTK_BOX(row), lbl);
    gtk_box_append(GTK_BOX(row), entry);
    return row;
}

static void on_add_url_slot_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (!g_arch_urls_box) return;

    /* Count existing rows to determine the next number. */
    int n = 1;
    GtkWidget *c = gtk_widget_get_first_child(g_arch_urls_box);
    while (c) { n++; c = gtk_widget_get_next_sibling(c); }

    GtkWidget *row = make_url_row(n);
    gtk_box_append(GTK_BOX(g_arch_urls_box), row);

    /* Focus the new entry immediately. */
    GtkWidget *entry = gtk_widget_get_last_child(row);
    if (entry) gtk_widget_grab_focus(entry);
}

static void show_arch_dialog(GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Split Archive Transfer", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Start",  GTK_RESPONSE_OK, NULL);

    gtk_widget_set_size_request(dialog, 580, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_orientable_set_orientation(GTK_ORIENTABLE(content), GTK_ORIENTATION_VERTICAL);

    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (form, 24);
    gtk_widget_set_margin_end   (form, 24);
    gtk_widget_set_margin_top   (form, 20);
    gtk_widget_set_margin_bottom(form, 20);

    /* ── Section label: Part URLs ── */
    GtkWidget *lbl_urls = gtk_label_new("Part download URLs");
    gtk_label_set_xalign(GTK_LABEL(lbl_urls), 0.0);
    gtk_widget_add_css_class(lbl_urls, "form-label");

    GtkWidget *lbl_url_hint = gtk_label_new(
        "Add one entry per part, in order. Click ‘+’ to add more slots.");
    gtk_label_set_xalign(GTK_LABEL(lbl_url_hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(lbl_url_hint), TRUE);
    gtk_widget_add_css_class(lbl_url_hint, "form-hint");

    /* ── Dynamic URL entry slots ── */
    g_arch_urls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    /* Start with 2 numbered slots */
    for (int i = 1; i <= 2; i++)
        gtk_box_append(GTK_BOX(g_arch_urls_box), make_url_row(i));

    /* ── “+” button to add another slot ── */
    GtkWidget *btn_add_slot = gtk_button_new_with_label("+  Add URL");
    gtk_widget_add_css_class(btn_add_slot, "btn-add-slot");
    gtk_widget_set_halign(btn_add_slot, GTK_ALIGN_START);
    g_signal_connect(btn_add_slot, "clicked",
                     G_CALLBACK(on_add_url_slot_clicked), NULL);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(sep, "form-sep");

    /* ── Destination .pkg name ── */
    GtkWidget *lbl_name = gtk_label_new("Destination .pkg filename");
    gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
    gtk_widget_add_css_class(lbl_name, "form-label");

    GtkWidget *lbl_name_hint = gtk_label_new(
        "The .pkg extension is added automatically if omitted.");
    gtk_label_set_xalign(GTK_LABEL(lbl_name_hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(lbl_name_hint), TRUE);
    gtk_widget_add_css_class(lbl_name_hint, "form-hint");

    g_arch_entry_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_arch_entry_name), "e.g. my_game");
    gtk_widget_add_css_class(g_arch_entry_name, "form-entry");

    /* ── Password field ── */
    GtkWidget *lbl_pass = gtk_label_new("Archive password (leave blank if none)");
    gtk_label_set_xalign(GTK_LABEL(lbl_pass), 0.0);
    gtk_widget_add_css_class(lbl_pass, "form-label");

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_arch_entry_pass = gtk_combo_box_text_new_with_entry();
    populate_password_combo(g_arch_entry_pass);
    GtkWidget *arch_child = gtk_combo_box_get_child(GTK_COMBO_BOX(g_arch_entry_pass));
    if (arch_child && GTK_IS_ENTRY(arch_child)) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(arch_child), "(optional)");
    }
G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_widget_add_css_class(g_arch_entry_pass, "form-entry");

    /* ── FTP destination info ── */
    char ftp_info[512];
    snprintf(ftp_info, sizeof(ftp_info), "FTP destination → ftp://%s:%d/%s/",
             g_ftp_host, g_ftp_port, g_ftp_dir);
    GtkWidget *lbl_ftp = gtk_label_new(ftp_info);
    gtk_label_set_xalign(GTK_LABEL(lbl_ftp), 0.0);
    gtk_widget_add_css_class(lbl_ftp, "detect-label");

    /* ── Assemble form ── */
    gtk_box_append(GTK_BOX(form), lbl_urls);
    gtk_box_append(GTK_BOX(form), lbl_url_hint);
    gtk_box_append(GTK_BOX(form), g_arch_urls_box);
    gtk_box_append(GTK_BOX(form), btn_add_slot);
    gtk_box_append(GTK_BOX(form), sep);
    gtk_box_append(GTK_BOX(form), lbl_name);
    gtk_box_append(GTK_BOX(form), lbl_name_hint);
    gtk_box_append(GTK_BOX(form), g_arch_entry_name);
    gtk_box_append(GTK_BOX(form), lbl_pass);
    gtk_box_append(GTK_BOX(form), g_arch_entry_pass);
    gtk_box_append(GTK_BOX(form), lbl_ftp);
    gtk_box_append(GTK_BOX(content), form);

    GtkWidget *ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                        GTK_RESPONSE_OK);
    gtk_widget_add_css_class(ok, "btn-primary");

    g_signal_connect(dialog, "response", G_CALLBACK(on_arch_dialog_response), NULL);
    gtk_window_present(GTK_WINDOW(dialog));

    /* Focus first URL entry */
    GtkWidget *first_row = gtk_widget_get_first_child(g_arch_urls_box);
    if (first_row) {
        GtkWidget *first_entry = gtk_widget_get_last_child(first_row);
        if (first_entry) gtk_widget_grab_focus(first_entry);
    }
}

/* ═══════════════════════════════════════════════════════════
 * Header action callbacks
 * ═══════════════════════════════════════════════════════════ */

static void on_add_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    show_add_dialog(GTK_WINDOW(user_data));
}

static void on_arch_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    show_arch_dialog(GTK_WINDOW(user_data));
}

static void on_settings_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    show_settings_dialog(GTK_WINDOW(user_data));
}

static void on_github_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    g_app_info_launch_default_for_uri(APP_GITHUB_URL, NULL, NULL);
}

/* ═══════════════════════════════════════════════════════════
 * Application activate — builds the main window
 * ═══════════════════════════════════════════════════════════ */

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    load_css();
    load_settings();
    trigger_ftp_ensure_dir_async();

    g_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_window), "PS FTP Downloader");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 860, 640);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* ── Header bar ── */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(header, "main-header");
    gtk_widget_set_hexpand(header, TRUE);

    GtkWidget *title_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top   (title_col, 10);
    gtk_widget_set_margin_bottom(title_col, 10);

    /* GitHub icon button — resolves path at runtime (dev vs installed). */
    static const char *GITHUB_ICON_PATHS[] = {
        "github-mark.svg",
        "/usr/share/ps-ftp-downloader/github-mark.svg",
        NULL
    };
    GtkWidget *github_img = NULL;
    for (int i = 0; GITHUB_ICON_PATHS[i]; i++) {
        if (g_file_test(GITHUB_ICON_PATHS[i], G_FILE_TEST_EXISTS)) {
            github_img = gtk_image_new_from_file(GITHUB_ICON_PATHS[i]);
            break;
        }
    }
    if (!github_img)
        github_img = gtk_image_new_from_icon_name("help-about-symbolic");

    /* Make the GitHub icon larger */
    gtk_widget_set_size_request(github_img, 32, 32);

    GtkWidget *btn_github = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(btn_github), github_img);
    gtk_widget_add_css_class(btn_github, "flat");
    gtk_widget_add_css_class(btn_github, "github-btn");
    gtk_widget_set_valign(btn_github, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(btn_github, "View on GitHub");
    g_signal_connect(btn_github, "clicked", G_CALLBACK(on_github_clicked), NULL);

    GtkWidget *lbl_title = gtk_label_new("PS FTP Downloader");
    gtk_widget_add_css_class(lbl_title, "app-title");
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);

    /* Group title and GitHub icon together horizontally */
    GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(title_row), lbl_title);
    gtk_box_append(GTK_BOX(title_row), btn_github);

    char sub[256];
    snprintf(sub, sizeof(sub), "ftp://%s:%d/%s", g_ftp_host, g_ftp_port, g_ftp_dir);
    g_lbl_sub = gtk_label_new(sub);
    gtk_widget_add_css_class(g_lbl_sub, "app-subtitle");
    gtk_label_set_xalign(GTK_LABEL(g_lbl_sub), 0.0);

    gtk_box_append(GTK_BOX(title_col), title_row);
    gtk_box_append(GTK_BOX(title_col), g_lbl_sub);

    /* Spacer pushes the action buttons to the right edge. */
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);

    GtkWidget *btn_settings = gtk_button_new_with_label("Settings");
    gtk_widget_add_css_class(btn_settings, "btn-settings");
    gtk_widget_set_valign(btn_settings, GTK_ALIGN_CENTER);
    g_signal_connect(btn_settings, "clicked",
                     G_CALLBACK(on_settings_clicked), g_window);

    GtkWidget *btn_arch = gtk_button_new_with_label("Split Archive");
    gtk_widget_add_css_class(btn_arch, "btn-add");
    gtk_widget_set_valign(btn_arch, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(btn_arch,
        "Download a multi-part archive, extract locally with 7zip, and upload the .pkg via FTP");
    g_signal_connect(btn_arch, "clicked", G_CALLBACK(on_arch_clicked), g_window);

    GtkWidget *btn_add = gtk_button_new_with_label("New Download");
    gtk_widget_add_css_class(btn_add, "btn-add");
    gtk_widget_set_valign(btn_add, GTK_ALIGN_CENTER);
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_add_clicked), g_window);

    gtk_box_append(GTK_BOX(header), title_col);
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), btn_settings);
    gtk_box_append(GTK_BOX(header), btn_arch);
    gtk_box_append(GTK_BOX(header), btn_add);

    /* ── Content stack: empty placeholder ↔ download list ── */
    g_stack = gtk_stack_new();
    gtk_widget_set_vexpand(g_stack, TRUE);

    /* Empty state */
    GtkWidget *empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(empty_box, "empty-box");
    gtk_widget_set_halign(empty_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);

    GtkWidget *e_icon  = gtk_label_new("📡");
    GtkWidget *e_title = gtk_label_new("No active transfers");
    GtkWidget *e_sub   = gtk_label_new(
        "Click 'New Download' to start a direct\nnetwork-to-PS stream");
    gtk_widget_add_css_class(e_icon,  "empty-icon");
    gtk_widget_add_css_class(e_title, "empty-title");
    gtk_widget_add_css_class(e_sub,   "empty-sub");
    gtk_label_set_justify(GTK_LABEL(e_sub), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(empty_box), e_icon);
    gtk_box_append(GTK_BOX(empty_box), e_title);
    gtk_box_append(GTK_BOX(empty_box), e_sub);

    /* Scrollable download list */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    g_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_list_box), GTK_SELECTION_NONE);
    gtk_list_box_set_show_separators(GTK_LIST_BOX(g_list_box), FALSE);
    gtk_widget_set_hexpand(g_list_box, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), g_list_box);

    gtk_stack_add_named(GTK_STACK(g_stack), empty_box, "empty");
    gtk_stack_add_named(GTK_STACK(g_stack), scroll,    "list");
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "empty");

    /* ── Status bar ── */
    GtkWidget *statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(statusbar, "statusbar");
    g_lbl_stats = gtk_label_new("No transfers");
    gtk_label_set_xalign(GTK_LABEL(g_lbl_stats), 0.0);
    gtk_widget_set_hexpand(g_lbl_stats, TRUE);
    gtk_box_append(GTK_BOX(statusbar), g_lbl_stats);

    /* ── Assemble root ── */
    gtk_box_append(GTK_BOX(root), header);
    gtk_box_append(GTK_BOX(root), g_stack);
    gtk_box_append(GTK_BOX(root), statusbar);

    gtk_window_set_child(GTK_WINDOW(g_window), root);
    gtk_window_present(GTK_WINDOW(g_window));
}

/* ═══════════════════════════════════════════════════════════
 * Entry point
 * ═══════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_ALL);

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    curl_global_cleanup();
    return status;
}
