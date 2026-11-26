/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See pkg_stream.h. Raw BSD loopback sockets on 127.0.0.1, supporting HTTP/1.1
 * keep-alive, HEAD, single byte Range requests (206 Partial Content / 416), and a
 * per-stream SMB handle pool with transient SMB_DROPPED/SMB_TIMEOUT reconnects. */
#include "pkg_stream.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define STREAM_MAX_WORKERS 8
#define STREAM_BACKLOG 16
#define STREAM_STACK (1024 * 1024)
#define STREAM_HEADER_BYTES 8192
#define STREAM_POLL_MS 200
#define STREAM_IO_TIMEOUT_S 30
#define STREAM_SNDBUF (512 * 1024)
#define STREAM_DEFAULT_CHUNK (256 * 1024)
#define STREAM_MAX_CHUNK (1024 * 1024)
#define STREAM_MIN_CHUNK (64 * 1024)
#define STREAM_SMB_RETRIES 2
#define STREAM_BROWSER_CHUNK (2 * 1024 * 1024)
#define STREAM_PULL_SLOTS 4
#define STREAM_PULL_WAIT_MS 30000

enum pull_state { PULL_FREE, PULL_NEEDED, PULL_INFLIGHT, PULL_READY, PULL_ERROR };

struct pull_slot {
    enum pull_state state;
    uint64_t id;
    uint64_t offset;
    size_t len;
    uint8_t *data;
};

struct smb_slot {
    struct smb_session *session;
    void *file;
    int in_use;
};

struct pkg_stream {
    int browser_mode;
    struct smb_server server;
    char share[128];
    char path[1024];
    uint64_t size;
    int64_t mtime;
    uint16_t port;
    char url_path[96];
    char etag[48];
    char last_modified[48];

    int listener;
    pthread_t accept_thread;
    int accept_started;
    int stop;
    int last_smb_error;
    uint64_t served;

    pthread_mutex_t lock;
    pthread_cond_t done_cond;
    int active_workers;
    int active_fds[STREAM_MAX_WORKERS];

    pthread_mutex_t pool_lock;
    struct smb_slot pool[STREAM_MAX_WORKERS];

    pthread_mutex_t pull_lock;
    pthread_cond_t pull_cond;
    struct pull_slot pulls[STREAM_PULL_SLOTS];
};

static uint64_t g_next_pull_id = 1;

struct stream_worker_arg {
    struct pkg_stream *st;
    int fd;
};

static void wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;

    while (n--) *q++ = 0;
}

static void make_token(char out[17])
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    struct timespec ts;

    if (fd >= 0) {
        ssize_t n = read(fd, &v, sizeof v);

        close(fd);
        if (n == (ssize_t)sizeof v && v != 0) {
            snprintf(out, 17, "%016" PRIx64, v);
            return;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v = ((uint64_t)ts.tv_sec * 1000000003ULL) ^ ((uint64_t)ts.tv_nsec << 17) ^ ((uint64_t)getpid() * 0x9e3779b97f4a7c15ULL);
    if (!v) v = 0x517cc1b727220a95ULL;
    snprintf(out, 17, "%016" PRIx64, v);
}

static void format_http_time(int64_t mtime, char *out, size_t cap)
{
    static const char *const wdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t t = mtime > 0 ? (time_t)mtime : (time_t)0;
    struct tm tm_utc;

    if (!gmtime_r(&t, &tm_utc)) {
        snprintf(out, cap, "Thu, 01 Jan 1970 00:00:00 GMT");
        return;
    }
    snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", wdays[tm_utc.tm_wday % 7], tm_utc.tm_mday, months[tm_utc.tm_mon % 12],
        tm_utc.tm_year + 1900, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
}

static int fd_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void close_slot_handles(struct smb_slot *slot)
{
    if (slot->session) {
        if (slot->file) smb_close_file(slot->session, slot->file);
        smb_disconnect(slot->session);
    }
    slot->session = NULL;
    slot->file = NULL;
}

static int acquire_smb_slot(struct pkg_stream *st, int *slot_idx, struct smb_session **out_s, void **out_f)
{
    int i, empty = -1, err;
    struct smb_session *s = NULL;
    void *f = NULL;

    pthread_mutex_lock(&st->pool_lock);
    for (i = 0; i < STREAM_MAX_WORKERS; i++) {
        if (st->pool[i].session && !st->pool[i].in_use) {
            st->pool[i].in_use = 1;
            *slot_idx = i;
            *out_s = st->pool[i].session;
            *out_f = st->pool[i].file;
            pthread_mutex_unlock(&st->pool_lock);
            return 0;
        }
        if (!st->pool[i].session && !st->pool[i].in_use && empty < 0) empty = i;
    }
    if (empty < 0 || __atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
        pthread_mutex_unlock(&st->pool_lock);
        return SMB_FAILED;
    }
    st->pool[empty].in_use = 1;
    pthread_mutex_unlock(&st->pool_lock);

    err = smb_connect_share(&st->server, st->share, &s, NULL);
    if (!err) err = smb_open_file(s, st->path, &f);
    if (err) {
        if (s) smb_disconnect(s);
        pthread_mutex_lock(&st->pool_lock);
        st->pool[empty].in_use = 0;
        pthread_mutex_unlock(&st->pool_lock);
        __atomic_store_n(&st->last_smb_error, err, __ATOMIC_RELAXED);
        return err;
    }
    pthread_mutex_lock(&st->pool_lock);
    st->pool[empty].session = s;
    st->pool[empty].file = f;
    pthread_mutex_unlock(&st->pool_lock);
    *slot_idx = empty;
    *out_s = s;
    *out_f = f;
    return 0;
}

static void release_smb_slot(struct pkg_stream *st, int slot_idx, struct smb_session *s, void *f, int broken)
{
    int discard;

    if (slot_idx < 0 || slot_idx >= STREAM_MAX_WORKERS) return;
    discard = broken || __atomic_load_n(&st->stop, __ATOMIC_RELAXED);
    pthread_mutex_lock(&st->pool_lock);
    st->pool[slot_idx].session = s;
    st->pool[slot_idx].file = f;
    if (discard) {
        close_slot_handles(&st->pool[slot_idx]);
    }
    st->pool[slot_idx].in_use = 0;
    pthread_mutex_unlock(&st->pool_lock);
}

static int reopen_smb_slot(struct pkg_stream *st, int slot_idx, struct smb_session **s, void **f)
{
    struct smb_session *ns = NULL;
    void *nf = NULL;
    int err;

    if (*s) {
        if (*f) smb_close_file(*s, *f);
        smb_disconnect(*s);
        *s = NULL;
        *f = NULL;
    }
    pthread_mutex_lock(&st->pool_lock);
    st->pool[slot_idx].session = NULL;
    st->pool[slot_idx].file = NULL;
    pthread_mutex_unlock(&st->pool_lock);

    if (__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) return SMB_FAILED;
    err = smb_connect_share(&st->server, st->share, &ns, NULL);
    if (!err) err = smb_open_file(ns, st->path, &nf);
    if (err) {
        if (ns) smb_disconnect(ns);
        __atomic_store_n(&st->last_smb_error, err, __ATOMIC_RELAXED);
        return err;
    }
    pthread_mutex_lock(&st->pool_lock);
    st->pool[slot_idx].session = ns;
    st->pool[slot_idx].file = nf;
    pthread_mutex_unlock(&st->pool_lock);
    *s = ns;
    *f = nf;
    return 0;
}

static int token_has(const char *value, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *p = value;

    while (*p) {
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : p + strlen(p);

        while (p < end && (*p == ' ' || *p == '\t')) p++;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - p) == nlen && strncasecmp(p, needle, nlen) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

static int parse_u64_strict(const char *p, const char **end_out, uint64_t *val)
{
    uint64_t acc = 0;
    int digits = 0;

    while (*p >= '0' && *p <= '9') {
        uint64_t d = (uint64_t)(*p - '0');

        if (acc > (UINT64_MAX - d) / 10) return -1;
        acc = acc * 10 + d;
        digits = 1;
        p++;
    }
    if (!digits) return -1;
    *val = acc;
    *end_out = p;
    return 0;
}

/* Returns 0 on valid single byte range, -1 on invalid/unsatisfiable range. */
static int parse_byte_range(const char *header, uint64_t size, uint64_t *first, uint64_t *last)
{
    const char *p = header;
    uint64_t a = 0, b = 0;

    if (!size) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "bytes=", 6) != 0) return -1;
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    if (strchr(p, ',')) return -1;
    if (*p == '-') {
        if (parse_u64_strict(p + 1, &p, &b) != 0 || b == 0) return -1;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) return -1;
        *first = b >= size ? 0 : size - b;
        *last = size - 1;
        return 0;
    }
    if (parse_u64_strict(p, &p, &a) != 0) return -1;
    if (*p != '-') return -1;
    p++;
    if (*p >= '0' && *p <= '9') {
        if (parse_u64_strict(p, &p, &b) != 0) return -1;
    } else {
        b = size - 1;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p || a >= size || b < a) return -1;
    if (b >= size) b = size - 1;
    *first = a;
    *last = b;
    return 0;
}

/* Reads one HTTP request head. Returns 1 if parsed, 0 on clean EOF/stop, -1 on bad/error. */
static int read_request_head(struct pkg_stream *st, int fd, char *buf, size_t cap, char *method, size_t mcap, char *target, size_t tcap, char *range,
    size_t rcap, int *keep_alive)
{
    size_t used = 0;
    char *head_end = NULL, *line, *save_line = NULL;
    char version[16] = "HTTP/1.1";
    int has_close = 0, has_keep = 0;

    method[0] = 0;
    target[0] = 0;
    range[0] = 0;
    while (!__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
        struct pollfd pfd = {fd, POLLIN, 0};
        int ready = poll(&pfd, 1, STREAM_POLL_MS);

        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) continue;
        if (used + 1 >= cap) return -1;
        {
            ssize_t n = read(fd, buf + used, cap - 1 - used);

            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return used == 0 ? 0 : -1;
            used += (size_t)n;
            buf[used] = 0;
            head_end = strstr(buf, "\r\n\r\n");
            if (head_end) break;
        }
    }
    if (!head_end) return 0;
    *head_end = 0;

    line = strtok_r(buf, "\r\n", &save_line);
    if (!line) return -1;
    {
        char *sp1 = strchr(line, ' '), *sp2, *qmark;

        if (!sp1) return -1;
        *sp1 = 0;
        snprintf(method, mcap, "%s", line);
        line = sp1 + 1;
        while (*line == ' ') line++;
        sp2 = strchr(line, ' ');
        if (sp2) {
            *sp2 = 0;
            snprintf(version, sizeof version, "%s", sp2 + 1);
        }
        qmark = strchr(line, '?');
        if (qmark) *qmark = 0;
        snprintf(target, tcap, "%s", line);
    }

    while ((line = strtok_r(NULL, "\r\n", &save_line)) != NULL) {
        char *colon = strchr(line, ':');
        char *val;

        if (!colon) continue;
        *colon = 0;
        val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        if (!strcasecmp(line, "Range")) {
            snprintf(range, rcap, "%s", val);
        } else if (!strcasecmp(line, "Connection")) {
            if (token_has(val, "close")) has_close = 1;
            if (token_has(val, "keep-alive")) has_keep = 1;
        }
    }
    *keep_alive = !strcmp(version, "HTTP/1.0") ? has_keep : !has_close;
    return 1;
}

static int send_simple(int fd, int status, const char *reason, const char *extra_headers, int keep_alive)
{
    char resp[512];
    int n = snprintf(resp, sizeof resp,
        "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nAccept-Ranges: bytes\r\n%sConnection: %s\r\n\r\n",
        status, reason, extra_headers ? extra_headers : "", keep_alive ? "keep-alive" : "close");

    return n > 0 ? fd_write_all(fd, resp, (size_t)n) : -1;
}

static int stream_range_from_smb(struct pkg_stream *st, int fd, uint64_t offset, uint64_t length)
{
    int slot = -1, broken = 0;
    struct smb_session *s = NULL;
    void *f = NULL;
    size_t chunk_cap, rsize;
    uint8_t *buf;
    uint64_t remaining = length;

    if (acquire_smb_slot(st, &slot, &s, &f) != 0) return -1;
    rsize = smb_read_size(s);
    chunk_cap = rsize ? rsize : STREAM_DEFAULT_CHUNK;
    if (chunk_cap > STREAM_MAX_CHUNK) chunk_cap = STREAM_MAX_CHUNK;
    if (chunk_cap < STREAM_MIN_CHUNK) chunk_cap = STREAM_MIN_CHUNK;
    buf = malloc(chunk_cap);
    if (!buf) {
        release_smb_slot(st, slot, s, f, 0);
        return -1;
    }
    while (remaining > 0 && !__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
        size_t ask = remaining < chunk_cap ? (size_t)remaining : chunk_cap;
        long long n = -1;
        int attempt;

        for (attempt = 0; attempt <= STREAM_SMB_RETRIES; attempt++) {
            n = smb_read(s, f, buf, ask, offset);
            if (n >= 0) break;
            if ((n != SMB_DROPPED && n != SMB_TIMEOUT) || attempt == STREAM_SMB_RETRIES) break;
            usleep(100000);
            if (reopen_smb_slot(st, slot, &s, &f) != 0) {
                n = SMB_FAILED;
                break;
            }
        }
        if (n <= 0) {
            if (n < 0) __atomic_store_n(&st->last_smb_error, (int)n, __ATOMIC_RELAXED);
            broken = 1;
            break;
        }
        if (fd_write_all(fd, buf, (size_t)n) != 0) {
            broken = -1;                /* client went away cleanly; keep SMB handle reusable */
            break;
        }
        offset += (uint64_t)n;
        remaining -= (uint64_t)n;
        __atomic_add_fetch(&st->served, (uint64_t)n, __ATOMIC_RELAXED);
    }
    free(buf);
    release_smb_slot(st, slot, s, f, broken > 0);
    return broken != 0 ? -1 : 0;
}

static int stream_range_from_browser(struct pkg_stream *st, int fd, uint64_t offset, uint64_t length)
{
    uint64_t remaining = length;

    while (remaining > 0 && !__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
        size_t ask = remaining < STREAM_BROWSER_CHUNK ? (size_t)remaining : STREAM_BROWSER_CHUNK;
        int idx = -1, i, err = 0;
        uint8_t *data = NULL;
        struct timespec dl;

        pthread_mutex_lock(&st->pull_lock);
        while (!__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
            for (i = 0; i < STREAM_PULL_SLOTS; i++) {
                if (st->pulls[i].state == PULL_FREE) {
                    idx = i;
                    break;
                }
            }
            if (idx >= 0) break;
            pthread_cond_wait(&st->pull_cond, &st->pull_lock);
        }
        if (idx < 0 || __atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
            pthread_mutex_unlock(&st->pull_lock);
            return -1;
        }
        st->pulls[idx].state = PULL_NEEDED;
        st->pulls[idx].id = __atomic_fetch_add(&g_next_pull_id, 1, __ATOMIC_RELAXED);
        st->pulls[idx].offset = offset;
        st->pulls[idx].len = ask;
        st->pulls[idx].data = NULL;
        pthread_cond_broadcast(&st->pull_cond);

        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += STREAM_PULL_WAIT_MS / 1000;
        while (!__atomic_load_n(&st->stop, __ATOMIC_RELAXED) && (st->pulls[idx].state == PULL_NEEDED || st->pulls[idx].state == PULL_INFLIGHT)) {
            if (pthread_cond_timedwait(&st->pull_cond, &st->pull_lock, &dl) == ETIMEDOUT) {
                err = 1;
                break;
            }
        }
        if (!err && st->pulls[idx].state == PULL_READY && st->pulls[idx].data && st->pulls[idx].len == ask) {
            data = st->pulls[idx].data;
            st->pulls[idx].data = NULL;
        } else {
            free(st->pulls[idx].data);
            st->pulls[idx].data = NULL;
            err = 1;
        }
        st->pulls[idx].state = PULL_FREE;
        pthread_cond_broadcast(&st->pull_cond);
        pthread_mutex_unlock(&st->pull_lock);

        if (err || !data) return -1;
        if (fd_write_all(fd, data, ask) != 0) {
            free(data);
            return -1;
        }
        free(data);
        offset += ask;
        remaining -= ask;
        __atomic_add_fetch(&st->served, ask, __ATOMIC_RELAXED);
    }
    return 0;
}

static void register_fd(struct pkg_stream *st, int fd)
{
    int i;

    pthread_mutex_lock(&st->lock);
    for (i = 0; i < STREAM_MAX_WORKERS; i++) {
        if (st->active_fds[i] < 0) {
            st->active_fds[i] = fd;
            break;
        }
    }
    pthread_mutex_unlock(&st->lock);
}

static void unregister_worker(struct pkg_stream *st, int fd)
{
    int i;

    pthread_mutex_lock(&st->lock);
    for (i = 0; i < STREAM_MAX_WORKERS; i++) {
        if (st->active_fds[i] == fd) {
            st->active_fds[i] = -1;
            break;
        }
    }
    close(fd);
    st->active_workers--;
    pthread_cond_broadcast(&st->done_cond);
    pthread_mutex_unlock(&st->lock);
}

static void *worker_main(void *opaque)
{
    struct stream_worker_arg *arg = opaque;
    struct pkg_stream *st = arg->st;
    int fd = arg->fd;
    char *head_buf = malloc(STREAM_HEADER_BYTES);

    free(arg);
    register_fd(st, fd);
    while (head_buf && !__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
        char method[16], target[256], range[128];
        int keep_alive = 1, got;
        int is_head, partial = 0;
        uint64_t first = 0, last = 0, length = 0;
        char hdr[512];
        int hlen;

        got = read_request_head(st, fd, head_buf, STREAM_HEADER_BYTES, method, sizeof method, target, sizeof target, range, sizeof range, &keep_alive);
        if (got <= 0) break;

        is_head = !strcmp(method, "HEAD");
        if (!is_head && strcmp(method, "GET") != 0) {
            if (send_simple(fd, 405, "Method Not Allowed", NULL, keep_alive) != 0 || !keep_alive) break;
            continue;
        }
        if (strcmp(target, st->url_path) != 0) {
            if (send_simple(fd, 404, "Not Found", NULL, keep_alive) != 0 || !keep_alive) break;
            continue;
        }
        if (range[0]) {
            if (parse_byte_range(range, st->size, &first, &last) != 0) {
                char cr[96];

                snprintf(cr, sizeof cr, "Content-Range: bytes */%" PRIu64 "\r\n", st->size);
                if (send_simple(fd, 416, "Range Not Satisfiable", cr, keep_alive) != 0 || !keep_alive) break;
                continue;
            }
            partial = 1;
            length = last - first + 1;
        } else {
            first = 0;
            last = st->size ? st->size - 1 : 0;
            length = st->size;
        }

        if (partial) {
            hlen = snprintf(hdr, sizeof hdr,
                "HTTP/1.1 206 Partial Content\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Accept-Ranges: bytes\r\n"
                "Content-Length: %" PRIu64 "\r\n"
                "Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n"
                "ETag: %s\r\n"
                "Last-Modified: %s\r\n"
                "Connection: %s\r\n\r\n",
                length, first, last, st->size, st->etag, st->last_modified, keep_alive ? "keep-alive" : "close");
        } else {
            hlen = snprintf(hdr, sizeof hdr,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Accept-Ranges: bytes\r\n"
                "Content-Length: %" PRIu64 "\r\n"
                "ETag: %s\r\n"
                "Last-Modified: %s\r\n"
                "Connection: %s\r\n\r\n",
                length, st->etag, st->last_modified, keep_alive ? "keep-alive" : "close");
        }
        if (hlen <= 0 || fd_write_all(fd, hdr, (size_t)hlen) != 0) break;
        if (!is_head && length > 0) {
            int rc = st->browser_mode ? stream_range_from_browser(st, fd, first, length) : stream_range_from_smb(st, fd, first, length);

            if (rc != 0) break;
        }
        if (!keep_alive) break;
    }
    free(head_buf);
    unregister_worker(st, fd);
    return NULL;
}

static void start_worker(struct pkg_stream *st, int fd)
{
    struct stream_worker_arg *arg = malloc(sizeof *arg);
    struct timeval tv = {STREAM_IO_TIMEOUT_S, 0};
    int on = 1, sndbuf = STREAM_SNDBUF, result;
    pthread_attr_t attr;
    pthread_t thr;

    if (!arg) {
        close(fd);
        return;
    }
    pthread_mutex_lock(&st->lock);
    if (__atomic_load_n(&st->stop, __ATOMIC_RELAXED) || st->active_workers >= STREAM_MAX_WORKERS) {
        pthread_mutex_unlock(&st->lock);
        free(arg);
        close(fd);
        return;
    }
    st->active_workers++;
    pthread_mutex_unlock(&st->lock);

    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    arg->st = st;
    arg->fd = fd;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STREAM_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    result = pthread_create(&thr, &attr, worker_main, arg);
    pthread_attr_destroy(&attr);
    if (result) {
        free(arg);
        close(fd);
        pthread_mutex_lock(&st->lock);
        st->active_workers--;
        pthread_cond_broadcast(&st->done_cond);
        pthread_mutex_unlock(&st->lock);
    }
}

static void *accept_main(void *opaque)
{
    struct pkg_stream *st = opaque;
    struct pollfd pfd = {st->listener, POLLIN, 0};

    while (!__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
        int ready = poll(&pfd, 1, STREAM_POLL_MS);

        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        if (pfd.revents & (POLLERR | POLLNVAL)) break;
        if (pfd.revents & POLLIN) {
            int fd = accept(st->listener, NULL, NULL);

            if (fd >= 0) start_worker(st, fd);
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED) break;
        }
    }
    return NULL;
}

static int start_common(struct pkg_stream *st)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof addr;
    int i, on = 1;
    char token[17];

    st->listener = -1;
    pthread_mutex_init(&st->lock, NULL);
    pthread_cond_init(&st->done_cond, NULL);
    pthread_mutex_init(&st->pool_lock, NULL);
    pthread_mutex_init(&st->pull_lock, NULL);
    pthread_cond_init(&st->pull_cond, NULL);
    for (i = 0; i < STREAM_MAX_WORKERS; i++) st->active_fds[i] = -1;

    make_token(token);
    snprintf(st->url_path, sizeof st->url_path, "/pkg/%s/package.pkg", token);
    snprintf(st->etag, sizeof st->etag, "\"%" PRIx64 "-%" PRIx64 "\"", st->size, (uint64_t)st->mtime);
    format_http_time(st->mtime, st->last_modified, sizeof st->last_modified);

    st->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (st->listener < 0) goto fail;
    setsockopt(st->listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(st->listener, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(st->listener, STREAM_BACKLOG) != 0 ||
        getsockname(st->listener, (struct sockaddr *)&addr, &alen) != 0)
        goto fail;
    st->port = ntohs(addr.sin_port);
    if (pthread_create(&st->accept_thread, NULL, accept_main, st) != 0) goto fail;
    st->accept_started = 1;
    return 0;

fail:
    if (st->listener >= 0) close(st->listener);
    pthread_cond_destroy(&st->pull_cond);
    pthread_mutex_destroy(&st->pull_lock);
    pthread_mutex_destroy(&st->pool_lock);
    pthread_cond_destroy(&st->done_cond);
    pthread_mutex_destroy(&st->lock);
    wipe(&st->server, sizeof st->server);
    free(st);
    return -1;
}

int pkg_stream_start(const struct smb_server *server, const char *share, const char *path, struct smb_session *initial_session, void *initial_file,
    uint64_t size, int64_t mtime, struct pkg_stream **out)
{
    struct pkg_stream *st;

    if (!server || !share || !path || !out) return -1;
    st = calloc(1, sizeof *st);
    if (!st) return -1;
    st->server = *server;
    snprintf(st->share, sizeof st->share, "%s", share);
    snprintf(st->path, sizeof st->path, "%s", path);
    st->size = size;
    st->mtime = mtime;
    if (initial_session && initial_file) {
        st->pool[0].session = initial_session;
        st->pool[0].file = initial_file;
    }
    if (start_common(st) != 0) return -1;
    *out = st;
    return 0;
}

int pkg_stream_start_browser(uint64_t size, int64_t mtime, struct pkg_stream **out)
{
    struct pkg_stream *st;

    if (!out) return -1;
    st = calloc(1, sizeof *st);
    if (!st) return -1;
    st->browser_mode = 1;
    st->size = size;
    st->mtime = mtime;
    if (start_common(st) != 0) return -1;
    *out = st;
    return 0;
}

int pkg_stream_pull_need(struct pkg_stream *st, uint64_t *req_id, uint64_t *offset, size_t *len, int timeout_ms)
{
    struct timespec dl;
    int i, found = -1;

    if (!st || !req_id || !offset || !len) return -1;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&st->pull_lock);
    for (;;) {
        if (__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
            pthread_mutex_unlock(&st->pull_lock);
            return -1;
        }
        for (i = 0; i < STREAM_PULL_SLOTS; i++) {
            if (st->pulls[i].state == PULL_NEEDED) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            st->pulls[found].state = PULL_INFLIGHT;
            *req_id = st->pulls[found].id;
            *offset = st->pulls[found].offset;
            *len = st->pulls[found].len;
            pthread_mutex_unlock(&st->pull_lock);
            return 1;
        }
        if (pthread_cond_timedwait(&st->pull_cond, &st->pull_lock, &dl) == ETIMEDOUT) {
            pthread_mutex_unlock(&st->pull_lock);
            return 0;
        }
    }
}

int pkg_stream_pull_give(struct pkg_stream *st, uint64_t req_id, uint8_t *data, size_t len)
{
    int i;

    if (!st || !data) return -1;
    pthread_mutex_lock(&st->pull_lock);
    if (__atomic_load_n(&st->stop, __ATOMIC_RELAXED)) {
        pthread_mutex_unlock(&st->pull_lock);
        return -1;
    }
    for (i = 0; i < STREAM_PULL_SLOTS; i++) {
        if (st->pulls[i].state == PULL_INFLIGHT && st->pulls[i].id == req_id && st->pulls[i].len == len) {
            st->pulls[i].data = data;
            st->pulls[i].state = PULL_READY;
            pthread_cond_broadcast(&st->pull_cond);
            pthread_mutex_unlock(&st->pull_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&st->pull_lock);
    return -1;
}

void pkg_stream_url(const struct pkg_stream *st, char *out, size_t capacity)
{
    if (!st || !out || !capacity) return;
    snprintf(out, capacity, "http://127.0.0.1:%u%s", (unsigned)st->port, st->url_path);
}

uint64_t pkg_stream_served(const struct pkg_stream *st)
{
    return st ? __atomic_load_n(&st->served, __ATOMIC_RELAXED) : 0;
}

int pkg_stream_active_clients(const struct pkg_stream *st)
{
    int n;

    if (!st) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&st->lock);
    n = st->active_workers;
    pthread_mutex_unlock((pthread_mutex_t *)&st->lock);
    return n;
}

int pkg_stream_last_smb_error(const struct pkg_stream *st)
{
    return st ? __atomic_load_n(&st->last_smb_error, __ATOMIC_RELAXED) : 0;
}

void pkg_stream_signal_stop(struct pkg_stream *st)
{
    if (!st) return;
    __atomic_store_n(&st->stop, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&st->pull_lock);
    pthread_cond_broadcast(&st->pull_cond);
    pthread_mutex_unlock(&st->pull_lock);
}

void pkg_stream_stop(struct pkg_stream *st)
{
    int i;

    if (!st) return;
    pkg_stream_signal_stop(st);
    if (st->listener >= 0) shutdown(st->listener, SHUT_RDWR);
    pthread_mutex_lock(&st->lock);
    for (i = 0; i < STREAM_MAX_WORKERS; i++) {
        if (st->active_fds[i] >= 0) shutdown(st->active_fds[i], SHUT_RDWR);
    }
    pthread_mutex_unlock(&st->lock);

    if (st->accept_started) pthread_join(st->accept_thread, NULL);
    if (st->listener >= 0) {
        close(st->listener);
        st->listener = -1;
    }

    pthread_mutex_lock(&st->lock);
    while (st->active_workers > 0) pthread_cond_wait(&st->done_cond, &st->lock);
    pthread_mutex_unlock(&st->lock);

    pthread_mutex_lock(&st->pull_lock);
    for (i = 0; i < STREAM_PULL_SLOTS; i++) {
        free(st->pulls[i].data);
        st->pulls[i].data = NULL;
    }
    pthread_mutex_unlock(&st->pull_lock);

    pthread_mutex_lock(&st->pool_lock);
    for (i = 0; i < STREAM_MAX_WORKERS; i++) close_slot_handles(&st->pool[i]);
    pthread_mutex_unlock(&st->pool_lock);

    pthread_cond_destroy(&st->pull_cond);
    pthread_mutex_destroy(&st->pull_lock);
    pthread_mutex_destroy(&st->pool_lock);
    pthread_cond_destroy(&st->done_cond);
    pthread_mutex_destroy(&st->lock);
    wipe(&st->server, sizeof st->server);
    free(st);
}
