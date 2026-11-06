/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The icon opens the console's own browser, and that browser's network is the
 * console's, not this ELF's lwIP. So the same routes also listen on the kernel's
 * sockets, bound to the loopback address and to nothing else: this listener cannot be
 * reached from any network. */
#include "local_http.h"
#include "log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOCAL_STACK (1024 * 1024)
#define LOCAL_WORKERS 8
#define LOCAL_BACKLOG 16
#define LOCAL_POLL_MS 200
#define LOCAL_STOP_WAIT_TICKS 300
#define LOCAL_TICK_US 10000
#define LOCAL_IO_TIMEOUT_S 30

struct connection {
    const struct http_backend *backend;
    int fd;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const struct http_backend *g_backend;
static int g_listener = -1, g_stop, g_workers, g_running;

static ssize_t stream_read(void *context, void *data, size_t length)
{
    return read(*(int *)context, data, length);
}

static ssize_t stream_write(void *context, const void *data, size_t length)
{
    return write(*(int *)context, data, length);
}

static void add_worker(int change)
{
    pthread_mutex_lock(&g_lock);
    g_workers += change;
    pthread_mutex_unlock(&g_lock);
}

static void *worker_main(void *opaque)
{
    struct connection connection = *(struct connection *)opaque;
    struct http_stream stream = {&connection.fd, stream_read, stream_write};

    free(opaque);
    http_serve(connection.backend, &stream);
    close(connection.fd);
    add_worker(-1);
    return NULL;
}

static void start_worker(int fd)
{
    struct connection *connection = malloc(sizeof *connection);
    struct timeval timeout = {LOCAL_IO_TIMEOUT_S, 0};
    pthread_attr_t attr;
    pthread_t thread;
    int result, on = 1;

    if (!connection || g_workers >= LOCAL_WORKERS) {
        free(connection);
        close(fd);
        return;
    }
    /* A browser that goes away must not raise SIGPIPE, and one that stalls must not hold a worker. */
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    connection->backend = g_backend;
    connection->fd = fd;
    add_worker(1);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, LOCAL_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    result = pthread_create(&thread, &attr, worker_main, connection);
    pthread_attr_destroy(&attr);
    if (result) {
        free(connection);
        close(fd);
        add_worker(-1);
    }
}

static void *accept_main(void *unused)
{
    struct pollfd wait = {g_listener, POLLIN, 0};

    (void)unused;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        if (poll(&wait, 1, LOCAL_POLL_MS) > 0) {
            int fd = accept(g_listener, NULL, NULL);

            if (fd >= 0) start_worker(fd);
        }
    }
    close(g_listener);
    g_listener = -1;
    __atomic_store_n(&g_running, 0, __ATOMIC_RELAXED);
    return NULL;
}

int local_http_start(const struct http_backend *backend, uint16_t port)
{
    struct sockaddr_in address;
    pthread_attr_t attr;
    pthread_t thread;
    int on = 1, result;

    g_backend = backend;
    g_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listener < 0) return -1;
    setsockopt(g_listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listener, (struct sockaddr *)&address, sizeof address) != 0 || listen(g_listener, LOCAL_BACKLOG) != 0) {
        close(g_listener);
        g_listener = -1;
        return -1;
    }
    g_stop = 0;
    __atomic_store_n(&g_running, 1, __ATOMIC_RELAXED);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    result = pthread_create(&thread, &attr, accept_main, NULL);
    pthread_attr_destroy(&attr);
    if (result) {
        close(g_listener);
        g_listener = -1;
        g_running = 0;
        return -1;
    }
    return 0;
}

void local_http_stop(void)
{
    int tick;

    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    for (tick = 0; tick < LOCAL_STOP_WAIT_TICKS; tick++) {
        int workers;

        pthread_mutex_lock(&g_lock);
        workers = g_workers;
        pthread_mutex_unlock(&g_lock);
        if (!workers && !__atomic_load_n(&g_running, __ATOMIC_RELAXED)) return;
        usleep(LOCAL_TICK_US);
    }
    say("local page: connections were still open when the run ended");
}
