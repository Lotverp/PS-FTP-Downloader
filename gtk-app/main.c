/*
 * PS FTP Downloader — GTK4 + libcurl
 *
 * Streams files directly from any HTTP(S) URL to a PlayStation console via FTP,
 * using a lock-based ring-buffer pipe — zero local disk usage.
 *
 * Architecture:
 *   - Main thread: GTK4 UI only (all widget mutations happen here via g_idle_add)
 *   - Download thread: orchestrates probing + pipe + FTP upload
 *   - HTTP thread: fills the ring-buffer from the HTTP source
 *
 * Build:    make
 * Requires: libgtk-4-dev, libcurl4-openssl-dev, build-essential
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

/* ═══════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════ */

#define APP_ID           "io.psftpdownloader.app"
#define APP_VERSION      "1.1.0"
#define APP_GITHUB_URL   "https://github.com/Lotverp/PS-FTP-Downloader"

#define FTP_HOST_DEFAULT "192.168.1.111"
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
    DL_CANCELLED    /* user-cancelled                       */
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
    GtkWidget *btn_cancel;
    GtkWidget *btn_remove;

    pthread_t thread;
} Download;

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

/* Root-level GTK widgets kept for cross-function access. */
static GtkWidget *g_window;
static GtkWidget *g_list_box;
static GtkWidget *g_lbl_stats;
static GtkWidget *g_stack;

/* Dialog entry refs (valid only while the respective dialog is open). */
static GtkWidget *g_entry_url;
static GtkWidget *g_entry_name;
static GtkWidget *g_lbl_detected;
static GtkWidget *g_set_host, *g_set_port, *g_set_dir, *g_set_user, *g_set_pass;

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
            case DL_ACTIVE:                   active++; break;
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
        "status-done",   "status-error",  "status-error-clickable", NULL
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
    gboolean running  = (dl->status == DL_QUEUED  ||
                         dl->status == DL_PROBING  ||
                         dl->status == DL_ACTIVE);
    gboolean stalled  = (dl->status == DL_PAUSED  || dl->status == DL_ERROR);
    gboolean terminal = (dl->status == DL_DONE    ||
                         dl->status == DL_ERROR    ||
                         dl->status == DL_CANCELLED);

    gtk_widget_set_visible(dl->btn_pause,  running);
    gtk_widget_set_visible(dl->btn_resume, stalled);
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

/* libcurl write callback — HTTP thread calls this to push bytes into the pipe. */
static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Pipe  *pipe  = (Pipe *)userdata;
    size_t total = size * nmemb;
    size_t written = 0;

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

typedef struct { Pipe *pipe; Download *dl; } HttpThreadArgs;

static void *http_thread_fn(void *arg) {
    HttpThreadArgs *a    = (HttpThreadArgs *)arg;
    Pipe           *pipe = a->pipe;
    Download       *dl   = a->dl;
    free(a);

    CURL *c = curl_easy_init();
    CURL_SET(c, CURLOPT_URL,            dl->url);
    CURL_SET(c, CURLOPT_WRITEFUNCTION,  http_write_cb);
    CURL_SET(c, CURLOPT_WRITEDATA,      pipe);
    CURL_SET(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURL_SET(c, CURLOPT_MAXREDIRS,      15L);
    CURL_SET(c, CURLOPT_USERAGENT,      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
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

        res = curl_easy_perform(c);

        if (res == CURLE_OK || res == CURLE_WRITE_ERROR || pipe->cancel || dl->request_stop)
            break; /* Success or user cancelled */

        /* Transient error (e.g. connection drop) -> wait and retry */
        retries++;
        sleep(2);
    }

    pthread_mutex_lock(&pipe->mutex);
    pipe->http_done = 1;
    /* CURLE_WRITE_ERROR means http_write_cb returned 0 (cancelled) — not an error. */
    if (res != CURLE_OK && res != CURLE_WRITE_ERROR &&
        !dl->request_stop && dl->error_msg[0] == '\0') {
        strncpy(dl->error_msg, curl_easy_strerror(res), sizeof(dl->error_msg) - 1);
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

static void *download_thread_fn(void *arg) {
    Download *dl = (Download *)arg;
    dl->request_stop = STOP_NONE;

    /* ── Step 1: HTTP HEAD — get content length ── */
    dl->status = DL_PROBING;
    post_update(dl);

    {
        CURL *head = curl_easy_init();
        CURL_SET(head, CURLOPT_URL,            dl->url);
        CURL_SET(head, CURLOPT_NOBODY,         1L);
        CURL_SET(head, CURLOPT_FOLLOWLOCATION, 1L);
        CURL_SET(head, CURLOPT_CONNECTTIMEOUT, 15L);
        CURL_SET(head, CURLOPT_USERAGENT,      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        CURL_SET(head, CURLOPT_FAILONERROR,    1L);
        curl_easy_perform(head);
        curl_off_t cl = 0;
        curl_easy_getinfo(head, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        curl_easy_cleanup(head);
        if (cl > 0) dl->total_size = cl;
    }

    /* ── Step 2: FTP SIZE probe — find resume offset ── */
    char ftp_url[2048];
    snprintf(ftp_url, sizeof(ftp_url), "ftp://%s:%d/%s/%s",
             g_ftp_host, g_ftp_port, g_ftp_dir, dl->dest_name);

    {
        curl_off_t remote_size = 0;
        CURL *probe = curl_easy_init();
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
        CURL_SET(ftp, CURLOPT_URL,                      ftp_url);
        CURL_SET(ftp, CURLOPT_UPLOAD,                   1L);
        CURL_SET(ftp, CURLOPT_READFUNCTION,             ftp_read_cb);
        CURL_SET(ftp, CURLOPT_READDATA,                 pipe);
        CURL_SET(ftp, CURLOPT_USERNAME,                 g_ftp_user);
        CURL_SET(ftp, CURLOPT_PASSWORD,                 g_ftp_pass);
        CURL_SET(ftp, CURLOPT_XFERINFOFUNCTION,         xfer_progress_cb);
        CURL_SET(ftp, CURLOPT_XFERINFODATA,             dl);
        CURL_SET(ftp, CURLOPT_NOPROGRESS,               0L);
        CURL_SET(ftp, CURLOPT_FTP_CREATE_MISSING_DIRS,  1L);
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

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        "Transfer error");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                              "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dialog), "Error details");
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_window_destroy), NULL);
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
        pthread_create(&dl->thread, NULL, download_thread_fn, dl);
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

        free(dl);
        /* Compact the array: shift subsequent entries left. */
        for (int j = i; j < n_downloads - 1; j++)
            downloads[j] = downloads[j + 1];
        downloads[--n_downloads] = NULL;
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

    /* Action buttons */
    dl->btn_resume = make_action_btn("Resume", "btn-resume", dl->id,
                                      G_CALLBACK(on_resume_clicked));
    dl->btn_pause  = make_action_btn("Pause",  "btn-pause",  dl->id,
                                      G_CALLBACK(on_pause_clicked));
    dl->btn_cancel = make_action_btn("Cancel", "btn-cancel", dl->id,
                                      G_CALLBACK(on_cancel_clicked));
    dl->btn_remove = make_action_btn("Remove", "btn-remove", dl->id,
                                      G_CALLBACK(on_remove_clicked));

    /* Resume and Remove are hidden initially (revealed by update_row_ui). */
    gtk_widget_set_visible(dl->btn_resume, FALSE);
    gtk_widget_set_visible(dl->btn_remove, FALSE);

    gtk_box_append(GTK_BOX(top), dl->lbl_name);
    gtk_box_append(GTK_BOX(top), dl->lbl_status);
    gtk_box_append(GTK_BOX(top), dl->btn_resume);
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
    CURL_SET(c, CURLOPT_URL,            a->url);
    CURL_SET(c, CURLOPT_NOBODY,         1L);
    CURL_SET(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURL_SET(c, CURLOPT_CONNECTTIMEOUT, 10L);
    CURL_SET(c, CURLOPT_USERAGENT,      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    CURL_SET(c, CURLOPT_FAILONERROR,    1L);
    curl_easy_perform(c);

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

/* Fired when the URL entry changes — debounce via minimum length check. */
static void on_url_changed(GtkEditable *editable, gpointer user_data) {
    (void)user_data;
    const char *url = gtk_editable_get_text(editable);

    if (!url || strlen(url) < (size_t)URL_MIN_LEN) {
        if (g_lbl_detected)
            gtk_label_set_text(GTK_LABEL(g_lbl_detected),
                               "Enter a URL for automatic detection");
        return;
    }

    if (g_lbl_detected)
        gtk_label_set_text(GTK_LABEL(g_lbl_detected), "Analyzing URL…");

    ProbeArgs *a = malloc(sizeof(ProbeArgs));
    strncpy(a->url, url, sizeof(a->url) - 1);

    pthread_t th;
    pthread_create(&th, NULL, probe_thread_fn, a);
    pthread_detach(th);
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
        g_entry_url = g_entry_name = g_lbl_detected = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    const char *url  = gtk_editable_get_text(GTK_EDITABLE(g_entry_url));
    const char *name = gtk_editable_get_text(GTK_EDITABLE(g_entry_name));

    if (!url || strlen(url) < (size_t)URL_MIN_LEN) {
        g_entry_url = g_entry_name = g_lbl_detected = NULL;
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    /* Detect extension from URL. */
    char det_ext[32] = "";
    extract_url_ext(url, det_ext, sizeof(det_ext));

    /* Build destination filename. */
    char full_name[512] = "";
    if (name && strlen(name) > 0) {
        char *dot = strrchr(name, '.');
        if (dot && strlen(dot) <= EXT_MAX_LEN)
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
        snprintf(full_name, sizeof(full_name), "%s", slash ? slash + 1 : "file");
    }

    /* Allocate and register the new Download. */
    pthread_mutex_lock(&dl_mutex);
    if (n_downloads >= MAX_DOWNLOADS) {
        pthread_mutex_unlock(&dl_mutex);
        g_entry_url = g_entry_name = g_lbl_detected = NULL;
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

    pthread_create(&dl->thread, NULL, download_thread_fn, dl);
    pthread_detach(dl->thread);

    g_entry_url = g_entry_name = g_lbl_detected = NULL;
    gtk_window_destroy(GTK_WINDOW(dialog));
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

        if (host && host[0]) strncpy(g_ftp_host, host, sizeof(g_ftp_host) - 1);
        if (port && atoi(port) > 0) g_ftp_port = atoi(port);
        if (dir  && dir[0])  strncpy(g_ftp_dir,  dir,  sizeof(g_ftp_dir)  - 1);

        strncpy(g_ftp_user,
                (user && user[0]) ? user : FTP_USER_DEFAULT,
                sizeof(g_ftp_user) - 1);
        strncpy(g_ftp_pass,
                (pass && pass[0]) ? pass : FTP_PASS_DEFAULT,
                sizeof(g_ftp_pass) - 1);
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

    gtk_box_append(GTK_BOX(content), form);

    GtkWidget *ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                        GTK_RESPONSE_OK);
    gtk_widget_add_css_class(ok, "btn-primary");

    g_signal_connect(dialog, "response", G_CALLBACK(on_settings_response), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
}

/* ═══════════════════════════════════════════════════════════
 * Header action callbacks
 * ═══════════════════════════════════════════════════════════ */

static void on_add_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    show_add_dialog(GTK_WINDOW(user_data));
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
    GtkWidget *lbl_sub = gtk_label_new(sub);
    gtk_widget_add_css_class(lbl_sub, "app-subtitle");
    gtk_label_set_xalign(GTK_LABEL(lbl_sub), 0.0);

    gtk_box_append(GTK_BOX(title_col), title_row);
    gtk_box_append(GTK_BOX(title_col), lbl_sub);

    /* Spacer pushes the action buttons to the right edge. */
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);

    GtkWidget *btn_settings = gtk_button_new_with_label("Settings");
    gtk_widget_add_css_class(btn_settings, "btn-settings");
    gtk_widget_set_valign(btn_settings, GTK_ALIGN_CENTER);
    g_signal_connect(btn_settings, "clicked",
                     G_CALLBACK(on_settings_clicked), g_window);

    GtkWidget *btn_add = gtk_button_new_with_label("New Download");
    gtk_widget_add_css_class(btn_add, "btn-add");
    gtk_widget_set_valign(btn_add, GTK_ALIGN_CENTER);
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_add_clicked), g_window);

    gtk_box_append(GTK_BOX(header), title_col);
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), btn_settings);
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
