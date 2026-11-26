/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See pkg_install.h. Never logs filenames, IP addresses, MACs, or Content IDs. */
#include "pkg_install.h"
#include "files.h"
#include "pkg_appinst.h"
#include "pkg_stream.h"

#include <ctype.h>
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
#define APPINST_ERR_NO_SPACE_1 0x80990005u
#define APPINST_ERR_NO_SPACE_2 0x8099000fu
#define APPINST_ERR_CANCELED 0x80900013u

struct pkg_job {
    struct smb_server server;
    char share[81];
    char path[1024];
    char label[256];
};

static void (*g_log)(const char *);

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

static void extract_header_cid(const uint8_t hdr[PKG_HEADER_BYTES], char out[48])
{
    size_t i;

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

static void *pkg_install_thread(void *arg)
{
    struct pkg_job *j = arg;
    struct smb_session *session = NULL;
    void *file = NULL;
    struct smb_entry entry;
    struct pkg_stream *stream = NULL;
    uint8_t hdr[PKG_HEADER_BYTES];
    char header_cid[48] = "", cid[48] = "", url[256], content_name[128], detail[32];
    const char *code = NULL;
    uint32_t sony_err = 0;
    long started_ms, last_active_ms, smb_err_at = 0;
    uint64_t last_progress = 0;
    int tick, err;

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
    if (err || hdr[0] != 0x7f || hdr[1] != 'C' || hdr[2] != 'N' || hdr[3] != 'T') {
        wipe(&j->server, sizeof j->server);
        smb_close_file(session, file);
        smb_disconnect(session);
        files_job_end(err ? smb_error_code(err) : "bad_pkg");
        free(j);
        return NULL;
    }
    extract_header_cid(hdr, header_cid);

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

    pkg_stream_url(stream, url, sizeof url);
    derive_content_name(j->label, content_name, sizeof content_name);
    code = pkg_appinst_start(url, content_name, cid, sizeof cid, &sony_err);
    if (code) {
        if (sony_err) {
            snprintf(detail, sizeof detail, "0x%08x", sony_err);
            files_job_detail(detail);
        }
        pkg_stream_stop(stream);
        files_job_end(code);
        free(j);
        return NULL;
    }
    if (!cid[0]) snprintf(cid, sizeof cid, "%s", header_cid);

    started_ms = mono_ms();
    last_active_ms = started_ms;
    while (!files_job_canceled()) {
        struct pkg_appinst_status st;
        uint64_t served = pkg_stream_served(stream);
        uint64_t capped_served = served > entry.size ? entry.size : served;
        uint64_t progress = capped_served;
        int poll_rc = pkg_appinst_poll(cid, &st);
        long now = mono_ms();
        int clients = pkg_stream_active_clients(stream);
        int smb_err = pkg_stream_last_smb_error(stream);

        if (poll_rc == 0 && st.downloaded > 0) progress = st.downloaded > entry.size ? entry.size : st.downloaded;
        if (progress > last_progress) {
            last_progress = progress;
            last_active_ms = now;
        }
        if (progress) files_job_set(progress);

        if (poll_rc == 0) {
            if (st.state == PKG_STATE_PLAYABLE) {
                files_job_set(entry.size);
                files_job_detail("");
                code = NULL;
                break;
            }
            if (st.state == PKG_STATE_PROMOTING) {
                files_job_set(entry.size);
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
