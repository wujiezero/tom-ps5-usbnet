/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Serves the page on 127.0.0.1, so that a real browser can walk all of it: only the page,
 * http_serve and the real file routes on a folder. No USB and no lwIP. */
#include "files.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_stop;

static int info(void *c, char *out, size_t n)
{
    (void)c;
    snprintf(out, n, "{\"name\":\"tom-ps5-usbnet\",\"version\":\"test\",\"ip\":\"192.0.2.50\",\"link_mbps\":10000}");
    return 0;
}

static void stop(void *c) { (void)c; g_stop = 1; }

static int route(void *c, const struct http_request *r, struct http_stream *stream)
{
    (void)c;
    if (!strcmp(r->path, "/tile/status")) return http_respond(stream, 200, "application/json", "{\"state\":\"installed\",\"error\":\"\"}");
    if (!strcmp(r->path, "/tile/remove")) return http_respond(stream, 200, "application/json", "{\"state\":\"removed\",\"error\":\"\"}");
    if (!strcmp(r->path, "/tile/install")) return http_respond(stream, 200, "application/json", "{}");
    return files_route(r, stream);
}

static const struct http_backend g_backend = {.route = route, .info = info, .stop = stop};

static ssize_t stream_read(void *c, void *d, size_t n) { return read(*(int *)c, d, n); }
static ssize_t stream_write(void *c, const void *d, size_t n) { return write(*(int *)c, d, n); }

static void *worker(void *opaque)
{
    int fd = (int)(intptr_t)opaque;
    struct http_stream stream = {&fd, stream_read, stream_write};

    http_serve(&g_backend, &stream);
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    struct sockaddr_in address;
    int on = 1, listener = socket(AF_INET, SOCK_STREAM, 0);

    if (argc < 3) {
        fprintf(stderr, "usage: page-server port folder\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)atoi(argv[1]));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    files_test_root = argv[2];
    if (bind(listener, (struct sockaddr *)&address, sizeof address) != 0 || listen(listener, 16) != 0) {
        perror("listen");
        return 1;
    }
    while (!g_stop) {
        pthread_t thread;
        int fd = accept(listener, NULL, NULL);

        if (fd < 0) continue;
        pthread_create(&thread, NULL, worker, (void *)(intptr_t)fd);
        pthread_detach(thread);
    }
    sleep(1);
    return 0;
}
