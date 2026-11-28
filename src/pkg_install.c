/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See pkg_install.h. Never logs filenames, IP addresses, MACs, or Content IDs. */
#include "pkg_install.h"
#include "files.h"
#include "pkg_appinst.h"
#include "pkg_stream.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define PKG_HEADER_BYTES 128
#define PKG_CID_OFFSET 0x40
#define PKG_CID_MAX 36
#define PKG_MIN_BYTES 512
#define PKG_POLL_SLICE_US 50000
#define PKG_POLL_TICKS 5            /* 250 ms between status queries */
#define PKG_GRACE_MS 15000L
#define PKG_STALL_MS 60000L
#define PKG_SMB_ERR_WAIT_MS 5000L
#define PKG_NEED_TIMEOUT_MS 1000
#define PKG_MAX_BROWSER_CHUNK (8 * 1024 * 1024)
#define APPINST_ERR_NO_SPACE_1 0x80990005u
#define APPINST_ERR_NO_SPACE_2 0x8099000fu
#define APPINST_ERR_CANCELED 0x80900013u

struct pkg_job {
    struct smb_server server;
    char share[81];
    char path[1024];
    char label[256];
};

struct browser_pkg_job {
    char label[256];
    char header_cid[48];
    uint64_t size;
    int64_t mtime;
};

static void (*g_log)(const char *);
static pthread_mutex_t g_browser_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_browser_done = PTHREAD_COND_INITIALIZER;
static struct pkg_stream *g_browser_stream;
static char g_browser_label[256];
static uint64_t g_browser_size;
static int g_browser_starting;
static int g_browser_refs;

void pkg_install_set_log(void (*log)(const char *line))
{
    g_log = log;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;

    while (n--) *q++ = 0;
}

static long mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *base_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

int pkg_install_is_pkg_name(const char *path)
{
    const char *name;
    size_t len;

    if (!path || !path[0]) return 0;
    name = base_of(path);
    len = strlen(name);
    if (len > 4 && !strcasecmp(name + len - 4, ".pkg")) return 1;
    if (len > 5 && !strcasecmp(name + len - 5, ".fpkg")) return 1;
    return 0;
}

static void derive_content_name(const char *label, char *out, size_t cap)
{
    size_t len = strlen(label);

    if (len > 5 && !strcasecmp(label + len - 5, ".fpkg")) len -= 5;
    else if (len > 4 && !strcasecmp(label + len - 4, ".pkg")) len -= 4;
    if (len >= cap) len = cap - 1;
    memcpy(out, label, len);
    out[len] = 0;
    if (!out[0]) snprintf(out, cap, "Package");
}

static int is_valid_pkg_magic(const uint8_t hdr[PKG_HEADER_BYTES])
{
    if (hdr[0] != 0x7f) return 0;
    if (hdr[1] == 'C' && hdr[2] == 'N' && hdr[3] == 'T') return 1;
    if (hdr[1] == 'F' && hdr[2] == 'I' && hdr[3] == 'H') return 1;
    return 0;
}

static void extract_header_cid(const uint8_t hdr[PKG_HEADER_BYTES], char out[48])
{
    size_t i;

    if (hdr[1] != 'C') {
        out[0] = 0;
        return;
    }
    for (i = 0; i < PKG_CID_MAX; i++) {
        uint8_t c = hdr[PKG_CID_OFFSET + i];

        if (c < 0x21 || c > 0x7e) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

static int read_pkg_header(struct smb_session *s, void *f, uint8_t hdr[PKG_HEADER_BYTES])
{
    size_t have = 0;

    while (have < PKG_HEADER_BYTES) {
        long long n = smb_read(s, f, hdr + have, PKG_HEADER_BYTES - have, have);

        if (n < 0) return (int)n;
        if (n == 0) return SMB_FAILED;
        have += (size_t)n;
    }
    return 0;
}

/* Shared Sony installer invocation and status-polling loop for both SMB and Browser streams. */
static const char *run_appinst_loop(struct pkg_stream *stream, uint64_t size, const char *label, const char *header_cid)
{
    char cid[48] = "", url[256], content_name[128], detail[32];
    const char *code = NULL;
    uint32_t sony_err = 0;
    long started_ms, last_active_ms, smb_err_at = 0;
    uint64_t last_progress = 0;
    int tick;

    pkg_stream_url(stream, url, sizeof url);
    derive_content_name(label, content_name, sizeof content_name);
    code = pkg_appinst_start(url, content_name, cid, sizeof cid, &sony_err);
    if (code) {
        if (sony_err) {
            snprintf(detail, sizeof detail, "0x%08x", sony_err);
            files_job_detail(detail);
        }
        return code;
    }
    if (!cid[0] && header_cid) snprintf(cid, sizeof cid, "%s", header_cid);

    started_ms = mono_ms();
    last_active_ms = started_ms;
    while (!files_job_canceled()) {
        struct pkg_appinst_status st;
        uint64_t served = pkg_stream_served(stream);
        uint64_t capped_served = served > size ? size : served;
        uint64_t progress = capped_served;
        int poll_rc = pkg_appinst_poll(cid, &st);
        long now = mono_ms();
        int clients = pkg_stream_active_clients(stream);
        int smb_err = pkg_stream_last_smb_error(stream);

        if (poll_rc == 0 && st.downloaded > 0) progress = st.downloaded > size ? size : st.downloaded;
        if (progress > last_progress) {
            last_progress = progress;
            last_active_ms = now;
        }
        if (progress) files_job_set(progress);

        if (poll_rc == 0) {
            if (st.state == PKG_STATE_PLAYABLE) {
                files_job_set(size);
                files_job_detail("");
                code = NULL;
                break;
            }
            if (st.state == PKG_STATE_PROMOTING) {
                files_job_set(size);
                files_job_detail("promoting");
                last_active_ms = now;
            } else if (st.state == PKG_STATE_ERROR) {
                if (st.error_code) {
                    char line[64];

                    snprintf(detail, sizeof detail, "0x%08x", st.error_code);
                    files_job_detail(detail);
                    snprintf(line, sizeof line, "pkg: install error 0x%08x", st.error_code);
                    if (g_log) g_log(line);
                }
                if (st.error_code == APPINST_ERR_NO_SPACE_1 || st.error_code == APPINST_ERR_NO_SPACE_2) code = "no_space";
                else if (st.error_code == APPINST_ERR_CANCELED) code = "canceled";
                else code = "install_failed";
                break;
            } else if (st.state == PKG_STATE_TRANSFERRING && clients > 0) {
                last_active_ms = now;
            }
        } else if (clients > 0) {
            last_active_ms = now;
        }

        if (smb_err && clients == 0) {
            if (!smb_err_at) smb_err_at = now;
            if (now - smb_err_at >= PKG_SMB_ERR_WAIT_MS) {
                code = smb_error_code(smb_err);
                break;
            }
        } else {
            smb_err_at = 0;
        }

        if (now - started_ms >= PKG_GRACE_MS && now - last_active_ms >= PKG_STALL_MS) {
            code = smb_err ? smb_error_code(smb_err) : "install_failed";
            break;
        }

        for (tick = 0; tick < PKG_POLL_TICKS && !files_job_canceled(); tick++) usleep(PKG_POLL_SLICE_US);
    }

    if (files_job_canceled()) {
        pkg_appinst_cancel(cid);
        code = "canceled";
    }
    if (g_log) {
        char msg[128];

        snprintf(msg, sizeof msg, "pkg: stream served=%" PRIu64 " code=%s", pkg_stream_served(stream), code ? code : "ok");
        g_log(msg);
    }
    return code;
}

static void *pkg_install_thread(void *arg)
{
    struct pkg_job *j = arg;
    struct smb_session *session = NULL;
    void *file = NULL;
    struct smb_entry entry;
    struct pkg_stream *stream = NULL;
    uint8_t hdr[PKG_HEADER_BYTES];
    char header_cid[48] = "";
    const char *code = NULL;
    int err;

    err = smb_connect_share(&j->server, j->share, &session, NULL);
    if (err) {
        wipe(&j->server, sizeof j->server);
        files_job_end(smb_error_code(err));
        free(j);
        return NULL;
    }
    err = smb_stat(session, j->path, &entry);
    if (err) {
        wipe(&j->server, sizeof j->server);
        smb_disconnect(session);
        files_job_end(smb_error_code(err));
        free(j);
        return NULL;
    }
    if (entry.folder) {
        wipe(&j->server, sizeof j->server);
        smb_disconnect(session);
        files_job_end("is_folder");
        free(j);
        return NULL;
    }
    if (entry.size < PKG_MIN_BYTES) {
        wipe(&j->server, sizeof j->server);
        smb_disconnect(session);
        files_job_end("bad_pkg");
        free(j);
        return NULL;
    }
    err = smb_open_file(session, j->path, &file);
    if (err) {
        wipe(&j->server, sizeof j->server);
        smb_disconnect(session);
        files_job_end(smb_error_code(err));
        free(j);
        return NULL;
    }
    err = read_pkg_header(session, file, hdr);
    if (err || !is_valid_pkg_magic(hdr)) {
        wipe(&j->server, sizeof j->server);
        smb_close_file(session, file);
        smb_disconnect(session);
        files_job_end(err ? smb_error_code(err) : "bad_pkg");
        free(j);
        return NULL;
    }
    extract_header_cid(hdr, header_cid);
    if (g_log) {
        char msg[128];

        snprintf(msg, sizeof msg, "pkg: smb magic=%02x%02x%02x%02x size=%" PRIu64, hdr[0], hdr[1], hdr[2], hdr[3], entry.size);
        g_log(msg);
    }

    (void)files_job_size(entry.size);
    if (files_job_canceled()) {
        wipe(&j->server, sizeof j->server);
        smb_close_file(session, file);
        smb_disconnect(session);
        files_job_end("canceled");
        free(j);
        return NULL;
    }

    if (pkg_stream_start(&j->server, j->share, j->path, session, file, entry.size, entry.mtime, &stream) != 0) {
        wipe(&j->server, sizeof j->server);
        smb_close_file(session, file);
        smb_disconnect(session);
        files_job_end("failed");
        free(j);
        return NULL;
    }
    wipe(&j->server, sizeof j->server);

    code = run_appinst_loop(stream, entry.size, j->label, header_cid);
    pkg_stream_stop(stream);
    files_job_end(code);
    free(j);
    return NULL;
}

int pkg_install_smb(const struct smb_server *server, const char *share, const char *path, const char *client, struct http_stream *stream)
{
    struct pkg_job *j = calloc(1, sizeof *j);
    int started = 0, result;

    if (!j) return http_respond(stream, 503, "application/json", "{\"error\":\"no_memory\"}");
    j->server = *server;
    snprintf(j->share, sizeof j->share, "%s", share);
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->label, sizeof j->label, "%s", base_of(path));
    result = files_job_run(stream, "pkg", j->label, NULL, client ? client : "", pkg_install_thread, j, &started);
    if (!started) {
        wipe(j, sizeof *j);
        free(j);
    }
    return result;
}

/* ---- Browser local PKG streaming ------------------------------------------- */

static void *pkg_browser_thread(void *arg)
{
    struct browser_pkg_job *j = arg;
    struct pkg_stream *stream = NULL;
    const char *code = NULL;

    (void)files_job_size(j->size);
    if (files_job_canceled()) {
        pthread_mutex_lock(&g_browser_lock);
        g_browser_starting = 0;
        pthread_mutex_unlock(&g_browser_lock);
        files_job_end("canceled");
        free(j);
        return NULL;
    }
    if (pkg_stream_start_browser(j->size, j->mtime, &stream) != 0) {
        pthread_mutex_lock(&g_browser_lock);
        g_browser_starting = 0;
        pthread_mutex_unlock(&g_browser_lock);
        files_job_end("failed");
        free(j);
        return NULL;
    }

    pthread_mutex_lock(&g_browser_lock);
    g_browser_stream = stream;
    snprintf(g_browser_label, sizeof g_browser_label, "%s", j->label);
    g_browser_size = j->size;
    g_browser_starting = 0;
    pthread_mutex_unlock(&g_browser_lock);

    code = run_appinst_loop(stream, j->size, j->label, j->header_cid);

    pthread_mutex_lock(&g_browser_lock);
    g_browser_stream = NULL;
    g_browser_label[0] = 0;
    g_browser_size = 0;
    pkg_stream_signal_stop(stream);
    while (g_browser_refs > 0) pthread_cond_wait(&g_browser_done, &g_browser_lock);
    pthread_mutex_unlock(&g_browser_lock);

    pkg_stream_stop(stream);
    files_job_end(code);
    free(j);
    return NULL;
}

static struct pkg_stream *browser_stream_acquire(void)
{
    struct pkg_stream *st;

    pthread_mutex_lock(&g_browser_lock);
    st = g_browser_stream;
    if (st) g_browser_refs++;
    pthread_mutex_unlock(&g_browser_lock);
    return st;
}

static void browser_stream_release(void)
{
    pthread_mutex_lock(&g_browser_lock);
    if (--g_browser_refs == 0) pthread_cond_broadcast(&g_browser_done);
    pthread_mutex_unlock(&g_browser_lock);
}

static int read_stream_exact(const struct http_request *r, struct http_stream *stream, uint8_t *buf, size_t len)
{
    size_t have = r->body_length;

    if (have > len) return -1;
    if (have) memcpy(buf, r->body, have);
    while (have < len) {
        ssize_t n = stream->read(stream->context, buf + have, len - have);

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    return 0;
}

static int error_json(struct http_stream *stream, int status, const char *code)
{
    char json[64];

    snprintf(json, sizeof json, "{\"error\":\"%s\"}", code);
    return http_respond(stream, status, "application/json", json);
}

static int valid_browser_name(const char *name)
{
    const char *p;

    if (!name[0]) return 0;
    for (p = name; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return 0;
    return 1;
}

static void parse_client_q(const struct http_request *r, char client[17])
{
    size_t i;

    client[0] = 0;
    if (http_query_text(r->query, "client", client, 17) != 0) client[0] = 0;
    for (i = 0; client[i]; i++)
        if (!((client[i] >= '0' && client[i] <= '9') || (client[i] >= 'a' && client[i] <= 'z'))) {
            client[0] = 0;
            break;
        }
}

static int browser_start(const struct http_request *r, struct http_stream *stream)
{
    char name[256], client[17];
    uint64_t size = http_query_number(r->query, "size");
    int64_t mtime = (int64_t)http_query_number(r->query, "mtime");
    uint8_t hdr[PKG_HEADER_BYTES];
    struct browser_pkg_job *j;
    int started = 0, result;

    if (http_query_text(r->query, "name", name, sizeof name) != 0 || !valid_browser_name(name))
        return error_json(stream, 400, "bad_path");
    if (!pkg_install_is_pkg_name(name)) return error_json(stream, 400, "not_pkg");
    if (size < PKG_MIN_BYTES || !r->has_length || r->length != PKG_HEADER_BYTES) return error_json(stream, 400, "bad_pkg");
    if (read_stream_exact(r, stream, hdr, sizeof hdr) != 0) return error_json(stream, 400, "bad_request");
    if (!is_valid_pkg_magic(hdr)) return error_json(stream, 400, "bad_pkg");

    j = calloc(1, sizeof *j);
    if (!j) return error_json(stream, 503, "no_memory");
    if (g_log) {
        char msg[128];

        snprintf(msg, sizeof msg, "pkg: browser magic=%02x%02x%02x%02x size=%" PRIu64, hdr[0], hdr[1], hdr[2], hdr[3], size);
        g_log(msg);
    }
    snprintf(j->label, sizeof j->label, "%s", name);
    extract_header_cid(hdr, j->header_cid);
    j->size = size;
    j->mtime = mtime;
    parse_client_q(r, client);
    pthread_mutex_lock(&g_browser_lock);
    g_browser_starting = 1;
    pthread_mutex_unlock(&g_browser_lock);
    result = files_job_run(stream, "pkg", j->label, NULL, client, pkg_browser_thread, j, &started);
    if (!started) {
        pthread_mutex_lock(&g_browser_lock);
        g_browser_starting = 0;
        pthread_mutex_unlock(&g_browser_lock);
        free(j);
    }
    return result;
}

static int browser_need(struct http_stream *stream)
{
    struct pkg_stream *st;
    uint64_t req_id = 0, offset = 0;
    size_t len = 0;
    int rc, starting = 0;
    char json[128];

    pthread_mutex_lock(&g_browser_lock);
    st = g_browser_stream;
    if (st) g_browser_refs++;
    else starting = g_browser_starting;
    pthread_mutex_unlock(&g_browser_lock);
    if (!st) {
        if (starting) {
            usleep(50000);
            return http_respond(stream, 200, "application/json", "{\"idle\":true}");
        }
        return http_respond(stream, 200, "application/json", "{\"done\":true}");
    }
    rc = pkg_stream_pull_need(st, &req_id, &offset, &len, PKG_NEED_TIMEOUT_MS);
    browser_stream_release();
    if (rc < 0) return http_respond(stream, 200, "application/json", "{\"done\":true}");
    if (rc == 0) return http_respond(stream, 200, "application/json", "{\"idle\":true}");
    snprintf(json, sizeof json, "{\"req\":%" PRIu64 ",\"offset\":%" PRIu64 ",\"len\":%zu}", req_id, offset, len);
    return http_respond(stream, 200, "application/json", json);
}

static int browser_chunk(const struct http_request *r, struct http_stream *stream)
{
    uint64_t req_id = http_query_number(r->query, "req");
    uint8_t *buf;
    struct pkg_stream *st;
    int rc;

    if (!req_id || !r->has_length || r->length == 0 || r->length > PKG_MAX_BROWSER_CHUNK)
        return error_json(stream, 400, "bad_request");
    buf = malloc((size_t)r->length);
    if (!buf) return error_json(stream, 503, "no_memory");
    if (read_stream_exact(r, stream, buf, (size_t)r->length) != 0) {
        free(buf);
        return error_json(stream, 400, "bad_request");
    }
    st = browser_stream_acquire();
    if (!st) {
        free(buf);
        return error_json(stream, 409, "canceled");
    }
    rc = pkg_stream_pull_give(st, req_id, buf, (size_t)r->length);
    browser_stream_release();
    if (rc != 0) {
        free(buf);
        return error_json(stream, 409, "canceled");
    }
    return http_respond(stream, 200, "application/json", "{}");
}

static void json_escape(char *dst, size_t cap, const char *s)
{
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)s;

    if (!cap) return;
    while (*p && used + 1 < cap) {
        if (*p == '"' || *p == '\\') {
            if (used + 2 >= cap) break;
            dst[used++] = '\\';
            dst[used++] = (char)*p++;
        } else if (*p < 0x20) {
            if (used + 6 >= cap) break;
            snprintf(dst + used, cap - used, "\\u%04x", *p++);
            used += 6;
        } else {
            dst[used++] = (char)*p++;
        }
    }
    dst[used] = 0;
}

static int browser_status(struct http_stream *stream)
{
    char label[256], esc[600], json[700];
    uint64_t size = 0, done = 0;
    int active = 0;

    pthread_mutex_lock(&g_browser_lock);
    if (g_browser_stream) {
        active = 1;
        snprintf(label, sizeof label, "%s", g_browser_label);
        size = g_browser_size;
        done = pkg_stream_served(g_browser_stream);
    }
    pthread_mutex_unlock(&g_browser_lock);
    if (!active) return http_respond(stream, 200, "application/json", "{\"active\":false}");
    json_escape(esc, sizeof esc, label);
    snprintf(json, sizeof json, "{\"active\":true,\"name\":\"%s\",\"size\":%" PRIu64 ",\"done\":%" PRIu64 "}", esc, size, done);
    return http_respond(stream, 200, "application/json", json);
}

int pkg_install_route(const struct http_request *r, struct http_stream *stream)
{
    const char *path = r->path;
    int post = !strcmp(r->method, "POST");

    if (strncmp(path, "/pkg/browser/", 13)) return HTTP_NOT_MINE;
    path += 13;
    if (post && !strcmp(path, "start")) return browser_start(r, stream);
    if (!post && !strcmp(path, "status")) return browser_status(stream);
    if (!post && !strcmp(path, "need")) return browser_need(stream);
    if (post && !strcmp(path, "chunk")) return browser_chunk(r, stream);
    return HTTP_NOT_MINE;
}
