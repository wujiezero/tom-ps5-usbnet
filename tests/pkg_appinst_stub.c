/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test stub for pkg_appinst.h: connects over real TCP loopback sockets to
 * pkg_stream's HTTP/1.1 Range server (issuing HEAD, sequential keep-alive Range GETs,
 * and concurrent Range GETs), verifies the full byte stream, and drives realistic
 * transferring -> promoting -> playable state transitions. */
#include "pkg_appinst.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Controllable test hooks. */
int pkg_stub_mode = 0;          /* 0 = ok, 1 = refuse start, 2 = error mid-transfer, 3 = no_api */
uint32_t pkg_stub_error = 0x80b22403u;
int pkg_stub_delay_ms = 0;
uint64_t pkg_stub_verified_bytes = 0;
uint32_t pkg_stub_hash = 0;

static struct {
    pthread_mutex_t lock;
    int active;
    int stop;
    int started;
    pthread_t thread;
    char url[512];
    char host[64];
    uint16_t port;
    char path[256];
    char cid[48];
    struct pkg_appinst_status status;
} g_stub = {.lock = PTHREAD_MUTEX_INITIALIZER};

static uint32_t fnv1a_step(uint32_t h, const uint8_t *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= buf[i];
        h *= 16777619u;
    }
    return h;
}

static int parse_loopback_url(const char *url, char *host, size_t hcap, uint16_t *port, char *path, size_t pcap)
{
    const char *p, *colon, *slash;
    unsigned long pv;

    if (!url || strncmp(url, "http://", 7) != 0) return -1;
    p = url + 7;
    colon = strchr(p, ':');
    slash = strchr(p, '/');
    if (!colon || !slash || colon > slash) return -1;
    if ((size_t)(colon - p) >= hcap) return -1;
    memcpy(host, p, (size_t)(colon - p));
    host[colon - p] = 0;
    pv = strtoul(colon + 1, NULL, 10);
    if (!pv || pv > 65535) return -1;
    *port = (uint16_t)pv;
    snprintf(path, pcap, "%s", slash);
    return 0;
}

static int tcp_connect(const char *host, uint16_t port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all_fd(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int read_http_head(int fd, char *hdr, size_t cap, char *extra, size_t extra_cap, size_t *extra_len)
{
    size_t used = 0;

    *extra_len = 0;
    while (used + 1 < cap) {
        ssize_t n = read(fd, hdr + used, cap - 1 - used);
        char *end;

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
        hdr[used] = 0;
        end = strstr(hdr, "\r\n\r\n");
        if (end) {
            size_t hdr_bytes = (size_t)(end + 4 - hdr);
            size_t tail = used - hdr_bytes;

            if (tail > extra_cap) tail = extra_cap;
            if (tail) memcpy(extra, hdr + hdr_bytes, tail);
            *extra_len = tail;
            hdr[hdr_bytes] = 0;
            return 0;
        }
    }
    return -1;
}

static int find_header_u64(const char *hdr, const char *key, uint64_t *val)
{
    size_t klen = strlen(key);
    const char *p = hdr;

    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        if (line_len > klen + 1 && strncasecmp(p, key, klen) == 0 && p[klen] == ':') {
            *val = strtoull(p + klen + 1, NULL, 10);
            return 0;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return -1;
}

struct worker_range {
    char host[64];
    uint16_t port;
    char path[256];
    uint64_t start;
    uint64_t len;
    uint32_t hash;
    int ok;
};

static int fetch_range_fd(int fd, const char *path, uint64_t start, uint64_t len, int keep_alive, uint8_t *out, uint32_t *hash_io)
{
    char req[512], hdr[4096], extra[4096];
    size_t extra_len = 0, got = 0;
    int req_len, status = 0;
    uint64_t clen = 0;

    req_len = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=%" PRIu64 "-%" PRIu64 "\r\nConnection: %s\r\n\r\n",
        path, start, start + len - 1, keep_alive ? "keep-alive" : "close");
    if (write_all_fd(fd, req, (size_t)req_len) != 0) return -1;
    if (read_http_head(fd, hdr, sizeof hdr, extra, sizeof extra, &extra_len) != 0) return -1;
    if (sscanf(hdr, "HTTP/1.%*d %d", &status) != 1 || status != 206) return -1;
    if (find_header_u64(hdr, "Content-Length", &clen) != 0 || clen != len) return -1;
    if (extra_len) {
        size_t take = extra_len > len ? (size_t)len : extra_len;

        if (out) memcpy(out, extra, take);
        if (hash_io) *hash_io = fnv1a_step(*hash_io, (const uint8_t *)extra, take);
        got += take;
    }
    while (got < len) {
        uint8_t buf[32768];
        size_t want = len - got < sizeof buf ? (size_t)(len - got) : sizeof buf;
        ssize_t n = read(fd, buf, want);

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        if (out) memcpy(out + got, buf, (size_t)n);
        if (hash_io) *hash_io = fnv1a_step(*hash_io, buf, (size_t)n);
        got += (size_t)n;
    }
    return 0;
}

static void *concurrent_tail(void *arg)
{
    struct worker_range *r = arg;
    int fd = tcp_connect(r->host, r->port);
    uint8_t *buf = malloc((size_t)r->len);

    if (fd >= 0 && buf && fetch_range_fd(fd, r->path, r->start, r->len, 0, buf, NULL) == 0) {
        r->hash = fnv1a_step(2166136261u, buf, (size_t)r->len);
        r->ok = 1;
    }
    free(buf);
    if (fd >= 0) close(fd);
    return NULL;
}

static void *stub_worker(void *unused)
{
    int fd, mode, delay_ms;
    char host[64], path[256], req[512], hdr[4096], extra[4096];
    uint16_t port;
    size_t extra_len = 0;
    uint64_t total = 0, offset = 0;
    uint32_t hash = 2166136261u;
    int status = 0;
    struct worker_range tail;
    pthread_t tail_thr;
    int use_tail = 0;

    (void)unused;
    pthread_mutex_lock(&g_stub.lock);
    snprintf(host, sizeof host, "%s", g_stub.host);
    port = g_stub.port;
    snprintf(path, sizeof path, "%s", g_stub.path);
    mode = pkg_stub_mode;
    delay_ms = pkg_stub_delay_ms;
    pthread_mutex_unlock(&g_stub.lock);

    fd = tcp_connect(host, port);
    if (fd < 0) goto fail_err;
    snprintf(req, sizeof req, "HEAD %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n", path);
    if (write_all_fd(fd, req, strlen(req)) != 0 || read_http_head(fd, hdr, sizeof hdr, extra, sizeof extra, &extra_len) != 0) goto fail_close;
    if (sscanf(hdr, "HTTP/1.%*d %d", &status) != 1 || status != 200 || find_header_u64(hdr, "Content-Length", &total) != 0 || total == 0)
        goto fail_close;
    if (!strstr(hdr, "Accept-Ranges: bytes") || !strstr(hdr, "ETag:") || !strstr(hdr, "Last-Modified:")) goto fail_close;

    pthread_mutex_lock(&g_stub.lock);
    g_stub.status.total = total;
    pthread_mutex_unlock(&g_stub.lock);

    if (mode == 2) {
        uint64_t first = total > 64 ? 64 : total;

        if (fetch_range_fd(fd, path, 0, first, 0, NULL, &hash) == 0) {
            pthread_mutex_lock(&g_stub.lock);
            g_stub.status.downloaded = first;
            g_stub.status.error_code = pkg_stub_error;
            g_stub.status.state = PKG_STATE_ERROR;
            pthread_mutex_unlock(&g_stub.lock);
        } else {
            goto fail_close;
        }
        close(fd);
        return NULL;
    }

    /* When large enough and fast, run a concurrent second Range reader for the last 64 KiB. */
    if (total >= 128 * 1024 && delay_ms == 0) {
        memset(&tail, 0, sizeof tail);
        snprintf(tail.host, sizeof tail.host, "%s", host);
        tail.port = port;
        snprintf(tail.path, sizeof tail.path, "%s", path);
        tail.start = total - 65536;
        tail.len = 65536;
        if (pthread_create(&tail_thr, NULL, concurrent_tail, &tail) == 0) use_tail = 1;
    }

    while (offset < total) {
        uint64_t chunk = total - offset;
        uint8_t *tmp;

        if (__atomic_load_n(&g_stub.stop, __ATOMIC_RELAXED)) break;
        if (chunk > 65536) chunk = 65536;
        tmp = malloc((size_t)chunk);
        if (!tmp || fetch_range_fd(fd, path, offset, chunk, 1, tmp, &hash) != 0) {
            free(tmp);
            if (use_tail) pthread_join(tail_thr, NULL);
            goto fail_close;
        }
        free(tmp);
        offset += chunk;
        pthread_mutex_lock(&g_stub.lock);
        g_stub.status.downloaded = offset;
        pthread_mutex_unlock(&g_stub.lock);
        if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);
    }
    close(fd);
    if (use_tail) {
        pthread_join(tail_thr, NULL);
        if (!tail.ok) goto fail_err;
    }
    if (__atomic_load_n(&g_stub.stop, __ATOMIC_RELAXED)) return NULL;

    pthread_mutex_lock(&g_stub.lock);
    pkg_stub_verified_bytes = offset;
    pkg_stub_hash = hash;
    g_stub.status.state = PKG_STATE_PROMOTING;
    pthread_mutex_unlock(&g_stub.lock);
    usleep(30000);
    if (__atomic_load_n(&g_stub.stop, __ATOMIC_RELAXED)) return NULL;
    pthread_mutex_lock(&g_stub.lock);
    g_stub.status.state = PKG_STATE_PLAYABLE;
    pthread_mutex_unlock(&g_stub.lock);
    return NULL;

fail_close:
    close(fd);
fail_err:
    pthread_mutex_lock(&g_stub.lock);
    if (!g_stub.stop) {
        g_stub.status.state = PKG_STATE_ERROR;
        g_stub.status.error_code = pkg_stub_error ? pkg_stub_error : 0x80990001u;
    }
    pthread_mutex_unlock(&g_stub.lock);
    return NULL;
}

static void join_previous(void)
{
    pthread_t thr;
    int need = 0;

    pthread_mutex_lock(&g_stub.lock);
    if (g_stub.started) {
        g_stub.stop = 1;
        thr = g_stub.thread;
        g_stub.started = 0;
        need = 1;
    }
    pthread_mutex_unlock(&g_stub.lock);
    if (need) pthread_join(thr, NULL);
}

const char *pkg_appinst_start(const char *uri, const char *content_name, char *out_content_id, size_t out_cap, uint32_t *out_err)
{
    (void)content_name;
    join_previous();
    if (out_content_id && out_cap) out_content_id[0] = 0;
    if (out_err) *out_err = 0;
    if (pkg_stub_mode == 3) return "no_install_api";
    if (pkg_stub_mode == 1) {
        if (out_err) *out_err = pkg_stub_error;
        if (pkg_stub_error == 0x80990005u || pkg_stub_error == 0x8099000fu) return "no_space";
        return "install_refused";
    }
    pthread_mutex_lock(&g_stub.lock);
    memset(&g_stub.status, 0, sizeof g_stub.status);
    g_stub.stop = 0;
    g_stub.active = 1;
    g_stub.status.state = PKG_STATE_TRANSFERRING;
    snprintf(g_stub.url, sizeof g_stub.url, "%s", uri ? uri : "");
    snprintf(g_stub.cid, sizeof g_stub.cid, "UP0000-STUB00001_00-0000000000000001");
    if (parse_loopback_url(g_stub.url, g_stub.host, sizeof g_stub.host, &g_stub.port, g_stub.path, sizeof g_stub.path) != 0) {
        g_stub.active = 0;
        pthread_mutex_unlock(&g_stub.lock);
        if (out_err) *out_err = 0x80990001u;
        return "install_refused";
    }
    if (out_content_id && out_cap) snprintf(out_content_id, out_cap, "%s", g_stub.cid);
    if (pthread_create(&g_stub.thread, NULL, stub_worker, NULL) != 0) {
        g_stub.active = 0;
        pthread_mutex_unlock(&g_stub.lock);
        return "no_thread";
    }
    g_stub.started = 1;
    pthread_mutex_unlock(&g_stub.lock);
    return NULL;
}

int pkg_appinst_poll(const char *content_id, struct pkg_appinst_status *out)
{
    if (!content_id || !out) return -1;
    pthread_mutex_lock(&g_stub.lock);
    if (!g_stub.active) {
        pthread_mutex_unlock(&g_stub.lock);
        return -1;
    }
    *out = g_stub.status;
    pthread_mutex_unlock(&g_stub.lock);
    return 0;
}

void pkg_appinst_cancel(const char *content_id)
{
    (void)content_id;
    join_previous();
    pthread_mutex_lock(&g_stub.lock);
    g_stub.active = 0;
    pthread_mutex_unlock(&g_stub.lock);
}
