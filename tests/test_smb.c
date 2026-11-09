/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The NAS routes through the real HTTP layer and the file manager's job, against a
 * share that is a local folder (smb_fake.c). */
#include "files.h"
#include "smb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed at line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

extern const char *smb_fake_root;
extern size_t smb_fake_read_size;
extern char smb_fake_password[];
extern int smb_fake_sessions;

struct memory {
    const uint8_t *in;
    size_t in_length, in_at;
    char *out;
    size_t out_length;
};

static char g_root[256], g_volume[300], g_nas[300], g_store[300];

static ssize_t memory_read(void *context, void *data, size_t length)
{
    struct memory *m = context;
    size_t left = m->in_length - m->in_at, n = length < left ? length : left;

    if (n > 13) n = 13;                 /* bodies arrive in pieces */
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

static int route(void *c, const struct http_request *r, struct http_stream *s)
{
    int result = smb_route(r, s);

    (void)c;
    return result != HTTP_NOT_MINE ? result : files_route(r, s);
}

static int noop_info(void *c, char *o, size_t n) { (void)c; (void)o; (void)n; return 0; }
static const struct http_backend g_backend = {.route = route, .info = noop_info};

static int request(const char *method, const char *target, const char *data, char **body)
{
    size_t length = data ? strlen(data) : 0, room = 16384 + length;
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

static int simple(const char *method, const char *target, const char *data, const char *expect)
{
    char *body = NULL;
    int status = request(method, target, data, &body);

    if (expect && !strstr(body, expect)) {
        fprintf(stderr, "%s %s: %d %s (wanted %s)\n", method, target, status, body, expect);
        exit(1);
    }
    free(body);
    return status;
}

static void encode(char *out, size_t capacity, const char *text)
{
    size_t at = 0;

    for (; *text && at + 4 < capacity; text++) {
        unsigned char c = (unsigned char)*text;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' || c == '_') out[at++] = (char)c;
        else at += (size_t)snprintf(out + at, capacity - at, "%%%02X", c);
    }
    out[at] = 0;
}

static void put(const char *path, size_t size, unsigned seed)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    CHECK(f);
    for (i = 0; i < size; i++) fputc((int)((i * 131 + seed) & 0xff), f);
    fclose(f);
}

static int same(const char *a, const char *b)
{
    char command[1024];

    snprintf(command, sizeof command, "cmp -s '%s' '%s'", a, b);
    return system(command) == 0;
}

static int exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static void wait_job(const char *state)
{
    int i;

    for (i = 0; i < 1000; i++) {
        char *body = NULL;
        int found;

        CHECK(request("GET", "/fs/job", NULL, &body) == 200);
        found = strstr(body, state) != NULL;
        if (!found && !strstr(body, "running")) {
            fprintf(stderr, "job: %s (wanted %s)\n", body, state);
            exit(1);
        }
        free(body);
        if (found) return;
        usleep(10000);
    }
    CHECK(!"the job did not end");
}

static void copy(const char *path, const char *to_name, int status, const char *state)
{
    char target[2048], to[600], encoded[1200];

    snprintf(to, sizeof to, "%s/%s", g_volume, to_name);
    encode(encoded, sizeof encoded, to);
    snprintf(target, sizeof target, "/smb/copy?id=1&share=media&path=%s&to=%s&client=abc1", path, encoded);
    CHECK(simple("POST", target, "", NULL) == status);
    if (state) wait_job(state);
}

static void saved(void)
{
    char *body = NULL, line[512];
    struct stat st;
    FILE *f;

    CHECK(simple("GET", "/smb/servers", NULL, "{\"servers\":[]}") == 200);
    CHECK(simple("POST", "/smb/save", "name=Home%20NAS&host=nas.test&user=me&password=p%26w%22rd", "{\"id\":1}") == 200);
    CHECK(simple("POST", "/smb/save", "host=nas.test&user=bad&password=x", "{\"id\":2}") == 200);
    CHECK(simple("POST", "/smb/save", "host=down.test", "{\"id\":3}") == 200);
    CHECK(request("GET", "/smb/servers", NULL, &body) == 200);
    CHECK(strstr(body, "\"name\":\"Home NAS\"") && strstr(body, "\"password\":true") && strstr(body, "\"port\":445"));
    CHECK(!strstr(body, "p&w"));        /* never sent back */
    CHECK(strstr(body, "\"name\":\"down.test\",\"host\":\"down.test\"") && strstr(body, "\"password\":false"));
    free(body);
    CHECK(stat(g_store, &st) == 0 && (st.st_mode & 0777) == 0600);
    f = fopen(g_store, "r");
    CHECK(f && fgets(line, sizeof line, f));
    fclose(f);
    CHECK(!strstr(line, "p&w") && !strchr(line, '\t'));
    /* Bad forms. */
    CHECK(simple("POST", "/smb/save", "host=nas%20test", "bad_request") == 400);
    CHECK(simple("POST", "/smb/save", "user=me", "bad_request") == 400);
    CHECK(simple("POST", "/smb/save", "host=nas.test&user=a%0Ab", "bad_request") == 400);
    CHECK(simple("POST", "/smb/save", "host=nas.test&port=70000", "bad_request") == 400);
    CHECK(simple("POST", "/smb/save", "id=99&host=nas.test", "not_found") == 404);
    /* Read back from the file; an edit without a password keeps it. */
    smb_set_store(g_store);
    CHECK(simple("POST", "/smb/save", "id=1&name=Home&host=nas.test&user=me", "{\"id\":1}") == 200);
    CHECK(simple("GET", "/smb/servers", NULL, "\"name\":\"Home\"") == 200);
    CHECK(simple("GET", "/smb/shares?id=1", NULL, "\"name\":\"media\"") == 200);
    CHECK(!strcmp(smb_fake_password, "p&w\"rd"));
}

static void browsing(void)
{
    char *body = NULL;

    CHECK(request("GET", "/smb/shares?id=1", NULL, &body) == 200);
    CHECK(!strstr(body, "hidden$") && strstr(body, "a \\\"quoted\\\" remark"));
    free(body);
    CHECK(simple("GET", "/smb/shares?id=2", NULL, "\"error\":\"auth\"") == 409);
    CHECK(simple("GET", "/smb/shares?id=3", NULL, "\"error\":\"unreachable\"") == 409);
    CHECK(simple("GET", "/smb/shares?id=9", NULL, "not_found") == 404);
    CHECK(request("GET", "/smb/list?id=1&share=media&path=sub", NULL, &body) == 200);
    CHECK(strstr(body, "\"name\":\"b.txt\",\"dir\":false,\"size\":5") && strstr(body, "\"name\":\"deep\",\"dir\":true"));
    free(body);
    CHECK(simple("GET", "/smb/list?id=1&share=media", NULL, "\"name\":\"a.bin\"") == 200);
    CHECK(simple("GET", "/smb/list?id=1&share=nothing", NULL, "no_share") == 409);
    CHECK(simple("GET", "/smb/list?id=1&share=media&path=missing", NULL, "not_found") == 404);
    CHECK(simple("GET", "/smb/list?id=1&share=media&path=../x", NULL, "bad_path") == 400);
    CHECK(simple("GET", "/smb/list?id=1&share=media&path=sub/..", NULL, "bad_path") == 400);
    CHECK(simple("GET", "/smb/list?id=1&share=media&path=/sub", NULL, "bad_path") == 400);
    CHECK(simple("GET", "/smb/list?id=1&share=media&path=sub%5Cx", NULL, "bad_path") == 400);
    CHECK(simple("GET", "/smb/list?id=1&share=me/dia", NULL, "bad_path") == 400);
    CHECK(simple("GET", "/smb/list?id=1", NULL, "bad_path") == 400);
    CHECK(smb_fake_sessions == 0);
}

static void copies(void)
{
    char a[600], b[600], target[2048];

    copy("sub", "sub", 200, "\"state\":\"done\"");
    snprintf(a, sizeof a, "%s/media/sub/deep/c.txt", g_nas);
    snprintf(b, sizeof b, "%s/sub/deep/c.txt", g_volume);
    CHECK(same(a, b));
    snprintf(b, sizeof b, "%s/sub/empty", g_volume);
    CHECK(exists(b));
    CHECK(simple("GET", "/fs/job", NULL, "\"kind\":\"smb\"") == 200);
    CHECK(simple("GET", "/fs/transfers", NULL, "\"name\":\"sub\"") == 200);
    copy("sub", "sub", 409, NULL);      /* it is there now */
    copy("a.bin", "a.bin", 200, "\"state\":\"done\"");
    snprintf(a, sizeof a, "%s/media/a.bin", g_nas);
    snprintf(b, sizeof b, "%s/a.bin", g_volume);
    CHECK(same(a, b));
    snprintf(b, sizeof b, "%s/a.bin.tom-part", g_volume);
    CHECK(!exists(b));
    copy("", "all", 200, "\"state\":\"done\"");          /* the share itself */
    snprintf(b, sizeof b, "%s/all/a.bin", g_volume);
    CHECK(same(a, b));
    /* A read that fails leaves nothing behind. */
    snprintf(a, sizeof a, "%s/media/bad", g_nas);
    CHECK(mkdir(a, 0755) == 0);
    snprintf(a, sizeof a, "%s/media/bad/broken.bin", g_nas);
    put(a, 200000, 4);
    copy("bad", "bad", 200, "\"error\":\"dropped\"");
    snprintf(b, sizeof b, "%s/bad", g_volume);
    CHECK(!exists(b));
    /* A cancel stops it, and the half copy goes. */
    snprintf(a, sizeof a, "%s/media/slow.bin", g_nas);
    put(a, 64 * 1024 * 200, 5);
    copy("slow.bin", "slow.bin", 200, NULL);
    CHECK(simple("POST", "/fs/cancel", "", NULL) == 200);
    wait_job("\"state\":\"canceled\"");
    snprintf(b, sizeof b, "%s/slow.bin", g_volume);
    CHECK(!exists(b));
    snprintf(b, sizeof b, "%s/slow.bin.tom-part", g_volume);
    CHECK(!exists(b));
    /* Where it may not go. */
    CHECK(simple("POST", "/smb/copy?id=1&share=media&path=a.bin&to=/tmp/x", "", "bad_path") == 400);
    encode(b, sizeof b, g_volume);
    snprintf(target, sizeof target, "/smb/copy?id=1&share=media&path=a.bin&to=%s", b);
    CHECK(simple("POST", target, "", "bad_path") == 400);   /* the volume itself */
    snprintf(a, sizeof a, "%s/no/such/x", g_volume);
    encode(b, sizeof b, a);
    snprintf(target, sizeof target, "/smb/copy?id=1&share=media&path=a.bin&to=%s", b);
    CHECK(simple("POST", target, "", "not_found") == 404);
    CHECK(simple("POST", "/smb/copy?id=1&share=media&path=../a.bin&to=/data/x", "", "bad_path") == 400);
    copy("missing", "missing", 200, "\"error\":\"not_found\"");
    CHECK(smb_fake_sessions == 0);
}

static void forgetting(void)
{
    CHECK(simple("POST", "/smb/forget?id=2", "", NULL) == 200);
    CHECK(simple("POST", "/smb/forget?id=2", "", "not_found") == 404);
    smb_set_store(g_store);
    CHECK(simple("GET", "/smb/servers", NULL, "\"id\":3") == 200);
    CHECK(simple("GET", "/smb/shares?id=2", NULL, "not_found") == 404);
}

int main(void)
{
    char path[600], command[700];

    snprintf(g_root, sizeof g_root, "%s/tom-usbnet-smb-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(g_root));
    snprintf(g_volume, sizeof g_volume, "%s/vol", g_root);
    snprintf(g_nas, sizeof g_nas, "%s/nas", g_root);
    snprintf(g_store, sizeof g_store, "%s/nas.txt", g_root);
    CHECK(mkdir(g_volume, 0755) == 0 && mkdir(g_nas, 0755) == 0);
    snprintf(path, sizeof path, "%s/media", g_nas);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/hidden$", g_nas);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/media/sub", g_nas);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/media/sub/deep", g_nas);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/media/sub/empty", g_nas);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/media/sub/b.txt", g_nas);
    put(path, 5, 1);
    snprintf(path, sizeof path, "%s/media/sub/deep/c.txt", g_nas);
    put(path, 70000, 2);
    snprintf(path, sizeof path, "%s/media/a.bin", g_nas);
    put(path, 3 * 1024 * 1024 + 17, 3);
    smb_fake_root = g_nas;
    files_test_root = g_volume;
    smb_set_store(g_store);
    saved();
    browsing();
    copies();
    forgetting();
    snprintf(command, sizeof command, "rm -rf '%s'", g_root);
    CHECK(system(command) == 0);
    printf("test-smb: ok\n");
    return 0;
}
