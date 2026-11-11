/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/forward.c on the host: FTP reply parsing, the rules and their routes, and the
 * relay itself, with lwIP's side on loopback sockets (tests/forward_fake.c) in front of
 * the services in tests/forward_services.c. test_app.c runs it again through lwIP. */
#include "forward.h"
#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "test_forward: %s, line %d\n", #condition, __LINE__); exit(1); } } while (0)
#define OFFSET 30000                    /* forward_fake.c's */
#define ECHO_BYTES (1 << 20)

void services_start(uint16_t echo, uint16_t ftp);

void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- passive replies --------------------------------------------------------- */

static int found(const char *line, struct forward_passive *p)
{
    return forward_find_passive(line, strlen(line), p) == 0;
}

static void passive(void)
{
    static const char *const refused[] = {
        "227-Entering Passive Mode (1,2,3,4,5,6)", "200 Entering Passive Mode (1,2,3,4,5,6)", "227 Entering Passive Mode (1,2,3,4,5)",
        "227 Entering Passive Mode (300,1,1,1,1,1)", "227 Entering Passive Mode (1,2,3,4,0,0)", "229 Entering Extended Passive Mode (|||0|)",
        "229 Entering Extended Passive Mode (|||99999|)", "229 Entering Extended Passive Mode (||6446|)", "229 Entering Extended Passive Mode",
        "227", ""};
    const char *a = "227 Entering Passive Mode (127,0,0,1,78,52).\r\n";
    const char *b = "227 =192,168,1,9,4,1";
    const char *c = "229 Entering Extended Passive Mode (|||6446|)\r\n";
    struct forward_passive p;
    size_t i;

    CHECK(found(a, &p) && p.port == 78 * 256 + 52 && p.address == htonl(0x7f000001) && !p.extended);
    CHECK(!strncmp(a + p.start, "127,0,0,1,78,52", p.end - p.start) && a[p.end] == ')');
    CHECK(found(b, &p) && p.port == 1025 && p.address == htonl(0xc0a80109) && p.end == strlen(b));
    CHECK(found(c, &p) && p.extended && p.port == 6446 && p.end - p.start == 4 && !strncmp(c + p.start, "6446", 4));
    for (i = 0; i < sizeof refused / sizeof refused[0]; i++) CHECK(!found(refused[i], &p));
}

/* ---- the routes ------------------------------------------------------------------ */

struct memory {
    const char *in;
    size_t in_length, at, used;
    char out[65536];
};

static ssize_t memory_read(void *context, void *data, size_t length)
{
    struct memory *m = context;
    size_t n = m->in_length - m->at < length ? m->in_length - m->at : length;

    memcpy(data, m->in + m->at, n);
    m->at += n;
    return (ssize_t)n;
}

static ssize_t memory_write(void *context, const void *data, size_t length)
{
    struct memory *m = context;
    size_t n = sizeof m->out - 1 - m->used < length ? sizeof m->out - 1 - m->used : length;

    memcpy(m->out + m->used, data, n);
    m->used += n;
    m->out[m->used] = 0;
    return (ssize_t)length;
}

/* The status, and the body in reply. The first byte of the body arrives with the
 * head and the rest through the stream, as from a socket. */
static int call(const char *method, const char *path, const char *body, char *reply, size_t capacity)
{
    static struct memory m;
    struct http_stream stream = {&m, memory_read, memory_write};
    size_t length = body ? strlen(body) : 0;
    struct http_request request = {method, path, "", "127.0.0.1", "", length, body != NULL, (const uint8_t *)body, length ? 1 : 0};
    const char *start;
    int status = 0;

    memset(&m, 0, sizeof m);
    m.in = body ? body + (length ? 1 : 0) : "";
    m.in_length = length ? length - 1 : 0;
    CHECK(forward_route(&request, &stream) != HTTP_NOT_MINE);
    CHECK(sscanf(m.out, "HTTP/1.1 %d", &status) == 1);
    start = strstr(m.out, "\r\n\r\n");
    CHECK(start);
    snprintf(reply, capacity, "%s", start + 4);
    return status;
}

static void routes(const char *store)
{
    static char reply[65536];
    char text[512];
    FILE *f;

    unlink(store);
    forward_set_store(store);
    CHECK(call("GET", "/fwd/rules", NULL, reply, sizeof reply) == 200);
    CHECK(strstr(reply, "\"running\":false") && strstr(reply, "\"port\":9021,\"to\":9021,\"on\":true") && strstr(reply, "\"presets\":[{\"port\":9021"));
    CHECK(strstr(reply, "\"reserved\":[80,9190]"));
    CHECK(call("POST", "/fwd/save", "80 80 1 -\n", reply, sizeof reply) == 400 && strstr(reply, "reserved_port"));
    CHECK(call("POST", "/fwd/save", "9000 9190 1 -\n", reply, sizeof reply) == 400 && strstr(reply, "reserved_port"));
    CHECK(call("POST", "/fwd/save", "21000 21001 1 a\n21000 21003 1 b\n", reply, sizeof reply) == 400 && strstr(reply, "duplicate_port"));
    CHECK(call("POST", "/fwd/save", "21000 21001 1 a%22b\n", reply, sizeof reply) == 400 && strstr(reply, "bad_request"));
    CHECK(call("POST", "/fwd/save", "21000 0 1 -\n", reply, sizeof reply) == 400);
    CHECK(call("POST", "/fwd/save", "70000 21001 1 -\n", reply, sizeof reply) == 400);
    CHECK(call("POST", "/fwd/save", "21000 21001 1 %zz\n", reply, sizeof reply) == 400);
    CHECK(call("POST", "/fwd/save", "", reply, sizeof reply) == 200 && strstr(reply, "\"rules\":[]"));
    CHECK(call("POST", "/fwd/save",
        "21000 21001 1 %E5%9B%9E%E5%A3%B0\r\n21002 21003 1 ftp\n\n21004 21005 1 -\n21006 21007 0 off\n", reply, sizeof reply) == 200);
    CHECK(strstr(reply, "\"port\":21000,\"to\":21001,\"on\":true,\"name\":\"回声\",\"state\":\"idle\""));
    CHECK(strstr(reply, "\"port\":21006,\"to\":21007,\"on\":false,\"name\":\"off\",\"state\":\"off\"") && !strstr(reply, "\"port\":9021,\"to\""));
    f = fopen(store, "r");
    CHECK(f && fgets(text, sizeof text, f) && !strcmp(text, "1 21000 21001 1 e59b9ee5a3b0\n"));
    CHECK(fgets(text, sizeof text, f) && fgets(text, sizeof text, f) && !strcmp(text, "1 21004 21005 1 -\n"));
    fclose(f);
    forward_set_store(store);           /* read again from the file */
    CHECK(call("GET", "/fwd/rules", NULL, reply, sizeof reply) == 200 && strstr(reply, "\"name\":\"回声\"") && strstr(reply, "\"port\":21006"));
}

/* ---- the relay ----------------------------------------------------------------- */

static int connect_to(uint16_t port)
{
    struct sockaddr_in to;
    struct timeval timeout = {5, 0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    CHECK(fd >= 0);
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0) {
        close(fd);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    return fd;
}

static void read_line(int fd, char *line, size_t capacity)
{
    size_t used = 0;

    for (;;) {
        char c;

        CHECK(read(fd, &c, 1) == 1);
        if (c == '\n') break;
        if (c != '\r' && used + 1 < capacity) line[used++] = c;
    }
    line[used] = 0;
}

static size_t read_all(int fd, char *out, size_t capacity)
{
    size_t used = 0;
    ssize_t n;

    while (used < capacity && (n = read(fd, out + used, capacity - used)) > 0) used += (size_t)n;
    return used;
}

static void *echo_writer(void *opaque)
{
    int fd = (int)(intptr_t)opaque;
    char *data = malloc(ECHO_BYTES);
    size_t at = 0, i;

    for (i = 0; i < ECHO_BYTES; i++) data[i] = (char)(i * 7 + i / 4096);
    while (at < ECHO_BYTES) {
        ssize_t n = write(fd, data + at, ECHO_BYTES - at);

        CHECK(n > 0);
        at += (size_t)n;
    }
    shutdown(fd, SHUT_WR);
    free(data);
    return NULL;
}

static void echo(void)
{
    char *back = malloc(ECHO_BYTES + 1);
    int fd = connect_to(21000 + OFFSET);
    pthread_t writer;
    size_t i;

    CHECK(fd >= 0 && back);
    CHECK(pthread_create(&writer, NULL, echo_writer, (void *)(intptr_t)fd) == 0);
    CHECK(read_all(fd, back, ECHO_BYTES + 1) == ECHO_BYTES);
    pthread_join(writer, NULL);
    for (i = 0; i < ECHO_BYTES; i++) CHECK(back[i] == (char)(i * 7 + i / 4096));
    close(fd);
    free(back);
}

/* One PASV or EPSV and its data connection. The address must be the adapter's. */
static void passive_transfer(int control, int extended)
{
    char line[256], data[64];
    unsigned a, b, c, d, high, low, port;
    int fd;

    snprintf(line, sizeof line, "%s\r\n", extended ? "EPSV" : "PASV");
    CHECK(write(control, line, strlen(line)) == (ssize_t)strlen(line));
    read_line(control, line, sizeof line);
    if (extended) {
        CHECK(sscanf(line, "229 Entering Extended Passive Mode (|||%u|)", &port) == 1);
    } else {
        CHECK(sscanf(line, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).", &a, &b, &c, &d, &high, &low) == 6);
        CHECK(a == 192 && b == 0 && c == 2 && d == 50);
        port = high << 8 | low;
    }
    fd = connect_to((uint16_t)port);
    CHECK(fd >= 0);
    memset(data, 0, sizeof data);
    CHECK(read_all(fd, data, sizeof data - 1) == 9 && !strcmp(data, extended ? "DATA-EPSV" : "DATA-PASV"));
    close(fd);
    read_line(control, line, sizeof line);
    CHECK(!strcmp(line, "226 done"));
    CHECK(connect_to((uint16_t)port) < 0);  /* one connection only */
}

static void ftp(void)
{
    char line[256];
    int control = connect_to(21002 + OFFSET);

    CHECK(control >= 0);
    read_line(control, line, sizeof line);
    CHECK(!strcmp(line, "220 fake ftp"));
    passive_transfer(control, 0);
    passive_transfer(control, 1);
    passive_transfer(control, 0);
    CHECK(write(control, "QUIT\r\n", 6) == 6);
    read_line(control, line, sizeof line);
    CHECK(!strcmp(line, "221 bye"));
    CHECK(read(control, line, 1) == 0);
    close(control);
}

static void relay(void)
{
    static char reply[65536];
    char byte;
    int fd, tries;

    services_start(21001, 21003);
    CHECK(forward_start("192.0.2.50") == 0);
    for (tries = 0; tries < 100; tries++) {
        CHECK(call("GET", "/fwd/rules", NULL, reply, sizeof reply) == 200);
        if (strstr(reply, "\"port\":21004,\"to\":21005,\"on\":true,\"name\":\"\",\"state\":\"listening\"")) break;
        usleep(20000);
    }
    CHECK(tries < 100 && strstr(reply, "\"running\":true"));
    echo();
    ftp();
    fd = connect_to(21004 + OFFSET);    /* nothing behind it: turned away */
    CHECK(fd >= 0 && read(fd, &byte, 1) <= 0);
    close(fd);
    CHECK(connect_to(21006 + OFFSET) < 0);
    /* Saved while running: 21006 starts, 21004 stops. */
    CHECK(call("POST", "/fwd/save", "21000 21001 1 -\n21002 21003 1 -\n21006 21001 1 -\n", reply, sizeof reply) == 200);
    CHECK(strstr(reply, "\"port\":21006,\"to\":21001,\"on\":true,\"name\":\"\",\"state\":\"listening\""));
    CHECK(connect_to(21004 + OFFSET) < 0);
    fd = connect_to(21006 + OFFSET);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1 && read(fd, &byte, 1) == 1 && byte == 'x');
    close(fd);
    for (tries = 0; tries < 100; tries++) {
        CHECK(call("GET", "/fwd/rules", NULL, reply, sizeof reply) == 200);
        if (strstr(reply, "\"port\":21006,\"to\":21001,\"on\":true,\"name\":\"\",\"state\":\"listening\",\"links\":0")) break;
        usleep(20000);
    }
    CHECK(tries < 100);
    CHECK(strstr(reply, "\"port\":21000,\"to\":21001,\"on\":true,\"name\":\"\",\"state\":\"listening\",\"links\":0,\"bytes\":2097152"));
    forward_stop();
    CHECK(connect_to(21000 + OFFSET) < 0);
    CHECK(call("GET", "/fwd/rules", NULL, reply, sizeof reply) == 200 && strstr(reply, "\"running\":false"));
}

int main(void)
{
    char store[64];

    signal(SIGPIPE, SIG_IGN);
    alarm(60);
    snprintf(store, sizeof store, "/tmp/tom-forward-%d.txt", (int)getpid());
    passive();
    routes(store);
    relay();
    unlink(store);
    puts("forward test passed");
    return 0;
}
