#include "net_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __ORBIS__
/* =========================================================================
 * Desktop (Linux) implementation using libcurl
 * ========================================================================= */
#include <curl/curl.h>

typedef struct {
    char *data;
    size_t size;
} MemChunk;

static size_t curl_mem_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    MemChunk *mem = (MemChunk *)userp;
    char *ptr = (char *)realloc(mem->data, mem->size + total + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

bool net_init(void) {
    return (curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
}

void net_cleanup(void) {
    curl_global_cleanup();
}

bool net_get(const char *url, const char *auth_bearer, NetResponse *out) {
    if (!out) return false;
    memset(out, 0, sizeof(NetResponse));

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(out->error_msg, sizeof(out->error_msg), "cURL init failed");
        return false;
    }

    MemChunk chunk = { (char *)malloc(1), 0 };
    if (chunk.data) chunk.data[0] = '\0';

    struct curl_slist *headers = NULL;
    if (auth_bearer && auth_bearer[0]) {
        char auth_hdr[384];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", auth_bearer);
        headers = curl_slist_append(headers, auth_hdr);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "PS4Downloader/1.0 (Mozilla/5.0 PS4)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_mem_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    out->status_code = (int)http_code;

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !chunk.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Network error: %s", curl_easy_strerror(res));
        if (chunk.data) free(chunk.data);
        return false;
    }

    out->data = chunk.data;
    out->size = chunk.size;
    out->success = (http_code >= 200 && http_code < 400);
    return out->success;
}

bool net_post(const char *url, const char *post_data, const char *content_type, const char *auth_bearer, NetResponse *out) {
    if (!out) return false;
    memset(out, 0, sizeof(NetResponse));

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(out->error_msg, sizeof(out->error_msg), "cURL init failed");
        return false;
    }

    MemChunk chunk = { (char *)malloc(1), 0 };
    if (chunk.data) chunk.data[0] = '\0';

    struct curl_slist *headers = NULL;
    if (auth_bearer && auth_bearer[0]) {
        char auth_hdr[384];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", auth_bearer);
        headers = curl_slist_append(headers, auth_hdr);
    }
    if (content_type && content_type[0]) {
        char ct_hdr[256];
        snprintf(ct_hdr, sizeof(ct_hdr), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, ct_hdr);
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data ? post_data : "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "PS4Downloader/1.0 (Mozilla/5.0 PS4)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_mem_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    out->status_code = (int)http_code;

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !chunk.data) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Network error: %s", curl_easy_strerror(res));
        if (chunk.data) free(chunk.data);
        return false;
    }

    out->data = chunk.data;
    out->size = chunk.size;
    out->success = (http_code >= 200 && http_code < 400);
    return out->success;
}

bool net_download_file(const char *url, const char *dest_path, NetWriteChunkCb chunk_cb, void *userdata, char *err_msg, size_t err_len) {
    (void)dest_path;
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err_msg) snprintf(err_msg, err_len, "cURL init failed");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chunk_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, userdata);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 40L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (err_msg) snprintf(err_msg, err_len, "%s", curl_easy_strerror(res));
        return false;
    }
    return true;
}

#else
/* =========================================================================
 * Native PS4 OrbisOS implementation using SceHttp / SceNet / SceSsl
 * ========================================================================= */
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

static int s_netMemId = 0;
static int s_sslCtxId = 0;
static int s_httpCtxId = 0;

static int skipSslCallback(int libsslId, unsigned int verifyErr, void * const sslCert[], int certNum, void *userArg) {
    (void)libsslId;
    (void)verifyErr;
    (void)sslCert;
    (void)certNum;
    (void)userArg;
    return 1; /* HTTP_SUCCESS */
}

bool net_init(void) {
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);

    if (s_netMemId == 0 || s_httpCtxId == 0) {
        sceNetInit();
        s_netMemId = sceNetPoolCreate("netPool", 64 * 1024, 0);
        if (s_netMemId < 0) return false;

        s_sslCtxId = sceSslInit(128 * 1024);
        if (s_sslCtxId < 0) return false;

        s_httpCtxId = sceHttpInit(s_netMemId, s_sslCtxId, 128 * 1024);
        if (s_httpCtxId < 0) return false;
    }
    return true;
}

void net_cleanup(void) {
    if (s_httpCtxId > 0) sceHttpTerm(s_httpCtxId);
    if (s_sslCtxId > 0) sceSslTerm(s_sslCtxId);
    if (s_netMemId > 0) sceNetPoolDestroy(s_netMemId);
    sceNetTerm();
}

static bool ps4_http_request(const char *url, int method, const char *post_data, size_t post_len, const char *content_type, const char *auth_bearer, NetResponse *out) {
    if (!out) return false;
    memset(out, 0, sizeof(NetResponse));

    if (!s_httpCtxId && !net_init()) {
        snprintf(out->error_msg, sizeof(out->error_msg), "sceHttp init failed");
        return false;
    }

    int tpl = sceHttpCreateTemplate(s_httpCtxId, "Mozilla/5.0 (PLAYSTATION 4; 1.00)", ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) {
        snprintf(out->error_msg, sizeof(out->error_msg), "sceHttpCreateTemplate failed (0x%X)", tpl);
        return false;
    }
    sceHttpsSetSslCallback(tpl, skipSslCallback, NULL);

    int conn = sceHttpCreateConnectionWithURL(tpl, url, 1);
    if (conn < 0) {
        sceHttpDeleteTemplate(tpl);
        snprintf(out->error_msg, sizeof(out->error_msg), "Connection failed (0x%X)", conn);
        return false;
    }

    int req = sceHttpCreateRequestWithURL(conn, method, url, post_len);
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        snprintf(out->error_msg, sizeof(out->error_msg), "Request creation failed (0x%X)", req);
        return false;
    }

    if (auth_bearer && auth_bearer[0]) {
        char auth_hdr[384];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", auth_bearer);
        sceHttpAddRequestHeader(req, "Authorization", auth_hdr, 0);
    }
    if (content_type && content_type[0]) {
        sceHttpAddRequestHeader(req, "Content-Type", content_type, 0);
    }

    int send_res = sceHttpSendRequest(req, post_data, post_len);
    if (send_res < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        snprintf(out->error_msg, sizeof(out->error_msg), "SendRequest failed (0x%X)", send_res);
        return false;
    }

    int32_t status_code = 0;
    sceHttpGetStatusCode(req, &status_code);
    out->status_code = status_code;

    /* Read response buffer */
    char buf[4096];
    char *accum = (char *)malloc(1);
    accum[0] = '\0';
    size_t total_read = 0;

    int read_bytes = 0;
    while ((read_bytes = sceHttpReadData(req, buf, sizeof(buf))) > 0) {
        char *new_ptr = (char *)realloc(accum, total_read + read_bytes + 1);
        if (!new_ptr) break;
        accum = new_ptr;
        memcpy(accum + total_read, buf, read_bytes);
        total_read += read_bytes;
        accum[total_read] = '\0';
    }

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);

    out->data = accum;
    out->size = total_read;
    out->success = (status_code >= 200 && status_code < 400);
    return out->success;
}

bool net_get(const char *url, const char *auth_bearer, NetResponse *out) {
    return ps4_http_request(url, ORBIS_METHOD_GET, NULL, 0, NULL, auth_bearer, out);
}

bool net_post(const char *url, const char *post_data, const char *content_type, const char *auth_bearer, NetResponse *out) {
    size_t len = post_data ? strlen(post_data) : 0;
    return ps4_http_request(url, ORBIS_METHOD_POST, post_data, len, content_type, auth_bearer, out);
}

bool net_download_file(const char *url, const char *dest_path, NetWriteChunkCb chunk_cb, void *userdata, char *err_msg, size_t err_len) {
    (void)dest_path;
    if (!s_httpCtxId && !net_init()) {
        if (err_msg) snprintf(err_msg, err_len, "sceHttp init failed");
        return false;
    }

    int tpl = sceHttpCreateTemplate(s_httpCtxId, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36", ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) {
        if (err_msg) snprintf(err_msg, err_len, "Template error (0x%X)", tpl);
        return false;
    }
    sceHttpsSetSslCallback(tpl, skipSslCallback, NULL);

    int conn = sceHttpCreateConnectionWithURL(tpl, url, 1);
    if (conn < 0) {
        sceHttpDeleteTemplate(tpl);
        if (err_msg) snprintf(err_msg, err_len, "Conn error (0x%X)", conn);
        return false;
    }

    int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url, 0);
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        if (err_msg) snprintf(err_msg, err_len, "Req error (0x%X)", req);
        return false;
    }

    int send_res = sceHttpSendRequest(req, NULL, 0);
    if (send_res < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        if (err_msg) snprintf(err_msg, err_len, "Send error (0x%X)", send_res);
        return false;
    }

    char buf[65536]; /* 64 KB buffer for high speed PS4 SATA transfer */
    int read_bytes = 0;
    while ((read_bytes = sceHttpReadData(req, buf, sizeof(buf))) > 0) {
        size_t written = chunk_cb(buf, 1, read_bytes, userdata);
        if (written != (size_t)read_bytes) {
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tpl);
            if (err_msg) snprintf(err_msg, err_len, "Download aborted");
            return false;
        }
    }

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
    return true;
}

#endif

void net_response_free(NetResponse *resp) {
    if (resp && resp->data) {
        free(resp->data);
        resp->data = NULL;
        resp->size = 0;
    }
}
