/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See smb.h. As with the rest of the page there is no login: whoever reaches the port
 * can use the saved servers, but cannot read their passwords back. A copy writes
 * nowhere files.c would refuse, through its job slot, and each file goes to
 * <name>.tom-part first, as an upload does. While one buffer is written to disk the
 * next is read from the share. */
#include "smb.h"
#include "files.h"
#include "smb_client.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SERVERS 16
#define NAME_BYTES 64
#define SHARE_BYTES 81
#define PATH_BYTES 1024
#define BODY_BYTES 2048
#define MAX_DEPTH 64
#define MAX_LISTED 5000
#define PART_SUFFIX ".tom-part"
#define SYNC_FROM (16ULL * 1024 * 1024)
#define CLIENT_BYTES 17

struct saved {
    unsigned id;
    char name[NAME_BYTES];
    struct smb_server server;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct saved g_saved[MAX_SERVERS];
static unsigned g_next_id = 1;
static int g_loaded;
static char g_store[PATH_BYTES];

static void wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;

    while (n--) *q++ = 0;
}

void smb_set_store(const char *path)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_store, sizeof g_store, "%s", path);
    g_loaded = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ---- text ------------------------------------------------------------------ */

struct text {
    char *data;
    size_t used, capacity;
    int failed;
};

static void add(struct text *t, const char *data, size_t length)
{
    if (t->failed) return;
    if (t->used + length + 1 > t->capacity) {
        size_t capacity = t->capacity ? t->capacity : 1024;
        char *grown;

        while (t->used + length + 1 > capacity) capacity *= 2;
        grown = realloc(t->data, capacity);
        if (!grown) {
            t->failed = 1;
            return;
        }
        t->data = grown;
        t->capacity = capacity;
    }
    memcpy(t->data + t->used, data, length);
    t->used += length;
    t->data[t->used] = 0;
}

static void add_text(struct text *t, const char *data)
{
    add(t, data, strlen(data));
}

static void add_string(struct text *t, const char *value)
{
    const unsigned char *p;

    add(t, "\"", 1);
    for (p = (const unsigned char *)value; *p; p++) {
        char escaped[8];

        if (*p == '"' || *p == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*p;
            add(t, escaped, 2);
        } else if (*p < 0x20) {
            snprintf(escaped, sizeof escaped, "\\u%04x", *p);
            add_text(t, escaped);
        } else {
            add(t, (const char *)p, 1);
        }
    }
    add(t, "\"", 1);
}

static int send_text(struct http_stream *stream, struct text *t)
{
    int result = t->failed ? http_respond(stream, 503, "application/json", "{\"error\":\"no_memory\"}") : http_respond(stream, 200, "application/json", t->data);

    free(t->data);
    return result;
}

static int error(struct http_stream *stream, int status, const char *code)
{
    char json[64];

    snprintf(json, sizeof json, "{\"error\":\"%s\"}", code);
    return http_respond(stream, status, "application/json", json);
}

/* ---- what is accepted ------------------------------------------------------ */

static int plain(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) return 0;
    return 1;
}

static int good_host(const char *host)
{
    const char *p;

    if (!host[0]) return 0;
    for (p = host; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) return 0;
    return 1;
}

static int good_share(const char *share)
{
    return share[0] && plain(share) && !strpbrk(share, "/\\:");
}

/* Inside the share: "" or parts joined by '/', none empty, "." or "..". */
static int good_path(const char *path)
{
    const char *p = path;

    if (!path[0]) return 1;
    if (!plain(path) || strchr(path, '\\')) return 0;
    for (;;) {
        const char *next = strchr(p, '/');
        size_t part = next ? (size_t)(next - p) : strlen(p);

        if (!part || (part == 1 && p[0] == '.') || (part == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!next) return 1;
        p = next + 1;
    }
}

/* ---- the saved servers ----------------------------------------------------- */
/* One line for each: "1 <id> <name> <host> <port> <user> <domain> <password>", the
 * texts in hex, so nothing in them can break a line. The file is the owner's only. */

static void put_hex(FILE *f, const char *s)
{
    if (!*s) fputc('-', f);
    for (; *s; s++) fprintf(f, "%02x", (unsigned char)*s);
}

static int get_hex(const char *hex, char *out, size_t capacity)
{
    size_t n = strlen(hex), i;

    if (!strcmp(hex, "-")) {
        out[0] = 0;
        return 0;
    }
    if (n % 2 || n / 2 >= capacity) return -1;
    for (i = 0; i < n / 2; i++) {
        unsigned value;

        if (sscanf(hex + 2 * i, "%2x", &value) != 1) return -1;
        out[i] = (char)value;
    }
    out[n / 2] = 0;
    return plain(out) ? 0 : -1;
}

static void load(void)
{
    FILE *f;
    char *line;
    int n = 0;

    if (g_loaded) return;
    g_loaded = 1;
    wipe(g_saved, sizeof g_saved);
    if (!g_store[0] || !(f = fopen(g_store, "r"))) return;
    line = malloc(2048);
    while (line && n < MAX_SERVERS && fgets(line, 2048, f)) {
        char *field[8], *save = NULL, *p = line;
        struct saved *s = &g_saved[n];
        int count = 0;
        unsigned long port;

        line[strcspn(line, "\r\n")] = 0;
        while (count < 8 && (field[count] = strtok_r(p, " ", &save)) != NULL) {
            p = NULL;
            count++;
        }
        if (count != 8 || strcmp(field[0], "1")) continue;
        s->id = (unsigned)strtoul(field[1], NULL, 10);
        port = strtoul(field[4], NULL, 10);
        if (!s->id || port > 65535 || get_hex(field[2], s->name, sizeof s->name) || get_hex(field[3], s->server.host, sizeof s->server.host) ||
            get_hex(field[5], s->server.user, sizeof s->server.user) || get_hex(field[6], s->server.domain, sizeof s->server.domain) ||
            get_hex(field[7], s->server.password, sizeof s->server.password) || !good_host(s->server.host)) {
            wipe(s, sizeof *s);
            continue;
        }
        s->server.port = (unsigned)port;
        if (s->id >= g_next_id) g_next_id = s->id + 1;
        n++;
    }
    if (line) {
        wipe(line, 2048);
        free(line);
    }
    fclose(f);
}

static int store(void)
{
    char temporary[PATH_BYTES + 8];
    FILE *f;
    int fd, i, result = 0;

    if (!g_store[0]) return 0;
    snprintf(temporary, sizeof temporary, "%s.new", g_store);
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    for (i = 0; i < MAX_SERVERS; i++) {
        const struct saved *s = &g_saved[i];

        if (!s->id) continue;
        fprintf(f, "1 %u ", s->id);
        put_hex(f, s->name);
        fputc(' ', f);
        put_hex(f, s->server.host);
        fprintf(f, " %u ", s->server.port);
        put_hex(f, s->server.user);
        fputc(' ', f);
        put_hex(f, s->server.domain);
        fputc(' ', f);
        put_hex(f, s->server.password);
        fputc('\n', f);
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) result = -1;
    if (fclose(f) != 0) result = -1;
    if (!result && rename(temporary, g_store) != 0) result = -1;
    if (result) unlink(temporary);
    return result;
}

static struct saved *find(unsigned id)
{
    int i;

    for (i = 0; id && i < MAX_SERVERS; i++)
        if (g_saved[i].id == id) return &g_saved[i];
    return NULL;
}

/* A copy of the saved server, so no lock is held across the network. */
static int server_of(const struct http_request *r, struct smb_server *out)
{
    struct saved *s;

    pthread_mutex_lock(&g_lock);
    load();
    s = find((unsigned)http_query_number(r->query, "id"));
    if (s) *out = s->server;
    pthread_mutex_unlock(&g_lock);
    return s ? 0 : -1;
}

static int servers(struct http_stream *stream)
{
    struct text t = {NULL, 0, 0, 0};
    int i, first = 1;

    add_text(&t, "{\"servers\":[");
    pthread_mutex_lock(&g_lock);
    load();
    for (i = 0; i < MAX_SERVERS; i++) {
        const struct saved *s = &g_saved[i];
        char number[64];

        if (!s->id) continue;
        snprintf(number, sizeof number, "%s{\"id\":%u,\"name\":", first ? "" : ",", s->id);
        add_text(&t, number);
        add_string(&t, s->name);
        add_text(&t, ",\"host\":");
        add_string(&t, s->server.host);
        snprintf(number, sizeof number, ",\"port\":%u,\"user\":", s->server.port ? s->server.port : 445);
        add_text(&t, number);
        add_string(&t, s->server.user);
        add_text(&t, ",\"domain\":");
        add_string(&t, s->server.domain);
        add_text(&t, s->server.password[0] ? ",\"password\":true}" : ",\"password\":false}");
        first = 0;
    }
    pthread_mutex_unlock(&g_lock);
    add_text(&t, "]}");
    return send_text(stream, &t);
}

/* The form, sent as the body so the password is in no URL. */
static int read_body(const struct http_request *r, struct http_stream *stream, char *out)
{
    size_t have = r->body_length;

    if (!r->has_length || r->length >= BODY_BYTES || have > r->length) return -1;
    memcpy(out, r->body, have);
    while (have < r->length) {
        ssize_t n = stream->read(stream->context, out + have, (size_t)r->length - have);

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    out[have] = 0;
    return 0;
}

/* 0, -1 if it is absent (or too long or holds a NUL), -2 if it holds a control byte. */
static int field(const char *body, const char *key, char *out, size_t capacity)
{
    if (http_query_text(body, key, out, capacity) != 0) {
        out[0] = 0;
        return -1;
    }
    return plain(out) ? 0 : -2;
}

static int save(const struct http_request *r, struct http_stream *stream)
{
    char *body = malloc(BODY_BYTES), port[8] = "", id[16] = "";
    struct saved entry, *slot = NULL;
    int has_password, i, result;
    char json[48];

    if (!body) return error(stream, 503, "no_memory");
    memset(&entry, 0, sizeof entry);
    if (read_body(r, stream, body) != 0) {
        free(body);
        return error(stream, 400, "bad_request");
    }
    /* An absent field is empty; a password left out of an edit keeps the saved one. */
    has_password = field(body, "password", entry.server.password, sizeof entry.server.password) != -1;
    if (field(body, "host", entry.server.host, sizeof entry.server.host) != 0 || !good_host(entry.server.host) ||
        field(body, "name", entry.name, sizeof entry.name) == -2 || field(body, "user", entry.server.user, sizeof entry.server.user) == -2 ||
        field(body, "domain", entry.server.domain, sizeof entry.server.domain) == -2 || !plain(entry.server.password)) {
        wipe(body, BODY_BYTES);
        free(body);
        wipe(&entry, sizeof entry);
        return error(stream, 400, "bad_request");
    }
    if (http_query_text(body, "port", port, sizeof port) == 0 && port[0]) {
        unsigned long value = strtoul(port, NULL, 10);

        if (!value || value > 65535) entry.server.host[0] = 0;
        entry.server.port = value == 445 ? 0 : (unsigned)value;
    }
    (void)http_query_text(body, "id", id, sizeof id);
    wipe(body, BODY_BYTES);
    free(body);
    if (!entry.server.host[0]) return error(stream, 400, "bad_request");
    if (!entry.name[0]) snprintf(entry.name, sizeof entry.name, "%s", entry.server.host);
    pthread_mutex_lock(&g_lock);
    load();
    if (id[0]) {
        slot = find((unsigned)strtoul(id, NULL, 10));
        if (!slot) {
            pthread_mutex_unlock(&g_lock);
            wipe(&entry, sizeof entry);
            return error(stream, 404, "not_found");
        }
        entry.id = slot->id;
        if (!has_password) memcpy(entry.server.password, slot->server.password, sizeof entry.server.password);
    } else {
        for (i = 0; i < MAX_SERVERS && !slot; i++)
            if (!g_saved[i].id) slot = &g_saved[i];
        if (!slot) {
            pthread_mutex_unlock(&g_lock);
            wipe(&entry, sizeof entry);
            return error(stream, 409, "too_many");
        }
        entry.id = g_next_id++;
    }
    *slot = entry;
    wipe(&entry, sizeof entry);
    result = store();
    snprintf(json, sizeof json, "{\"id\":%u}", slot->id);
    pthread_mutex_unlock(&g_lock);
    return result ? error(stream, 409, "not_saved") : http_respond(stream, 200, "application/json", json);
}

static int forget(const struct http_request *r, struct http_stream *stream)
{
    struct saved *s;
    int result;

    pthread_mutex_lock(&g_lock);
    load();
    s = find((unsigned)http_query_number(r->query, "id"));
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        return error(stream, 404, "not_found");
    }
    wipe(s, sizeof *s);
    result = store();
    pthread_mutex_unlock(&g_lock);
    return result ? error(stream, 409, "not_saved") : http_respond(stream, 200, "application/json", "{}");
}

/* ---- browsing -------------------------------------------------------------- */

struct listing {
    struct text t;
    int count;
};

static int one_share(void *context, const char *name, const char *remark)
{
    struct listing *l = context;

    if (l->count) add_text(&l->t, ",");
    add_text(&l->t, "{\"name\":");
    add_string(&l->t, name);
    add_text(&l->t, ",\"remark\":");
    add_string(&l->t, plain(remark) ? remark : "");
    add_text(&l->t, "}");
    return ++l->count >= MAX_LISTED;
}

static int shares(const struct http_request *r, struct http_stream *stream)
{
    struct listing l = {{NULL, 0, 0, 0}, 0};
    struct smb_server server;
    int result;

    if (server_of(r, &server) != 0) return error(stream, 404, "not_found");
    add_text(&l.t, "{\"shares\":[");
    result = smb_list_shares(&server, one_share, &l);
    wipe(&server, sizeof server);
    if (result) {
        free(l.t.data);
        return error(stream, 409, smb_error_code(result));
    }
    add_text(&l.t, "]}");
    return send_text(stream, &l.t);
}

static int query_share(const struct http_request *r, char *share, char *path)
{
    if (http_query_text(r->query, "share", share, SHARE_BYTES) != 0 || !good_share(share)) return -1;
    if (http_query_text(r->query, "path", path, PATH_BYTES) != 0) path[0] = 0;
    return good_path(path) ? 0 : -1;
}

static int list(const struct http_request *r, struct http_stream *stream)
{
    char share[SHARE_BYTES], path[PATH_BYTES];
    struct text t = {NULL, 0, 0, 0};
    struct smb_server server;
    struct smb_session *s;
    struct smb_entry e;
    const char *over = "";
    void *dir;
    int result, count = 0;

    if (query_share(r, share, path) != 0) return error(stream, 400, "bad_path");
    if (server_of(r, &server) != 0) return error(stream, 404, "not_found");
    result = smb_connect_share(&server, share, &s, &over);
    wipe(&server, sizeof server);
    if (result) return error(stream, 409, smb_error_code(result));
    result = smb_opendir(s, path, &dir);
    if (result) {
        smb_disconnect(s);
        return error(stream, result == SMB_NOT_FOUND ? 404 : 409, smb_error_code(result));
    }
    add_text(&t, "{\"over\":");
    add_string(&t, over);
    add_text(&t, ",\"entries\":[");
    while (count < MAX_LISTED && smb_readdir(s, dir, &e) == 1) {
        char numbers[96];

        add_text(&t, count++ ? ",{\"name\":" : "{\"name\":");
        add_string(&t, e.name);
        snprintf(numbers, sizeof numbers, ",\"dir\":%s,\"size\":%" PRIu64 ",\"mtime\":%" PRId64 "}", e.folder ? "true" : "false", e.size, e.mtime);
        add_text(&t, numbers);
    }
    add_text(&t, count >= MAX_LISTED ? "],\"truncated\":true}" : "]}");
    smb_closedir(s, dir);
    smb_disconnect(s);
    return send_text(stream, &t);
}

/* ---- copying --------------------------------------------------------------- */

/* Writes one buffer while the next is read. */
struct writer {
    pthread_mutex_t lock;
    pthread_cond_t change;
    int fd, busy, quit, error;
    const uint8_t *data;
    size_t length;
};

struct job {
    struct smb_server server;
    char share[SHARE_BYTES], path[PATH_BYTES], to[PATH_BYTES];
    struct smb_session *session;
    struct writer writer;
    uint8_t *buffer[2];
    size_t chunk;
    int error;                          /* an SMB_* failure, or 0 */
    int local;                          /* an errno from the console's side, or 0 */
};

static void *write_loop(void *argument)
{
    struct writer *w = argument;

    pthread_mutex_lock(&w->lock);
    for (;;) {
        const uint8_t *p;
        size_t left;
        int code = 0;

        while (!w->busy && !w->quit) pthread_cond_wait(&w->change, &w->lock);
        if (w->quit && !w->busy) break;
        p = w->data;
        left = w->length;
        pthread_mutex_unlock(&w->lock);
        while (left) {
            ssize_t n = write(w->fd, p, left);

            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                code = n < 0 ? errno : EIO;
                break;
            }
            p += n;
            left -= (size_t)n;
        }
        pthread_mutex_lock(&w->lock);
        if (code && !w->error) w->error = code;
        w->busy = 0;
        pthread_cond_broadcast(&w->change);
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

/* Waits for the buffer in flight; the first write failure, or 0. */
static int writer_idle(struct writer *w)
{
    int code;

    pthread_mutex_lock(&w->lock);
    while (w->busy) pthread_cond_wait(&w->change, &w->lock);
    code = w->error;
    pthread_mutex_unlock(&w->lock);
    return code;
}

static int writer_give(struct writer *w, int fd, const uint8_t *data, size_t length)
{
    int code = writer_idle(w);

    if (code) return code;
    pthread_mutex_lock(&w->lock);
    w->fd = fd;
    w->data = data;
    w->length = length;
    w->busy = 1;
    pthread_cond_broadcast(&w->change);
    pthread_mutex_unlock(&w->lock);
    return 0;
}

static int join(char *out, const char *base, const char *name)
{
    int n = base[0] ? snprintf(out, PATH_BYTES, "%s/%s", base, name) : snprintf(out, PATH_BYTES, "%s", name);

    return n < 0 || n >= PATH_BYTES ? -1 : 0;
}

static int stopped(struct job *j)
{
    if (files_job_canceled()) j->local = ECANCELED;
    return j->error || j->local;
}

static void measure(struct job *j, const char *path, const struct smb_entry *e, int depth, uint64_t *bytes)
{
    struct smb_entry child;
    void *dir;
    char *inner;

    if (stopped(j)) return;
    if (!e->folder) {
        *bytes += e->size;
        return;
    }
    if (depth >= MAX_DEPTH) {
        j->local = ENAMETOOLONG;
        return;
    }
    if ((j->error = smb_opendir(j->session, path, &dir)) != 0) return;
    inner = malloc(PATH_BYTES);
    if (!inner) j->local = ENOMEM;
    while (inner && !stopped(j) && smb_readdir(j->session, dir, &child) == 1) {
        if (join(inner, path, child.name) != 0) {
            j->local = ENAMETOOLONG;
            break;
        }
        measure(j, inner, &child, depth + 1, bytes);
    }
    free(inner);
    smb_closedir(j->session, dir);
}

static void copy_file(struct job *j, const char *path, const char *to, const struct smb_entry *e)
{
    char part[PATH_BYTES + sizeof PART_SUFFIX];
    uint64_t offset = 0;
    void *file;
    int fd, which = 0;

    if ((j->error = smb_open_file(j->session, path, &file)) != 0) return;
    snprintf(part, sizeof part, "%s" PART_SUFFIX, to);
    fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        j->local = errno;
        smb_close_file(j->session, file);
        return;
    }
    while (!stopped(j)) {
        long long n = smb_read(j->session, file, j->buffer[which], j->chunk, offset);

        if (n < 0) {
            j->error = (int)n;
            break;
        }
        if (n == 0) break;
        if ((j->local = writer_give(&j->writer, fd, j->buffer[which], (size_t)n)) != 0) break;
        offset += (uint64_t)n;
        files_job_add((uint64_t)n);
        which ^= 1;
    }
    {
        int code = writer_idle(&j->writer);

        if (code && !j->local) j->local = code;
    }
    smb_close_file(j->session, file);
    if (!j->error && !j->local && offset != e->size) j->error = SMB_FAILED;    /* the file changed under the copy */
    if (!j->error && !j->local && offset >= SYNC_FROM && fsync(fd) != 0) j->local = errno;
    if (close(fd) != 0 && !j->error && !j->local) j->local = errno;
    if (!j->error && !j->local && rename(part, to) != 0) j->local = errno;
    if (j->error || j->local) unlink(part);
}

static void copy_tree(struct job *j, const char *path, const char *to, const struct smb_entry *e, int depth)
{
    struct smb_entry child;
    char *inner, *outer;
    void *dir;

    if (stopped(j)) return;
    if (!e->folder) {
        copy_file(j, path, to, e);
        return;
    }
    if (depth >= MAX_DEPTH) {
        j->local = ENAMETOOLONG;
        return;
    }
    if (mkdir(to, 0777) != 0 && errno != EEXIST) {
        j->local = errno;
        return;
    }
    if ((j->error = smb_opendir(j->session, path, &dir)) != 0) return;
    inner = malloc(PATH_BYTES);
    outer = malloc(PATH_BYTES);
    if (!inner || !outer) j->local = ENOMEM;
    while (inner && outer && !stopped(j) && smb_readdir(j->session, dir, &child) == 1) {
        if (join(inner, path, child.name) != 0 || join(outer, to, child.name) != 0 || !files_path_allowed(outer)) {
            j->local = ENAMETOOLONG;
            break;
        }
        copy_tree(j, inner, outer, &child, depth + 1);
    }
    free(inner);
    free(outer);
    smb_closedir(j->session, dir);
}

static const char *local_code(int code)
{
    return code == ECANCELED ? "canceled" : code == ENOSPC ? "no_space" : code == EACCES || code == EPERM ? "denied" : code == EROFS ? "read_only" :
        code == ENAMETOOLONG ? "name_too_long" : code == ENOMEM ? "no_memory" : code == EEXIST ? "exists" : "failed";
}

static void *copy_thread(void *argument)
{
    struct job *j = argument;
    struct smb_entry root;
    pthread_t writer;
    uint64_t bytes = 0;
    const char *code = NULL;
    int writing = 0;

    pthread_mutex_init(&j->writer.lock, NULL);
    pthread_cond_init(&j->writer.change, NULL);
    j->error = smb_connect_share(&j->server, j->share, &j->session, NULL);
    wipe(&j->server, sizeof j->server);
    memset(&root, 0, sizeof root);
    root.folder = 1;                    /* the share itself */
    if (!j->error && j->path[0]) j->error = smb_stat(j->session, j->path, &root);
    if (!j->error) measure(j, j->path, &root, 0, &bytes);
    if (!j->error && !j->local && files_job_size(bytes) != 0) code = "no_space";
    if (!code && !j->error && !j->local) {
        j->chunk = smb_read_size(j->session);
        j->buffer[0] = malloc(j->chunk);
        j->buffer[1] = malloc(j->chunk);
        if (!j->buffer[0] || !j->buffer[1]) j->local = ENOMEM;
        else if (pthread_create(&writer, NULL, write_loop, &j->writer) != 0) j->local = ENOMEM;
        else writing = 1;
    }
    if (writing) {
        copy_tree(j, j->path, j->to, &root, 0);
        pthread_mutex_lock(&j->writer.lock);
        j->writer.quit = 1;
        pthread_cond_broadcast(&j->writer.change);
        pthread_mutex_unlock(&j->writer.lock);
        pthread_join(writer, NULL);
    }
    if (j->session) smb_disconnect(j->session);
    if (!code && j->local) code = local_code(j->local);
    if (!code && j->error) code = smb_error_code(j->error);
    files_job_end(code);
    free(j->buffer[0]);
    free(j->buffer[1]);
    pthread_mutex_destroy(&j->writer.lock);
    pthread_cond_destroy(&j->writer.change);
    free(j);
    return NULL;
}

static int copy(const struct http_request *r, struct http_stream *stream)
{
    struct job *j = calloc(1, sizeof *j);
    char client[CLIENT_BYTES] = "";
    size_t i;
    int started, result;

    if (!j) return error(stream, 503, "no_memory");
    if (query_share(r, j->share, j->path) != 0 || http_query_text(r->query, "to", j->to, sizeof j->to) != 0) {
        free(j);
        return error(stream, 400, "bad_path");
    }
    if (server_of(r, &j->server) != 0) {
        free(j);
        return error(stream, 404, "not_found");
    }
    if (http_query_text(r->query, "client", client, sizeof client) != 0) client[0] = 0;
    for (i = 0; client[i]; i++)
        if (!((client[i] >= '0' && client[i] <= '9') || (client[i] >= 'a' && client[i] <= 'z'))) client[0] = 0;
    result = files_job_run(stream, "smb", j->to, j->to, client, copy_thread, j, &started);
    if (!started) {
        wipe(j, sizeof *j);
        free(j);
    }
    return result;
}

/* ---- routes ---------------------------------------------------------------- */

int smb_route(const struct http_request *r, struct http_stream *stream)
{
    const char *path = r->path;
    int post = !strcmp(r->method, "POST");

    if (strncmp(path, "/smb/", 5)) return HTTP_NOT_MINE;
    path += 5;
    if (!post) {
        if (!strcmp(path, "servers")) return servers(stream);
        if (!strcmp(path, "shares")) return shares(r, stream);
        if (!strcmp(path, "list")) return list(r, stream);
    } else {
        if (!strcmp(path, "save")) return save(r, stream);
        if (!strcmp(path, "forget")) return forget(r, stream);
        if (!strcmp(path, "copy")) return copy(r, stream);
    }
    return HTTP_NOT_MINE;
}
