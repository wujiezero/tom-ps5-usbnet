/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The file routes on a real temporary folder, through the real HTTP layer, with
 * requests fed in from memory. */
#include "files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed at line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

struct memory {
    const uint8_t *in;
    size_t in_length, in_at, chunk;
    uint8_t *out;
    size_t out_length;
};

static char g_root[256];
static int g_lines, g_saw_name, g_saw_upload, g_saw_pace;

static ssize_t memory_read(void *context, void *data, size_t length)
{
    struct memory *m = context;
    size_t left = m->in_length - m->in_at, n = length < left ? length : left;

    if (m->chunk && n > m->chunk) n = m->chunk;
    if (!n) return 0;                   /* the peer has gone */
    memcpy(data, m->in + m->in_at, n);
    m->in_at += n;
    return (ssize_t)n;
}

static ssize_t memory_write(void *context, const void *data, size_t length)
{
    struct memory *m = context;

    m->out = realloc(m->out, m->out_length + length + 1);
    memcpy(m->out + m->out_length, data, length);
    m->out_length += length;
    m->out[m->out_length] = 0;
    return (ssize_t)length;
}

static int fail_route(void *c, const struct http_request *r, struct http_stream *s)
{
    (void)c;
    return files_route(r, s);
}

static int noop_info(void *c, char *o, size_t n) { (void)c; (void)o; (void)n; return 0; }
static const struct http_backend g_backend = {.route = fail_route, .info = noop_info};

/* Sends a request; returns the status and leaves the body in `body`. */
static int request_full(const char *method, const char *target, const void *data, size_t length, size_t truncate_to, const char *headers, char **body, size_t *body_length)
{
    size_t room = 16384 + length;
    char *wire = malloc(room);
    int used = snprintf(wire, room, "%s %s HTTP/1.1\r\nHost: 192.0.2.5\r\n%s", method, target, headers ? headers : "");
    struct memory m = {0};
    struct http_stream stream = {&m, memory_read, memory_write};
    int status = 0;
    char *split;

    if (!strcmp(method, "POST")) used += snprintf(wire + used, room - (size_t)used, "Content-Length: %zu\r\n", length);
    used += snprintf(wire + used, room - (size_t)used, "\r\n");
    if (data) memcpy(wire + used, data, length);
    m.in = (uint8_t *)wire;
    m.in_length = (size_t)used + (truncate_to < length ? truncate_to : length);
    m.chunk = 7001;
    http_serve(&g_backend, &stream);
    free(wire);
    if (!m.out) return 0;
    sscanf((char *)m.out, "HTTP/1.1 %d", &status);
    split = strstr((char *)m.out, "\r\n\r\n");
    if (body && split) {
        *body_length = m.out_length - (size_t)(split + 4 - (char *)m.out);
        *body = malloc(*body_length + 1);
        memcpy(*body, split + 4, *body_length);
        (*body)[*body_length] = 0;
    }
    free(m.out);
    return status;
}

static int request(const char *method, const char *target, char **body)
{
    size_t length;

    return request_full(method, target, NULL, 0, 0, NULL, body, &length);
}

static int simple(const char *method, const char *target)
{
    char *body = NULL;
    int status = request(method, target, &body);

    free(body);
    return status;
}

/* Target with `path` under the root, percent-encoded as a browser does it. */
static void target(char *out, size_t capacity, const char *route, const char *key, const char *path)
{
    size_t at = (size_t)snprintf(out, capacity, "%s?%s=", route, key);
    char full[1024];
    const char *p;

    snprintf(full, sizeof full, "%s%s", g_root, path);
    for (p = full; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' || c == '_')
            out[at++] = (char)c;
        else at += (size_t)snprintf(out + at, capacity - at, "%%%02X", c);
    }
    out[at] = 0;
}

static void target2(char *out, size_t capacity, const char *route, const char *from, const char *to)
{
    char first[2048], second[2048];

    target(first, sizeof first, route, "from", from);
    target(second, sizeof second, "x", "to", to);
    snprintf(out, capacity, "%s&%s", first, second + 2);
}

static int exists(const char *path)
{
    char full[1024];

    snprintf(full, sizeof full, "%s%s", g_root, path);
    return access(full, F_OK) == 0;
}

static void write_local(const char *path, const char *text)
{
    char full[1024];
    FILE *f;

    snprintf(full, sizeof full, "%s%s", g_root, path);
    f = fopen(full, "w");
    CHECK(f);
    fputs(text, f);
    fclose(f);
}

static long size_of(const char *path)
{
    char full[1024];
    struct stat st;

    snprintf(full, sizeof full, "%s%s", g_root, path);
    return stat(full, &st) == 0 ? (long)st.st_size : -1;
}

static void wait_job(const char *state)
{
    int i;

    for (i = 0; i < 500; i++) {
        char *body = NULL;
        int found;

        CHECK(request("GET", "/fs/job", &body) == 200);
        found = strstr(body, state) != NULL;
        free(body);
        if (found) return;
        usleep(10000);
    }
    CHECK(!"the job did not end");
}

static void paths(void)
{
    static const char *const no[] = {"/", "/etc/passwd", "/data/../etc", "/data/./x", "/data//x", "/data/x/", "/database", "/mnt/ext", "/mnt/extra",
        "/mnt/ext12", "/mnt/usb", "/mnt/usb0/..", "/mnt/usbx", "/data/a\\b", "/data/a\nb", "/system/x", "/user/app", "relative", "", NULL};
    static const char *const yes[] = {"/data", "/data/a b", "/data/中文/文件.bin", "/mnt/ext0", "/mnt/ext9/x", "/mnt/usb3/y/z", "/data/...", "/data/..x", NULL};
    char big[1100];
    int i;

    for (i = 0; no[i]; i++) if (files_path_allowed(no[i])) { fprintf(stderr, "allowed: %s\n", no[i]); exit(1); }
    for (i = 0; yes[i]; i++) if (!files_path_allowed(yes[i])) { fprintf(stderr, "refused: %s\n", yes[i]); exit(1); }
    memcpy(big, "/data/", 6);
    memset(big + 6, 'x', sizeof big - 7);
    big[sizeof big - 1] = 0;
    CHECK(!files_path_allowed(big));
}

static void folders(void)
{
    char url[4096], *body = NULL;
    size_t length;

    CHECK(request("GET", "/fs/volumes", &body) == 200);
    CHECK(strstr(body, g_root) && strstr(body, "\"total\":"));
    free(body);
    target(url, sizeof url, "/fs/mkdir", "path", "/a");
    CHECK(simple("POST", url) == 200);
    CHECK(simple("POST", url) == 409);
    target(url, sizeof url, "/fs/mkdir", "path", "/no/such/parent");
    CHECK(simple("POST", url) == 404);
    write_local("/a/with \"quote\" \\ 中.txt", "hi");
    target(url, sizeof url, "/fs/list", "path", "/a");
    CHECK(request_full("GET", url, NULL, 0, 0, NULL, &body, &length) == 200);
    CHECK(strstr(body, "with \\\"quote\\\" \\\\ 中.txt") && strstr(body, "\"kind\":\"file\"") && strstr(body, "\"size\":2"));
    free(body);
    target(url, sizeof url, "/fs/list", "path", "/missing");
    CHECK(simple("GET", url) == 404);
}

static void uploads(void)
{
    enum { SIZE = 3 * 1024 * 1024 + 123 };
    uint8_t *data = malloc(SIZE);
    char url[4096], *body = NULL;
    size_t length;
    int i, fd;

    for (i = 0; i < SIZE; i++) data[i] = (uint8_t)(i * 31 + (i >> 9));
    target(url, sizeof url, "/fs/put", "path", "/a/up load.bin");
    CHECK(request_full("POST", url, data, SIZE, SIZE, NULL, &body, &length) == 200);
    CHECK(strstr(body, "\"bytes\":3145851"));
    free(body);
    CHECK(size_of("/a/up load.bin") == SIZE);
    {
        char full[1024];
        uint8_t *back = malloc(SIZE);

        snprintf(full, sizeof full, "%s/a/up load.bin", g_root);
        fd = open(full, O_RDONLY);
        CHECK(read(fd, back, SIZE) == SIZE && !memcmp(back, data, SIZE));
        close(fd);
        free(back);
    }
    CHECK(!exists("/a/up load.bin.tom-part"));
    /* Refused without overwrite, and the file is unchanged. */
    CHECK(request_full("POST", url, data, 10, 10, NULL, &body, &length) == 409);
    CHECK(strstr(body, "exists"));
    free(body);
    CHECK(size_of("/a/up load.bin") == SIZE);
    /* A connection lost part-way leaves the old file and no part file. */
    {
        char with[4200];

        snprintf(with, sizeof with, "%s&overwrite=1", url);
        CHECK(request_full("POST", with, data, 1000, 400, NULL, &body, &length) != 200);
        free(body);
        body = NULL;
        CHECK(size_of("/a/up load.bin") == SIZE);
        CHECK(!exists("/a/up load.bin.tom-part"));
        CHECK(request_full("POST", with, data, 1000, 1000, NULL, &body, &length) == 200);
        free(body);
        CHECK(size_of("/a/up load.bin") == 1000);
    }
    /* An empty file is a file. */
    target(url, sizeof url, "/fs/put", "path", "/a/empty");
    CHECK(simple("POST", url) == 200);
    CHECK(size_of("/a/empty") == 0);
    /* Over a folder, no. */
    target(url, sizeof url, "/fs/put", "path", "/a");
    CHECK(simple("POST", url) == 400 || simple("POST", url) == 409);
    free(data);
}

static void downloads(void)
{
    char url[4096], *body = NULL;
    size_t length;
    size_t at;
    int found = 0;

    write_local("/a/range.txt", "0123456789");
    target(url, sizeof url, "/fs/get", "path", "/a/range.txt");
    CHECK(request_full("GET", url, NULL, 0, 0, NULL, &body, &length) == 200);
    CHECK(length == 10 && !memcmp(body, "0123456789", 10));
    free(body);
    CHECK(request_full("GET", url, NULL, 0, 0, "Range: bytes=3-5\r\n", &body, &length) == 206);
    CHECK(length == 3 && !memcmp(body, "345", 3));
    free(body);
    CHECK(request_full("GET", url, NULL, 0, 0, "Range: bytes=7-\r\n", &body, &length) == 206);
    CHECK(length == 3 && !memcmp(body, "789", 3));
    free(body);
    CHECK(request_full("GET", url, NULL, 0, 0, "Range: bytes=99-\r\n", &body, &length) == 400);
    free(body);
    target(url, sizeof url, "/fs/get", "path", "/a");
    CHECK(simple("GET", url) == 409);
    /* A folder, with a name past tar's 100 bytes. */
    target(url, sizeof url, "/fs/mkdir", "path", "/a/deep");
    CHECK(simple("POST", url) == 200);
    {
        char name[160], local[200];

        memset(name, 'n', 130);
        name[130] = 0;
        snprintf(local, sizeof local, "/a/deep/%s", name);
        write_local(local, "long name");
    }
    target(url, sizeof url, "/fs/tar", "path", "/a");
    CHECK(request_full("GET", url, NULL, 0, 0, NULL, &body, &length) == 200);
    /* Walks the blocks as tar would. */
    for (at = 0; at + 512 <= length && body[at]; ) {
        const char *block = body + at;
        unsigned long stored = strtoul(block + 148, NULL, 8), sum = 0, size = strtoul(block + 124, NULL, 8);
        int i;

        for (i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? ' ' : (unsigned char)block[i];
        CHECK(sum == stored);
        CHECK(!memcmp(block + 257, "ustar  ", 8));
        if (block[156] == 'L') {
            found = 1;
            CHECK(strstr(block + 512, "/deep/nnnnnnnnnn") && strlen(block + 512) + 1 == size);
        }
        at += 512 + (size + 511) / 512 * 512;
    }
    CHECK(found);
    CHECK(at + 1024 == length);
    free(body);
}

static void changes(void)
{
    char url[8192], *body = NULL;
    char link_path[1024], outside[1024];
    int fd;

    target2(url, sizeof url, "/fs/rename", "/a/range.txt", "/a/renamed.txt");
    CHECK(simple("POST", url) == 200);
    CHECK(exists("/a/renamed.txt") && !exists("/a/range.txt"));
    target2(url, sizeof url, "/fs/rename", "/a/renamed.txt", "/a/empty");
    CHECK(simple("POST", url) == 409);
    /* The volume itself cannot go. */
    target(url, sizeof url, "/fs/delete", "path", "");
    CHECK(simple("POST", url) == 400);
    /* A link is not followed out of a volume, not to list, not to write, not to delete. */
    snprintf(outside, sizeof outside, "%s.outside", g_root);
    CHECK(mkdir(outside, 0777) == 0);
    snprintf(link_path, sizeof link_path, "%s/keep", outside);
    fd = open(link_path, O_CREAT | O_WRONLY, 0666);
    close(fd);
    snprintf(link_path, sizeof link_path, "%s/a/out", g_root);
    CHECK(symlink(outside, link_path) == 0);
    target(url, sizeof url, "/fs/list", "path", "/a/out");
    CHECK(simple("GET", url) == 400);
    target(url, sizeof url, "/fs/put", "path", "/a/out/new");
    CHECK(simple("POST", url) == 400);
    target(url, sizeof url, "/fs/delete", "path", "/a/out/keep");
    CHECK(simple("POST", url) == 400);
    target(url, sizeof url, "/fs/mkdir", "path", "/d");
    CHECK(simple("POST", url) == 200);
    target(url, sizeof url, "/fs/mkdir", "path", "/d/sub");
    CHECK(simple("POST", url) == 200);
    snprintf(link_path, sizeof link_path, "%s/d/sub/out", g_root);
    CHECK(symlink(outside, link_path) == 0);
    target(url, sizeof url, "/fs/delete", "path", "/d");
    CHECK(simple("POST", url) == 200);
    CHECK(!exists("/d"));
    snprintf(link_path, sizeof link_path, "%s/keep", outside);
    CHECK(access(link_path, F_OK) == 0);
    unlink(link_path);
    rmdir(outside);
    snprintf(link_path, sizeof link_path, "%s/a/out", g_root);
    unlink(link_path);
    (void)body;
}

static void jobs(void)
{
    char url[8192];

    target(url, sizeof url, "/fs/mkdir", "path", "/b");
    CHECK(simple("POST", url) == 200);
    target2(url, sizeof url, "/fs/copy", "/a/deep", "/b/deep");
    CHECK(simple("POST", url) == 200);
    wait_job("\"state\":\"done\"");
    CHECK(exists("/b/deep") && exists("/a/deep"));
    target2(url, sizeof url, "/fs/copy", "/a/deep", "/b/deep");
    CHECK(simple("POST", url) == 409);
    target2(url, sizeof url, "/fs/copy", "/a", "/a/deep/inside");
    CHECK(simple("POST", url) == 409);
    target2(url, sizeof url, "/fs/move", "/a/renamed.txt", "/b/moved.txt");
    CHECK(simple("POST", url) == 200);
    CHECK(exists("/b/moved.txt") && !exists("/a/renamed.txt"));
    target2(url, sizeof url, "/fs/copy", "/missing", "/b/x");
    CHECK(simple("POST", url) == 404);
}

static int write_small(const char *path)
{
    char full[1024];
    int fd;

    snprintf(full, sizeof full, "%s%s", g_root, path);
    fd = open(full, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static void disk_speed(void)
{
    char url[4096], *body = NULL;

    target(url, sizeof url, "/fs/bench", "path", "");
    CHECK(request("POST", url, &body) == 200);
    CHECK(strstr(body, "\"write\":") && strstr(body, "\"read\":"));
    free(body);
    CHECK(!exists("/.tom-bench.tmp"));
    CHECK(g_lines > 0);
    /* What a run that died left. */
    CHECK(write_small("/.tom-bench.tmp"));
    files_sweep();
    CHECK(!exists("/.tom-bench.tmp"));
    /* Only a volume, not a folder on one. */
    target(url, sizeof url, "/fs/bench", "path", "/b");
    CHECK(simple("POST", url) == 400);
}

/* Long enough to be noted: the volume, never the file's name. */
static void noted_upload(void)
{
    size_t size = 17 * 1024 * 1024, length;
    uint8_t *data = calloc(1, size);
    char url[4096], *body = NULL;

    CHECK(data);
    files_test_sample_ms = 0;           /* every block but the last */
    target(url, sizeof url, "/fs/put", "path", "/b/secret-name.bin");
    CHECK(request_full("POST", url, data, size, size, NULL, &body, &length) == 200);
    free(body);
    free(data);
    files_test_sample_ms = 30000;
    CHECK(g_saw_upload && !g_saw_name);
    CHECK(g_saw_pace >= 1 && g_saw_pace <= 16);
}

/* What every page's poll sees. */
static void shared_transfers(void)
{
    char url[4096], *body = NULL;
    size_t length, i;

    target(url, sizeof url, "/fs/put", "path", "/shown.bin");
    strcat(url, "&client=abc123");
    CHECK(request_full("POST", url, "12345", 5, 5, NULL, &body, &length) == 200);
    free(body);
    body = NULL;
    CHECK(request("GET", "/fs/transfers", &body) == 200);
    CHECK(strstr(body, "\"kind\":\"upload\",\"client\":\"abc123\",\"name\":\"shown.bin\""));
    CHECK(strstr(body, "\"size\":5,\"done\":5,"));
    CHECK(strstr(body, "\"state\":\"done\""));
    free(body);
    body = NULL;
    /* A client id is only ever an id. */
    target(url, sizeof url, "/fs/put", "path", "/odd.bin");
    strcat(url, "&client=%22x");
    CHECK(request_full("POST", url, "1", 1, 1, NULL, &body, &length) == 200);
    free(body);
    body = NULL;
    CHECK(request("GET", "/fs/transfers", &body) == 200);
    CHECK(strstr(body, "\"client\":\"\",\"name\":\"odd.bin\""));
    free(body);
    /* Finished ones make room, however many there have been. */
    for (i = 0; i < 40; i++) {
        body = NULL;
        target(url, sizeof url, "/fs/put", "path", "/many.bin");
        strcat(url, "&overwrite=1");
        CHECK(request_full("POST", url, "1", 1, 1, NULL, &body, &length) == 200);
        free(body);
    }
    body = NULL;
    CHECK(request("GET", "/fs/transfers", &body) == 200);
    CHECK(strstr(body, "\"name\":\"many.bin\"") && !strstr(body, "shown.bin"));
    free(body);
    /* A file that arrives through several reads is written whole. */
    {
        size_t size = 3 * 1024 * 1024 + 123;
        uint8_t *data = malloc(size), *back;
        char full[1024];
        int fd;

        CHECK(data);
        for (i = 0; i < size; i++) data[i] = (uint8_t)(i * 31 + (i >> 16));
        body = NULL;
        target(url, sizeof url, "/fs/put", "path", "/blocks.bin");
        CHECK(request_full("POST", url, data, size, size, NULL, &body, &length) == 200);
        free(body);
        snprintf(full, sizeof full, "%s/blocks.bin", g_root);
        back = malloc(size);
        fd = open(full, O_RDONLY);
        CHECK(back && fd >= 0 && read(fd, back, size) == (ssize_t)size && !memcmp(back, data, size));
        close(fd);
        free(back);
        free(data);
    }
}

static void guards(void)
{
    char url[4096], *body = NULL, *big;
    size_t length;

    target(url, sizeof url, "/fs/volumes", "x", "");
    CHECK(simple("GET", "/fs/list?path=/etc") == 400);
    CHECK(simple("GET", "/fs/list?path=/data/%2E%2E/etc") == 400);
    CHECK(simple("GET", "/fs/list") == 400);
    target(url, sizeof url, "/fs/delete", "path", "/b");
    CHECK(request_full("POST", url, NULL, 0, 0, "Origin: http://evil.example\r\n", &body, &length) == 403);
    free(body);
    CHECK(exists("/b"));
    /* A name in Host is another site's rebound to this address. */
    {
        struct memory m = {0};
        struct http_stream stream = {&m, memory_read, memory_write};
        const char *wire = "GET /fs/volumes HTTP/1.1\r\nHost: evil.example\r\n\r\n";

        m.in = (const uint8_t *)wire;
        m.in_length = strlen(wire);
        http_serve(&g_backend, &stream);
        CHECK(!strncmp((char *)m.out, "HTTP/1.1 403", 12));
        free(m.out);
    }
    /* A path cut by the buffer would be a different path, so it is refused whole. */
    big = malloc(12000);
    memcpy(big, "/fs/list?path=/data/", 20);
    memset(big + 20, 'a', 9000);
    big[9020] = 0;
    CHECK(simple("GET", big) == 400);
    free(big);
    CHECK(simple("GET", "/fs/nothing") == 404);
}

static void remove_all(const char *path)
{
    struct stat st;
    struct dirent *e;
    DIR *d;

    CHECK(lstat(path, &st) == 0);
    if (!S_ISDIR(st.st_mode)) {
        CHECK(unlink(path) == 0);
        return;
    }
    d = opendir(path);
    CHECK(d);
    while ((e = readdir(d)) != NULL) {
        char child[1024];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        remove_all(child);
    }
    closedir(d);
    CHECK(rmdir(path) == 0);
}

static void note(const char *line)
{
    if (strstr(line, "secret-name")) g_saw_name = 1;
    if (strstr(line, "upload ") && strstr(line, "closing fsync")) g_saw_upload = 1;
    if (strstr(line, "upload ") && strstr(line, " GiB: ") && strstr(line, "disk ") && strstr(line, "slowest write ")) g_saw_pace++;
    g_lines++;
}

int main(void)
{
    char *made;

    snprintf(g_root, sizeof g_root, "%s/tom-usbnet-files-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    made = mkdtemp(g_root);
    CHECK(made);
    /* /tmp is a link on macOS, and the root itself is taken as given. */
    files_test_root = g_root;
    files_set_log(note);
    paths();
    folders();
    uploads();
    downloads();
    changes();
    jobs();
    disk_speed();
    noted_upload();
    shared_transfers();
    guards();
    files_stop();
    remove_all(g_root);
    printf("test-files: all OK\n");
    return 0;
}
