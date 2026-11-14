/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The icon opens the console's own browser, and that browser's network is the
 * console's, not this ELF's lwIP. So the same routes also listen on the kernel's
 * sockets. With an adapter, bound to the loopback address and to nothing else: this
 * listener cannot be reached from any network. Without one it is the page itself, on
 * every address the console has. */
#include "local_http.h"
#include "log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
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
static int g_listener = -1, g_stop, g_workers, g_running, g_broken;

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

static void *accept_main(void *opaque)
{
    int listener = (int)(intptr_t)opaque;
    struct pollfd wait = {listener, POLLIN, 0};

    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        int ready = poll(&wait, 1, LOCAL_POLL_MS);

        if (ready < 0 && errno != EINTR) {
            __atomic_store_n(&g_broken, 1, __ATOMIC_RELAXED);
            break;
        }
        if (ready > 0 && (wait.revents & (POLLERR | POLLNVAL))) {
            __atomic_store_n(&g_broken, 1, __ATOMIC_RELAXED);
            break;
        }
        if (ready > 0) {
            int fd = accept(listener, NULL, NULL);

            if (fd >= 0) start_worker(fd);
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED && errno != EMFILE &&
                    errno != ENFILE && errno != ENOBUFS) {
                __atomic_store_n(&g_broken, 1, __ATOMIC_RELAXED);
                break;
            }
        }
    }
    close(listener);
    __atomic_store_n(&g_running, 0, __ATOMIC_RELAXED);
    return NULL;
}

int local_http_start(const struct http_backend *backend, uint16_t port, int every_address)
{
    struct sockaddr_in address;
    pthread_attr_t attr;
    pthread_t thread;
    int on = 1, result;

    if (__atomic_load_n(&g_running, __ATOMIC_RELAXED)) {
        errno = EBUSY;
        return -1;
    }
    g_backend = backend;
    g_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listener < 0) return -1;
    setsockopt(g_listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
#ifdef LOCAL_HTTP_TESTING
    (void)every_address;                /* the host's own network is no business of a test */
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#else
    address.sin_addr.s_addr = htonl(every_address ? INADDR_ANY : INADDR_LOOPBACK);
#endif
    if (bind(g_listener, (struct sockaddr *)&address, sizeof address) != 0 || listen(g_listener, LOCAL_BACKLOG) != 0) {
        close(g_listener);
        g_listener = -1;
        return -1;
    }
    g_stop = 0;
    g_broken = 0;
    __atomic_store_n(&g_running, 1, __ATOMIC_RELAXED);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    result = pthread_create(&thread, &attr, accept_main, (void *)(intptr_t)g_listener);
    pthread_attr_destroy(&attr);
    if (result) {
        close(g_listener);
        g_running = 0;
    }
    g_listener = -1;                    /* the accept thread owns it now */
    return result ? -1 : 0;
}

int local_http_close(void)
{
    int tick;

    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    for (tick = 0; tick < LOCAL_STOP_WAIT_TICKS; tick++) {
        if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED)) return 0;
        usleep(LOCAL_TICK_US);
    }
    say("local page: the listener did not stop");
    return -1;
}

int local_http_connections(void)
{
    int workers;

    pthread_mutex_lock(&g_lock);
    workers = g_workers;
    pthread_mutex_unlock(&g_lock);
    return workers;
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

int local_http_broken(void)
{
    return __atomic_load_n(&g_broken, __ATOMIC_RELAXED);
}
