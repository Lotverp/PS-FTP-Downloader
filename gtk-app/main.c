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
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
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
#define APP_VERSION      "1.3.4"
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
static char g_saved_passwords[1024] = "[DLPSGAME.COM],DLPSGAME.COM,dlpsgame.com,SuperPSX,superpsx";
static char g_realdebrid_token[256] = "";
static gboolean g_uc_enabled = TRUE;
static gboolean g_uc_headless = TRUE;
static char g_uc_last_cookie[4096] = "";
static char g_uc_last_user_agent[512] = "";
static char g_uc_last_referer[2048] = "";

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
static GtkWidget *g_set_host, *g_set_port, *g_set_dir, *g_set_user, *g_set_pass, *g_set_tmp, *g_set_saved_pass, *g_set_rd_token;
static GtkWidget *g_chk_uc_enabled, *g_chk_uc_headless;

/* Split-archive dialog refs. */
static GtkWidget *g_arch_urls_box;   /* VBox containing one GtkEntry per URL  */
static GtkWidget *g_arch_entry_name; /* destination .pkg filename             */
static GtkWidget *g_arch_entry_pass; /* archive password (may be empty)       */

/* Catalog state */
static GListStore *g_catalog_store = NULL;
static char g_catalog_sources[8][1024];
static int  g_n_sources = 0;
static GtkWidget *g_catalog_search_entry = NULL;
static GtkWidget *g_dlps_search_entry = NULL;
static GtkWidget *g_catalog_cat_dropdown = NULL;
static GtkWidget *g_catalog_status_lbl = NULL;
static GtkCustomFilter *g_catalog_cat_filter = NULL;
static char g_selected_category[64] = "Tutte";
static GtkWidget *g_toast_box = NULL;
static GtkWidget *g_toast_label = NULL;
static GtkWidget *g_toast_progress = NULL;
static guint g_toast_pulse_id = 0;
static guint g_toast_timeout_id = 0;
static void show_toast(const char *msg);
static void show_prep_progress(const char *msg);
static void hide_prep_progress(void);
static void trigger_catalog_fetch_async(void);
typedef struct AutoSession AutoSession;
static gboolean is_uc_resolver_available(void);
static void trigger_uc_resolve(const char *url, const char *game_title, const char *password, GtkWidget *dialog, AutoSession *sess);
static void trigger_dlps_fetch(const char *query, int page);

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
                 errors ? "  (Errori)" : "");

    gtk_label_set_text(GTK_LABEL(g_lbl_stats), buf);
    const char *cur = g_stack ? gtk_stack_get_visible_child_name(GTK_STACK(g_stack)) : NULL;
    if (!cur || (strcmp(cur, "catalog") != 0 && strcmp(cur, "dlpsgame") != 0)) {
        if (g_stack) {
            gtk_stack_set_visible_child_name(GTK_STACK(g_stack),
                                             total == 0 ? "empty" : "list");
        }
    }
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
            gtk_label_set_text(GTK_LABEL(dl->lbl_status), "ERROR");
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
    if (g_uc_last_user_agent[0]) {
        CURL_SET(c, CURLOPT_USERAGENT, g_uc_last_user_agent);
    } else {
        CURL_SET(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    }
    CURL_SET(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURL_SET(c, CURLOPT_MAXREDIRS, 15L);
    CURL_SET(c, CURLOPT_COOKIEFILE, "");        /* abilita il cookie engine */
    if (g_uc_last_cookie[0]) {
        CURL_SET(c, CURLOPT_COOKIE, g_uc_last_cookie);
    }
    CURL_SET(c, CURLOPT_ACCEPT_ENCODING, "");
    CURL_SET(c, CURLOPT_AUTOREFERER, 1L);
    GUri *u = g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED, NULL);
    const char *host = u ? g_uri_get_host(u) : NULL;
    if (g_uc_last_referer[0]) {
        GUri *ref_u = g_uri_parse(g_uc_last_referer, G_URI_FLAGS_PARSE_RELAXED, NULL);
        const char *ref_host = ref_u ? g_uri_get_host(ref_u) : NULL;
        if (ref_host && host && (g_ascii_strcasecmp(ref_host, host) == 0 ||
            strstr(host, "fishoilenjoyer.xyz") || strstr(host, "akirabox") ||
            strstr(host, "fileditch") || strstr(host, "vikingfile") ||
            strstr(host, "vik1ngfile"))) {
            CURL_SET(c, CURLOPT_REFERER, g_uc_last_referer);
        } else if (u && host) {
            char *ref = g_strdup_printf("%s://%s/", g_uri_get_scheme(u), host);
            CURL_SET(c, CURLOPT_REFERER, ref);
            g_free(ref);
        }
        if (ref_u) g_uri_unref(ref_u);
    } else if (u && host) {
        char *ref = g_strdup_printf("%s://%s/", g_uri_get_scheme(u), host);
        CURL_SET(c, CURLOPT_REFERER, ref);
        g_free(ref);
    }
    if (u) g_uri_unref(u);
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
        char *rd = g_key_file_get_string(kf, "General", "realdebrid_token", NULL);
        if (rd && rd[0]) {
            strncpy(g_realdebrid_token, rd, sizeof(g_realdebrid_token) - 1);
            g_free(rd);
        }
        if (g_key_file_has_key(kf, "General", "uc_enabled", NULL)) {
            g_uc_enabled = g_key_file_get_boolean(kf, "General", "uc_enabled", NULL);
        } else {
            g_uc_enabled = TRUE;
        }
        if (g_key_file_has_key(kf, "General", "uc_headless", NULL)) {
            g_uc_headless = g_key_file_get_boolean(kf, "General", "uc_headless", NULL);
        } else {
            g_uc_headless = TRUE;
        }

        gsize n_src = 0;
        char **src_list = g_key_file_get_string_list(kf, "Catalog", "sources", &n_src, NULL);
        if (src_list) {
            g_n_sources = 0;
            for (gsize i = 0; i < n_src && g_n_sources < 8; i++) {
                if (src_list[i] && src_list[i][0]) {
                    strncpy(g_catalog_sources[g_n_sources++], src_list[i], sizeof(g_catalog_sources[0]) - 1);
                }
            }
            g_strfreev(src_list);
        }
        if (g_n_sources == 0) {
            strncpy(g_catalog_sources[0], "https://raw.githubusercontent.com/Lotverp/PS-FTP-Downloader/main/catalog.json", sizeof(g_catalog_sources[0]) - 1);
            g_n_sources = 1;
        }
    } else {
        if (g_n_sources == 0) {
            strncpy(g_catalog_sources[0], "https://raw.githubusercontent.com/Lotverp/PS-FTP-Downloader/main/catalog.json", sizeof(g_catalog_sources[0]) - 1);
            g_n_sources = 1;
        }
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
    g_key_file_set_string(kf, "General", "realdebrid_token", g_realdebrid_token);
    g_key_file_set_boolean(kf, "General", "uc_enabled", g_uc_enabled);
    g_key_file_set_boolean(kf, "General", "uc_headless", g_uc_headless);

    if (g_n_sources > 0) {
        const char *src_ptrs[8];
        for (int i = 0; i < g_n_sources; i++) {
            src_ptrs[i] = g_catalog_sources[i];
        }
        g_key_file_set_string_list(kf, "Catalog", "sources", src_ptrs, g_n_sources);
    } else {
        g_key_file_remove_key(kf, "Catalog", "sources", NULL);
    }

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
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(provider);
}

/* ═══════════════════════════════════════════════════════════
 * Error popup (main thread) — shown when the user clicks ERROR
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
    gtk_widget_set_focusable(btn, FALSE);
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
    char *p = NULL;
    char *argv[10]; int n = 0;
    
    if (strcmp(bin, "unar") == 0) {
        argv[n++] = (char *)bin;
        argv[n++] = "-f";  /* force overwrite */
        argv[n++] = "-D";  /* do not create extra containing directory */
        argv[n++] = "-o";
        argv[n++] = (char *)dir;
        if (pass && pass[0]) {
            argv[n++] = "-p";
            argv[n++] = (char *)pass;
        }
        argv[n++] = (char *)archive;
    } else if (strcmp(bin, "unrar") == 0) {
        argv[n++] = (char *)bin;
        argv[n++] = "x";
        argv[n++] = "-y";
        if (pass && pass[0]) {
            p = g_strdup_printf("-p%s", pass);
            argv[n++] = p;
        } else {
            argv[n++] = "-p-";
        }
        argv[n++] = (char *)archive;
        o = g_strdup_printf("%s/", dir);
        argv[n++] = o;
    } else {
        /* 7z, 7zz, 7za */
        argv[n++] = (char *)bin;
        argv[n++] = "x";
        argv[n++] = "-y";
        o = g_strdup_printf("-o%s", dir);
        argv[n++] = o;
        if (pass && pass[0]) {
            p = g_strdup_printf("-p%s", pass);
            argv[n++] = p;
        } else {
            argv[n++] = "-p-";  /* disable password prompt on stdin */
        }
        argv[n++] = "--";
        argv[n++] = (char *)archive;
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
    snprintf(base_dir, sizeof(base_dir), "%.480s/ps-ftp-arch", base_cache_dir);
    
    if (mkdir(base_dir, 0755) != 0 && errno != EEXIST) {
        snprintf(dl->error_msg, sizeof(dl->error_msg),
                 "Cannot create base temp dir %.100s: %.100s", base_dir, strerror(errno));
        dl->status = DL_ERROR;
        post_update(dl);
        return NULL;
    }
    if (mkdir(sa->tmp_dir, 0755) != 0 && errno != EEXIST) {
        snprintf(dl->error_msg, sizeof(dl->error_msg),
                 "Cannot create job dir %.100s: %.100s", sa->tmp_dir, strerror(errno));
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
        gboolean is_rar = (part_paths[0] && strcasestr(part_paths[0], ".rar") != NULL);
        const char *rar_bins[] = { "7z", "7zz", "unar", "unrar", NULL };
        const char *gen_bins[] = { "7z", "7zz", "unar", "7za", "unrar", NULL };
        const char **bins = is_rar ? rar_bins : gen_bins;

        /* Build list of candidate passwords to try:
         * 1. sa->password (if set)
         * 2. empty string "" (unencrypted archive)
         * 3. [DLPSGAME.COM], DLPSGAME.COM, dlpsgame.com, SuperPSX
         * 4. user saved passwords from settings
         */
        GPtrArray *cand_pass = g_ptr_array_new_with_free_func(g_free);
        if (sa->password[0]) g_ptr_array_add(cand_pass, g_strdup(sa->password));
        g_ptr_array_add(cand_pass, g_strdup(""));

        const char *def_pass[] = { "[DLPSGAME.COM]", "DLPSGAME.COM", "dlpsgame.com", "SuperPSX", "superpsx", NULL };
        for (int d = 0; def_pass[d]; d++) {
            gboolean found = FALSE;
            for (guint c = 0; c < cand_pass->len; c++) {
                if (strcmp((char *)cand_pass->pdata[c], def_pass[d]) == 0) { found = TRUE; break; }
            }
            if (!found) g_ptr_array_add(cand_pass, g_strdup(def_pass[d]));
        }

        if (g_saved_passwords[0]) {
            char *sp = g_strdup(g_saved_passwords);
            char *tok = strtok(sp, ",;");
            while (tok) {
                char *clean = g_strstrip(g_strdup(tok));
                if (clean[0]) {
                    gboolean found = FALSE;
                    for (guint c = 0; c < cand_pass->len; c++) {
                        if (strcmp((char *)cand_pass->pdata[c], clean) == 0) { found = TRUE; break; }
                    }
                    if (!found) g_ptr_array_add(cand_pass, clean);
                    else g_free(clean);
                } else g_free(clean);
                tok = strtok(NULL, ",;");
            }
            g_free(sp);
        }

        int ret = -1;
        int last_meaningful_ret = -1;
        char best_err[512] = "";

        for (guint p_idx = 0; p_idx < cand_pass->len; p_idx++) {
            const char *curr_pass = (const char *)cand_pass->pdata[p_idx];
            for (int b = 0; bins[b]; b++) {
                char tmp_err[512] = "";
                ret = run_extract(bins[b], sa->tmp_dir, curr_pass, part_paths[0],
                                  tmp_err, sizeof(tmp_err));
                if (ret == 0) {
                    if (curr_pass[0]) strncpy(sa->password, curr_pass, sizeof(sa->password) - 1);
                    break;
                }
                if (ret != -1) {
                    last_meaningful_ret = ret;
                    strncpy(best_err, tmp_err, sizeof(best_err) - 1);
                }
            }
            if (ret == 0) break;
        }
        g_ptr_array_free(cand_pass, TRUE);

        if (ret != 0) {
            if (last_meaningful_ret == -1) {
                snprintf(dl->error_msg, sizeof(dl->error_msg), "No extraction tool found. Please install 7zip or unar.");
            } else {
                strncpy(dl->error_msg, best_err, sizeof(dl->error_msg) - 1);
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
 * Download creation helpers
 * ═══════════════════════════════════════════════════════════ */

static Download *start_single_download(const char *url,
                                       const char *dest_name,
                                       const char *password) {
    if (!url || strlen(url) < (size_t)URL_MIN_LEN) return NULL;

    char det_ext[32] = "";
    extract_url_ext(url, det_ext, sizeof(det_ext));

    char full_name[512] = "";
    if (dest_name && strlen(dest_name) > 0) {
        const char *dot = strrchr(dest_name, '.');
        if (dot && is_known_ext(dot))
            snprintf(full_name, sizeof(full_name), "%s", dest_name);
        else if (det_ext[0])
            snprintf(full_name, sizeof(full_name), "%s.%s", dest_name, det_ext);
        else
            snprintf(full_name, sizeof(full_name), "%s", dest_name);
    } else {
        char tmp[4096];
        strncpy(tmp, url, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *q = strchr(tmp, '?');
        if (q) *q = '\0';
        char *slash = strrchr(tmp, '/');
        char *raw_name = slash ? slash + 1 : "file";
        char *unescaped = g_uri_unescape_string(raw_name, NULL);
        snprintf(full_name, sizeof(full_name), "%s", unescaped ? unescaped : raw_name);
        if (unescaped) g_free(unescaped);
    }

    pthread_mutex_lock(&dl_mutex);
    if (n_downloads >= MAX_DOWNLOADS) {
        pthread_mutex_unlock(&dl_mutex);
        return NULL;
    }

    Download *dl = calloc(1, sizeof(Download));
    if (!dl) {
        pthread_mutex_unlock(&dl_mutex);
        return NULL;
    }
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
        if (password && password[0]) {
            strncpy(sa->password, password, sizeof(sa->password) - 1);
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
                 "%.400s/ps-ftp-arch/%ld-%.64s", base_cache_dir, (long)time(NULL), safe);

        pthread_create(&dl->thread, NULL, split_archive_thread_fn, sa);
    } else {
        pthread_create(&dl->thread, NULL, download_thread_fn, dl);
    }
    pthread_detach(dl->thread);

    return dl;
}

static Download *start_split_download(const char **urls, int n,
                                      const char *pkg_name,
                                      const char *password) {
    if (!urls || n <= 0 || !pkg_name || !pkg_name[0]) return NULL;

    SplitArchiveArgs *sa = calloc(1, sizeof(SplitArchiveArgs));
    if (!sa) return NULL;

    for (int i = 0; i < n && i < SA_MAX_URLS; i++) {
        if (urls[i] && strlen(urls[i]) >= (size_t)URL_MIN_LEN) {
            strncpy(sa->urls[sa->n_urls++], urls[i], SA_URL_LEN - 1);
        }
    }

    if (sa->n_urls == 0) {
        free(sa);
        return NULL;
    }

    char final_pkg[SA_NAME_LEN] = "";
    const char *dot = strrchr(pkg_name, '.');
    if (dot && strcasecmp(dot, ".pkg") == 0)
        snprintf(final_pkg, sizeof(final_pkg), "%s", pkg_name);
    else
        snprintf(final_pkg, sizeof(final_pkg), "%s.pkg", pkg_name);

    strncpy(sa->pkg_name, final_pkg, sizeof(sa->pkg_name) - 1);
    if (password && password[0]) {
        strncpy(sa->password, password, sizeof(sa->password) - 1);
    }

    const char *base_cache_dir = g_tmp_dir[0] ? g_tmp_dir : g_get_user_cache_dir();
    char safe[64] = "";
    strncpy(safe, final_pkg, sizeof(safe) - 1);
    for (char *p = safe; *p; p++)
        if (*p == '/' || *p == ' ' || *p == '\'' || *p == '"') *p = '_';
    snprintf(sa->tmp_dir, sizeof(sa->tmp_dir),
             "%.400s/ps-ftp-arch/%ld-%.64s", base_cache_dir, (long)time(NULL), safe);

    pthread_mutex_lock(&dl_mutex);
    if (n_downloads >= MAX_DOWNLOADS) {
        pthread_mutex_unlock(&dl_mutex);
        free(sa);
        return NULL;
    }

    Download *dl = calloc(1, sizeof(Download));
    if (!dl) {
        pthread_mutex_unlock(&dl_mutex);
        free(sa);
        return NULL;
    }
    dl->id         = next_id++;
    dl->status     = DL_DOWNLOADING;
    dl->total_size = -1;
    dl->progress   = -1.0;
    dl->eta        = -1.0;

    strncpy(dl->dest_name, final_pkg, sizeof(dl->dest_name) - 1);
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

    return dl;
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

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    char *pass = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_entry_pass_single));
G_GNUC_END_IGNORE_DEPRECATIONS

    if (strstr(url, "dlpsgame.com/")) {
        if (g_stack) {
            gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "dlpsgame");
            if (g_dlps_search_entry) {
                gtk_editable_set_text(GTK_EDITABLE(g_dlps_search_entry), url);
            }
            trigger_dlps_fetch(url, 1);
            show_toast("Apertura gioco da DLPSGame...");
        }
        if (pass) g_free(pass);
        g_entry_url = g_entry_name = g_entry_pass_single = g_lbl_detected = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    if (strstr(url, "downloadgameps3.net") || strstr(url, "filecrypt.cc")) {
        if (g_uc_enabled && is_uc_resolver_available()) {
            trigger_uc_resolve(url, (name && name[0]) ? name : "Download", pass, NULL, NULL);
            if (pass) g_free(pass);
            g_entry_url = g_entry_name = g_entry_pass_single = g_lbl_detected = NULL;
            gtk_window_destroy(GTK_WINDOW(dialog));
            return;
        }
        if (pass) g_free(pass);
        show_toast("Attenzione: link protetto! Abilita Undetected ChromeDriver o sbloccalo nel browser.");
        g_entry_url = g_entry_name = g_entry_pass_single = g_lbl_detected = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    start_single_download(url, name, pass);

    if (pass) g_free(pass);
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

static void show_add_dialog_prefilled(GtkWindow *parent, const char *pre_url, const char *pre_name, const char *pre_pass);
static void show_add_dialog(GtkWindow *parent) {
    show_add_dialog_prefilled(parent, NULL, NULL, NULL);
}

static void show_add_dialog_prefilled(GtkWindow *parent, const char *pre_url, const char *pre_name, const char *pre_pass) {
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

    if (pre_url && pre_url[0]) gtk_editable_set_text(GTK_EDITABLE(g_entry_url), pre_url);
    if (pre_name && pre_name[0]) gtk_editable_set_text(GTK_EDITABLE(g_entry_name), pre_name);
    if (pre_pass && pre_pass[0]) {
        GtkWidget *child = gtk_combo_box_get_child(GTK_COMBO_BOX(g_entry_pass_single));
        if (child && GTK_IS_EDITABLE(child)) gtk_editable_set_text(GTK_EDITABLE(child), pre_pass);
    }

    g_signal_connect(dialog, "response", G_CALLBACK(on_add_dialog_response), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(pre_url && pre_url[0] ? g_entry_name : g_entry_url);
}

/* ═══════════════════════════════════════════════════════════
 * Undetected ChromeDriver (UC) Bot Resolver helpers
 * ═══════════════════════════════════════════════════════════ */

static const char *get_python_binary(void) {
    static char py_path[512] = "";
    if (py_path[0]) return py_path;

    const char *data_dir = g_get_user_data_dir();
    char venv_py[512];
    snprintf(venv_py, sizeof(venv_py), "%s/ps-ftp-downloader/venv/bin/python3", data_dir);
    if (access(venv_py, X_OK) == 0) {
        strncpy(py_path, venv_py, sizeof(py_path) - 1);
        return py_path;
    }

    if (system("which python3 >/dev/null 2>&1") == 0) {
        strncpy(py_path, "python3", sizeof(py_path) - 1);
        return py_path;
    }

    return NULL;
}

static const char *get_uc_resolver_path(void) {
    static char path[1024] = "";
    if (path[0]) return path;

    char exe_dir[512] = "";
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char *slash = strrchr(exe_dir, '/');
        if (slash) *slash = '\0';
        snprintf(path, sizeof(path), "%s/uc_resolver.py", exe_dir);
        if (access(path, R_OK) == 0) return path;
    }

    snprintf(path, sizeof(path), "/usr/share/ps-ftp-downloader/uc_resolver.py");
    if (access(path, R_OK) == 0) return path;

    snprintf(path, sizeof(path), "./uc_resolver.py");
    if (access(path, R_OK) == 0) return path;

    snprintf(path, sizeof(path), "gtk-app/uc_resolver.py");
    if (access(path, R_OK) == 0) return path;

    path[0] = '\0';
    return NULL;
}

static gboolean is_uc_resolver_available(void) {
    const char *script = get_uc_resolver_path();
    const char *py = get_python_binary();
    if (!script || !py) return FALSE;

    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "%s \"%s\" --check >/dev/null 2>&1", py, script);
    return (system(cmd) == 0);
}

static char *get_uc_resolver_status_str(void) {
    const char *script = get_uc_resolver_path();
    const char *py = get_python_binary();
    if (!script) {
        return g_strdup("Script uc_resolver.py non trovato");
    }
    if (!py) {
        return g_strdup("python3 non trovato nel sistema");
    }

    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "%s \"%s\" --check 2>/dev/null", py, script);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return g_strdup("Errore esecuzione test resolver");
    }

    char buf[1024] = "";
    if (fgets(buf, sizeof(buf), fp) != NULL) {
        pclose(fp);
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, buf, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *obj = json_node_get_object(root);
                gboolean ready = json_object_has_member(obj, "ready") ? json_object_get_boolean_member(obj, "ready") : FALSE;
                const char *browser = json_object_has_member(obj, "browser") ? json_object_get_string_member(obj, "browser") : NULL;
                g_object_unref(parser);
                if (ready) {
                    return g_strdup_printf("Pronto all'uso (Browser: %s)", browser ? browser : "Chromium");
                }
            }
            g_object_unref(parser);
        }
        return g_strdup("Dipendenze non trovate (clicca 'Setup Resolver')");
    }
    pclose(fp);
    return g_strdup("Non configurato (clicca 'Setup Resolver')");
}

static void on_setup_uc_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GtkWidget *lbl_status = (GtkWidget *)user_data;
    char setup_script[1024] = "";
    char exe_dir[512] = "";
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char *slash = strrchr(exe_dir, '/');
        if (slash) *slash = '\0';
        snprintf(setup_script, sizeof(setup_script), "%s/setup-bot-resolver.sh", exe_dir);
    }
    if (access(setup_script, X_OK) != 0) {
        snprintf(setup_script, sizeof(setup_script), "/usr/share/ps-ftp-downloader/setup-bot-resolver.sh");
    }
    if (access(setup_script, X_OK) != 0) {
        snprintf(setup_script, sizeof(setup_script), "gtk-app/setup-bot-resolver.sh");
    }

    if (access(setup_script, R_OK) == 0) {
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "bash \"%s\"", setup_script);
        g_spawn_command_line_async(cmd, NULL);
        show_toast("Setup avviato: installazione dipendenze in corso...");
    } else {
        show_toast("Esegui: pip install undetected-chromedriver selenium");
    }

    if (lbl_status && GTK_IS_LABEL(lbl_status)) {
        char *st = get_uc_resolver_status_str();
        gtk_label_set_text(GTK_LABEL(lbl_status), st);
        g_free(st);
    }
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
        if (g_set_rd_token && GTK_IS_EDITABLE(g_set_rd_token)) {
            const char *rd = gtk_editable_get_text(GTK_EDITABLE(g_set_rd_token));
            if (rd) {
                strncpy(g_realdebrid_token, rd, sizeof(g_realdebrid_token) - 1);
                g_strstrip(g_realdebrid_token);
            }
        }
        if (g_chk_uc_enabled && GTK_IS_CHECK_BUTTON(g_chk_uc_enabled))
            g_uc_enabled = gtk_check_button_get_active(GTK_CHECK_BUTTON(g_chk_uc_enabled));
        if (g_chk_uc_headless && GTK_IS_CHECK_BUTTON(g_chk_uc_headless))
            g_uc_headless = gtk_check_button_get_active(GTK_CHECK_BUTTON(g_chk_uc_headless));

        save_settings();

        if (g_lbl_sub && GTK_IS_WIDGET(g_lbl_sub)) {
            char sub[512];
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

    gtk_widget_set_size_request(dialog, 460, -1);

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
        { "Real-Debrid Token (Opzionale)", &g_set_rd_token, g_realdebrid_token, TRUE },
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

    /* ── Undetected ChromeDriver Section ── */
    GtkWidget *sep_uc = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep_uc, 8);
    gtk_widget_set_margin_bottom(sep_uc, 4);
    gtk_box_append(GTK_BOX(form), sep_uc);

    GtkWidget *lbl_uc_sec = gtk_label_new("Undetected ChromeDriver (Automazione Bot / Link Protetti)");
    gtk_label_set_xalign(GTK_LABEL(lbl_uc_sec), 0.0);
    gtk_widget_add_css_class(lbl_uc_sec, "form-label");
    gtk_box_append(GTK_BOX(form), lbl_uc_sec);

    char *uc_st = get_uc_resolver_status_str();
    GtkWidget *lbl_uc_status = gtk_label_new(uc_st);
    gtk_label_set_xalign(GTK_LABEL(lbl_uc_status), 0.0);
    gtk_box_append(GTK_BOX(form), lbl_uc_status);
    g_free(uc_st);

    GtkWidget *uc_box_opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(uc_box_opts, 4);

    g_chk_uc_enabled = gtk_check_button_new_with_label("Abilita Undetected ChromeDriver (Download Automatico / 1-Click)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_chk_uc_enabled), g_uc_enabled);
    gtk_box_append(GTK_BOX(uc_box_opts), g_chk_uc_enabled);

    g_chk_uc_headless = gtk_check_button_new_with_label("Modalità Headless (avvia browser in background)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_chk_uc_headless), g_uc_headless);
    gtk_box_append(GTK_BOX(uc_box_opts), g_chk_uc_headless);

    GtkWidget *btn_setup_uc = gtk_button_new_with_label("Setup Dipendenze Undetected ChromeDriver");
    gtk_widget_add_css_class(btn_setup_uc, "btn-settings");
    g_signal_connect(btn_setup_uc, "clicked", G_CALLBACK(on_setup_uc_clicked), lbl_uc_status);
    gtk_box_append(GTK_BOX(uc_box_opts), btn_setup_uc);

    gtk_box_append(GTK_BOX(form), uc_box_opts);

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
    const char *pkg_name_input = g_arch_entry_name ?
        gtk_editable_get_text(GTK_EDITABLE(g_arch_entry_name)) : NULL;

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    char *password = g_arch_entry_pass ?
        gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(g_arch_entry_pass)) : NULL;
G_GNUC_END_IGNORE_DEPRECATIONS

    char pkg_name[SA_NAME_LEN] = "";
    if (pkg_name_input && pkg_name_input[0]) {
        snprintf(pkg_name, sizeof(pkg_name), "%s", pkg_name_input);
    } else {
        snprintf(pkg_name, sizeof(pkg_name), "archive.pkg");
    }

    const char *url_ptrs[SA_MAX_URLS];
    char url_buffers[SA_MAX_URLS][SA_URL_LEN];
    int n_urls = 0;

    if (g_arch_urls_box) {
        GtkWidget *row = gtk_widget_get_first_child(g_arch_urls_box);
        while (row && n_urls < SA_MAX_URLS) {
            GtkWidget *entry = gtk_widget_get_last_child(row);
            if (entry && GTK_IS_ENTRY(entry)) {
                const char *raw_url = gtk_editable_get_text(GTK_EDITABLE(entry));
                if (raw_url) {
                    char url[SA_URL_LEN];
                    strncpy(url, raw_url, sizeof(url) - 1);
                    url[sizeof(url) - 1] = '\0';
                    g_strstrip(url);
                    if (strlen(url) >= (size_t)URL_MIN_LEN) {
                        strncpy(url_buffers[n_urls], url, SA_URL_LEN - 1);
                        url_buffers[n_urls][SA_URL_LEN - 1] = '\0';
                        url_ptrs[n_urls] = url_buffers[n_urls];
                        n_urls++;
                    }
                }
            }
            row = gtk_widget_get_next_sibling(row);
        }
    }

    if (n_urls > 0) {
        start_split_download(url_ptrs, n_urls, pkg_name, password);
    }

    if (password) g_free(password);
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
 * Catalog module: structs, GObject wrapper, fetch & UI
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    char id[128];
    char name[256];
    char version[64];
    char category[64];
    char region[32];
    char description[512];
    char icon_url[1024];
    char url[SA_URL_LEN];
    char parts[SA_MAX_URLS][SA_URL_LEN];
    int  n_parts;
    char password[256];
    char pkg_name[SA_NAME_LEN];
    gint64 size;
    char source_name[256];
} CatalogItem;

#define TYPE_CATALOG_OBJECT (catalog_object_get_type())
G_DECLARE_FINAL_TYPE(CatalogObject, catalog_object, CATALOG, OBJECT, GObject)

struct _CatalogObject {
    GObject parent_instance;
    CatalogItem *item;
};

G_DEFINE_TYPE(CatalogObject, catalog_object, G_TYPE_OBJECT)

static void catalog_object_init(CatalogObject *self) {
    self->item = NULL;
}

static void catalog_object_finalize(GObject *object) {
    CatalogObject *self = CATALOG_OBJECT(object);
    if (self->item) {
        g_free(self->item);
        self->item = NULL;
    }
    G_OBJECT_CLASS(catalog_object_parent_class)->finalize(object);
}

static void catalog_object_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    CatalogObject *self = CATALOG_OBJECT(object);
    if (prop_id == 1) {
        g_value_set_string(value, (self->item && self->item->name[0]) ? self->item->name : "");
    } else {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void catalog_object_class_init(CatalogObjectClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->get_property = catalog_object_get_property;
    gobject_class->finalize = catalog_object_finalize;
    g_object_class_install_property(gobject_class, 1,
        g_param_spec_string("name", "Name", "Name", "", G_PARAM_READABLE));
}

static CatalogObject *catalog_object_new(const CatalogItem *item) {
    CatalogObject *obj = g_object_new(TYPE_CATALOG_OBJECT, NULL);
    if (item) {
        obj->item = g_new0(CatalogItem, 1);
        *obj->item = *item;
    }
    return obj;
}

/* Toast & Pre-Download Progress helpers */
static gboolean on_toast_pulse_cb(gpointer user_data) {
    (void)user_data;
    if (g_toast_progress && GTK_IS_PROGRESS_BAR(g_toast_progress)) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(g_toast_progress));
    }
    return G_SOURCE_CONTINUE;
}

static void hide_prep_progress(void) {
    if (g_toast_pulse_id > 0) {
        g_source_remove(g_toast_pulse_id);
        g_toast_pulse_id = 0;
    }
    if (g_toast_progress && GTK_IS_WIDGET(g_toast_progress)) {
        gtk_widget_set_visible(g_toast_progress, FALSE);
    }
}

static gboolean on_hide_toast(gpointer user_data) {
    (void)user_data;
    hide_prep_progress();
    if (g_toast_box && GTK_IS_WIDGET(g_toast_box)) {
        gtk_widget_set_visible(g_toast_box, FALSE);
    }
    g_toast_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void show_toast(const char *msg) {
    if (!g_toast_box || !g_toast_label) return;
    hide_prep_progress();
    gtk_label_set_text(GTK_LABEL(g_toast_label), msg ? msg : "");
    gtk_widget_set_visible(g_toast_box, TRUE);
    if (g_toast_timeout_id > 0) g_source_remove(g_toast_timeout_id);
    g_toast_timeout_id = g_timeout_add_seconds(4, on_hide_toast, NULL);
}

static void show_prep_progress(const char *msg) {
    if (!g_toast_box || !g_toast_label) return;
    if (g_toast_timeout_id > 0) {
        g_source_remove(g_toast_timeout_id);
        g_toast_timeout_id = 0;
    }
    gtk_label_set_text(GTK_LABEL(g_toast_label), msg ? msg : "Preparazione download in corso...");
    if (g_toast_progress && GTK_IS_PROGRESS_BAR(g_toast_progress)) {
        gtk_widget_set_visible(g_toast_progress, TRUE);
        gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(g_toast_progress), 0.08);
        if (g_toast_pulse_id == 0) {
            g_toast_pulse_id = g_timeout_add(70, on_toast_pulse_cb, NULL);
        }
    }
    gtk_widget_set_visible(g_toast_box, TRUE);
}

/* Validation helpers */
static gboolean is_valid_feed_url(const char *url) {
    if (!url || !url[0]) return FALSE;
    if (g_str_has_prefix(url, "https://")) return TRUE;
    /* Local testing support (Step 8) */
    if (g_str_has_prefix(url, "http://localhost:") || g_str_has_prefix(url, "http://127.0.0.1:") ||
        g_str_has_prefix(url, "http://localhost/")  || g_str_has_prefix(url, "http://127.0.0.1/"))
        return TRUE;
    return FALSE;
}

static gboolean is_valid_pkg_name(const char *name) {
    if (!name || !name[0]) return FALSE;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return FALSE;
    if (strstr(name, "..") != NULL) return FALSE;
    return TRUE;
}

static char *get_catalog_cache_path(const char *source_url) {
    const char *cache_dir = g_get_user_cache_dir();
    char *dir = g_build_filename(cache_dir, "ps-ftp-downloader", NULL);
    g_mkdir_with_parents(dir, 0755);
    guint h = g_str_hash(source_url);
    char fname[64];
    snprintf(fname, sizeof(fname), "catalog-%08x.json", h);
    char *path = g_build_filename(dir, fname, NULL);
    g_free(dir);
    return path;
}

typedef struct {
    char *data;
    size_t size;
    size_t max_size;
} MemBuf;

static size_t curl_buf_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    MemBuf *mem = (MemBuf *)userp;
    if (mem->size + total > mem->max_size) return 0;
    char *ptr = realloc(mem->data, mem->size + total + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(mem->data + mem->size, contents, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

static int parse_catalog_json(const char *json_data, gsize json_len, const char *source_url, CatalogItem *out_items, int max_items) {
    if (!json_data || json_len == 0) return 0;
    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    int count = 0;
    if (json_parser_load_from_data(parser, json_data, json_len, &err)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            const char *feed_name = json_object_has_member(obj, "name") ?
                json_object_get_string_member(obj, "name") : source_url;
            if (json_object_has_member(obj, "items")) {
                JsonArray *arr = json_object_get_array_member(obj, "items");
                guint len = json_array_get_length(arr);
                for (guint i = 0; i < len && count < max_items; i++) {
                    JsonNode *inode = json_array_get_element(arr, i);
                    if (!JSON_NODE_HOLDS_OBJECT(inode)) continue;
                    JsonObject *iobj = json_node_get_object(inode);
                    CatalogItem it;
                    memset(&it, 0, sizeof(it));
                    strncpy(it.source_name, feed_name, sizeof(it.source_name) - 1);

                    if (json_object_has_member(iobj, "id"))
                        strncpy(it.id, json_object_get_string_member(iobj, "id"), sizeof(it.id) - 1);
                    if (json_object_has_member(iobj, "name"))
                        strncpy(it.name, json_object_get_string_member(iobj, "name"), sizeof(it.name) - 1);
                    if (json_object_has_member(iobj, "version"))
                        strncpy(it.version, json_object_get_string_member(iobj, "version"), sizeof(it.version) - 1);
                    if (json_object_has_member(iobj, "category"))
                        strncpy(it.category, json_object_get_string_member(iobj, "category"), sizeof(it.category) - 1);
                    if (json_object_has_member(iobj, "region"))
                        strncpy(it.region, json_object_get_string_member(iobj, "region"), sizeof(it.region) - 1);
                    if (json_object_has_member(iobj, "description"))
                        strncpy(it.description, json_object_get_string_member(iobj, "description"), sizeof(it.description) - 1);
                    if (json_object_has_member(iobj, "icon"))
                        strncpy(it.icon_url, json_object_get_string_member(iobj, "icon"), sizeof(it.icon_url) - 1);
                    if (json_object_has_member(iobj, "url"))
                        strncpy(it.url, json_object_get_string_member(iobj, "url"), sizeof(it.url) - 1);
                    if (json_object_has_member(iobj, "password"))
                        strncpy(it.password, json_object_get_string_member(iobj, "password"), sizeof(it.password) - 1);
                    if (json_object_has_member(iobj, "pkg_name"))
                        strncpy(it.pkg_name, json_object_get_string_member(iobj, "pkg_name"), sizeof(it.pkg_name) - 1);
                    if (json_object_has_member(iobj, "size"))
                        it.size = json_object_get_int_member(iobj, "size");

                    if (json_object_has_member(iobj, "parts")) {
                        JsonArray *parts_arr = json_object_get_array_member(iobj, "parts");
                        guint plen = json_array_get_length(parts_arr);
                        for (guint p = 0; p < plen && it.n_parts < SA_MAX_URLS; p++) {
                            const char *part_url = json_array_get_string_element(parts_arr, p);
                            if (part_url && part_url[0]) {
                                strncpy(it.parts[it.n_parts++], part_url, SA_URL_LEN - 1);
                            }
                        }
                    }

                    if (!it.category[0]) strncpy(it.category, "Altro", sizeof(it.category) - 1);

                    /* Validation rules (Step 3: name vuoto o pkg_name vuoto scartare) */
                    if (!it.name[0]) continue;
                    if (!it.pkg_name[0]) continue;
                    if (!is_valid_pkg_name(it.pkg_name)) continue;

                    gboolean urls_valid = TRUE;
                    if (it.n_parts > 0) {
                        for (int p = 0; p < it.n_parts; p++) {
                            if (!is_valid_feed_url(it.parts[p])) { urls_valid = FALSE; break; }
                        }
                    } else if (it.url[0]) {
                        if (!is_valid_feed_url(it.url)) urls_valid = FALSE;
                    } else {
                        urls_valid = FALSE;
                    }

                    if (!urls_valid) continue;

                    out_items[count++] = it;
                }
            }
        }
    } else {
        if (err) g_error_free(err);
    }
    g_object_unref(parser);
    return count;
}

typedef struct {
    CatalogItem *items;
    int count;
} CatalogFetchResult;

static gboolean on_catalog_fetch_done(gpointer user_data) {
    CatalogFetchResult *res = (CatalogFetchResult *)user_data;
    if (!g_catalog_store) {
        if (res->items) free(res->items);
        free(res);
        return G_SOURCE_REMOVE;
    }

    g_list_store_remove_all(g_catalog_store);

    GHashTable *cat_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(cat_table, g_strdup("Tutte"));

    for (int i = 0; i < res->count; i++) {
        CatalogObject *obj = catalog_object_new(&res->items[i]);
        g_list_store_append(g_catalog_store, obj);
        g_object_unref(obj);

        if (res->items[i].category[0]) {
            if (!g_hash_table_contains(cat_table, res->items[i].category)) {
                g_hash_table_add(cat_table, g_strdup(res->items[i].category));
            }
        }
    }

    if (g_catalog_cat_dropdown) {
        GtkStringList *sl = gtk_string_list_new(NULL);
        gtk_string_list_append(sl, "Tutte");

        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, cat_table);
        while (g_hash_table_iter_next(&iter, &key, &val)) {
            const char *c = (const char *)key;
            if (strcmp(c, "Tutte") != 0) {
                gtk_string_list_append(sl, c);
            }
        }
        gtk_drop_down_set_model(GTK_DROP_DOWN(g_catalog_cat_dropdown), G_LIST_MODEL(sl));
        g_object_unref(sl);
    }
    g_hash_table_destroy(cat_table);

    if (res->items) free(res->items);
    free(res);
    return G_SOURCE_REMOVE;
}

static void *catalog_fetch_thread_fn(void *arg) {
    (void)arg;
    int max_items = 256;
    CatalogItem *all_items = calloc(max_items, sizeof(CatalogItem));
    int total_count = 0;

    for (int s = 0; s < g_n_sources; s++) {
        const char *src_url = g_catalog_sources[s];
        if (!src_url || !src_url[0]) continue;

        MemBuf mem;
        mem.data = malloc(1);
        mem.size = 0;
        mem.max_size = 5 * 1024 * 1024;
        if (mem.data) mem.data[0] = '\0';

        CURL *c = curl_easy_init();
        curl_common_opts(c, src_url);
        CURL_SET(c, CURLOPT_NOSIGNAL, 1L);
        CURL_SET(c, CURLOPT_URL, src_url);
        CURL_SET(c, CURLOPT_TIMEOUT, 15L);
        CURL_SET(c, CURLOPT_CONNECTTIMEOUT, 10L);
        CURL_SET(c, CURLOPT_FOLLOWLOCATION, 1L);
        CURL_SET(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)mem.max_size);
        CURL_SET(c, CURLOPT_WRITEFUNCTION, curl_buf_write_cb);
        CURL_SET(c, CURLOPT_WRITEDATA, &mem);

        CURLcode res = curl_easy_perform(c);
        long http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(c);

        gboolean parsed_ok = FALSE;
        if (res == CURLE_OK && http_code == 200 && mem.data && mem.size > 0) {
            int parsed = parse_catalog_json(mem.data, mem.size, src_url, all_items + total_count, max_items - total_count);
            if (parsed > 0) {
                total_count += parsed;
                parsed_ok = TRUE;
                char *cpath = get_catalog_cache_path(src_url);
                g_file_set_contents(cpath, mem.data, mem.size, NULL);
                g_free(cpath);
            }
        }

        if (!parsed_ok) {
            char *cpath = get_catalog_cache_path(src_url);
            char *cached_data = NULL;
            gsize cached_len = 0;
            if (g_file_get_contents(cpath, &cached_data, &cached_len, NULL)) {
                int parsed = parse_catalog_json(cached_data, cached_len, src_url, all_items + total_count, max_items - total_count);
                if (parsed > 0) {
                    total_count += parsed;
                }
                g_free(cached_data);
            }
            g_free(cpath);
        }

        if (mem.data) free(mem.data);
    }

    /* Fallback a file catalogo locale se nessun elemento e' stato scaricato (es. offline, primo avvio, o dev) */
    if (total_count == 0) {
        static const char *LOCAL_CATALOGS[] = {
            "catalog.json",
            "../catalog.json",
            "/usr/share/ps-ftp-downloader/catalog.json",
            "/usr/share/ps-ftp-downloader/catalog.json.example",
            NULL
        };
        for (int i = 0; LOCAL_CATALOGS[i]; i++) {
            char *data = NULL;
            gsize len = 0;
            if (g_file_get_contents(LOCAL_CATALOGS[i], &data, &len, NULL)) {
                int parsed = parse_catalog_json(data, len, "Catalogo Locale", all_items, max_items);
                g_free(data);
                if (parsed > 0) {
                    total_count = parsed;
                    break;
                }
            }
        }
    }

    CatalogFetchResult *res = malloc(sizeof(CatalogFetchResult));
    res->items = all_items;
    res->count = total_count;
    g_idle_add(on_catalog_fetch_done, res);
    return NULL;
}

static void trigger_catalog_fetch_async(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, catalog_fetch_thread_fn, NULL) == 0) {
        pthread_detach(th);
    }
}

/* Icon async loader */
typedef struct {
    char icon_url[1024];
    char cache_path[512];
    GtkWidget *image;
    int target_w;
    int target_h;
} IconLoadTask;

static void set_image_from_file_scaled(GtkWidget *widget, const char *path, int target_w, int target_h) {
    if (!widget || !path || !path[0]) return;
    GError *err = NULL;
    gboolean is_pic = GTK_IS_PICTURE(widget);
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path,
        (!is_pic && target_w > 0) ? target_w : -1,
        (!is_pic && target_h > 0) ? target_h : -1, TRUE, &err);
    if (pb) {
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
G_GNUC_END_IGNORE_DEPRECATIONS
        if (tex) {
            if (GTK_IS_IMAGE(widget)) {
                gtk_image_set_from_paintable(GTK_IMAGE(widget), GDK_PAINTABLE(tex));
            } else if (GTK_IS_PICTURE(widget)) {
                gtk_picture_set_paintable(GTK_PICTURE(widget), GDK_PAINTABLE(tex));
            }
            g_object_unref(tex);
        }
        g_object_unref(pb);
    } else {
        if (err) g_error_free(err);
        unlink(path);
        if (GTK_IS_IMAGE(widget)) {
            gtk_image_set_from_icon_name(GTK_IMAGE(widget), "media-optical");
        }
    }
}

static gboolean on_icon_loaded_idle(gpointer user_data) {
    IconLoadTask *task = (IconLoadTask *)user_data;
    if (task->image && (GTK_IS_IMAGE(task->image) || GTK_IS_PICTURE(task->image)) &&
        g_file_test(task->cache_path, G_FILE_TEST_EXISTS)) {
        set_image_from_file_scaled(task->image, task->cache_path, task->target_w, task->target_h);
    }
    if (task->image) g_object_unref(task->image);
    free(task);
    return G_SOURCE_REMOVE;
}

static void *icon_loader_thread(void *arg) {
    IconLoadTask *task = (IconLoadTask *)arg;
    if (!g_file_test(task->cache_path, G_FILE_TEST_EXISTS)) {
        CURL *c = curl_easy_init();
        if (c) {
            curl_common_opts(c, task->icon_url);
            CURL_SET(c, CURLOPT_NOSIGNAL, 1L);
            CURL_SET(c, CURLOPT_URL, task->icon_url);
            CURL_SET(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
            CURL_SET(c, CURLOPT_TIMEOUT, 10L);
            CURL_SET(c, CURLOPT_CONNECTTIMEOUT, 5L);
            CURL_SET(c, CURLOPT_FOLLOWLOCATION, 1L);
            CURL_SET(c, CURLOPT_FAILONERROR, 1L);
            FILE *fp = fopen(task->cache_path, "wb");
            if (fp) {
                CURL_SET(c, CURLOPT_WRITEDATA, fp);
                CURLcode res = curl_easy_perform(c);
                fclose(fp);
                long http_code = 0;
                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
                if (res != CURLE_OK || http_code != 200) {
                    unlink(task->cache_path);
                }
            }
            curl_easy_cleanup(c);
        }
    }
    g_idle_add(on_icon_loaded_idle, task);
    return NULL;
}

static void load_icon_async(const char *url, GtkWidget *img, int target_w, int target_h) {
    if (!url || !url[0] || !is_valid_feed_url(url)) return;
    const char *cache_dir = g_get_user_cache_dir();
    char *dir = g_build_filename(cache_dir, "ps-ftp-downloader", "icons", NULL);
    g_mkdir_with_parents(dir, 0755);
    guint h = g_str_hash(url);
    char fname[64];
    snprintf(fname, sizeof(fname), "ico-%08x.png", h);
    char *path = g_build_filename(dir, fname, NULL);
    g_free(dir);

    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        set_image_from_file_scaled(img, path, target_w, target_h);
        g_free(path);
        return;
    }

    IconLoadTask *task = malloc(sizeof(IconLoadTask));
    strncpy(task->icon_url, url, sizeof(task->icon_url) - 1);
    task->icon_url[sizeof(task->icon_url) - 1] = '\0';
    strncpy(task->cache_path, path, sizeof(task->cache_path) - 1);
    task->cache_path[sizeof(task->cache_path) - 1] = '\0';
    g_free(path);
    task->image = g_object_ref(img);
    task->target_w = target_w;
    task->target_h = target_h;

    pthread_t th;
    if (pthread_create(&th, NULL, icon_loader_thread, task) == 0) {
        pthread_detach(th);
    } else {
        g_object_unref(task->image);
        free(task);
    }
}

/* 1-click install logic */
static void perform_catalog_install(const CatalogItem *item) {
    if (item->n_parts > 0) {
        const char *parts[SA_MAX_URLS];
        for (int p = 0; p < item->n_parts; p++) parts[p] = item->parts[p];
        start_split_download(parts, item->n_parts, item->pkg_name, item->password);
    } else {
        start_single_download(item->url, item->pkg_name, item->password);
    }

    pthread_mutex_lock(&dl_mutex);
    int total = n_downloads;
    pthread_mutex_unlock(&dl_mutex);
    if (g_stack) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), total == 0 ? "empty" : "list");
    }

    char toast_msg[512];
    snprintf(toast_msg, sizeof(toast_msg), "Aggiunto: %s", item->name);
    show_toast(toast_msg);
}

typedef struct {
    CatalogItem item;
} ConfirmInstallData;

static void on_confirm_install_response(GtkDialog *dialog, int response, gpointer user_data) {
    ConfirmInstallData *cd = (ConfirmInstallData *)user_data;
    if (response == GTK_RESPONSE_OK) {
        perform_catalog_install(&cd->item);
    }
    free(cd);
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void on_catalog_install_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    CatalogObject *obj = CATALOG_OBJECT(user_data);
    if (!obj || !obj->item) return;
    const CatalogItem *item = obj->item;

    gboolean duplicate = FALSE;
    pthread_mutex_lock(&dl_mutex);
    for (int i = 0; i < n_downloads; i++) {
        if (downloads[i] &&
            downloads[i]->status != DL_ERROR &&
            downloads[i]->status != DL_CANCELLED &&
            strcasecmp(downloads[i]->dest_name, item->pkg_name) == 0) {
            duplicate = TRUE;
            break;
        }
    }
    pthread_mutex_unlock(&dl_mutex);

    if (duplicate) {
        GtkWidget *dialog = gtk_dialog_new_with_buttons(
            "Conferma Installazione", GTK_WINDOW(g_window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "Annulla", GTK_RESPONSE_CANCEL,
            "Scarica di nuovo", GTK_RESPONSE_OK,
            NULL);
        GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "Il pacchetto '%s' è già presente nei download.\nVuoi scaricarlo di nuovo?",
                 item->pkg_name);
        GtkWidget *lbl = gtk_label_new(msg);
        gtk_widget_set_margin_top(lbl, 20);
        gtk_widget_set_margin_bottom(lbl, 20);
        gtk_widget_set_margin_start(lbl, 20);
        gtk_widget_set_margin_end(lbl, 20);
        gtk_box_append(GTK_BOX(content), lbl);

        ConfirmInstallData *cd = malloc(sizeof(ConfirmInstallData));
        cd->item = *item;
        g_signal_connect(dialog, "response", G_CALLBACK(on_confirm_install_response), cd);
        gtk_window_present(GTK_WINDOW(dialog));
    } else {
        perform_catalog_install(item);
    }
}

/* Catalog row factory */
static void on_catalog_item_setup(GtkSignalListItemFactory *self, GtkListItem *list_item, gpointer user_data) {
    (void)self; (void)user_data;
    gtk_list_item_set_selectable(list_item, FALSE);
    gtk_list_item_set_focusable(list_item, FALSE);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(card, "catalog-card");
    gtk_widget_set_hexpand(card, TRUE);

    GtkWidget *img = gtk_image_new_from_icon_name("package-x-generic");
    gtk_widget_add_css_class(img, "catalog-icon");
    gtk_image_set_pixel_size(GTK_IMAGE(img), 44);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(col, TRUE);

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lbl_name = gtk_label_new("");
    gtk_widget_add_css_class(lbl_name, "catalog-title");
    gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);

    GtkWidget *lbl_ver = gtk_label_new("");
    gtk_widget_add_css_class(lbl_ver, "catalog-ver");

    GtkWidget *lbl_cat = gtk_label_new("");
    gtk_widget_add_css_class(lbl_cat, "catalog-cat");

    GtkWidget *lbl_reg = gtk_label_new("");
    gtk_widget_add_css_class(lbl_reg, "badge-region");

    gtk_box_append(GTK_BOX(top_row), lbl_name);
    gtk_box_append(GTK_BOX(top_row), lbl_ver);
    gtk_box_append(GTK_BOX(top_row), lbl_cat);
    gtk_box_append(GTK_BOX(top_row), lbl_reg);

    GtkWidget *lbl_desc = gtk_label_new("");
    gtk_widget_add_css_class(lbl_desc, "catalog-desc");
    gtk_label_set_xalign(GTK_LABEL(lbl_desc), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_desc), PANGO_ELLIPSIZE_END);

    GtkWidget *lbl_meta = gtk_label_new("");
    gtk_widget_add_css_class(lbl_meta, "catalog-meta");
    gtk_label_set_xalign(GTK_LABEL(lbl_meta), 0.0);

    gtk_box_append(GTK_BOX(col), top_row);
    gtk_box_append(GTK_BOX(col), lbl_desc);
    gtk_box_append(GTK_BOX(col), lbl_meta);

    GtkWidget *btn_inst = gtk_button_new_with_label("Installa");
    gtk_widget_add_css_class(btn_inst, "btn-install");
    gtk_widget_set_valign(btn_inst, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(btn_inst, FALSE);

    gtk_box_append(GTK_BOX(card), img);
    gtk_box_append(GTK_BOX(card), col);
    gtk_box_append(GTK_BOX(card), btn_inst);

    g_object_set_data(G_OBJECT(card), "img", img);
    g_object_set_data(G_OBJECT(card), "name", lbl_name);
    g_object_set_data(G_OBJECT(card), "ver", lbl_ver);
    g_object_set_data(G_OBJECT(card), "cat", lbl_cat);
    g_object_set_data(G_OBJECT(card), "reg", lbl_reg);
    g_object_set_data(G_OBJECT(card), "desc", lbl_desc);
    g_object_set_data(G_OBJECT(card), "meta", lbl_meta);
    g_object_set_data(G_OBJECT(card), "btn", btn_inst);

    gtk_list_item_set_child(list_item, card);
}

static void on_catalog_item_bind(GtkSignalListItemFactory *self, GtkListItem *list_item, gpointer user_data) {
    (void)self; (void)user_data;
    GtkWidget *card = gtk_list_item_get_child(list_item);
    CatalogObject *obj = CATALOG_OBJECT(gtk_list_item_get_item(list_item));
    if (!card || !obj) return;

    GtkWidget *img = g_object_get_data(G_OBJECT(card), "img");
    GtkWidget *lbl_name = g_object_get_data(G_OBJECT(card), "name");
    GtkWidget *lbl_ver = g_object_get_data(G_OBJECT(card), "ver");
    GtkWidget *lbl_cat = g_object_get_data(G_OBJECT(card), "cat");
    GtkWidget *lbl_reg = g_object_get_data(G_OBJECT(card), "reg");
    GtkWidget *lbl_desc = g_object_get_data(G_OBJECT(card), "desc");
    GtkWidget *lbl_meta = g_object_get_data(G_OBJECT(card), "meta");
    GtkWidget *btn = g_object_get_data(G_OBJECT(card), "btn");

    if (!obj || !obj->item) return;
    gtk_label_set_text(GTK_LABEL(lbl_name), obj->item->name);
    gtk_label_set_text(GTK_LABEL(lbl_ver), obj->item->version[0] ? obj->item->version : "v1.0");
    gtk_label_set_text(GTK_LABEL(lbl_cat), obj->item->category[0] ? obj->item->category : "App");
    gtk_label_set_text(GTK_LABEL(lbl_desc), obj->item->description);

    if (lbl_reg) {
        if (obj->item->region[0] && strcasecmp(obj->item->region, "Tutte") != 0 && strcasecmp(obj->item->region, "ALL") != 0) {
            gtk_label_set_text(GTK_LABEL(lbl_reg), obj->item->region);
            gtk_widget_set_visible(lbl_reg, TRUE);
        } else {
            gtk_widget_set_visible(lbl_reg, FALSE);
        }
    }

    char meta_buf[1024] = "";
    if (obj->item->size > 0) {
        char sz_str[32];
        fmt_size((curl_off_t)obj->item->size, sz_str, sizeof(sz_str));
        snprintf(meta_buf, sizeof(meta_buf), "Dimensione: %s • File: %s • Sorgente: %s",
                 sz_str, obj->item->pkg_name, obj->item->source_name[0] ? obj->item->source_name : "Feed");
    } else {
        snprintf(meta_buf, sizeof(meta_buf), "File: %s • Sorgente: %s",
                 obj->item->pkg_name, obj->item->source_name[0] ? obj->item->source_name : "Feed");
    }
    gtk_label_set_text(GTK_LABEL(lbl_meta), meta_buf);

    gtk_image_set_from_icon_name(GTK_IMAGE(img), "package-x-generic");
    if (obj->item->icon_url[0]) {
        load_icon_async(obj->item->icon_url, img, 64, 64);
    }

    g_signal_handlers_disconnect_by_func(btn, G_CALLBACK(on_catalog_install_clicked), NULL);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_catalog_install_clicked), obj);
}

static int g_selected_catalog_region = 0; /* 0: Tutte, 1: Europa, 2: America */
static GtkWidget *g_catalog_reg_dropdown = NULL;

static gboolean catalog_category_filter_func(gpointer item, gpointer user_data) {
    (void)user_data;
    if (!item) return FALSE;
    CatalogObject *obj = CATALOG_OBJECT(item);
    if (!obj || !obj->item) return FALSE;
    if (g_selected_category[0] && strcmp(g_selected_category, "Tutte") != 0 && strcmp(g_selected_category, "All") != 0) {
        if (strcasecmp(obj->item->category, g_selected_category) != 0) return FALSE;
    }
    if (g_selected_catalog_region == 1) { /* Europa */
        if (obj->item->region[0] && strcasecmp(obj->item->region, "Tutte") != 0 && strcasecmp(obj->item->region, "ALL") != 0) {
            if (strcasecmp(obj->item->region, "EUR") != 0 && strcasecmp(obj->item->region, "Europa") != 0 && !strstr(obj->item->region, "EUR"))
                return FALSE;
        }
    } else if (g_selected_catalog_region == 2) { /* America */
        if (obj->item->region[0] && strcasecmp(obj->item->region, "Tutte") != 0 && strcasecmp(obj->item->region, "ALL") != 0) {
            if (strcasecmp(obj->item->region, "USA") != 0 && strcasecmp(obj->item->region, "America") != 0 && !strstr(obj->item->region, "USA"))
                return FALSE;
        }
    }
    return TRUE;
}

static void on_catalog_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    GtkStringFilter *filter = GTK_STRING_FILTER(user_data);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    gtk_string_filter_set_search(filter, text);
}

static void on_catalog_cat_selected(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    guint pos = gtk_drop_down_get_selected(dropdown);
    GListModel *model = gtk_drop_down_get_model(dropdown);
    if (model && GTK_IS_STRING_LIST(model)) {
        const char *s = gtk_string_list_get_string(GTK_STRING_LIST(model), pos);
        if (s) strncpy(g_selected_category, s, sizeof(g_selected_category) - 1);
        else strncpy(g_selected_category, "Tutte", sizeof(g_selected_category) - 1);
    } else {
        strncpy(g_selected_category, "Tutte", sizeof(g_selected_category) - 1);
    }
    if (g_catalog_cat_filter) {
        gtk_filter_changed(GTK_FILTER(g_catalog_cat_filter), GTK_FILTER_CHANGE_DIFFERENT);
    }
}

static void on_catalog_reg_selected(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    guint pos = gtk_drop_down_get_selected(dropdown);
    g_selected_catalog_region = (int)pos;
    if (g_catalog_cat_filter) {
        gtk_filter_changed(GTK_FILTER(g_catalog_cat_filter), GTK_FILTER_CHANGE_DIFFERENT);
    }
}

/* Forward declarations */
static void on_back_to_downloads_clicked(GtkButton *btn, gpointer user_data);
static void on_sources_clicked(GtkButton *btn, gpointer user_data);
static void on_refresh_catalog_clicked(GtkButton *btn, gpointer user_data);
static void show_sources_dialog(GtkWindow *parent);

static GtkWidget *build_catalog_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "catalog-page");
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_vexpand(box, TRUE);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(toolbar, "catalog-toolbar");

    GtkWidget *btn_back = gtk_button_new_with_label("Torna ai Download");
    gtk_widget_add_css_class(btn_back, "btn-nav");
    gtk_widget_set_focusable(btn_back, FALSE);
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_to_downloads_clicked), NULL);

    g_catalog_search_entry = gtk_search_entry_new();
    gtk_widget_add_css_class(g_catalog_search_entry, "catalog-search");
    gtk_widget_set_hexpand(g_catalog_search_entry, TRUE);
    g_object_set(g_catalog_search_entry, "placeholder-text", "Cerca in Tools...", NULL);

    g_catalog_cat_dropdown = gtk_drop_down_new(NULL, NULL);
    gtk_widget_add_css_class(g_catalog_cat_dropdown, "catalog-dropdown");
    gtk_widget_set_focusable(g_catalog_cat_dropdown, FALSE);
    GtkStringList *init_cats = gtk_string_list_new(NULL);
    gtk_string_list_append(init_cats, "Tutte");
    gtk_drop_down_set_model(GTK_DROP_DOWN(g_catalog_cat_dropdown), G_LIST_MODEL(init_cats));
    g_object_unref(init_cats);
    g_signal_connect(g_catalog_cat_dropdown, "notify::selected-item", G_CALLBACK(on_catalog_cat_selected), NULL);

    GtkStringList *reg_list = gtk_string_list_new(NULL);
    gtk_string_list_append(reg_list, "Regione: Tutte");
    gtk_string_list_append(reg_list, "Europa (EUR)");
    gtk_string_list_append(reg_list, "America (USA)");
    g_catalog_reg_dropdown = gtk_drop_down_new(G_LIST_MODEL(reg_list), NULL);
    gtk_widget_add_css_class(g_catalog_reg_dropdown, "catalog-dropdown");
    gtk_widget_set_focusable(g_catalog_reg_dropdown, FALSE);
    g_object_unref(reg_list);
    g_signal_connect(g_catalog_reg_dropdown, "notify::selected", G_CALLBACK(on_catalog_reg_selected), NULL);

    GtkWidget *btn_sources = gtk_button_new_with_label("Sorgenti");
    gtk_widget_add_css_class(btn_sources, "btn-sources");
    gtk_widget_set_focusable(btn_sources, FALSE);
    g_signal_connect(btn_sources, "clicked", G_CALLBACK(on_sources_clicked), NULL);

    GtkWidget *btn_refresh = gtk_button_new_with_label("Aggiorna");
    gtk_widget_add_css_class(btn_refresh, "btn-refresh-cat");
    gtk_widget_set_focusable(btn_refresh, FALSE);
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_catalog_clicked), NULL);

    gtk_box_append(GTK_BOX(toolbar), btn_back);
    gtk_box_append(GTK_BOX(toolbar), g_catalog_search_entry);
    gtk_box_append(GTK_BOX(toolbar), g_catalog_cat_dropdown);
    gtk_box_append(GTK_BOX(toolbar), g_catalog_reg_dropdown);
    gtk_box_append(GTK_BOX(toolbar), btn_sources);
    gtk_box_append(GTK_BOX(toolbar), btn_refresh);

    gtk_box_append(GTK_BOX(box), toolbar);

    g_catalog_store = g_list_store_new(TYPE_CATALOG_OBJECT);

    GtkExpression *expr = gtk_property_expression_new(TYPE_CATALOG_OBJECT, NULL, "name");
    GtkStringFilter *str_filter = gtk_string_filter_new(expr);
    gtk_string_filter_set_ignore_case(str_filter, TRUE);
    gtk_string_filter_set_match_mode(str_filter, GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
    g_signal_connect(g_catalog_search_entry, "search-changed", G_CALLBACK(on_catalog_search_changed), str_filter);

    g_catalog_cat_filter = gtk_custom_filter_new(catalog_category_filter_func, NULL, NULL);

    GtkEveryFilter *every = gtk_every_filter_new();
    gtk_multi_filter_append(GTK_MULTI_FILTER(every), GTK_FILTER(str_filter));
    gtk_multi_filter_append(GTK_MULTI_FILTER(every), GTK_FILTER(g_catalog_cat_filter));

    GtkFilterListModel *flm = gtk_filter_list_model_new(G_LIST_MODEL(g_catalog_store), GTK_FILTER(every));
    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(flm));

    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(on_catalog_item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_catalog_item_bind), NULL);

    GtkWidget *list_view = gtk_list_view_new(GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory));
    gtk_widget_add_css_class(list_view, "catalog-list");

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list_view);

    gtk_box_append(GTK_BOX(box), scrolled);

    return box;
}

/* Sources dialog */
static GtkWidget *g_sources_dialog = NULL;
static GtkWidget *g_sources_list_box = NULL;
static GtkWidget *g_sources_entry = NULL;

static void on_remove_source_clicked(GtkButton *btn, gpointer user_data);

static void refresh_sources_list_ui(void) {
    if (!g_sources_list_box) return;
    GtkWidget *child = gtk_widget_get_first_child(g_sources_list_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(g_sources_list_box), child);
        child = next;
    }
    for (int i = 0; i < g_n_sources; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(row, "source-row");
        GtkWidget *lbl = gtk_label_new(g_catalog_sources[i]);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl, TRUE);

        GtkWidget *btn_del = gtk_button_new_with_label("Rimuovi");
        gtk_widget_add_css_class(btn_del, "btn-remove-source");
        g_object_set_data(G_OBJECT(btn_del), "src_idx", GINT_TO_POINTER(i));
        g_signal_connect(btn_del, "clicked", G_CALLBACK(on_remove_source_clicked), NULL);

        gtk_box_append(GTK_BOX(row), lbl);
        gtk_box_append(GTK_BOX(row), btn_del);
        gtk_list_box_append(GTK_LIST_BOX(g_sources_list_box), row);
    }
}

static void on_remove_source_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "src_idx"));
    if (idx >= 0 && idx < g_n_sources) {
        for (int i = idx; i < g_n_sources - 1; i++) {
            strncpy(g_catalog_sources[i], g_catalog_sources[i + 1], sizeof(g_catalog_sources[0]) - 1);
        }
        g_catalog_sources[g_n_sources - 1][0] = '\0';
        g_n_sources--;
        save_settings();
        refresh_sources_list_ui();
    }
}

static void on_add_source_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (!g_sources_entry) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(g_sources_entry));
    if (!text || !text[0]) return;

    char url[1024];
    strncpy(url, text, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    g_strstrip(url);

    if (!is_valid_feed_url(url)) return;
    if (g_n_sources >= 8) return;

    for (int i = 0; i < g_n_sources; i++) {
        if (strcmp(g_catalog_sources[i], url) == 0) return;
    }

    strncpy(g_catalog_sources[g_n_sources++], url, sizeof(g_catalog_sources[0]) - 1);
    save_settings();
    gtk_editable_set_text(GTK_EDITABLE(g_sources_entry), "");
    refresh_sources_list_ui();
}

static void on_refresh_catalog_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    save_settings();
    trigger_catalog_fetch_async();
    if (g_sources_dialog) {
        gtk_window_destroy(GTK_WINDOW(g_sources_dialog));
        g_sources_dialog = NULL;
    }
}

static void on_sources_dialog_response(GtkDialog *dialog, int response, gpointer user_data) {
    (void)response; (void)user_data;
    save_settings();
    g_sources_dialog = g_sources_list_box = g_sources_entry = NULL;
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void show_sources_dialog(GtkWindow *parent) {
    if (g_sources_dialog) {
        gtk_window_present(GTK_WINDOW(g_sources_dialog));
        return;
    }

    g_sources_dialog = gtk_dialog_new_with_buttons(
        "Sorgenti Feed Tools", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Chiudi", GTK_RESPONSE_CLOSE, NULL);

    gtk_widget_set_size_request(g_sources_dialog, 560, 420);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(g_sources_dialog));
    gtk_orientable_set_orientation(GTK_ORIENTABLE(content), GTK_ORIENTATION_VERTICAL);

    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(form, 20);
    gtk_widget_set_margin_end(form, 20);
    gtk_widget_set_margin_top(form, 16);
    gtk_widget_set_margin_bottom(form, 16);

    GtkWidget *lbl_add = gtk_label_new("Aggiungi Feed Tools (URL https):");
    gtk_label_set_xalign(GTK_LABEL(lbl_add), 0.0);
    gtk_widget_add_css_class(lbl_add, "form-label");

    GtkWidget *add_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_sources_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_sources_entry), "https://example.com/catalog.json");
    gtk_widget_add_css_class(g_sources_entry, "form-entry");
    gtk_widget_set_hexpand(g_sources_entry, TRUE);

    GtkWidget *btn_add_src = gtk_button_new_with_label("Aggiungi");
    gtk_widget_add_css_class(btn_add_src, "btn-primary");
    g_signal_connect(btn_add_src, "clicked", G_CALLBACK(on_add_source_clicked), NULL);

    gtk_box_append(GTK_BOX(add_box), g_sources_entry);
    gtk_box_append(GTK_BOX(add_box), btn_add_src);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 6);
    gtk_widget_set_margin_bottom(sep, 6);

    GtkWidget *lbl_list = gtk_label_new("Sorgenti Configurate:");
    gtk_label_set_xalign(GTK_LABEL(lbl_list), 0.0);
    gtk_widget_add_css_class(lbl_list, "form-label");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    g_sources_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_sources_list_box), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), g_sources_list_box);

    GtkWidget *btn_refresh_sources = gtk_button_new_with_label("Aggiorna Tools Adesso");
    gtk_widget_add_css_class(btn_refresh_sources, "btn-settings");
    gtk_widget_set_margin_top(btn_refresh_sources, 8);
    g_signal_connect(btn_refresh_sources, "clicked", G_CALLBACK(on_refresh_catalog_clicked), NULL);

    gtk_box_append(GTK_BOX(form), lbl_add);
    gtk_box_append(GTK_BOX(form), add_box);
    gtk_box_append(GTK_BOX(form), sep);
    gtk_box_append(GTK_BOX(form), lbl_list);
    gtk_box_append(GTK_BOX(form), scroll);
    gtk_box_append(GTK_BOX(form), btn_refresh_sources);

    gtk_box_append(GTK_BOX(content), form);

    refresh_sources_list_ui();

    g_signal_connect(g_sources_dialog, "response", G_CALLBACK(on_sources_dialog_response), NULL);
    gtk_window_present(GTK_WINDOW(g_sources_dialog));
}

static void on_sources_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    show_sources_dialog(GTK_WINDOW(g_window));
}

static void on_catalog_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (g_stack) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "catalog");
        guint n_items = g_catalog_store ? g_list_model_get_n_items(G_LIST_MODEL(g_catalog_store)) : 0;
        if (n_items == 0) {
            trigger_catalog_fetch_async();
        }
    }
}

static void on_back_to_downloads_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    pthread_mutex_lock(&dl_mutex);
    int total = n_downloads;
    pthread_mutex_unlock(&dl_mutex);
    if (g_stack) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), total == 0 ? "empty" : "list");
    }
}

/* ═══════════════════════════════════════════════════════════
 * DLPSGame Live PS4 Catalog Module (6,500+ Games & Hosters)
 * ═══════════════════════════════════════════════════════════ */

#define DLPS_MAX_LINKS 16

typedef enum {
    DLPS_LINK_UNKNOWN = 0,
    DLPS_LINK_DIRECT_PKG,
    DLPS_LINK_MEDIAFIRE,
    DLPS_LINK_HOSTER,
    DLPS_LINK_PROTECTOR
} DlpsLinkType;

typedef struct {
    char name[256];
    char url[2048];
    char hoster[128];
    DlpsLinkType type;
} DlpsLink;

typedef struct {
    gint64 id;
    char title[256];
    char post_url[1024];
    char cover_url[2048];
    char shot_urls[2][1024];
    char cusa[32];
    char platform[32];
    char region[32];
    char genre[64];
    char year[16];
    char password[128];
    char description[1024];
    DlpsLink links[DLPS_MAX_LINKS];
    int n_links;
} DlpsGame;

#define TYPE_DLPS_GAME_OBJECT (dlps_game_object_get_type())
G_DECLARE_FINAL_TYPE(DlpsGameObject, dlps_game_object, DLPS, GAME_OBJECT, GObject)

struct _DlpsGameObject {
    GObject parent_instance;
    DlpsGame *game;
};

G_DEFINE_TYPE(DlpsGameObject, dlps_game_object, G_TYPE_OBJECT)

static void dlps_game_object_init(DlpsGameObject *self) {
    self->game = NULL;
}

static void dlps_game_object_finalize(GObject *object) {
    DlpsGameObject *self = DLPS_GAME_OBJECT(object);
    if (self->game) {
        g_free(self->game);
        self->game = NULL;
    }
    G_OBJECT_CLASS(dlps_game_object_parent_class)->finalize(object);
}

static void dlps_game_object_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    DlpsGameObject *self = DLPS_GAME_OBJECT(object);
    if (prop_id == 1) {
        g_value_set_string(value, (self->game && self->game->title[0]) ? self->game->title : "");
    } else {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void dlps_game_object_class_init(DlpsGameObjectClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->get_property = dlps_game_object_get_property;
    gobject_class->finalize = dlps_game_object_finalize;
    g_object_class_install_property(gobject_class, 1,
        g_param_spec_string("title", "Title", "Title", "", G_PARAM_READABLE));
}

static DlpsGameObject *dlps_game_object_new(const DlpsGame *game) {
    DlpsGameObject *obj = g_object_new(TYPE_DLPS_GAME_OBJECT, NULL);
    if (game) {
        obj->game = g_new0(DlpsGame, 1);
        *obj->game = *game;
    }
    return obj;
}

/* DLPS State Globals */
static GListStore *g_dlps_store = NULL;
static int g_dlps_current_page = 1;
static int g_dlps_total_pages = 1;
static int g_dlps_total_items = 0;
static char g_dlps_search_query[256] = "";
static int g_dlps_region_filter = 0; /* 0: Tutte, 1: Europa (EUR), 2: America (USA), 3: Giappone / Asia */
static int g_dlps_console_filter = 0; /* 0: Tutte, 1: PS5, 2: PS4, 3: PS3, 4: PS2 */
static gboolean g_dlps_is_loading = FALSE;
static GtkWidget *g_dlps_spinner = NULL;
static GtkWidget *g_dlps_lbl_page = NULL;
static GtkWidget *g_dlps_btn_prev = NULL;
static GtkWidget *g_dlps_btn_next = NULL;
static GtkWidget *g_dlps_region_dropdown = NULL;
static GtkWidget *g_dlps_console_dropdown = NULL;

static void unescape_html(char *str) {
    if (!str) return;
    char *r = str, *w = str;
    while (*r) {
        if (*r == '&') {
            if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
            else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
            else if (strncmp(r, "&apos;", 6) == 0 || strncmp(r, "&#039;", 6) == 0) { *w++ = '\''; r += 6; }
            else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
            else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
            else if (strncmp(r, "&#8211;", 7) == 0) { *w++ = '-'; r += 7; }
            else if (strncmp(r, "&#8212;", 7) == 0) { *w++ = '-'; r += 7; }
            else if (strncmp(r, "&#8216;", 7) == 0 || strncmp(r, "&#8217;", 7) == 0) { *w++ = '\''; r += 7; }
            else if (strncmp(r, "&#8220;", 7) == 0 || strncmp(r, "&#8221;", 7) == 0) { *w++ = '"'; r += 7; }
            else if (strncmp(r, "&#091;", 6) == 0) { *w++ = '['; r += 6; }
            else if (strncmp(r, "&#093;", 6) == 0) { *w++ = ']'; r += 6; }
            else if (strncmp(r, "&nbsp;", 6) == 0) { *w++ = ' '; r += 6; }
            else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void strip_html_tags(char *str) {
    if (!str) return;
    char *r = str, *w = str;
    gboolean in_tag = FALSE;
    while (*r) {
        if (*r == '<') in_tag = TRUE;
        else if (*r == '>') in_tag = FALSE;
        else if (!in_tag) *w++ = *r;
        r++;
    }
    *w = '\0';
}

static DlpsLinkType detect_dlps_link_type(const char *url) {
    if (!url || !url[0]) return DLPS_LINK_UNKNOWN;
    if (strstr(url, "downloadgameps3.net") || strstr(url, "filecrypt.cc"))
        return DLPS_LINK_PROTECTOR;
    if (strstr(url, "mediafire.com/file/"))
        return DLPS_LINK_MEDIAFIRE;
    if (strstr(url, ".pkg") || strstr(url, ".PKG") || strstr(url, ".pkg?") || strstr(url, ".PKG?"))
        return DLPS_LINK_DIRECT_PKG;
    if (strstr(url, "1fichier.com") || strstr(url, "akirabox.com") ||
        strstr(url, "vikingfile.com") || strstr(url, "rootz.so") ||
        strstr(url, "mega.nz") || strstr(url, "rapidgator.net"))
        return DLPS_LINK_HOSTER;
    return DLPS_LINK_HOSTER;
}

static void extract_dlps_links(const char *html_text, DlpsGame *g) {
    if (!html_text || !g) return;
    g->n_links = 0;

    GRegex *p_re = g_regex_new("<p[^>]*>(.*?)</p>", G_REGEX_DOTALL | G_REGEX_CASELESS, 0, NULL);
    GRegex *a_re = g_regex_new("<a[^>]+href=[\"']([^\"']+)[\"'][^>]*>(.*?)</a>", G_REGEX_DOTALL | G_REGEX_CASELESS, 0, NULL);
    GMatchInfo *p_info = NULL;

    if (g_regex_match(p_re, html_text, 0, &p_info)) {
        while (g_match_info_matches(p_info) && g->n_links < DLPS_MAX_LINKS) {
            char *para = g_match_info_fetch(p_info, 1);
            if (para && strstr(para, "<a")) {
                char *first_a = strstr(para, "<a");
                char prefix[160] = "";
                if (first_a && first_a > para) {
                    size_t plen = first_a - para;
                    if (plen >= sizeof(prefix)) plen = sizeof(prefix) - 1;
                    strncpy(prefix, para, plen);
                    prefix[plen] = '\0';
                    strip_html_tags(prefix);
                    unescape_html(prefix);
                    g_strstrip(prefix);
                    size_t l = strlen(prefix);
                    while (l > 0 && (prefix[l-1] == ':' || prefix[l-1] == '-' || prefix[l-1] == ' ')) {
                        prefix[--l] = '\0';
                    }
                    g_strstrip(prefix);
                }

                if (g_ascii_strncasecmp(prefix, "guide", 5) == 0 ||
                    g_ascii_strncasecmp(prefix, "note", 4) == 0) {
                    prefix[0] = '\0';
                }

                GMatchInfo *a_info = NULL;
                if (g_regex_match(a_re, para, 0, &a_info)) {
                    while (g_match_info_matches(a_info) && g->n_links < DLPS_MAX_LINKS) {
                        char *href = g_match_info_fetch(a_info, 1);
                        char *text = g_match_info_fetch(a_info, 2);

                        if (href && text && strncmp(href, "http", 4) == 0 &&
                            !strstr(href, "guide-download")) {
                            char clean_text[128];
                            strncpy(clean_text, text, sizeof(clean_text) - 1);
                            clean_text[sizeof(clean_text) - 1] = '\0';
                            strip_html_tags(clean_text);
                            unescape_html(clean_text);
                            g_strstrip(clean_text);

                            if (g_ascii_strcasecmp(clean_text, "here") != 0 &&
                                g_ascii_strcasecmp(clean_text, "guide download") != 0) {

                                DlpsLinkType ltype = detect_dlps_link_type(href);
                                const char *hoster = clean_text;
                                if (strstr(href, "mediafire.com") && !strcasestr(clean_text, "mediafire")) {
                                    hoster = "Mediafire";
                                } else if (strstr(href, "1fichier.com") && !strcasestr(clean_text, "1fichier") && !strcasestr(clean_text, "1file")) {
                                    hoster = "1fichier";
                                } else if (strstr(href, "akirabox.com") && !strcasestr(clean_text, "akia")) {
                                    hoster = "Akia";
                                } else if (strstr(href, "rootz.so") && !strcasestr(clean_text, "rootz")) {
                                    hoster = "Rootz";
                                } else if (strstr(href, "downloadgameps3.net")) {
                                    hoster = clean_text[0] ? clean_text : "DownloadGamePS3";
                                }

                                char final_name[256];
                                if (prefix[0] && g_ascii_strcasecmp(prefix, hoster) != 0) {
                                    snprintf(final_name, sizeof(final_name), "%s [%s]", prefix, hoster);
                                } else {
                                    snprintf(final_name, sizeof(final_name), "%s", hoster[0] ? hoster : "Download Link");
                                }

                                strncpy(g->links[g->n_links].name, final_name, sizeof(g->links[0].name) - 1);
                                strncpy(g->links[g->n_links].url, href, sizeof(g->links[0].url) - 1);
                                strncpy(g->links[g->n_links].hoster, hoster, sizeof(g->links[0].hoster) - 1);
                                g->links[g->n_links].type = ltype;
                                g->n_links++;
                            }
                        }
                        g_free(href);
                        g_free(text);
                        g_match_info_next(a_info, NULL);
                    }
                    g_match_info_free(a_info);
                }
            }
            g_free(para);
            g_match_info_next(p_info, NULL);
        }
    }
    g_match_info_free(p_info);
    g_regex_unref(p_re);

    /* Fallback if no <p> was found but <a> exists */
    if (g->n_links == 0) {
        GMatchInfo *a_info2 = NULL;
        if (g_regex_match(a_re, html_text, 0, &a_info2)) {
            while (g_match_info_matches(a_info2) && g->n_links < DLPS_MAX_LINKS) {
                char *href = g_match_info_fetch(a_info2, 1);
                char *text = g_match_info_fetch(a_info2, 2);
                if (href && text && strncmp(href, "http", 4) == 0 && !strstr(href, "guide-download")) {
                    char clean_text[128];
                    strncpy(clean_text, text, sizeof(clean_text) - 1);
                    clean_text[sizeof(clean_text) - 1] = '\0';
                    strip_html_tags(clean_text);
                    unescape_html(clean_text);
                    g_strstrip(clean_text);
                    if (g_ascii_strcasecmp(clean_text, "here") != 0 &&
                        g_ascii_strcasecmp(clean_text, "guide download") != 0) {
                        strncpy(g->links[g->n_links].name, clean_text[0] ? clean_text : "Download Link", sizeof(g->links[0].name) - 1);
                        strncpy(g->links[g->n_links].url, href, sizeof(g->links[0].url) - 1);
                        strncpy(g->links[g->n_links].hoster, clean_text, sizeof(g->links[0].hoster) - 1);
                        g->links[g->n_links].type = detect_dlps_link_type(href);
                        g->n_links++;
                    }
                }
                g_free(href);
                g_free(text);
                g_match_info_next(a_info2, NULL);
            }
            g_match_info_free(a_info2);
        }
    }
    g_regex_unref(a_re);
}

static void extract_dlps_region(const char *decoded, const char *html_content, DlpsGame *g) {
    if (!g) return;
    g->region[0] = '\0';
    GRegex *reg_re = g_regex_new("(?:CUSA|PPSA|NPUB|NPEB|BLUS|BLES|NPUA|NPHA|ULUS|ULES|UCUS)\\d{5}\\s*(?:&#8211;|-|–|\\s)*\\s*(USA|EUR|EUROPE|AMERICA|ASIA|JAPAN|JPN)", G_REGEX_CASELESS, 0, NULL);
    GMatchInfo *reg_info = NULL;
    gboolean matched = FALSE;
    if (decoded && g_regex_match(reg_re, decoded, 0, &reg_info)) {
        matched = TRUE;
    } else if (!decoded && html_content && g_regex_match(reg_re, html_content, 0, &reg_info)) {
        matched = TRUE;
    }
    if (matched) {
        char *r = g_match_info_fetch(reg_info, 1);
        if (r) {
            if (g_ascii_strcasecmp(r, "USA") == 0 || g_ascii_strcasecmp(r, "AMERICA") == 0) {
                strncpy(g->region, "USA", sizeof(g->region) - 1);
            } else if (g_ascii_strcasecmp(r, "EUR") == 0 || g_ascii_strcasecmp(r, "EUROPE") == 0) {
                strncpy(g->region, "EUR", sizeof(g->region) - 1);
            } else if (g_ascii_strcasecmp(r, "JPN") == 0 || g_ascii_strcasecmp(r, "JAPAN") == 0) {
                strncpy(g->region, "JPN", sizeof(g->region) - 1);
            } else if (g_ascii_strcasecmp(r, "ASIA") == 0) {
                strncpy(g->region, "ASIA", sizeof(g->region) - 1);
            }
            g_free(r);
        }
    }
    g_match_info_free(reg_info);
    g_regex_unref(reg_re);

    /* Heuristic / fallback */
    if (!g->region[0]) {
        const char *d = decoded ? decoded : (html_content ? html_content : "");
        if (strstr(d, "USA") || (!decoded && strstr(g->title, "USA")) ||
            strncmp(g->cusa, "BLUS", 4) == 0 || strncmp(g->cusa, "NPUB", 4) == 0 || strncmp(g->cusa, "ULUS", 4) == 0) {
            strncpy(g->region, "USA", sizeof(g->region) - 1);
        } else if (strstr(d, "EUR") || (!decoded && strstr(g->title, "EUR")) ||
                   strncmp(g->cusa, "BLES", 4) == 0 || strncmp(g->cusa, "NPEB", 4) == 0 || strncmp(g->cusa, "ULES", 4) == 0) {
            strncpy(g->region, "EUR", sizeof(g->region) - 1);
        } else if (strstr(d, "JPN") || (!decoded && strstr(g->title, "JPN")) ||
                   strncmp(g->cusa, "BLJM", 4) == 0 || strncmp(g->cusa, "NPJB", 4) == 0) {
            strncpy(g->region, "JPN", sizeof(g->region) - 1);
        } else if (strstr(d, "ASIA") || (!decoded && strstr(g->title, "ASIA"))) {
            strncpy(g->region, "ASIA", sizeof(g->region) - 1);
        }
    }
}

static int parse_dlps_games_json(const char *json_data, gsize json_len, DlpsGame *out_games,
                                 int max_games, int region_filter, gboolean is_search,
                                 char *status_override, size_t status_override_size) {
    if (!json_data || json_len == 0 || max_games <= 0) return 0;
    if (status_override && status_override_size > 0) status_override[0] = '\0';
    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    int count = 0;
    int max_cands = max_games * 4;
    DlpsGame *all_candidates = calloc(max_cands, sizeof(DlpsGame));
    int cand_count = 0;

    if (json_parser_load_from_data(parser, json_data, json_len, &err)) {
        JsonNode *root = json_parser_get_root(parser);
        JsonArray *arr = NULL;
        gboolean free_arr = FALSE;
        if (root && JSON_NODE_HOLDS_ARRAY(root)) {
            arr = json_node_get_array(root);
        } else if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            arr = json_array_new();
            json_array_add_element(arr, json_node_copy(root));
            free_arr = TRUE;
        }

        if (arr) {
            guint len = json_array_get_length(arr);
            for (guint i = 0; i < len && cand_count < max_cands; i++) {
                JsonNode *node = json_array_get_element(arr, i);
                if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
                JsonObject *obj = json_node_get_object(node);
                DlpsGame g;
                memset(&g, 0, sizeof(g));

                if (json_object_has_member(obj, "id"))
                    g.id = json_object_get_int_member(obj, "id");
                if (json_object_has_member(obj, "link"))
                    strncpy(g.post_url, json_object_get_string_member(obj, "link"), sizeof(g.post_url) - 1);

                if (json_object_has_member(obj, "title")) {
                    JsonObject *to = json_object_get_object_member(obj, "title");
                    if (to && json_object_has_member(to, "rendered")) {
                        strncpy(g.title, json_object_get_string_member(to, "rendered"), sizeof(g.title) - 1);
                        unescape_html(g.title);
                    }
                }

                const char *html_content = "";
                if (json_object_has_member(obj, "content")) {
                    JsonObject *co = json_object_get_object_member(obj, "content");
                    if (co && json_object_has_member(co, "rendered"))
                        html_content = json_object_get_string_member(co, "rendered");
                }

                /* 1. Extract cover image: first <img src="..."> */
                const char *img_tag = strstr(html_content, "<img");
                if (img_tag) {
                    const char *src_attr = strstr(img_tag, "src=\"");
                    if (!src_attr) src_attr = strstr(img_tag, "src='");
                    if (src_attr) {
                        char quote = src_attr[4];
                        src_attr += 5;
                        const char *src_end = strchr(src_attr, quote);
                        if (src_end && (src_end - src_attr < (int)sizeof(g.cover_url))) {
                            strncpy(g.cover_url, src_attr, src_end - src_attr);
                            g.cover_url[src_end - src_attr] = '\0';
                        }
                    }
                    /* Additional screenshots (large images following the cover) */
                    int n_shots = 0;
                    const char *scan = img_tag + 4;
                    while (n_shots < 2 && (scan = strstr(scan, "<img")) != NULL) {
                        const char *s_attr = strstr(scan, "src=\"");
                        const char *tag_end = strchr(scan, '>');
                        if (s_attr && tag_end && s_attr < tag_end) {
                            s_attr += 5;
                            const char *s_end = strchr(s_attr, '"');
                            if (s_end && (s_end - s_attr) < (int)sizeof(g.shot_urls[0]) &&
                                strncmp(s_attr, g.cover_url, s_end - s_attr) != 0 &&
                                (strstr(s_attr, "/s1600/") && strstr(s_attr, "/s1600/") < s_end)) {
                                strncpy(g.shot_urls[n_shots], s_attr, s_end - s_attr);
                                g.shot_urls[n_shots][s_end - s_attr] = '\0';
                                n_shots++;
                            }
                        }
                        scan += 4;
                    }
                }

                /* 2. Extract Title ID (PS5 PPSA, PS4 CUSA, PS3/PSP NPUB/BLES etc.) */
                GRegex *cusa_re = g_regex_new("(CUSA|PPSA|NPUB|NPEB|BLUS|BLES|NPUA|NPHA|ULUS|ULES|UCUS)\\d{5}", G_REGEX_CASELESS, 0, NULL);
                GMatchInfo *minfo = NULL;
                if (g_regex_match(cusa_re, html_content, 0, &minfo)) {
                    char *c = g_match_info_fetch(minfo, 0);
                    if (c) {
                        strncpy(g.cusa, c, sizeof(g.cusa) - 1);
                        g_free(c);
                    }
                }
                g_match_info_free(minfo);
                g_regex_unref(cusa_re);

                /* Rilevamento piattaforma console da categorie WP, Title ID o titolo */
                if (json_object_has_member(obj, "categories")) {
                    JsonArray *cats = json_object_get_array_member(obj, "categories");
                    guint clen = json_array_get_length(cats);
                    for (guint cidx = 0; cidx < clen; cidx++) {
                        gint64 cid = json_array_get_int_element(cats, cidx);
                        if (cid == 63019) { strncpy(g.platform, "PS5", sizeof(g.platform) - 1); break; }
                        else if (cid == 4370) { strncpy(g.platform, "PS4", sizeof(g.platform) - 1); break; }
                        else if (cid == 64) { strncpy(g.platform, "PS3", sizeof(g.platform) - 1); break; }
                        else if (cid == 5917) { strncpy(g.platform, "PS2", sizeof(g.platform) - 1); break; }
                        else if (cid == 110) { strncpy(g.platform, "PSN", sizeof(g.platform) - 1); break; }
                        else if (cid == 63) { strncpy(g.platform, "PSP", sizeof(g.platform) - 1); break; }
                        else if (cid == 2297) { strncpy(g.platform, "PS Vita", sizeof(g.platform) - 1); break; }
                    }
                }
                if (!g.platform[0]) {
                    if (strncmp(g.cusa, "PPSA", 4) == 0) strncpy(g.platform, "PS5", sizeof(g.platform) - 1);
                    else if (strncmp(g.cusa, "CUSA", 4) == 0) strncpy(g.platform, "PS4", sizeof(g.platform) - 1);
                    else if (strncmp(g.cusa, "BLUS", 4) == 0 || strncmp(g.cusa, "BLES", 4) == 0 ||
                             strncmp(g.cusa, "NPUB", 4) == 0 || strncmp(g.cusa, "NPEB", 4) == 0)
                        strncpy(g.platform, "PS3", sizeof(g.platform) - 1);
                    else if (strncmp(g.cusa, "SLUS", 4) == 0 || strncmp(g.cusa, "SLES", 4) == 0 ||
                             strncmp(g.cusa, "SCES", 4) == 0)
                        strncpy(g.platform, "PS2", sizeof(g.platform) - 1);
                    else if (strncmp(g.cusa, "ULUS", 4) == 0 || strncmp(g.cusa, "ULES", 4) == 0 ||
                             strncmp(g.cusa, "UCUS", 4) == 0)
                        strncpy(g.platform, "PSP", sizeof(g.platform) - 1);
                    else if (strncmp(g.cusa, "PCSF", 4) == 0 || strncmp(g.cusa, "PCSB", 4) == 0 ||
                             strncmp(g.cusa, "PCSE", 4) == 0)
                        strncpy(g.platform, "PS Vita", sizeof(g.platform) - 1);
                    else if (strstr(g.post_url, "downloadgamepsp.org")) {
                        strncpy(g.platform, strstr(g.post_url, "psvita") ? "PS Vita" : "PSP", sizeof(g.platform) - 1);
                    } else if (strstr(g.post_url, "nswgame.com") || strstr(g.post_url, "nswgf.com")) {
                        strncpy(g.platform, "Switch", sizeof(g.platform) - 1);
                    } else if (strstr(g.post_url, "dlxbgame.com")) {
                        strncpy(g.platform, "Xbox", sizeof(g.platform) - 1);
                    } else if (strstr(g.post_url, "gamepciso.com")) {
                        strncpy(g.platform, "PC", sizeof(g.platform) - 1);
                    } else if (strstr(g.title, "PS5") || strstr(html_content, "PS5")) strncpy(g.platform, "PS5", sizeof(g.platform) - 1);
                    else if (strstr(g.title, "PS4") || strstr(html_content, "PS4")) strncpy(g.platform, "PS4", sizeof(g.platform) - 1);
                    else if (strstr(g.title, "PS3") || strstr(html_content, "PS3")) strncpy(g.platform, "PS3", sizeof(g.platform) - 1);
                    else if (strstr(g.title, "PS2") || strstr(html_content, "PS2")) strncpy(g.platform, "PS2", sizeof(g.platform) - 1);
                    else if (strstr(g.title, "PS Vita") || strstr(g.title, "PSVita")) strncpy(g.platform, "PS Vita", sizeof(g.platform) - 1);
                    else if (strstr(g.title, "PSP")) strncpy(g.platform, "PSP", sizeof(g.platform) - 1);
                    else if (strstr(g.title, "PS1") || strstr(g.title, "PSX")) strncpy(g.platform, "PS1", sizeof(g.platform) - 1);
                }

                /* 3. Extract GENRE and RELEASE year from table */
                GRegex *genre_re = g_regex_new("GENRE.*?<td[^>]*>.*?<span[^>]*>([^<]+)</span>", G_REGEX_DOTALL | G_REGEX_CASELESS, 0, NULL);
                if (g_regex_match(genre_re, html_content, 0, &minfo)) {
                    char *val = g_match_info_fetch(minfo, 1);
                    if (val) {
                        strncpy(g.genre, g_strstrip(val), sizeof(g.genre) - 1);
                        g_free(val);
                    }
                }
                g_match_info_free(minfo);
                g_regex_unref(genre_re);

                GRegex *year_re = g_regex_new("RELEASE.*?<td[^>]*>.*?<span[^>]*>([^<]+)</span>", G_REGEX_DOTALL | G_REGEX_CASELESS, 0, NULL);
                if (g_regex_match(year_re, html_content, 0, &minfo)) {
                    char *val = g_match_info_fetch(minfo, 1);
                    if (val) {
                        strncpy(g.year, g_strstrip(val), sizeof(g.year) - 1);
                        g_free(val);
                    }
                }
                g_match_info_free(minfo);
                g_regex_unref(year_re);

                /* 4. Extract data-payload base64 (may contain multiple regions, e.g. USA + EUR) */
                GRegex *pl_re = g_regex_new("data-payload=[\"']([^\"']+)[\"']", 0, 0, NULL);
                GMatchInfo *pl_info = NULL;
                int payload_count = 0;
                DlpsGame payload_games[4];
                memset(payload_games, 0, sizeof(payload_games));

                if (g_regex_match(pl_re, html_content, 0, &pl_info)) {
                    while (g_match_info_matches(pl_info) && payload_count < 4) {
                        char *b64 = g_match_info_fetch(pl_info, 1);
                        if (b64) {
                            gsize out_len = 0;
                            guchar *raw = g_base64_decode(b64, &out_len);
                            if (raw) {
                                char *decoded = g_strndup((char *)raw, out_len);
                                DlpsGame pg = g; /* copy common post metadata */

                                /* Find password (handles Password, Pasword, etc.) */
                                GRegex *pwd_re = g_regex_new("Pass?w(?:or|0r)d?\\s*:\\s*([^\\s<]+)", G_REGEX_CASELESS, 0, NULL);
                                GMatchInfo *pwd_info = NULL;
                                if (g_regex_match(pwd_re, decoded, 0, &pwd_info)) {
                                    char *p = g_match_info_fetch(pwd_info, 1);
                                    if (p) {
                                        strncpy(pg.password, g_strstrip(p), sizeof(pg.password) - 1);
                                        g_free(p);
                                    }
                                }
                                g_match_info_free(pwd_info);
                                g_regex_unref(pwd_re);

                                /* Title ID from this payload */
                                GRegex *cusa_re2 = g_regex_new("(CUSA|PPSA|NPUB|NPEB|BLUS|BLES|NPUA|NPHA|ULUS|ULES|UCUS)\\d{5}", G_REGEX_CASELESS, 0, NULL);
                                GMatchInfo *cusa_info2 = NULL;
                                if (g_regex_match(cusa_re2, decoded, 0, &cusa_info2)) {
                                    char *c = g_match_info_fetch(cusa_info2, 0);
                                    if (c) {
                                        strncpy(pg.cusa, c, sizeof(pg.cusa) - 1);
                                        g_free(c);
                                    }
                                }
                                g_match_info_free(cusa_info2);
                                g_regex_unref(cusa_re2);

                                extract_dlps_region(decoded, html_content, &pg);
                                extract_dlps_links(decoded, &pg);

                                payload_games[payload_count++] = pg;

                                g_free(decoded);
                                g_free(raw);
                            }
                            g_free(b64);
                        }
                        g_match_info_next(pl_info, NULL);
                    }
                }
                g_match_info_free(pl_info);
                g_regex_unref(pl_re);

                if (payload_count == 0) {
                    DlpsGame pg = g;
                    extract_dlps_region(NULL, html_content, &pg);
                    if (pg.n_links == 0 && pg.post_url[0]) {
                        strncpy(pg.links[0].name, "Pagina DLPSGame", sizeof(pg.links[0].name) - 1);
                        strncpy(pg.links[0].url, pg.post_url, sizeof(pg.links[0].url) - 1);
                        pg.n_links = 1;
                    }
                    payload_games[payload_count++] = pg;
                }

                for (int p_idx = 0; p_idx < payload_count && cand_count < max_cands; p_idx++) {
                    all_candidates[cand_count++] = payload_games[p_idx];
                }
            }
            if (free_arr) json_array_unref(arr);
        }
    } else {
        if (err) g_error_free(err);
    }
    g_object_unref(parser);

    /* Pass 1: Filter by region_filter */
    if (region_filter > 0) {
        for (int i = 0; i < cand_count && count < max_games; i++) {
            DlpsGame *pg = &all_candidates[i];
            gboolean match = FALSE;
            if (region_filter == 1 && (strcmp(pg->region, "EUR") == 0 || strcmp(pg->region, "EUROPE") == 0)) {
                match = TRUE;
            } else if (region_filter == 2 && (strcmp(pg->region, "USA") == 0 || strcmp(pg->region, "AMERICA") == 0)) {
                match = TRUE;
            } else if (region_filter == 3 && (strcmp(pg->region, "JPN") == 0 || strcmp(pg->region, "JAPAN") == 0 || strcmp(pg->region, "ASIA") == 0)) {
                match = TRUE;
            }

            if (match) {
                if (pg->region[0] && !strstr(pg->title, pg->region)) {
                    char clarified[256];
                    snprintf(clarified, sizeof(clarified), "%.230s [%s]", pg->title, pg->region);
                    strncpy(pg->title, clarified, sizeof(pg->title) - 1);
                }
                out_games[count++] = *pg;
            }
        }
    } else {
        for (int i = 0; i < cand_count && count < max_games; i++) {
            DlpsGame *pg = &all_candidates[i];
            if (pg->region[0] && !strstr(pg->title, pg->region)) {
                char clarified[256];
                snprintf(clarified, sizeof(clarified), "%.230s [%s]", pg->title, pg->region);
                strncpy(pg->title, clarified, sizeof(pg->title) - 1);
            }
            out_games[count++] = *pg;
        }
    }

    /* Fallback: if user performed a specific search and 0 games matched region filter,
     * but candidate games were found for other regions (e.g. A Day Without Me [USA]):
     * do NOT return empty! Show the games from other available regions so the user is not left in the dark.
     */
    if (count == 0 && cand_count > 0 && is_search) {
        for (int i = 0; i < cand_count && count < max_games; i++) {
            DlpsGame *pg = &all_candidates[i];
            if (pg->region[0] && !strstr(pg->title, pg->region)) {
                char clarified[256];
                snprintf(clarified, sizeof(clarified), "%.230s [%s]", pg->title, pg->region);
                strncpy(pg->title, clarified, sizeof(pg->title) - 1);
            }
            out_games[count++] = *pg;
        }
        if (status_override && status_override_size > 0 && count > 0) {
            snprintf(status_override, status_override_size,
                     "Disponibile per %s",
                     out_games[0].region[0] ? out_games[0].region : "altra regione");
        }
    }

    free(all_candidates);
    return count;
}

typedef struct {
    char *data;
    size_t size;
} DlpsMem;

static size_t dlps_mem_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    DlpsMem *mem = (DlpsMem *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

static size_t dlps_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    int *totals = (int *)userdata;
    if (strncasecmp(buffer, "x-wp-total:", 11) == 0) {
        totals[0] = atoi(buffer + 11);
    } else if (strncasecmp(buffer, "x-wp-totalpages:", 16) == 0) {
        totals[1] = atoi(buffer + 16);
    }
    return total;
}

typedef struct {
    DlpsGame *games;
    int count;
    int total_items;
    int total_pages;
    int page;
    guint req_id;
    char error_msg[256];
    char status_override[256];
} DlpsFetchResult;

typedef struct {
    char query[256];
    int region;
    int console;
    int page;
    int per_page;
    guint req_id;
} DlpsFetchReq;

static guint g_dlps_active_req_id = 0;
static void trigger_dlps_fetch(const char *query, int page);

static gboolean on_dlps_fetch_done(gpointer user_data) {
    DlpsFetchResult *res = (DlpsFetchResult *)user_data;

    /* Stale request check: discard if user made another request while this was loading */
    if (res->req_id != g_dlps_active_req_id) {
        if (res->games) free(res->games);
        free(res);
        return G_SOURCE_REMOVE;
    }

    g_dlps_is_loading = FALSE;
    if (g_dlps_spinner) gtk_spinner_stop(GTK_SPINNER(g_dlps_spinner));

    if (!g_dlps_store) {
        if (res->games) free(res->games);
        free(res);
        return G_SOURCE_REMOVE;
    }

    g_dlps_current_page = res->page;
    g_dlps_total_pages = res->total_pages > 0 ? res->total_pages : 1;
    g_dlps_total_items = (res->total_items > 0) ? res->total_items : res->count;

    g_list_store_remove_all(g_dlps_store);
    for (int i = 0; i < res->count; i++) {
        DlpsGameObject *obj = dlps_game_object_new(&res->games[i]);
        g_list_store_append(g_dlps_store, G_OBJECT(obj));
        g_object_unref(obj);
    }

    if (g_dlps_lbl_page) {
        char pbuf[512];
        if (res->status_override[0]) {
            snprintf(pbuf, sizeof(pbuf), "Pagina %d di %d  •  %d %s  •  %s",
                     g_dlps_current_page, g_dlps_total_pages,
                     g_dlps_total_items,
                     g_dlps_total_items == 1 ? "gioco" : "giochi",
                     res->status_override);
        } else if (res->total_items > 0 || res->count > 0) {
            const char *cons_suffix = "";
            switch (g_dlps_console_filter) {
                case 1: cons_suffix = "  •  PS5"; break;
                case 2: cons_suffix = "  •  PS4"; break;
                case 3: cons_suffix = "  •  PS3"; break;
                case 4: cons_suffix = "  •  PS2"; break;
                case 5: cons_suffix = "  •  PS1/PSX"; break;
                case 6: cons_suffix = "  •  PSP"; break;
                case 7: cons_suffix = "  •  PS Vita"; break;
                case 8: cons_suffix = "  •  PSN"; break;
                case 9: cons_suffix = "  •  Switch"; break;
                case 10: cons_suffix = "  •  Xbox"; break;
                case 11: cons_suffix = "  •  PC"; break;
                default: break;
            }

            const char *reg_suffix = "";
            if (g_dlps_region_filter == 1) reg_suffix = "  •  Europa";
            else if (g_dlps_region_filter == 2) reg_suffix = "  •  America";
            else if (g_dlps_region_filter == 3) reg_suffix = "  •  Giappone/Asia";
            snprintf(pbuf, sizeof(pbuf), "Pagina %d di %d  •  %d %s%s%s",
                     g_dlps_current_page, g_dlps_total_pages,
                     g_dlps_total_items,
                     g_dlps_total_items == 1 ? "gioco" : "giochi",
                     cons_suffix, reg_suffix);
        } else if (res->error_msg[0]) {
            snprintf(pbuf, sizeof(pbuf), "%s", res->error_msg);
        } else {
            snprintf(pbuf, sizeof(pbuf), "Nessun gioco trovato");
        }
        gtk_label_set_text(GTK_LABEL(g_dlps_lbl_page), pbuf);
    }

    if (g_dlps_btn_prev) gtk_widget_set_sensitive(g_dlps_btn_prev, g_dlps_current_page > 1);
    if (g_dlps_btn_next) gtk_widget_set_sensitive(g_dlps_btn_next, g_dlps_current_page < g_dlps_total_pages);

    if (res->games) free(res->games);
    free(res);
    return G_SOURCE_REMOVE;
}

static gboolean dlps_extract_slug(const char *input, char *out_slug, size_t out_size, char *out_domain, size_t out_dom_size) {
    if (!input || !out_slug || out_size == 0) return FALSE;
    out_slug[0] = '\0';
    if (out_domain && out_dom_size > 0) out_domain[0] = '\0';

    const char *p = input;
    while (*p == ' ' || *p == '\t') p++;

    const char *domains[] = {
        "dlpsgame.com/",
        "downloadgamepsp.org/",
        "dlxbgame.com/",
        "gamepciso.com/",
        "nswgame.com/",
        "nswgf.com/",
        NULL
    };

    for (int i = 0; domains[i]; i++) {
        const char *site = strstr(p, domains[i]);
        if (site) {
            if (out_domain && out_dom_size > 0) {
                snprintf(out_domain, out_dom_size, "https://%.*s", (int)(strlen(domains[i]) - 1), site);
            }
            p = site + strlen(domains[i]);
            while (*p == '/') p++;
            size_t len = 0;
            while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ' ' && len < out_size - 1) {
                out_slug[len++] = *p++;
            }
            out_slug[len] = '\0';
            return (len > 0);
        }
    }

    /* Check direct slug: must contain -pkg, -iso, or -ps4, -ps5, -ps3, -ps2, -psp, -psvita */
    size_t in_len = strlen(p);
    gboolean is_explicit_slug = FALSE;
    if (in_len >= 5 && (strstr(p, "-pkg") || strstr(p, "-iso") || strstr(p, "ps4-") || strstr(p, "ps5-") || strstr(p, "ps3-") || strstr(p, "ps2-") || strstr(p, "free-download"))) {
        is_explicit_slug = TRUE;
    }

    if (is_explicit_slug) {
        const char *c = p;
        gboolean valid = TRUE;
        size_t len = 0;
        while (*c) {
            if (!g_ascii_isalnum(*c) && *c != '-' && *c != '_') {
                valid = FALSE;
                break;
            }
            len++;
            c++;
        }
        if (valid && len >= 5 && len < out_size) {
            strncpy(out_slug, p, out_size - 1);
            out_slug[out_size - 1] = '\0';
            return TRUE;
        }
    }

    return FALSE;
}

static void *dlps_fetch_thread_fn(void *arg) {
    DlpsFetchReq *req = (DlpsFetchReq *)arg;
    CURL *c = curl_easy_init();
    DlpsFetchResult *res_data = g_new0(DlpsFetchResult, 1);
    res_data->page = req->page;
    res_data->req_id = req->req_id;

    if (!c) {
        snprintf(res_data->error_msg, sizeof(res_data->error_msg), "Impossibile inizializzare cURL");
        g_idle_add(on_dlps_fetch_done, res_data);
        g_free(req);
        return NULL;
    }

    const char *base_domain = "https://dlpsgame.com";
    int cat_id = 0;
    const char *console_search_tag = NULL;

    switch (req->console) {
        case 1: /* PS5 */
            base_domain = "https://dlpsgame.com";
            cat_id = 63019;
            break;
        case 2: /* PS4 */
            base_domain = "https://dlpsgame.com";
            cat_id = 4370;
            break;
        case 3: /* PS3 */
            base_domain = "https://dlpsgame.com";
            cat_id = 64;
            break;
        case 4: /* PS2 */
            base_domain = "https://dlpsgame.com";
            cat_id = 5917;
            break;
        case 5: /* PS1 / PSX */
            base_domain = "https://dlpsgame.com";
            console_search_tag = "PS1";
            break;
        case 6: /* PSP */
            base_domain = "https://downloadgamepsp.org";
            cat_id = 63;
            break;
        case 7: /* PS Vita */
            base_domain = "https://downloadgamepsp.org";
            cat_id = 2297;
            break;
        case 8: /* PSN */
            base_domain = "https://dlpsgame.com";
            cat_id = 110;
            break;
        case 9: /* Switch */
            base_domain = "https://nswgame.com";
            cat_id = 0;
            break;
        case 10: /* Xbox */
            base_domain = "https://dlxbgame.com";
            cat_id = 0;
            break;
        case 11: /* PC */
            base_domain = "https://gamepciso.com";
            cat_id = 0;
            break;
        default:
            base_domain = "https://dlpsgame.com";
            cat_id = 0;
            break;
    }

    gboolean is_search = (req->query[0] != '\0');
    char slug[256] = "";
    char custom_domain[256] = "";
    gboolean is_slug = is_search && dlps_extract_slug(req->query, slug, sizeof(slug), custom_domain, sizeof(custom_domain));

    const char *target_domain = custom_domain[0] ? custom_domain : base_domain;

    char fetch_url[1024] = "";
    char cat_param[64] = "";
    if (cat_id > 0) {
        snprintf(cat_param, sizeof(cat_param), "&categories=%d", cat_id);
    }

    gboolean slug_success = FALSE;
    DlpsMem chunk = { malloc(1), 0 };
    int totals[2] = { 0, 0 };

    /* Configure cURL with fast options and automatic decompression */
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dlps_mem_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, dlps_header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *)totals);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);

    /* 1. If explicit URL or slug, attempt direct slug lookup */
    if (is_slug) {
        snprintf(fetch_url, sizeof(fetch_url),
                 "%s/wp-json/wp/v2/posts?slug=%s&_fields=id,link,title,content,categories",
                 target_domain, slug);

        curl_easy_setopt(c, CURLOPT_URL, fetch_url);
        CURLcode res = curl_easy_perform(c);
        if (res == CURLE_OK && chunk.data && chunk.size > 10) {
            const char *p = chunk.data;
            while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
            if (*p == '[' && *(p + 1) != ']') {
                DlpsGame *games = calloc(req->per_page, sizeof(DlpsGame));
                int n = parse_dlps_games_json(chunk.data, chunk.size, games, req->per_page, req->region,
                                              is_search, res_data->status_override, sizeof(res_data->status_override));
                if (n > 0) {
                    slug_success = TRUE;
                    res_data->total_items = totals[0] > 0 ? totals[0] : n;
                    res_data->total_pages = totals[1] > 0 ? totals[1] : 1;
                    res_data->games = games;
                    res_data->count = n;
                } else {
                    free(games);
                }
            }
        }
    }

    /* 2. Full-text search or catalog browsing */
    if (!slug_success) {
        if (chunk.data) {
            free(chunk.data);
            chunk.data = malloc(1);
            chunk.size = 0;
        }
        totals[0] = 0;
        totals[1] = 0;

        char clean_query[300] = "";
        if (is_search) {
            const char *src = req->query;
            while (*src == ' ' || *src == '\t') src++;
            /* Strip known domain prefixes if pasted */
            for (const char *site = src; ; ) {
                const char *d = strstr(site, "://");
                if (d) {
                    const char *slash = strchr(d + 3, '/');
                    if (slash) { src = slash + 1; break; }
                }
                break;
            }
            size_t cq_len = 0;
            while (*src && *src != '/' && *src != '?' && *src != '#' && cq_len < sizeof(clean_query) - 1) {
                if (*src == '-' || *src == '_' || *src == '+') {
                    clean_query[cq_len++] = ' ';
                } else {
                    clean_query[cq_len++] = *src;
                }
                src++;
            }
            clean_query[cq_len] = '\0';
            g_strstrip(clean_query);
            if (console_search_tag && !strcasestr(clean_query, console_search_tag)) {
                size_t cql = strlen(clean_query);
                if (cql + strlen(console_search_tag) + 2 < sizeof(clean_query)) {
                    snprintf(clean_query + cql, sizeof(clean_query) - cql, " %s", console_search_tag);
                }
            }
        } else {
            /* Browsing with empty query */
            if (console_search_tag) {
                strncpy(clean_query, console_search_tag, sizeof(clean_query) - 1);
            } else if (req->region == 1) {
                strncpy(clean_query, "EUR", sizeof(clean_query) - 1);
            } else if (req->region == 2) {
                strncpy(clean_query, "USA", sizeof(clean_query) - 1);
            } else if (req->region == 3) {
                strncpy(clean_query, "JPN", sizeof(clean_query) - 1);
            }
        }

        if (clean_query[0]) {
            char *esc = curl_easy_escape(c, clean_query, 0);
            snprintf(fetch_url, sizeof(fetch_url),
                     "%s/wp-json/wp/v2/posts?per_page=%d&page=%d&search=%s%s&_fields=id,link,title,content,categories",
                     target_domain, req->per_page, req->page, esc ? esc : clean_query, cat_param);
            if (esc) curl_free(esc);
        } else {
            snprintf(fetch_url, sizeof(fetch_url),
                     "%s/wp-json/wp/v2/posts?per_page=%d&page=%d%s&_fields=id,link,title,content,categories",
                     target_domain, req->per_page, req->page, cat_param);
        }

        curl_easy_setopt(c, CURLOPT_URL, fetch_url);
        CURLcode res = curl_easy_perform(c);
        if (res == CURLE_OK && chunk.data) {
            res_data->total_items = totals[0];
            res_data->total_pages = totals[1];
            DlpsGame *games = calloc(req->per_page, sizeof(DlpsGame));
            int n = parse_dlps_games_json(chunk.data, chunk.size, games, req->per_page, req->region,
                                          is_search, res_data->status_override, sizeof(res_data->status_override));
            res_data->games = games;
            res_data->count = n;
        } else if (res == CURLE_OPERATION_TIMEDOUT) {
            snprintf(res_data->error_msg, sizeof(res_data->error_msg),
                     "Timeout connessione (server occupato). Prova a premere Invio o usa una parola chiave più precisa.");
        } else {
            snprintf(res_data->error_msg, sizeof(res_data->error_msg),
                     "Errore connessione: %s", curl_easy_strerror(res));
        }
    }

    curl_easy_cleanup(c);
    if (chunk.data) free(chunk.data);
    g_free(req);

    g_idle_add(on_dlps_fetch_done, res_data);
    return NULL;
}

static void trigger_dlps_fetch(const char *query, int page) {
    g_dlps_active_req_id++;
    guint current_id = g_dlps_active_req_id;

    g_dlps_is_loading = TRUE;
    if (g_dlps_spinner) gtk_spinner_start(GTK_SPINNER(g_dlps_spinner));
    if (g_dlps_lbl_page) {
        if (query && query[0]) {
            gtk_label_set_text(GTK_LABEL(g_dlps_lbl_page), "Ricerca in corso...");
        } else {
            gtk_label_set_text(GTK_LABEL(g_dlps_lbl_page), "Caricamento catalogo...");
        }
    }

    DlpsFetchReq *req = g_new0(DlpsFetchReq, 1);
    if (query) {
        strncpy(req->query, query, sizeof(req->query) - 1);
        strncpy(g_dlps_search_query, query, sizeof(g_dlps_search_query) - 1);
    } else {
        req->query[0] = '\0';
        g_dlps_search_query[0] = '\0';
    }
    req->region = g_dlps_region_filter;
    req->console = g_dlps_console_filter;
    req->page = page > 0 ? page : 1;
    req->per_page = 12;
    req->req_id = current_id;

    pthread_t th;
    if (pthread_create(&th, NULL, dlps_fetch_thread_fn, req) == 0) {
        pthread_detach(th);
    } else {
        g_free(req);
        g_dlps_is_loading = FALSE;
        if (g_dlps_spinner) gtk_spinner_stop(GTK_SPINNER(g_dlps_spinner));
    }
}

/* DLPS Details Dialog & Actions */
typedef struct {
    char url[2048];
    char dest_name[256];
    char password[128];
    GtkWidget *dialog;
} DlpsPreFillData;

/* Auto-download session: ordered candidate links, tried one after the other */
typedef struct AutoSession AutoSession;
static void auto_session_next(AutoSession *sess);
static void auto_session_free(AutoSession *sess);
static gboolean dlps_dispatch_link(const char *url, const char *title,
                                   const char *pass, gboolean from_clipboard,
                                   AutoSession *sess);

typedef struct {
    char url[2048];
    char game_title[256];
    char password[128];
    GtkWidget *dialog;
    AutoSession *sess;
} DlpsActionData;

static void on_copy_str_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *text = (const char *)user_data;
    if (!text || !text[0]) return;
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(cb, text);
    show_toast("Copiato negli appunti!");
}

static void on_open_browser_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *url = (const char *)user_data;
    if (url && url[0]) {
        g_app_info_launch_default_for_uri(url, NULL, NULL);
    }
}

static void on_prefill_download_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DlpsPreFillData *d = (DlpsPreFillData *)user_data;
    if (d->dialog && GTK_IS_WINDOW(d->dialog)) {
        gtk_window_destroy(GTK_WINDOW(d->dialog));
    }
    show_add_dialog_prefilled(GTK_WINDOW(g_window), d->url, d->dest_name, d->password);
    free(d);
}

/* Mediafire Background Resolver */
typedef struct {
    int success;
    char direct_url[2048];
    char game_title[256];
    char password[128];
    GtkWidget *dialog;
    AutoSession *sess;
} MediafireRes;

static gboolean on_mediafire_resolved_idle(gpointer user_data) {
    MediafireRes *res = (MediafireRes *)user_data;
    if (res->success && res->direct_url[0]) {
        if (res->dialog && GTK_IS_WINDOW(res->dialog)) {
            gtk_window_destroy(GTK_WINDOW(res->dialog));
        }
        char dest_name[256];
        snprintf(dest_name, sizeof(dest_name), "%.250s.pkg", res->game_title);
        show_toast("Link Mediafire risolto: download avviato verso la PS!");
        start_single_download(res->direct_url, dest_name, res->password);
        auto_session_free(res->sess);
    } else if (res->sess) {
        show_prep_progress("Mediafire non disponibile: tentativo con link successivo...");
        auto_session_next(res->sess);
    } else {
        show_toast("Impossibile risolvere il link Mediafire (file rimosso da DMCA o non disponibile).");
    }
    free(res);
    return G_SOURCE_REMOVE;
}

static void *mediafire_resolve_thread(void *arg) {
    DlpsActionData *req = (DlpsActionData *)arg;
    MediafireRes *res = calloc(1, sizeof(MediafireRes));
    strncpy(res->game_title, req->game_title, sizeof(res->game_title) - 1);
    strncpy(res->password, req->password, sizeof(res->password) - 1);
    res->dialog = req->dialog;
    res->sess = req->sess;

    CURL *c = curl_easy_init();
    if (c) {
        DlpsMem chunk = { malloc(1), 0 };
        if (chunk.data) chunk.data[0] = '\0';
        curl_easy_setopt(c, CURLOPT_URL, req->url);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dlps_mem_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
        CURLcode cres = curl_easy_perform(c);
        curl_easy_cleanup(c);

        if (cres == CURLE_OK && chunk.data) {
            const char *p = strstr(chunk.data, "href=\"https://download");
            if (!p) p = strstr(chunk.data, "href='https://download");
            if (!p) {
                const char *btn = strstr(chunk.data, "aria-label=\"Download file\"");
                if (btn) p = strstr(btn, "href=\"");
            }
            if (p) {
                const char *h = strstr(p, "http");
                if (h) {
                    const char *end = strpbrk(h, "\"' ");
                    if (end && (size_t)(end - h) < sizeof(res->direct_url)) {
                        strncpy(res->direct_url, h, end - h);
                        res->direct_url[end - h] = '\0';
                        res->success = 1;
                    }
                }
            }
            free(chunk.data);
        }
    }
    free(req);
    g_idle_add(on_mediafire_resolved_idle, res);
    return NULL;
}

static void trigger_mediafire_resolve(const char *url, const char *game_title, const char *password, GtkWidget *dialog, AutoSession *sess) {
    show_prep_progress("Risoluzione link diretto Mediafire in corso...");
    DlpsActionData *req = calloc(1, sizeof(DlpsActionData));
    strncpy(req->url, url, sizeof(req->url) - 1);
    strncpy(req->game_title, game_title, sizeof(req->game_title) - 1);
    strncpy(req->password, password, sizeof(req->password) - 1);
    req->dialog = dialog;
    req->sess = sess;

    pthread_t th;
    if (pthread_create(&th, NULL, mediafire_resolve_thread, req) == 0) {
        pthread_detach(th);
    } else {
        free(req);
        show_toast("Errore creazione thread risolutore");
        if (sess) auto_session_next(sess);
    }
}

static void on_dlps_mediafire_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DlpsActionData *d = (DlpsActionData *)user_data;
    trigger_mediafire_resolve(d->url, d->game_title, d->password, d->dialog, NULL);
    free(d);
}

/* Real-Debrid Background Resolver */
typedef struct {
    int success;
    char download_url[2048];
    char game_title[256];
    char password[128];
    char error_msg[256];
    GtkWidget *dialog;
    AutoSession *sess;
} RealDebridRes;

static gboolean on_realdebrid_resolved_idle(gpointer user_data) {
    RealDebridRes *res = (RealDebridRes *)user_data;
    if (res->success && res->download_url[0]) {
        if (res->dialog && GTK_IS_WINDOW(res->dialog)) {
            gtk_window_destroy(GTK_WINDOW(res->dialog));
        }
        char dest_name[256];
        snprintf(dest_name, sizeof(dest_name), "%.250s.pkg", res->game_title);
        start_single_download(res->download_url, dest_name, res->password);
        show_toast("Link sbloccato con Real-Debrid: download avviato!");
        auto_session_free(res->sess);
    } else if (res->sess) {
        show_prep_progress("Real-Debrid non ha sbloccato il link: provo il successivo...");
        auto_session_next(res->sess);
    } else {
        char buf[300];
        snprintf(buf, sizeof(buf), "Errore Real-Debrid: %s", res->error_msg[0] ? res->error_msg : "Sblocco fallito");
        show_toast(buf);
    }
    free(res);
    return G_SOURCE_REMOVE;
}

static void *realdebrid_resolve_thread(void *arg) {
    DlpsActionData *req = (DlpsActionData *)arg;
    RealDebridRes *res = calloc(1, sizeof(RealDebridRes));
    strncpy(res->game_title, req->game_title, sizeof(res->game_title) - 1);
    strncpy(res->password, req->password, sizeof(res->password) - 1);
    res->dialog = req->dialog;
    res->sess = req->sess;

    CURL *c = curl_easy_init();
    if (c) {
        DlpsMem chunk = { malloc(1), 0 };
        if (chunk.data) chunk.data[0] = '\0';

        char auth_hdr[300];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", g_realdebrid_token);
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, auth_hdr);

        char *esc_link = curl_easy_escape(c, req->url, 0);
        char *esc_pass = req->password[0] ? curl_easy_escape(c, req->password, 0) : NULL;
        char postfields[4096];
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
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dlps_mem_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);

        CURLcode cres = curl_easy_perform(c);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);

        if (cres == CURLE_OK && chunk.data) {
            JsonParser *parser = json_parser_new();
            if (json_parser_load_from_data(parser, chunk.data, chunk.size, NULL)) {
                JsonNode *root = json_parser_get_root(parser);
                if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                    JsonObject *obj = json_node_get_object(root);
                    if (json_object_has_member(obj, "download")) {
                        const char *dl = json_object_get_string_member(obj, "download");
                        if (dl && dl[0]) {
                            strncpy(res->download_url, dl, sizeof(res->download_url) - 1);
                            res->success = 1;
                        }
                    } else if (json_object_has_member(obj, "error")) {
                        const char *err = json_object_get_string_member(obj, "error");
                        if (err) strncpy(res->error_msg, err, sizeof(res->error_msg) - 1);
                    }
                }
            }
            g_object_unref(parser);
            free(chunk.data);
        } else {
            snprintf(res->error_msg, sizeof(res->error_msg), "Connessione fallita (%s)", curl_easy_strerror(cres));
        }
    }
    free(req);
    g_idle_add(on_realdebrid_resolved_idle, res);
    return NULL;
}

static void trigger_realdebrid_resolve(const char *url, const char *game_title, const char *password, GtkWidget *dialog, AutoSession *sess) {
    show_prep_progress("Sblocco link tramite Real-Debrid in corso... Preparazione download");
    DlpsActionData *req = calloc(1, sizeof(DlpsActionData));
    strncpy(req->url, url, sizeof(req->url) - 1);
    strncpy(req->game_title, game_title, sizeof(req->game_title) - 1);
    strncpy(req->password, password, sizeof(req->password) - 1);
    req->dialog = dialog;
    req->sess = sess;

    pthread_t th;
    if (pthread_create(&th, NULL, realdebrid_resolve_thread, req) == 0) {
        pthread_detach(th);
    } else {
        free(req);
        show_toast("Errore avvio thread Real-Debrid");
        if (sess) auto_session_next(sess);
    }
}

static void on_dlps_realdebrid_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DlpsActionData *d = (DlpsActionData *)user_data;
    trigger_realdebrid_resolve(d->url, d->game_title, d->password, d->dialog, NULL);
    free(d);
}

/* ═══════════════════════════════════════════════════════════
 * Undetected ChromeDriver (UC) Background Resolver
 * ═══════════════════════════════════════════════════════════ */
typedef struct {
    int success;
    char resolved_url[2048];
    char game_title[256];
    char password[128];
    char error_msg[256];
    char cookie[4096];
    char user_agent[512];
    char referer[2048];
    GtkWidget *dialog;
    AutoSession *sess;
} UcResolveRes;

static gboolean on_uc_resolved_idle(gpointer user_data) {
    UcResolveRes *res = (UcResolveRes *)user_data;
    if (res->success && res->resolved_url[0]) {
        if (res->dialog && GTK_IS_WINDOW(res->dialog)) {
            gtk_window_destroy(GTK_WINDOW(res->dialog));
        }
        if (res->cookie[0]) {
            strncpy(g_uc_last_cookie, res->cookie, sizeof(g_uc_last_cookie) - 1);
            g_uc_last_cookie[sizeof(g_uc_last_cookie) - 1] = '\0';
        }
        if (res->user_agent[0]) {
            strncpy(g_uc_last_user_agent, res->user_agent, sizeof(g_uc_last_user_agent) - 1);
            g_uc_last_user_agent[sizeof(g_uc_last_user_agent) - 1] = '\0';
        }
        if (res->referer[0]) {
            strncpy(g_uc_last_referer, res->referer, sizeof(g_uc_last_referer) - 1);
            g_uc_last_referer[sizeof(g_uc_last_referer) - 1] = '\0';
        }
        show_toast("Link sbloccato con Undetected ChromeDriver!");
        if (!dlps_dispatch_link(res->resolved_url, res->game_title, res->password, FALSE, NULL)) {
            char dest_name[256];
            snprintf(dest_name, sizeof(dest_name), "%.250s.pkg", res->game_title);
            start_single_download(res->resolved_url, dest_name, res->password);
        }
        auto_session_free(res->sess);
    } else if (res->sess) {
        show_prep_progress("Risolutore UC: tentativo con link successivo...");
        auto_session_next(res->sess);
    } else {
        char buf[300];
        snprintf(buf, sizeof(buf), "Risolutore UC: %s", res->error_msg[0] ? res->error_msg : "Sblocco non riuscito");
        show_toast(buf);
    }
    free(res);
    return G_SOURCE_REMOVE;
}

static void *uc_resolve_thread(void *arg) {
    DlpsActionData *req = (DlpsActionData *)arg;
    UcResolveRes *res = calloc(1, sizeof(UcResolveRes));
    strncpy(res->game_title, req->game_title, sizeof(res->game_title) - 1);
    strncpy(res->password, req->password, sizeof(res->password) - 1);
    res->dialog = req->dialog;
    res->sess = req->sess;

    const char *script_path = get_uc_resolver_path();
    const char *py_bin = get_python_binary();

    if (!script_path || !py_bin) {
        snprintf(res->error_msg, sizeof(res->error_msg), "uc_resolver.py o python3 non trovato");
        res->success = 0;
        free(req);
        g_idle_add(on_uc_resolved_idle, res);
        return NULL;
    }

    GPtrArray *argv_arr = g_ptr_array_new();
    g_ptr_array_add(argv_arr, (gpointer)py_bin);
    g_ptr_array_add(argv_arr, (gpointer)script_path);
    g_ptr_array_add(argv_arr, (gpointer)req->url);
    if (req->password[0]) {
        g_ptr_array_add(argv_arr, (gpointer)"--password");
        g_ptr_array_add(argv_arr, (gpointer)req->password);
    }
    if (g_uc_headless) {
        g_ptr_array_add(argv_arr, (gpointer)"--headless");
    }
    g_ptr_array_add(argv_arr, (gpointer)"--timeout");
    g_ptr_array_add(argv_arr, (gpointer)"65");
    g_ptr_array_add(argv_arr, NULL);

    char *stdout_buf = NULL;
    char *stderr_buf = NULL;
    int exit_status = 0;
    GError *err = NULL;

    gboolean spawned = g_spawn_sync(
        NULL,
        (char **)argv_arr->pdata,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL,
        NULL,
        &stdout_buf,
        &stderr_buf,
        &exit_status,
        &err
    );
    g_ptr_array_free(argv_arr, TRUE);

    if (spawned && exit_status == 0 && stdout_buf) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, stdout_buf, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *obj = json_node_get_object(root);
                if (json_object_has_member(obj, "success") && json_object_get_boolean_member(obj, "success")) {
                    if (json_object_has_member(obj, "url")) {
                        const char *u = json_object_get_string_member(obj, "url");
                        if (u && u[0]) {
                            strncpy(res->resolved_url, u, sizeof(res->resolved_url) - 1);
                            res->success = 1;
                        }
                    }
                    if (json_object_has_member(obj, "cookie")) {
                        const char *ck = json_object_get_string_member(obj, "cookie");
                        if (ck && ck[0]) strncpy(res->cookie, ck, sizeof(res->cookie) - 1);
                    }
                    if (json_object_has_member(obj, "user_agent")) {
                        const char *ua = json_object_get_string_member(obj, "user_agent");
                        if (ua && ua[0]) strncpy(res->user_agent, ua, sizeof(res->user_agent) - 1);
                    }
                    if (json_object_has_member(obj, "referer")) {
                        const char *rf = json_object_get_string_member(obj, "referer");
                        if (rf && rf[0]) strncpy(res->referer, rf, sizeof(res->referer) - 1);
                    }
                    if (json_object_has_member(obj, "password")) {
                        const char *pw = json_object_get_string_member(obj, "password");
                        if (pw && pw[0]) strncpy(res->password, pw, sizeof(res->password) - 1);
                    }
                } else if (json_object_has_member(obj, "message")) {
                    strncpy(res->error_msg, json_object_get_string_member(obj, "message"), sizeof(res->error_msg) - 1);
                }
            }
        }
        g_object_unref(parser);
    } else {
        if (err) {
            strncpy(res->error_msg, err->message, sizeof(res->error_msg) - 1);
            g_error_free(err);
        } else if (stderr_buf && stderr_buf[0]) {
            strncpy(res->error_msg, stderr_buf, sizeof(res->error_msg) - 1);
        } else {
            strncpy(res->error_msg, "Risoluzione link fallita o timeout", sizeof(res->error_msg) - 1);
        }
    }

    if (stdout_buf) g_free(stdout_buf);
    if (stderr_buf) g_free(stderr_buf);
    free(req);

    g_idle_add(on_uc_resolved_idle, res);
    return NULL;
}

static void trigger_uc_resolve(const char *url, const char *game_title, const char *password, GtkWidget *dialog, AutoSession *sess) {
    show_prep_progress("Undetected ChromeDriver: risoluzione protezioni in corso...");
    DlpsActionData *req = calloc(1, sizeof(DlpsActionData));
    strncpy(req->url, url, sizeof(req->url) - 1);
    strncpy(req->game_title, game_title, sizeof(req->game_title) - 1);
    strncpy(req->password, password ? password : "", sizeof(req->password) - 1);
    req->dialog = dialog;
    req->sess = sess;

    pthread_t th;
    if (pthread_create(&th, NULL, uc_resolve_thread, req) == 0) {
        pthread_detach(th);
    } else {
        free(req);
        show_toast("Errore creazione thread Undetected ChromeDriver");
        if (sess) auto_session_next(sess);
    }
}

static void on_dlps_uc_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DlpsActionData *d = (DlpsActionData *)user_data;
    trigger_uc_resolve(d->url, d->game_title, d->password, d->dialog, NULL);
    free(d);
}

/* ── Automatic link dispatch / one-click download ───────────── */
static gboolean url_is_protected(const char *url) {
    return strstr(url, "downloadgameps3.net") || strstr(url, "dlpsgame.com") ||
           strstr(url, "filecrypt.cc");
}

static gboolean url_looks_direct_file(const char *url) {
    char tmp[2048];
    strncpy(tmp, url, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *q = strchr(tmp, '?');
    if (q) *q = '\0';
    if (strcasestr(tmp, ".pkg") || strcasestr(tmp, ".rar") ||
        strcasestr(tmp, ".7z")  || strcasestr(tmp, ".zip"))
        return TRUE;
    return FALSE;
}

/* Returns TRUE when a download/resolve action was started (session ownership moves with it). */
static gboolean dlps_dispatch_link(const char *url, const char *title,
                                   const char *pass, gboolean from_clipboard,
                                   AutoSession *sess) {
    if (!url || strncmp(url, "http", 4) != 0) return FALSE;

    if (url_is_protected(url)) {
        if (g_uc_enabled && is_uc_resolver_available()) {
            trigger_uc_resolve(url, title, pass, NULL, sess);
            return TRUE;
        }
        if (!from_clipboard && !sess)
            show_toast("Link protetto: completa la verifica nel browser, poi copia il link finale.");
        return FALSE;
    }

    if (strstr(url, "mediafire.com/file/")) {
        trigger_mediafire_resolve(url, title, pass, NULL, sess);
        return TRUE;
    }

    if (url_looks_direct_file(url)) {
        char dest_name[256];
        snprintf(dest_name, sizeof(dest_name), "%.250s.pkg", title);
        show_toast("Link diretto rilevato: download avviato verso la PS!");
        start_single_download(url, dest_name, pass);
        auto_session_free(sess);
        return TRUE;
    }

    if (g_realdebrid_token[0]) {
        trigger_realdebrid_resolve(url, title, pass, NULL, sess);
        return TRUE;
    }

    if (g_uc_enabled && is_uc_resolver_available()) {
        trigger_uc_resolve(url, title, pass, NULL, sess);
        return TRUE;
    }

    return FALSE;
}

/* Clipboard watcher: after the user unlocks a protected link in the browser,
 * the first valid link copied is picked up and downloaded automatically. */
typedef struct {
    gboolean armed;
    gulong handler;
    guint timeout_id;
    char title[256];
    char pass[128];
} ClipWatch;

static ClipWatch g_clip_watch = { 0 };

static void clip_watch_disarm(void) {
    if (!g_clip_watch.armed) return;
    GdkDisplay *disp = gdk_display_get_default();
    if (disp && g_clip_watch.handler)
        g_signal_handler_disconnect(gdk_display_get_clipboard(disp), g_clip_watch.handler);
    if (g_clip_watch.timeout_id) g_source_remove(g_clip_watch.timeout_id);
    g_clip_watch.handler = 0;
    g_clip_watch.timeout_id = 0;
    g_clip_watch.armed = FALSE;
}

static gboolean on_clip_watch_timeout(gpointer data) {
    (void)data;
    g_clip_watch.timeout_id = 0;
    clip_watch_disarm();
    return G_SOURCE_REMOVE;
}

static void on_clip_watch_text(GObject *source, GAsyncResult *res, gpointer data) {
    (void)data;
    GError *err = NULL;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), res, &err);
    if (err) g_error_free(err);
    if (!text) return;
    char *clean = g_strstrip(g_strdup(text));
    g_free(text);
    if (g_clip_watch.armed &&
        dlps_dispatch_link(clean, g_clip_watch.title, g_clip_watch.pass, TRUE, NULL)) {
        clip_watch_disarm();
    }
    g_free(clean);
}

static void on_clip_watch_changed(GdkClipboard *cb, gpointer data) {
    (void)data;
    if (g_clip_watch.armed)
        gdk_clipboard_read_text_async(cb, NULL, on_clip_watch_text, NULL);
}

static void clip_watch_arm(const char *title, const char *pass) {
    GdkDisplay *disp = gdk_display_get_default();
    if (!disp) return;
    clip_watch_disarm();
    strncpy(g_clip_watch.title, title, sizeof(g_clip_watch.title) - 1);
    g_clip_watch.title[sizeof(g_clip_watch.title) - 1] = '\0';
    strncpy(g_clip_watch.pass, pass ? pass : "", sizeof(g_clip_watch.pass) - 1);
    g_clip_watch.pass[sizeof(g_clip_watch.pass) - 1] = '\0';
    g_clip_watch.handler = g_signal_connect(gdk_display_get_clipboard(disp), "changed",
                                            G_CALLBACK(on_clip_watch_changed), NULL);
    g_clip_watch.timeout_id = g_timeout_add_seconds(900, on_clip_watch_timeout, NULL);
    g_clip_watch.armed = TRUE;
}

/* Ordered candidate list for one-click download; each failure moves to the next link. */
struct AutoSession {
    char title[256];
    char pass[128];
    DlpsLink links[DLPS_MAX_LINKS];
    int n;
    int idx;
};

static void auto_session_free(AutoSession *sess) {
    free(sess);
}

static gboolean link_is_extra_content(const DlpsLink *l) {
    return g_ascii_strncasecmp(l->name, "update", 6) == 0 ||
           g_ascii_strncasecmp(l->name, "dlc", 3) == 0 ||
           g_ascii_strncasecmp(l->name, "patch", 5) == 0;
}

static int link_auto_rank(const DlpsLink *l) {
    switch (l->type) {
        case DLPS_LINK_DIRECT_PKG: return 0;
        case DLPS_LINK_MEDIAFIRE:  return 1;
        case DLPS_LINK_HOSTER:     return g_realdebrid_token[0] ? 2 : 5;
        case DLPS_LINK_PROTECTOR:  return 4;
        default:                   return 9;
    }
}

static void auto_session_next(AutoSession *sess) {
    if (!sess) return;
    while (sess->idx < sess->n) {
        const DlpsLink *l = &sess->links[sess->idx++];
        if (link_auto_rank(l) > 2) {
            if (g_uc_enabled && is_uc_resolver_available()) {
                trigger_uc_resolve(l->url, sess->title, sess->pass, NULL, sess);
                return;
            }
            continue;   /* needs a human: handled as last resort */
        }
        if (dlps_dispatch_link(l->url, sess->title, sess->pass, FALSE, sess))
            return;                            /* session now owned by the started action */
    }

    /* Nothing could be resolved automatically: open the first protected/hoster page and
     * wait for the user to copy the final link. */
    const char *manual = NULL;
    for (int i = 0; i < sess->n; i++) {
        if (link_auto_rank(&sess->links[i]) > 2) { manual = sess->links[i].url; break; }
    }
    if (manual) {
        clip_watch_arm(sess->title, sess->pass);
        g_app_info_launch_default_for_uri(manual, NULL, NULL);
        if (g_uc_enabled && is_uc_resolver_available()) {
            show_toast("Risoluzione automatica non riuscita: sblocca nel browser e copia il link finale.");
        } else {
            show_toast("Serve una verifica manuale: completala nel browser e copia il link finale, il download parte da solo.");
        }
    } else {
        show_toast("Nessun link scaricabile automaticamente per questo gioco.");
    }
    auto_session_free(sess);
}

static void on_dlps_auto_clicked(GtkButton *btn, gpointer user_data) {
    const DlpsGame *game = (const DlpsGame *)user_data;
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
    if (!game || game->n_links <= 0) {
        show_toast("Nessun link disponibile per questo gioco.");
        return;
    }

    AutoSession *sess = calloc(1, sizeof(AutoSession));
    if (!sess) return;
    strncpy(sess->title, game->title, sizeof(sess->title) - 1);
    strncpy(sess->pass, game->password, sizeof(sess->pass) - 1);

    /* Order: main game before update/DLC (extras only if nothing else exists),
     * then by rank: direct .pkg > Mediafire > hoster via Real-Debrid > manual links. */
    gboolean has_main = FALSE;
    for (int i = 0; i < game->n_links; i++)
        if (!link_is_extra_content(&game->links[i])) has_main = TRUE;

    for (int rank = 0; rank <= 9; rank++) {
        for (int i = 0; i < game->n_links && sess->n < DLPS_MAX_LINKS; i++) {
            if (has_main && link_is_extra_content(&game->links[i])) continue;
            if (link_auto_rank(&game->links[i]) == rank)
                sess->links[sess->n++] = game->links[i];
        }
    }

    if (root && GTK_IS_WINDOW(root)) gtk_window_destroy(GTK_WINDOW(root));
    auto_session_next(sess);
}


/* Direct PKG Paste & Start */
typedef struct {
    GtkWidget *entry_url;
    char game_title[256];
    char password[128];
    GtkWidget *dialog;
} DlpsDirectDlData;

static void on_clipboard_text_received(GObject *source, GAsyncResult *res, gpointer data) {
    GdkClipboard *c = GDK_CLIPBOARD(source);
    GtkWidget *e = (GtkWidget *)data;
    if (!e || !GTK_IS_ENTRY(e)) return;
    GError *err = NULL;
    char *text = gdk_clipboard_read_text_finish(c, res, &err);
    if (text && text[0]) {
        char *clean = g_strstrip(g_strdup(text));
        if (strncmp(clean, "http", 4) == 0) {
            gtk_editable_set_text(GTK_EDITABLE(e), clean);
            show_toast("Link incollato dagli appunti!");
        } else {
            show_toast("Il testo negli appunti non è un link http(s)!");
        }
        g_free(clean);
        g_free(text);
    } else {
        if (err) g_error_free(err);
        show_toast("Nessun testo negli appunti!");
    }
}

static void on_paste_clipboard_to_entry(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GtkWidget *entry = (GtkWidget *)user_data;
    if (!entry || !GTK_IS_ENTRY(entry)) return;
    GdkDisplay *disp = gdk_display_get_default();
    if (!disp) return;
    GdkClipboard *cb = gdk_display_get_clipboard(disp);
    gdk_clipboard_read_text_async(cb, NULL, on_clipboard_text_received, entry);
}

static void on_start_direct_dl_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DlpsDirectDlData *data = (DlpsDirectDlData *)user_data;
    if (!data || !data->entry_url) return;

    const char *raw_url = gtk_editable_get_text(GTK_EDITABLE(data->entry_url));
    if (!raw_url || strlen(raw_url) < (size_t)URL_MIN_LEN) {
        show_toast("Inserisci prima il link diretto del file .pkg!");
        return;
    }

    char url[2048];
    strncpy(url, raw_url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    g_strstrip(url);

    if (strstr(url, "downloadgameps3.net") || strstr(url, "dlpsgame.com") || strstr(url, "filecrypt.cc")) {
        if (g_uc_enabled && is_uc_resolver_available()) {
            trigger_uc_resolve(url, data->game_title[0] ? data->game_title : "Game", data->password, data->dialog, NULL);
            free(data);
            return;
        }
        show_toast("Attenzione: link protetto! Abilita Undetected ChromeDriver o sbloccalo nel browser.");
        return;
    }

    if (strstr(url, "mediafire.com/file/")) {
        trigger_mediafire_resolve(url, data->game_title, data->password, data->dialog, NULL);
        free(data);
        return;
    }

    char dest_name[256];
    snprintf(dest_name, sizeof(dest_name), "%.250s.pkg", data->game_title);

    if (data->dialog && GTK_IS_WINDOW(data->dialog)) {
        gtk_window_destroy(GTK_WINDOW(data->dialog));
    }

    show_add_dialog_prefilled(GTK_WINDOW(g_window), url, dest_name, data->password);
    free(data);
}

static void show_dlps_game_details_dialog(GtkWindow *parent, const DlpsGame *game) {
    if (!game) return;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        game->title, parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Chiudi", GTK_RESPONSE_CLOSE,
        NULL);

    gtk_widget_set_size_request(dialog, 700, 560);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_orientable_set_orientation(GTK_ORIENTABLE(content), GTK_ORIENTATION_VERTICAL);

    GtkWidget *main_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(main_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(main_scroll, TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    /* Header: Cover + Info */
    GtkWidget *header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

    GtkWidget *img_cover = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(img_cover), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_overflow(img_cover, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(img_cover, "game-cover-dialog");
    gtk_widget_set_size_request(img_cover, 200, 242);
    gtk_widget_set_valign(img_cover, GTK_ALIGN_START);
    if (game->cover_url[0]) {
        load_icon_async(game->cover_url, img_cover, 200, 242);
    }
    gtk_box_append(GTK_BOX(header_row), img_cover);

    GtkWidget *info_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(info_col, TRUE);

    GtkWidget *lbl_title = gtk_label_new(game->title);
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
    gtk_label_set_wrap(GTK_LABEL(lbl_title), TRUE);
    gtk_widget_add_css_class(lbl_title, "game-title");
    gtk_box_append(GTK_BOX(info_col), lbl_title);

    /* Badges row */
    GtkWidget *badges_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    if (game->cusa[0]) {
        GtkWidget *b_cusa = gtk_label_new(game->cusa);
        gtk_widget_add_css_class(b_cusa, "badge-cusa");
        gtk_box_append(GTK_BOX(badges_row), b_cusa);
    }
    if (game->region[0]) {
        GtkWidget *b_reg = gtk_label_new(game->region);
        gtk_widget_add_css_class(b_reg, "badge-region");
        gtk_box_append(GTK_BOX(badges_row), b_reg);
    }
    if (game->year[0]) {
        GtkWidget *b_year = gtk_label_new(game->year);
        gtk_widget_add_css_class(b_year, "badge-year");
        gtk_box_append(GTK_BOX(badges_row), b_year);
    }
    if (game->genre[0]) {
        GtkWidget *b_genre = gtk_label_new(game->genre);
        gtk_widget_add_css_class(b_genre, "badge-genre");
        gtk_box_append(GTK_BOX(badges_row), b_genre);
    }
    gtk_box_append(GTK_BOX(info_col), badges_row);

    /* Password Box */
    if (game->password[0]) {
        GtkWidget *pwd_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(pwd_box, "password-box");

        char ptext[256];
        snprintf(ptext, sizeof(ptext), "Password Archivio: %s", game->password);
        GtkWidget *lbl_pwd = gtk_label_new(ptext);
        gtk_label_set_xalign(GTK_LABEL(lbl_pwd), 0.0);
        gtk_widget_set_hexpand(lbl_pwd, TRUE);
        gtk_box_append(GTK_BOX(pwd_box), lbl_pwd);

        GtkWidget *btn_copy_pwd = gtk_button_new_with_label("Copia Password");
        gtk_widget_add_css_class(btn_copy_pwd, "btn-copy-link");
        gtk_widget_set_focusable(btn_copy_pwd, FALSE);
        g_signal_connect(btn_copy_pwd, "clicked", G_CALLBACK(on_copy_str_clicked), (gpointer)game->password);
        gtk_box_append(GTK_BOX(pwd_box), btn_copy_pwd);

        gtk_box_append(GTK_BOX(info_col), pwd_box);
    }

    if (game->n_links > 0) {
        GtkWidget *btn_auto = gtk_button_new_with_label("Scarica Automatico su PS");
        gtk_widget_add_css_class(btn_auto, "btn-primary");
        gtk_widget_set_halign(btn_auto, GTK_ALIGN_START);
        gtk_widget_set_focusable(btn_auto, FALSE);
        g_signal_connect(btn_auto, "clicked", G_CALLBACK(on_dlps_auto_clicked), (gpointer)game);
        gtk_box_append(GTK_BOX(info_col), btn_auto);

        GtkWidget *lbl_auto = gtk_label_new(
            "Sceglie il miglior link, lo sblocca e avvia lo streaming FTP senza passaggi manuali.");
        gtk_label_set_xalign(GTK_LABEL(lbl_auto), 0.0);
        gtk_label_set_wrap(GTK_LABEL(lbl_auto), TRUE);
        gtk_widget_add_css_class(lbl_auto, "form-hint");
        gtk_box_append(GTK_BOX(info_col), lbl_auto);
    }

    if (game->post_url[0]) {
        GtkWidget *btn_post = gtk_button_new_with_label("Apri Pagina Web");
        gtk_widget_add_css_class(btn_post, "btn-browser");
        gtk_widget_set_halign(btn_post, GTK_ALIGN_START);
        gtk_widget_set_focusable(btn_post, FALSE);
        g_signal_connect(btn_post, "clicked", G_CALLBACK(on_open_browser_clicked), (gpointer)game->post_url);
        gtk_box_append(GTK_BOX(info_col), btn_post);
    }

    gtk_box_append(GTK_BOX(header_row), info_col);
    gtk_box_append(GTK_BOX(box), header_row);

    if (game->shot_urls[0][0]) {
        GtkWidget *shots_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        for (int s = 0; s < 2; s++) {
            if (!game->shot_urls[s][0]) continue;
            GtkWidget *shot = gtk_picture_new();
            gtk_picture_set_content_fit(GTK_PICTURE(shot), GTK_CONTENT_FIT_COVER);
            gtk_widget_set_overflow(shot, GTK_OVERFLOW_HIDDEN);
            gtk_widget_add_css_class(shot, "game-shot");
            gtk_widget_set_size_request(shot, 320, 180);
            load_icon_async(game->shot_urls[s], shot, 320, 180);
            gtk_box_append(GTK_BOX(shots_row), shot);
        }
        gtk_box_append(GTK_BOX(box), shots_row);
    }

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(box), sep);

    /* Links Section */
    GtkWidget *lbl_links_sec = gtk_label_new("Link e Hoster Disponibili:");
    gtk_label_set_xalign(GTK_LABEL(lbl_links_sec), 0.0);
    gtk_widget_add_css_class(lbl_links_sec, "form-label");
    gtk_box_append(GTK_BOX(box), lbl_links_sec);

    for (int i = 0; i < game->n_links; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "hoster-row");

        GtkWidget *lbl_badge = gtk_label_new("");
        if (game->links[i].type == DLPS_LINK_DIRECT_PKG) {
            gtk_label_set_text(GTK_LABEL(lbl_badge), "DIRETTO");
            gtk_widget_add_css_class(lbl_badge, "badge-direct");
        } else if (game->links[i].type == DLPS_LINK_MEDIAFIRE) {
            gtk_label_set_text(GTK_LABEL(lbl_badge), "MEDIAFIRE");
            gtk_widget_add_css_class(lbl_badge, "badge-year");
        } else if (game->links[i].type == DLPS_LINK_PROTECTOR) {
            gtk_label_set_text(GTK_LABEL(lbl_badge), "PROTETTO");
            gtk_widget_add_css_class(lbl_badge, "badge-protector");
        } else {
            gtk_label_set_text(GTK_LABEL(lbl_badge), "HOSTER");
            gtk_widget_add_css_class(lbl_badge, "badge-genre");
        }
        gtk_box_append(GTK_BOX(row), lbl_badge);

        GtkWidget *lbl_h = gtk_label_new(game->links[i].name);
        gtk_label_set_xalign(GTK_LABEL(lbl_h), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl_h), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl_h, TRUE);
        gtk_widget_add_css_class(lbl_h, "hoster-name");
        gtk_box_append(GTK_BOX(row), lbl_h);

        /* Context action buttons */
        if (game->links[i].type == DLPS_LINK_DIRECT_PKG) {
            DlpsPreFillData *pfd = malloc(sizeof(DlpsPreFillData));
            strncpy(pfd->url, game->links[i].url, sizeof(pfd->url) - 1);
            snprintf(pfd->dest_name, sizeof(pfd->dest_name), "%.250s.pkg", game->title);
            strncpy(pfd->password, game->password, sizeof(pfd->password) - 1);
            pfd->dialog = dialog;

            GtkWidget *btn_dl = gtk_button_new_with_label("Scarica .PKG");
            gtk_widget_add_css_class(btn_dl, "btn-prefill");
            gtk_widget_set_focusable(btn_dl, FALSE);
            g_signal_connect(btn_dl, "clicked", G_CALLBACK(on_prefill_download_clicked), pfd);
            gtk_box_append(GTK_BOX(row), btn_dl);
        } else if (game->links[i].type == DLPS_LINK_MEDIAFIRE) {
            DlpsActionData *mfd = malloc(sizeof(DlpsActionData));
            strncpy(mfd->url, game->links[i].url, sizeof(mfd->url) - 1);
            strncpy(mfd->game_title, game->title, sizeof(mfd->game_title) - 1);
            strncpy(mfd->password, game->password, sizeof(mfd->password) - 1);
            mfd->dialog = dialog;

            GtkWidget *btn_mf = gtk_button_new_with_label("Risolvi e Scarica");
            gtk_widget_add_css_class(btn_mf, "btn-resolve");
            gtk_widget_set_focusable(btn_mf, FALSE);
            g_signal_connect(btn_mf, "clicked", G_CALLBACK(on_dlps_mediafire_clicked), mfd);
            gtk_box_append(GTK_BOX(row), btn_mf);

            GtkWidget *btn_wb = gtk_button_new_with_label("Apri nel Browser");
            gtk_widget_add_css_class(btn_wb, "btn-browser");
            gtk_widget_set_focusable(btn_wb, FALSE);
            g_signal_connect(btn_wb, "clicked", G_CALLBACK(on_open_browser_clicked), (gpointer)game->links[i].url);
            gtk_box_append(GTK_BOX(row), btn_wb);
        } else {
            /* Protected or Hoster link */
            if (g_realdebrid_token[0]) {
                DlpsActionData *rdd = malloc(sizeof(DlpsActionData));
                strncpy(rdd->url, game->links[i].url, sizeof(rdd->url) - 1);
                strncpy(rdd->game_title, game->title, sizeof(rdd->game_title) - 1);
                strncpy(rdd->password, game->password, sizeof(rdd->password) - 1);
                rdd->dialog = dialog;

                GtkWidget *btn_rd = gtk_button_new_with_label("Debrid 1-Click");
                gtk_widget_add_css_class(btn_rd, "btn-debrid");
                gtk_widget_set_focusable(btn_rd, FALSE);
                g_signal_connect(btn_rd, "clicked", G_CALLBACK(on_dlps_realdebrid_clicked), rdd);
                gtk_box_append(GTK_BOX(row), btn_rd);
            }

            if (is_uc_resolver_available() && (game->links[i].type == DLPS_LINK_PROTECTOR || game->links[i].type == DLPS_LINK_HOSTER)) {
                DlpsActionData *ucd = malloc(sizeof(DlpsActionData));
                strncpy(ucd->url, game->links[i].url, sizeof(ucd->url) - 1);
                strncpy(ucd->game_title, game->title, sizeof(ucd->game_title) - 1);
                strncpy(ucd->password, game->password, sizeof(ucd->password) - 1);
                ucd->dialog = dialog;
                ucd->sess = NULL;

                GtkWidget *btn_uc = gtk_button_new_with_label("Sblocca con UC");
                gtk_widget_add_css_class(btn_uc, "btn-uc");
                gtk_widget_set_focusable(btn_uc, FALSE);
                g_signal_connect(btn_uc, "clicked", G_CALLBACK(on_dlps_uc_clicked), ucd);
                gtk_box_append(GTK_BOX(row), btn_uc);
            }

            const char *btn_lbl = (game->links[i].type == DLPS_LINK_PROTECTOR) ? "Sblocca nel Browser" : "Apri Pagina";
            GtkWidget *btn_wb = gtk_button_new_with_label(btn_lbl);
            gtk_widget_add_css_class(btn_wb, "btn-browser");
            gtk_widget_set_focusable(btn_wb, FALSE);
            g_signal_connect(btn_wb, "clicked", G_CALLBACK(on_open_browser_clicked), (gpointer)game->links[i].url);
            gtk_box_append(GTK_BOX(row), btn_wb);
        }

        GtkWidget *btn_cp = gtk_button_new_with_label("Copia");
        gtk_widget_add_css_class(btn_cp, "btn-copy-link");
        gtk_widget_set_focusable(btn_cp, FALSE);
        g_signal_connect(btn_cp, "clicked", G_CALLBACK(on_copy_str_clicked), (gpointer)game->links[i].url);
        gtk_box_append(GTK_BOX(row), btn_cp);

        gtk_box_append(GTK_BOX(box), row);
    }

    /* Direct PKG Downloader section */
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(box), sep2);

    GtkWidget *box_direct = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box_direct, "direct-dl-box");

    GtkWidget *lbl_dd_title = gtk_label_new("Avvia Download Diretto .PKG verso PlayStation");
    gtk_label_set_xalign(GTK_LABEL(lbl_dd_title), 0.0);
    gtk_widget_add_css_class(lbl_dd_title, "form-label");
    gtk_box_append(GTK_BOX(box_direct), lbl_dd_title);

    GtkWidget *lbl_dd_desc = gtk_label_new(
        "Se hai sbloccato il file nel browser o generato il link diretto (es. con Real-Debrid),\n"
        "incollalo qui per avviare istantaneamente lo streaming FTP verso la tua console:");
    gtk_label_set_xalign(GTK_LABEL(lbl_dd_desc), 0.0);
    gtk_label_set_wrap(GTK_LABEL(lbl_dd_desc), TRUE);
    gtk_widget_add_css_class(lbl_dd_desc, "form-hint");
    gtk_box_append(GTK_BOX(box_direct), lbl_dd_desc);

    GtkWidget *row_input = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *entry_direct = gtk_entry_new();
    gtk_widget_set_hexpand(entry_direct, TRUE);
    gtk_widget_add_css_class(entry_direct, "form-entry");
    g_object_set(entry_direct, "placeholder-text", "https://.../titolo-gioco.pkg", NULL);
    gtk_box_append(GTK_BOX(row_input), entry_direct);

    GtkWidget *btn_paste = gtk_button_new_with_label("Incolla da Appunti");
    gtk_widget_add_css_class(btn_paste, "btn-copy-link");
    gtk_widget_set_focusable(btn_paste, FALSE);
    g_signal_connect(btn_paste, "clicked", G_CALLBACK(on_paste_clipboard_to_entry), entry_direct);
    gtk_box_append(GTK_BOX(row_input), btn_paste);

    DlpsDirectDlData *ddd = malloc(sizeof(DlpsDirectDlData));
    ddd->entry_url = entry_direct;
    strncpy(ddd->game_title, game->title, sizeof(ddd->game_title) - 1);
    strncpy(ddd->password, game->password, sizeof(ddd->password) - 1);
    ddd->dialog = dialog;

    GtkWidget *btn_start_ps = gtk_button_new_with_label("Avvia su PS");
    gtk_widget_add_css_class(btn_start_ps, "btn-primary");
    gtk_widget_set_focusable(btn_start_ps, FALSE);
    g_signal_connect(btn_start_ps, "clicked", G_CALLBACK(on_start_direct_dl_clicked), ddd);
    gtk_box_append(GTK_BOX(row_input), btn_start_ps);

    gtk_box_append(GTK_BOX(box_direct), row_input);
    gtk_box_append(GTK_BOX(box), box_direct);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(main_scroll), box);
    gtk_box_append(GTK_BOX(content), main_scroll);

    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
}

/* DLPS ListView item factory */
static void on_dlps_card_details_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DlpsGameObject *obj = DLPS_GAME_OBJECT(user_data);
    if (!obj || !obj->game) return;
    show_dlps_game_details_dialog(GTK_WINDOW(g_window), obj->game);
}

static void on_dlps_item_setup(GtkSignalListItemFactory *self, GtkListItem *list_item, gpointer user_data) {
    (void)self; (void)user_data;
    gtk_list_item_set_selectable(list_item, FALSE);
    gtk_list_item_set_focusable(list_item, FALSE);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(card, "game-card");

    GtkWidget *img = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(img), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_overflow(img, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(img, "game-cover");
    gtk_widget_set_size_request(img, 100, 121);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), img);

    GtkWidget *info_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(info_col, TRUE);

    GtkWidget *lbl_title = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
    gtk_label_set_wrap(GTK_LABEL(lbl_title), TRUE);
    gtk_widget_add_css_class(lbl_title, "game-title");
    gtk_box_append(GTK_BOX(info_col), lbl_title);

    /* Badge row 1: System, Title ID, Region */
    GtkWidget *badges_row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl_platform = gtk_label_new("");
    gtk_widget_add_css_class(lbl_platform, "badge-platform");
    GtkWidget *lbl_cusa = gtk_label_new("");
    gtk_widget_add_css_class(lbl_cusa, "badge-cusa");
    GtkWidget *lbl_reg = gtk_label_new("");
    gtk_widget_add_css_class(lbl_reg, "badge-region");

    gtk_box_append(GTK_BOX(badges_row1), lbl_platform);
    gtk_box_append(GTK_BOX(badges_row1), lbl_cusa);
    gtk_box_append(GTK_BOX(badges_row1), lbl_reg);
    gtk_box_append(GTK_BOX(info_col), badges_row1);

    /* Badge row 2: Genre, Year, Password */
    GtkWidget *badges_row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl_genre = gtk_label_new("");
    gtk_widget_add_css_class(lbl_genre, "badge-genre");
    GtkWidget *lbl_year = gtk_label_new("");
    gtk_widget_add_css_class(lbl_year, "badge-year");
    GtkWidget *lbl_pass = gtk_label_new("");
    gtk_widget_add_css_class(lbl_pass, "badge-pass");

    gtk_box_append(GTK_BOX(badges_row2), lbl_genre);
    gtk_box_append(GTK_BOX(badges_row2), lbl_year);
    gtk_box_append(GTK_BOX(badges_row2), lbl_pass);
    gtk_box_append(GTK_BOX(info_col), badges_row2);

    GtkWidget *lbl_links = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(lbl_links), 0.0);
    gtk_label_set_wrap(GTK_LABEL(lbl_links), TRUE);
    gtk_label_set_lines(GTK_LABEL(lbl_links), 1);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_links), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(lbl_links, "game-links-snippet");
    gtk_box_append(GTK_BOX(info_col), lbl_links);

    gtk_box_append(GTK_BOX(card), info_col);

    GtkWidget *btn = gtk_button_new_with_label("Dettagli e Link");
    gtk_widget_add_css_class(btn, "btn-install");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(btn, FALSE);
    gtk_box_append(GTK_BOX(card), btn);

    g_object_set_data(G_OBJECT(card), "img", img);
    g_object_set_data(G_OBJECT(card), "title", lbl_title);
    g_object_set_data(G_OBJECT(card), "platform", lbl_platform);
    g_object_set_data(G_OBJECT(card), "cusa", lbl_cusa);
    g_object_set_data(G_OBJECT(card), "reg", lbl_reg);
    g_object_set_data(G_OBJECT(card), "year", lbl_year);
    g_object_set_data(G_OBJECT(card), "genre", lbl_genre);
    g_object_set_data(G_OBJECT(card), "pass", lbl_pass);
    g_object_set_data(G_OBJECT(card), "links", lbl_links);
    g_object_set_data(G_OBJECT(card), "btn", btn);

    gtk_list_item_set_child(list_item, card);
}

static void on_dlps_item_bind(GtkSignalListItemFactory *self, GtkListItem *list_item, gpointer user_data) {
    (void)self; (void)user_data;
    GtkWidget *card = gtk_list_item_get_child(list_item);
    DlpsGameObject *obj = DLPS_GAME_OBJECT(gtk_list_item_get_item(list_item));
    if (!obj || !obj->game) return;

    GtkWidget *img = g_object_get_data(G_OBJECT(card), "img");
    GtkWidget *lbl_title = g_object_get_data(G_OBJECT(card), "title");
    GtkWidget *lbl_platform = g_object_get_data(G_OBJECT(card), "platform");
    GtkWidget *lbl_cusa = g_object_get_data(G_OBJECT(card), "cusa");
    GtkWidget *lbl_reg = g_object_get_data(G_OBJECT(card), "reg");
    GtkWidget *lbl_year = g_object_get_data(G_OBJECT(card), "year");
    GtkWidget *lbl_genre = g_object_get_data(G_OBJECT(card), "genre");
    GtkWidget *lbl_pass = g_object_get_data(G_OBJECT(card), "pass");
    GtkWidget *lbl_links = g_object_get_data(G_OBJECT(card), "links");
    GtkWidget *btn = g_object_get_data(G_OBJECT(card), "btn");

    gtk_label_set_text(GTK_LABEL(lbl_title), obj->game->title);

    if (lbl_platform) {
        if (obj->game->platform[0]) {
            gtk_label_set_text(GTK_LABEL(lbl_platform), obj->game->platform);
            gtk_widget_set_visible(lbl_platform, TRUE);
        } else {
            gtk_widget_set_visible(lbl_platform, FALSE);
        }
    }

    if (obj->game->cusa[0]) {
        gtk_label_set_text(GTK_LABEL(lbl_cusa), obj->game->cusa);
        gtk_widget_set_visible(lbl_cusa, TRUE);
    } else {
        gtk_widget_set_visible(lbl_cusa, FALSE);
    }

    if (lbl_reg) {
        if (obj->game->region[0]) {
            gtk_label_set_text(GTK_LABEL(lbl_reg), obj->game->region);
            gtk_widget_set_visible(lbl_reg, TRUE);
        } else {
            gtk_widget_set_visible(lbl_reg, FALSE);
        }
    }

    if (obj->game->year[0]) {
        gtk_label_set_text(GTK_LABEL(lbl_year), obj->game->year);
        gtk_widget_set_visible(lbl_year, TRUE);
    } else {
        gtk_widget_set_visible(lbl_year, FALSE);
    }

    if (obj->game->genre[0]) {
        gtk_label_set_text(GTK_LABEL(lbl_genre), obj->game->genre);
        gtk_widget_set_visible(lbl_genre, TRUE);
    } else {
        gtk_widget_set_visible(lbl_genre, FALSE);
    }

    if (obj->game->password[0]) {
        char pbuf[160];
        snprintf(pbuf, sizeof(pbuf), "Password: %s", obj->game->password);
        gtk_label_set_text(GTK_LABEL(lbl_pass), pbuf);
        gtk_widget_set_visible(lbl_pass, TRUE);
    } else {
        gtk_widget_set_visible(lbl_pass, FALSE);
    }

    char lbuf[512];
    if (obj->game->n_links > 0) {
        snprintf(lbuf, sizeof(lbuf), "Link (%d): %s",
                 obj->game->n_links, obj->game->links[0].name);
    } else {
        snprintf(lbuf, sizeof(lbuf), "Pagina DLPSGame");
    }
    gtk_label_set_text(GTK_LABEL(lbl_links), lbuf);

    gtk_picture_set_paintable(GTK_PICTURE(img), NULL);
    if (obj->game->cover_url[0]) {
        load_icon_async(obj->game->cover_url, img, 100, 121);
    }

    g_signal_handlers_disconnect_by_func(btn, G_CALLBACK(on_dlps_card_details_clicked), NULL);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_dlps_card_details_clicked), obj);
}

static void on_dlps_console_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)gobject; (void)pspec; (void)user_data;
    if (!g_dlps_console_dropdown || !g_dlps_store) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(g_dlps_console_dropdown));
    if ((int)sel == g_dlps_console_filter) return;
    g_dlps_console_filter = (int)sel;
    const char *q = g_dlps_search_entry ? gtk_editable_get_text(GTK_EDITABLE(g_dlps_search_entry)) : "";
    trigger_dlps_fetch(q, 1);
}

static void on_dlps_region_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)gobject; (void)pspec; (void)user_data;
    if (!g_dlps_region_dropdown || !g_dlps_store) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(g_dlps_region_dropdown));
    if ((int)sel == g_dlps_region_filter) return;
    g_dlps_region_filter = (int)sel;
    const char *q = g_dlps_search_entry ? gtk_editable_get_text(GTK_EDITABLE(g_dlps_search_entry)) : "";
    trigger_dlps_fetch(q, 1);
}

static guint g_dlps_search_timer_id = 0;

static gboolean on_dlps_search_debounced_cb(gpointer user_data) {
    (void)user_data;
    g_dlps_search_timer_id = 0;
    const char *q = g_dlps_search_entry ? gtk_editable_get_text(GTK_EDITABLE(g_dlps_search_entry)) : "";
    trigger_dlps_fetch(q, 1);
    return G_SOURCE_REMOVE;
}

static void on_dlps_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)entry; (void)user_data;
    if (g_dlps_search_timer_id > 0) {
        g_source_remove(g_dlps_search_timer_id);
        g_dlps_search_timer_id = 0;
    }
    const char *q = g_dlps_search_entry ? gtk_editable_get_text(GTK_EDITABLE(g_dlps_search_entry)) : "";
    if (!q || q[0] == '\0') {
        /* When search bar is cleared, reset catalog immediately */
        trigger_dlps_fetch("", 1);
        return;
    }
    /* Only auto-search while typing if user has entered at least 3 characters to avoid server overload */
    if (strlen(q) >= 3) {
        g_dlps_search_timer_id = g_timeout_add(800, on_dlps_search_debounced_cb, NULL);
    }
}

static void on_dlps_search_activated(GtkSearchEntry *entry, gpointer user_data) {
    (void)user_data;
    if (g_dlps_search_timer_id > 0) {
        g_source_remove(g_dlps_search_timer_id);
        g_dlps_search_timer_id = 0;
    }
    const char *q = gtk_editable_get_text(GTK_EDITABLE(entry));
    trigger_dlps_fetch(q, 1);
}

static void on_dlps_search_btn_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (g_dlps_search_timer_id > 0) {
        g_source_remove(g_dlps_search_timer_id);
        g_dlps_search_timer_id = 0;
    }
    const char *q = g_dlps_search_entry ? gtk_editable_get_text(GTK_EDITABLE(g_dlps_search_entry)) : "";
    trigger_dlps_fetch(q, 1);
}

static void on_dlps_prev_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (g_dlps_current_page > 1) {
        trigger_dlps_fetch(g_dlps_search_query, g_dlps_current_page - 1);
    }
}

static void on_dlps_next_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (g_dlps_current_page < g_dlps_total_pages) {
        trigger_dlps_fetch(g_dlps_search_query, g_dlps_current_page + 1);
    }
}

static void on_dlps_refresh_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    trigger_dlps_fetch(g_dlps_search_query, g_dlps_current_page);
}

static GtkWidget *build_dlpsgame_page(void) {
    if (!g_dlps_store) {
        g_dlps_store = g_list_store_new(TYPE_DLPS_GAME_OBJECT);
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "dlps-page");
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_vexpand(box, TRUE);

    /* Row 1: Search and Filters */
    GtkWidget *toolbar_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(toolbar_top, "dlps-toolbar");

    GtkWidget *btn_back = gtk_button_new_with_label("Torna ai Download");
    gtk_widget_add_css_class(btn_back, "btn-nav");
    gtk_widget_set_focusable(btn_back, FALSE);
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_to_downloads_clicked), NULL);

    g_dlps_search_entry = gtk_search_entry_new();
    gtk_widget_add_css_class(g_dlps_search_entry, "dlps-search");
    gtk_widget_set_hexpand(g_dlps_search_entry, TRUE);
    g_object_set(g_dlps_search_entry, "placeholder-text", "Cerca per titolo, CUSA o incolla link/slug...", NULL);
    g_signal_connect(g_dlps_search_entry, "activate", G_CALLBACK(on_dlps_search_activated), NULL);
    g_signal_connect(g_dlps_search_entry, "search-changed", G_CALLBACK(on_dlps_search_changed), NULL);

    GtkWidget *btn_search = gtk_button_new_with_label("Cerca");
    gtk_widget_add_css_class(btn_search, "btn-page");
    gtk_widget_set_focusable(btn_search, FALSE);
    g_signal_connect(btn_search, "clicked", G_CALLBACK(on_dlps_search_btn_clicked), NULL);

    GtkStringList *dlps_consoles = gtk_string_list_new(NULL);
    gtk_string_list_append(dlps_consoles, "Tutte le Console");
    gtk_string_list_append(dlps_consoles, "PlayStation 5 (PS5)");
    gtk_string_list_append(dlps_consoles, "PlayStation 4 (PS4)");
    gtk_string_list_append(dlps_consoles, "PlayStation 3 (PS3)");
    gtk_string_list_append(dlps_consoles, "PlayStation 2 (PS2)");
    gtk_string_list_append(dlps_consoles, "PlayStation 1 (PS1 / PSX)");
    gtk_string_list_append(dlps_consoles, "PlayStation Portable (PSP)");
    gtk_string_list_append(dlps_consoles, "PlayStation Vita (PS Vita)");
    gtk_string_list_append(dlps_consoles, "PlayStation Network (PSN)");
    gtk_string_list_append(dlps_consoles, "Nintendo Switch");
    gtk_string_list_append(dlps_consoles, "Xbox");
    gtk_string_list_append(dlps_consoles, "PC Games");

    g_dlps_console_dropdown = gtk_drop_down_new(G_LIST_MODEL(dlps_consoles), NULL);
    gtk_widget_add_css_class(g_dlps_console_dropdown, "catalog-dropdown");
    gtk_widget_set_focusable(g_dlps_console_dropdown, FALSE);
    g_object_unref(dlps_consoles);
    g_signal_connect(g_dlps_console_dropdown, "notify::selected", G_CALLBACK(on_dlps_console_changed), NULL);

    GtkStringList *dlps_regions = gtk_string_list_new(NULL);
    gtk_string_list_append(dlps_regions, "Tutte le Regioni");
    gtk_string_list_append(dlps_regions, "Europa (EUR)");
    gtk_string_list_append(dlps_regions, "America (USA)");
    gtk_string_list_append(dlps_regions, "Giappone / Asia");

    g_dlps_region_dropdown = gtk_drop_down_new(G_LIST_MODEL(dlps_regions), NULL);
    gtk_widget_add_css_class(g_dlps_region_dropdown, "catalog-dropdown");
    gtk_widget_set_focusable(g_dlps_region_dropdown, FALSE);
    g_object_unref(dlps_regions);
    g_signal_connect(g_dlps_region_dropdown, "notify::selected", G_CALLBACK(on_dlps_region_changed), NULL);

    gtk_box_append(GTK_BOX(toolbar_top), btn_back);
    gtk_box_append(GTK_BOX(toolbar_top), g_dlps_search_entry);
    gtk_box_append(GTK_BOX(toolbar_top), btn_search);
    gtk_box_append(GTK_BOX(toolbar_top), g_dlps_console_dropdown);
    gtk_box_append(GTK_BOX(toolbar_top), g_dlps_region_dropdown);

    /* Row 2: Navigation & Status Bar */
    GtkWidget *toolbar_bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(toolbar_bottom, "dlps-statusbar");

    g_dlps_btn_prev = gtk_button_new_with_label("◀ Precedente");
    gtk_widget_add_css_class(g_dlps_btn_prev, "btn-page");
    gtk_widget_set_focusable(g_dlps_btn_prev, FALSE);
    gtk_widget_set_sensitive(g_dlps_btn_prev, FALSE);
    g_signal_connect(g_dlps_btn_prev, "clicked", G_CALLBACK(on_dlps_prev_clicked), NULL);

    g_dlps_spinner = gtk_spinner_new();
    gtk_widget_set_valign(g_dlps_spinner, GTK_ALIGN_CENTER);

    g_dlps_lbl_page = gtk_label_new("Pronto");
    gtk_widget_add_css_class(g_dlps_lbl_page, "dlps-page-label");
    gtk_widget_set_hexpand(g_dlps_lbl_page, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_dlps_lbl_page), 0.5);

    g_dlps_btn_next = gtk_button_new_with_label("Successivo ▶");
    gtk_widget_add_css_class(g_dlps_btn_next, "btn-page");
    gtk_widget_set_focusable(g_dlps_btn_next, FALSE);
    gtk_widget_set_sensitive(g_dlps_btn_next, FALSE);
    g_signal_connect(g_dlps_btn_next, "clicked", G_CALLBACK(on_dlps_next_clicked), NULL);

    GtkWidget *btn_refresh = gtk_button_new_with_label("Aggiorna");
    gtk_widget_add_css_class(btn_refresh, "btn-page");
    gtk_widget_set_focusable(btn_refresh, FALSE);
    gtk_widget_set_tooltip_text(btn_refresh, "Aggiorna elenco");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_dlps_refresh_clicked), NULL);

    gtk_box_append(GTK_BOX(toolbar_bottom), g_dlps_btn_prev);
    gtk_box_append(GTK_BOX(toolbar_bottom), g_dlps_spinner);
    gtk_box_append(GTK_BOX(toolbar_bottom), g_dlps_lbl_page);
    gtk_box_append(GTK_BOX(toolbar_bottom), g_dlps_btn_next);
    gtk_box_append(GTK_BOX(toolbar_bottom), btn_refresh);

    gtk_box_append(GTK_BOX(box), toolbar_top);
    gtk_box_append(GTK_BOX(box), toolbar_bottom);

    /* ListView Model */
    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_dlps_store));

    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(on_dlps_item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_dlps_item_bind), NULL);

    GtkWidget *list_view = gtk_list_view_new(GTK_SELECTION_MODEL(sel), GTK_LIST_ITEM_FACTORY(factory));

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list_view);

    gtk_box_append(GTK_BOX(box), scrolled);
    return box;
}

static void on_dlps_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (g_stack) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "dlpsgame");
        guint n_items = g_dlps_store ? g_list_model_get_n_items(G_LIST_MODEL(g_dlps_store)) : 0;
        if (n_items == 0 && !g_dlps_is_loading) {
            trigger_dlps_fetch("", 1);
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * Application activate — builds the main window
 * ═══════════════════════════════════════════════════════════ */

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    load_css();
    load_settings();
    trigger_ftp_ensure_dir_async();
    trigger_catalog_fetch_async();

    g_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_window), "PS FTP Downloader");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 860, 640);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* ── Header bar ── */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
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

    gtk_widget_set_size_request(github_img, 32, 32);

    GtkWidget *btn_github = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(btn_github), github_img);
    gtk_widget_add_css_class(btn_github, "flat");
    gtk_widget_add_css_class(btn_github, "github-btn");
    gtk_widget_set_focusable(btn_github, FALSE);
    gtk_widget_set_valign(btn_github, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(btn_github, "View on GitHub");
    g_signal_connect(btn_github, "clicked", G_CALLBACK(on_github_clicked), NULL);

    GtkWidget *lbl_title = gtk_label_new("PS FTP Downloader");
    gtk_widget_add_css_class(lbl_title, "app-title");
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);

    GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(title_row), lbl_title);
    gtk_box_append(GTK_BOX(title_row), btn_github);

    char sub[512];
    snprintf(sub, sizeof(sub), "ftp://%s:%d/%s", g_ftp_host, g_ftp_port, g_ftp_dir);
    g_lbl_sub = gtk_label_new(sub);
    gtk_widget_add_css_class(g_lbl_sub, "app-subtitle");
    gtk_label_set_xalign(GTK_LABEL(g_lbl_sub), 0.0);

    gtk_box_append(GTK_BOX(title_col), title_row);
    gtk_box_append(GTK_BOX(title_col), g_lbl_sub);

    /* Spacer pushes the action buttons to the right edge. */
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);

    GtkWidget *btn_dlps = gtk_button_new_with_label("Sfoglia Giochi");
    gtk_widget_add_css_class(btn_dlps, "btn-dlps");
    gtk_widget_set_focusable(btn_dlps, FALSE);
    gtk_widget_set_valign(btn_dlps, GTK_ALIGN_CENTER);
    g_signal_connect(btn_dlps, "clicked", G_CALLBACK(on_dlps_clicked), NULL);

    GtkWidget *btn_sources = gtk_button_new_with_label("Sorgenti");
    gtk_widget_add_css_class(btn_sources, "btn-settings");
    gtk_widget_set_focusable(btn_sources, FALSE);
    gtk_widget_set_valign(btn_sources, GTK_ALIGN_CENTER);
    g_signal_connect(btn_sources, "clicked", G_CALLBACK(on_sources_clicked), g_window);

    GtkWidget *btn_catalog = gtk_button_new_with_label("Tools");
    gtk_widget_add_css_class(btn_catalog, "btn-nav");
    gtk_widget_set_focusable(btn_catalog, FALSE);
    gtk_widget_set_valign(btn_catalog, GTK_ALIGN_CENTER);
    g_signal_connect(btn_catalog, "clicked", G_CALLBACK(on_catalog_clicked), NULL);

    GtkWidget *btn_settings = gtk_button_new_with_label("Settings");
    gtk_widget_add_css_class(btn_settings, "btn-settings");
    gtk_widget_set_focusable(btn_settings, FALSE);
    gtk_widget_set_valign(btn_settings, GTK_ALIGN_CENTER);
    g_signal_connect(btn_settings, "clicked",
                     G_CALLBACK(on_settings_clicked), g_window);

    GtkWidget *btn_arch = gtk_button_new_with_label("Split Archive");
    gtk_widget_add_css_class(btn_arch, "btn-add");
    gtk_widget_set_focusable(btn_arch, FALSE);
    gtk_widget_set_valign(btn_arch, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(btn_arch,
        "Download a multi-part archive, extract locally with 7zip, and upload the .pkg via FTP");
    g_signal_connect(btn_arch, "clicked", G_CALLBACK(on_arch_clicked), g_window);

    GtkWidget *btn_add = gtk_button_new_with_label("New Download");
    gtk_widget_add_css_class(btn_add, "btn-add");
    gtk_widget_set_focusable(btn_add, FALSE);
    gtk_widget_set_valign(btn_add, GTK_ALIGN_CENTER);
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_add_clicked), g_window);

    gtk_box_append(GTK_BOX(header), title_col);
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), btn_dlps);
    gtk_box_append(GTK_BOX(header), btn_catalog);
    gtk_box_append(GTK_BOX(header), btn_sources);
    gtk_box_append(GTK_BOX(header), btn_settings);
    gtk_box_append(GTK_BOX(header), btn_arch);
    gtk_box_append(GTK_BOX(header), btn_add);

    /* ── Content stack: empty placeholder ↔ download list ↔ catalog ── */
    g_stack = gtk_stack_new();
    gtk_widget_set_vexpand(g_stack, TRUE);

    /* Empty state */
    GtkWidget *empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(empty_box, "empty-box");
    gtk_widget_set_halign(empty_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);

    GtkWidget *e_icon  = gtk_image_new_from_icon_name("folder-download-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(e_icon), 48);
    GtkWidget *e_title = gtk_label_new("Nessun trasferimento attivo");
    GtkWidget *e_sub   = gtk_label_new(
        "Avvia un nuovo download o sfoglia le sezioni 'Sfoglia Giochi' e 'Tools'");
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

    /* Catalog view */
    GtkWidget *catalog_view = build_catalog_page();
    GtkWidget *dlps_view = build_dlpsgame_page();

    gtk_stack_add_named(GTK_STACK(g_stack), empty_box, "empty");
    gtk_stack_add_named(GTK_STACK(g_stack), scroll,    "list");
    gtk_stack_add_named(GTK_STACK(g_stack), catalog_view, "catalog");
    gtk_stack_add_named(GTK_STACK(g_stack), dlps_view, "dlpsgame");
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "empty");

    /* ── Status bar ── */
    GtkWidget *statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(statusbar, "statusbar");
    g_lbl_stats = gtk_label_new("No transfers");
    gtk_label_set_xalign(GTK_LABEL(g_lbl_stats), 0.0);
    gtk_widget_set_hexpand(g_lbl_stats, TRUE);
    gtk_box_append(GTK_BOX(statusbar), g_lbl_stats);

    g_toast_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(g_toast_box, "toast-box");
    gtk_widget_set_visible(g_toast_box, FALSE);

    g_toast_label = gtk_label_new("");
    gtk_widget_add_css_class(g_toast_label, "toast-label");
    gtk_box_append(GTK_BOX(g_toast_box), g_toast_label);

    g_toast_progress = gtk_progress_bar_new();
    gtk_widget_add_css_class(g_toast_progress, "toast-progress");
    gtk_widget_set_size_request(g_toast_progress, 150, 6);
    gtk_widget_set_valign(g_toast_progress, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(g_toast_progress, FALSE);
    gtk_box_append(GTK_BOX(g_toast_box), g_toast_progress);

    gtk_box_append(GTK_BOX(statusbar), g_toast_box);

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
