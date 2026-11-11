/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See forward.h. One thread accepts on every listener. Each connection has two more,
 * one per direction, so neither waits on the other; their reads and writes wake every
 * TICK_MS to see whether the connection or the run has ended. The far end on the
 * console is a kernel socket to 127.0.0.1, so a service there sees every client as
 * the console itself. Nothing here logs an address. */
#include "forward.h"
#include "forward_lwip.h"
#include "log.h"
#include "tile.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NAME_BYTES 48
#define PATH_BYTES 1024
#define BODY_BYTES 8192
#define JSON_BYTES 16384
#define PAGE_PORT 80                    /* the page itself, on lwIP */
#define TICK_MS 1000
#define WAIT_MS 200
#define RETRY_MS 5000
#define BACKLOG 8
#define DATA_MAX 16
#define DATA_WAIT_MS 30000
#define LISTENERS (FORWARD_MAX + DATA_MAX)
#define LINKS_MAX 64
#define BUFFER_BYTES (128 * 1024)
#define LINE_BYTES 512
#define LINK_STACK (256 * 1024)
#define STOP_WAIT_TICKS 400
#define STOP_TICK_US 10000
/* After the service has closed its side, how long a client may stay silent. */
#define HALF_OPEN_TICKS 60

struct rule {
    uint16_t port, to;
    int on;
    char name[NAME_BYTES];
    int listening, failed;              /* while running */
    unsigned links;
    uint64_t bytes;
};

struct listener {
    int fd;                             /* -1 for a free slot */
    uint16_t port;                      /* lwIP's */
    /* A data port of an FTP passive reply: where it goes, from whom, until when. */
    int data;
    uint16_t to, owner;
    uint32_t target, peer;
    long until;
};

struct link {
    int outer, inner;                   /* lwIP's, the kernel's */
    uint16_t owner;                     /* the rule's port, for its counts */
    int data;                           /* an FTP data connection */
    int ftp;                            /* -1 until the service's first bytes say */
    uint32_t peer, target;
    uint16_t to;
    int dead, refs, inner_done;
    size_t line_used;
    char line[LINE_BYTES];
};

/* Default ports of open-source payloads, as their READMEs give them. */
static const struct {
    uint16_t port;
    const char *name;
} PRESETS[] = {
    {9021, "ELF 加载器（elfldr / etaHEN）"},
    {9020, "ELF 加载器（漏洞自带）"},
    {2121, "FTP（ftpsrv）"},
    {1337, "FTP（etaHEN）"},
    {9090, "PKG 直接安装（etaHEN）"},
    {12800, "PKG 直接安装 V2（etaHEN）"},
    {9081, "内核日志（etaHEN）"},
    {3232, "内核日志（klogsrv）"},
    {8080, "网页（websrv）"},
    {2323, "Shell（shsrv）"},
    {2159, "GDB（gdbsrv）"},
};
#define PRESET_COUNT ((int)(sizeof PRESETS / sizeof PRESETS[0]))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rule g_rules[FORWARD_MAX];
static int g_count, g_loaded, g_changed;
static char g_store[PATH_BYTES];
static struct listener g_listeners[LISTENERS];
static int g_running, g_stop, g_links;
static uint32_t g_address;
static pthread_t g_engine;

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void forward_set_store(const char *path)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_store, sizeof g_store, "%s", path);
    g_loaded = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ---- rules ----------------------------------------------------------------- */

static struct rule *find_rule(uint16_t port)
{
    int i;

    for (i = 0; port && i < g_count; i++)
        if (g_rules[i].port == port) return &g_rules[i];
    return NULL;
}

/* Printable, and nothing a JSON string would have to escape. */
static int good_name(const char *name)
{
    const unsigned char *p;

    for (p = (const unsigned char *)name; *p; p++)
        if (*p < 0x20 || *p == 0x7f || *p == '"' || *p == '\\') return 0;
    return 1;
}

/* NULL, or why the rule cannot be the next of these. */
static const char *refused(const struct rule *r, const struct rule *before, int count)
{
    int i;

    if (!r->port || !r->to) return "bad_request";
    if (r->port == PAGE_PORT || r->to == TILE_PORT) return "reserved_port";
    if (!good_name(r->name)) return "bad_request";
    for (i = 0; i < count; i++)
        if (before[i].port == r->port) return "duplicate_port";
    return NULL;
}

static void use_presets(void)
{
    int i;

    memset(g_rules, 0, sizeof g_rules);
    for (i = 0; i < PRESET_COUNT && i < FORWARD_MAX; i++) {
        g_rules[i].port = g_rules[i].to = PRESETS[i].port;
        g_rules[i].on = 1;
        snprintf(g_rules[i].name, sizeof g_rules[i].name, "%s", PRESETS[i].name);
    }
    g_count = i;
}

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int get_hex(const char *hex, char *out, size_t capacity)
{
    size_t n = strlen(hex), i;

    if (n % 2 || n / 2 >= capacity) return -1;
    for (i = 0; i < n / 2; i++) {
        int high = hex_value(hex[2 * i]), low = hex_value(hex[2 * i + 1]);

        if (high < 0 || low < 0 || (!high && !low)) return -1;
        out[i] = (char)(high << 4 | low);
    }
    out[i] = 0;
    return 0;
}

/* A file that is not there gives the presets; one that is there, even empty, is the
 * owner's choice. Lines: 1 port to on name-in-hex (or -). */
static void load(void)
{
    FILE *f;
    char line[256];

    if (g_loaded) return;
    g_loaded = 1;
    if (!g_store[0] || !(f = fopen(g_store, "r"))) {
        use_presets();
        return;
    }
    memset(g_rules, 0, sizeof g_rules);
    g_count = 0;
    while (g_count < FORWARD_MAX && fgets(line, sizeof line, f)) {
        struct rule r;
        unsigned version, port, to, on;
        char hex[2 * NAME_BYTES + 2];

        memset(&r, 0, sizeof r);
        if (sscanf(line, "%u %u %u %u %97s", &version, &port, &to, &on, hex) != 5 || version != 1 || port > 65535 || to > 65535)
            continue;
        if (strcmp(hex, "-") && get_hex(hex, r.name, sizeof r.name)) continue;
        r.port = (uint16_t)port;
        r.to = (uint16_t)to;
        r.on = on != 0;
        if (refused(&r, g_rules, g_count)) continue;
        g_rules[g_count++] = r;
    }
    fclose(f);
}

static int store(const struct rule *rules, int count)
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
    for (i = 0; i < count; i++) {
        const unsigned char *p = (const unsigned char *)rules[i].name;

        fprintf(f, "1 %u %u %d ", rules[i].port, rules[i].to, rules[i].on);
        if (!*p) fputc('-', f);
        for (; *p; p++) fprintf(f, "%02x", *p);
        fputc('\n', f);
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) result = -1;
    if (fclose(f) != 0) result = -1;
    if (!result && rename(temporary, g_store) != 0) result = -1;
    if (result) unlink(temporary);
    return result;
}

static void account(uint16_t owner, uint64_t bytes, int ended)
{
    struct rule *r;

    pthread_mutex_lock(&g_lock);
    r = find_rule(owner);
    if (r) {
        r->bytes += bytes;
        if (ended && r->links) r->links--;
    }
    if (ended) g_links--;
    pthread_mutex_unlock(&g_lock);
}

/* ---- FTP passive replies ------------------------------------------------------ */

static int number(const char *s, size_t length, size_t *at, unsigned *value)
{
    size_t i = *at;
    unsigned v = 0;
    int digits = 0;

    while (i < length && s[i] >= '0' && s[i] <= '9' && digits < 6) {
        v = v * 10 + (unsigned)(s[i] - '0');
        i++;
        digits++;
    }
    if (!digits || (i < length && s[i] >= '0' && s[i] <= '9')) return -1;
    *at = i;
    *value = v;
    return 0;
}

int forward_find_passive(const char *line, size_t length, struct forward_passive *out)
{
    size_t i;

    memset(out, 0, sizeof *out);
    if (length < 5 || line[3] != ' ') return -1;
    if (!memcmp(line, "227", 3)) {
        for (i = 4; i < length; i++) {
            unsigned v[6];
            unsigned char bytes[4];
            size_t at = i;
            int k;

            if (line[i] < '0' || line[i] > '9' || (line[i - 1] >= '0' && line[i - 1] <= '9')) continue;
            for (k = 0; k < 6; k++) {
                if (number(line, length, &at, &v[k]) || v[k] > 255) break;
                if (k < 5) {
                    if (at >= length || line[at] != ',') break;
                    at++;
                }
            }
            if (k < 6) continue;
            for (k = 0; k < 4; k++) bytes[k] = (unsigned char)v[k];
            memcpy(&out->address, bytes, 4);
            out->port = (uint16_t)(v[4] << 8 | v[5]);
            out->start = i;
            out->end = at;
            return out->port ? 0 : -1;
        }
        return -1;
    }
    if (!memcmp(line, "229", 3)) {
        const char *open = memchr(line + 4, '(', length - 4);
        unsigned port;
        size_t at;
        char d;

        if (!open) return -1;
        at = (size_t)(open - line) + 1;
        if (at + 5 > length) return -1;
        d = line[at];
        if (d < 33 || d > 126 || (d >= '0' && d <= '9') || line[at + 1] != d || line[at + 2] != d) return -1;
        at += 3;
        i = at;
        if (number(line, length, &at, &port) || !port || port > 65535 || at >= length || line[at] != d) return -1;
        out->port = (uint16_t)port;
        out->start = i;
        out->end = at;
        out->extended = 1;
        return 0;
    }
    return -1;
}

/* A lwIP port, open for one connection from peer, joined to target:to. 0 if none. */
static uint16_t open_data(uint32_t target, uint16_t to, uint32_t peer, uint16_t owner)
{
    int fd = fwd_lwip_listen(0, 1), port, i;

    if (fd < 0) return 0;
    port = fwd_lwip_port(fd);
    pthread_mutex_lock(&g_lock);
    for (i = 0; port > 0 && g_running && !__atomic_load_n(&g_stop, __ATOMIC_RELAXED) && i < LISTENERS; i++) {
        struct listener *l = &g_listeners[i];

        if (l->fd >= 0) continue;
        memset(l, 0, sizeof *l);
        l->fd = fd;
        l->port = (uint16_t)port;
        l->data = 1;
        l->to = to;
        l->owner = owner;
        l->target = target;
        l->peer = peer;
        l->until = now_ms() + DATA_WAIT_MS;
        pthread_mutex_unlock(&g_lock);
        return (uint16_t)port;
    }
    pthread_mutex_unlock(&g_lock);
    fwd_lwip_close(fd, 0);
    return 0;
}

/* The line held in l, rewritten into out; 0 to send it as it is. */
static size_t rewrite(struct link *l, char *out, size_t capacity)
{
    const unsigned char *a = (const unsigned char *)&g_address;
    struct forward_passive p;
    uint32_t target;
    uint16_t port;
    int n;

    if (forward_find_passive(l->line, l->line_used, &p) != 0) return 0;
    target = p.extended || !p.address ? htonl(INADDR_LOOPBACK) : p.address;
    port = open_data(target, p.port, l->peer, l->owner);
    if (!port) return 0;
    if (p.extended)
        n = snprintf(out, capacity, "%.*s%u%.*s", (int)p.start, l->line, port, (int)(l->line_used - p.end), l->line + p.end);
    else
        n = snprintf(out, capacity, "%.*s%u,%u,%u,%u,%u,%u%.*s", (int)p.start, l->line, a[0], a[1], a[2], a[3], port >> 8, port & 255,
            (int)(l->line_used - p.end), l->line + p.end);
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}

/* ---- one connection ------------------------------------------------------------ */

static int gone(struct link *l)
{
    return __atomic_load_n(&l->dead, __ATOMIC_RELAXED) || __atomic_load_n(&g_stop, __ATOMIC_RELAXED);
}

/* Both directions stop: the kernel side wakes at once, lwIP's within a tick. */
static void cut(struct link *l)
{
    __atomic_store_n(&l->dead, 1, __ATOMIC_RELAXED);
    shutdown(l->inner, SHUT_RDWR);
}

static void release(struct link *l)
{
    if (__atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL)) return;
    close(l->inner);
    fwd_lwip_close(l->outer, l->dead);
    account(l->owner, 0, 1);
    free(l);
}

static int outer_send(struct link *l, const char *p, size_t n)
{
    while (n) {
        long w = fwd_lwip_write(l->outer, p, n);

        if (w > 0) {
            p += w;
            n -= (size_t)w;
        } else if (w != FWD_LWIP_TIMEOUT || gone(l)) {
            return -1;
        }
    }
    return 0;
}

static int inner_send(struct link *l, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(l->inner, p, n);

        if (w > 0) {
            p += w;
            n -= (size_t)w;
        } else if (w == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) || gone(l)) {
            return -1;
        }
    }
    return 0;
}

static int send_line(struct link *l)
{
    char out[LINE_BYTES + 64];
    size_t length = rewrite(l, out, sizeof out);
    int result = length ? outer_send(l, out, length) : outer_send(l, l->line, l->line_used);

    l->line_used = 0;
    return result;
}

/* From the service to the client. A connection whose service greets with 220 is
 * taken for FTP and goes on a line at a time. */
static int deliver(struct link *l, const char *data, size_t n)
{
    if (l->ftp < 0) l->ftp = !l->data && n >= 3 && !memcmp(data, "220", 3);
    if (!l->ftp) return outer_send(l, data, n);
    while (n) {
        size_t room = LINE_BYTES - l->line_used, take = n < room ? n : room;
        const char *end = memchr(data, '\n', take);

        if (end) take = (size_t)(end - data) + 1;
        memcpy(l->line + l->line_used, data, take);
        l->line_used += take;
        data += take;
        n -= take;
        if ((end || l->line_used == LINE_BYTES) && send_line(l) != 0) return -1;
    }
    return 0;
}

static void *outward_main(void *opaque)
{
    struct link *l = opaque;
    char *buffer = malloc(BUFFER_BYTES);
    int idle = 0;

    while (buffer) {
        long n = fwd_lwip_read(l->outer, buffer, BUFFER_BYTES);

        if (n > 0) {
            idle = 0;
            if (inner_send(l, buffer, (size_t)n) != 0) break;
            account(l->owner, (uint64_t)n, 0);
            continue;
        }
        if (n == FWD_LWIP_TIMEOUT) {
            if (gone(l)) break;
            if (__atomic_load_n(&l->inner_done, __ATOMIC_RELAXED) && ++idle > HALF_OPEN_TICKS) break;
            continue;
        }
        if (n == 0) {
            shutdown(l->inner, SHUT_WR);
            free(buffer);
            release(l);
            return NULL;
        }
        break;
    }
    free(buffer);
    cut(l);
    release(l);
    return NULL;
}

static int inner_connect(uint32_t target, uint16_t port)
{
    struct sockaddr_in to;
    struct timeval timeout = {TICK_MS / 1000, (TICK_MS % 1000) * 1000};
    int fd = socket(AF_INET, SOCK_STREAM, 0), on = 1;

    if (fd < 0) return -1;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = target;
    if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0) {
        close(fd);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    return fd;
}

static int spawn(void *(*main)(void *), void *argument)
{
    pthread_attr_t attr;
    pthread_t thread;
    int result;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, LINK_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    result = pthread_create(&thread, &attr, main, argument);
    pthread_attr_destroy(&attr);
    return result;
}

/* Connects to the service, then carries what it sends back to the client. */
static void *inward_main(void *opaque)
{
    struct link *l = opaque;
    char *buffer;

    l->inner = inner_connect(l->target, l->to);
    if (l->inner < 0) {
        fwd_lwip_close(l->outer, 1);    /* the client sees the refusal the service gave */
        account(l->owner, 0, 1);
        free(l);
        return NULL;
    }
    l->refs = 2;
    if (spawn(outward_main, l) != 0) {
        l->refs = 1;
        cut(l);
        release(l);
        return NULL;
    }
    buffer = malloc(BUFFER_BYTES);
    while (buffer) {
        ssize_t n = read(l->inner, buffer, BUFFER_BYTES);

        if (n > 0) {
            if (deliver(l, buffer, (size_t)n) != 0) break;
            account(l->owner, (uint64_t)n, 0);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (gone(l)) break;
            continue;
        }
        if (n == 0 && !gone(l)) {
            if (l->line_used && outer_send(l, l->line, l->line_used) != 0) break;
            fwd_lwip_shutdown(l->outer);
            __atomic_store_n(&l->inner_done, 1, __ATOMIC_RELAXED);
            free(buffer);
            release(l);
            return NULL;
        }
        break;
    }
    free(buffer);
    cut(l);
    release(l);
    return NULL;
}

/* ---- listening -------------------------------------------------------------------- */

static void close_listener(struct listener *l)
{
    fwd_lwip_close(l->fd, 0);
    l->fd = -1;
}

/* With the lock: a listener for every rule that is on, and none for any other. */
static void reconcile(void)
{
    int i, j;

    for (i = 0; i < LISTENERS; i++) {
        struct listener *l = &g_listeners[i];
        struct rule *r;

        if (l->fd < 0 || l->data) continue;
        r = find_rule(l->port);
        if (!r || !r->on) close_listener(l);
    }
    for (i = 0; i < g_count; i++) {
        struct rule *r = &g_rules[i];
        int fd;

        r->listening = 0;
        if (!r->on) {
            r->failed = 0;
            continue;
        }
        for (j = 0; j < LISTENERS; j++)
            if (g_listeners[j].fd >= 0 && !g_listeners[j].data && g_listeners[j].port == r->port) r->listening = 1;
        if (r->listening) continue;
        for (j = 0; j < LISTENERS && g_listeners[j].fd >= 0; j++) {}
        if (j == LISTENERS) break;
        fd = fwd_lwip_listen(r->port, BACKLOG);
        if (fd < 0) {
            if (!r->failed) say("forward: port %u could not listen, errno %d", r->port, errno);
            r->failed = 1;
            continue;
        }
        memset(&g_listeners[j], 0, sizeof g_listeners[j]);
        g_listeners[j].fd = fd;
        g_listeners[j].port = r->port;
        r->listening = 1;
        r->failed = 0;
    }
}

static void take(int slot)
{
    struct listener *l;
    struct link *k;
    uint32_t peer = 0;
    int fd;

    pthread_mutex_lock(&g_lock);
    l = &g_listeners[slot];
    if (l->fd < 0 || (fd = fwd_lwip_accept(l->fd, &peer, TICK_MS)) < 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    k = g_links < LINKS_MAX ? calloc(1, sizeof *k) : NULL;
    if (k && l->data) {
        if (peer != l->peer) {          /* the data port is only for whoever asked */
            free(k);
            k = NULL;
        } else {
            k->data = 1;
            k->target = l->target;
            k->to = l->to;
            k->owner = l->owner;
            close_listener(l);
        }
    } else if (k) {
        struct rule *r = find_rule(l->port);

        if (r && r->on) {
            k->target = htonl(INADDR_LOOPBACK);
            k->to = r->to;
            k->owner = r->port;
        } else {
            free(k);
            k = NULL;
        }
    }
    if (k) {
        struct rule *r = find_rule(k->owner);

        if (r) r->links++;
        g_links++;
    }
    pthread_mutex_unlock(&g_lock);
    if (!k) {
        fwd_lwip_close(fd, 1);
        return;
    }
    k->outer = fd;
    k->inner = -1;
    k->peer = peer;
    k->ftp = -1;
    if (spawn(inward_main, k) != 0) {
        fwd_lwip_close(fd, 1);
        account(k->owner, 0, 1);
        free(k);
    }
}

static void *engine_main(void *unused)
{
    int fds[LISTENERS], slots[LISTENERS], ready[LISTENERS];
    long retry = 0;

    (void)unused;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        long t = now_ms();
        int i, n = 0;

        pthread_mutex_lock(&g_lock);
        if (g_changed || t >= retry) {
            reconcile();
            g_changed = 0;
            retry = t + RETRY_MS;
        }
        for (i = 0; i < LISTENERS; i++) {
            struct listener *l = &g_listeners[i];

            if (l->fd < 0) continue;
            if (l->data && t > l->until) {
                close_listener(l);
                continue;
            }
            fds[n] = l->fd;
            slots[n++] = i;
        }
        pthread_mutex_unlock(&g_lock);
        if (fwd_lwip_wait(fds, n, WAIT_MS, ready) <= 0) continue;
        for (i = 0; i < n; i++)
            if (ready[i]) take(slots[i]);
    }
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < LISTENERS; i++)
        if (g_listeners[i].fd >= 0) close_listener(&g_listeners[i]);
    for (int i = 0; i < g_count; i++) g_rules[i].listening = g_rules[i].failed = 0;
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int forward_start(const char *address)
{
    int i, on = 0;

    if (g_running || inet_pton(AF_INET, address, &g_address) != 1) return -1;
    pthread_mutex_lock(&g_lock);
    load();
    for (i = 0; i < LISTENERS; i++) g_listeners[i].fd = -1;
    for (i = 0; i < g_count; i++) on += g_rules[i].on;
    g_changed = 1;
    g_stop = 0;
    g_running = 1;
    pthread_mutex_unlock(&g_lock);
    if (pthread_create(&g_engine, NULL, engine_main, NULL) != 0) {
        g_running = 0;
        return -1;
    }
    say("forward: %d rules, %d on", g_count, on);
    return 0;
}

void forward_stop(void)
{
    int tick, links = 0;

    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_mutex_unlock(&g_lock);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    pthread_join(g_engine, NULL);
    for (tick = 0; tick < STOP_WAIT_TICKS; tick++) {
        pthread_mutex_lock(&g_lock);
        links = g_links;
        pthread_mutex_unlock(&g_lock);
        if (!links) return;
        usleep(STOP_TICK_US);
    }
    say("forward: %d connections were still open when the run ended", links);
}

/* ---- the page's routes ---------------------------------------------------------------- */

static void append(char *out, size_t capacity, size_t *used, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void append(char *out, size_t capacity, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*used >= capacity) return;
    va_start(ap, fmt);
    n = vsnprintf(out + *used, capacity - *used, fmt, ap);
    va_end(ap);
    *used = n < 0 ? capacity : *used + (size_t)n;
}

static int rules(struct http_stream *stream)
{
    char *json = malloc(JSON_BYTES);
    size_t used = 0;
    int i, result;

    if (!json) return http_respond(stream, 503, "application/json", "{\"error\":\"no_memory\"}");
    pthread_mutex_lock(&g_lock);
    load();
    append(json, JSON_BYTES, &used, "{\"running\":%s,\"max\":%d,\"reserved\":[%d,%d],\"rules\":[", g_running ? "true" : "false", FORWARD_MAX,
        PAGE_PORT, TILE_PORT);
    for (i = 0; i < g_count; i++) {
        const struct rule *r = &g_rules[i];
        const char *state = !r->on ? "off" : !g_running ? "idle" : r->listening ? "listening" : r->failed ? "busy" : "starting";

        append(json, JSON_BYTES, &used, "%s{\"port\":%u,\"to\":%u,\"on\":%s,\"name\":\"%s\",\"state\":\"%s\",\"links\":%u,\"bytes\":%" PRIu64 "}",
            i ? "," : "", r->port, r->to, r->on ? "true" : "false", r->name, state, r->links, r->bytes);
    }
    pthread_mutex_unlock(&g_lock);
    append(json, JSON_BYTES, &used, "],\"presets\":[");
    for (i = 0; i < PRESET_COUNT; i++) append(json, JSON_BYTES, &used, "%s{\"port\":%u,\"name\":\"%s\"}", i ? "," : "", PRESETS[i].port, PRESETS[i].name);
    append(json, JSON_BYTES, &used, "]}");
    result = used < JSON_BYTES ? http_respond(stream, 200, "application/json", json)
                               : http_respond(stream, 503, "application/json", "{\"error\":\"no_memory\"}");
    free(json);
    return result;
}

static int error(struct http_stream *stream, int status, const char *code)
{
    char json[64];

    snprintf(json, sizeof json, "{\"error\":\"%s\"}", code);
    return http_respond(stream, status, "application/json", json);
}

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

static int percent_decode(const char *in, char *out, size_t capacity)
{
    size_t used = 0;

    for (; *in; in++) {
        int c = (unsigned char)*in;

        if (c == '%') {
            int high = hex_value(in[1]), low = high < 0 ? -1 : hex_value(in[2]);

            if (low < 0) return -1;
            c = high << 4 | low;
            in += 2;
        }
        if (!c || used + 1 >= capacity) return -1;
        out[used++] = (char)c;
    }
    out[used] = 0;
    return 0;
}

/* The whole list, a rule a line: port to on name, the name percent-encoded or -. */
static int save(const struct http_request *r, struct http_stream *stream)
{
    char *body = malloc(BODY_BYTES), *line, *next;
    struct rule *list = calloc(FORWARD_MAX, sizeof *list);
    const char *why = NULL;
    int count = 0, i, j;

    if (!body || !list) {
        free(body);
        free(list);
        return error(stream, 503, "no_memory");
    }
    if (read_body(r, stream, body) != 0) why = "bad_request";
    for (line = body; !why && line && *line; line = next) {
        unsigned port, to, on;
        char name[3 * NAME_BYTES + 2];

        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!line[strspn(line, " \r")]) continue;
        if (count == FORWARD_MAX) {
            why = "too_many_ports";
            break;
        }
        memset(&list[count], 0, sizeof list[count]);
        if (sscanf(line, "%u %u %u %145s", &port, &to, &on, name) != 4 || port > 65535 || to > 65535 ||
                (strcmp(name, "-") && percent_decode(name, list[count].name, sizeof list[count].name))) {
            why = "bad_request";
            break;
        }
        list[count].port = (uint16_t)port;
        list[count].to = (uint16_t)to;
        list[count].on = on != 0;
        why = refused(&list[count], list, count);
        count++;
    }
    free(body);
    if (why) {
        free(list);
        return error(stream, 400, why);
    }
    pthread_mutex_lock(&g_lock);
    load();
    if (store(list, count) != 0) {
        pthread_mutex_unlock(&g_lock);
        free(list);
        return error(stream, 500, "not_saved");
    }
    for (i = 0; i < count; i++)
        for (j = 0; j < g_count; j++)
            if (g_rules[j].port == list[i].port) {
                list[i].listening = g_rules[j].listening && list[i].on;
                list[i].links = g_rules[j].links;
                list[i].bytes = g_rules[j].bytes;
            }
    memcpy(g_rules, list, sizeof *list * FORWARD_MAX);
    g_count = count;
    g_changed = 1;
    pthread_mutex_unlock(&g_lock);
    free(list);
    say("forward: %d rules saved", count);
    usleep(WAIT_MS * 1000 + 50000);     /* the listeners follow before the list is shown */
    return rules(stream);
}

int forward_route(const struct http_request *r, struct http_stream *stream)
{
    if (strncmp(r->path, "/fwd/", 5)) return HTTP_NOT_MINE;
    if (!strcmp(r->method, "GET") && !strcmp(r->path + 5, "rules")) return rules(stream);
    if (!strcmp(r->method, "POST") && !strcmp(r->path + 5, "save")) return save(r, stream);
    return HTTP_NOT_MINE;
}
