/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See files.h. There is no login, as with the rest of this page: whoever reaches
 * the port can delete files. What is limited is where. A path must be plain, with no
 * "." or ".." and no empty part, must sit under a volume, and may not pass through a
 * symbolic link. A volume itself can be neither deleted nor renamed.
 *
 * An upload goes to <name>.tom-part and is renamed when complete, so a dropped
 * connection leaves no half file under the real name. */
#include "files.h"

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define MAX_PATH_BYTES 1024
#define MAX_DEPTH 64
#define IO_BYTES (1024 * 1024)
#define MAX_ENTRIES 20000
#define SYNC_FROM (16ULL * 1024 * 1024)
#define PART_SUFFIX ".tom-part"
#define TAR_BLOCK 512
#define JOB_STACK (1024 * 1024)
#define O_DIRECT_BIT 0x00010000         /* FreeBSD's O_DIRECT */
#define BENCH_FILE "/.tom-bench.tmp"
#define BENCH_ALIGN 4096
#define STOP_WAIT_TICKS 3000            /* a slow disk can hold one write for seconds */
#define STOP_TICK_US 10000
#define MAX_TRANSFERS 12
#define TRANSFER_KEEP_MS 15000L         /* how long the other pages still see a finished one */
#define CLIENT_BYTES 17
#define SAMPLE_MS 30000L                /* how often a long upload says how it is going */
#ifdef FILES_TESTING
#define BENCH_MIB 16
#else
#define BENCH_MIB 2048
#endif

struct volume {
    const char *path, *kind;
    const char *from;
};

static void (*g_log)(const char *);

#ifdef FILES_TESTING
const char *files_test_root;
long files_test_sample_ms = SAMPLE_MS;
#undef SAMPLE_MS
#define SAMPLE_MS files_test_sample_ms
#endif

static struct {
    pthread_mutex_t lock;
    int running, cancel, move;
    const char *state, *kind;
    char from[MAX_PATH_BYTES], to[MAX_PATH_BYTES], error[32];
    uint64_t done, total;
    long started, ended;
} g_job = {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, "idle", "", "", "", "", 0, 0, 0, 0};

/* What every open page shows: the console's browser and a computer on the LAN see the
 * same transfers. id 0 is a free slot. */
struct transfer {
    unsigned id;
    const char *kind, *state;
    char name[128], client[CLIENT_BYTES];
    uint64_t size, done;
    long started, ended;
};

static pthread_mutex_t g_transfer_lock = PTHREAD_MUTEX_INITIALIZER;
static struct transfer g_transfers[MAX_TRANSFERS];
static unsigned g_transfer_id;
static char g_job_client[CLIENT_BYTES];
static struct transfer *g_job_transfer;

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void files_set_log(void (*log)(const char *line))
{
    g_log = log;
}

/* ---- which paths ---------------------------------------------------------- */

static size_t numbered_root(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);

    if (strncmp(path, prefix, n) || path[n] < '0' || path[n] > '9') return 0;
    return path[n + 1] == '/' || !path[n + 1] ? n + 1 : 0;
}

/* The length of the volume part of `path`, or 0 if it is on no volume. */
static size_t root_length(const char *path)
{
    size_t n;

#ifdef FILES_TESTING
    n = files_test_root ? strlen(files_test_root) : 0;
    if (n && !strncmp(path, files_test_root, n) && (path[n] == '/' || !path[n])) return n;
#endif
    if (!strncmp(path, "/data", 5) && (path[5] == '/' || !path[5])) return 5;
    n = numbered_root(path, "/mnt/ext");
    return n ? n : numbered_root(path, "/mnt/usb");
}

int files_path_allowed(const char *path)
{
    size_t root = root_length(path), length = strlen(path);
    const char *p;

    if (!root || length >= MAX_PATH_BYTES - sizeof PART_SUFFIX) return 0;
    for (p = path; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f || *p == '\\') return 0;
    for (p = path + root; *p;) {
        const char *next = strchr(p + 1, '/');
        size_t part = (next ? (size_t)(next - p) : strlen(p)) - 1;

        if (!part || (part == 1 && p[1] == '.') || (part == 2 && p[1] == '.' && p[2] == '.')) return 0;
        if (!next) break;
        p = next;
    }
    return 1;
}

/* Every part below the volume, the last one too if it exists, is no symbolic link. */
static int no_links(const char *path)
{
    char walk[MAX_PATH_BYTES];
    size_t i = root_length(path), length = strlen(path);

    memcpy(walk, path, length + 1);
    while (i < length) {
        struct stat st;

        for (i++; i < length && walk[i] != '/'; i++) {}
        walk[i] = 0;
        if (lstat(walk, &st) != 0) return errno == ENOENT;
        if (S_ISLNK(st.st_mode)) return 0;
        walk[i] = i < length ? '/' : 0;
    }
    return 1;
}

static int usable(const char *path)
{
    return files_path_allowed(path) && no_links(path);
}

static int is_volume(const char *path)
{
    return root_length(path) == strlen(path);
}

static uint64_t free_bytes(const char *path)
{
    struct statfs fs;

    if (statfs(path, &fs) != 0) return UINT64_MAX;
    return (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize;
}

/* Only the volume reaches the log, never a file's name. */
static void note_transfer(const char *what, const char *path, uint64_t bytes, long ms, long sync_ms)
{
    char line[192];

    if (!g_log || bytes < SYNC_FROM) return;
    snprintf(line, sizeof line, "%s %.*s: %" PRIu64 " bytes in %ld ms, %" PRIu64 " MB/s, closing fsync %ld ms", what, (int)root_length(path), path, bytes, ms,
        ms > 0 ? bytes / (uint64_t)ms / 1000 : 0, sync_ms);
    g_log(line);
}

/* ---- small replies -------------------------------------------------------- */

static int reply(struct http_stream *stream, int status, const char *json)
{
    return http_respond(stream, status, "application/json", json);
}

static int error(struct http_stream *stream, int status, const char *code)
{
    char json[64];

    snprintf(json, sizeof json, "{\"error\":\"%s\"}", code);
    return reply(stream, status, json);
}

static const char *errno_code(int code)
{
    return code == ENOENT ? "not_found" : code == EEXIST ? "exists" : code == ENOTEMPTY ? "not_empty" : code == ENOSPC ? "no_space" :
        code == EACCES || code == EPERM ? "denied" : code == EROFS ? "read_only" : code == EXDEV ? "other_volume" :
        code == ENAMETOOLONG ? "name_too_long" : code == EISDIR ? "is_folder" : code == ENOTDIR ? "not_folder" : code == EBUSY ? "busy" : "failed";
}

static int fail(struct http_stream *stream)
{
    int code = errno;

    return error(stream, code == ENOENT ? 404 : 409, errno_code(code));
}

struct text {
    char *data;
    size_t used, capacity;
    int failed;
};

static void add(struct text *t, const char *data, size_t length)
{
    if (t->failed) return;
    if (t->used + length + 1 > t->capacity) {
        size_t size = t->capacity * 2 + length + 1;
        char *grown = realloc(t->data, size);

        if (!grown) {
            t->failed = 1;
            return;
        }
        t->data = grown;
        t->capacity = size;
    }
    memcpy(t->data + t->used, data, length);
    t->used += length;
    t->data[t->used] = 0;
}

static void add_text(struct text *t, const char *data)
{
    add(t, data, strlen(data));
}

static void add_json_string(struct text *t, const char *value)
{
    add(t, "\"", 1);
    for (; *value; value++) {
        char escaped[8];

        if (*value == '"' || *value == '\\') {
            snprintf(escaped, sizeof escaped, "\\%c", *value);
            add_text(t, escaped);
        } else if ((unsigned char)*value < 0x20) {
            snprintf(escaped, sizeof escaped, "\\u%04x", (unsigned)(unsigned char)*value);
            add_text(t, escaped);
        } else {
            add(t, value, 1);
        }
    }
    add(t, "\"", 1);
}

static int query_path(const struct http_request *r, const char *key, char *out)
{
    return http_query_text(r->query, key, out, MAX_PATH_BYTES) == 0 && usable(out) ? 0 : -1;
}

/* ---- volumes and folders -------------------------------------------------- */

static void add_volume(struct text *t, const char *path, const char *kind, const struct statfs *fs, int *first)
{
    char numbers[160];

    add_text(t, *first ? "{\"path\":" : ",{\"path\":");
    *first = 0;
    add_json_string(t, path);
    add_text(t, ",\"kind\":\"");
    add_text(t, kind);
    add_text(t, "\",\"format\":");
    add_json_string(t, fs->f_fstypename);
    snprintf(numbers, sizeof numbers, ",\"total\":%" PRIu64 ",\"free\":%" PRIu64 "}", (uint64_t)fs->f_blocks * (uint64_t)fs->f_bsize,
        (uint64_t)fs->f_bavail * (uint64_t)fs->f_bsize);
    add_text(t, numbers);
}

/* A slot counts only if a device is mounted on that very path. PS5 formats M.2 and USB
 * extended storage alike, and only the device tells them apart. */
static void try_volume(struct text *t, const char *path, int *first)
{
    struct statfs fs;

    if (statfs(path, &fs) != 0 || strcmp(fs.f_mntonname, path)) return;
    if (strncmp(fs.f_mntfromname, "/dev/", 5)) return;
    add_volume(t, path, !strncmp(path, "/mnt/usb", 8) ? "usb" : !strncmp(fs.f_mntfromname, "/dev/nvme", 9) ? "m2" :
        !strncmp(fs.f_mntfromname, "/dev/da", 7) ? "usb-extended" : "extended", &fs, first);
}

static int volumes(struct http_stream *stream)
{
    struct text t = {NULL, 0, 0, 0};
    struct statfs fs;
    int first = 1, i, result;

    add_text(&t, "{\"volumes\":[");
#ifdef FILES_TESTING
    if (files_test_root && statfs(files_test_root, &fs) == 0) add_volume(&t, files_test_root, "internal", &fs, &first);
#endif
    if (statfs("/data", &fs) == 0) add_volume(&t, "/data", "internal", &fs, &first);
    for (i = 0; i < 10; i++) {
        char path[16];

        snprintf(path, sizeof path, "/mnt/ext%d", i);
        try_volume(&t, path, &first);
        snprintf(path, sizeof path, "/mnt/usb%d", i);
        try_volume(&t, path, &first);
    }
    add_text(&t, "]}");
    if (t.failed) {
        free(t.data);
        return error(stream, 503, "no_memory");
    }
    result = reply(stream, 200, t.data);
    free(t.data);
    return result;
}

static int list(const struct http_request *r, struct http_stream *stream)
{
    char path[MAX_PATH_BYTES], child[MAX_PATH_BYTES + 256], numbers[96];
    struct text t = {NULL, 0, 0, 0};
    struct dirent *e;
    DIR *d;
    int count = 0, result;

    if (query_path(r, "path", path) != 0) return error(stream, 400, "bad_path");
    d = opendir(path);
    if (!d) return fail(stream);
    add_text(&t, "{\"path\":");
    add_json_string(&t, path);
    add_text(&t, ",\"entries\":[");
    while ((e = readdir(d)) != NULL && count < MAX_ENTRIES && !t.failed) {
        struct stat st;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (lstat(child, &st) != 0) continue;
        add_text(&t, count++ ? ",{\"name\":" : "{\"name\":");
        add_json_string(&t, e->d_name);
        snprintf(numbers, sizeof numbers, ",\"kind\":\"%s\",\"size\":%" PRIu64 ",\"mtime\":%" PRId64 "}",
            S_ISDIR(st.st_mode) ? "folder" : S_ISLNK(st.st_mode) ? "link" : S_ISREG(st.st_mode) ? "file" : "other",
            (uint64_t)st.st_size, (int64_t)st.st_mtime);
        add_text(&t, numbers);
    }
    closedir(d);
    snprintf(numbers, sizeof numbers, "],\"truncated\":%s}", e ? "true" : "false");
    add_text(&t, numbers);
    if (t.failed) {
        free(t.data);
        return error(stream, 503, "no_memory");
    }
    result = reply(stream, 200, t.data);
    free(t.data);
    return result;
}

static int make_folder(const struct http_request *r, struct http_stream *stream)
{
    char path[MAX_PATH_BYTES];

    if (query_path(r, "path", path) != 0 || is_volume(path)) return error(stream, 400, "bad_path");
    if (mkdir(path, 0777) != 0) return fail(stream);
    return reply(stream, 200, "{}");
}

static int rename_entry(const struct http_request *r, struct http_stream *stream)
{
    char from[MAX_PATH_BYTES], to[MAX_PATH_BYTES];
    struct stat st;

    if (query_path(r, "from", from) != 0 || query_path(r, "to", to) != 0 || is_volume(from) || is_volume(to))
        return error(stream, 400, "bad_path");
    if (lstat(to, &st) == 0 && strcasecmp(from, to)) return error(stream, 409, "exists");
    if (rename(from, to) != 0) return fail(stream);
    return reply(stream, 200, "{}");
}

/* Stops at the first failure. Links are removed, never followed. */
static int remove_tree(const char *path, int depth, volatile int *cancel)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    int result = 0;

    if (cancel && *cancel) {
        errno = ECANCELED;
        return -1;
    }
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    if (depth >= MAX_DEPTH) {
        errno = ENAMETOOLONG;
        return -1;
    }
    d = opendir(path);
    if (!d) return -1;
    while (!result && (e = readdir(d)) != NULL) {
        char *child;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        child = malloc(MAX_PATH_BYTES + 256);
        if (!child) {
            errno = ENOMEM;
            result = -1;
            break;
        }
        snprintf(child, MAX_PATH_BYTES + 256, "%s/%s", path, e->d_name);
        result = remove_tree(child, depth + 1, cancel);
        free(child);
    }
    closedir(d);
    return result ? result : rmdir(path);
}

static int delete_entry(const struct http_request *r, struct http_stream *stream)
{
    char path[MAX_PATH_BYTES];

    if (query_path(r, "path", path) != 0 || is_volume(path)) return error(stream, 400, "bad_path");
    if (remove_tree(path, 0, NULL) != 0) return fail(stream);
    return reply(stream, 200, "{}");
}

/* Sizes up a tree: a job's total, and the folder size the page asks for. */
static int measure(const char *path, int depth, uint64_t *bytes, uint64_t *files)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    int result = 0;

    if (lstat(path, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode)) return 0;
    if (!S_ISDIR(st.st_mode)) {
        *bytes += (uint64_t)st.st_size;
        ++*files;
        return 0;
    }
    if (depth >= MAX_DEPTH) return -1;
    d = opendir(path);
    if (!d) return -1;
    while (!result && (e = readdir(d)) != NULL) {
        char *child;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        child = malloc(MAX_PATH_BYTES + 256);
        if (!child) {
            result = -1;
            break;
        }
        snprintf(child, MAX_PATH_BYTES + 256, "%s/%s", path, e->d_name);
        result = measure(child, depth + 1, bytes, files);
        free(child);
    }
    closedir(d);
    return result;
}

static int folder_size(const struct http_request *r, struct http_stream *stream)
{
    char path[MAX_PATH_BYTES], json[96];
    uint64_t bytes = 0, files = 0;

    if (query_path(r, "path", path) != 0) return error(stream, 400, "bad_path");
    if (measure(path, 0, &bytes, &files) != 0) return fail(stream);
    snprintf(json, sizeof json, "{\"bytes\":%" PRIu64 ",\"files\":%" PRIu64 "}", bytes, files);
    return reply(stream, 200, json);
}

/* ---- transfers that every page shows --------------------------------------- */

static void client_of(const struct http_request *r, char *out)
{
    size_t i;

    out[0] = 0;
    if (http_query_text(r->query, "client", out, CLIENT_BYTES) != 0) {
        out[0] = 0;
        return;
    }
    for (i = 0; out[i]; i++)
        if (!((out[i] >= '0' && out[i] <= '9') || (out[i] >= 'a' && out[i] <= 'z'))) {
            out[0] = 0;
            return;
        }
}

/* NULL if every slot is in use: that transfer then runs without being shown. */
static struct transfer *transfer_begin(const char *kind, const char *path, const char *client, uint64_t size)
{
    struct transfer *t = NULL;
    const char *name = strrchr(path, '/');
    long now = now_ms();
    int i;

    pthread_mutex_lock(&g_transfer_lock);
    for (i = 0; i < MAX_TRANSFERS; i++) {
        struct transfer *c = &g_transfers[i];

        if (!c->id) {
            t = c;
            break;
        }
        if (c->ended && (!t || c->ended < t->ended)) t = c;
    }
    if (t) {
        memset(t, 0, sizeof *t);
        if (!++g_transfer_id) g_transfer_id = 1;
        t->id = g_transfer_id;
        t->kind = kind;
        t->state = "active";
        snprintf(t->name, sizeof t->name, "%s", name && name[1] ? name + 1 : path);
        snprintf(t->client, sizeof t->client, "%s", client);
        t->size = size;
        t->started = now;
    }
    pthread_mutex_unlock(&g_transfer_lock);
    return t;
}

static void transfer_add(struct transfer *t, uint64_t bytes)
{
    if (t) __atomic_add_fetch(&t->done, bytes, __ATOMIC_RELAXED);
}

static void transfer_end(struct transfer *t, const char *state)
{
    if (!t) return;
    pthread_mutex_lock(&g_transfer_lock);
    t->state = state;
    t->ended = now_ms();
    pthread_mutex_unlock(&g_transfer_lock);
}

static int transfers(struct http_stream *stream)
{
    struct text t = {NULL, 0, 0, 0};
    long now = now_ms();
    int first = 1, i, result;

    add_text(&t, "{\"transfers\":[");
    pthread_mutex_lock(&g_transfer_lock);
    for (i = 0; i < MAX_TRANSFERS; i++) {
        struct transfer *c = &g_transfers[i];
        char numbers[128];

        if (c->id && c->ended && now - c->ended >= TRANSFER_KEEP_MS) c->id = 0;
        if (!c->id) continue;
        snprintf(numbers, sizeof numbers, "%s{\"id\":%u,\"size\":%" PRIu64 ",\"done\":%" PRIu64 ",\"ms\":%ld,\"state\":\"", first ? "" : ",", c->id, c->size,
            __atomic_load_n(&c->done, __ATOMIC_RELAXED), (c->ended ? c->ended : now) - c->started);
        first = 0;
        add_text(&t, numbers);
        add_text(&t, c->state);
        add_text(&t, "\",\"kind\":\"");
        add_text(&t, c->kind);
        add_text(&t, "\",\"client\":\"");
        add_text(&t, c->client);
        add_text(&t, "\",\"name\":");
        add_json_string(&t, c->name);
        add_text(&t, "}");
    }
    pthread_mutex_unlock(&g_transfer_lock);
    add_text(&t, "]}");
    if (t.failed) {
        free(t.data);
        return error(stream, 503, "no_memory");
    }
    result = reply(stream, 200, t.data);
    free(t.data);
    return result;
}

/* ---- upload ---------------------------------------------------------------- */

/* Where an upload's time went since the last line about it: waiting for the network to
 * fill a block, or for the disk to take one. A write only waits once the cache is full,
 * so a disk that cannot keep up shows as the disk's share growing. */
struct pace {
    long long since, read_us, write_us, slowest_read, slowest_write;
    uint64_t bytes;
};

static void say_pace(const char *path, uint64_t total, struct pace *p, long long now)
{
    long long us = now - p->since, busy = p->read_us + p->write_us;
    char line[224];

    if (g_log && us > 0 && busy > 0) {
        snprintf(line, sizeof line, "upload %.*s at %.1f GiB: %" PRIu64 " MB/s over %lld s, disk %lld%%, network %lld%%, slowest write %lld ms, "
            "slowest read %lld ms", (int)root_length(path), path, (double)total / (1024.0 * 1024 * 1024), p->bytes / (uint64_t)us,
            (us + 500000) / 1000000, p->write_us * 100 / busy, p->read_us * 100 / busy, p->slowest_write / 1000, p->slowest_read / 1000);
        g_log(line);
    }
    memset(p, 0, sizeof *p);
    p->since = now;
}

/* Whole blocks to the disk, not whatever one read returned: what arrived with the head
 * starts the first block, so every write but the last is IO_BYTES at a multiple of it. */
static int receive(const struct http_request *r, struct http_stream *stream, int fd, uint64_t *total, struct transfer *t, const char *path)
{
    uint8_t *buffer = malloc(IO_BYTES);
    uint64_t want = r->length;
    size_t have = r->body_length < want ? r->body_length : (size_t)want;
    struct pace pace = {0};
    int failed = 0;

    if (!buffer) {
        errno = ENOMEM;
        return -1;
    }
    if (have) memcpy(buffer, r->body, have);
    pace.since = now_us();
    while (*total < want && !failed) {
        size_t room = want - *total < IO_BYTES ? (size_t)(want - *total) : IO_BYTES;
        long long before = now_us(), between, after;

        while (have < room) {
            ssize_t n = stream->read(stream->context, buffer + have, room - have);

            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                failed = 1;
                break;
            }
            have += (size_t)n;
        }
        between = now_us();
        if (have && write(fd, buffer, have) != (ssize_t)have) failed = 1;
        after = now_us();
        *total += have;
        transfer_add(t, have);
        pace.bytes += have;
        pace.read_us += between - before;
        pace.write_us += after - between;
        if (between - before > pace.slowest_read) pace.slowest_read = between - before;
        if (after - between > pace.slowest_write) pace.slowest_write = after - between;
        if (after - pace.since >= SAMPLE_MS * 1000LL && *total < want && !failed) say_pace(path, *total, &pace, after);
        have = 0;
    }
    free(buffer);
    return failed ? -1 : 0;
}

static int upload(const struct http_request *r, struct http_stream *stream)
{
    char path[MAX_PATH_BYTES], part[MAX_PATH_BYTES], parent[MAX_PATH_BYTES], json[160], client[CLIENT_BYTES];
    struct transfer *t;
    struct stat st;
    uint64_t total = 0;
    long started = now_ms(), written, sync_ms = 0;
    char *slash;
    int fd, code;

    if (query_path(r, "path", path) != 0 || is_volume(path)) return error(stream, 400, "bad_path");
    if (!r->has_length) return error(stream, 400, "length_required");
    if (lstat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return error(stream, 409, "is_folder");
        if (!http_query_flag(r->query, "overwrite")) return error(stream, 409, "exists");
    }
    snprintf(parent, sizeof parent, "%s", path);
    slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    if (r->length > free_bytes(parent)) return error(stream, 409, "no_space");
    snprintf(part, sizeof part, "%s" PART_SUFFIX, path);
    fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return fail(stream);
    client_of(r, client);
    t = transfer_begin("upload", path, client, r->length);
    if (receive(r, stream, fd, &total, t, path) != 0) {
        code = errno;
        close(fd);
        unlink(part);
        transfer_end(t, "failed");
        errno = code;
        return fail(stream);
    }
    written = now_ms() - started;
    if (total >= SYNC_FROM) {
        long before = now_ms();

        if (fsync(fd) != 0) {
            code = errno;
            close(fd);
            unlink(part);
            transfer_end(t, "failed");
            errno = code;
            return fail(stream);
        }
        sync_ms = now_ms() - before;
    }
    close(fd);
    if (rename(part, path) != 0) {
        /* exFAT refuses to rename over a file. */
        if (errno != EEXIST || unlink(path) != 0 || rename(part, path) != 0) {
            code = errno;
            unlink(part);
            transfer_end(t, "failed");
            errno = code;
            return fail(stream);
        }
    }
    transfer_end(t, "done");
    note_transfer("upload", path, total, written, sync_ms);
    snprintf(json, sizeof json, "{\"bytes\":%" PRIu64 ",\"ms\":%ld,\"sync_ms\":%ld}", total, written, sync_ms);
    return reply(stream, 200, json);
}

/* ---- download ------------------------------------------------------------- */

static int parse_range(const char *range, uint64_t size, uint64_t *first, uint64_t *last)
{
    char *end;

    if (strncmp(range, "bytes=", 6) || range[6] < '0' || range[6] > '9') return -1;
    *first = strtoull(range + 6, &end, 10);
    if (*end != '-') return -1;
    *last = end[1] ? strtoull(end + 1, &end, 10) : size - 1;
    if (end[0] && end[0] != '-') return -1;
    if (*last >= size) *last = size - 1;
    return *first < size && *first <= *last ? 0 : -1;
}

static void add_attachment_name(struct text *t, const char *name)
{
    add_text(t, "Content-Disposition: attachment; filename*=UTF-8''");
    for (; *name; name++) {
        unsigned char c = (unsigned char)*name;
        char escaped[4];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
            add(t, name, 1);
        } else {
            snprintf(escaped, sizeof escaped, "%%%02X", c);
            add_text(t, escaped);
        }
    }
    add_text(t, "\r\n");
}

static int send_file(int fd, uint64_t offset, uint64_t left, struct http_stream *stream, struct transfer *t)
{
    uint8_t *buffer = malloc(IO_BYTES);
    int result = 0;

    if (!buffer || lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        free(buffer);
        return -1;
    }
    while (left && !result) {
        ssize_t n = read(fd, buffer, left < IO_BYTES ? (size_t)left : IO_BYTES);

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) result = -1;
        else if (http_write_all(stream, buffer, (size_t)n) != 0) result = -1;
        else {
            left -= (uint64_t)n;
            transfer_add(t, (uint64_t)n);
        }
    }
    free(buffer);
    return result;
}

static int download(const struct http_request *r, struct http_stream *stream)
{
    char path[MAX_PATH_BYTES], line[160];
    struct text head = {NULL, 0, 0, 0};
    struct transfer *t;
    struct stat st;
    uint64_t first = 0, last;
    long started = now_ms();
    int fd, partial = 0, result;

    if (query_path(r, "path", path) != 0) return error(stream, 400, "bad_path");
    fd = open(path, O_RDONLY);
    if (fd < 0) return fail(stream);
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return error(stream, 409, "is_folder");
    }
    last = st.st_size ? (uint64_t)st.st_size - 1 : 0;
    if (r->range[0] && st.st_size) {
        if (parse_range(r->range, (uint64_t)st.st_size, &first, &last) != 0) {
            close(fd);
            return error(stream, 400, "bad_range");
        }
        partial = 1;
    }
    add_text(&head, partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n");
    add_text(&head, "Content-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\nConnection: close\r\n");
    snprintf(line, sizeof line, "Content-Length: %" PRIu64 "\r\n", st.st_size ? last - first + 1 : 0);
    add_text(&head, line);
    if (partial) {
        snprintf(line, sizeof line, "Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n", first, last, (uint64_t)st.st_size);
        add_text(&head, line);
    }
    add_attachment_name(&head, strrchr(path, '/') + 1);
    add_text(&head, "\r\n");
    result = head.failed ? -1 : http_write_all(stream, head.data, head.used);
    free(head.data);
    t = transfer_begin("download", path, "", st.st_size ? last - first + 1 : 0);
    if (!result && st.st_size) result = send_file(fd, first, last - first + 1, stream, t);
    close(fd);
    transfer_end(t, result ? "failed" : "done");
    if (!result) note_transfer("download", path, st.st_size ? last - first + 1 : 0, now_ms() - started, 0);
    return result;
}

/* ---- a folder as a tar stream --------------------------------------------- */

static void octal(char *out, size_t size, uint64_t value)
{
    size_t i = size - 1;

    out[i] = 0;
    while (i--) {
        out[i] = (char)('0' + (value & 7));
        value >>= 3;
    }
}

static void tar_header(uint8_t *h, const char *name, uint64_t size, int64_t mtime, char type)
{
    unsigned sum = 0;
    int i;

    memset(h, 0, TAR_BLOCK);
    memcpy(h, name, strlen(name) < 100 ? strlen(name) : 100);
    octal((char *)h + 100, 8, type == '5' ? 0777 : 0666);
    octal((char *)h + 108, 8, 0);
    octal((char *)h + 116, 8, 0);
    if (size >> 33) {                   /* GNU's base-256, past what eleven octal digits hold */
        h[124] = 0x80;
        for (i = 0; i < 8; i++) h[135 - i] = (uint8_t)(size >> (i * 8));
    } else {
        octal((char *)h + 124, 12, size);
    }
    octal((char *)h + 136, 12, (uint64_t)mtime);
    memset(h + 148, ' ', 8);
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar  ", 8);
    for (i = 0; i < TAR_BLOCK; i++) sum += h[i];
    octal((char *)h + 148, 7, sum);
}

/* A name of 100 bytes or more goes first, as GNU's long-name entry. */
static int tar_entry(struct http_stream *stream, const char *name, uint64_t size, int64_t mtime, char type)
{
    uint8_t block[TAR_BLOCK];
    size_t length = strlen(name);

    if (length >= 100) {
        size_t sent;

        tar_header(block, "././@LongLink", length + 1, 0, 'L');
        if (http_write_all(stream, block, TAR_BLOCK) != 0) return -1;
        for (sent = 0; sent <= length; sent += TAR_BLOCK) {
            size_t n = length + 1 - sent < TAR_BLOCK ? length + 1 - sent : TAR_BLOCK;

            memset(block, 0, TAR_BLOCK);
            memcpy(block, name + sent, n);
            if (http_write_all(stream, block, TAR_BLOCK) != 0) return -1;
        }
    }
    tar_header(block, name, size, mtime, type);
    return http_write_all(stream, block, TAR_BLOCK);
}

static int tar_tree(struct http_stream *stream, const char *path, const char *name, int depth, uint64_t *bytes, struct transfer *t)
{
    static const uint8_t zeros[TAR_BLOCK];
    struct stat st;

    if (lstat(path, &st) != 0 || depth >= MAX_DEPTH) return 0;  /* gone since: leave it out */
    if (S_ISREG(st.st_mode)) {
        int fd = open(path, O_RDONLY), result;
        size_t pad = (TAR_BLOCK - (size_t)(st.st_size % TAR_BLOCK)) % TAR_BLOCK;

        if (fd < 0) return 0;
        result = tar_entry(stream, name, (uint64_t)st.st_size, st.st_mtime, '0');
        if (!result && st.st_size) result = send_file(fd, 0, (uint64_t)st.st_size, stream, t);
        close(fd);
        *bytes += (uint64_t)st.st_size;
        return !result && pad ? http_write_all(stream, zeros, pad) : result;
    }
    if (S_ISDIR(st.st_mode)) {
        char *child_path, *child_name, folder[MAX_PATH_BYTES + 2];
        struct dirent *e;
        DIR *d;
        int result;

        snprintf(folder, sizeof folder, "%s/", name);
        result = tar_entry(stream, folder, 0, st.st_mtime, '5');
        d = result ? NULL : opendir(path);
        if (!d) return result;
        child_path = malloc(MAX_PATH_BYTES + 256);
        child_name = malloc(MAX_PATH_BYTES + 256);
        while (!result && child_path && child_name && (e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(child_path, MAX_PATH_BYTES + 256, "%s/%s", path, e->d_name);
            snprintf(child_name, MAX_PATH_BYTES + 256, "%s/%s", name, e->d_name);
            result = tar_tree(stream, child_path, child_name, depth + 1, bytes, t);
        }
        free(child_path);
        free(child_name);
        closedir(d);
        return result;
    }
    return 0;
}

static int download_folder(const struct http_request *r, struct http_stream *stream)
{
    static const uint8_t zeros[2 * TAR_BLOCK];
    char path[MAX_PATH_BYTES], name[300];
    struct text head = {NULL, 0, 0, 0};
    struct transfer *t;
    const char *base;
    uint64_t bytes = 0;
    long started = now_ms();
    int result;

    if (query_path(r, "path", path) != 0) return error(stream, 400, "bad_path");
    base = strrchr(path, '/');
    base = base && base[1] ? base + 1 : "volume";
    snprintf(name, sizeof name, "%s.tar", base);
    add_text(&head, "HTTP/1.1 200 OK\r\nContent-Type: application/x-tar\r\nCache-Control: no-store\r\nConnection: close\r\n");
    add_attachment_name(&head, name);
    add_text(&head, "\r\n");
    result = head.failed ? -1 : http_write_all(stream, head.data, head.used);
    free(head.data);
    t = transfer_begin("download", path, "", 0);
    if (!result) result = tar_tree(stream, path, base, 0, &bytes, t);
    if (!result) result = http_write_all(stream, zeros, sizeof zeros);
    transfer_end(t, result ? "failed" : "done");
    if (!result) note_transfer("folder download", path, bytes, now_ms() - started, 0);
    return result;
}

/* ---- copying and moving on the console ------------------------------------ */

static void job_add(uint64_t bytes)
{
    pthread_mutex_lock(&g_job.lock);
    g_job.done += bytes;
    pthread_mutex_unlock(&g_job.lock);
    transfer_add(g_job_transfer, bytes);
}

static int copy_file(const char *from, const char *to, const struct stat *st, uint8_t *buffer)
{
    char part[MAX_PATH_BYTES + 16];
    int in = open(from, O_RDONLY), out, code = 0;

    if (in < 0) return -1;
    snprintf(part, sizeof part, "%s" PART_SUFFIX, to);
    out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        code = errno;
        close(in);
        errno = code;
        return -1;
    }
    for (;;) {
        ssize_t n = read(in, buffer, IO_BYTES);

        if (g_job.cancel) {
            code = ECANCELED;
            break;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) code = errno;
        if (n <= 0) break;
        if (write(out, buffer, (size_t)n) != n) {
            code = errno ? errno : EIO;
            break;
        }
        job_add((uint64_t)n);
    }
    if (!code && (uint64_t)st->st_size >= SYNC_FROM && fsync(out) != 0) code = errno;
    close(out);
    close(in);
    if (!code && rename(part, to) != 0) code = errno;
    if (code) {
        unlink(part);
        errno = code;
        return -1;
    }
    return 0;
}

static int copy_tree(const char *from, const char *to, int depth, uint8_t *buffer)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    int result = 0;

    if (g_job.cancel) {
        errno = ECANCELED;
        return -1;
    }
    if (lstat(from, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode)) return 0;
    if (S_ISREG(st.st_mode)) return copy_file(from, to, &st, buffer);
    if (!S_ISDIR(st.st_mode)) return 0;
    if (depth >= MAX_DEPTH) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(to, 0777) != 0 && errno != EEXIST) return -1;
    d = opendir(from);
    if (!d) return -1;
    while (!result && (e = readdir(d)) != NULL) {
        char *child_from, *child_to;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        child_from = malloc(MAX_PATH_BYTES + 256);
        child_to = malloc(MAX_PATH_BYTES + 256);
        if (!child_from || !child_to) {
            free(child_from);
            free(child_to);
            errno = ENOMEM;
            result = -1;
            break;
        }
        snprintf(child_from, MAX_PATH_BYTES + 256, "%s/%s", from, e->d_name);
        snprintf(child_to, MAX_PATH_BYTES + 256, "%s/%s", to, e->d_name);
        result = copy_tree(child_from, child_to, depth + 1, buffer);
        free(child_from);
        free(child_to);
    }
    closedir(d);
    return result;
}

static void job_finish(const char *state, const char *error_code)
{
    transfer_end(g_job_transfer, !strcmp(state, "done") ? "done" : "failed");
    g_job_transfer = NULL;
    pthread_mutex_lock(&g_job.lock);
    g_job.state = state;
    snprintf(g_job.error, sizeof g_job.error, "%s", error_code);
    g_job.ended = now_ms();
    g_job.running = 0;
    pthread_mutex_unlock(&g_job.lock);
}

/* ---- a job that runs elsewhere: smb.c's copies from a share --------------- */

int files_job_run(struct http_stream *stream, const char *kind, const char *label, const char *to, const char *client, void *(*body)(void *), void *arg,
    int *started)
{
    struct stat st;
    char parent[MAX_PATH_BYTES], *slash;
    pthread_attr_t attr;
    pthread_t thread;
    int result;

    *started = 0;
    if (!usable(to) || is_volume(to)) return error(stream, 400, "bad_path");
    if (lstat(to, &st) == 0) return error(stream, 409, "exists");
    snprintf(parent, sizeof parent, "%s", to);
    slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    if (lstat(parent, &st) != 0) return fail(stream);
    if (!S_ISDIR(st.st_mode)) return error(stream, 409, "not_folder");
    pthread_mutex_lock(&g_job.lock);
    if (g_job.running) {
        pthread_mutex_unlock(&g_job.lock);
        return error(stream, 409, "job_running");
    }
    g_job.running = 1;
    g_job.cancel = 0;
    g_job.move = 0;
    g_job.kind = kind;
    g_job.state = "running";
    g_job.error[0] = 0;
    g_job.done = 0;
    g_job.total = 0;
    g_job.started = now_ms();
    g_job.ended = 0;
    snprintf(g_job.from, sizeof g_job.from, "%s", label);
    snprintf(g_job.to, sizeof g_job.to, "%s", to);
    snprintf(g_job_client, sizeof g_job_client, "%s", client);
    pthread_mutex_unlock(&g_job.lock);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, JOB_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    result = pthread_create(&thread, &attr, body, arg);
    pthread_attr_destroy(&attr);
    if (result) {
        job_finish("failed", "no_thread");
        return error(stream, 503, "no_thread");
    }
    *started = 1;
    return reply(stream, 200, "{}");
}

int files_job_size(uint64_t bytes)
{
    char parent[MAX_PATH_BYTES], *slash;

    pthread_mutex_lock(&g_job.lock);
    g_job.total = bytes;
    pthread_mutex_unlock(&g_job.lock);
    snprintf(parent, sizeof parent, "%s", g_job.to);
    slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    if (bytes > free_bytes(parent)) return -1;
    g_job_transfer = transfer_begin(g_job.kind, g_job.from, g_job_client, bytes);
    return 0;
}

void files_job_add(uint64_t bytes)
{
    job_add(bytes);
}

int files_job_canceled(void)
{
    return __atomic_load_n(&g_job.cancel, __ATOMIC_RELAXED);
}

void files_job_end(const char *error_code)
{
    if (!error_code || !error_code[0]) {
        note_transfer(g_job.kind, g_job.to, g_job.done, now_ms() - g_job.started, 0);
        job_finish("done", "");
        return;
    }
    /* The target did not exist when the job began, so all of it is this job's. */
    remove_tree(g_job.to, 0, NULL);
    if (!strcmp(error_code, "canceled")) job_finish("canceled", "");
    else job_finish("failed", error_code);
}

static void *job_thread(void *unused)
{
    uint8_t *buffer = malloc(IO_BYTES);
    uint64_t bytes = 0, files = 0;
    char parent[MAX_PATH_BYTES], *slash;

    (void)unused;
    if (!buffer) {
        job_finish("failed", "no_memory");
        return NULL;
    }
    snprintf(parent, sizeof parent, "%s", g_job.to);
    slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    if (measure(g_job.from, 0, &bytes, &files) != 0) {
        free(buffer);
        job_finish("failed", errno_code(errno));
        return NULL;
    }
    pthread_mutex_lock(&g_job.lock);
    g_job.total = bytes;
    pthread_mutex_unlock(&g_job.lock);
    if (bytes > free_bytes(parent)) {
        free(buffer);
        job_finish("failed", "no_space");
        return NULL;
    }
    g_job_transfer = transfer_begin(g_job.kind, g_job.from, g_job_client, bytes);
    if (copy_tree(g_job.from, g_job.to, 0, buffer) != 0) {
        int code = errno;

        free(buffer);
        /* The target did not exist when the job began, so all of it is this job's. */
        remove_tree(g_job.to, 0, NULL);
        job_finish(code == ECANCELED ? "canceled" : "failed", code == ECANCELED ? "" : errno_code(code));
        return NULL;
    }
    free(buffer);
    note_transfer(g_job.kind, g_job.to, bytes, now_ms() - g_job.started, 0);
    if (g_job.move && remove_tree(g_job.from, 0, NULL) != 0) {
        job_finish("failed", "source_kept");
        return NULL;
    }
    job_finish("done", "");
    return NULL;
}

/* One job at a time: a copy, a move or a soak. */
static int start_thread(struct http_stream *stream, void *(*body)(void *), const char *kind, const char *from, const char *to, const char *client)
{
    pthread_attr_t attr;
    pthread_t thread;
    int result;

    pthread_mutex_lock(&g_job.lock);
    if (g_job.running) {
        pthread_mutex_unlock(&g_job.lock);
        return error(stream, 409, "job_running");
    }
    g_job.running = 1;
    g_job.cancel = 0;
    g_job.move = !strcmp(kind, "move");
    g_job.kind = kind;
    g_job.state = "running";
    g_job.error[0] = 0;
    g_job.done = 0;
    g_job.total = 0;
    g_job.started = now_ms();
    g_job.ended = 0;
    snprintf(g_job.from, sizeof g_job.from, "%s", from);
    snprintf(g_job.to, sizeof g_job.to, "%s", to);
    snprintf(g_job_client, sizeof g_job_client, "%s", client);
    pthread_mutex_unlock(&g_job.lock);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, JOB_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    result = pthread_create(&thread, &attr, body, NULL);
    pthread_attr_destroy(&attr);
    if (result) {
        job_finish("failed", "no_thread");
        return error(stream, 503, "no_thread");
    }
    return reply(stream, 200, "{}");
}

static int start_job(const struct http_request *r, struct http_stream *stream, int move)
{
    char from[MAX_PATH_BYTES], to[MAX_PATH_BYTES], client[CLIENT_BYTES];
    struct stat st;
    size_t length;

    if (query_path(r, "from", from) != 0 || query_path(r, "to", to) != 0 || is_volume(from) || is_volume(to))
        return error(stream, 400, "bad_path");
    length = strlen(from);
    if (!strcmp(from, to) || (!strncmp(from, to, length) && to[length] == '/')) return error(stream, 409, "into_itself");
    if (lstat(from, &st) != 0) return fail(stream);
    if (lstat(to, &st) == 0) return error(stream, 409, "exists");
    if (move && rename(from, to) == 0) {
        pthread_mutex_lock(&g_job.lock);
        if (!g_job.running) {
            g_job.state = "done";
            g_job.error[0] = 0;
            g_job.done = g_job.total = 0;
            g_job.started = g_job.ended = now_ms();
        }
        pthread_mutex_unlock(&g_job.lock);
        return reply(stream, 200, "{\"instant\":true}");
    }
    client_of(r, client);
    return start_thread(stream, job_thread, move ? "move" : "copy", from, to, client);
}

static int job_status(struct http_stream *stream)
{
    char json[220];
    long ms;

    pthread_mutex_lock(&g_job.lock);
    ms = (g_job.running ? now_ms() : g_job.ended) - g_job.started;
    snprintf(json, sizeof json, "{\"state\":\"%s\",\"kind\":\"%s\",\"error\":\"%s\",\"done\":%" PRIu64 ",\"total\":%" PRIu64 ",\"ms\":%ld,\"move\":%d}", g_job.state,
        g_job.kind, g_job.error, g_job.done, g_job.total, ms, g_job.move);
    pthread_mutex_unlock(&g_job.lock);
    return reply(stream, 200, json);
}

void files_stop(void)
{
    int running, i;

    pthread_mutex_lock(&g_job.lock);
    g_job.cancel = 1;
    pthread_mutex_unlock(&g_job.lock);
    for (i = 0; i < STOP_WAIT_TICKS; i++) {
        pthread_mutex_lock(&g_job.lock);
        running = g_job.running;
        pthread_mutex_unlock(&g_job.lock);
        if (!running) return;
        usleep(STOP_TICK_US);
    }
}

/* The test file of a run that did not finish. */
static void sweep_volume(const char *volume)
{
    char path[MAX_PATH_BYTES], line[MAX_PATH_BYTES + 48];

    snprintf(path, sizeof path, "%s" BENCH_FILE, volume);
    if (unlink(path) == 0 && g_log) {
        snprintf(line, sizeof line, "removed a test file left on %s", volume);
        g_log(line);
    }
}

void files_sweep(void)
{
    int i;

#ifdef FILES_TESTING
    if (files_test_root) sweep_volume(files_test_root);
#endif
    sweep_volume("/data");
    for (i = 0; i < 10; i++) {
        char path[16];

        snprintf(path, sizeof path, "/mnt/ext%d", i);
        sweep_volume(path);
        snprintf(path, sizeof path, "/mnt/usb%d", i);
        sweep_volume(path);
    }
}

/* ---- how fast a disk is, with no network ----------------------------------- */

static long mbps_of(uint64_t bytes, long ms)
{
    return (long)(bytes * 8 / (uint64_t)(ms > 0 ? ms : 1) / 1000);
}

static void fill_block(uint8_t *block, size_t length)
{
    uint32_t state = 0x9e3779b9u;
    size_t i;

    for (i = 0; i + 4 <= length; i += 4) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        memcpy(block + i, &state, 4);
    }
}

static int bench(const struct http_request *r, struct http_stream *stream)
{
    static int busy;
    char path[MAX_PATH_BYTES], file[MAX_PATH_BYTES + 32], json[128], line[MAX_PATH_BYTES + 128];
    uint8_t *allocated, *block;
    long started, written_ms = 0, read_ms = 0;
    int fd, direct = 1, i, failed = 0;
    uint64_t done = 0;

    if (query_path(r, "path", path) != 0 || !is_volume(path)) return error(stream, 400, "bad_path");
    if (free_bytes(path) < (uint64_t)BENCH_MIB * 2 * 1024 * 1024) return error(stream, 409, "no_space");
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_RELAXED)) return error(stream, 409, "busy");
    allocated = malloc(IO_BYTES + BENCH_ALIGN);
    if (!allocated) {
        busy = 0;
        return error(stream, 503, "no_memory");
    }
    block = (uint8_t *)(((uintptr_t)allocated + BENCH_ALIGN - 1) & ~(uintptr_t)(BENCH_ALIGN - 1));
    fill_block(block, IO_BYTES);
    snprintf(file, sizeof file, "%s" BENCH_FILE, path);
    fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT_BIT, 0666);
    if (fd < 0) {                       /* exFAT may refuse it */
        direct = 0;
        fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    if (fd < 0) {
        free(allocated);
        busy = 0;
        return fail(stream);
    }
    started = now_ms();
    for (i = 0; i < BENCH_MIB && !failed; i++) failed = write(fd, block, IO_BYTES) != IO_BYTES;
    if (!failed && fsync(fd) != 0) failed = 1;
    written_ms = now_ms() - started;
    close(fd);
    fd = failed ? -1 : open(file, direct ? O_RDONLY | O_DIRECT_BIT : O_RDONLY);
    if (fd >= 0) {
        started = now_ms();
        while (done < (uint64_t)BENCH_MIB * IO_BYTES && read(fd, block, IO_BYTES) == IO_BYTES) done += IO_BYTES;
        read_ms = now_ms() - started;
        close(fd);
    }
    unlink(file);
    free(allocated);
    busy = 0;
    if (failed) return error(stream, 409, "failed");
    snprintf(line, sizeof line, "disk %s: write %ld Mbit/s, read %ld Mbit/s, %d MiB, %s", path, mbps_of((uint64_t)BENCH_MIB * IO_BYTES, written_ms),
        mbps_of(done, read_ms), BENCH_MIB, direct ? "O_DIRECT" : "through the cache, so the read may be the cache's");
    if (g_log) g_log(line);
    snprintf(json, sizeof json, "{\"write\":%ld,\"read\":%ld,\"direct\":%d}", mbps_of((uint64_t)BENCH_MIB * IO_BYTES, written_ms), mbps_of(done, read_ms), direct);
    return reply(stream, 200, json);
}

/* ---- routes ---------------------------------------------------------------- */

int files_route(const struct http_request *r, struct http_stream *stream)
{
    const char *path = r->path;
    int post = !strcmp(r->method, "POST");

    if (strncmp(path, "/fs/", 4)) return HTTP_NOT_MINE;
    path += 4;
    if (!post) {
        if (!strcmp(path, "volumes")) return volumes(stream);
        if (!strcmp(path, "list")) return list(r, stream);
        if (!strcmp(path, "get")) return download(r, stream);
        if (!strcmp(path, "tar")) return download_folder(r, stream);
        if (!strcmp(path, "size")) return folder_size(r, stream);
        if (!strcmp(path, "job")) return job_status(stream);
        if (!strcmp(path, "transfers")) return transfers(stream);
    } else {
        if (!strcmp(path, "put")) return upload(r, stream);
        if (!strcmp(path, "bench")) return bench(r, stream);
        if (!strcmp(path, "mkdir")) return make_folder(r, stream);
        if (!strcmp(path, "rename")) return rename_entry(r, stream);
        if (!strcmp(path, "delete")) return delete_entry(r, stream);
        if (!strcmp(path, "copy")) return start_job(r, stream, 0);
        if (!strcmp(path, "move")) return start_job(r, stream, 1);
        if (!strcmp(path, "cancel")) {
            pthread_mutex_lock(&g_job.lock);
            g_job.cancel = 1;
            pthread_mutex_unlock(&g_job.lock);
            return reply(stream, 200, "{}");
        }
    }
    return HTTP_NOT_MINE;
}
