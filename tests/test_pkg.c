/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests for PKG name detection, loopback HTTP/1.1 Range streaming (pkg_stream),
 * and end-to-end NAS PKG streaming installation (pkg_install + pkg_appinst_stub). */
#include "files.h"
#include "pkg_install.h"
#include "pkg_stream.h"
#include "smb_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "test-pkg failed at line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

extern const char *smb_fake_root;
extern int smb_fake_sessions;

extern int pkg_stub_mode;
extern uint32_t pkg_stub_error;
extern int pkg_stub_delay_ms;
extern uint64_t pkg_stub_verified_bytes;
extern uint32_t pkg_stub_hash;

struct memory {
    const uint8_t *in;
    size_t in_length, in_at;
    char *out;
    size_t out_length;
};

static char g_root[256], g_nas[300], g_volume[300], g_last_log[192];

static void record_log(const char *line)
{
    snprintf(g_last_log, sizeof g_last_log, "%s", line);
}

static ssize_t memory_read(void *context, void *data, size_t length)
{
    struct memory *m = context;
    size_t left = m->in_length - m->in_at, n = length < left ? length : left;

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

static int noop_route(void *c, const struct http_request *r, struct http_stream *s)
{
    (void)c;
    return files_route(r, s);
}

static int noop_info(void *c, char *o, size_t n) { (void)c; (void)o; (void)n; return 0; }
static const struct http_backend g_backend = {.route = noop_route, .info = noop_info};

static int fs_request(const char *method, const char *target, const char *data, char **body)
{
    size_t length = data ? strlen(data) : 0, room = 4096 + length;
    char *wire = malloc(room), *split;
    int used = snprintf(wire, room, "%s %s HTTP/1.1\r\nHost: 192.0.2.5\r\n", method, target), status = 0;
    struct memory m = {0};
    struct http_stream stream = {&m, memory_read, memory_write};

    if (!strcmp(method, "POST")) used += snprintf(wire + used, room - (size_t)used, "Content-Length: %zu\r\n", length);
    used += snprintf(wire + used, room - (size_t)used, "\r\n");
    if (data) memcpy(wire + used, data, length);
    m.in = (uint8_t *)wire;
    m.in_length = (size_t)used + length;
    http_serve(&g_backend, &stream);
    free(wire);
    if (!m.out) return 0;
    sscanf(m.out, "HTTP/1.1 %d", &status);
    split = strstr(m.out, "\r\n\r\n");
    if (body) *body = strdup(split ? split + 4 : "");
    free(m.out);
    return status;
}

static void write_fake_pkg(const char *path, size_t size, int valid_magic)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    CHECK(f);
    for (i = 0; i < size; i++) {
        uint8_t b = (uint8_t)((i * 131 + 7) & 0xff);

        if (i == 0 && valid_magic) b = 0x7f;
        else if (i == 1 && valid_magic) b = 'C';
        else if (i == 2 && valid_magic) b = 'N';
        else if (i == 3 && valid_magic) b = 'T';
        fputc(b, f);
    }
    fclose(f);
}

static uint32_t fnv1a_file(const char *path, uint64_t start, size_t len)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[4096];
    uint32_t h = 2166136261u;
    size_t left = len;

    CHECK(f && fseeko(f, (off_t)start, SEEK_SET) == 0);
    while (left > 0) {
        size_t want = left < sizeof buf ? left : sizeof buf;
        size_t got = fread(buf, 1, want, f), i;

        CHECK(got == want);
        for (i = 0; i < got; i++) {
            h ^= buf[i];
            h *= 16777619u;
        }
        left -= got;
    }
    fclose(f);
    return h;
}

static int parse_url(const char *url, uint16_t *port, char *path, size_t cap)
{
    const char *p, *slash;

    CHECK(strncmp(url, "http://127.0.0.1:", 17) == 0);
    p = url + 17;
    slash = strchr(p, '/');
    CHECK(slash);
    *port = (uint16_t)atoi(p);
    snprintf(path, cap, "%s", slash);
    return 0;
}

static int connect_loopback(uint16_t port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    CHECK(fd >= 0);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    return fd;
}

static void send_str(int fd, const char *s)
{
    size_t len = strlen(s), off = 0;

    while (off < len) {
        ssize_t n = write(fd, s + off, len - off);

        CHECK(n > 0);
        off += (size_t)n;
    }
}

static void recv_http_response(int fd, int *status, char *hdr, size_t hdr_cap, uint8_t **body_out, size_t *body_len)
{
    size_t used = 0, clen = 0, got = 0;
    char *end = NULL;
    char *cl;

    while (used + 1 < hdr_cap) {
        ssize_t n = read(fd, hdr + used, hdr_cap - 1 - used);

        CHECK(n > 0);
        used += (size_t)n;
        hdr[used] = 0;
        end = strstr(hdr, "\r\n\r\n");
        if (end) break;
    }
    CHECK(end);
    CHECK(sscanf(hdr, "HTTP/1.%*d %d", status) == 1);
    cl = strstr(hdr, "Content-Length:");
    if (cl && cl < end) clen = (size_t)strtoull(cl + 15, NULL, 10);
    *body_len = clen;
    *body_out = malloc(clen + 1);
    CHECK(*body_out);
    {
        size_t hdr_bytes = (size_t)(end + 4 - hdr);
        size_t extra = used - hdr_bytes;

        if (extra > clen) extra = clen;
        if (extra) memcpy(*body_out, hdr + hdr_bytes, extra);
        got = extra;
        hdr[hdr_bytes] = 0;
    }
    while (got < clen) {
        ssize_t n = read(fd, *body_out + got, clen - got);

        CHECK(n > 0);
        got += (size_t)n;
    }
    (*body_out)[clen] = 0;
}

struct conc_arg {
    uint16_t port;
    const char *path;
    const char *file_path;
    uint64_t start;
    size_t len;
};

static void *conc_worker(void *opaque)
{
    struct conc_arg *a = opaque;
    char req[512], hdr[2048];
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0, fd = connect_loopback(a->port);
    uint32_t h = 2166136261u, expected;
    size_t i;

    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=%" PRIu64 "-%" PRIu64 "\r\nConnection: close\r\n\r\n",
        a->path, a->start, a->start + a->len - 1);
    send_str(fd, req);
    recv_http_response(fd, &status, hdr, sizeof hdr, &body, &body_len);
    close(fd);
    CHECK(status == 206 && body_len == a->len);
    for (i = 0; i < body_len; i++) {
        h ^= body[i];
        h *= 16777619u;
    }
    free(body);
    expected = fnv1a_file(a->file_path, a->start, a->len);
    CHECK(h == expected);
    return NULL;
}

static void test_names(void)
{
    CHECK(pkg_install_is_pkg_name("game.pkg"));
    CHECK(pkg_install_is_pkg_name("sub/Game.PKG"));
    CHECK(pkg_install_is_pkg_name("patch/update.FpKg"));
    CHECK(!pkg_install_is_pkg_name(".pkg"));
    CHECK(!pkg_install_is_pkg_name("sub/.fpkg"));
    CHECK(!pkg_install_is_pkg_name("game.pkg.bin"));
    CHECK(!pkg_install_is_pkg_name("game.bin"));
    CHECK(!pkg_install_is_pkg_name(""));
}

static void test_stream_server(void)
{
    char pkg_path[600], url[256], url_path[128], req[512], hdr[2048];
    struct smb_server srv = {.host = "nas.test", .user = "me", .password = "pw"};
    struct smb_session *s = NULL;
    struct smb_entry entry;
    void *file = NULL;
    struct pkg_stream *st = NULL;
    uint16_t port = 0;
    int fd, status = 0, i;
    uint8_t *body = NULL;
    size_t body_len = 0, file_size = 300000;
    pthread_t thr[4];
    struct conc_arg args[4];

    snprintf(pkg_path, sizeof pkg_path, "%s/media/stream.pkg", g_nas);
    write_fake_pkg(pkg_path, file_size, 1);

    CHECK(smb_connect_share(&srv, "media", &s, NULL) == 0);
    CHECK(smb_stat(s, "stream.pkg", &entry) == 0);
    CHECK(smb_open_file(s, "stream.pkg", &file) == 0);
    CHECK(pkg_stream_start(&srv, "media", "stream.pkg", s, file, entry.size, entry.mtime, &st) == 0);

    pkg_stream_url(st, url, sizeof url);
    parse_url(url, &port, url_path, sizeof url_path);

    /* Keep-alive connection: HEAD + 206 partial + 416 + 404 + 405 */
    fd = connect_loopback(port);
    snprintf(req, sizeof req, "HEAD %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", url_path);
    send_str(fd, req);
    /* Wait only for HEAD headers (body_len for HEAD would read Content-Length bytes if we didn't handle HEAD) */
    {
        size_t used = 0;

        while (used + 1 < sizeof hdr) {
            ssize_t n = read(fd, hdr + used, sizeof hdr - 1 - used);

            CHECK(n > 0);
            used += (size_t)n;
            hdr[used] = 0;
            if (strstr(hdr, "\r\n\r\n")) break;
        }
        CHECK(strstr(hdr, "HTTP/1.1 200 OK") && strstr(hdr, "Accept-Ranges: bytes") && strstr(hdr, "ETag:") && strstr(hdr, "Last-Modified:"));
        CHECK(strstr(hdr, "Content-Length: 300000"));
    }

    /* Sequential 206 partial ranges on the same keep-alive socket */
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=0-63\r\n\r\n", url_path);
    send_str(fd, req);
    recv_http_response(fd, &status, hdr, sizeof hdr, &body, &body_len);
    CHECK(status == 206 && body_len == 64 && body[0] == 0x7f && body[1] == 'C' && body[2] == 'N' && body[3] == 'T');
    CHECK(strstr(hdr, "Content-Range: bytes 0-63/300000"));
    free(body);

    /* Suffix range (-128) */
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=-128\r\n\r\n", url_path);
    send_str(fd, req);
    recv_http_response(fd, &status, hdr, sizeof hdr, &body, &body_len);
    CHECK(status == 206 && body_len == 128 && strstr(hdr, "Content-Range: bytes 299872-299999/300000"));
    free(body);

    /* Clamped range end beyond EOF */
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=299000-99999999\r\n\r\n", url_path);
    send_str(fd, req);
    recv_http_response(fd, &status, hdr, sizeof hdr, &body, &body_len);
    CHECK(status == 206 && body_len == 1000 && strstr(hdr, "Content-Range: bytes 299000-299999/300000"));
    free(body);

    /* Out-of-bounds range -> 416 */
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=99999999-\r\n\r\n", url_path);
    send_str(fd, req);
    recv_http_response(fd, &status, hdr, sizeof hdr, &body, &body_len);
    CHECK(status == 416 && strstr(hdr, "Content-Range: bytes */300000"));
    free(body);

    /* Wrong path -> 404 */
    send_str(fd, "GET /pkg/wrong/package.pkg HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    recv_http_response(fd, &status, hdr, sizeof hdr, &body, &body_len);
    CHECK(status == 404);
    free(body);

    /* Wrong method -> 405 */
    snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", url_path);
    send_str(fd, req);
    recv_http_response(fd, &status, hdr, sizeof hdr, &body, &body_len);
    CHECK(status == 405);
    free(body);
    close(fd);

    /* Concurrent Range reads across 4 threads */
    for (i = 0; i < 4; i++) {
        args[i].port = port;
        args[i].path = url_path;
        args[i].file_path = pkg_path;
        args[i].start = (uint64_t)i * 75000;
        args[i].len = 75000;
        CHECK(pthread_create(&thr[i], NULL, conc_worker, &args[i]) == 0);
    }
    for (i = 0; i < 4; i++) pthread_join(thr[i], NULL);
    CHECK(pkg_stream_served(st) >= 300000);

    pkg_stream_stop(st);
    CHECK(smb_fake_sessions == 0);
}

static void wait_job(const char *want_substr)
{
    int i;

    for (i = 0; i < 500; i++) {
        char *body = NULL;
        int match;

        CHECK(fs_request("GET", "/fs/job", NULL, &body) == 200);
        match = strstr(body, want_substr) != NULL;
        if (!match && !strstr(body, "\"state\":\"running\"")) {
            fprintf(stderr, "unexpected job state: %s (wanted %s)\n", body, want_substr);
            exit(1);
        }
        free(body);
        if (match) return;
        usleep(10000);
    }
    CHECK(!"job timed out");
}

static void run_install_direct(const char *rel_path, const char *want_substr, int cancel_after_start)
{
    struct smb_server srv = {.host = "nas.test", .user = "me", .password = "pw"};
    struct memory m = {0};
    struct http_stream stream = {&m, memory_read, memory_write};
    int status = 0;

    CHECK(pkg_install_smb(&srv, "media", rel_path, "test1", &stream) == 0);
    CHECK(m.out && sscanf(m.out, "HTTP/1.1 %d", &status) == 1 && status == 200);
    free(m.out);
    if (cancel_after_start) {
        char *body = NULL;

        usleep(60000);
        CHECK(fs_request("POST", "/fs/cancel", "", &body) == 200);
        free(body);
    }
    wait_job(want_substr);
}

static void test_install_orchestrator(void)
{
    char good_path[600], bad_path[600];
    uint32_t expected_hash;
    char *body = NULL;

    snprintf(good_path, sizeof good_path, "%s/media/good.fpkg", g_nas);
    write_fake_pkg(good_path, 200 * 1024, 1);
    expected_hash = fnv1a_file(good_path, 0, 200 * 1024);

    /* 1. Happy path end-to-end */
    pkg_stub_mode = 0;
    pkg_stub_delay_ms = 0;
    pkg_stub_verified_bytes = 0;
    pkg_stub_hash = 0;
    run_install_direct("good.fpkg", "\"state\":\"done\"", 0);
    CHECK(pkg_stub_verified_bytes == 200 * 1024);
    CHECK(pkg_stub_hash == expected_hash);
    CHECK(fs_request("GET", "/fs/transfers", NULL, &body) == 200);
    CHECK(strstr(body, "\"kind\":\"pkg\"") && strstr(body, "\"name\":\"good.fpkg\""));
    free(body);
    CHECK(smb_fake_sessions == 0);

    /* 2. Bad magic rejected before starting HTTP stream */
    snprintf(bad_path, sizeof bad_path, "%s/media/badmagic.pkg", g_nas);
    write_fake_pkg(bad_path, 4096, 0);
    run_install_direct("badmagic.pkg", "\"error\":\"bad_pkg\"", 0);
    CHECK(smb_fake_sessions == 0);

    /* 3. Installer refuses start with Sony error code -> exposed in detail */
    pkg_stub_mode = 1;
    pkg_stub_error = 0x80b22403u;
    run_install_direct("good.fpkg", "\"error\":\"install_refused\",\"detail\":\"0x80b22403\"", 0);

    /* 4. Installer fails mid-transfer with out-of-space code -> mapped to no_space + detail */
    pkg_stub_mode = 2;
    pkg_stub_error = 0x80990005u;
    g_last_log[0] = 0;
    run_install_direct("good.fpkg", "\"error\":\"no_space\",\"detail\":\"0x80990005\"", 0);
    CHECK(strstr(g_last_log, "0x80990005") && !strstr(g_last_log, "good.fpkg"));
    CHECK(smb_fake_sessions == 0);

    /* 5. Cancellation mid-install */
    pkg_stub_mode = 0;
    pkg_stub_delay_ms = 40;
    run_install_direct("good.fpkg", "\"state\":\"canceled\"", 1);
    pkg_stub_delay_ms = 0;
    CHECK(smb_fake_sessions == 0);
}

int main(void)
{
    char path[600], command[700];

    snprintf(g_root, sizeof g_root, "%s/tom-usbnet-pkg-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(g_root));
    snprintf(g_nas, sizeof g_nas, "%s/nas", g_root);
    snprintf(g_volume, sizeof g_volume, "%s/vol", g_root);
    CHECK(mkdir(g_nas, 0755) == 0 && mkdir(g_volume, 0755) == 0);
    snprintf(path, sizeof path, "%s/media", g_nas);
    CHECK(mkdir(path, 0755) == 0);

    smb_fake_root = g_nas;
    files_test_root = g_volume;
    pkg_install_set_log(record_log);

    test_names();
    test_stream_server();
    test_install_orchestrator();

    snprintf(command, sizeof command, "rm -rf '%s'", g_root);
    CHECK(system(command) == 0);
    printf("test-pkg: ok\n");
    return 0;
}
